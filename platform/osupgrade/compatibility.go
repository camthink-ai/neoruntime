package osupgrade

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strconv"
	"strings"
)

const (
	DefaultOSCompatibilityPath = "/etc/aipc-os-release"
	// DefaultAppManifestPath is the canonical manifest location under the
	// sole install root /data/aipc. It is also the env default for
	// AIPC_APP_MANIFEST (see runner.go / os_upgrade.go handler).
	DefaultAppManifestPath    = "/data/aipc/app-manifest.json"
	LegacyOptAppManifestPath  = "/opt/aipc/app-manifest.json" // pre-migration /opt rootfs layout (wiped on upgrade)
	LegacyDataAppManifestPath = "/data/app-manifest.json"     // pre-migration flat /data layout
	DefaultDataSchemaPath     = "/data/aipc-data/schema-version"
)

type OSCompatibility struct {
	OSVersion   string
	Machine     string
	Product     string
	CompatLevel int
	DataSchema  int
}

type AppManifest struct {
	AppVersion          string `json:"app_version"`
	Machine             string `json:"machine"`
	Product             string `json:"product,omitempty"`
	RequiredCompatLevel int    `json:"required_compat_level"`
	SupportedDataSchema []int  `json:"supported_data_schema"`
	TargetDataSchema    int    `json:"target_data_schema"`
}

type CompatibilityError struct {
	Code    string
	Message string
}

func (e *CompatibilityError) Error() string {
	return e.Code + ": " + e.Message
}

func LoadOSCompatibility(path string) (*OSCompatibility, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	values := parseKeyValue(string(data))
	compatLevel, err := positiveInt(values["AIPC_COMPAT_LEVEL"], "AIPC_COMPAT_LEVEL")
	if err != nil {
		return nil, err
	}
	dataSchema, err := positiveInt(values["DATA_SCHEMA"], "DATA_SCHEMA")
	if err != nil {
		return nil, err
	}
	result := &OSCompatibility{
		OSVersion:   values["OS_VERSION"],
		Machine:     values["MACHINE"],
		Product:     values["PRODUCT"],
		CompatLevel: compatLevel,
		DataSchema:  dataSchema,
	}
	if result.Machine == "" {
		return nil, fmt.Errorf("MACHINE is missing from %s", path)
	}
	return result, nil
}

func LoadAppManifest(path string) (*AppManifest, error) {
	resolved, err := ResolveAppManifestPath(path)
	if err != nil {
		return nil, err
	}
	data, err := os.ReadFile(resolved)
	if err != nil {
		return nil, err
	}
	var manifest AppManifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		return nil, fmt.Errorf("parse app manifest: %w", err)
	}
	if manifest.AppVersion == "" || manifest.Machine == "" || manifest.RequiredCompatLevel <= 0 ||
		len(manifest.SupportedDataSchema) == 0 || manifest.TargetDataSchema <= 0 {
		return nil, fmt.Errorf("app manifest is incomplete")
	}
	return &manifest, nil
}

// ResolveAppManifestPath supports both the current /opt/aipc installation and
// legacy/persistent installations rooted at /data. Explicit non-standard paths
// remain strict; fallback is enabled for any of the three canonical manifest
// locations so that an env path pointing at /opt (wiped on a rootfs upgrade
// while aipc-restore is masked or incomplete) still discovers the persistent
// /data copy, and vice versa. Empty files are skipped: a 0-byte manifest left
// behind by an incomplete restore is not usable and must not short-circuit the
// fallback (otherwise the caller gets an opaque parse error instead of the
// valid copy sitting one directory over).
func ResolveAppManifestPath(path string) (string, error) {
	candidates := appManifestCandidates(path)
	for _, candidate := range candidates {
		info, err := os.Stat(candidate)
		if err == nil && !info.IsDir() {
			if info.Size() == 0 {
				continue
			}
			return candidate, nil
		}
		if err != nil && !os.IsNotExist(err) {
			return "", err
		}
	}
	return "", fmt.Errorf("App manifest not found; checked %s", strings.Join(candidates, ", "))
}

// appManifestCandidates returns the requested path first, followed by every
// canonical manifest location so the resolver is robust regardless of which
// path the service env points at. A non-default root (test/chroot) is honored
// when the requested path sits under any of the canonical locations.
func appManifestCandidates(path string) []string {
	clean := filepath.Clean(path)
	candidates := []string{clean}
	seen := map[string]bool{clean: true}

	locations := []string{DefaultAppManifestPath, LegacyOptAppManifestPath, LegacyDataAppManifestPath}
	root := string(filepath.Separator)
	for _, loc := range locations {
		suffix := filepath.Clean(loc)
		if clean == suffix || strings.HasSuffix(clean, suffix) {
			root = strings.TrimSuffix(clean, suffix)
			if root == "" {
				root = string(filepath.Separator)
			}
			break
		}
	}

	for _, fallback := range locations {
		candidate := filepath.Join(root, strings.TrimPrefix(fallback, "/"))
		if !seen[candidate] {
			candidates = append(candidates, candidate)
			seen[candidate] = true
		}
	}
	return candidates
}

func ReadDataSchema(path string) (int, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0, err
	}
	return positiveInt(strings.TrimSpace(string(data)), "data schema")
}

func CheckCompatibility(target *OSCompatibility, app *AppManifest, currentSchema int) error {
	if target == nil || app == nil {
		return fmt.Errorf("compatibility metadata is unavailable")
	}
	if !strings.EqualFold(target.Machine, app.Machine) {
		return &CompatibilityError{
			Code:    "APP_MACHINE_MISMATCH",
			Message: fmt.Sprintf("OS machine %s does not match App machine %s", target.Machine, app.Machine),
		}
	}
	if target.Product != "" && app.Product != "" && !strings.EqualFold(target.Product, app.Product) {
		return &CompatibilityError{
			Code:    "APP_PRODUCT_MISMATCH",
			Message: fmt.Sprintf("OS product %s does not match App product %s", target.Product, app.Product),
		}
	}
	if target.CompatLevel != app.RequiredCompatLevel {
		return &CompatibilityError{
			Code: "APP_COMPAT_LEVEL_MISMATCH",
			Message: fmt.Sprintf(
				"OS compatibility level is %d, App requires %d",
				target.CompatLevel,
				app.RequiredCompatLevel,
			),
		}
	}
	if target.DataSchema != currentSchema || app.TargetDataSchema != target.DataSchema ||
		!containsInt(app.SupportedDataSchema, currentSchema) {
		return &CompatibilityError{
			Code: "APP_DATA_SCHEMA_UNSUPPORTED",
			Message: fmt.Sprintf(
				"OS data schema is %d, current data schema is %d, App targets %d and supports %v",
				target.DataSchema,
				currentSchema,
				app.TargetDataSchema,
				app.SupportedDataSchema,
			),
		}
	}
	return nil
}

func parseKeyValue(content string) map[string]string {
	values := make(map[string]string)
	for _, line := range strings.Split(content, "\n") {
		line = strings.TrimSpace(line)
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		key, value, ok := strings.Cut(line, "=")
		if ok {
			values[strings.TrimSpace(key)] = strings.Trim(strings.TrimSpace(value), `"'`)
		}
	}
	return values
}

func positiveInt(value, field string) (int, error) {
	number, err := strconv.Atoi(strings.TrimSpace(value))
	if err != nil || number <= 0 {
		return 0, fmt.Errorf("%s must be a positive integer", field)
	}
	return number, nil
}

func containsInt(values []int, wanted int) bool {
	for _, value := range values {
		if value == wanted {
			return true
		}
	}
	return false
}
