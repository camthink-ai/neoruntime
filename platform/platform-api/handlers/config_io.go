package handlers

// Shared config-bundle helpers used by both the media OSD bundle
// (media_config_bundle.go) and the device-scope clone (device_config.go).
//
// Both features move a set of config files (and, for device scope, DB state)
// between devices as a single tar.gz. The transport mechanics — streaming a
// tree into a tar writer with a sha256 manifest, extracting one back safely,
// snapshotting a tree for rollback — are identical, so they live here once.
//
// Scope-specific concerns (which files to include, identity policy, DB tables)
// stay in their own files; this file only knows about bytes, trees, and tars.

import (
	"archive/tar"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"

	"aipc/platform/platform-api/internal/atomicfile"
)

// manifestEntry describes one file inside a config bundle tar.
type manifestEntry struct {
	Path   string `json:"path"` // archive-relative path, e.g. "etc/camera-daemon.yaml"
	SHA256 string `json:"sha256"`
	Size   int64  `json:"size"`
	Mode   uint32 `json:"mode,omitempty"` // octal file mode to restore on import
}

// configManifest is the self-describing index written at <root>/manifest.json.
// Extra carries scope-specific payload (e.g. {"tables":[...]} for device scope).
type configManifest struct {
	Schema    string            `json:"schema"`
	Version   int               `json:"version"`
	CreatedAt string            `json:"created_at"`
	Source    map[string]string `json:"source,omitempty"`
	Files     []manifestEntry   `json:"files"`
	Extra     map[string]any    `json:"extra,omitempty"`
}

// configApplyMu serializes every path that writes /data/aipc/etc config files
// (and then restarts the services that consume them). Three apply paths touch
// that tree and must not interleave:
//
//   - per-field media edits (media.go's 8 SetConfig/stream helpers) → hold
//     h.configMu, then projectMediaConfig acquires this lock;
//   - media/bundle import (applyImportedMediaConfig) → holds h.configMu, then
//     acquires this lock, then calls projectMediaConfigLocked (NOT the locking
//     wrapper — it already holds this lock, and sync.Mutex is not reentrant);
//   - device-clone file apply (ImportDeviceConfig → applyTree here) → acquires
//     this lock only (clone lives on APIHandlers, which has no configMu).
//
// Lock order is therefore configMu → configApplyMu on the media side and
// configApplyMu alone on the clone side, so the two never form a cycle. Without
// this lock, a clone restore could overwrite the etc tree mid-edit while a
// concurrent web media write is in flight (or vice versa) — the P0 race.
var configApplyMu sync.Mutex

// sha256Sum returns the hex-encoded sha256 of data.
func sha256Sum(data []byte) string {
	sum := sha256.Sum256(data)
	return hex.EncodeToString(sum[:])
}

// walkFiles returns the archive-relative file paths under root, sorted and
// slash-cleaned ("etc/osd/logo.png", not "etc/osd/../osd/x"). Symlinks are
// skipped (config files are regular; following links into /etc risks cycles).
func walkFiles(root string) ([]string, error) {
	var rels []string
	err := filepath.Walk(root, func(path string, info os.FileInfo, err error) error {
		if err != nil {
			return err
		}
		if info.IsDir() {
			return nil
		}
		if info.Mode()&os.ModeSymlink != 0 {
			return nil
		}
		rel, err := filepath.Rel(root, path)
		if err != nil {
			return err
		}
		rels = append(rels, filepath.ToSlash(rel))
		return nil
	})
	if err != nil {
		return nil, err
	}
	sort.Strings(rels)
	return rels, nil
}

// tarWriteFile writes the regular file at absPath into tw under arcName and
// returns its manifest entry (sha256 + size + mode). arcName must be slash-clean.
func tarWriteFile(tw *tar.Writer, absPath, arcName string) (manifestEntry, error) {
	data, err := os.ReadFile(absPath)
	if err != nil {
		return manifestEntry{}, fmt.Errorf("read %s: %w", absPath, err)
	}
	info, err := os.Stat(absPath)
	if err != nil {
		return manifestEntry{}, fmt.Errorf("stat %s: %w", absPath, err)
	}
	mode := uint32(0644)
	if m := info.Mode().Perm(); m != 0 {
		mode = uint32(m)
	}
	if err := tarWriteBytes(tw, arcName, data, mode); err != nil {
		return manifestEntry{}, err
	}
	return manifestEntry{
		Path:   arcName,
		SHA256: sha256Sum(data),
		Size:   int64(len(data)),
		Mode:   mode,
	}, nil
}

