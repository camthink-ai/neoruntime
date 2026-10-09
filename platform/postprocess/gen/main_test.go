package main

import (
	"os"
	"path/filepath"
	"testing"
)

// The -check drift guard must pass on freshly written artifacts and fail
// on any divergence — that asymmetry is the whole contract (a check that
// cannot fail would let hand edits and stale regens through CI).
func TestCheckArtifactsDetectsDrift(t *testing.T) {
	dir := t.TempDir()
	p := artifactFiles{
		json:   filepath.Join(dir, "postprocess_schema.json"),
		header: filepath.Join(dir, "postprocess_schema.h"),
	}

	if err := writeArtifacts(p); err != nil {
		t.Fatalf("writeArtifacts: %v", err)
	}
	if err := checkArtifacts(p); err != nil {
		t.Fatalf("checkArtifacts on fresh artifacts = %v, want nil", err)
	}

	// Corrupt each artifact in turn — both must trip the check.
	for name, path := range map[string]string{"json": p.json, "header": p.header} {
		if err := os.WriteFile(path, []byte("tampered\n"), 0o644); err != nil {
			t.Fatalf("tamper %s: %v", name, err)
		}
		if err := checkArtifacts(p); err == nil {
			t.Fatalf("checkArtifacts after tampering %s = nil, want drift error", name)
		}
		if err := writeArtifacts(p); err != nil {
			t.Fatalf("restore %s: %v", name, err)
		}
	}

	// A missing artifact is a drift too (with its run-make hint).
	if err := os.Remove(p.json); err != nil {
		t.Fatalf("remove json: %v", err)
	}
	if err := checkArtifacts(p); err == nil {
		t.Fatal("checkArtifacts with missing json = nil, want error")
	}
}
