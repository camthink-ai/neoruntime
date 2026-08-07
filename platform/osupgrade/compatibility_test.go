package osupgrade

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestCheckCompatibility(t *testing.T) {
	osInfo := &OSCompatibility{Machine: "hailo15-ne503", Product: "ne503", CompatLevel: 1, DataSchema: 1}
	app := &AppManifest{
		AppVersion:          "1.2.0",
		Machine:             "hailo15-ne503",
		Product:             "ne503",
		RequiredCompatLevel: 1,
		SupportedDataSchema: []int{1},
		TargetDataSchema:    1,
	}
	if err := CheckCompatibility(osInfo, app, 1); err != nil {
		t.Fatal(err)
	}
	app.RequiredCompatLevel = 2
	err := CheckCompatibility(osInfo, app, 1)
	var compatibilityErr *CompatibilityError
	if !errors.As(err, &compatibilityErr) || compatibilityErr.Code != "APP_COMPAT_LEVEL_MISMATCH" {
		t.Fatalf("unexpected error: %v", err)
	}
}

func TestLoadCompatibilityFiles(t *testing.T) {
	dir := t.TempDir()
	osPath := filepath.Join(dir, "aipc-os-release")
	appPath := filepath.Join(dir, "app-manifest.json")
	schemaPath := filepath.Join(dir, "schema-version")
	if err := os.WriteFile(osPath, []byte(
		"OS_VERSION=1.12.0\nAIPC_COMPAT_LEVEL=1\nDATA_SCHEMA=1\nMACHINE=hailo15-ne503\nPRODUCT=ne503\n",
	), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(appPath, []byte(
		`{"app_version":"1.2.0","machine":"hailo15-ne503","required_compat_level":1,"supported_data_schema":[1],"target_data_schema":1}`,
	), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(schemaPath, []byte("1\n"), 0644); err != nil {
		t.Fatal(err)
	}
	osInfo, err := LoadOSCompatibility(osPath)
	if err != nil {
		t.Fatal(err)
	}
	app, err := LoadAppManifest(appPath)
	if err != nil {
		t.Fatal(err)
	}
	schema, err := ReadDataSchema(schemaPath)
	if err != nil {
		t.Fatal(err)
	}
	if err := CheckCompatibility(osInfo, app, schema); err != nil {
		t.Fatal(err)
	}
}

func TestLoadAppManifestFallsBackToLegacyDataPath(t *testing.T) {
	root := t.TempDir()
	manifestPath := filepath.Join(root, "data", "app-manifest.json")
	if err := os.MkdirAll(filepath.Dir(manifestPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(manifestPath, []byte(
		`{"app_version":"1.2.0","machine":"hailo15-ne503","product":"ne503","required_compat_level":1,"supported_data_schema":[1],"target_data_schema":1}`,
	), 0644); err != nil {
		t.Fatal(err)
	}

	requested := filepath.Join(root, "opt", "aipc", "app-manifest.json")
	manifest, err := LoadAppManifest(requested)
	if err != nil {
		t.Fatal(err)
	}
	if manifest.AppVersion != "1.2.0" {
		t.Fatalf("unexpected App version %q", manifest.AppVersion)
	}
	resolved, err := ResolveAppManifestPath(requested)
	if err != nil {
		t.Fatal(err)
	}
	if resolved != manifestPath {
		t.Fatalf("resolved path = %q, want %q", resolved, manifestPath)
	}
}

// R6: a 0-byte manifest at the requested path must not short-circuit the
// fallback — the resolver must skip it and return the valid copy at a
// fallback location instead of failing with an opaque parse error later.
func TestResolveAppManifestPathSkipsEmptyFile(t *testing.T) {
	root := t.TempDir()
	// Requested path exists but is empty (simulates an incomplete restore).
	emptyPath := filepath.Join(root, "opt", "aipc", "app-manifest.json")
	if err := os.MkdirAll(filepath.Dir(emptyPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(emptyPath, nil, 0644); err != nil {
		t.Fatal(err)
	}
	// Persistent copy on /data is valid.
	dataPath := filepath.Join(root, "data", "app-manifest.json")
	if err := os.MkdirAll(filepath.Dir(dataPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(dataPath, []byte(
		`{"app_version":"1.2.1","machine":"hailo15-ne503","product":"ne503","required_compat_level":1,"supported_data_schema":[1],"target_data_schema":1}`,
	), 0644); err != nil {
		t.Fatal(err)
	}

	resolved, err := ResolveAppManifestPath(emptyPath)
	if err != nil {
		t.Fatalf("expected fallback to /data copy, got error: %v", err)
	}
	if resolved != dataPath {
		t.Fatalf("resolved path = %q, want %q (empty /opt file should be skipped)", resolved, dataPath)
	}
}

// R7: fallback must be bidirectional. When the env path points at the legacy
// /data location, the resolver must still discover a valid manifest at /opt
// (e.g. a fresh image where /data has not been populated yet). This makes the
// resolver robust regardless of which canonical path a service env uses.
func TestResolveAppManifestPathFallsBackFromDataToOpt(t *testing.T) {
	root := t.TempDir()
	// Requested /data path does not exist; /opt copy is valid (image default).
	optPath := filepath.Join(root, "opt", "aipc", "app-manifest.json")
	if err := os.MkdirAll(filepath.Dir(optPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(optPath, []byte(
		`{"app_version":"1.2.1","machine":"hailo15-ne503","product":"ne503","required_compat_level":1,"supported_data_schema":[1],"target_data_schema":1}`,
	), 0644); err != nil {
		t.Fatal(err)
	}

	requested := filepath.Join(root, "data", "app-manifest.json")
	resolved, err := ResolveAppManifestPath(requested)
	if err != nil {
		t.Fatalf("expected fallback to /opt copy, got error: %v", err)
	}
	if resolved != optPath {
		t.Fatalf("resolved path = %q, want %q (should fall back from /data to /opt)", resolved, optPath)
	}
}
