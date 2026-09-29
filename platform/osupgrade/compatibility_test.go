package osupgrade

import (
	"errors"
	"os"
	"path/filepath"
	"testing"
)

func TestCheckCompatibility(t *testing.T) {
	baseApp := func() *AppManifest {
		return &AppManifest{
			AppVersion:          "1.2.0",
			Machine:             "hailo15-ne503",
			Product:             "ne503",
			MinOSVersion:        "1.10.0",
			MaxOSVersion:        "1.20.0",
			SupportedDataSchema: []int{1, 2},
			TargetDataSchema:    2,
		}
	}
	tests := []struct {
		name     string
		osInfo   *OSCompatibility
		mutate   func(*AppManifest)
		schema   int
		wantErr  bool
		wantCode string
	}{
		{
			name:   "os version inside range is compatible",
			osInfo: &OSCompatibility{OSVersion: "1.12.0", Machine: "hailo15-ne503", Product: "ne503"},
			schema: 1,
		},
		{
			name:   "os version equal to min boundary",
			osInfo: &OSCompatibility{OSVersion: "1.10.0", Machine: "hailo15-ne503", Product: "ne503"},
			schema: 1,
		},
		{
			name:   "os version equal to max boundary",
			osInfo: &OSCompatibility{OSVersion: "1.20.0", Machine: "hailo15-ne503", Product: "ne503"},
			schema: 1,
		},
		{
			name:     "os version below min",
			osInfo:   &OSCompatibility{OSVersion: "1.9.0", Machine: "hailo15-ne503", Product: "ne503"},
			schema:   1,
			wantErr:  true,
			wantCode: "APP_OS_VERSION_UNSUPPORTED",
		},
		{
			name:     "os version above max",
			osInfo:   &OSCompatibility{OSVersion: "1.21.0", Machine: "hailo15-ne503", Product: "ne503"},
			schema:   1,
			wantErr:  true,
			wantCode: "APP_OS_VERSION_UNSUPPORTED",
		},
		{
			// semantic compare, not lexicographic: 1.9.x < 1.10.x
			name:     "semantic comparison 1.9.0 below 1.10.0 minimum",
			osInfo:   &OSCompatibility{OSVersion: "1.9.5", Machine: "hailo15-ne503", Product: "ne503"},
			mutate:   func(a *AppManifest) { a.MinOSVersion = "1.10.0"; a.MaxOSVersion = "1.20.0" },
			schema:   1,
			wantErr:  true,
			wantCode: "APP_OS_VERSION_UNSUPPORTED",
		},
		{
			name:     "machine mismatch",
			osInfo:   &OSCompatibility{OSVersion: "1.12.0", Machine: "other-board", Product: "ne503"},
			schema:   1,
			wantErr:  true,
			wantCode: "APP_MACHINE_MISMATCH",
		},
		{
			name:     "product mismatch",
			osInfo:   &OSCompatibility{OSVersion: "1.12.0", Machine: "hailo15-ne503", Product: "ne50x"},
			schema:   1,
			wantErr:  true,
			wantCode: "APP_PRODUCT_MISMATCH",
		},
		{
			name:   "empty product on either side skips product check",
			osInfo: &OSCompatibility{OSVersion: "1.12.0", Machine: "hailo15-ne503"},
			mutate: func(a *AppManifest) { a.Product = "" },
			schema: 1,
		},
		{
			name:     "current schema not supported by app",
			osInfo:   &OSCompatibility{OSVersion: "1.12.0", Machine: "hailo15-ne503", Product: "ne503"},
			schema:   3,
			wantErr:  true,
			wantCode: "APP_DATA_SCHEMA_UNSUPPORTED",
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			app := baseApp()
			if tt.mutate != nil {
				tt.mutate(app)
			}
			err := CheckCompatibility(tt.osInfo, app, tt.schema)
			if tt.wantErr {
				var compatibilityErr *CompatibilityError
				if !errors.As(err, &compatibilityErr) || compatibilityErr.Code != tt.wantCode {
					t.Fatalf("error = %v, want code %s", err, tt.wantCode)
				}
				return
			}
			if err != nil {
				t.Fatal(err)
			}
		})
	}
}

