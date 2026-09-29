package handlers

import (
	"crypto/sha256"
	"encoding/hex"
	"io"
	"os"
	"sync/atomic"

	"aipc/platform/common/logger"
)

// hashBackfillRunning prevents overlapping backfill passes (the startup
// trigger and the ScanModels trigger can race). A skipped pass is harmless:
// whatever is still empty is picked up by the next boot or scan.
var hashBackfillRunning atomic.Bool

// sha256File streams the file through sha256 — model binaries reach ~1.3 GB,
// so nothing here may read them wholesale.
func sha256File(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// BackfillMissingHashes computes and persists the sha256 of device-level
// models whose file_hash is empty. Disk-seeded rows never carried a hash —
// only the web import flow computed one. The hash is the model's identity on
// the model page (there is no version field), letting the update flow and the
// user tell whether a file actually changed. IO-heavy by design, so callers
// run it in the background; it must never block an API request.
func (h *APIHandlers) BackfillMissingHashes() {
	if h.aiModelRepo == nil {
		return
	}
	if !hashBackfillRunning.CompareAndSwap(false, true) {
		return
	}
	defer hashBackfillRunning.Store(false)

	rows, err := h.aiModelRepo.List()
	if err != nil {
		logger.Warn("hash backfill: list models: %v", err)
		return
	}
	for _, row := range rows {
		// App-owned rows are invisible on the model page; already-hashed
		// rows keep their import-time identity.
		if row.FileHash != "" || row.OwnerAppID != "" || row.FilePath == "" {
			continue
		}
		info, err := os.Stat(row.FilePath)
		if err != nil || !info.Mode().IsRegular() {
			logger.Warn("hash backfill: %s: file missing or not regular: %s", row.ModelID, row.FilePath)
			continue
		}
		hash, err := sha256File(row.FilePath)
		if err != nil {
			logger.Warn("hash backfill: %s: %v", row.ModelID, err)
			continue
		}
		if err := h.aiModelRepo.UpdateFileHash(row.ModelID, hash); err != nil {
			logger.Warn("hash backfill: save %s: %v", row.ModelID, err)
			continue
		}
		logger.Info("hash backfill: %s → %s (%d bytes)", row.ModelID, hash, info.Size())
	}
}
