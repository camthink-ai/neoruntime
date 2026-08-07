package handlers

import (
	"aipc/platform/common/constants"
	"bufio"
	"context"
	"io"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"
)

// Allowed systemd services regex
var serviceRegexp = regexp.MustCompile(`^[A-Za-z0-9\-\.]+$`)

// Allowed log files directories
var allowedLogDirs = []string{
	"/var/log",
	constants.LogPath(),
}

type LogHandler struct{}

func NewLogHandler() *LogHandler {
	return &LogHandler{}
}

// LogFileInfo represents a log file with metadata
type LogFileInfo struct {
	Path         string `json:"path"`
	Name         string `json:"name"`
	Size         int64  `json:"size"`
	ModifiedTime string `json:"modified_time"`
	Service      string `json:"service,omitempty"`
}

// serviceLogMap maps log file names to their corresponding services
var serviceLogMap = map[string]string{
	"ai-runtime.log":     "ai-runtime",
	"camera-daemon.log":  "camera-daemon",
	"platform-api.log":   "platform-api",
	"event-bus.log":      "event-bus",
	"app-manager.log":    "app-manager",
	"device-control.log": "device-control",
}

// GetServices enumerates key system services
func (h *LogHandler) GetServices(c *gin.Context) {
	// A fixed list of known services, or dynamically queried via systemctl
	services := []gin.H{
		{"id": "ai-runtime", "name": "AI Runtime"},
		{"id": "camera-daemon", "name": "Camera Daemon"},
		{"id": "platform-api", "name": "Platform API"},
		{"id": "event-bus", "name": "Event Bus"},
		{"id": "app-manager", "name": "App Manager"},
		{"id": "device-control", "name": "Device Control"},
		{"id": "sshd", "name": "SSH Server"},
		{"id": "docker", "name": "Docker"},
		{"id": "networkd", "name": "Network"},
	}

	Resp(c).OK(gin.H{
		"services": services,
	})
}

// GetFiles lists log files in allowed directories
func (h *LogHandler) GetFiles(c *gin.Context) {
	var files []LogFileInfo

	for _, dir := range allowedLogDirs {
		// Read upper dir
		entries, err := os.ReadDir(dir)
		if err != nil {
			continue // Skip if directory does not exist
		}

		for _, entry := range entries {
			if !entry.IsDir() && strings.HasSuffix(entry.Name(), ".log") {
				fullPath := filepath.Join(dir, entry.Name())

				// Get file info
				info, err := entry.Info()
				var size int64
				var modTime string
				if err == nil {
					size = info.Size()
					modTime = info.ModTime().Format("2006-01-02 15:04:05")
				}

				// Determine service from filename
				service := serviceLogMap[entry.Name()]

				files = append(files, LogFileInfo{
					Path:         fullPath,
					Name:         entry.Name(),
					Size:         size,
					ModifiedTime: modTime,
					Service:      service,
				})
			}
		}
	}

	Resp(c).OK(gin.H{
		"files": files,
	})
}

// GetContent reads historical logs (target can be a service or a file path)
func (h *LogHandler) GetContent(c *gin.Context) {
	logType := c.Query("type")   // "service" or "file"
	target := c.Query("target")  // e.g. "ai-runtime" or "/var/log/syslog"
	linesStr := c.Query("lines") // e.g. "500"

	if logType == "" || target == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Missing type or target parameter")
		return
	}

	lines := 500
	if linesStr != "" {
		if l, err := strconv.Atoi(linesStr); err == nil && l > 0 && l <= 10000 {
			lines = l
		}
	}

	var content []byte
	var err error

	if logType == "service" {
		if !serviceRegexp.MatchString(target) {
			Resp(c).FailMsg(CodeInvalidRequest, "Invalid service name format")
			return
		}

		// execute journalctl -u <target> -n <lines> --no-pager
		cmd := exec.Command("journalctl", "-u", target, "-n", strconv.Itoa(lines), "--no-pager")
		content, err = cmd.CombinedOutput()
		if err != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to read service logs: "+err.Error())
			return
		}

	} else if logType == "file" {
		// Security check: path traversal
		if strings.Contains(target, "..") {
			Resp(c).FailMsg(CodeInvalidRequest, "Path traversal attempt blocked")
			return
		}

		// Security check: white-list root
		allowed := false
		for _, dir := range allowedLogDirs {
			if strings.HasPrefix(target, dir) {
				allowed = true
				break
			}
		}
		if !allowed {
			Resp(c).FailMsg(CodeAccessDenied, "Access denied to read this path")
			return
		}

		// Use 'tail' command to efficiently get the last N lines of a file, since reverse reading in Go is somewhat complex manually
		cmd := exec.Command("tail", "-n", strconv.Itoa(lines), target)
		content, err = cmd.CombinedOutput()
		if err != nil {
			Resp(c).FailMsg(CodeNotFound, "Failed to read log file: "+err.Error())
			return
		}

	} else {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid log type string, must be 'service' or 'file'")
		return
	}

	Resp(c).OK(gin.H{
		"target":  target,
		"type":    logType,
		"lines":   lines,
		"content": string(content),
	})
}

