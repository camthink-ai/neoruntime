package handlers

import (
	"aipc/platform/common/constants"
	"bufio"
	"context"
	"crypto/rsa"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/gin-gonic/gin"
	"gopkg.in/yaml.v3"

	"aipc/platform/common/events"
	"aipc/platform/common/logger"
	"aipc/platform/platform-api/adapters/auth"
	"aipc/platform/platform-api/config"
	"aipc/platform/platform-api/internal/secrets"
)

// OTAStatus represents the current OTA upgrade status
type OTAStatus struct {
	JobID           string `json:"job_id,omitempty"`           // Client-visible upgrade session ID
	Status          string `json:"status"`                     // idle, uploading, extracting, deploying, success, failed
	Progress        int    `json:"progress"`                   // 0-100
	Message         string `json:"message"`                    // Human readable message
	CurrentStep     string `json:"current_step"`               // Current step name
	Version         string `json:"version"`                    // Target version
	StartTime       int64  `json:"start_time"`                 // Unix timestamp
	FinishedAt      int64  `json:"finished_at,omitempty"`      // Unix timestamp when terminal
	Error           string `json:"error"`                      // Error message if failed
	RebootNeeded    bool   `json:"reboot_needed"`              // Whether reboot is needed
	RebootConfirmed bool   `json:"reboot_confirmed,omitempty"` // Whether the required reboot was observed
	BootID          string `json:"boot_id,omitempty"`          // Boot ID that wrote the terminal status
	CurrentBootID   string `json:"current_boot_id,omitempty"`  // Current device boot ID returned by platform-api
	LogPath         string `json:"log_path,omitempty"`         // Device-local deploy log path
	LogTail         string `json:"log_tail,omitempty"`         // Last deploy log lines for failed status
}

var (
	otaStatus     OTAStatus
	otaStatusMu   sync.RWMutex
	otaStatusFile = envDefault("AIPC_OTA_STATUS_FILE", "/data/aipc-data/ota_status.json")
	otaWorkDir    = envDefault("AIPC_OTA_WORK_DIR", "/data/aipc-data/ota-work")
)

const otaRebootCompleteGraceSeconds int64 = 45

func init() {
	// Load persisted status on startup
	loadOTAStatus()
}

func loadOTAStatus() {
	data, err := os.ReadFile(otaStatusFile)
	if err == nil {
		json.Unmarshal(data, &otaStatus)
	}
}

func reloadOTAStatusLocked() {
	if data, err := os.ReadFile(otaStatusFile); err == nil {
		var persisted OTAStatus
		if json.Unmarshal(data, &persisted) == nil {
			otaStatus = persisted
		}
	}
}

func currentBootID() string {
	data, err := os.ReadFile("/proc/sys/kernel/random/boot_id")
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(data))
}

func clearCompletedOTARebootIfSettledLocked() {
	if otaStatus.Status != "success" || !otaStatus.RebootNeeded || otaStatus.FinishedAt == 0 {
		return
	}

	current := currentBootID()
	if otaStatus.BootID != "" && current != "" {
		if otaStatus.BootID == current {
			return
		}
		otaStatus.RebootNeeded = false
		otaStatus.RebootConfirmed = true
		if strings.Contains(strings.ToLower(otaStatus.Message), "reboot") {
			otaStatus.Message = "Firmware upgrade completed"
		}
		saveOTAStatus()
		return
	}

	// Legacy OTA status files did not record a boot ID. Keep the old timeout
	// fallback only for those statuses; new packages use the boot-ID proof above.
	if time.Now().Unix()-otaStatus.FinishedAt < otaRebootCompleteGraceSeconds {
		return
	}
	otaStatus.RebootNeeded = false
	otaStatus.RebootConfirmed = true
	if strings.Contains(strings.ToLower(otaStatus.Message), "reboot") {
		otaStatus.Message = "Firmware upgrade completed"
	}
	saveOTAStatus()
}

