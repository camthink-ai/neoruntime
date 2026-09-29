package handlers

import (
	"errors"
	"fmt"
	"os"
	"path/filepath"

	"aipc/platform/osupgrade"
)

// AppPackageCompatibility is the compatibility verdict of an app package
// against the currently running OS. OTAParseFirmware returns it advisory so
// the UI can warn before install; performOTAUpgrade enforces it as a hard
// gate, and deploy.sh re-checks as a backstop.
type AppPackageCompatibility struct {
	Valid           bool   `json:"valid"`
	ErrorCode       string `json:"error_code,omitempty"`
	Message         string `json:"message,omitempty"`
	OSVersion       string `json:"os_version,omitempty"`
	AppMinOSVersion string `json:"app_min_os_version,omitempty"`
	AppMaxOSVersion string `json:"app_max_os_version,omitempty"`
}

// evaluateAppPackageCompatibility judges an extracted app package (the
// directory holding deploy.sh) against the running OS. Legacy devices without
// /etc/aipc-os-release are allowed with a warning, so installing the first
// app package that brings the new contract is never blocked by metadata the
// old image never shipped.
func evaluateAppPackageCompatibility(packageRoot string) AppPackageCompatibility {
	report := AppPackageCompatibility{Valid: true}

	osInfo, err := osupgrade.LoadOSCompatibility(
		envDefault("AIPC_OS_COMPATIBILITY_FILE", osupgrade.DefaultOSCompatibilityPath))
	if err != nil {
		if os.IsNotExist(err) {
			report.Message = "legacy OS image without /etc/aipc-os-release; compatibility check skipped"
			return report
		}
		return incompatibleReport("APP_OS_METADATA_UNAVAILABLE", err.Error())
	}
	report.OSVersion = osInfo.OSVersion

	manifest, err := osupgrade.LoadAppManifestFile(appPackageManifestPath(packageRoot))
	if err != nil {
		if os.IsNotExist(errors.Unwrap(err)) || os.IsNotExist(err) {
			return incompatibleReport("APP_MANIFEST_MISSING",
				fmt.Sprintf("app manifest not found in package under %s", packageRoot))
		}
		return incompatibleReport("APP_COMPATIBILITY_METADATA_INVALID", err.Error())
	}
	report.AppMinOSVersion = manifest.MinOSVersion
	report.AppMaxOSVersion = manifest.MaxOSVersion

	currentSchema, err := osupgrade.ReadDataSchema(
		envDefault("AIPC_DATA_SCHEMA_FILE", osupgrade.DefaultDataSchemaPath))
	if err != nil {
		// Fresh device without a persisted schema: judge against the
		// schema the package itself targets.
		currentSchema = manifest.TargetDataSchema
	}

	if compatErr := osupgrade.CheckCompatibility(osInfo, manifest, currentSchema); compatErr != nil {
		var compat *osupgrade.CompatibilityError
		if errors.As(compatErr, &compat) {
			return incompatibleReport(compat.Code, compat.Message)
		}
		return incompatibleReport("APP_COMPATIBILITY_METADATA_INVALID", compatErr.Error())
	}
	return report
}

func incompatibleReport(code, message string) AppPackageCompatibility {
	return AppPackageCompatibility{
		Valid:     false,
		ErrorCode: code,
		Message:   message,
	}
}

// appPackageManifestPath locates opt/aipc/app-manifest.json inside an
// extracted package, tolerating the single level of directory nesting the
// package builder produces (VERSION/deploy.sh lookup follows the same rule).
func appPackageManifestPath(packageRoot string) string {
	candidates := []string{
		filepath.Join(packageRoot, "opt", "aipc", "app-manifest.json"),
	}
	entries, err := os.ReadDir(packageRoot)
	if err == nil {
		for _, entry := range entries {
			if !entry.IsDir() {
				continue
			}
			candidates = append(candidates,
				filepath.Join(packageRoot, entry.Name(), "opt", "aipc", "app-manifest.json"))
		}
	}
	for _, candidate := range candidates {
		if info, err := os.Stat(candidate); err == nil && !info.IsDir() && info.Size() > 0 {
			return candidate
		}
	}
	return candidates[0]
}