// Download initiates a download of the full log content
func (h *LogHandler) Download(c *gin.Context) {
	logType := c.Query("type")
	target := c.Query("target")

	if logType == "" || target == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Missing type or target parameter")
		return
	}

	if logType == "service" {
		if !serviceRegexp.MatchString(target) {
			Resp(c).FailMsg(CodeInvalidRequest, "Invalid service name")
			return
		}

		c.Header("Content-Disposition", `attachment; filename="`+target+`.log"`)
		c.Header("Content-Type", "text/plain; charset=utf-8")
		ctx, cancel := context.WithTimeout(c.Request.Context(), 30*time.Second)
		defer cancel()

		cmd := exec.CommandContext(ctx, "journalctl", "-u", target, "--no-pager")
		cmd.Stdout = c.Writer
		cmd.Stderr = c.Writer
		err := cmd.Run()
		if err != nil {
			// Cannot send standard JSON response once header is written, just stop writing.
			return
		}

	} else if logType == "file" {
		if strings.Contains(target, "..") {
			Resp(c).FailMsg(CodeInvalidRequest, "Path traversal attempt blocked")
			return
		}

		allowed := false
		for _, dir := range allowedLogDirs {
			if strings.HasPrefix(target, dir) {
				allowed = true
				break
			}
		}
		if !allowed {
			Resp(c).FailMsg(CodeAccessDenied, "Access denied")
			return
		}

		if _, err := os.Stat(target); os.IsNotExist(err) {
			Resp(c).FailMsg(CodeNotFound, "File not found")
			return
		}

		filename := filepath.Base(target)
		c.Header("Content-Disposition", `attachment; filename="`+filename+`"`)
		c.Header("Content-Type", "application/octet-stream")
		c.File(target)
	} else {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid log type")
	}
}

// HandleStreamWS sets up a websocket to stream logs in real-time
func (h *LogHandler) HandleStreamWS(c *gin.Context) {
	logType := c.Query("type")
	target := c.Query("target")

	if logType == "" || target == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Missing type or target parameter")
		return
	}

	// Security Validation
	if logType == "service" {
		if !serviceRegexp.MatchString(target) {
			Resp(c).FailMsg(CodeInvalidRequest, "Invalid service name")
			return
		}
	} else if logType == "file" {
		if strings.Contains(target, "..") {
			Resp(c).FailMsg(CodeInvalidRequest, "Path traversal block")
			return
		}
		allowed := false
		for _, dir := range allowedLogDirs {
			if strings.HasPrefix(target, dir) {
				allowed = true
				break
			}
		}
		if !allowed {
			Resp(c).FailMsg(CodeAccessDenied, "Access denied")
			return
		}
	} else {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid log type")
		return
	}

	upgrader := websocket.Upgrader{
		CheckOrigin: func(r *http.Request) bool {
			return true // Allow all for now
		},
	}

	conn, err := upgrader.Upgrade(c.Writer, c.Request, nil)
	if err != nil {
		return // Connection error usually doesn't need a formal JSON response
	}
	defer conn.Close()

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	var cmd *exec.Cmd

	if logType == "service" {
		// Follow service logs
		cmd = exec.CommandContext(ctx, "journalctl", "-u", target, "-f", "-n", "100")
	} else {
		// Follow file logs
		cmd = exec.CommandContext(ctx, "tail", "-f", "-n", "100", target)
	}

	stdout, err := cmd.StdoutPipe()
	if err != nil {
		conn.WriteMessage(websocket.TextMessage, []byte("Error: "+err.Error()))
		return
	}

	// Also capture stderr
	cmd.Stderr = cmd.Stdout

	if err := cmd.Start(); err != nil {
		conn.WriteMessage(websocket.TextMessage, []byte("Failed to start stream: "+err.Error()))
		return
	}

	// Read from WS (blocks until client disconnects)
	go func() {
		for {
			if _, _, err := conn.ReadMessage(); err != nil {
				cancel() // Client disconnected, kill the journaling process
				break
			}
		}
	}()

	// Read from tail/journalctl process and write to WS
	reader := bufio.NewReader(stdout)
	for {
		line, err := reader.ReadBytes('\n')
		if err != nil {
			if err != io.EOF {
				conn.WriteMessage(websocket.TextMessage, []byte("\n=== Stream ended with error ===\n"))
			}
			break
		}

		if err := conn.WriteMessage(websocket.TextMessage, line); err != nil {
			break
		}
	}

	cancel() // Ensure we kill the process when the pipe breaks
	cmd.Wait()
}

