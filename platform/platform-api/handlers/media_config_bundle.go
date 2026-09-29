package handlers

// OSD-aware media-config bundle: a tar.gz that carries the full media-config
// envelope PLUS the OSD overlay image binaries referenced by osd_config.json.
//
// Why this exists alongside media_config_io.go: the pure-JSON /config/export
// snapshots the seven config sources but NOT the ~25MB PNG/BMP overlay images
// living under /data/aipc/etc/osd. Importing that JSON onto a fresh device
// leaves every image_path overlay pointing at a missing file. The bundle is the
// config + the bytes, so a clone of the media surface actually renders.
//
// Transport: a self-describing tar.gz (manifest.json holds a sha256 per file).
// Export reuses buildMediaEnvelope so the JSON-in-tar is byte-identical to what
// /config/export would have returned; import reuses validateMediaEnvelope +
// applyImportedMediaConfig so the apply path is shared, not forked.

import (
	"archive/tar"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"github.com/gin-gonic/gin"

	"aipc/platform/common/constants"
	eventLoggerPkg "aipc/platform/common/events"
	"aipc/platform/common/logger"
	"aipc/platform/platform-api/internal/atomicfile"
)

const (
	mediaBundleSchema  = "aipc.media_bundle"
	mediaBundleVersion = 1
)

// mediaBundleStaging holds scratch tar files and extraction dirs for bundle
// export/import. Kept under /data (constants.RootPath) for space; created lazily
// by each handler. A var (not const) because it derives from constants.RootPath().
var mediaBundleStaging = constants.RootPath() + "/clone-staging"

// collectOsdImages parses osd_config.json (raw) and returns the deduplicated,
// sorted list of valid image_path values (absolute paths under the osd dir that
// exist). Mirrors validOsdImagePath's rules so the bundle only carries images
// the daemon would actually load.
func collectOsdImages(osdRaw json.RawMessage) []string {
	if len(osdRaw) == 0 {
		return nil
	}
	var doc struct {
		Streams []struct {
			ImageOverlays []struct {
				ImagePath string `json:"image_path"`
			} `json:"image_overlays"`
		} `json:"streams"`
	}
	if err := json.Unmarshal(osdRaw, &doc); err != nil {
		return nil
	}
	seen := map[string]struct{}{}
	var out []string
	for _, s := range doc.Streams {
		for _, img := range s.ImageOverlays {
			p, ok := validOsdImagePath(img.ImagePath)
			if !ok {
				continue
			}
			if _, dup := seen[p]; dup {
				continue
			}
			seen[p] = struct{}{}
			out = append(out, p)
		}
	}
	sort.Strings(out)
	return out
}

// validOsdDestPath checks the destination path for an incoming OSD image: it
// must be .png/.bmp and confined under osdDir. Unlike validOsdImagePath it does
// NOT require the file to exist (the destination is being created), which is
// exactly what import needs.
func validOsdDestPath(dest, osdDir string) bool {
	cleaned := filepath.Clean(dest)
	if cleaned == osdDir || !strings.HasPrefix(cleaned, osdDir+string(filepath.Separator)) {
		return false
	}
	ext := strings.ToLower(filepath.Ext(cleaned))
	return ext == ".png" || ext == ".bmp"
}

func validBundleSuffix(name string) bool {
	n := strings.ToLower(name)
	return strings.HasSuffix(n, ".tar.gz") || strings.HasSuffix(n, ".tgz")
}