// saveOTAStatus persists OTA status to disk.
// MUST be called with otaStatusMu already held (called from updateOTAStatus).
func saveOTAStatus() {
	data, _ := json.Marshal(otaStatus)
	_ = os.MkdirAll(filepath.Dir(otaStatusFile), 0755)
	os.WriteFile(otaStatusFile, data, 0644)
}

func updateOTAStatus(status, message, step string, progress int) {
	otaStatusMu.Lock()
	defer otaStatusMu.Unlock()
	otaStatus.Status = status
	otaStatus.Message = message
	otaStatus.CurrentStep = step
	otaStatus.Progress = progress
	otaStatus.Error = ""
	otaStatus.RebootNeeded = false
	otaStatus.LogTail = ""
	if isOTATerminalStatus(status) {
		otaStatus.FinishedAt = time.Now().Unix()
	} else if otaStatus.StartTime == 0 {
		otaStatus.StartTime = time.Now().Unix()
		otaStatus.FinishedAt = 0
	}
	saveOTAStatus()
}

func beginOTAStatus(jobID, status, message, step string, progress int) {
	otaStatusMu.Lock()
	defer otaStatusMu.Unlock()
	otaStatus = OTAStatus{
		JobID:       jobID,
		Status:      status,
		Message:     message,
		CurrentStep: step,
		Progress:    progress,
		StartTime:   time.Now().Unix(),
	}
	saveOTAStatus()
}

func isOTATerminalStatus(status string) bool {
	switch status {
	case "", "idle", "success", "failed":
		return true
	default:
		return false
	}
}

func newOTAJobID() string {
	return fmt.Sprintf("ota-%d", time.Now().UnixNano())
}

func attachOTALogTail(status *OTAStatus) {
	if status == nil || status.Status != "failed" || status.LogTail != "" || status.LogPath == "" {
		return
	}
	data, err := os.ReadFile(status.LogPath)
	if err != nil || len(data) == 0 {
		return
	}
	const maxBytes = 8192
	if len(data) > maxBytes {
		data = data[len(data)-maxBytes:]
		if idx := strings.IndexByte(string(data), '\n'); idx >= 0 && idx+1 < len(data) {
			data = data[idx+1:]
		}
	}
	status.LogTail = string(data)
}

// SystemHandlers handles system level configurations
type SystemHandlers struct {
	configPath  string
	eventLogger *events.Logger
	configMgr   *config.Manager
	rsaPriv     *rsa.PrivateKey // decrypts frontend-encrypted old/new passwords; nil -> plaintext fallback
}

func NewSystemHandlers(configPath string, configMgr *config.Manager, rsaPriv *rsa.PrivateKey) *SystemHandlers {
	if configPath == "" {
		configPath = constants.ConfigPath() + "/platform-api.yaml"
	}
	return &SystemHandlers{configPath: configPath, configMgr: configMgr, rsaPriv: rsaPriv}
}

func (h *SystemHandlers) SetEventLogger(logger *events.Logger) {
	h.eventLogger = logger
}

// projectAuthConfig persists the marshaled platform-api.yaml through the Config
// Controller state machine (revision history + audit + atomic write + Verify)
// when a Manager is available, falling back to a direct write otherwise. The
// detached rollback probe + restart are scheduled separately by the caller —
// the adapter only projects the file.
func (h *SystemHandlers) projectAuthConfig(ctx context.Context, actor, yamlStr string) error {
	if h.configMgr != nil {
		if _, _, err := h.configMgr.Apply(ctx, "auth", "config", yamlStr, actor); err != nil {
			logger.Warn("auth manager apply failed, falling back to direct write: %v", err)
		} else {
			return nil
		}
	}
	return os.WriteFile(h.configPath, []byte(yamlStr), 0644)
}

