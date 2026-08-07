package utils

import (
	"net/url"
	"strings"
)

// ParseListenAddress parses a listen address that may be in URL format
// (e.g., "unix:///path/to/socket") or plain path format (e.g., "/path/to/socket")
// Returns the path suitable for net.Listen("unix", path)
func ParseListenAddress(addr string) (string, error) {
	// If it's already a plain path (starts with /), return as-is
	if strings.HasPrefix(addr, "/") {
		return addr, nil
	}

	// Try to parse as URL
	u, err := url.Parse(addr)
	if err != nil {
		// If parsing fails, assume it's a plain path
		return addr, nil
	}

	// If it's a unix:// URL, extract the path
	if u.Scheme == "unix" {
		// For unix:// URLs, the path is in u.Path
		// But we need to handle both unix:///path and unix://path
		if u.Path != "" {
			return u.Path, nil
		}
		// If Path is empty, try Host+Path (for unix://path format)
		if u.Host != "" {
			return u.Host + u.Path, nil
		}
	}

	// For other schemes or if we can't determine, return as-is
	return addr, nil
}
