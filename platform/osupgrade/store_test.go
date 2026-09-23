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

func TestNeedsVerificationOnlyForPostBootStates(t *testing.T) {
	states := []struct {
		state State
		want  bool
	}{
		{StateIdle, false},
		{StateUploading, false},
		{StateValidating, false},
		{StateReady, false},
		{StateInstalling, false},
		{StateInstalled, true},
		{StateAwaitingReboot, true},
		{StateRebooting, true},
		{StateVerifying, true},
		{StateRollback, true},
		{StateSuccess, false},
		{StateFailed, false},
		{StateCancelled, false},
	}

	for _, tt := range states {
		t.Run(string(tt.state), func(t *testing.T) {
			store := NewStore(t.TempDir())
			job := &Job{ID: "job", State: tt.state}
			if err := store.Save(job); err != nil {
				t.Fatal(err)
			}
			if err := store.SetActive(job.ID); err != nil {
				t.Fatal(err)
			}

			got, err := NewRunner(store).NeedsVerification()
			if err != nil {
				t.Fatal(err)
			}
			if got != tt.want {
				t.Fatalf("NeedsVerification() = %v, want %v", got, tt.want)
			}
		})
	}
}

func TestNeedsVerificationWithoutActiveJob(t *testing.T) {
	got, err := NewRunner(NewStore(t.TempDir())).NeedsVerification()
	if err != nil {
		t.Fatal(err)
	}
	if got {
		t.Fatal("missing active job must not trigger boot verification")
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