// UpdatePassword updates the background login password
func (h *SystemHandlers) UpdatePassword(c *gin.Context) {
	var req struct {
		OldPassword string `json:"old_password"`                    // base64(RSA) ciphertext, plaintext, or empty
		NewPassword string `json:"new_password" binding:"required"` // base64(RSA) ciphertext
		Timestamp   int64  `json:"timestamp"`                       // unix seconds; 0 = legacy client
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	data, err := os.ReadFile(h.configPath)
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to read config: "+err.Error())
		return
	}

	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to parse config: "+err.Error())
		return
	}

	// Because yaml.v3 Unmarshal to map[string]interface{} yields untyped inner maps.
	authSectionRaw, exists := config["auth"]
	if !exists {
		Resp(c).FailMsg(CodeServiceError, "Auth section not found in config")
		return
	}

	authSection, ok := authSectionRaw.(map[string]interface{})
	if !ok {
		// Fallback for map[interface{}]interface{} generated by some versions/parsers
		inner, okFallback := authSectionRaw.(map[interface{}]interface{})
		if !okFallback {
			Resp(c).FailMsg(CodeServiceError, "Auth section format is invalid")
			return
		}
		authSection = make(map[string]interface{})
		for k, v := range inner {
			authSection[k.(string)] = v
		}
		config["auth"] = authSection
	}

	// Replay protection (skipped for legacy clients without a timestamp).
	if req.Timestamp != 0 {
		if err := secrets.ValidateTimestamp(req.Timestamp, time.Now()); err != nil {
			Resp(c).FailMsg(CodeInvalidTimestamp, "Request timestamp out of allowed window")
			return
		}
	}

	// Decrypt the submitted passwords. Lenient: an undecryptable value passes
	// through unchanged so legacy plaintext clients keep working.
	newPassword := secrets.DecryptPassword(req.NewPassword, h.rsaPriv)
	if len(newPassword) < 8 || len(newPassword) > 32 {
		Resp(c).FailMsg(CodeInvalidParameter, "Password length must be 8-32")
		return
	}

	currentPassword, _ := authSection["password"].(string)
	if req.OldPassword != "" {
		oldPassword := secrets.DecryptPassword(req.OldPassword, h.rsaPriv)
		if !secrets.VerifyPassword(oldPassword, currentPassword) {
			Resp(c).FailMsg(CodeInvalidRequest, "Incorrect old password")
			return
		}
	}

	hashed, err := secrets.HashPassword(newPassword)
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to hash new password: "+err.Error())
		return
	}
	authSection["password"] = hashed

	outData, err := yaml.Marshal(config)
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to serialize config: "+err.Error())
		return
	}

	// R-auth-rollback: platform-api changing its OWN auth config + restarting
	// itself is unrecoverable if the new config breaks startup (the API is
	// unreachable to revert). Stage a pre-Apply backup on tmpfs that a detached
	// systemd-run probe restores on sustained post-restart failure. This is
	// separate from the Manager's in-memory Backup (which only covers a
	// synchronous Verify-fail). Only stage when a non-empty file already
	// exists — a missing/empty file leaves nothing to restore.
	const authBackupPath = "/run/aipc-auth-backup.yaml"
	staged := false
	if pre, rerr := os.ReadFile(h.configPath); rerr == nil && len(pre) > 0 {
		if werr := os.WriteFile(authBackupPath, pre, 0600); werr == nil {
			staged = true
		}
	}

	if err := h.projectAuthConfig(c.Request.Context(), getUsernameFromContext(c), string(outData)); err != nil {
		_ = os.Remove(authBackupPath) // don't leave a stale backup for a probe to act on
		Resp(c).FailMsg(CodeServiceError, "Failed to write config: "+err.Error())
		return
	}

	// Launch the detached rollback probe (best-effort, non-blocking). It survives
	// platform-api's stop (own cgroup), polls `systemctl is-active`, and on
	// sustained failure restores the staged backup + restarts again. Absent
	// systemd (dev) or a probe already in flight, it harmlessly no-ops — the
	// restart goroutine below still fires either way.
	if staged {
		if perr := auth.ScheduleAuthRollbackProbe(authBackupPath, h.configPath, "platform-api"); perr != nil {
			_ = os.Remove(authBackupPath)
		}
	}

	// Log password change
	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(string(events.EventPasswordChanged), events.MessageParams{"username": getUsernameFromContext(c)}, getUsernameFromContext(c))
	}

	// Schedule a restart of the API service
	go func() {
		time.Sleep(2 * time.Second)
		_ = exec.Command("systemctl", "restart", "platform-api").Run()
	}()

	Resp(c).OK(gin.H{"message": "Password updated successfully. Service is restarting."})
}

