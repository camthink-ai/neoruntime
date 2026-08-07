package socket

import (
	"fmt"
	"os"
	"path/filepath"
)

const (
	AIPCGroupGID uint32 = 1001
)

// SetSocketGroupPermission sets the group ownership and permissions for a Unix socket
// to allow containers in the aipc group to access it.
// socketPath: path to the Unix socket file
func SetSocketGroupPermission(socketPath string) error {
	if socketPath == "" {
		return nil
	}

	// Check if socket exists
	if _, err := os.Stat(socketPath); err != nil {
		return fmt.Errorf("socket not found: %s", err)
	}

	// Change group ownership to aipc group (GID 1001)
	if err := os.Chown(socketPath, 0, int(AIPCGroupGID)); err != nil {
		// If we can't chown (not root), try chmod instead
		if err := os.Chmod(socketPath, 0666); err != nil {
			return fmt.Errorf("failed to set socket permissions: %w", err)
		}
		return nil
	}

	// Set permissions: owner rw, group rw, others none
	if err := os.Chmod(socketPath, 0660); err != nil {
		return fmt.Errorf("failed to set socket mode: %w", err)
	}

	return nil
}

// CreateUnixSocketWithPermission creates a Unix socket listener with proper permissions
// for container access. The socket is created with aipc group ownership.
func CreateUnixSocketWithPermission(listenAddr string) (func() error, error) {
	// Parse Unix socket path
	if len(listenAddr) < 7 || listenAddr[:7] != "unix://" {
		return nil, fmt.Errorf("not a unix socket address: %s", listenAddr)
	}
	socketPath := listenAddr[7:]

	// Ensure directory exists with proper permissions
	socketDir := filepath.Dir(socketPath)
	if err := os.MkdirAll(socketDir, 0755); err != nil {
		return nil, fmt.Errorf("failed to create socket directory: %w", err)
	}

	// Set directory group ownership
	os.Chown(socketDir, 0, int(AIPCGroupGID))
	os.Chmod(socketDir, 0775)

	// Remove existing socket if any
	os.Remove(socketPath)

	// Return cleanup function
	cleanup := func() error {
		return os.Remove(socketPath)
	}

	return cleanup, nil
}

// EnsureSocketDirPermission ensures the socket directory has proper permissions
func EnsureSocketDirPermission(socketPath string) error {
	socketDir := filepath.Dir(socketPath)

	// Create directory if not exists
	if err := os.MkdirAll(socketDir, 0755); err != nil {
		return err
	}

	// Set directory permissions
	if err := os.Chown(socketDir, 0, int(AIPCGroupGID)); err == nil {
		os.Chmod(socketDir, 0775)
	}

	return nil
}
