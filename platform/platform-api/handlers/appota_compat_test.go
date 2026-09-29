package handlers

import (
	"os"
	"path/filepath"
	"testing"

	"aipc/platform/osupgrade"
)

func withCompatTestEnv(t *testing.T) (osPath, schemaPath string) {
	t.Helper()
	dir := t.TempDir()
	osPath = filepath.Join(dir, "aipc-os-release")
	schemaPath = filepath.Join(dir, "schema-version")
	t.Setenv("AIPC_OS_COMPATIBILITY_FILE", osPath)
	t.Setenv("AIPC_DATA_SCHEMA_FILE", schemaPath)
	if err := os.WriteFile(osPath, []byte(
		"OS_VERSION=1.12.0\nMACHINE=hailo15-ne503\nPRODUCT=ne503\n",
	), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(schemaPath, []byte("1\n"), 0644); err != nil {
		t.Fatal(err)
	}
	return osPath, schemaPath
}

func writeAppPackage(t *testing.T, packageRoot, manifest string) {
	t.Helper()
	manifestPath := filepath.Join(packageRoot, "opt", "aipc", "app-manifest.json")
	if err := os.MkdirAll(filepath.Dir(manifestPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(manifestPath, []byte(manifest), 0644); err != nil {
		t.Fatal(err)
	}
}

func TestEvaluateAppPackageCompatibility(t *testing.T) {
	withCompatTestEnv(t)
	const validManifest = `{"app_version":"1.3.0","machine":"hailo15-ne503","product":"ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`
	tests := []struct {
		name      string
		osVersion string
		manifest  string
		wantValid bool
		wantCode  string
	}{
		{
			name:      "package inside declared os range",
			manifest:  validManifest,
			wantValid: true,
		},
		{
			name:      "os version below package minimum",
			manifest:  `{"app_version":"1.3.0","machine":"hailo15-ne503","min_os_version":"1.13.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`,
			wantValid: false,
			wantCode:  "APP_OS_VERSION_UNSUPPORTED",
		},
		{
			name:      "os version above package maximum",
			manifest:  `{"app_version":"1.3.0","machine":"hailo15-ne503","min_os_version":"1.10.0","max_os_version":"1.11.0","supported_data_schema":[1],"target_data_schema":1}`,
			wantValid: false,
			wantCode:  "APP_OS_VERSION_UNSUPPORTED",
		},
		{
			// Lexicographic comparison would accept "1.9.5" >= "1.10.0";
			// the range check must compare numerically.
			name:      "semantic comparison rejects 1.9.5 against 1.10.0 minimum",
			osVersion: "1.9.5",
			manifest:  validManifest,
			wantValid: false,
			wantCode:  "APP_OS_VERSION_UNSUPPORTED",
		},
		{
			name:      "machine mismatch",
			manifest:  `{"app_version":"1.3.0","machine":"other-board","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`,
			wantValid: false,
			wantCode:  "APP_MACHINE_MISMATCH",
		},
		{
			name:      "current data schema unsupported by package",
			manifest:  `{"app_version":"1.3.0","machine":"hailo15-ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[2],"target_data_schema":2}`,
			wantValid: false,
			wantCode:  "APP_DATA_SCHEMA_UNSUPPORTED",
		},
		{
			name:      "manifest missing from package",
			manifest:  "",
			wantValid: false,
			wantCode:  "APP_MANIFEST_MISSING",
		},
		{
			name:      "legacy compat-level manifest rejected as invalid metadata",
			manifest:  `{"app_version":"1.3.0","machine":"hailo15-ne503","required_compat_level":1,"supported_data_schema":[1],"target_data_schema":1}`,
			wantValid: false,
			wantCode:  "APP_COMPATIBILITY_METADATA_INVALID",
		},
		{
			name:      "min greater than max rejected as invalid metadata",
			manifest:  `{"app_version":"1.3.0","machine":"hailo15-ne503","min_os_version":"1.20.0","max_os_version":"1.10.0","supported_data_schema":[1],"target_data_schema":1}`,
			wantValid: false,
			wantCode:  "APP_COMPATIBILITY_METADATA_INVALID",
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			// Subtests share one os-release file; always rewrite it so a
			// case that overrides the version does not leak into the next.
			osVersion := tt.osVersion
			if osVersion == "" {
				osVersion = "1.12.0"
			}
			if err := os.WriteFile(os.Getenv("AIPC_OS_COMPATIBILITY_FILE"), []byte(
				"OS_VERSION="+osVersion+"\nMACHINE=hailo15-ne503\nPRODUCT=ne503\n",
			), 0644); err != nil {
				t.Fatal(err)
			}
			packageRoot := t.TempDir()
			if tt.manifest != "" {
				writeAppPackage(t, packageRoot, tt.manifest)
			}
			report := evaluateAppPackageCompatibility(packageRoot)
			if report.Valid != tt.wantValid {
				t.Fatalf("Valid = %v, want %v (code %q, message %q)",
					report.Valid, tt.wantValid, report.ErrorCode, report.Message)
			}
			if !tt.wantValid && report.ErrorCode != tt.wantCode {
				t.Fatalf("ErrorCode = %q, want %q (message %q)", report.ErrorCode, tt.wantCode, report.Message)
			}
		})
	}
}

func TestEvaluateAppPackageCompatibilityReportsRange(t *testing.T) {
	withCompatTestEnv(t)
	packageRoot := t.TempDir()
	writeAppPackage(t, packageRoot,
		`{"app_version":"1.3.0","machine":"hailo15-ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`)
	report := evaluateAppPackageCompatibility(packageRoot)
	if !report.Valid {
		t.Fatalf("expected valid package, got %q: %q", report.ErrorCode, report.Message)
	}
	if report.OSVersion != "1.12.0" || report.AppMinOSVersion != "1.10.0" || report.AppMaxOSVersion != "1.20.0" {
		t.Fatalf("unexpected range echo: %+v", report)
	}
}

// Legacy devices never shipped /etc/aipc-os-release; the first app package
// that brings the new contract must install there without being blocked.
func TestEvaluateAppPackageCompatibilityLegacyOSAllowed(t *testing.T) {
	osPath, schemaPath := withCompatTestEnv(t)
	if err := os.Remove(osPath); err != nil {
		t.Fatal(err)
	}
	if err := os.Remove(schemaPath); err != nil {
		t.Fatal(err)
	}
	packageRoot := t.TempDir()
	writeAppPackage(t, packageRoot,
		`{"app_version":"1.3.0","machine":"hailo15-ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`)
	report := evaluateAppPackageCompatibility(packageRoot)
	if !report.Valid {
		t.Fatalf("legacy image must be allowed, got %q: %q", report.ErrorCode, report.Message)
	}
	if report.Message == "" {
		t.Fatal("legacy allowance should carry a warning message")
	}
}

// A fresh device has no persisted schema file yet: the check must fall back
// to the schema the package itself targets instead of failing.
func TestEvaluateAppPackageCompatibilitySchemaFallback(t *testing.T) {
	_, schemaPath := withCompatTestEnv(t)
	if err := os.Remove(schemaPath); err != nil {
		t.Fatal(err)
	}
	packageRoot := t.TempDir()
	writeAppPackage(t, packageRoot,
		`{"app_version":"1.3.0","machine":"hailo15-ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`)
	report := evaluateAppPackageCompatibility(packageRoot)
	if !report.Valid {
		t.Fatalf("missing schema file must fall back to package target, got %q: %q", report.ErrorCode, report.Message)
	}
}

// The package builder nests the payload one directory deep; the manifest
// lookup must tolerate that layout.
func TestEvaluateAppPackageCompatibilityNestedPackageLayout(t *testing.T) {
	withCompatTestEnv(t)
	root := t.TempDir()
	writeAppPackage(t, filepath.Join(root, "payload"),
		`{"app_version":"1.3.0","machine":"hailo15-ne503","min_os_version":"1.10.0","max_os_version":"1.20.0","supported_data_schema":[1],"target_data_schema":1}`)
	report := evaluateAppPackageCompatibility(root)
	if !report.Valid {
		t.Fatalf("nested package must be accepted, got %q: %q", report.ErrorCode, report.Message)
	}
}

func TestAppOTAInProgressReadsDiskStatus(t *testing.T) {
	withOTAStatusTestFile(t)
	tests := []struct {
		status string
		want   bool
	}{
		{"", false}, // empty on-disk status counts as terminal
		{"idle", false},
		{"uploading", true},
		{"extracting", true},
		{"deploying", true},
		{"success", false},
		{"failed", false},
	}
	for _, tt := range tests {
		if err := os.WriteFile(otaStatusFile, []byte(`{"status":"`+tt.status+`"}`), 0644); err != nil {
			t.Fatal(err)
		}
		if got := appOTAInProgress(); got != tt.want {
			t.Fatalf("appOTAInProgress with disk status %q = %v, want %v", tt.status, got, tt.want)
		}
	}
}

func TestOSUpgradeInProgressFollowsActiveJob(t *testing.T) {
	store := osupgrade.NewStore(t.TempDir())
	h := &SystemHandlers{}
	if h.osUpgradeInProgress() {
		t.Fatal("nil store must disable the gate")
	}
	h.SetOSUpgradeStore(store)

	job := &osupgrade.Job{ID: "job-1", State: osupgrade.StateReady}
	if err := store.Save(job); err != nil {
		t.Fatal(err)
	}
	if err := store.SetActive(job.ID); err != nil {
		t.Fatal(err)
	}
	if !h.osUpgradeInProgress() {
		t.Fatal("ready job must count as in progress (awaiting install)")
	}

	job.State = osupgrade.StateSuccess
	if err := store.Save(job); err != nil {
		t.Fatal(err)
	}
	if h.osUpgradeInProgress() {
		t.Fatal("terminal job must release the gate")
	}
}