// tarWriteBytes writes raw bytes into tw under arcName with the given mode.
func tarWriteBytes(tw *tar.Writer, arcName string, data []byte, mode uint32) error {
	hdr := &tar.Header{
		Name: arcName,
		Mode: int64(mode),
		Size: int64(len(data)),
	}
	if err := tw.WriteHeader(hdr); err != nil {
		return fmt.Errorf("tar header %s: %w", arcName, err)
	}
	if _, err := tw.Write(data); err != nil {
		return fmt.Errorf("tar write %s: %w", arcName, err)
	}
	return nil
}

// writeTarGz opens a gzip+tar writer over w and hands it to fn along with a
// slice the fn appends manifest entries to. The writer chain is closed in the
// right order (tar, then gzip) so the footer is flushed.
func writeTarGz(w io.Writer, fn func(tw *tar.Writer) error) error {
	gzw := gzip.NewWriter(w)
	tw := tar.NewWriter(gzw)
	if err := fn(tw); err != nil {
		tw.Close()
		gzw.Close()
		return err
	}
	if err := tw.Close(); err != nil {
		gzw.Close()
		return fmt.Errorf("close tar: %w", err)
	}
	if err := gzw.Close(); err != nil {
		return fmt.Errorf("close gzip: %w", err)
	}
	return nil
}

// untar extracts a tar.gz from r into destDir, returning the manifest entry for
// "manifest.json" (if present) and the map of arcName→absPath for every entry.
// It rejects entries that escape destDir (Zip Slip) and entries that aren't
// regular files, so a malicious bundle cannot overwrite files outside destDir.
func untar(r io.Reader, destDir string) (manifestJSON []byte, members map[string]string, err error) {
	gzr, gzErr := gzip.NewReader(r)
	if gzErr != nil {
		return nil, nil, fmt.Errorf("gzip: %w", gzErr)
	}
	defer gzr.Close()
	tr := tar.NewReader(gzr)
	members = map[string]string{}
	cleanDest, _ := filepath.Abs(destDir)
	for {
		hdr, herr := tr.Next()
		if herr == io.EOF {
			break
		}
		if herr != nil {
			return nil, nil, fmt.Errorf("tar next: %w", herr)
		}
		// Defense against path traversal: clean and confine under destDir.
		clean := filepath.Clean(hdr.Name)
		if strings.HasPrefix(clean, "..") || filepath.IsAbs(clean) {
			return nil, nil, fmt.Errorf("unsafe archive path %q", hdr.Name)
		}
		// Skip non-regular entries (dirs/symlinks). Config trees are flat-ish;
		// MkdirAll per-file below recreates needed dirs.
		if hdr.Typeflag != tar.TypeReg && hdr.Typeflag != tar.TypeDir {
			continue
		}
		dst := filepath.Join(cleanDest, clean)
		if rel, rerr := filepath.Rel(cleanDest, dst); rerr != nil || strings.HasPrefix(rel, "..") {
			return nil, nil, fmt.Errorf("unsafe archive path %q", hdr.Name)
		}
		if hdr.Typeflag == tar.TypeDir {
			if err := os.MkdirAll(dst, 0755); err != nil {
				return nil, nil, fmt.Errorf("mkdir %s: %w", dst, err)
			}
			continue
		}
		if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
			return nil, nil, fmt.Errorf("mkdir parent %s: %w", filepath.Dir(dst), err)
		}
		data := make([]byte, hdr.Size)
		if _, err := io.ReadFull(tr, data); err != nil {
			return nil, nil, fmt.Errorf("read %s: %w", hdr.Name, err)
		}
		mode := os.FileMode(0644)
		if hdr.Mode > 0 {
			mode = os.FileMode(hdr.Mode).Perm()
		}
		if err := atomicfile.Write(dst, data, mode); err != nil {
			return nil, nil, fmt.Errorf("extract %s: %w", hdr.Name, err)
		}
		members[filepath.ToSlash(clean)] = dst
		if filepath.ToSlash(clean) == "manifest.json" {
			manifestJSON = data
		}
	}
	return manifestJSON, members, nil
}

