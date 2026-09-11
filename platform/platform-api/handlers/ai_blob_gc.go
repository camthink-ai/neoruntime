package handlers

import (
	"os"
	"path/filepath"
	"strings"
	"time"

	"aipc/platform/common/constants"
	"aipc/platform/common/logger"
	"aipc/platform/platform-api/model"
)

// Blob reclamation once the last reference goes away. Every delete/failure
// path converges on the reference-count rule AbandonStagedModel established
// (fail closed, zero count deletes), so a deduped upload can never take a
// live model's blob with it; a periodic sweep collects the orphans no
// request path cleans up (crashed imports, superseded file swaps).

// cleanupStagedBlob reclaims a blob this request staged when the request then
// fails. existed marks a dedup hit — the blob predates this request, so only
// a zero reference count may reclaim it. A blob the request itself created is
// still counted first: a racing RegisterModel(file_hash=...) may have
// consumed it in the window since staging.
func (h *APIHandlers) cleanupStagedBlob(fileHash, ext string, existed bool) {
	if fileHash == "" || h.modelStore == nil {
		return
	}

	// Count and delete are one atomic decision against every admission path.
	// Otherwise a register/update could commit a reference after the count but
	// before Delete and be left pointing at a missing blob.
	h.blobRefMu.Lock()
	defer h.blobRefMu.Unlock()

	if h.aiModelRepo != nil {
		count, err := h.aiModelRepo.CountByFileHash(fileHash)
		// Fail closed on reference-count errors: a DB hiccup must never
		// turn a failed upload into deleting a live model's blob.
		if err != nil || count > 0 {
			return
		}
	} else if existed {
		// No DB to check references against; never reclaim a blob that
		// predates this request.
		return
	}
	if err := h.modelStore.Delete(fileHash, ext); err != nil {
		logger.Warn("Failed to clean up staged blob %s: %v", fileHash, err)
	}
}

// removeModelFile reclaims whatever the platform owns once the row that
// pointed at it is gone (the caller deletes the row first, so the reference
// count already excludes it):
//   - CAS blobs follow the reference-count rule. Deleting the blob path
//     never breaks a live registration — materialized runtime copies are
//     hardlinks sharing the inode.
//   - A raw FilePath (disk-scan rows, legacy no-store uploads) is removed
//     only inside the platform model root. An app's bundled HEF or a
//     user-registered model_path outside the root is not ours to delete.
func (h *APIHandlers) removeModelFile(m *model.AIModel) {
	if m == nil {
		return
	}
	if m.FileHash != "" && h.modelStore != nil {
		h.cleanupStagedBlob(m.FileHash, ".hef", true)
		// Backfilled disk-scan rows carry the content hash of a file that
		// lives outside the blob store: the CAS delete above is a no-op and
		// the real file is reclaimed by the root rule below.
		if filepath.Clean(m.FilePath) == h.modelStore.BlobPath(m.FileHash, ".hef") {
			return
		}
	}
	if m.FilePath == "" {
		return
	}
	if !h.isPlatformOwnedModelPath(m.FilePath) {
		logger.Info("Model file %s lives outside the platform model root; leaving it in place", m.FilePath)
		return
	}
	if err := os.Remove(m.FilePath); err != nil && !os.IsNotExist(err) {
		logger.Warn("Failed to remove model file %s: %v", m.FilePath, err)
	}
}

// isPlatformOwnedModelPath reports whether path sits under a directory the
// platform itself writes model files into (the model root and the legacy
// configured storage path). Foreign locations — app install directories,
// arbitrary model_path registrations — survive row deletion.
func (h *APIHandlers) isPlatformOwnedModelPath(path string) bool {
	abs, err := filepath.Abs(path)
	if err != nil {
		return false
	}
	roots := []string{constants.ModelsPath()}
	if h.modelStorage != "" {
		roots = append(roots, h.modelStorage)
	}
	for _, root := range roots {
		rootAbs, err := filepath.Abs(root)
		if err != nil {
			continue
		}
		if rel, err := filepath.Rel(rootAbs, abs); err == nil &&
			rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator)) {
			return true
		}
	}
	return false
}

// modelBlobGCGrace is how long a blob must sit unreferenced before the sweep
// may collect it — long enough for a wizard parse→register round trip, short
// enough that a crashed import's model file does not linger for good.
const modelBlobGCGrace = time.Hour

// sweepOrphanBlobs deletes CAS blobs no row references, plus stale upload
// temp files. It rides the self-heal tick and fails closed per blob: any
// reference-count error keeps the blob for the next pass.
func (h *APIHandlers) sweepOrphanBlobs(grace time.Duration) {
	if h.modelStore == nil || h.aiModelRepo == nil {
		return
	}
	removedTemp := h.modelStore.RemoveStaleTempFiles(grace)
	blobs, err := h.modelStore.ListBlobs()
	if err != nil {
		logger.Warn("Model blob sweep failed to list blobs: %v", err)
		return
	}
	removed := 0
	for _, b := range blobs {
		if time.Since(b.ModTime) < grace {
			continue
		}
		count, err := h.aiModelRepo.CountByFileHash(b.Hash)
		if err != nil || count > 0 {
			continue
		}
		// Reference count and delete must be atomic against admission
		// (blobRefMu): a register/update/upload mid-flight could be holding
		// an Exists proof for this very blob and commit its referencing row
		// right after this count — deleting then would leave a registered
		// model pointing at a missing HEF. The recount inside the lock is
		// the authoritative one; the pre-check above only skips lock-free
		// work for referenced blobs.
		h.blobRefMu.Lock()
		count, err = h.aiModelRepo.CountByFileHash(b.Hash)
		if err == nil && count == 0 {
			if err := h.modelStore.Delete(b.Hash, b.Ext); err == nil {
				removed++
			}
		}
		h.blobRefMu.Unlock()
	}
	if removed+removedTemp > 0 {
		logger.Info("Model blob sweep reclaimed %d orphan blob(s) and %d stale temp file(s)", removed, removedTemp)
	}
}
