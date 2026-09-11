package server

import (
	"bytes"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"aipc/platform/platform-api/storage"
)

// writeTestPackage builds a valid AMPK .bin at path from meta and fake HEF
// bytes — the unpack path never parses the HEF, only hashes and copies it.
func writeTestPackage(t *testing.T, path string, meta *storage.PackageMeta, hef []byte) {
	t.Helper()
	f, err := os.Create(path)
	if err != nil {
		t.Fatalf("Create(%s) error: %v", path, err)
	}
	defer f.Close()
	if err := storage.WritePackage(f, meta, bytes.NewReader(hef)); err != nil {
		t.Fatalf("WritePackage() error: %v", err)
	}
}

func testHEFBytes() []byte {
	// Distinct, longer than a header mishap would accidentally produce.
	return bytes.Repeat([]byte("HEF1-"), 40)
}

// tunedDetectionMeta is a detection package carrying config overrides, so the
// merged-config → column-lift → composed-variant chain is observable.
func tunedDetectionMeta() *storage.PackageMeta {
	return &storage.PackageMeta{
		ModelID:    "custom_det",
		ModelType:  "detection",
		OutputMode: "platform",
		Config:     json.RawMessage(`{"threshold":0.55,"max_detections":200,"labels":"car,person"}`),
		HEF:        storage.PackageHEF{Filename: "customer_model.hef"},
	}
}

func TestUnpackBundledPackageDetection(t *testing.T) {
	dir := t.TempDir()
	binPath := filepath.Join(dir, "model.bin")
	hef := testHEFBytes()
	writeTestPackage(t, binPath, tunedDetectionMeta(), hef)

	reg, err := unpackBundledPackage(binPath, dir, "custom_det")
	if err != nil {
		t.Fatalf("unpackBundledPackage() error: %v", err)
	}

	// Detection + platform mode: staged under the postprocess profile
	// basename so modelload's plugin-basename passthrough applies.
	if reg.HEF != "hailo_yolov8n_384_640.hef" {
		t.Errorf("HEF basename = %q, want default profile basename", reg.HEF)
	}
	if reg.ModelID != "custom_det" {
		t.Errorf("ModelID = %q, want custom_det", reg.ModelID)
	}
	if reg.ModelType != "detection" {
		t.Errorf("ModelType = %q, want detection", reg.ModelType)
	}

	// The merged config's threshold/max_detections must reach the composed
	// variant (column lift — the composer reads columns, not config).
	var variant map[string]interface{}
	if err := json.Unmarshal([]byte(reg.ModelVariant), &variant); err != nil {
		t.Fatalf("ModelVariant is not JSON: %v (%q)", err, reg.ModelVariant)
	}
	if v, _ := variant["detection_threshold"].(float64); v != 0.55 {
		t.Errorf("detection_threshold = %v, want 0.55", variant["detection_threshold"])
	}
	if v, _ := variant["max_boxes"].(float64); v != 200 {
		t.Errorf("max_boxes = %v, want 200", variant["max_boxes"])
	}
	if v, _ := variant["backend_function"].(string); v != "hailo_yolov8n" {
		t.Errorf("backend_function = %v, want hailo_yolov8n", variant["backend_function"])
	}
	if labels, ok := variant["labels"].([]interface{}); !ok || len(labels) != 3 || labels[0] != "unlabeled" || labels[1] != "car" {
		t.Errorf("labels = %v, want [unlabeled car person]", variant["labels"])
	}

	// HEF bytes staged verbatim at the profile basename.
	staged, err := os.ReadFile(filepath.Join(dir, reg.HEF))
	if err != nil {
		t.Fatalf("staged HEF missing: %v", err)
	}
	if !bytes.Equal(staged, hef) {
		t.Errorf("staged HEF bytes differ from package payload")
	}

	// Sidecar round-trips through loadBundledRegistration.
	loaded, err := loadBundledRegistration(dir)
	if err != nil {
		t.Fatalf("loadBundledRegistration() error: %v", err)
	}
	if *loaded != *reg {
		t.Errorf("sidecar round-trip = %+v, want %+v", loaded, reg)
	}
}

func TestUnpackBundledPackageDetectionExplicitProfile(t *testing.T) {
	// A config that pins postprocess_profile must stage the HEF under that
	// profile's basename — otherwise the plugin default gets bound and the
	// model decodes with the wrong postprocess function.
	dir := t.TempDir()
	binPath := filepath.Join(dir, "model.bin")
	meta := tunedDetectionMeta()
	meta.Config = json.RawMessage(`{"postprocess_profile":"hailo_yolov8s_384_640"}`)
	writeTestPackage(t, binPath, meta, testHEFBytes())

	reg, err := unpackBundledPackage(binPath, dir, "custom_det")
	if err != nil {
		t.Fatalf("unpackBundledPackage() error: %v", err)
	}
	if reg.HEF != "hailo_yolov8s_384_640.hef" {
		t.Errorf("HEF basename = %q, want pinned profile basename", reg.HEF)
	}
	if !strings.Contains(reg.ModelVariant, `"backend_function":"hailo_yolov8s"`) {
		t.Errorf("variant = %q, want hailo_yolov8s backend_function", reg.ModelVariant)
	}
}