// verifyManifest checks every entry's sha256 against the extracted files in
// members. It returns the list of verified archive paths and an error naming the
// first mismatch / missing file. Extra files present on disk (not in manifest)
// are tolerated — the manifest is the authority for what we applied.
func verifyManifest(m *configManifest, members map[string]string) ([]string, error) {
	var verified []string
	for _, e := range m.Files {
		dst, ok := members[e.Path]
		if !ok {
			return nil, fmt.Errorf("manifest lists %s but it is absent from the bundle", e.Path)
		}
		data, err := os.ReadFile(dst)
		if err != nil {
			return nil, fmt.Errorf("read back %s: %w", e.Path, err)
		}
		if sha256Sum(data) != e.SHA256 {
			return nil, fmt.Errorf("checksum mismatch for %s (bundle tampered or truncated)", e.Path)
		}
		verified = append(verified, e.Path)
	}
	return verified, nil
}

// snapshotTree recursively copies srcDir into dstDir (created if missing) for
// rollback. It copies regular files only (skips symlinks), preserving neither
// subdirectory perms (0755 used) as config dirs are uniform. Best-effort: a
// missing src file is a no-op; a copy error aborts.
func snapshotTree(srcDir, dstDir string) error {
	rels, err := walkFiles(srcDir)
	if err != nil {
		return fmt.Errorf("walk %s: %w", srcDir, err)
	}
	if err := os.MkdirAll(dstDir, 0755); err != nil {
		return fmt.Errorf("mkdir %s: %w", dstDir, err)
	}
	for _, rel := range rels {
		data, rerr := os.ReadFile(filepath.Join(srcDir, rel))
		if rerr != nil {
			continue
		}
		dst := filepath.Join(dstDir, rel)
		if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
			return fmt.Errorf("mkdir parent %s: %w", filepath.Dir(dst), err)
		}
		if err := atomicfile.Write(dst, data, 0644); err != nil {
			return fmt.Errorf("copy %s: %w", rel, err)
		}
	}
	return nil
}

// applyTree writes each verified bundle member (archive path under arcRoot)
// onto the corresponding path under liveRoot via atomicfile, preserving mode.
// skipPaths are archive-relative paths to leave untouched on the target (used by
// device-scope identity policy: ssl/rsa/device.conf). Returns the applied list.
func applyTree(members map[string]string, arcRoot, liveRoot string, skipPaths map[string]bool) ([]string, error) {
	var applied []string
	for arcName, staged := range members {
		if arcName == "manifest.json" {
			continue
		}
		if !strings.HasPrefix(arcName, arcRoot+"/") {
			continue // belongs to a different tree (e.g. state/ during device import)
		}
		rel := strings.TrimPrefix(arcName, arcRoot+"/")
		if pathSkipped(rel, skipPaths) {
			continue
		}
		data, err := os.ReadFile(staged)
		if err != nil {
			return applied, fmt.Errorf("read staged %s: %w", arcName, err)
		}
		dst := filepath.Join(liveRoot, rel)
		if err := os.MkdirAll(filepath.Dir(dst), 0755); err != nil {
			return applied, fmt.Errorf("mkdir %s: %w", filepath.Dir(dst), err)
		}
		mode := os.FileMode(0644)
		if info, err := os.Stat(staged); err == nil {
			if mm := info.Mode().Perm(); mm != 0 {
				mode = mm
			}
		}
		if err := atomicfile.Write(dst, data, mode); err != nil {
			return applied, fmt.Errorf("apply %s: %w", arcName, err)
		}
		applied = append(applied, rel)
	}
	sort.Strings(applied)
	return applied, nil
}

// decodeManifest parses manifest.json bytes into a configManifest.
func decodeManifest(data []byte) (*configManifest, error) {
	var m configManifest
	if err := json.Unmarshal(data, &m); err != nil {
		return nil, fmt.Errorf("decode manifest.json: %w", err)
	}
	return &m, nil
}

// pathSkipped reports whether rel matches an exact skip entry or falls under a
// trailing-slash prefix entry (e.g. "ssl/" skips "ssl/server.key"). Used by
// applyTree so device-scope clone can skip whole identity subtrees (ssl/, rsa/)
// plus exact identity files (platform-api.yaml, device.conf) in one mechanism.
func pathSkipped(rel string, skipPaths map[string]bool) bool {
	if skipPaths[rel] {
		return true
	}
	for p := range skipPaths {
		if strings.HasSuffix(p, "/") && strings.HasPrefix(rel, p) {
			return true
		}
	}
	return false
}
