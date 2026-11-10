package server

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"strings"

	"aipc/platform/app-manager/containerd"
	"aipc/platform/app-manager/manifest"
	"aipc/platform/app-manager/registry"
	"aipc/platform/app-manager/stopbudget"
	"aipc/platform/common/logger"
	"aipc/platform/common/utils"
)

// imageRecoveryContext keeps recovery alive when the HTTP/gRPC caller gives
// up, while retaining namespace and tracing values carried by the request.
// The operation remains bounded so a wedged containerd cannot hold the
// per-app mutation lock forever.
func imageRecoveryContext(parent context.Context) (context.Context, context.CancelFunc) {
	return context.WithTimeout(context.WithoutCancel(parent), stopbudget.ImageRecoveryBudget)
}

// imageReferencesForApp applies the normalized image recorded at install time
// for legacy single-container apps, then returns every image the app needs.
func imageReferencesForApp(appInfo *registry.AppInfo, appManifest *manifest.AppManifest) []string {
	if !appManifest.IsMultiContainer() && appInfo != nil && appInfo.Image != "" {
		appManifest.Spec.Image = appInfo.Image
	}
	return appManifest.ImageReferences()
}

func installedImageReferences(appInfo *registry.AppInfo) ([]string, error) {
	if appInfo == nil || appInfo.ManifestPath == "" {
		return nil, fmt.Errorf("installed app manifest is unavailable")
	}
	appManifest, err := manifest.LoadManifest(appInfo.ManifestPath)
	if err != nil {
		return nil, err
	}
	return imageReferencesForApp(appInfo, appManifest), nil
}

func sameImageReferences(a, b []string) bool {
	if len(a) != len(b) {
		return false
	}
	for i := range a {
		if a[i] != b[i] {
			return false
		}
	}
	return true
}

// shouldDiscardSavedImageArchive reports whether an update that commits
// without a new local archive must invalidate the previously saved one.
// Identical reference strings are not proof: a mutable tag (foo:latest) can
// be re-pushed with different content while keeping its name, so the archive
// is retained only when its recorded digests still match the images behind
// every reference (bindingProven).
func shouldDiscardSavedImageArchive(isUpdate, hasNewArchive, bindingProven bool, oldRefs, newRefs []string) bool {
	return isUpdate && !hasNewArchive &&
		(!bindingProven || !sameImageReferences(oldRefs, newRefs))
}

// savedImageDigests is the on-disk sidecar schema recorded next to a
// recovery archive at publication time.
type savedImageDigests struct {
	Digests map[string]string `json:"digests"`
}

func writeSavedImageDigestsAt(path string, digests map[string]string) error {
	data, err := json.MarshalIndent(savedImageDigests{Digests: digests}, "", "  ")
	if err != nil {
		return err
	}
	tmp := path + ".tmp"
	file, err := os.OpenFile(tmp, os.O_CREATE|os.O_TRUNC|os.O_WRONLY, 0600)
	if err != nil {
		return err
	}
	if _, err := file.Write(append(data, '\n')); err != nil {
		file.Close()
		os.Remove(tmp)
		return err
	}
	if err := file.Sync(); err != nil {
		file.Close()
		os.Remove(tmp)
		return err
	}
	if err := file.Close(); err != nil {
		os.Remove(tmp)
		return err
	}
	return os.Rename(tmp, path)
}

func readSavedImageDigestsAt(path string) (map[string]string, bool) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, false
	}
	var sidecar savedImageDigests
	if err := json.Unmarshal(data, &sidecar); err != nil || len(sidecar.Digests) == 0 {
		return nil, false
	}
	return sidecar.Digests, true
}

// publishSavedImageDigestsAt records digests next to a freshly published
// archive. A failed write must never leave the previous binding in place:
// the archive on disk is already the new one, so after a metadata reset
// recovery would compare it against the stale digest, reject a valid
// archive, and strand an offline app whose registry fallback cannot run.
// Removing the sidecar instead reads as unproven — the conservative
// direction already used for archives published before this scheme.
func publishSavedImageDigestsAt(path string, digests map[string]string) error {
	if err := writeSavedImageDigestsAt(path, digests); err != nil {
		_ = os.Remove(path)
		return err
	}
	return nil
}