func TestUnpackBundledPackageUnknownPostprocessProfile(t *testing.T) {
	// A typo'd profile in the package config must fail the install rather than
	// silently staging the HEF under the default profile's basename — the load
	// path rejects the same row, so accepting it here would let a package
	// register a model that can never load.
	dir := t.TempDir()
	binPath := filepath.Join(dir, "model.bin")
	meta := tunedDetectionMeta()
	meta.Config = json.RawMessage(`{"postprocess_profile":"yolov8x_640_640"}`)
	writeTestPackage(t, binPath, meta, testHEFBytes())

	_, err := unpackBundledPackage(binPath, dir, "custom_det")
	if err == nil || !strings.Contains(err.Error(), "postprocess_profile") {
		t.Fatalf("error = %v, want postprocess_profile rejection", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "hailo_yolov8n_384_640.hef")); !os.IsNotExist(err) {
		t.Error("rejected package must not publish a HEF at the default profile basename")
	}
}

func TestUnpackBundledPackageDigestCorrupted(t *testing.T) {
	dir := t.TempDir()
	binPath := filepath.Join(dir, "model.bin")
	writeTestPackage(t, binPath, tunedDetectionMeta(), testHEFBytes())

	// Corrupt one payload byte; the staged HEF must never reach its final
	// path and no sidecar may survive.
	blob, err := os.ReadFile(binPath)
	if err != nil {
		t.Fatalf("ReadFile() error: %v", err)
	}
	blob[len(blob)-1] ^= 0xFF
	if err := os.WriteFile(binPath, blob, 0o644); err != nil {
		t.Fatalf("WriteFile() error: %v", err)
	}

	if _, err := unpackBundledPackage(binPath, dir, "custom_det"); err == nil {
		t.Fatal("unpackBundledPackage() expected digest error, got nil")
	}
	if _, err := os.Stat(filepath.Join(dir, "hailo_yolov8n_384_640.hef")); !os.IsNotExist(err) {
		t.Error("corrupted package must not publish a HEF at its final path")
	}
	if _, err := os.Stat(filepath.Join(dir, "hailo_yolov8n_384_640.hef.tmp")); !os.IsNotExist(err) {
		t.Error("staging .tmp must be removed on verification failure")
	}
	if _, err := os.Stat(filepath.Join(dir, bundledRegistrationFile)); !os.IsNotExist(err) {
		t.Error("sidecar must not be written when verification fails")
	}
}

func TestUnpackBundledPackageUnknownModelType(t *testing.T) {
	dir := t.TempDir()
	binPath := filepath.Join(dir, "model.bin")
	meta := tunedDetectionMeta()
	meta.ModelType = "sentience"
	writeTestPackage(t, binPath, meta, testHEFBytes())

	_, err := unpackBundledPackage(binPath, dir, "custom_det")
	if err == nil || !strings.Contains(err.Error(), "unknown model_type") {
		t.Fatalf("error = %v, want unknown model_type", err)
	}
}

func TestUnpackBundledPackageBadConfigJSON(t *testing.T) {
	dir := t.TempDir()
	binPath := filepath.Join(dir, "model.bin")
	meta := tunedDetectionMeta()
	meta.Config = json.RawMessage(`[1,2,3]`) // valid JSON, not an object
	writeTestPackage(t, binPath, meta, testHEFBytes())

	_, err := unpackBundledPackage(binPath, dir, "custom_det")
	if err == nil || !strings.Contains(err.Error(), "not a JSON object") {
		t.Fatalf("error = %v, want config not-a-JSON-object", err)
	}
}

func TestUnpackBundledPackageNonDetectionKeepsFilename(t *testing.T) {
	// Non-detection packages keep the package's original .hef filename and
	// register under their semantic type with no composed variant.
	dir := t.TempDir()
	binPath := filepath.Join(dir, "model.bin")
	meta := &storage.PackageMeta{
		ModelID:   "custom_clip",
		ModelType: "clip",
		HEF:       storage.PackageHEF{Filename: "my_clip.hef"},
	}
	writeTestPackage(t, binPath, meta, testHEFBytes())

	reg, err := unpackBundledPackage(binPath, dir, "custom_clip")
	if err != nil {
		t.Fatalf("unpackBundledPackage() error: %v", err)
	}
	if reg.HEF != "my_clip.hef" {
		t.Errorf("HEF basename = %q, want my_clip.hef", reg.HEF)
	}
	if reg.ModelType != "clip" {
		t.Errorf("ModelType = %q, want clip", reg.ModelType)
	}
	if reg.ModelVariant != "" {
		t.Errorf("ModelVariant = %q, want empty", reg.ModelVariant)
	}
}

