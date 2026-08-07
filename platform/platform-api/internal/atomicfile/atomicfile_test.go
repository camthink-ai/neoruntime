package atomicfile

import (
	"errors"
	"io/fs"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"testing"
)

// atomicLeftovers counts .atomic-* tmp files left in dir; asserts cleanup on
// both success and failure paths.
func atomicLeftovers(t *testing.T, dir string) int {
	t.Helper()
	n := 0
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatalf("readdir %s: %v", dir, err)
	}
	for _, e := range entries {
		if strings.HasPrefix(e.Name(), ".atomic-") {
			n++
		}
	}
	return n
}

func TestWrite_CreatesFileWithContentAndPerm(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "cfg.json")

	if err := Write(path, []byte(`{"a":1}`), 0o600); err != nil {
		t.Fatalf("Write: %v", err)
	}

	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	if string(got) != `{"a":1}` {
		t.Fatalf("content = %q, want %q", got, `{"a":1}`)
	}
	fi, err := os.Stat(path)
	if err != nil {
		t.Fatalf("Stat: %v", err)
	}
	if runtime.GOOS != "windows" && fi.Mode().Perm() != 0o600 {
		t.Fatalf("perm = %o, want 0o600", fi.Mode().Perm())
	}
	if n := atomicLeftovers(t, dir); n != 0 {
		t.Fatalf("leftover tmp files = %d, want 0", n)
	}
}

func TestWrite_OverwritesExistingFile(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "cfg.json")

	if err := Write(path, []byte("old"), 0o644); err != nil {
		t.Fatalf("first Write: %v", err)
	}
	if err := Write(path, []byte("new contents"), 0o644); err != nil {
		t.Fatalf("second Write: %v", err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	if string(got) != "new contents" {
		t.Fatalf("content = %q, want %q", got, "new contents")
	}
}

// TestWrite_NoPartialVisible asserts the atomic guarantee: after a write the
// file is either absent or fully present — never an empty/partial stub.
func TestWrite_NoPartialVisible(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "cfg.json")
	payload := strings.Repeat("x", 4096)

	if err := Write(path, []byte(payload), 0o644); err != nil {
		t.Fatalf("Write: %v", err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	if string(got) != payload {
		t.Fatalf("read shorter than written: got %d bytes, want %d", len(got), len(payload))
	}
}

func TestWrite_MissingParentDir(t *testing.T) {
	dir := t.TempDir()
	missing := filepath.Join(dir, "does", "not", "exist", "cfg.json")
	err := Write(missing, []byte("x"), 0o644)
	if err == nil {
		t.Fatal("Write succeeded into a non-existent dir; want error")
	}
	if n := atomicLeftovers(t, dir); n != 0 {
		t.Fatalf("leftover tmp files on failure = %d, want 0", n)
	}
}

// TestWrite_TargetIsDirectory forces os.Rename to fail (renaming a file over
// a non-empty directory is rejected) to exercise the rename-error branch and
// the tmp-cleanup-on-failure path.
func TestWrite_TargetIsDirectory(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "cfg.json")
	// Pre-create the target as a non-empty directory.
	if err := os.Mkdir(path, 0o755); err != nil {
		t.Fatalf("Mkdir: %v", err)
	}
	if err := os.WriteFile(filepath.Join(path, "blocker"), []byte("x"), 0o644); err != nil {
		t.Fatalf("seed blocker: %v", err)
	}
	err := Write(path, []byte("x"), 0o644)
	if err == nil {
		t.Fatal("Write over a non-empty dir succeeded; want rename error")
	}
	if n := atomicLeftovers(t, dir); n != 0 {
		t.Fatalf("leftover tmp files on rename failure = %d, want 0", n)
	}
}

// TestWrite_ReadOnlyParentDir makes CreateTemp fail (cannot create in a
// read-only directory), covering the create-tmp error branch.
func TestWrite_ReadOnlyParentDir(t *testing.T) {
	if os.Geteuid() == 0 {
		t.Skip("running as root; read-only dir does not block root writes")
	}
	dir := t.TempDir()
	path := filepath.Join(dir, "cfg.json")
	// Remove write bit from the parent so CreateTemp cannot create a file.
	if err := os.Chmod(dir, 0o500); err != nil {
		t.Fatalf("Chmod: %v", err)
	}
	defer os.Chmod(dir, 0o755) // restore so t.TempDir cleanup can remove it

	err := Write(path, []byte("x"), 0o644)
	if err == nil {
		t.Fatal("Write succeeded in a read-only dir; want error")
	}
}

func TestWrite_EmptyPath(t *testing.T) {
	if err := Write("", []byte("x"), 0o644); err == nil {
		t.Fatal("Write(\"\") succeeded; want error")
	}
}

func TestRead_MissingFileWrapsNotExist(t *testing.T) {
	dir := t.TempDir()
	_, err := Read(filepath.Join(dir, "absent.json"))
	if err == nil {
		t.Fatal("Read absent file succeeded; want error")
	}
	if !errors.Is(err, fs.ErrNotExist) {
		t.Fatalf("err = %v, want wrap of fs.ErrNotExist", err)
	}
}

func TestRead_ReturnsBytes(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "cfg.json")
	if err := Write(path, []byte("hello"), 0o644); err != nil {
		t.Fatalf("Write: %v", err)
	}
	got, err := Read(path)
	if err != nil {
		t.Fatalf("Read: %v", err)
	}
	if string(got) != "hello" {
		t.Fatalf("got %q, want %q", got, "hello")
	}
}

func TestWriteString(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "s.txt")
	if err := WriteString(path, "plain", 0o644); err != nil {
		t.Fatalf("WriteString: %v", err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	if string(got) != "plain" {
		t.Fatalf("got %q, want %q", got, "plain")
	}
}