// ExportMediaBundle (GET /api/v1/media/config/bundle) streams a tar.gz carrying
// the full media-config envelope PLUS the OSD overlay image binaries referenced
// by osd_config.json. Pure read on config; writes only a temp file under
// mediaBundleStaging (removed after the response ships). Uses the temp-file +
// c.File() shape from debug_log so errors surface before any bytes are sent.
func (h *MediaHandlers) ExportMediaBundle(c *gin.Context) {
	env, err := h.buildMediaEnvelope()
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to build media config envelope: "+err.Error())
		return
	}
	osdImages := collectOsdImages(env.Config.Osd)

	if err := os.MkdirAll(mediaBundleStaging, 0755); err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to create staging dir: "+err.Error())
		return
	}

	devName := "device"
	if hn, e := os.Hostname(); e == nil && hn != "" {
		devName = hn
	}
	tempFile := filepath.Join(mediaBundleStaging,
		fmt.Sprintf("media_bundle_%s_%s.tar.gz", devName, time.Now().UTC().Format("20060102-150405")))

	manifest := &configManifest{
		Schema:    mediaBundleSchema,
		Version:   mediaBundleVersion,
		CreatedAt: time.Now().UTC().Format(time.RFC3339),
		Source:    map[string]string{"hostname": devName},
		Extra:     map[string]any{"envelope": "envelope.json", "osd_images": len(osdImages)},
	}
	if err := writeBundle(tempFile, env, osdImages, manifest); err != nil {
		_ = os.Remove(tempFile)
		Resp(c).FailMsg(CodeCameraError, "Failed to build bundle: "+err.Error())
		return
	}

	c.Header("Content-Disposition", `attachment; filename="`+filepath.Base(tempFile)+`"`)
	c.Header("Content-Type", "application/gzip")
	c.File(tempFile)
	go os.Remove(tempFile) // best-effort cleanup after the response drains
}

// writeBundle assembles the tar.gz at path: envelope.json first, then each OSD
// image under osd/<basename>, then manifest.json (which carries the sha256 of
// every preceding entry). manifest.Files is filled as a side effect. On error
// the partial file is removed.
func writeBundle(path string, env mediaConfigEnvelope, osdImages []string, manifest *configManifest) (retErr error) {
	f, err := os.Create(path)
	if err != nil {
		return fmt.Errorf("create bundle: %w", err)
	}
	defer func() {
		f.Close()
		if retErr != nil {
			_ = os.Remove(path)
		}
	}()

	return writeTarGz(f, func(tw *tar.Writer) error {
		envBytes, err := json.MarshalIndent(env, "", "  ")
		if err != nil {
			return fmt.Errorf("marshal envelope: %w", err)
		}
		if err := tarWriteBytes(tw, "envelope.json", envBytes, 0644); err != nil {
			return err
		}
		manifest.Files = append(manifest.Files, manifestEntry{
			Path: "envelope.json", SHA256: sha256Sum(envBytes), Size: int64(len(envBytes)), Mode: 0644,
		})

		for _, abs := range osdImages {
			arc := "osd/" + filepath.Base(abs)
			me, err := tarWriteFile(tw, abs, arc)
			if err != nil {
				return err
			}
			manifest.Files = append(manifest.Files, me)
		}

		mb, err := json.MarshalIndent(manifest, "", "  ")
		if err != nil {
			return fmt.Errorf("marshal manifest: %w", err)
		}
		return tarWriteBytes(tw, "manifest.json", mb, 0644)
	})
}