// recordSavedImageDigests binds a freshly published archive to the digests
// containerd currently holds behind refs, so a later no-upload update can
// prove the archive still belongs to the installed images instead of trusting
// mutable reference strings.
func (s *AppManagerServer) recordSavedImageDigests(ctx context.Context, appID string, refs []string) {
	if s.client == nil {
		return
	}
	digests := make(map[string]string, len(refs))
	for _, ref := range refs {
		image, err := s.client.GetImage(ctx, ref)
		if err != nil {
			logger.Warn("[IMAGE-RECOVERY] Cannot bind archive digests for %s: %v", ref, err)
			// A new archive must never inherit the previous binding.
			_ = os.Remove(containerd.SavedImageDigestsPath(appID))
			return
		}
		digests[ref] = image.Target().Digest.String()
	}
	if err := publishSavedImageDigestsAt(containerd.SavedImageDigestsPath(appID), digests); err != nil {
		logger.Warn("[IMAGE-RECOVERY] Failed to record archive digest binding for %s: %v", appID, err)
	}
}

// savedArchiveDigestsProven reports whether the saved archive's recorded
// digests still match what containerd holds behind every reference. A mutable
// tag re-pushed with different content keeps its name but changes its digest,
// which reference-string comparison alone cannot see.
func (s *AppManagerServer) savedArchiveDigestsProven(ctx context.Context, appID string, refs []string) bool {
	if s.client == nil {
		// The server deliberately continues with a nil client when
		// containerd is unavailable; a binding that cannot be checked is
		// unproven, never verified.
		return false
	}
	saved, ok := readSavedImageDigestsAt(containerd.SavedImageDigestsPath(appID))
	if !ok {
		return false
	}
	for _, ref := range refs {
		want, recorded := saved[ref]
		if !recorded {
			return false
		}
		image, err := s.client.GetImage(ctx, ref)
		if err != nil {
			return false
		}
		if image.Target().Digest.String() != want {
			return false
		}
	}
	return true
}

// purgeRestoredReferences drops a rejected archive and every reference its
// import created. Deleting the tar alone leaves the stale references
// resolving as available: callers that re-check availability would find
// nothing missing, skip the registry pull, and run the rejected binary.
func (s *AppManagerServer) purgeRestoredReferences(ctx context.Context, appID string, refs []string) {
	if rmErr := containerd.RemoveSavedImageTar(appID); rmErr != nil {
		logger.Warn("[IMAGE-RECOVERY] Failed to drop rejected archive for %s: %v", appID, rmErr)
	}
	if s.client == nil {
		return
	}
	for _, rejected := range refs {
		if rmErr := s.client.RemoveImage(ctx, rejected, true); rmErr != nil {
			logger.Warn("[IMAGE-RECOVERY] Failed to purge rejected reference %s: %v", rejected, rmErr)
		}
	}
}

// migrateUnboundSavedArchive binds a legacy archive — one published before
// the digest sidecar existed — to the images containerd currently holds,
// while that metadata is still available. The binding is what lets a later
// restore validate the archive instead of trusting it blindly: if a
// pre-sidecar release force-updated the app remotely and left a stale tar
// behind, the recorded current digests will not match it and recovery will
// reject the archive rather than silently running the old binary. Best
// effort — recordSavedImageDigests already removes the sidecar on failure,
// which reads as unproven and keeps the conservative direction.
func (s *AppManagerServer) migrateUnboundSavedArchive(ctx context.Context, appID string, refs []string) {
	if s.client == nil || len(refs) == 0 {
		return
	}
	if _, err := os.Stat(containerd.SavedImageTarPath(appID)); err != nil {
		return // no archive, nothing to bind
	}
	if _, ok := readSavedImageDigestsAt(containerd.SavedImageDigestsPath(appID)); ok {
		return // already bound
	}
	logger.Info("[IMAGE-RECOVERY] Binding legacy archive for %s to the currently installed images", appID)
	s.recordSavedImageDigests(ctx, appID, refs)
}

// recoverySourceCandidates returns only archive images that can legitimately
// satisfy requiredRef. The primary image retains the install-time behavior of
// binding the archive's first image to the manifest's primary reference; all
// other images must have a matching RepoTag so a multi-container app can never
// accidentally run its primary image in every container.
func recoverySourceCandidates(requiredRef string, primary bool, archivedTags []string, importedPrimary string) []string {
	seen := make(map[string]bool)
	var candidates []string
	add := func(ref string) {
		if ref == "" || seen[ref] {
			return
		}
		seen[ref] = true
		candidates = append(candidates, ref)
	}

	for _, tag := range archivedTags {
		if manifest.NormalizeImageName(tag) != requiredRef {
			continue
		}
		add(tag)
		add(manifest.NormalizeImageName(tag))
	}
	if primary {
		add(importedPrimary)
	}
	return candidates
}

