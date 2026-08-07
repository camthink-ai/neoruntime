package handlers

import (
	"aipc/platform/common/constants"
	"archive/tar"
	"compress/gzip"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
)

// DebugLogHandler handles developer debug log export (export only)
type DebugLogHandler struct{}

// NewDebugLogHandler creates a new debug log handler
func NewDebugLogHandler() *DebugLogHandler {
	return &DebugLogHandler{}
}

// ExportRequest represents the export request payload
type ExportRequest struct {
	Services []string `json:"services"`
	Files    []string `json:"files"`
	Lines    int      `json:"lines"`
}

// Export exports debug logs as a tar.gz archive
func (h *DebugLogHandler) Export(c *gin.Context) {
	var req ExportRequest
	// Parse request body, use defaults if empty or invalid
	_ = c.ShouldBindJSON(&req)

	// Default services if not specified
	if len(req.Services) == 0 {
		req.Services = []string{
			"ai-runtime",
			"camera-daemon",
			"platform-api",
			"event-bus",
			"app-manager",
			"device-control",
		}
	}

	// Default files if not specified (will skip non-existent files)
	if len(req.Files) == 0 {
		req.Files = []string{
			"/var/log/syslog",
			"/var/log/kern.log",
			"/var/log/auth.log",
			"/var/log/dmesg",
			"/var/log/boot.log",
		}
	}

	// Default lines
	if req.Lines == 0 {
		req.Lines = 10000
	}

	// Limit export size
	if req.Lines > 50000 {
		req.Lines = 50000
	}

	// Create temporary file
	tempFile := fmt.Sprintf("/tmp/debug-logs-%d.tar.gz", time.Now().Unix())
	defer os.Remove(tempFile)

	// Create tar.gz archive
	if err := h.createTarGz(tempFile, req); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create archive: "+err.Error())
		return
	}

	// Ensure file has content
	if info, err := os.Stat(tempFile); err != nil || info.Size() == 0 {
		Resp(c).FailMsg(CodeServiceError, "Failed to create archive: empty file")
		return
	}

	// Set response headers
	filename := fmt.Sprintf("debug-logs-%s.tar.gz", time.Now().Format("20060102-150405"))
	c.Header("Content-Disposition", "attachment; filename=\""+filename+"\"")
	c.Header("Content-Type", "application/gzip")

	// Send file
	c.File(tempFile)
}

// createTarGz creates a tar.gz archive with logs and system info
func (h *DebugLogHandler) createTarGz(outputPath string, req ExportRequest) error {
	file, err := os.Create(outputPath)
	if err != nil {
		return fmt.Errorf("failed to create output file: %w", err)
	}
	defer file.Close()

	gzw := gzip.NewWriter(file)
	defer gzw.Close()

	tw := tar.NewWriter(gzw)
	defer tw.Close()

	var collectedServices []string
	var collectedFiles []string

	// Add service logs
	for _, service := range req.Services {
		if err := h.addServiceLog(tw, service, req.Lines); err != nil {
			fmt.Printf("Warning: failed to add service log %s: %v\n", service, err)
		} else {
			collectedServices = append(collectedServices, service)
		}
	}

	// Add file logs
	for _, filePath := range req.Files {
		if h.isAllowedPath(filePath) {
			if err := h.addLogFile(tw, filePath); err != nil {
				fmt.Printf("Warning: failed to add log file %s: %v\n", filePath, err)
			} else {
				collectedFiles = append(collectedFiles, filePath)
			}
		}
	}

	// Add system info with collection summary
	if err := h.addSystemInfo(tw, collectedServices, collectedFiles); err != nil {
		fmt.Printf("Warning: failed to add system info: %v\n", err)
	}

	return nil
}

// addServiceLog adds service journalctl log to the archive
func (h *DebugLogHandler) addServiceLog(tw *tar.Writer, service string, lines int) error {
	// Get journalctl output
	cmd := exec.Command("journalctl", "-u", service, "-n", strconv.Itoa(lines), "--no-pager")
	output, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("failed to get service log: %w", err)
	}

	// Create tar header
	header := &tar.Header{
		Name:    fmt.Sprintf("logs/%s.log", service),
		Mode:    0644,
		Size:    int64(len(output)),
		ModTime: time.Now(),
	}

	if err := tw.WriteHeader(header); err != nil {
		return fmt.Errorf("failed to write header: %w", err)
	}

	if _, err := tw.Write(output); err != nil {
		return fmt.Errorf("failed to write content: %w", err)
	}

	return nil
}

