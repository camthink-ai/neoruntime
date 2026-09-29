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

// OSCompatibility describes the running OS image as declared in
// /etc/aipc-os-release. Only the fields the app-side range check needs are
// parsed; legacy AIPC_COMPAT_LEVEL / DATA_SCHEMA keys are ignored.
type OSCompatibility struct {
	OSVersion string
	Machine   string
	Product   string
}

// AppManifest is the compatibility declaration shipped inside an app package
// at opt/aipc/app-manifest.json. MinOSVersion/MaxOSVersion form a closed
// range of supported OS versions (x.y.z, compared semantically).
type AppManifest struct {
	AppVersion          string `json:"app_version"`
	Machine             string `json:"machine"`
	Product             string `json:"product,omitempty"`
	MinOSVersion        string `json:"min_os_version"`
	MaxOSVersion        string `json:"max_os_version"`
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
	result := &OSCompatibility{
		OSVersion: values["OS_VERSION"],
		Machine:   values["MACHINE"],
		Product:   values["PRODUCT"],
	}
	if result.Machine == "" {
		return nil, fmt.Errorf("MACHINE is missing from %s", path)
	}
	if !isValidOSVersion(result.OSVersion) {
		return nil, fmt.Errorf("OS_VERSION %q in %s is missing or not in x.y.z form", result.OSVersion, path)
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
	return parseAppManifest(data)
}

// LoadAppManifestFile loads the manifest from exactly path, without the
// installed-manifest fallback resolution of LoadAppManifest. Callers judging
// an app package must evaluate the manifest that ships inside the package.
func LoadAppManifestFile(path string) (*AppManifest, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return parseAppManifest(data)
}

func parseAppManifest(data []byte) (*AppManifest, error) {
	var manifest AppManifest
	if err := json.Unmarshal(data, &manifest); err != nil {
		return nil, fmt.Errorf("parse app manifest: %w", err)
	}
	if manifest.AppVersion == "" || manifest.Machine == "" ||
		!isValidOSVersion(manifest.MinOSVersion) || !isValidOSVersion(manifest.MaxOSVersion) ||
		len(manifest.SupportedDataSchema) == 0 || manifest.TargetDataSchema <= 0 {
		return nil, fmt.Errorf("app manifest is incomplete")
	}
	if cmp, err := CompareVersion(manifest.MinOSVersion, manifest.MaxOSVersion); err != nil || cmp > 0 {
		return nil, fmt.Errorf("app manifest os version range is invalid (min %q, max %q)",
			manifest.MinOSVersion, manifest.MaxOSVersion)
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

// CheckCompatibility judges an app package (or installed app) against the
// running OS: machine/product must match, the OS version must fall inside the
// app's declared [min, max] range, and the app must support the schema of the
// data currently on disk. OS upgrades never call this as a gate; only the
// app-side install path enforces it.
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
	if belowMin, err := CompareVersion(target.OSVersion, app.MinOSVersion); err != nil || belowMin < 0 {
		return &CompatibilityError{
			Code: "APP_OS_VERSION_UNSUPPORTED",
			Message: fmt.Sprintf(
				"current OS %s is below the app minimum %s (supported range %s-%s)",
				target.OSVersion, app.MinOSVersion, app.MinOSVersion, app.MaxOSVersion,
			),
		}
	}
	if aboveMax, err := CompareVersion(target.OSVersion, app.MaxOSVersion); err != nil || aboveMax > 0 {
		return &CompatibilityError{
			Code: "APP_OS_VERSION_UNSUPPORTED",
			Message: fmt.Sprintf(
				"current OS %s is above the app maximum %s (supported range %s-%s)",
				target.OSVersion, app.MaxOSVersion, app.MinOSVersion, app.MaxOSVersion,
			),
		}
	}
	if !containsInt(app.SupportedDataSchema, currentSchema) {
		return &CompatibilityError{
			Code: "APP_DATA_SCHEMA_UNSUPPORTED",
			Message: fmt.Sprintf(
				"current data schema is %d, App supports %v",
				currentSchema,
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

// CompareVersion compares two x.y.z version strings semantically (1.9.0 <
// 1.10.0). It returns -1 when a < b, 0 when a == b, and 1 when a > b.
func CompareVersion(a, b string) (int, error) {
	aParts, err := parseVersionParts(a)
	if err != nil {
		return 0, fmt.Errorf("version %q: %w", a, err)
	}
	bParts, err := parseVersionParts(b)
	if err != nil {
		return 0, fmt.Errorf("version %q: %w", b, err)
	}
	for index := 0; index < len(aParts); index++ {
		if aParts[index] != bParts[index] {
			if aParts[index] < bParts[index] {
				return -1, nil
			}
			return 1, nil
		}
	}
	return 0, nil
}

func isValidOSVersion(value string) bool {
	_, err := parseVersionParts(value)
	return err == nil
}

func parseVersionParts(value string) ([3]int, error) {
	var parts [3]int
	segments := strings.Split(strings.TrimSpace(value), ".")
	if len(segments) != 3 {
		return parts, fmt.Errorf("must be x.y.z with numeric components")
	}
	for index, segment := range segments {
		if segment == "" || strings.Trim(segment, "0123456789") != "" {
			return parts, fmt.Errorf("component %q is not a non-negative integer", segment)
		}
		parts[index], _ = strconv.Atoi(segment)
	}
	return parts, nil
}

func containsInt(values []int, wanted int) bool {
	for _, value := range values {
		if value == wanted {
			return true
		}
	}
	return false
}
