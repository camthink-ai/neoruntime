package handlers

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"
)

// withInstalledCompatEnv stages a synthetic installed-app compatibility
// environment: os-release, schema file and an installed manifest, all under a
// temp dir, wired through the AIPC_* env overrides the handlers honor.
func withInstalledCompatEnv(t *testing.T) (dir, manifestPath string) {
	t.Helper()
	dir = t.TempDir()
	osPath := filepath.Join(dir, "aipc-os-release")
	manifestPath = filepath.Join(dir, "app-manifest.json")
	schemaPath := filepath.Join(dir, "schema-version")
	t.Setenv("AIPC_OS_COMPATIBILITY_FILE", osPath)
	t.Setenv("AIPC_APP_MANIFEST", manifestPath)
	t.Setenv("AIPC_DATA_SCHEMA_FILE", schemaPath)
	if err := os.WriteFile(osPath, []byte(
		"OS_VERSION=1.12.0\nMACHINE=hailo15-ne503\nPRODUCT=ne503\n",
	), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(schemaPath, []byte("1\n"), 0644); err != nil {
		t.Fatal(err)
	}
	return dir, manifestPath
}

func writeInstalledManifest(t *testing.T, path, minOS, maxOS string) {
	t.Helper()
	manifest := `{"app_version":"v1.1.0-test","machine":"hailo15-ne503","product":"ne503",` +
		`"min_os_version":"` + minOS + `","max_os_version":"` + maxOS + `",` +
		`"supported_data_schema":[1],"target_data_schema":1}`
	if err := os.WriteFile(path, []byte(manifest), 0644); err != nil {
		t.Fatal(err)
	}
}

func TestEvaluateInstalledAppCompatibility(t *testing.T) {
	tests := []struct {
		name      string
		osRelease string // empty = keep the compatible default written by the helper
		manifest  string // "standard" | "absent" | a raw JSON body
		removeOS  bool
		wantValid bool
		wantCode  string
		wantOSVer string
	}{
		{
			name:      "compatible installed app",
			manifest:  "standard",
			wantValid: true,
			wantOSVer: "1.12.0",
		},
		{
			name:      "os outside the app range",
			osRelease: "OS_VERSION=1.13.0\nMACHINE=hailo15-ne503\nPRODUCT=ne503\n",
			manifest:  "standard",
			wantValid: false,
			wantCode:  "APP_OS_VERSION_UNSUPPORTED",
			wantOSVer: "1.13.0",
		},
		{
			name:      "machine mismatch",
			osRelease: "OS_VERSION=1.12.0\nMACHINE=hailo15-other\nPRODUCT=ne503\n",
			manifest:  "standard",
			wantValid: false,
			wantCode:  "APP_MACHINE_MISMATCH",
		},
		{
			name:      "missing installed manifest",
			manifest:  "absent",
			wantValid: false,
			wantCode:  "APP_MANIFEST_MISSING",
		},
		{
			name:      "incomplete manifest metadata",
			manifest:  `{"app_version":"v1.1.0-test","machine":"hailo15-ne503"}`,
			wantValid: false,
			wantCode:  "APP_COMPATIBILITY_METADATA_INVALID",
		},
		{
			name:      "legacy os image without os-release keeps services up",
			removeOS:  true,
			manifest:  "standard",
			wantValid: true,
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			_, manifestPath := withInstalledCompatEnv(t)
			switch {
			case tt.removeOS:
				os.Remove(os.Getenv("AIPC_OS_COMPATIBILITY_FILE"))
			case tt.osRelease != "":
				os.WriteFile(os.Getenv("AIPC_OS_COMPATIBILITY_FILE"), []byte(tt.osRelease), 0644)
			}
			switch {
			case tt.manifest == "standard":
				writeInstalledManifest(t, manifestPath, "1.12.0", "1.12.0")
			case tt.manifest != "absent":
				os.WriteFile(manifestPath, []byte(tt.manifest), 0644)
			}

			report := evaluateInstalledAppCompatibility()
			if report.Valid != tt.wantValid {
				t.Fatalf("valid = %v, want %v (code %q, message %q)",
					report.Valid, tt.wantValid, report.ErrorCode, report.Message)
			}
			if tt.wantCode != "" && report.ErrorCode != tt.wantCode {
				t.Fatalf("error code = %q, want %q (message %q)",
					report.ErrorCode, tt.wantCode, report.Message)
			}
			if tt.wantValid && report.ErrorCode != "" {
				t.Fatalf("valid report must carry no error code, got %q", report.ErrorCode)
			}
			if tt.wantOSVer != "" && report.OSVersion != tt.wantOSVer {
				t.Fatalf("os version = %q, want %q", report.OSVersion, tt.wantOSVer)
			}
			if tt.manifest == "standard" {
				if report.AppVersion != "v1.1.0-test" {
					t.Fatalf("app version = %q, want v1.1.0-test", report.AppVersion)
				}
				if report.AppMinOSVersion != "1.12.0" || report.AppMaxOSVersion != "1.12.0" {
					t.Fatalf("app range = [%q,%q], want [1.12.0,1.12.0]",
						report.AppMinOSVersion, report.AppMaxOSVersion)
				}
			}
		})
	}
}

// TestAppCompatStatusRoute pins the public registration contract: the banner
// endpoint must answer without a session token because the incompatibility it
// reports is exactly the state in which login/session bootstrap may be broken,
// and the banner shows on the pre-auth login page too.
func TestAppCompatStatusRoute(t *testing.T) {
	_, manifestPath := withInstalledCompatEnv(t)
	writeInstalledManifest(t, manifestPath, "1.12.0", "1.12.0")
	os.WriteFile(os.Getenv("AIPC_OS_COMPATIBILITY_FILE"),
		[]byte("OS_VERSION=1.13.0\nMACHINE=hailo15-ne503\nPRODUCT=ne503\n"), 0644)

	gin.SetMode(gin.TestMode)
	router := gin.New()
	router.GET("/api/v1/system/app-compat", (&APIHandlers{}).AppCompatStatus)

	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/api/v1/system/app-compat", nil)
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200 (body %s)", w.Code, w.Body.String())
	}
	body := w.Body.String()
	for _, want := range []string{
		`"code":0`,
		`"valid":false`,
		`"error_code":"APP_OS_VERSION_UNSUPPORTED"`,
		`"os_version":"1.13.0"`,
		`"app_version":"v1.1.0-test"`,
	} {
		if !strings.Contains(body, want) {
			t.Fatalf("response body missing %s: %s", want, body)
		}
	}
}