// GetStructuredEntries returns parsed and structured log entries
func (h *LogHandler) GetStructuredEntries(c *gin.Context) {
	logType := c.Query("type")   // "service" or "file"
	target := c.Query("target")  // service name or file path
	linesStr := c.Query("lines") // number of lines to fetch
	category := c.Query("category")
	level := c.Query("level")
	startTime := c.Query("start_time")
	endTime := c.Query("end_time")
	search := c.Query("search")
	limitStr := c.DefaultQuery("limit", "100")
	offsetStr := c.DefaultQuery("offset", "0")

	if logType == "" || target == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Missing type or target parameter")
		return
	}

	lines := 1000
	if linesStr != "" {
		if l, err := strconv.Atoi(linesStr); err == nil && l > 0 && l <= 10000 {
			lines = l
		}
	}

	limit, _ := strconv.Atoi(limitStr)
	offset, _ := strconv.Atoi(offsetStr)

	// Create parser
	parser := NewLogParser()

	var entries []LogEntry
	var err error

	// Fetch raw log entries
	if logType == "service" {
		if !serviceRegexp.MatchString(target) {
			Resp(c).FailMsg(CodeInvalidRequest, "Invalid service name format")
			return
		}
		entries, err = parser.ParseServiceLogs(target, lines)
	} else if logType == "file" {
		if strings.Contains(target, "..") {
			Resp(c).FailMsg(CodeInvalidRequest, "Path traversal attempt blocked")
			return
		}
		allowed := false
		for _, dir := range allowedLogDirs {
			if strings.HasPrefix(target, dir) {
				allowed = true
				break
			}
		}
		if !allowed {
			Resp(c).FailMsg(CodeAccessDenied, "Access denied to read this path")
			return
		}
		entries, err = parser.ParseLogFile(target, lines)
	} else {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid log type, must be 'service' or 'file'")
		return
	}

	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to parse logs: "+err.Error())
		return
	}

	// Apply filters
	filterOpts := FilterOptions{
		Category:  LogCategory(category),
		Level:     LogLevel(level),
		StartTime: startTime,
		EndTime:   endTime,
		Search:    search,
		Limit:     limit,
		Offset:    offset,
	}
	filteredEntries := FilterEntries(entries, filterOpts)

	Resp(c).OK(gin.H{
		"type":     logType,
		"target":   target,
		"total":    len(entries),
		"filtered": len(filteredEntries),
		"entries":  filteredEntries,
	})
}

// GetStatistics returns log statistics
func (h *LogHandler) GetStatistics(c *gin.Context) {
	linesStr := c.DefaultQuery("lines", "1000")
	lines, _ := strconv.Atoi(linesStr)

	if lines > 10000 {
		lines = 10000 // Max limit
	}

	parser := NewLogParser()
	allEntries := make([]LogEntry, 0)

	// Fetch entries from all known services
	services := []string{"ai-runtime", "camera-daemon", "platform-api", "event-bus", "app-manager", "device-control"}
	for _, service := range services {
		entries, err := parser.ParseServiceLogs(service, lines/len(services))
		if err == nil {
			allEntries = append(allEntries, entries...)
		}
	}

	// Calculate statistics
	stats := CalculateStatistics(allEntries)

	Resp(c).OK(gin.H{
		"statistics": stats,
	})
}