// addLogFile adds a log file to the archive
func (h *DebugLogHandler) addLogFile(tw *tar.Writer, filePath string) error {
	// Check if file exists, skip if not
	if _, err := os.Stat(filePath); os.IsNotExist(err) {
		return nil // Silently skip non-existent files
	}

	// Read file content
	output, err := os.ReadFile(filePath)
	if err != nil {
		return nil // Skip files that can't be read, don't interrupt export
	}

	// Skip empty files
	if len(output) == 0 {
		return nil
	}

	// Create tar header
	header := &tar.Header{
		Name:    fmt.Sprintf("logs/%s", filepath.Base(filePath)),
		Mode:    0644,
		Size:    int64(len(output)),
		ModTime: time.Now(),
	}

	if err := tw.WriteHeader(header); err != nil {
		return fmt.Errorf("failed to write header: %w", err)
	}

	if _, err := tw.Write(output); err != nil {
		return fmt.Errorf("failed to write content: %w", err)
	}

	return nil
}

// addSystemInfo adds system information to the archive
func (h *DebugLogHandler) addSystemInfo(tw *tar.Writer, services []string, files []string) error {
	info := ""

	// Export summary
	info += "=== Debug Log Export ===\n"
	info += fmt.Sprintf("Timestamp: %s\n", time.Now().Format(time.RFC3339))
	info += fmt.Sprintf("Services collected: %d\n", len(services))
	if len(services) > 0 {
		for _, s := range services {
			info += fmt.Sprintf("  - %s\n", s)
		}
	}
	info += fmt.Sprintf("Files collected: %d\n", len(files))
	if len(files) > 0 {
		for _, f := range files {
			info += fmt.Sprintf("  - %s\n", f)
		}
	}
	info += "\n"

	// System info
	if output, err := exec.Command("uname", "-a").CombinedOutput(); err == nil {
		info += "=== System Info ===\n" + string(output) + "\n\n"
	}

	// Uptime
	if output, err := exec.Command("uptime").CombinedOutput(); err == nil {
		info += "=== Uptime ===\n" + string(output) + "\n\n"
	}

	// Memory info (simplified)
	if output, err := exec.Command("cat", "/proc/meminfo").CombinedOutput(); err == nil {
		lines := strings.Split(string(output), "\n")
		if len(lines) >= 3 {
			info += "=== Memory Info ===\n"
			info += strings.Join(lines[:3], "\n") + "\n\n"
		}
	}

	// Disk usage
	if output, err := exec.Command("df", "-h").CombinedOutput(); err == nil {
		info += "=== Disk Usage ===\n" + string(output) + "\n\n"
	}

	// Create tar header
	header := &tar.Header{
		Name:    "system-info.txt",
		Mode:    0644,
		Size:    int64(len(info)),
		ModTime: time.Now(),
	}

	if err := tw.WriteHeader(header); err != nil {
		return fmt.Errorf("failed to write header: %w", err)
	}

	if _, err := tw.Write([]byte(info)); err != nil {
		return fmt.Errorf("failed to write content: %w", err)
	}

	return nil
}

// isAllowedPath checks if a path is within allowed directories
func (h *DebugLogHandler) isAllowedPath(path string) bool {
	cleanPath := filepath.Clean(path)

	allowedDirs := []string{
		"/var/log",
		constants.LogPath(),
	}

	for _, dir := range allowedDirs {
		if strings.HasPrefix(cleanPath, dir) {
			return true
		}
	}

	return false
}

// GetServices returns the list of available debug log services
func (h *DebugLogHandler) GetServices(c *gin.Context) {
	services := []gin.H{
		{"id": "ai-runtime", "name": "AI Runtime"},
		{"id": "camera-daemon", "name": "Camera Daemon"},
		{"id": "platform-api", "name": "Platform API"},
		{"id": "event-bus", "name": "Event Bus"},
		{"id": "app-manager", "name": "App Manager"},
		{"id": "device-control", "name": "Device Control"},
		{"id": "sshd", "name": "SSH Server"},
		{"id": "systemd", "name": "Systemd"},
	}

	Resp(c).OK(gin.H{
		"services": services,
	})
}

// GetFiles returns the list of available debug log files
func (h *DebugLogHandler) GetFiles(c *gin.Context) {
	allowedDirs := []string{
		"/var/log",
		constants.LogPath(),
	}

	var files []gin.H

	for _, dir := range allowedDirs {
		entries, err := os.ReadDir(dir)
		if err != nil {
			continue
		}

		for _, entry := range entries {
			if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".log") {
				fullPath := filepath.Join(dir, entry.Name())
				if info, err := entry.Info(); err == nil {
					files = append(files, gin.H{
						"path":          fullPath,
						"name":          entry.Name(),
						"size":          info.Size(),
						"modified_time": info.ModTime().Format("2006-01-02 15:04:05"),
					})
				}
			}
		}
	}

	Resp(c).OK(gin.H{
		"files": files,
	})
}
