package registry

import (
	"os"
	"path/filepath"
	"testing"
)

func newFailureRegistry(t *testing.T) (*Registry, func()) {
	t.Helper()
	dir := t.TempDir()
	r, err := NewRegistry(dir)
	if err != nil {
		t.Fatal(err)
	}
	// Rename the backing directory away so save's CreateTemp fails while the
	// in-memory registry remains usable for rollback assertions.
	moved := dir + "-moved"
	if err := os.Rename(dir, moved); err != nil {
		t.Fatal(err)
	}
	return r, func() { _ = os.Rename(moved, dir) }
}

func TestRegisterSaveFailureRestoresMemory(t *testing.T) {
	r, restore := newFailureRegistry(t)
	defer restore()
	app := &AppInfo{ID: "a"}
	if err := r.Register(app); err == nil {
		t.Fatal("Register succeeded with missing data directory")
	}
	if r.Exists("a") {
		t.Fatal("failed Register leaked app into memory")
	}
}

func TestUpdateSaveFailureRestoresMemory(t *testing.T) {
	dir := t.TempDir()
	r, err := NewRegistry(dir)
	if err != nil {
		t.Fatal(err)
	}
	if err := r.Register(&AppInfo{ID: "a", Name: "old"}); err != nil {
		t.Fatal(err)
	}
	moved := dir + "-moved"
	if err := os.Rename(dir, moved); err != nil {
		t.Fatal(err)
	}
	defer os.Rename(moved, dir)
	if err := r.Update(&AppInfo{ID: "a", Name: "new"}); err == nil {
		t.Fatal("Update succeeded with missing data directory")
	}
	got, _ := r.Get("a")
	if got.Name != "old" {
		t.Fatalf("failed Update left name %q", got.Name)
	}
}

func TestUnregisterSaveFailureRestoresMemory(t *testing.T) {
	dir := t.TempDir()
	r, err := NewRegistry(dir)
	if err != nil {
		t.Fatal(err)
	}
	if err := r.Register(&AppInfo{ID: "a"}); err != nil {
		t.Fatal(err)
	}
	moved := filepath.Clean(dir + "-moved")
	if err := os.Rename(dir, moved); err != nil {
		t.Fatal(err)
	}
	defer os.Rename(moved, dir)
	if err := r.Unregister("a"); err == nil {
		t.Fatal("Unregister succeeded with missing data directory")
	}
	if !r.Exists("a") {
		t.Fatal("failed Unregister removed app from memory")
	}
}

func TestSaveUsesAtomicReplacement(t *testing.T) {
	dir := t.TempDir()
	r, err := NewRegistry(dir)
	if err != nil {
		t.Fatal(err)
	}
	if err := r.Register(&AppInfo{ID: "a"}); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Join(dir, "registry.json")); err != nil {
		t.Fatal(err)
	}
	matches, _ := filepath.Glob(filepath.Join(dir, ".registry.json-*"))
	if len(matches) != 0 {
		t.Fatalf("temporary registry files remain: %v", matches)
	}
}
