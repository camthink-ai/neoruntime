package storage

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"testing"
)

func newQuotaStore(t *testing.T, minFree, maxTotal uint64) *ModelStorage {
	t.Helper()
	store, err := NewModelStorage(t.TempDir(), minFree, maxTotal)
	if err != nil {
		t.Fatalf("NewModelStorage: %v", err)
	}
	return store
}

func saveBytes(t *testing.T, store *ModelStorage, content []byte) (*SaveResult, error) {
	t.Helper()
	return store.SaveWithHash(bytes.NewReader(content), ".hef", int64(len(content)))
}

func hexHash(content []byte) string {
	sum := sha256.Sum256(content)
	return hex.EncodeToString(sum[:])
}

// A write that fits the budget succeeds; one that would push the blobs past
// the budget is refused before any bytes are staged.
func TestSaveWithHashEnforcesTotalQuota(t *testing.T) {
	store := newQuotaStore(t, 0, 100)

	small := []byte(strings.Repeat("a", 60))
	if res, err := saveBytes(t, store, small); err != nil || res.Existed {
		t.Fatalf("60-byte write under a 100-byte budget must succeed, err=%v existed=%v", err, res.Existed)
	}

	large := []byte(strings.Repeat("b", 50))
	if _, err := saveBytes(t, store, large); err == nil {
		t.Fatal("write that exceeds the total budget must be refused")
	} else if !strings.Contains(err.Error(), "quota exceeded") {
		t.Errorf("error must say the quota was exceeded, got: %v", err)
	}

	if !store.Exists(hexHash(small), ".hef") {
		t.Error("the accepted blob must survive the refused write")
	}
	if store.Exists(hexHash(large), ".hef") {
		t.Error("the refused write must not leave a blob behind")
	}
	if tmps, _ := filepath.Glob(filepath.Join(store.blobDir, "upload-*.tmp")); len(tmps) != 0 {
		t.Errorf("the refused write must not leave temp files behind, got %v", tmps)
	}
}

// Dedup against a full budget is refused: the hash — and therefore whether
// the write would dedup to zero new bytes — is unknowable before streaming,
// so admission uses the claimed size. Documented conservative behavior.
func TestSaveWithHashQuotaRefusesDedupAtCap(t *testing.T) {
	store := newQuotaStore(t, 0, 100)
	exact := []byte(strings.Repeat("c", 100))
	if _, err := saveBytes(t, store, exact); err != nil {
		t.Fatalf("seed exact-cap blob: %v", err)
	}

	if _, err := saveBytes(t, store, exact); err == nil {
		t.Fatal("re-upload of the same bytes at a full budget must be refused (conservative admission)")
	}
	if !store.Exists(hexHash(exact), ".hef") {
		t.Error("the existing blob must be untouched by the refused dedup")
	}
}

// A zero budget means uncapped — only the free-space floor applies, which is
// the default for deployments that never configure a budget.
func TestSaveWithHashZeroBudgetUncapped(t *testing.T) {
	store := newQuotaStore(t, 0, 0)
	for i := range 3 {
		content := []byte(strings.Repeat("d", 64))
		if _, err := saveBytes(t, store, content); err != nil {
			t.Fatalf("uncapped store must accept writes, iteration %d: %v", i, err)
		}
	}
}

// UsageBytes counts only hash-named blobs: staging temp files and foreign
// entries in the directory are not part of the budget.
func TestUsageBytesCountsOnlyBlobs(t *testing.T) {
	store := newQuotaStore(t, 0, 0)
	blob := []byte("payload")
	if _, err := saveBytes(t, store, blob); err != nil {
		t.Fatalf("seed blob: %v", err)
	}
	if err := os.WriteFile(filepath.Join(store.blobDir, "upload-crashed.tmp"), []byte("staged"), 0644); err != nil {
		t.Fatalf("seed tmp: %v", err)
	}
	if err := os.WriteFile(filepath.Join(store.blobDir, "README.txt"), []byte("foreign"), 0644); err != nil {
		t.Fatalf("seed foreign: %v", err)
	}

	usage, err := store.UsageBytes()
	if err != nil {
		t.Fatalf("UsageBytes: %v", err)
	}
	if usage != uint64(len(blob)) {
		t.Errorf("usage = %d, want exactly the blob size %d", usage, len(blob))
	}
}

// FreeBytes reports a sane value for a freshly made temp directory.
func TestFreeBytesSane(t *testing.T) {
	store := newQuotaStore(t, 0, 0)
	free, err := store.FreeBytes()
	if err != nil {
		t.Fatalf("FreeBytes: %v", err)
	}
	if free == 0 {
		t.Error("a fresh temp dir on a real filesystem should report free space")
	}
}

// Concurrent uploads against a shared budget: unsynchronized admissions can
// each observe the same UsageBytes, pass the check, and then publish —
// overshooting maxTotalBytes. The check-through-publish section is
// serialized, so with N same-size writes and a budget of ⌊N/2⌋+0.5 sizes,
// exactly ⌊N/2⌋ succeed and usage stays inside the budget.
func TestSaveWithHashConcurrentAdmissionsRespectBudget(t *testing.T) {
	const writers = 4
	const payload = 64
	store := newQuotaStore(t, 0, payload*2+payload/2) // fits 2, refuses the 3rd

	contents := make([][]byte, writers)
	for i := range contents {
		contents[i] = bytes.Repeat([]byte{byte('a' + i)}, payload)
	}

	start := make(chan struct{})
	errs := make([]error, writers)
	var wg sync.WaitGroup
	for i := 0; i < writers; i++ {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			<-start
			_, errs[i] = saveBytes(t, store, contents[i])
		}(i)
	}
	close(start)
	wg.Wait()

	succeeded := 0
	for i, err := range errs {
		if err == nil {
			succeeded++
		} else if !strings.Contains(err.Error(), "quota exceeded") {
			t.Errorf("writer %d failed for a non-quota reason: %v", i, err)
		}
	}
	if succeeded != 2 {
		t.Errorf("succeeded writes = %d, want exactly 2 under a %d-byte budget", succeeded, payload*2+payload/2)
	}
	usage, err := store.UsageBytes()
	if err != nil {
		t.Fatalf("UsageBytes: %v", err)
	}
	if usage != payload*2 {
		t.Errorf("usage = %d, want %d (budget %d must hold)", usage, payload*2, payload*2+payload/2)
	}
}
