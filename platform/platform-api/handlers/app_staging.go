package handlers

import (
	"fmt"
	"os"
	"path/filepath"
	"regexp"
	"time"

	"github.com/gin-gonic/gin"

	"aipc/platform/common/constants"
	"aipc/platform/common/logger"
)

const appStagingTTL = 24 * time.Hour

var stagingTokenPattern = regexp.MustCompile(`^[0-9]{10,20}_[0-9a-f]{8}$`)

func appStagingRoot() string {
	return filepath.Join(constants.RootPath(), "apps", "staging")
}

func newAppStagingDir() (string, error) {
	root := appStagingRoot()
	if err := os.MkdirAll(root, 0755); err != nil {
		return "", err
	}
	for i := 0; i < 8; i++ {
		dir := filepath.Join(root, uploadToken())
		if err := os.Mkdir(dir, 0700); err == nil {
			return dir, nil
		} else if !os.IsExist(err) {
			return "", err
		}
	}
	return "", fmt.Errorf("failed to allocate unique app staging directory")
}

// safeStagingArtifact accepts only a regular path immediately inside a safe,
// request-unique staging token directory. It intentionally rejects canonical
// manifests, token directories themselves, nested paths, and symlinked token
// directories.
func safeStagingArtifact(raw string) (string, string, error) {
	if raw == "" || !filepath.IsAbs(raw) {
		return "", "", fmt.Errorf("path must be an absolute staging artifact path")
	}
	clean := filepath.Clean(raw)
	root := appStagingRoot()
	rel, err := filepath.Rel(root, clean)
	if err != nil {
		return "", "", fmt.Errorf("invalid staging path: %w", err)
	}
	parts := splitCleanPath(rel)
	if len(parts) != 2 || !stagingTokenPattern.MatchString(parts[0]) {
		return "", "", fmt.Errorf("path must be a file directly under %s/<token>", root)
	}
	dir := filepath.Join(root, parts[0])
	info, err := os.Lstat(dir)
	if err != nil {
		return "", "", err
	}
	if !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
		return "", "", fmt.Errorf("staging token is not a real directory")
	}
	artifact, err := os.Lstat(clean)
	if err != nil {
		return "", "", err
	}
	if !artifact.Mode().IsRegular() || artifact.Mode()&os.ModeSymlink != 0 {
		return "", "", fmt.Errorf("staging artifact must be a regular file")
	}
	return clean, dir, nil
}

func safeStagingManifest(raw string) (string, error) {
	path, _, err := safeStagingArtifact(raw)
	if err != nil {
		return "", err
	}
	if filepath.Base(path) != "app.yaml" {
		return "", fmt.Errorf("manifest_path must name a staging app.yaml")
	}
	return path, nil
}

func splitCleanPath(path string) []string {
	if path == "." || path == "" {
		return nil
	}
	var out []string
	for path != "." && path != string(filepath.Separator) {
		dir, base := filepath.Split(path)
		if base == "" {
			break
		}
		out = append([]string{base}, out...)
		path = filepath.Clean(dir)
	}
	return out
}

// AbandonAppStaging removes request staging directories before an install task
// has started. Every supplied path is validated before anything is removed.
func (h *APIHandlers) AbandonAppStaging(c *gin.Context) {
	var req struct {
		Paths []string `json:"paths" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil || len(req.Paths) == 0 {
		Resp(c).FailMsg(CodeInvalidRequest, "paths must contain at least one staging artifact")
		return
	}
	dirs := make(map[string]struct{})
	for _, raw := range req.Paths {
		_, dir, err := safeStagingArtifact(raw)
		if err != nil {
			Resp(c).FailMsg(CodeInvalidParameter, err.Error())
			return
		}
		dirs[dir] = struct{}{}
	}
	for dir := range dirs {
		if err := os.RemoveAll(dir); err != nil {
			Resp(c).FailMsg(CodeOperationFailed, err.Error())
			return
		}
	}
	Resp(c).OK(gin.H{"abandoned": len(dirs)})
}

// CleanupAppStaging removes expired, safe token directories. Unknown entries
// and symlinks are deliberately left untouched.
func CleanupAppStaging(now time.Time) {
	root := appStagingRoot()
	entries, err := os.ReadDir(root)
	if err != nil {
		if !os.IsNotExist(err) {
			logger.Warn("Failed to scan app staging root %s: %v", root, err)
		}
		return
	}
	for _, entry := range entries {
		if !stagingTokenPattern.MatchString(entry.Name()) {
			continue
		}
		path := filepath.Join(root, entry.Name())
		info, err := os.Lstat(path)
		if err != nil || !info.IsDir() || info.Mode()&os.ModeSymlink != 0 {
			continue
		}
		if now.Sub(info.ModTime()) < appStagingTTL {
			continue
		}
		if err := os.RemoveAll(path); err != nil {
			logger.Warn("Failed to remove expired app staging directory %s: %v", path, err)
		}
	}
}
