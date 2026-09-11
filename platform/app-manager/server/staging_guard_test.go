package server

import (
	"os"
	"path/filepath"
	"testing"

	"aipc/platform/common/constants"
)

func TestClaimAppStagingOwnsArtifactsUntilTaskCleanup(t *testing.T) {
	oldRoot := constants.RootPath()
	root := t.TempDir()
	constants.SetRootPath(root)
	t.Cleanup(func() { constants.SetRootPath(oldRoot) })

	stage := filepath.Join(root, "apps", "staging", "1750000000_1234abcd")
	if err := os.MkdirAll(stage, 0o700); err != nil {
		t.Fatal(err)
	}
	manifestPath := filepath.Join(stage, "app.yaml")
	imagePath := filepath.Join(stage, "image.tar")
	for _, path := range []string{manifestPath, imagePath} {
		if err := os.WriteFile(path, []byte(filepath.Base(path)), 0o600); err != nil {
			t.Fatal(err)
		}
	}

	claimed, err := claimAppStaging("task-1", manifestPath, imagePath)
	if err != nil {
		t.Fatalf("claimAppStaging: %v", err)
	}
	if filepath.Dir(claimed[0]) != filepath.Dir(claimed[1]) {
		t.Fatalf("same-token artifacts moved to different dirs: %v", claimed)
	}
	if _, ok := appStagingDir(claimed[0]); ok {
		t.Error("abandon/TTL must not recognize an inflight artifact")
	}
	for _, path := range claimed {
		if _, err := os.Stat(path); err != nil {
			t.Fatalf("claimed artifact missing at %s: %v", path, err)
		}
	}

	cleanupOwnedAppStaging(claimed...)
	if _, err := os.Stat(filepath.Dir(claimed[0])); !os.IsNotExist(err) {
		t.Fatalf("task cleanup must remove inflight dir, stat err=%v", err)
	}
}

func TestClaimAppStagingLeavesExternalCLIPathsOwnedByCaller(t *testing.T) {
	oldRoot := constants.RootPath()
	root := t.TempDir()
	constants.SetRootPath(root)
	t.Cleanup(func() { constants.SetRootPath(oldRoot) })

	external := filepath.Join(t.TempDir(), "app.yaml")
	if err := os.WriteFile(external, []byte("manifest"), 0o600); err != nil {
		t.Fatal(err)
	}
	claimed, err := claimAppStaging("task-2", external, "")
	if err != nil {
		t.Fatal(err)
	}
	if claimed[0] != external {
		t.Fatalf("external path changed: got %q want %q", claimed[0], external)
	}
	cleanupOwnedAppStaging(claimed...)
	if _, err := os.Stat(external); err != nil {
		t.Fatalf("task cleanup deleted caller-owned path: %v", err)
	}
}
