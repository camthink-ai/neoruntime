package osupgrade

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

func TestLoadRecoveryBundle(t *testing.T) {
	dir := t.TempDir()
	fit := filepath.Join(dir, "fitImage")
	root := filepath.Join(dir, "swupdate-image-hailo15-ne503.ext4.gz")
	if err := os.WriteFile(fit, []byte("fit"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(root, gzipBytes(t, []byte("root "+SingleRecoveryMarker)), 0644); err != nil {
		t.Fatal(err)
	}
	manifest, err := BuildRecoveryManifest("hailo15-ne503", "1.11.0", "1.2.0", "key-1", fit, root)
	if err != nil {
		t.Fatal(err)
	}
	data, _ := json.Marshal(manifest)
	if err := os.WriteFile(filepath.Join(dir, "manifest.json"), data, 0644); err != nil {
		t.Fatal(err)
	}
	bundle, err := LoadRecoveryBundle(dir, "hailo15-ne503")
	if err != nil {
		t.Fatal(err)
	}
	if bundle.Manifest.RecoveryVersion != "1.2.0" {
		t.Fatalf("unexpected manifest: %+v", bundle.Manifest)
	}
	if !bundle.SupportsLocalUpdateMode("copy-a") || bundle.SupportsLocalUpdateMode("ota-copy-a") {
		t.Fatalf("expected generated recovery manifest to support only copy-a: %+v", bundle.Manifest)
	}
	if err := bundle.Compatible(&ValidationResult{
		Machine:            "hailo15-ne503",
		MinRecoveryVersion: "1.1.0",
		SecureBootKeyID:    "key-1",
	}); err != nil {
		t.Fatal(err)
	}
}

func TestRecoveryBundleLegacyLocalModesDefaultToCopyA(t *testing.T) {
	bundle := &RecoveryBundle{Manifest: RecoveryManifest{
		LocalUpdateProtocol: SingleRecoveryMarker,
	}}
	if !bundle.SupportsLocalUpdateMode("copy-a") {
		t.Fatal("legacy recovery manifest should support copy-a")
	}
	if bundle.SupportsLocalUpdateMode("ota-copy-a") {
		t.Fatal("legacy recovery manifest must not imply ota-copy-a support")
	}
}

func TestRecoveryBundleWildcardLocalModes(t *testing.T) {
	bundle := &RecoveryBundle{Manifest: RecoveryManifest{
		LocalUpdateProtocol: SingleRecoveryMarker,
		LocalUpdateModes:    []string{"*"},
	}}
	for _, mode := range []string{"copy-a", "copy-b", "init-partitions-single", "vendor-maintenance"} {
		if !bundle.SupportsLocalUpdateMode(mode) {
			t.Fatalf("wildcard recovery manifest should support %s", mode)
		}
	}
}

func TestPrepareRecoveryBundleFromPackageDetectsLocalModes(t *testing.T) {
	packagePath := filepath.Join(t.TempDir(), "image.swu")
	writeCPIO(t, packagePath, map[string][]byte{
		"fitImage":                             []byte("fit"),
		"swupdate-image-hailo15-ne503.ext4.gz": gzipBytes(t, []byte("root "+SingleRecoveryMarker)),
		"hailo-image-hailo15-ne503.ext4.gz":    gzipBytes(t, []byte("main root")),
	})
	bundle, err := PrepareRecoveryBundleFromPackage(
		packagePath,
		filepath.Join(t.TempDir(), "recovery"),
		"hailo15-ne503",
		"1.2.3",
		"key-1",
	)
	if err != nil {
		t.Fatal(err)
	}
	if bundle.Manifest.RecoveryVersion != "1.2.3" ||
		!bundle.SupportsLocalUpdateMode("copy-a") ||
		bundle.SupportsLocalUpdateMode("ota-copy-a") {
		t.Fatalf("unexpected prepared recovery bundle: %+v", bundle.Manifest)
	}
}

func TestPrepareRecoveryBundleFromPackageDetectsWildcardLocalModes(t *testing.T) {
	packagePath := filepath.Join(t.TempDir(), "image.swu")
	writeCPIO(t, packagePath, map[string][]byte{
		"fitImage": []byte("fit"),
		"swupdate-image-hailo15-ne503.ext4.gz": gzipBytes(t, []byte(
			"root "+SingleRecoveryMarker+" AIPC_LOCAL_RECOVERY_ANY_MODE_V1",
		)),
		"hailo-image-hailo15-ne503.ext4.gz": gzipBytes(t, []byte("main root")),
	})
	bundle, err := PrepareRecoveryBundleFromPackage(
		packagePath,
		filepath.Join(t.TempDir(), "recovery"),
		"hailo15-ne503",
		"1.2.3",
		"key-1",
	)
	if err != nil {
		t.Fatal(err)
	}
	if !bundle.SupportsLocalUpdateMode("init-partitions-single") ||
		!bundle.SupportsLocalUpdateMode("vendor-mode") {
		t.Fatalf("expected wildcard local modes: %+v", bundle.Manifest)
	}
}

func TestLoadRecoveryBundleRejectsTamper(t *testing.T) {
	dir := t.TempDir()
	fit := filepath.Join(dir, "fitImage")
	root := filepath.Join(dir, "swupdate-image-hailo15-ne503.ext4.gz")
	_ = os.WriteFile(fit, []byte("fit"), 0644)
	_ = os.WriteFile(root, gzipBytes(t, []byte(SingleRecoveryMarker)), 0644)
	manifest, err := BuildRecoveryManifest("hailo15-ne503", "1.11.0", "1.0.0", "", fit, root)
	if err != nil {
		t.Fatal(err)
	}
	data, _ := json.Marshal(manifest)
	_ = os.WriteFile(filepath.Join(dir, "manifest.json"), data, 0644)
	_ = os.WriteFile(fit, []byte("tampered"), 0644)
	if _, err := LoadRecoveryBundle(dir, "hailo15-ne503"); err == nil {
		t.Fatal("expected tampered bundled recovery to be rejected")
	}
}

func TestRecoveryCompatibilityRejectsMinimumVersion(t *testing.T) {
	bundle := &RecoveryBundle{Manifest: RecoveryManifest{
		Machine:         "hailo15-ne503",
		RecoveryVersion: "1.0.0",
	}}
	if err := bundle.Compatible(&ValidationResult{
		Machine:            "hailo15-ne503",
		MinRecoveryVersion: "2.0.0",
	}); err == nil {
		t.Fatal("expected recovery version incompatibility")
	}
}
