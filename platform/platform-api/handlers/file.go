package handlers

import (
	"archive/zip"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"

	"aipc/platform/common/constants"

	"github.com/gin-gonic/gin"
)

// FileHandler handles file management endpoints.
type FileHandler struct {
	allowedRoots []string
	defaultPath  string
}

// NewFileHandler creates a new FileHandler with allowed root directories.
func NewFileHandler(allowedRoots []string, rootPath string) *FileHandler {
	if rootPath == "" {
		rootPath = constants.RootPath()
	}
	if len(allowedRoots) == 0 {
		dataRoot := filepath.Dir(rootPath)
		if dataRoot == "." {
			dataRoot = rootPath
		}
		allowedRoots = []string{dataRoot, "/tmp"}
	}
	cleanRoots := make([]string, 0, len(allowedRoots))
	for _, root := range allowedRoots {
		if strings.TrimSpace(root) == "" {
			continue
		}
		cleanRoots = append(cleanRoots, filepath.Clean(root))
	}
	return &FileHandler{allowedRoots: cleanRoots, defaultPath: rootPath}
}

// validatePath checks that the path is within allowed roots and prevents traversal.
func (h *FileHandler) validatePath(rawPath string) (string, error) {
	cleaned := filepath.Clean(rawPath)
	if strings.Contains(cleaned, "..") {
		return "", fmt.Errorf("path traversal not allowed")
	}

	for _, root := range h.allowedRoots {
		if cleaned == root || strings.HasPrefix(cleaned, root+string(os.PathSeparator)) {
			return cleaned, nil
		}
	}
	return "", fmt.Errorf("access denied: path %s is outside allowed directories", cleaned)
}

// List returns directory contents.
func (h *FileHandler) List(c *gin.Context) {
	dirPath := c.DefaultQuery("path", h.defaultPath)
	safePath, err := h.validatePath(dirPath)
	if err != nil {
		Resp(c).FailMsg(CodeAccessDenied, err.Error())
		return
	}

	entries, err := os.ReadDir(safePath)
	if err != nil {
		Resp(c).FailMsg(CodeOperationFailed, err.Error())
		return
	}

	type fileEntry struct {
		Name    string `json:"name"`
		Path    string `json:"path"`
		IsDir   bool   `json:"is_dir"`
		Size    int64  `json:"size"`
		ModTime string `json:"mod_time"`
		Mode    string `json:"mode"`
	}

	files := make([]fileEntry, 0, len(entries))
	for _, e := range entries {
		info, err := e.Info()
		if err != nil {
			continue
		}
		files = append(files, fileEntry{
			Name:    e.Name(),
			Path:    filepath.Join(safePath, e.Name()),
			IsDir:   e.IsDir(),
			Size:    info.Size(),
			ModTime: info.ModTime().Format("2006-01-02 15:04:05"),
			Mode:    info.Mode().String(),
		})
	}

	// Directories first, then alphabetical
	sort.Slice(files, func(i, j int) bool {
		if files[i].IsDir != files[j].IsDir {
			return files[i].IsDir
		}
		return files[i].Name < files[j].Name
	})

	Resp(c).OK(gin.H{"path": safePath, "files": files})
}

// ReadContent returns the text content of a file (max 1MB).
func (h *FileHandler) ReadContent(c *gin.Context) {
	filePath := c.Query("path")
	if filePath == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "path is required")
		return
	}

	safePath, err := h.validatePath(filePath)
	if err != nil {
		Resp(c).FailMsg(CodeAccessDenied, err.Error())
		return
	}

	info, err := os.Stat(safePath)
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, err.Error())
		return
	}
	if info.IsDir() {
		Resp(c).FailMsg(CodeInvalidRequest, "path is a directory")
		return
	}
	if info.Size() > 1<<20 { // 1MB limit
		Resp(c).FailMsg(CodeInvalidRequest, "file too large (max 1MB for text view)")
		return
	}

	content, err := os.ReadFile(safePath)
	if err != nil {
		Resp(c).FailMsg(CodeOperationFailed, err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"path":    safePath,
		"size":    info.Size(),
		"content": string(content),
	})
}