// unavailableImageReferences checks both the image index and its unpacked
// overlayfs state. An image record without usable snapshots is not startable.
func (s *AppManagerServer) unavailableImageReferences(ctx context.Context, refs []string) []string {
	var unavailable []string
	for _, ref := range refs {
		if _, err := s.client.GetImage(ctx, ref); err != nil {
			unavailable = append(unavailable, ref)
			continue
		}
		if err := s.client.EnsureImageUnpacked(ctx, ref); err != nil {
			logger.Warn("Image %s is indexed but not usable: %v", ref, err)
			unavailable = append(unavailable, ref)
		}
	}
	return unavailable
}

// restoreAppImagesFromSavedTar rebuilds containerd's volatile image index
// from the per-app archive retained on persistent storage. It also recreates
// normalized manifest tags, which are metadata and are lost with meta.db.
func (s *AppManagerServer) restoreAppImagesFromSavedTar(ctx context.Context, appID string, refs []string) error {
	if err := requireSafePathSegment("app id", appID); err != nil {
		return err
	}
	if len(refs) == 0 {
		return nil
	}

	tarPath := containerd.SavedImageTarPath(appID)
	if _, err := os.Stat(tarPath); err != nil {
		return fmt.Errorf("saved image archive %s is unavailable: %w", tarPath, err)
	}

	archivedTags := utils.ExtractAllImageNamesFromTar(tarPath)
	logger.Info("[IMAGE-RECOVERY] Re-importing app %s from %s (RepoTags: %v)", appID, tarPath, archivedTags)
	importedPrimary, err := s.client.ImportImage(ctx, tarPath, "")
	if err != nil {
		return fmt.Errorf("import saved image archive %s: %w", tarPath, err)
	}

	var failures []string
	for i, ref := range refs {
		needsRetag := false
		if _, getErr := s.client.GetImage(ctx, ref); getErr != nil {
			needsRetag = true
		} else if unpackErr := s.client.EnsureImageUnpacked(ctx, ref); unpackErr != nil {
			// The record survived but its target/snapshot state did not. Replace
			// the manifest tag from the freshly imported archive before retrying.
			logger.Warn("[IMAGE-RECOVERY] Existing image %s is unusable after import: %v", ref, unpackErr)
			needsRetag = true
		}
		if needsRetag {
			var tagErr error
			for _, source := range recoverySourceCandidates(ref, i == 0, archivedTags, importedPrimary) {
				if source == ref {
					continue
				}
				if _, sourceErr := s.client.GetImage(ctx, source); sourceErr != nil {
					continue
				}
				if tagErr = s.client.TagImage(ctx, source, ref); tagErr == nil {
					logger.Info("[IMAGE-RECOVERY] Restored manifest tag %s from %s", ref, source)
					break
				}
			}
			if _, verifyErr := s.client.GetImage(ctx, ref); verifyErr != nil {
				detail := verifyErr.Error()
				if tagErr != nil {
					detail = tagErr.Error()
				}
				failures = append(failures, fmt.Sprintf("%s (%s)", ref, detail))
				continue
			}
		}
		if unpackErr := s.client.EnsureImageUnpacked(ctx, ref); unpackErr != nil {
			failures = append(failures, fmt.Sprintf("%s (%v)", ref, unpackErr))
		}
	}

	// Validate the archive's digest binding before declaring any outcome.
	// This must precede the partial-failure return below: the import above
	// already populated every reference it could, so a mismatched primary
	// would otherwise stay resolvable, the caller's availability re-check
	// would report only the failed secondary as missing, and the registry
	// pull would complete around the rejected primary still being served.
	saved, bound := readSavedImageDigestsAt(containerd.SavedImageDigestsPath(appID))
	if !bound {
		// An archive with no binding (or an unreadable one) is unproven —
		// the same conservative direction the update paths already use.
		// Devices upgrading from a pre-sidecar release carry such archives,
		// and a stale one left behind by an old force-update must not be
		// retagged onto the current references. Reject and let the
		// registry fallback run.
		s.purgeRestoredReferences(ctx, appID, refs)
		return fmt.Errorf("saved archive for %s carries no digest binding; rejecting it in favor of the registry fallback", appID)
	}
	for _, ref := range refs {
		want, recorded := saved[ref]
		if !recorded {
			continue
		}
		image, err := s.client.GetImage(ctx, ref)
		if err != nil {
			continue // unresolvable references are reported via failures below
		}
		if image.Target().Digest.String() != want {
			// The import above already populated the stale references, so
			// merely failing would leave them resolving as available:
			// callers that re-check availability would find nothing
			// missing, skip the registry pull, and run the rejected
			// binary. Purge what the rejected archive brought in so the
			// fallback actually engages.
			s.purgeRestoredReferences(ctx, appID, refs)
			return fmt.Errorf("saved archive digest for %s does not match the binding recorded at publication", ref)
		}
	}
	if len(failures) > 0 {
		return fmt.Errorf("saved archive did not restore required images: %s", strings.Join(failures, "; "))
	}
	logger.Info("[IMAGE-RECOVERY] App %s images restored from persistent archive", appID)
	return nil
}

