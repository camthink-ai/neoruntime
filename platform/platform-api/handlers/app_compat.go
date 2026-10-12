package handlers

import (
	"errors"
	"os"

	"aipc/platform/osupgrade"

	"github.com/gin-gonic/gin"
)

// evaluateInstalledAppCompatibility judges the app that is currently installed
// under /data/aipc against the running OS. It mirrors the package verdict of
// evaluateAppPackageCompatibility but reads the installed manifest, so the
// banner reflects live state: the os-upgrade job store keeps a verdict only
// until the next job replaces it, and installing an app package never updates
// it.
func evaluateInstalledAppCompatibility() AppPackageCompatibility {
	report := AppPackageCompatibility{Valid: true}

	osInfo, err := osupgrade.LoadOSCompatibility(
		envDefault("AIPC_OS_COMPATIBILITY_FILE", osupgrade.DefaultOSCompatibilityPath))
	if err != nil {
		if os.IsNotExist(err) {
			// Legacy image contract: without /etc/aipc-os-release the boot-time
			// compatibility gates skip entirely, so the banner must not fire.
			enrichInstalledManifest(&report)
			report.Message = "legacy OS image without /etc/aipc-os-release; compatibility check skipped"
			return report
		}
		return incompatibleReport("APP_OS_METADATA_UNAVAILABLE", err.Error())
	}
	report.OSVersion = osInfo.OSVersion

	resolved, err := osupgrade.ResolveAppManifestPath(installedManifestPath())
	if err != nil {
		// The resolver reports "not found" as a plain error after exhausting
		// the candidate locations; only stat failures surface otherwise.
		return invalidate(report, "APP_MANIFEST_MISSING", err.Error())
	}
	manifest, err := osupgrade.LoadAppManifestFile(resolved)
	if err != nil {
		return invalidate(report, "APP_COMPATIBILITY_METADATA_INVALID", err.Error())
	}
	fillManifestFields(&report, manifest)

	currentSchema, err := osupgrade.ReadDataSchema(
		envDefault("AIPC_DATA_SCHEMA_FILE", osupgrade.DefaultDataSchemaPath))
	if err != nil {
		// Fresh device without a persisted schema: judge against the
		// schema the installed app itself targets.
		currentSchema = manifest.TargetDataSchema
	}

	if compatErr := osupgrade.CheckCompatibility(osInfo, manifest, currentSchema); compatErr != nil {
		var compat *osupgrade.CompatibilityError
		if errors.As(compatErr, &compat) {
			return invalidate(report, compat.Code, compat.Message)
		}
		return invalidate(report, "APP_COMPATIBILITY_METADATA_INVALID", compatErr.Error())
	}
	return report
}

// AppCompatStatus serves the installed-app compatibility verdict for the
// global web banner. It always answers code 0 with the verdict inside data:
// an incompatible device is degraded, not broken, and the endpoint itself is
// part of the rescue channel.
func (h *APIHandlers) AppCompatStatus(c *gin.Context) {
	Resp(c).OK(evaluateInstalledAppCompatibility())
}

func installedManifestPath() string {
	return envDefault("AIPC_APP_MANIFEST", osupgrade.DefaultAppManifestPath)
}

// enrichInstalledManifest best-effort fills app detail for the legacy-image
// skip path; no verdict is derived from it.
func enrichInstalledManifest(report *AppPackageCompatibility) {
	resolved, err := osupgrade.ResolveAppManifestPath(installedManifestPath())
	if err != nil {
		return
	}
	if manifest, err := osupgrade.LoadAppManifestFile(resolved); err == nil {
		fillManifestFields(report, manifest)
	}
}

func fillManifestFields(report *AppPackageCompatibility, manifest *osupgrade.AppManifest) {
	report.AppVersion = manifest.AppVersion
	report.AppMinOSVersion = manifest.MinOSVersion
	report.AppMaxOSVersion = manifest.MaxOSVersion
}

// invalidate flags an in-progress report as incompatible while keeping the
// detail fields already filled in, so the banner still shows which versions
// are in play.
func invalidate(report AppPackageCompatibility, code, message string) AppPackageCompatibility {
	report.Valid = false
	report.ErrorCode = code
	report.Message = message
	return report
}
