package handlers

// Unit tests for the OSD-aware media-config bundle (media_config_bundle.go).
//
// The bundle ships the media-config envelope plus the OSD overlay image
// binaries as a self-describing tar.gz. These tests pin the pieces that
// clone_test.go does not cover for the media side: the upload suffix gate, the
// OSD destination path guard, the OSD-image collector, bundle assembly +
// manifest integrity, tamper detection, and the bundle-vs-clone schema gate.

import (
	"archive/tar"
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"testing"
)

// TestMediaBundleSuffix pins the upload-suffix gate on import.
func TestMediaBundleSuffix(t *testing.T) {
	for _, n := range []string{"a.tar.gz", "b.tgz", "C.TAR.GZ", "X.TGZ"} {
		if !validBundleSuffix(n) {
			t.Errorf("validBundleSuffix(%q) = false, want true", n)
		}
	}
	for _, n := range []string{"", "a.zip", "a.tar", "a.json", "a.tar.gz.exe", "tgz."} {
		if validBundleSuffix(n) {
			t.Errorf("validBundleSuffix(%q) = true, want false", n)
		}
	}
}

// TestMediaOsdDestPath pins the import-side OSD destination guard: a
// destination must be .png/.bmp and confined under the osd dir. Unlike
// validOsdImagePath it does NOT require the file to exist (dest is created).
func TestMediaOsdDestPath(t *testing.T) {
	dir := "/data/aipc/etc/osd"
	good := []string{
		dir + "/logo.png",
		dir + "/title.bmp",
		dir + "/sub/deeper.PNG", // validOsdDestPath allows nested; import handler rejects subdir separately
	}
	for _, p := range good {
		if !validOsdDestPath(p, dir) {
			t.Errorf("validOsdDestPath(%q) = false, want true", p)
		}
	}
	bad := []string{
		"/etc/passwd",                   // outside osd dir
		dir,                             // the dir itself
		dir + "/x.jpg",                  // wrong ext
		dir + "/noext",                  // no ext
		"/data/aipc/etc/osd_evil/x.png", // sibling dir, not under
		dir + "/../secret.png",          // traversal (Clean collapses)
	}
	for _, p := range bad {
		if validOsdDestPath(p, dir) {
			t.Errorf("validOsdDestPath(%q) = true, want false", p)
		}
	}
}

// TestMediaCollectOsdImages covers the parse + filter logic. Paths that do not
// exist on disk (or have the wrong extension, or escape the osd dir) are
// dropped; a malformed or empty document yields nil. The existence check is
// exercised implicitly: none of these paths exist in the test environment.
func TestMediaCollectOsdImages(t *testing.T) {
	if got := collectOsdImages(nil); got != nil {
		t.Errorf("collectOsdImages(nil) = %v, want nil", got)
	}
	if got := collectOsdImages(json.RawMessage(`{not json`)); got != nil {
		t.Errorf("collectOsdImages(malformed) = %v, want nil", got)
	}
	// Structurally valid, but every referenced path is absent / wrong-ext /
	// out-of-dir → collectOsdImages returns nothing.
	osd := json.RawMessage(`{"streams":[
		{"image_overlays":[
			{"image_path":"/data/aipc/etc/osd/missing.png"},
			{"image_path":"/etc/passwd"},
			{"image_path":"/data/aipc/etc/osd/notimage.txt"},
			{"image_path":""}
		]}
	]}`)
	if got := collectOsdImages(osd); len(got) != 0 {
		t.Errorf("collectOsdImages with no usable paths = %v, want empty", got)
	}
	// No image_overlays → nothing collected (not an error).
	if got := collectOsdImages(json.RawMessage(`{"streams":[{"image_overlays":[]}]}`)); len(got) != 0 {
		t.Errorf("collectOsdImages with empty overlays = %v, want empty", got)
	}
}