// ensureAppImagesAvailable prefers the persistent local archive and only uses
// the registry as a fallback. This keeps imported/private apps startable on an
// offline device after an OS upgrade resets containerd metadata.
func (s *AppManagerServer) ensureAppImagesAvailable(ctx context.Context, appID string, refs []string, progress *InstallTask) error {
	ctx, cancel := imageRecoveryContext(ctx)
	defer cancel()

	updateStartProgress(progress, "checking_image", 5, "Checking application images...")
	missing := s.unavailableImageReferences(ctx, refs)
	if len(missing) == 0 {
		// Everything is already available: this is the last moment the
		// current image metadata can bind a legacy pre-sidecar archive.
		s.migrateUnboundSavedArchive(ctx, appID, refs)
		updateStartProgress(progress, "image_ready", 55, "Application images are ready")
		return nil
	}

	logger.Warn("Images required by app %s are unavailable locally: %v", appID, missing)
	updateStartProgress(progress, "restoring_image", 20, "Restoring application images from local storage...")
	localErr := s.restoreAppImagesFromSavedTar(ctx, appID, refs)
	if localErr == nil {
		updateStartProgress(progress, "image_ready", 55, "Application images restored")
		return nil
	}
	logger.Warn("[IMAGE-RECOVERY] Local recovery failed for app %s: %v", appID, localErr)

	missing = s.unavailableImageReferences(ctx, refs)
	for _, ref := range missing {
		logger.Warn("Image %s still unavailable after local recovery; attempting registry pull", ref)
		updateStartProgress(progress, "pulling_image", 40, fmt.Sprintf("Pulling application image %s...", ref))
		if pullErr := s.client.PullImage(ctx, ref); pullErr != nil {
			return fmt.Errorf("image %s is unavailable; local recovery failed: %v; registry pull failed: %w", ref, localErr, pullErr)
		}
	}

	if remaining := s.unavailableImageReferences(ctx, refs); len(remaining) > 0 {
		return fmt.Errorf("images remain unavailable after local recovery and registry pull: %v", remaining)
	}
	updateStartProgress(progress, "image_ready", 55, "Application images are ready")
	return nil
}

// restorePersistedAppImages proactively repairs every installed app after the
// snapshot check. This catches the OS-upgrade failure mode where containerd's
// image list is empty (and therefore contains no corrupt image to inspect).
func (s *AppManagerServer) restorePersistedAppImages() {
	if s.client == nil {
		return
	}
	ctx := s.withNamespace(context.Background())
	for _, appInfo := range s.registry.List() {
		unlockApp := s.lockAppMutation(appInfo.ID)
		if appInfo.ManifestPath == "" {
			unlockApp()
			continue
		}
		appManifest, err := manifest.LoadManifest(appInfo.ManifestPath)
		if err != nil {
			logger.Warn("[IMAGE-RECOVERY] Skip app %s: failed to load manifest: %v", appInfo.ID, err)
			unlockApp()
			continue
		}
		refs := imageReferencesForApp(appInfo, appManifest)
		missing := s.unavailableImageReferences(ctx, refs)
		if len(missing) == 0 {
			unlockApp()
			continue
		}
		logger.Warn("[IMAGE-RECOVERY] App %s is missing required images %v; restoring persistent archive", appInfo.ID, missing)
		if err := s.restoreAppImagesFromSavedTar(ctx, appInfo.ID, refs); err != nil {
			// Do not pull all installed apps during boot. StartApp retains the
			// online fallback and reports a precise error to its caller.
			logger.Error("[IMAGE-RECOVERY] Startup restore failed for app %s: %v", appInfo.ID, err)
		}
		unlockApp()
	}
}