// RestartSystem handles hardware reboot
func (h *SystemHandlers) RestartSystem(c *gin.Context) {
	username := getUsernameFromContext(c)

	// Log system reboot
	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(string(events.EventSystemReboot), events.MessageParams{"triggered_by": username, "reason": "manual"}, getUsernameFromContext(c))
	}

	go func() {
		time.Sleep(2 * time.Second)
		_ = exec.Command("reboot").Run()
	}()
	Resp(c).OK(gin.H{"message": "System reboot initiated"})
}

// OTADetect checks for OTA updates
func (h *SystemHandlers) OTADetect(c *gin.Context) {
	Resp(c).OK(gin.H{
		"update_available": false,
		"version":          "",
		"changelog":        "",
	})
}

// OTAGetStatus returns current OTA status
func (h *SystemHandlers) OTAGetStatus(c *gin.Context) {
	otaStatusMu.Lock()
	// deploy.sh runs in a transient systemd service and atomically updates this
	// file after platform-api has been stopped/restarted. Treat disk as the
	// authoritative cross-process status before answering each request.
	reloadOTAStatusLocked()
	clearCompletedOTARebootIfSettledLocked()
	status := otaStatus
	otaStatusMu.Unlock()

	status.CurrentBootID = currentBootID()
	attachOTALogTail(&status)
	Resp(c).OK(status)
}

// OTAParseFirmware parses firmware package and returns version info without installing
func (h *SystemHandlers) OTAParseFirmware(c *gin.Context) {
	file, err := c.FormFile("firmware")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Failed to upload firmware: "+err.Error())
		return
	}

	tmpDir := "/tmp/ota_parse"
	os.RemoveAll(tmpDir)
	os.MkdirAll(tmpDir, 0755)
	defer os.RemoveAll(tmpDir)

	dest := filepath.Join(tmpDir, "firmware.tar.gz")
	if err := c.SaveUploadedFile(file, dest); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to save firmware: "+err.Error())
		return
	}

	// Extract
	cmdExt := exec.Command("tar", "xzf", "firmware.tar.gz")
	cmdExt.Dir = tmpDir
	if out, err := cmdExt.CombinedOutput(); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Extraction failed: "+string(out))
		return
	}

	// Find VERSION file
	versionFile := ""
	entries, _ := os.ReadDir(tmpDir)
	for _, e := range entries {
		if e.Name() == "VERSION" {
			versionFile = filepath.Join(tmpDir, "VERSION")
			break
		}
		if e.IsDir() {
			vPath := filepath.Join(tmpDir, e.Name(), "VERSION")
			if _, err := os.Stat(vPath); err == nil {
				versionFile = vPath
				break
			}
		}
	}

	if versionFile == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "VERSION file not found in firmware package")
		return
	}

	// Parse VERSION file
	version := ""
	buildDate := ""
	gitCommit := ""

	vData, err := os.ReadFile(versionFile)
	if err == nil {
		scanner := bufio.NewScanner(strings.NewReader(string(vData)))
		for scanner.Scan() {
			line := scanner.Text()
			if key, value, found := strings.Cut(line, "="); found {
				switch key {
				case "version":
					version = strings.TrimSpace(value)
				case "build_date":
					buildDate = strings.TrimSpace(value)
				case "git_commit":
					gitCommit = strings.TrimSpace(value)
				}
			}
		}
	}

	// Get current version
	currentVersion := "unknown"
	curData, err := os.ReadFile(constants.RootPath() + "/VERSION")
	if err == nil {
		scanner := bufio.NewScanner(strings.NewReader(string(curData)))
		for scanner.Scan() {
			line := scanner.Text()
			if strings.HasPrefix(line, "version=") {
				currentVersion = strings.TrimSpace(strings.TrimPrefix(line, "version="))
				break
			}
		}
	}

	// Save firmware path for later install
	savedPath := filepath.Join("/tmp", "ota_firmware_pending.tar.gz")
	os.Rename(dest, savedPath)

	Resp(c).OK(gin.H{
		"current_version": currentVersion,
		"target_version":  version,
		"build_date":      buildDate,
		"git_commit":      gitCommit,
		"firmware_path":   savedPath,
		"firmware_size":   file.Size,
	})
}

