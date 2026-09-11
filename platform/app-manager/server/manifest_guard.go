package server

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"

	"aipc/platform/common/constants"
	"aipc/platform/common/logger"
)

// safeAppIDPattern bounds app IDs used as directory names under the
// managed manifests root. manifest.Validate only rejects empty IDs, so
// this is the only thing stopping an ID like "../bin" from turning
// into a path.
var (
	safeAppIDPattern        = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]*$`)
	appStagingTokenPattern  = regexp.MustCompile(`^[0-9]{10,20}_[0-9a-f]{8}$`)
	appInflightTokenPattern = regexp.MustCompile(`^inflight-[A-Za-z0-9._-]+-[0-9]+$`)
)

// requireSafePathSegment guards app IDs and model aliases before they become
// directory names under the data root (appModelsDir/<alias>). Install runs
// bundled-model extraction before canonicalizeManifest — the step that
// otherwise rejects unsafe IDs — and extraction's failure rollback
// RemoveAlls the derived directory, so a value like ".." or "../.." would
// resolve outside the app's own tree, up to the data root itself.
func requireSafePathSegment(kind, value string) error {
	if !safeAppIDPattern.MatchString(value) {
		return fmt.Errorf("%s %q is not usable as a directory name", kind, value)
	}
	return nil
}

func managedManifestsRoot() string {
	// Derived from the shared root rather than Apps.ManifestsPath config:
	// that field is declared but unused on this side, and its value on
	// devices (/etc/aipc/apps) does not match where manifests actually
	// live. platform-api owns the real convention: <root>/apps/manifests.
	return filepath.Join(constants.RootPath(), "apps", "manifests")
}

// managedManifestDir reports the directory that owns manifestPath, but
// only when it is exactly <root>/apps/manifests/<appID> — a directory
// the platform created for this specific app. Anything else returns
// false: the manifests root itself, the install root, /tmp, another
// app's directory, or a path forged with separators in the app ID.
func managedManifestDir(manifestPath, appID string) (string, bool) {
	if manifestPath == "" || appID == "" || !safeAppIDPattern.MatchString(appID) {
		return "", false
	}
	dir := filepath.Dir(filepath.Clean(manifestPath))
	if dir != filepath.Join(managedManifestsRoot(), appID) {
		return "", false
	}
	return dir, true
}

// canonicalizeManifest copies the manifest file into the managed
// manifests root as <root>/apps/manifests/<appID>/app.yaml and returns
// that path. Callers arrive with arbitrary paths: platform-api uploads
// already land in the root (the copy is skipped), but CLI installs
// point at unpacked tarballs in /tmp and legacy installs at the
// install-root top level — paths whose parent directories uninstall
// cleanup must never act on, so the registry records the canonical
// copy instead. Source bytes are copied verbatim (comments and
// unknown fields survive); the caller-owned source file is left in
// place.
func canonicalizeManifest(manifestPath, appID string) (string, error) {
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		return "", fmt.Errorf("failed to read manifest %s: %w", manifestPath, err)
	}
	return canonicalizeManifestBytes(data, manifestPath, appID)
}

// canonicalizeManifestBytes publishes an immutable manifest snapshot. Async
// installs read caller-owned staging before returning a task id, so a later
// cancel/TTL cleanup cannot change the bytes that eventually commit.
func canonicalizeManifestBytes(data []byte, sourcePath, appID string) (string, error) {
	if !safeAppIDPattern.MatchString(appID) {
		return "", fmt.Errorf("app id %q is not usable as a manifest directory name", appID)
	}
	canonical := filepath.Join(managedManifestsRoot(), appID, "app.yaml")
	if filepath.Clean(sourcePath) == canonical {
		return canonical, nil
	}
	if err := os.MkdirAll(filepath.Dir(canonical), 0755); err != nil {
		return "", fmt.Errorf("failed to create manifest dir %s: %w", filepath.Dir(canonical), err)
	}
	if err := atomicWriteManifest(canonical, data); err != nil {
		return "", fmt.Errorf("failed to write canonical manifest %s: %w", canonical, err)
	}
	logger.Info("Manifest canonicalized: %s -> %s", sourcePath, canonical)
	return canonical, nil
}

func atomicWriteManifest(path string, data []byte) error {
	tmp, err := os.CreateTemp(filepath.Dir(path), ".app.yaml-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if err := tmp.Chmod(0644); err != nil {
		tmp.Close()
		return err
	}
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmpName, path)
}

// manifestPublishRollback snapshots the previous canonical manifest and returns
// an idempotent compensation function. Install callers invoke the returned
// rollback if any step after canonicalizeManifest fails.
func manifestPublishRollback(appID string) (func(), error) {
	if !safeAppIDPattern.MatchString(appID) {
		return nil, fmt.Errorf("app id %q is not usable as a manifest directory name", appID)
	}
	canonical := filepath.Join(managedManifestsRoot(), appID, "app.yaml")
	old, err := os.ReadFile(canonical)
	existed := err == nil
	if err != nil && !os.IsNotExist(err) {
		return nil, err
	}
	var once bool
	return func() {
		if once {
			return
		}
		once = true
		if existed {
			if err := atomicWriteManifest(canonical, old); err != nil {
				logger.Warn("Rollback: failed to restore canonical manifest %s: %v", canonical, err)
			}
			return
		}
		if err := os.Remove(canonical); err != nil && !os.IsNotExist(err) {
			logger.Warn("Rollback: failed to remove canonical manifest %s: %v", canonical, err)
		}
		_ = os.Remove(filepath.Dir(canonical))
	}, nil
}
