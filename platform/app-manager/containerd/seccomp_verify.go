package containerd

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"

	"aipc/platform/common/constants"
	"github.com/opencontainers/runtime-spec/specs-go"
)

// VerifySeccompProfile verifies a seccomp profile file
// Returns error if profile is invalid, nil if valid
func VerifySeccompProfile(profilePath string) error {
	// Resolve absolute path
	if !filepath.IsAbs(profilePath) {
		possiblePaths := []string{
			filepath.Join(constants.ConfigPath(), profilePath),
			filepath.Join("/etc/aipc", profilePath),
			filepath.Join("/opt/aipc/etc/security", profilePath),
		}
		found := false
		for _, path := range possiblePaths {
			if _, err := os.Stat(path); err == nil {
				profilePath = path
				found = true
				break
			}
		}
		if !found {
			return fmt.Errorf("seccomp profile not found: %s", profilePath)
		}
	}

	// Check file exists
	if _, err := os.Stat(profilePath); err != nil {
		return fmt.Errorf("seccomp profile file not found: %w", err)
	}

	// Read and parse file
	profileData, err := os.ReadFile(profilePath)
	if err != nil {
		return fmt.Errorf("failed to read seccomp profile: %w", err)
	}

	var profile specs.LinuxSeccomp
	if err := json.Unmarshal(profileData, &profile); err != nil {
		return fmt.Errorf("failed to parse seccomp profile JSON: %w", err)
	}

	// Validate profile structure
	if err := validateSeccompProfile(&profile); err != nil {
		return fmt.Errorf("invalid seccomp profile: %w", err)
	}

	return nil
}

// GetSeccompProfileInfo returns information about a seccomp profile
func GetSeccompProfileInfo(profilePath string) (map[string]interface{}, error) {
	if err := VerifySeccompProfile(profilePath); err != nil {
		return nil, err
	}

	// Resolve path
	if !filepath.IsAbs(profilePath) {
		possiblePaths := []string{
			filepath.Join(constants.ConfigPath(), profilePath),
			filepath.Join("/etc/aipc", profilePath),
			filepath.Join("/opt/aipc/etc/security", profilePath),
		}
		for _, path := range possiblePaths {
			if _, err := os.Stat(path); err == nil {
				profilePath = path
				break
			}
		}
	}

	profileData, err := os.ReadFile(profilePath)
	if err != nil {
		return nil, err
	}

	var profile specs.LinuxSeccomp
	if err := json.Unmarshal(profileData, &profile); err != nil {
		return nil, err
	}

	architectures := make([]string, len(profile.Architectures))
	for i, arch := range profile.Architectures {
		architectures[i] = string(arch)
	}

	syscallCount := 0
	for _, syscall := range profile.Syscalls {
		syscallCount += len(syscall.Names)
	}

	info := map[string]interface{}{
		"path":          profilePath,
		"defaultAction": string(profile.DefaultAction),
		"architectures": architectures,
		"syscallCount":  syscallCount,
	}

	return info, nil
}