// TestMediaBundleWriteAndVerify builds a real bundle from a synthetic envelope
// + two temp OSD images, then untars + verifies every manifest sha256. This is
// the media-side counterpart of clone's TestCloneExportExcludesIdentity.
func TestMediaBundleWriteAndVerify(t *testing.T) {
	osdDir := t.TempDir()
	imgPNG := filepath.Join(osdDir, "logo.png")
	imgBMP := filepath.Join(osdDir, "title.bmp")
	if err := os.WriteFile(imgPNG, []byte("\x89PNG\r\n\x1a\n fake png"), 0644); err != nil {
		t.Fatalf("write png: %v", err)
	}
	if err := os.WriteFile(imgBMP, []byte("BM fake bmp"), 0644); err != nil {
		t.Fatalf("write bmp: %v", err)
	}

	env := mediaConfigEnvelope{
		Schema:  mediaConfigSchema,
		Version: mediaConfigVersion,
		Config:  mediaConfigPayload{Osd: json.RawMessage(`{"streams":[]}`)},
	}
	out := filepath.Join(t.TempDir(), "media.tar.gz")
	manifest := &configManifest{Schema: mediaBundleSchema, Version: mediaBundleVersion}
	if err := writeBundle(out, env, []string{imgPNG, imgBMP}, manifest); err != nil {
		t.Fatalf("writeBundle: %v", err)
	}

	// The manifest must declare the bundle schema and carry envelope + both
	// OSD images (manifest.json itself is written but not listed in Files).
	if manifest.Schema != mediaBundleSchema {
		t.Errorf("manifest schema = %q, want %q", manifest.Schema, mediaBundleSchema)
	}
	listed := map[string]bool{}
	for _, e := range manifest.Files {
		listed[e.Path] = true
	}
	for _, want := range []string{"envelope.json", "osd/logo.png", "osd/title.bmp"} {
		if !listed[want] {
			t.Errorf("manifest missing entry %q (got %v)", want, bundleSortedKeys(listed))
		}
	}

	// Untar + verify: every listed sha256 must match the extracted bytes.
	f, err := os.Open(out)
	if err != nil {
		t.Fatalf("open bundle: %v", err)
	}
	manifestJSON, members, err := untar(f, t.TempDir())
	f.Close()
	if err != nil {
		t.Fatalf("untar: %v", err)
	}
	m, err := decodeManifest(manifestJSON)
	if err != nil {
		t.Fatalf("decode manifest: %v", err)
	}
	if _, err := verifyManifest(m, members); err != nil {
		t.Fatalf("verifyManifest failed on a freshly built bundle: %v", err)
	}
	// The envelope inside the tar carries the media schema (not the bundle
	// schema) — the bundle wraps the media envelope. members maps arcName →
	// extracted abs path, so read the file back from disk.
	envPath, ok := members["envelope.json"]
	if !ok {
		t.Fatal("envelope.json absent from extracted bundle")
	}
	envBytes, err := os.ReadFile(envPath)
	if err != nil {
		t.Fatalf("read extracted envelope.json: %v", err)
	}
	var inner mediaConfigEnvelope
	if err := json.Unmarshal(envBytes, &inner); err != nil {
		t.Fatalf("envelope.json unmarshal: %v", err)
	}
	if inner.Schema != mediaConfigSchema {
		t.Errorf("inner envelope schema = %q, want %q", inner.Schema, mediaConfigSchema)
	}
}

// TestMediaBundleTamperDetection flips one manifest sha256 and expects
// verifyManifest to reject the bundle — the integrity gate on the media side.
func TestMediaBundleTamperDetection(t *testing.T) {
	osdDir := t.TempDir()
	img := filepath.Join(osdDir, "logo.png")
	if err := os.WriteFile(img, []byte("png"), 0644); err != nil {
		t.Fatalf("write png: %v", err)
	}

	env := mediaConfigEnvelope{Schema: mediaConfigSchema, Version: mediaConfigVersion, Config: mediaConfigPayload{Osd: json.RawMessage(`{}`)}}
	out := filepath.Join(t.TempDir(), "media.tar.gz")
	manifest := &configManifest{Schema: mediaBundleSchema, Version: mediaBundleVersion}
	if err := writeBundle(out, env, []string{img}, manifest); err != nil {
		t.Fatalf("writeBundle: %v", err)
	}

	f, err := os.Open(out)
	if err != nil {
		t.Fatalf("open bundle: %v", err)
	}
	manifestJSON, members, err := untar(f, t.TempDir())
	f.Close()
	if err != nil {
		t.Fatalf("untar: %v", err)
	}
	m, err := decodeManifest(manifestJSON)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	for i := range m.Files {
		if m.Files[i].Path == "envelope.json" {
			m.Files[i].SHA256 = "0000000000000000000000000000000000000000000000000000000000000000"
		}
	}
	if _, err := verifyManifest(m, members); err == nil {
		t.Error("verifyManifest accepted a tampered sha256 (integrity gate broken)")
	}
}

// TestMediaBundleRejectsCloneSchema mirrors ImportMediaBundle's schema gate: a
// clone-schema manifest must be distinguishable from a media-bundle schema, so
// a mis-uploaded device clone is rejected by the bundle import.
func TestMediaBundleRejectsCloneSchema(t *testing.T) {
	var buf bytes.Buffer
	if err := writeTarGz(&buf, func(tw *tar.Writer) error {
		mb, _ := json.MarshalIndent(&configManifest{Schema: deviceCloneSchema, Version: deviceCloneVersion}, "", "  ")
		return tarWriteBytes(tw, "manifest.json", mb, 0644)
	}); err != nil {
		t.Fatalf("writeTarGz: %v", err)
	}
	manifestJSON, _, err := untar(&buf, t.TempDir())
	if err != nil {
		t.Fatalf("untar: %v", err)
	}
	m, err := decodeManifest(manifestJSON)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if m.Schema != deviceCloneSchema {
		t.Fatalf("precondition: manifest schema = %q, want %q", m.Schema, deviceCloneSchema)
	}
	// Mirrors ImportMediaBundle's check.
	if m.Schema == mediaBundleSchema {
		t.Fatal("clone schema must not equal media bundle schema")
	}
	t.Log("media bundle gate correctly distinguishes clone schema:", m.Schema)
}

// bundleSortedKeys is a small helper for readable failure output.
func bundleSortedKeys(m map[string]bool) []string {
	keys := make([]string, 0, len(m))
	for k := range m {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	return keys
}