// OTAInstall receives firmware and installs with progress tracking
func (h *SystemHandlers) OTAInstall(c *gin.Context) {
	username := getUsernameFromContext(c)

	// Check if already upgrading
	otaStatusMu.Lock()
	reloadOTAStatusLocked()
	inProgress := !isOTATerminalStatus(otaStatus.Status)
	otaStatusMu.Unlock()
	if inProgress {
		Resp(c).FailMsg(CodeOperationFailed, "Upgrade already in progress")
		return
	}

	// Reset status
	jobID := newOTAJobID()
	beginOTAStatus(jobID, "uploading", "Uploading firmware package...", "upload", 5)

	file, err := c.FormFile("firmware")
	if err != nil {
		updateOTAStatus("failed", "Failed to upload firmware: "+err.Error(), "upload", 0)
		if h.eventLogger != nil {
			h.eventLogger.LogWithCodeAsync(string(events.EventFirmwareUpdateFailed), events.MessageParams{"version": "", "error": "upload failed: " + err.Error()}, getUsernameFromContext(c))
		}
		Resp(c).FailMsg(CodeInvalidRequest, "Failed to upload firmware: "+err.Error())
		return
	}

	tmpDir := filepath.Join(otaWorkDir, "current")
	os.RemoveAll(tmpDir)
	os.MkdirAll(tmpDir, 0755)

	dest := filepath.Join(tmpDir, "firmware.tar.gz")
	if err := c.SaveUploadedFile(file, dest); err != nil {
		updateOTAStatus("failed", "Failed to save firmware: "+err.Error(), "upload", 0)
		if h.eventLogger != nil {
			h.eventLogger.LogWithCodeAsync(string(events.EventFirmwareUpdateFailed), events.MessageParams{"version": "", "error": "save failed: " + err.Error()}, getUsernameFromContext(c))
		}
		Resp(c).FailMsg(CodeServiceError, "Failed to save firmware: "+err.Error())
		return
	}

	// Log firmware update start
	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("firmware.update.started", events.MessageParams{"version": file.Filename}, getUsernameFromContext(c))
	}

	// Start upgrade in background
	go h.performOTAUpgrade(dest, tmpDir, username, h.eventLogger, jobID)

	Resp(c).OK(gin.H{
		"message":  "OTA upgrade started",
		"status":   "started",
		"progress": "/api/v1/system/ota/status",
		"job_id":   jobID,
	})
}