// WriteContent creates or overwrites a file with the given content.
func (h *FileHandler) WriteContent(c *gin.Context) {
	var req struct {
		Path    string `json:"path" binding:"required"`
		Content string `json:"content"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	safePath, err := h.validatePath(req.Path)
	if err != nil {
		Resp(c).FailMsg(CodeAccessDenied, err.Error())
		return
	}

	if err := os.WriteFile(safePath, []byte(req.Content), 0644); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, err.Error())
		return
	}

	Resp(c).OK(gin.H{"path": safePath, "size": len(req.Content)})
}

// Upload handles file upload via multipart form.
func (h *FileHandler) Upload(c *gin.Context) {
	destDir := c.PostForm("path")
	if destDir == "" {
		destDir = h.defaultPath
	}

	safePath, err := h.validatePath(destDir)
	if err != nil {
		Resp(c).FailMsg(CodeAccessDenied, err.Error())
		return
	}

	file, err := c.FormFile("file")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "file is required: "+err.Error())
		return
	}

	destFile := filepath.Join(safePath, file.Filename)
	if err := c.SaveUploadedFile(file, destFile); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, err.Error())
		return
	}

	Resp(c).OK(gin.H{"path": destFile, "size": file.Size})
}

// Download serves a file for download.
func (h *FileHandler) Download(c *gin.Context) {
	filePath := c.Query("path")
	if filePath == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "path is required")
		return
	}

	safePath, err := h.validatePath(filePath)
	if err != nil {
		Resp(c).FailMsg(CodeAccessDenied, err.Error())
		return
	}

	info, err := os.Stat(safePath)
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, err.Error())
		return
	}
	if info.IsDir() {
		Resp(c).FailMsg(CodeInvalidRequest, "cannot download a directory")
		return
	}

	c.FileAttachment(safePath, filepath.Base(safePath))
}

// Delete removes a file or empty directory.
func (h *FileHandler) Delete(c *gin.Context) {
	filePath := c.Query("path")
	if filePath == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "path is required")
		return
	}

	safePath, err := h.validatePath(filePath)
	if err != nil {
		Resp(c).FailMsg(CodeAccessDenied, err.Error())
		return
	}

	// Prevent deleting root allowed directories
	for _, root := range h.allowedRoots {
		if safePath == root {
			Resp(c).FailMsg(CodeAccessDenied, "cannot delete root directory")
			return
		}
	}

	if err := os.RemoveAll(safePath); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, err.Error())
		return
	}

	Resp(c).OK(gin.H{"deleted": safePath})
}

// MakeDir creates a new directory.
func (h *FileHandler) MakeDir(c *gin.Context) {
	var req struct {
		Path string `json:"path" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	safePath, err := h.validatePath(req.Path)
	if err != nil {
		Resp(c).FailMsg(CodeAccessDenied, err.Error())
		return
	}

	if err := os.MkdirAll(safePath, 0755); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, err.Error())
		return
	}

	Resp(c).OK(gin.H{"path": safePath})
}