func TestUnpackBundledPackageNonDetectionBadFilename(t *testing.T) {
	// A non-.hef filename in the metadata degrades to the model id.
	dir := t.TempDir()
	binPath := filepath.Join(dir, "model.bin")
	meta := &storage.PackageMeta{
		ModelID:   "custom_clip",
		ModelType: "clip",
		HEF:       storage.PackageHEF{Filename: "weights.bin"},
	}
	writeTestPackage(t, binPath, meta, testHEFBytes())

	reg, err := unpackBundledPackage(binPath, dir, "custom_clip")
	if err != nil {
		t.Fatalf("unpackBundledPackage() error: %v", err)
	}
	if reg.HEF != "custom_clip.hef" {
		t.Errorf("HEF basename = %q, want custom_clip.hef fallback", reg.HEF)
	}
}

func TestUnpackBundledPackageRawOutputMode(t *testing.T) {
	// Raw output: no postprocess session — empty grpc type and variant, HEF
	// keeps the package filename.
	dir := t.TempDir()
	binPath := filepath.Join(dir, "model.bin")
	meta := tunedDetectionMeta()
	meta.OutputMode = "raw"
	writeTestPackage(t, binPath, meta, testHEFBytes())

	reg, err := unpackBundledPackage(binPath, dir, "custom_det")
	if err != nil {
		t.Fatalf("unpackBundledPackage() error: %v", err)
	}
	if reg.HEF != "customer_model.hef" {
		t.Errorf("HEF basename = %q, want customer_model.hef", reg.HEF)
	}
	if reg.ModelType != "" {
		t.Errorf("ModelType = %q, want empty for raw output", reg.ModelType)
	}
	if reg.ModelVariant != "" {
		t.Errorf("ModelVariant = %q, want empty for raw output", reg.ModelVariant)
	}
	// The typeless registration must carry the explicit opt-in — the runtime's
	// transient gate rejects typeless registrations without it.
	if !reg.RawOutputOnly {
		t.Error("RawOutputOnly = false, want true for a raw-output package")
	}
	// And the sidecar must round-trip it for the reboot restore path.
	loaded, err := loadBundledRegistration(dir)
	if err != nil {
		t.Fatalf("loadBundledRegistration() error: %v", err)
	}
	if !loaded.RawOutputOnly {
		t.Error("sidecar round-trip lost RawOutputOnly")
	}
}

func TestLoadBundledRegistrationRejectsBadRecords(t *testing.T) {
	t.Run("missing_file", func(t *testing.T) {
		if _, err := loadBundledRegistration(t.TempDir()); err == nil {
			t.Fatal("expected error for missing sidecar")
		}
	})

	t.Run("corrupt_json", func(t *testing.T) {
		dir := t.TempDir()
		os.WriteFile(filepath.Join(dir, bundledRegistrationFile), []byte("{nope"), 0o644)
		if _, err := loadBundledRegistration(dir); err == nil || !strings.Contains(err.Error(), "corrupt") {
			t.Fatalf("error = %v, want corrupt record", err)
		}
	})

	t.Run("path_traversal_hef", func(t *testing.T) {
		dir := t.TempDir()
		reg := bundledRegistration{ModelID: "m", HEF: "../evil.hef", ModelType: "detection"}
		blob, _ := json.Marshal(reg)
		os.WriteFile(filepath.Join(dir, bundledRegistrationFile), blob, 0o644)
		if _, err := loadBundledRegistration(dir); err == nil || !strings.Contains(err.Error(), "incomplete") {
			t.Fatalf("error = %v, want incomplete record", err)
		}
	})
}

func TestBundledPackageHEFHash(t *testing.T) {
	dir := t.TempDir()
	binPath := filepath.Join(dir, "model.bin")
	hef := testHEFBytes()
	writeTestPackage(t, binPath, tunedDetectionMeta(), hef)

	got, err := bundledPackageHEFHash(binPath)
	if err != nil {
		t.Fatalf("bundledPackageHEFHash() error: %v", err)
	}
	sum := sha256.Sum256(hef)
	if want := hex.EncodeToString(sum[:]); got != want {
		t.Errorf("bundledPackageHEFHash() = %s, want inner-HEF sha256 %s", got, want)
	}

	// A corrupted package is an error, never a hash.
	blob, _ := os.ReadFile(binPath)
	blob[len(blob)-2] ^= 0xFF
	os.WriteFile(binPath, blob, 0o644)
	if _, err := bundledPackageHEFHash(binPath); err == nil {
		t.Fatal("bundledPackageHEFHash() expected error for corrupted package")
	}
}
