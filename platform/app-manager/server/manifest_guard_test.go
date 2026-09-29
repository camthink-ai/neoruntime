package server

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"aipc/platform/common/constants"
)

// withRoot points the shared root-path constant at root for the duration of
// the test. The manifest guard derives every path from it, so tests must own
// it; never run these with t.Parallel() — the override is process-global.
func withRoot(t *testing.T, root string) {
	t.Helper()
	orig := constants.RootPath()
	constants.SetRootPath(root)
	t.Cleanup(func() { constants.SetRootPath(orig) })
}

func TestManagedManifestDir(t *testing.T) {
	root := t.TempDir()
	withRoot(t, root)
	canonical := filepath.Join(root, "apps", "manifests", "api-tour", "app.yaml")

	tests := []struct {
		name   string
		path   string
		appID  string
		wantOK bool
	}{
		{"canonical per-app layout", canonical, "api-tour", true},
		{"manifest at install-root top level (incident shape)", filepath.Join(root, "api-tour-app.yaml"), "api-tour", false},
		{"manifest directly in install root", filepath.Join(root, "app.yaml"), "aipc", false},
		{"manifest directly in manifests root", filepath.Join(root, "apps", "manifests", "app.yaml"), "manifests", false},
		{"unpacked tarball in tmp", "/tmp/shelf-ops-0.3.0-arm64/app.yaml", "shelf-ops", false},
		{"another app's directory", canonical, "other-app", false},
		{"app id with traversal", filepath.Join(root, "apps", "manifests", "..", "etc", "app.yaml"), "../..", false},
		{"app id with slash", canonical, "api/tour", false},
		{"empty path", "", "api-tour", false},
		{"empty app id", canonical, "", false},
		{"relative path", "apps/manifests/api-tour/app.yaml", "api-tour", false},
		{"system config dir from stale config", "/etc/aipc/apps/api-tour/app.yaml", "api-tour", false},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			dir, ok := managedManifestDir(tt.path, tt.appID)
			if ok != tt.wantOK {
				t.Fatalf("managedManifestDir(%q, %q) ok = %v, want %v (dir=%q)",
					tt.path, tt.appID, ok, tt.wantOK, dir)
			}
			if ok {
				wantDir := filepath.Join(root, "apps", "manifests", tt.appID)
				if dir != wantDir {
					t.Fatalf("got dir %q, want %q", dir, wantDir)
				}
			}
		})
	}
}