func TestCompareVersion(t *testing.T) {
	tests := []struct {
		a, b string
		want int
	}{
		{"1.9.0", "1.10.0", -1},
		{"1.10.0", "1.9.0", 1},
		{"1.12.0", "1.12.0", 0},
		{"1.2.10", "1.2.9", 1},
		{"2.0.0", "1.99.99", 1},
		{"0.0.1", "0.0.2", -1},
	}
	for _, tt := range tests {
		got, err := CompareVersion(tt.a, tt.b)
		if err != nil {
			t.Fatalf("CompareVersion(%s, %s): %v", tt.a, tt.b, err)
		}
		if got != tt.want {
			t.Fatalf("CompareVersion(%s, %s) = %d, want %d", tt.a, tt.b, got, tt.want)
		}
	}
	for _, invalid := range []string{"1.2", "1.2.3.4", "1.2.x", "", "v1.2.3", "1.2.3-rc1"} {
		if _, err := CompareVersion(invalid, "1.2.3"); err == nil {
			t.Fatalf("CompareVersion(%q, ...) expected error for invalid version", invalid)
		}
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
		`{"app_version":"1.2.0","machine":"hailo15-ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`,
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
	if osInfo.OSVersion != "1.12.0" {
		t.Fatalf("OS version = %q, want 1.12.0", osInfo.OSVersion)
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

func TestLoadOSCompatibilityRejectsBadMetadata(t *testing.T) {
	dir := t.TempDir()
	osPath := filepath.Join(dir, "aipc-os-release")
	tests := []struct {
		name    string
		content string
	}{
		{"missing machine", "OS_VERSION=1.12.0\n"},
		{"missing os version", "MACHINE=hailo15-ne503\n"},
		{"malformed os version", "OS_VERSION=1.12\nMACHINE=hailo15-ne503\n"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if err := os.WriteFile(osPath, []byte(tt.content), 0644); err != nil {
				t.Fatal(err)
			}
			if _, err := LoadOSCompatibility(osPath); err == nil {
				t.Fatal("expected error")
			}
		})
	}
}

func TestLoadAppManifestRejectsInvalidMetadata(t *testing.T) {
	dir := t.TempDir()
	manifestPath := filepath.Join(dir, "app-manifest.json")
	tests := []struct {
		name    string
		content string
	}{
		{
			"legacy compat-level format",
			`{"app_version":"1.2.0","machine":"hailo15-ne503","required_compat_level":1,"supported_data_schema":[1],"target_data_schema":1}`,
		},
		{
			"min greater than max",
			`{"app_version":"1.2.0","machine":"hailo15-ne503","min_os_version":"1.20.0","max_os_version":"1.10.0","supported_data_schema":[1],"target_data_schema":1}`,
		},
		{
			"two-component version",
			`{"app_version":"1.2.0","machine":"hailo15-ne503","min_os_version":"1.10","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`,
		},
		{
			"empty schema list",
			`{"app_version":"1.2.0","machine":"hailo15-ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[],"target_data_schema":1}`,
		},
		{
			"missing machine",
			`{"app_version":"1.2.0","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`,
		},
		{
			"not json",
			`nonsense`,
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if err := os.WriteFile(manifestPath, []byte(tt.content), 0644); err != nil {
				t.Fatal(err)
			}
			if _, err := LoadAppManifestFile(manifestPath); err == nil {
				t.Fatal("expected error")
			}
		})
	}
}

// LoadAppManifestFile must read exactly the requested path: package judging
// must never silently fall back to the installed manifest.
func TestLoadAppManifestFileIsStrict(t *testing.T) {
	root := t.TempDir()
	installed := filepath.Join(root, "data", "aipc", "app-manifest.json")
	if err := os.MkdirAll(filepath.Dir(installed), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(installed, []byte(
		`{"app_version":"installed","machine":"hailo15-ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`,
	), 0644); err != nil {
		t.Fatal(err)
	}
	missing := filepath.Join(root, "package", "opt", "aipc", "app-manifest.json")
	if _, err := LoadAppManifestFile(missing); err == nil {
		t.Fatal("expected error for missing package manifest")
	}
}

func TestLoadAppManifestFallsBackToLegacyDataPath(t *testing.T) {
	root := t.TempDir()
	manifestPath := filepath.Join(root, "data", "app-manifest.json")
	if err := os.MkdirAll(filepath.Dir(manifestPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(manifestPath, []byte(
		`{"app_version":"1.2.0","machine":"hailo15-ne503","product":"ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`,
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
		`{"app_version":"1.2.1","machine":"hailo15-ne503","product":"ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`,
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
		`{"app_version":"1.2.1","machine":"hailo15-ne503","product":"ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`,
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