// ImportMediaBundle (POST /api/v1/media/config/import-bundle) applies a bundle
// from ExportMediaBundle: receive tar.gz → extract to staging → verify every
// manifest sha256 → snapshot current OSD dir → write OSD images → apply the
// embedded media-config envelope (same write path as /config/import) → restart
// camera-daemon + device-control. Images are written before osd_config.json so
// the daemon's boot replay sees referenced files in place. Images referenced on
// the target but absent from the bundle are left untouched (not deleted).
func (h *MediaHandlers) ImportMediaBundle(c *gin.Context) {
	if err := c.Request.ParseMultipartForm(2 << 30); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Failed to parse multipart upload: "+err.Error())
		return
	}
	file, header, err := c.Request.FormFile("file")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Missing 'file' in upload: "+err.Error())
		return
	}
	defer file.Close()
	if !validBundleSuffix(header.Filename) {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", "file must be .tar.gz or .tgz")
		return
	}

	if err := os.MkdirAll(mediaBundleStaging, 0755); err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to create staging dir: "+err.Error())
		return
	}
	extractDir := filepath.Join(mediaBundleStaging, "bundle-"+time.Now().UTC().Format("20060102-150405"))
	defer os.RemoveAll(extractDir)

	manifestJSON, members, err := untar(file, extractDir)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to extract bundle: "+err.Error())
		return
	}
	if len(manifestJSON) == 0 {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", "bundle has no manifest.json")
		return
	}
	manifest, err := decodeManifest(manifestJSON)
	if err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", err.Error())
		return
	}
	if manifest.Schema != mediaBundleSchema {
		Resp(c).FailTyped(CodeInvalidParameter, "validation",
			fmt.Sprintf("unsupported bundle schema %q (expected %q)", manifest.Schema, mediaBundleSchema))
		return
	}
	if manifest.Version != mediaBundleVersion {
		Resp(c).FailTyped(CodeInvalidParameter, "validation",
			fmt.Sprintf("unsupported bundle version %d (expected %d)", manifest.Version, mediaBundleVersion))
		return
	}
	if _, err := verifyManifest(manifest, members); err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "integrity", err.Error())
		return
	}

	// Parse + validate the embedded envelope (same checks as /config/import).
	envPath, ok := members["envelope.json"]
	if !ok {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", "bundle has no envelope.json")
		return
	}
	envBytes, err := os.ReadFile(envPath)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to read envelope.json: "+err.Error())
		return
	}
	var env mediaConfigEnvelope
	if err := json.Unmarshal(envBytes, &env); err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", "envelope.json: "+err.Error())
		return
	}
	if verr := validateMediaEnvelope(&env); verr != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", verr.Error())
		return
	}

	// Snapshot current OSD images for rollback (best-effort: dir may not exist).
	osdDir := filepath.Clean(constants.RootPath() + "/etc/osd")
	osdSnap := filepath.Join(mediaBackupRoot, "osd-"+time.Now().UTC().Format("20060102-150405"))
	_ = snapshotTree(osdDir, osdSnap)

	// Write OSD images from the bundle BEFORE applying osd_config.json so the
	// daemon's boot replay sees the referenced files. Only osd/<basename> entries
	// whose destination passes validOsdDestPath reach disk — a crafted bundle
	// cannot drop files outside the osd dir.
	osdApplied := []string{}
	for arcName, staged := range members {
		rel, ok := strings.CutPrefix(arcName, "osd/")
		if !ok || rel == "" || strings.ContainsRune(rel, '/') {
			continue
		}
		dest := filepath.Join(osdDir, rel)
		if !validOsdDestPath(dest, osdDir) {
			logger.Warn("media bundle import: refusing OSD entry outside osd dir: %s", arcName)
			continue
		}
		data, err := os.ReadFile(staged)
		if err != nil {
			Resp(c).FailMsg(CodeCameraError, "Failed to read staged "+arcName+": "+err.Error())
			return
		}
		if err := atomicfile.Write(dest, data, 0644); err != nil {
			Resp(c).FailMsg(CodeCameraError, "Failed to write OSD image "+rel+": "+err.Error())
			return
		}
		osdApplied = append(osdApplied, rel)
	}
	sort.Strings(osdApplied)

	// Apply the config envelope (writes base_yaml + 7 JSONs, restarts services).
	actor := getUsernameFromContext(c)
	ctx, cancel := context.WithTimeout(c.Request.Context(), 30*time.Second)
	defer cancel()
	applied, backupDir, err := h.applyImportedMediaConfig(ctx, actor, env)
	if err != nil {
		Resp(c).FailTyped(CodeCameraError, "import", err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.bundle.imported",
			eventLoggerPkg.MessageParams{"backup_dir": backupDir, "osd_dir": osdSnap}, actor)
	}

	Resp(c).OK(gin.H{
		"applied":        true,
		"backup_dir":     backupDir,
		"osd_backup_dir": osdSnap,
		"osd_images":     osdApplied,
		"restart":        "camera-daemon,device-control",
		"config_items":   applied,
	})
}