func TestCanonicalizeManifest(t *testing.T) {
	t.Run("copies foreign manifest into managed root verbatim", func(t *testing.T) {
		root := t.TempDir()
		withRoot(t, root)

		srcDir := t.TempDir() // simulates an unpacked tarball outside the install root
		src := filepath.Join(srcDir, "shelf-ops.yaml")
		original := "# platform comment\napiVersion: v1\nkind: Application\n"
		if err := os.WriteFile(src, []byte(original), 0644); err != nil {
			t.Fatal(err)
		}

		got, err := canonicalizeManifest(src, "shelf-ops")
		if err != nil {
			t.Fatalf("canonicalizeManifest: %v", err)
		}
		want := filepath.Join(root, "apps", "manifests", "shelf-ops", "app.yaml")
		if got != want {
			t.Fatalf("got path %q, want %q", got, want)
		}
		data, err := os.ReadFile(got)
		if err != nil {
			t.Fatal(err)
		}
		if string(data) != original {
			t.Fatalf("canonical copy altered bytes:\n got %q\nwant %q", data, original)
		}
		// The caller-owned source file stays where it is.
		if _, err := os.Stat(src); err != nil {
			t.Fatalf("source file should be left in place: %v", err)
		}
	})

	t.Run("canonical path is a no-op", func(t *testing.T) {
		root := t.TempDir()
		withRoot(t, root)

		canonical := filepath.Join(root, "apps", "manifests", "api-tour", "app.yaml")
		if err := os.MkdirAll(filepath.Dir(canonical), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(canonical, []byte("keep"), 0644); err != nil {
			t.Fatal(err)
		}

		got, err := canonicalizeManifest(canonical, "api-tour")
		if err != nil {
			t.Fatalf("canonicalizeManifest: %v", err)
		}
		if got != canonical {
			t.Fatalf("got path %q, want %q", got, canonical)
		}
		data, _ := os.ReadFile(canonical)
		if string(data) != "keep" {
			t.Fatalf("no-op path must not rewrite the file, got %q", data)
		}
	})

	t.Run("reinstall overwrites stale canonical copy", func(t *testing.T) {
		root := t.TempDir()
		withRoot(t, root)

		canonical := filepath.Join(root, "apps", "manifests", "shelf-ops", "app.yaml")
		if err := os.MkdirAll(filepath.Dir(canonical), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(canonical, []byte("old"), 0644); err != nil {
			t.Fatal(err)
		}
		src := filepath.Join(t.TempDir(), "new.yaml")
		if err := os.WriteFile(src, []byte("new"), 0644); err != nil {
			t.Fatal(err)
		}

		if _, err := canonicalizeManifest(src, "shelf-ops"); err != nil {
			t.Fatalf("canonicalizeManifest: %v", err)
		}
		data, _ := os.ReadFile(canonical)
		if string(data) != "new" {
			t.Fatalf("canonical copy should hold the new bytes, got %q", data)
		}
	})

	t.Run("rejects unsafe app ids", func(t *testing.T) {
		withRoot(t, t.TempDir())
		src := filepath.Join(t.TempDir(), "app.yaml")
		if err := os.WriteFile(src, []byte("x"), 0644); err != nil {
			t.Fatal(err)
		}
		for _, id := range []string{"", "../etc", "a/b", ".hidden", "-dash", "sp ace"} {
			if _, err := canonicalizeManifest(src, id); err == nil {
				t.Fatalf("app id %q should be rejected", id)
			}
		}
	})

	t.Run("missing source is an error", func(t *testing.T) {
		withRoot(t, t.TempDir())
		if _, err := canonicalizeManifest(filepath.Join(t.TempDir(), "gone.yaml"), "api-tour"); err == nil {
			t.Fatal("missing source should be an error")
		}
	})
}

func TestManifestPublishRollback(t *testing.T) {
	t.Run("restores previous canonical bytes", func(t *testing.T) {
		root := t.TempDir()
		withRoot(t, root)
		canonical := filepath.Join(root, "apps", "manifests", "app", "app.yaml")
		if err := os.MkdirAll(filepath.Dir(canonical), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(canonical, []byte("old"), 0644); err != nil {
			t.Fatal(err)
		}
		rollback, err := manifestPublishRollback("app")
		if err != nil {
			t.Fatal(err)
		}
		src := filepath.Join(t.TempDir(), "app.yaml")
		_ = os.WriteFile(src, []byte("new"), 0644)
		if _, err := canonicalizeManifest(src, "app"); err != nil {
			t.Fatal(err)
		}
		rollback()
		got, _ := os.ReadFile(canonical)
		if string(got) != "old" {
			t.Fatalf("rollback restored %q", got)
		}
	})

	t.Run("removes newly published canonical", func(t *testing.T) {
		root := t.TempDir()
		withRoot(t, root)
		rollback, err := manifestPublishRollback("app")
		if err != nil {
			t.Fatal(err)
		}
		src := filepath.Join(t.TempDir(), "app.yaml")
		_ = os.WriteFile(src, []byte("new"), 0644)
		canonical, err := canonicalizeManifest(src, "app")
		if err != nil {
			t.Fatal(err)
		}
		rollback()
		if _, err := os.Stat(canonical); !os.IsNotExist(err) {
			t.Fatalf("new canonical survived rollback: %v", err)
		}
	})
}

func TestUnloadModelsRefusesUnsafeAppID(t *testing.T) {
	// Uninstall removes appModelsDir(appID) wholesale; an id like ".." would
	// resolve to the data root's parent. Install can never create such an
	// app (extraction rejects the id first), so the only correct behavior on
	// encountering one is to refuse the removal.
	root := t.TempDir()
	withRoot(t, root)
	sentinel := filepath.Join(root, "do-not-delete")
	if err := os.WriteFile(sentinel, []byte("data"), 0644); err != nil {
		t.Fatal(err)
	}

	s := &AppManagerServer{}
	s.UnloadModels(context.Background(), "..", "/gone/app.yaml")

	if _, err := os.Stat(sentinel); err != nil {
		t.Fatalf("sentinel under the data root must survive: %v", err)
	}
}
