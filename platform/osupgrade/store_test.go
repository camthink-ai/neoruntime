package osupgrade

import (
	"os"
	"path/filepath"
	"testing"
)

func TestRollbackIsNotTerminal(t *testing.T) {
	if (Job{State: StateRollback}).Terminal() {
		t.Fatal("rollback is an in-progress state, not a terminal state")
	}
	for _, state := range []State{StateSuccess, StateFailed, StateCancelled} {
		if !(Job{State: state}).Terminal() {
			t.Fatalf("state %s should be terminal", state)
		}
	}
}

func TestRemovePackageOnlyForTerminalJob(t *testing.T) {
	store := NewStore(t.TempDir())
	job := &Job{ID: "job", State: StateVerifying}
	if err := store.Save(job); err != nil {
		t.Fatal(err)
	}
	if err := store.SetActive(job.ID); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(store.PackagePath(job.ID), []byte("package"), 0640); err != nil {
		t.Fatal(err)
	}
	if err := store.RemovePackage(job); err == nil {
		t.Fatal("expected non-terminal package removal to be rejected")
	}
	if _, err := os.Stat(store.PackagePath(job.ID)); err != nil {
		t.Fatalf("active package was removed: %v", err)
	}
	if _, err := os.Stat(store.ActivePath()); err != nil {
		t.Fatalf("active job pointer was removed: %v", err)
	}

	job.State = StateSuccess
	if err := store.Save(job); err != nil {
		t.Fatal(err)
	}
	if err := store.RemovePackage(job); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(filepath.Clean(store.PackagePath(job.ID))); !os.IsNotExist(err) {
		t.Fatalf("terminal package still exists: %v", err)
	}
}