// Rename renames or moves a file/directory.
func (h *FileHandler) Rename(c *gin.Context) {
	var req struct {
		OldPath string `json:"old_path" binding:"required"`
		NewPath string `json:"new_path" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	safeOld, err := h.validatePath(req.OldPath)
	if err != nil {
		Resp(c).FailMsg(CodeAccessDenied, "old_path: "+err.Error())
		return
	}
	safeNew, err := h.validatePath(req.NewPath)
	if err != nil {
		Resp(c).FailMsg(CodeAccessDenied, "new_path: "+err.Error())
		return
	}

	if err := os.Rename(safeOld, safeNew); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, err.Error())
		return
	}

	Resp(c).OK(gin.H{"old_path": safeOld, "new_path": safeNew})
}

// BatchDelete removes multiple files or directories.
func (h *FileHandler) BatchDelete(c *gin.Context) {
	var req struct {
		Paths []string `json:"paths" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	if len(req.Paths) == 0 {
		Resp(c).FailMsg(CodeInvalidRequest, "paths cannot be empty")
		return
	}

	// Validate all paths first
	safePaths := make([]string, 0, len(req.Paths))
	for _, path := range req.Paths {
		safePath, err := h.validatePath(path)
		if err != nil {
			Resp(c).FailMsg(CodeAccessDenied, fmt.Sprintf("invalid path %s: %s", path, err.Error()))
			return
		}

		// Prevent deleting root allowed directories
		isRoot := false
		for _, root := range h.allowedRoots {
			if safePath == root {
				isRoot = true
				break
			}
		}
		if isRoot {
			Resp(c).FailMsg(CodeAccessDenied, fmt.Sprintf("cannot delete root directory: %s", safePath))
			return
		}

		safePaths = append(safePaths, safePath)
	}

	// Delete all validated paths
	deleted := make([]string, 0, len(safePaths))
	failed := make([]map[string]string, 0)

	for _, safePath := range safePaths {
		if err := os.RemoveAll(safePath); err != nil {
			failed = append(failed, map[string]string{
				"path":  safePath,
				"error": err.Error(),
			})
		} else {
			deleted = append(deleted, safePath)
		}
	}

	// Return results
	result := gin.H{
		"deleted": deleted,
		"count":   len(deleted),
	}

	if len(failed) > 0 {
		result["failed"] = failed
		result["failed_count"] = len(failed)
		// Still return success if at least some files were deleted
		if len(deleted) > 0 {
			Resp(c).OK(result)
		} else {
			Resp(c).FailMsg(CodeOperationFailed, "all deletions failed")
		}
	} else {
		Resp(c).OK(result)
	}
}

// BatchDownload downloads multiple files as a ZIP archive.
func (h *FileHandler) BatchDownload(c *gin.Context) {
	var req struct {
		Paths []string `json:"paths" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	if len(req.Paths) == 0 {
		Resp(c).FailMsg(CodeInvalidRequest, "paths cannot be empty")
		return
	}

	// Validate all paths and collect files
	type fileToZip struct {
		SourcePath string
		ZipPath    string
	}

	filesToZip := make([]fileToZip, 0, len(req.Paths))
	var baseDir string

	for _, path := range req.Paths {
		safePath, err := h.validatePath(path)
		if err != nil {
			Resp(c).FailMsg(CodeAccessDenied, fmt.Sprintf("invalid path %s: %s", path, err.Error()))
			return
		}

		info, err := os.Stat(safePath)
		if err != nil {
			Resp(c).FailMsg(CodeNotFound, fmt.Sprintf("cannot access path %s: %s", path, err.Error()))
			return
		}

		// Skip directories
		if info.IsDir() {
			continue
		}

		// Determine base directory for relative paths
		if baseDir == "" {
			// Use the parent directory of the first file as base
			baseDir = filepath.Dir(safePath)
			// Use root path as base if possible
			for _, root := range h.allowedRoots {
				if strings.HasPrefix(safePath, root) {
					baseDir = root
					break
				}
			}
		}

		// Calculate relative path from base directory
		relPath := strings.TrimPrefix(safePath, baseDir)
		relPath = strings.TrimPrefix(relPath, "/")

		filesToZip = append(filesToZip, fileToZip{
			SourcePath: safePath,
			ZipPath:    relPath,
		})
	}

	if len(filesToZip) == 0 {
		Resp(c).FailMsg(CodeInvalidRequest, "no valid files to download")
		return
	}

	// Create ZIP file in memory
	c.Header("Content-Type", "application/zip")
	c.Header("Content-Disposition", fmt.Sprintf("attachment; filename=files_%s.zip", time.Now().Format("20060102_150405")))

	// Create a zip writer
	zipWriter := zip.NewWriter(c.Writer)
	defer zipWriter.Close()

	// Add each file to the ZIP
	for _, file := range filesToZip {
		if err := addFileToZip(zipWriter, file.SourcePath, file.ZipPath); err != nil {
			Resp(c).FailMsg(CodeOperationFailed, fmt.Sprintf("failed to add file %s to zip: %s", file.SourcePath, err.Error()))
			return
		}
	}
}

// addFileToZip adds a file to the ZIP archive
func addFileToZip(zipWriter *zip.Writer, sourcePath, zipPath string) error {
	file, err := os.Open(sourcePath)
	if err != nil {
		return err
	}
	defer file.Close()

	info, err := file.Stat()
	if err != nil {
		return err
	}

	header, err := zip.FileInfoHeader(info)
	if err != nil {
		return err
	}

	header.Name = zipPath
	header.Method = zip.Deflate

	writer, err := zipWriter.CreateHeader(header)
	if err != nil {
		return err
	}

	_, err = io.Copy(writer, file)
	return err
}

// Suppress unused import warnings
var _ = io.Discard
