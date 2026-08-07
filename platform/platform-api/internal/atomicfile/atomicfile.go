// Package atomicfile provides atomic file writes via a tmp-file + fsync + rename
// sequence, so readers never observe a partially-written config file.
//
// Extracted from the inline patterns in handlers/network.go (tmp + WriteFile +
// Rename) and storage/storage.go (CreateTemp + Rename). Centralizing it here
// gives every config adapter the same durability guarantees and lets the
// Config Manager swap a file behind a running service without a torn-write
// window.
package atomicfile

import (
	"errors"
	"fmt"
	"io"
	"os"
	"path/filepath"
)

// Write writes data to path atomically: it writes to a tmp file in the same
// directory, fsyncs the file, then renames it over the target. Because the
// rename is intra-directory and atomic on POSIX filesystems, any process
// opening path sees either the old or the new contents, never a partial write.
//
// perm is applied to the tmp file before rename; an existing target's mode is
// replaced (rename swaps the inode). If the parent directory does not exist,
// Write returns an error — callers must ensure the directory exists (most
// config paths live under /data/aipc/etc, created at install time).
func Write(path string, data []byte, perm os.FileMode) error {
	if path == "" {
		return errors.New("atomicfile: empty path")
	}

	dir := filepath.Dir(path)
	if dir == "" {
		return errors.New("atomicfile: empty parent directory")
	}

	// CreateTemp never reuses an existing name, so concurrent writers do not
	// clobber each other's tmp files. The tmp file lives in the same dir as
	// the target so Rename is a single-filesystem operation.
	tmp, err := os.CreateTemp(dir, ".atomic-*")
	if err != nil {
		return fmt.Errorf("atomicfile: create tmp in %s: %w", dir, err)
	}
	tmpPath := tmp.Name()
	cleaned := false
	cleanup := func() {
		if !cleaned {
			cleaned = true
			_ = os.Remove(tmpPath)
		}
	}
	// If we return anywhere below, remove the tmp file so the dir does not
	// accumulate .atomic-* litter on failure.
	defer func() { cleanup() }()

	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return fmt.Errorf("atomicfile: write tmp %s: %w", tmpPath, err)
	}
	if err := tmp.Chmod(perm); err != nil {
		tmp.Close()
		return fmt.Errorf("atomicfile: chmod tmp %s: %w", tmpPath, err)
	}
	// fsync the file contents before the rename so the data is durable even
	// if the box loses power between the rename and a later fsync of the
	// directory. Skipping this is the classic "rename succeeded but contents
	// are empty after crash" footgun.
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return fmt.Errorf("atomicfile: fsync tmp %s: %w", tmpPath, err)
	}
	if err := tmp.Close(); err != nil {
		return fmt.Errorf("atomicfile: close tmp %s: %w", tmpPath, err)
	}

	// Rename over the target. On POSIX this is atomic when src and dst are on
	// the same filesystem (guaranteed above, since both are in dir).
	if err := os.Rename(tmpPath, path); err != nil {
		return fmt.Errorf("atomicfile: rename %s -> %s: %w", tmpPath, path, err)
	}
	// Rename succeeded — the tmp inode is now the target inode; do NOT remove
	// it (cleanup would delete the just-written file).
	cleaned = true

	// Best-effort fsync of the parent dir so the rename itself is durable.
	// Failures here are not fatal to the write — the data is on disk, only the
	// directory entry may lag a crash. Ignore the error (some filesystems and
	// tmpfs do not support dir fsync).
	if d, err := os.Open(dir); err == nil {
		_ = d.Sync()
		_ = d.Close()
	}
	return nil
}

// WriteString is a convenience wrapper around Write for string payloads.
func WriteString(path string, s string, perm os.FileMode) error {
	return Write(path, []byte(s), perm)
}

// Read is a small helper that pairs with Write. It returns the raw bytes of
// path, or an error wrapping fs.ErrNotExist so callers can distinguish
// "no config yet" from "corrupt config".
func Read(path string) ([]byte, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("atomicfile: read %s: %w", path, err)
	}
	return b, nil
}

// Ensure the io import stays referenced even if future edits drop the only
// use (keeps gofmt/lint stable during incremental migration).
var _ = io.Discard
