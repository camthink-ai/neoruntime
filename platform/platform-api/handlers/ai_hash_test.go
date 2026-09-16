package handlers

import (
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"testing"

	"aipc/platform/platform-api/model"
)

// sha256Bytes is the test oracle for sha256File.
func sha256Bytes(t *testing.T, data []byte) string {
	t.Helper()
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

func TestBackfillMissingHashes(t *testing.T) {
	h := newAISyncTestHandler(t)
	dir := t.TempDir()

	// A disk-seeded row with no hash — the backfill target.
	targetPath := filepath.Join(dir, "yolov5m.hef")
	targetBody := []byte("fake hef bytes 001")
	if err := os.WriteFile(targetPath, targetBody, 0o644); err != nil {
		t.Fatalf("write target: %v", err)
	}
	target := &model.AIModel{ModelID: "yolov5m_vehicles", FilePath: targetPath, Source: "disk"}

	// An already-hashed row whose file no longer matches its stored hash —
	// must be skipped, not recomputed (import-time identity is sticky).
	stalePath := filepath.Join(dir, "stale.hef")
	if err := os.WriteFile(stalePath, []byte("newer bytes"), 0o644); err != nil {
		t.Fatalf("write stale: %v", err)
	}
	hashed := &model.AIModel{ModelID: "web_imported", FilePath: stalePath, FileHash: "deadbeef", Source: "manual"}

	// An app-owned row with no hash — invisible on the model page, skip.
	ownedPath := filepath.Join(dir, "app.bin")
	if err := os.WriteFile(ownedPath, []byte("app model"), 0o644); err != nil {
		t.Fatalf("write owned: %v", err)
	}
	owned := &model.AIModel{ModelID: "app_private", FilePath: ownedPath, OwnerAppID: "shelf-ops", Source: "dynamic"}

	// A row whose file vanished — skipped without failing the whole pass.
	missing := &model.AIModel{ModelID: "ghost", FilePath: filepath.Join(dir, "ghost.hef"), Source: "disk"}

	for _, m := range []*model.AIModel{target, hashed, owned, missing} {
		if err := h.aiModelRepo.Create(m); err != nil {
			t.Fatalf("create %s: %v", m.ModelID, err)
		}
	}

	h.BackfillMissingHashes()

	got := map[string]*model.AIModel{}
	for _, m := range []*model.AIModel{target, hashed, owned, missing} {
		row, err := h.aiModelRepo.GetByModelID(m.ModelID)
		if err != nil {
			t.Fatalf("reload %s: %v", m.ModelID, err)
		}
		got[m.ModelID] = row
	}

	if want := sha256Bytes(t, targetBody); got["yolov5m_vehicles"].FileHash != want {
		t.Errorf("target hash = %q, want %q", got["yolov5m_vehicles"].FileHash, want)
	}
	if got["web_imported"].FileHash != "deadbeef" {
		t.Errorf("already-hashed row was rewritten: %q", got["web_imported"].FileHash)
	}
	if got["app_private"].FileHash != "" {
		t.Errorf("app-owned row was hashed: %q", got["app_private"].FileHash)
	}
	if got["ghost"].FileHash != "" {
		t.Errorf("missing-file row was hashed: %q", got["ghost"].FileHash)
	}
}
