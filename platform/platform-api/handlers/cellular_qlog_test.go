package handlers

import (
	"os"
	"path/filepath"
	"testing"
)

func TestFindDMPortPrefersNonAT(t *testing.T) {
	got := findDMPort([]string{"/dev/ttyUSB2"})
	if got != "" && got == "/dev/ttyUSB2" {
		t.Fatalf("should prefer non-AT port, got %q", got)
	}
}

func TestQLogDirStats(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "a.bin"), []byte("hello"), 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(dir, "b.bin"), []byte("world!"), 0o644); err != nil {
		t.Fatal(err)
	}
	files, bytes := qlogDirStats(dir)
	if files != 2 || bytes != 11 {
		t.Fatalf("files=%d bytes=%d", files, bytes)
	}
}

func TestWriteQLogTarGz(t *testing.T) {
	dir := t.TempDir()
	if err := os.WriteFile(filepath.Join(dir, "cap.qmdl"), []byte("qlog"), 0o644); err != nil {
		t.Fatal(err)
	}
	out := filepath.Join(t.TempDir(), "out.tar.gz")
	f, err := os.Create(out)
	if err != nil {
		t.Fatal(err)
	}
	n, err := writeQLogTarGz(f, dir)
	_ = f.Close()
	if err != nil || n != 1 {
		t.Fatalf("n=%d err=%v", n, err)
	}
	st, err := os.Stat(out)
	if err != nil || st.Size() == 0 {
		t.Fatalf("empty archive: %v", err)
	}
}