// OTAInstallFromPath installs firmware from a local file path on the device
func (h *SystemHandlers) OTAInstallFromPath(c *gin.Context) {
	username := getUsernameFromContext(c)

	var req struct {
		Path string `json:"path" binding:"required"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	// Validate path
	firmwarePath := filepath.Clean(req.Path)

	if !filepath.IsAbs(firmwarePath) {
		Resp(c).FailMsg(CodeInvalidRequest, "Path must be absolute")
		return
	}

	// Check file exists
	if _, err := os.Stat(firmwarePath); os.IsNotExist(err) {
		Resp(c).FailMsg(CodeInvalidRequest, "Firmware file not found: "+firmwarePath)
		return
	}

	// Check if already upgrading
	otaStatusMu.Lock()
	reloadOTAStatusLocked()
	inProgress := !isOTATerminalStatus(otaStatus.Status)
	otaStatusMu.Unlock()
	if inProgress {
		Resp(c).FailMsg(CodeOperationFailed, "Upgrade already in progress")
		return
	}

	// Prepare temp directory
	tmpDir := filepath.Join(otaWorkDir, "current")
	os.RemoveAll(tmpDir)
	os.MkdirAll(tmpDir, 0755)

	// Copy firmware to temp dir
	dest := filepath.Join(tmpDir, "firmware.tar.gz")
	jobID := newOTAJobID()
	beginOTAStatus(jobID, "preparing", "Copying firmware file...", "prepare", 5)

	if err := copyFile(firmwarePath, dest); err != nil {
		updateOTAStatus("failed", "Failed to copy firmware: "+err.Error(), "prepare", 0)
		if h.eventLogger != nil {
			h.eventLogger.LogWithCodeAsync(string(events.EventFirmwareUpdateFailed), events.MessageParams{"version": "", "error": "copy failed"}, getUsernameFromContext(c))

		}
		Resp(c).FailMsg(CodeServiceError, "Failed to copy firmware: "+err.Error())
		return
	}

	// Log firmware update start
	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("firmware.update.started", events.MessageParams{"version": firmwarePath}, getUsernameFromContext(c))
	}

	// Start upgrade in background
	go h.performOTAUpgrade(dest, tmpDir, username, h.eventLogger, jobID)

	Resp(c).OK(gin.H{
		"message":     "OTA upgrade started",
		"status":      "started",
		"progress":    "/api/v1/system/ota/status",
		"source_path": firmwarePath,
		"job_id":      jobID,
	})
}

func copyFile(src, dst string) error {
	input, err := os.ReadFile(src)
	if err != nil {
		return err
	}
	return os.WriteFile(dst, input, 0644)
}

func (h *SystemHandlers) performOTAUpgrade(firmwarePath, tmpDir, username string, eventLogger *events.Logger, jobID string) {
	// Extract
	updateOTAStatus("extracting", "Extracting firmware package...", "extract", 20)

	cmdExt := exec.Command("tar", "xzf", filepath.Base(firmwarePath))
	cmdExt.Dir = tmpDir
	if out, err := cmdExt.CombinedOutput(); err != nil {
		updateOTAStatus("failed", "Extraction failed: "+string(out), "extract", 0)
		if eventLogger != nil {
			h.eventLogger.LogWithCodeAsync(string(events.EventFirmwareUpdateFailed), events.MessageParams{"version": "", "error": "extraction failed"}, username)
		}
		return
	}

	// Find deploy script and version
	scriptPath := filepath.Join(tmpDir, "deploy.sh")
	versionFile := filepath.Join(tmpDir, "VERSION")

	entries, _ := os.ReadDir(tmpDir)
	for _, e := range entries {
		if e.IsDir() {
			if _, err := os.Stat(filepath.Join(tmpDir, e.Name(), "deploy.sh")); err == nil {
				scriptPath = filepath.Join(tmpDir, e.Name(), "deploy.sh")
			}
			if _, err := os.Stat(filepath.Join(tmpDir, e.Name(), "VERSION")); err == nil {
				versionFile = filepath.Join(tmpDir, e.Name(), "VERSION")
			}
			break
		}
	}

	// Parse version
	var targetVersion string
	if vData, err := os.ReadFile(versionFile); err == nil {
		scanner := bufio.NewScanner(strings.NewReader(string(vData)))
		for scanner.Scan() {
			line := scanner.Text()
			if strings.HasPrefix(line, "version=") {
				targetVersion = strings.TrimSpace(strings.TrimPrefix(line, "version="))
				break
			}
		}
	}

	// Check deploy script exists
	if _, err := os.Stat(scriptPath); os.IsNotExist(err) {
		updateOTAStatus("failed", "deploy.sh not found in firmware package", "validate", 0)
		if eventLogger != nil {
			h.eventLogger.LogWithCodeAsync(string(events.EventFirmwareUpdateFailed), events.MessageParams{"version": targetVersion, "error": "deploy.sh not found"}, username)
		}
		return
	}

	// Update status to deploying
	logFile := filepath.Join(tmpDir, "deploy.log")
	otaStatusMu.Lock()
	otaStatus.JobID = jobID
	otaStatus.Status = "deploying"
	otaStatus.Message = "Deploying firmware..."
	otaStatus.CurrentStep = "deploy"
	otaStatus.Progress = 50
	otaStatus.Version = targetVersion
	otaStatus.LogPath = logFile
	otaStatus.LogTail = ""
	startTime := otaStatus.StartTime
	saveOTAStatus()
	otaStatusMu.Unlock()

	// Execute deploy as a transient service. deploy.sh owns the terminal status:
	// it writes success only after its command result and health check both pass.
	unitName := fmt.Sprintf("aipc-app-deploy-%d", time.Now().UnixNano())
	deployCommand := fmt.Sprintf("cd %s && exec ./deploy.sh --force >>%s 2>&1",
		shellQuote(filepath.Dir(scriptPath)), shellQuote(logFile))
	cmd := exec.Command("systemd-run", "--no-block", "--collect",
		"--unit="+unitName,
		"--description=AIPC OTA Deploy",
		"--setenv=AIPC_OTA_STATUS_FILE="+otaStatusFile,
		"--setenv=AIPC_OTA_PERSIST_STATUS_FILE="+otaStatusFile,
		"--setenv=AIPC_OTA_REBOOT_AFTER_SUCCESS=1",
		"--setenv=AIPC_OTA_REBOOT_DELAY_SECONDS=6",
		"--setenv=AIPC_OTA_VERSION="+targetVersion,
		"--setenv=AIPC_OTA_JOB_ID="+jobID,
		"--setenv=AIPC_OTA_LOG_FILE="+logFile,
		fmt.Sprintf("--setenv=AIPC_OTA_START_TIME=%d", startTime),
		"sh", "-c", deployCommand)

	if output, err := cmd.CombinedOutput(); err != nil {
		otaStatusMu.Lock()
		otaStatus.Status = "failed"
		otaStatus.Message = fmt.Sprintf("Failed to start deploy: %s", strings.TrimSpace(string(output)))
		otaStatus.CurrentStep = "deploy"
		otaStatus.Progress = 0
		otaStatus.Error = err.Error()
		otaStatus.FinishedAt = time.Now().Unix()
		otaStatus.LogPath = logFile
		saveOTAStatus()
		otaStatusMu.Unlock()

		if eventLogger != nil {
			h.eventLogger.LogWithCodeAsync(string(events.EventFirmwareUpdateFailed), events.MessageParams{"version": targetVersion, "error": "deploy failed"}, username)
		}
		return
	}
}

func shellQuote(value string) string {
	return "'" + strings.ReplaceAll(value, "'", "'\"'\"'") + "'"
}

// Helper function to get username from context
func getUsernameFromContext(c *gin.Context) string {
	// Try to get from basic auth
	if username, _, exists := c.Request.BasicAuth(); exists {
		return username
	}
	// Try to get from header (for future token-based auth)
	if username := c.GetHeader("X-User"); username != "" {
		return username
	}
	return "admin"
}
