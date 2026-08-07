package handlers

import (
	"aipc/platform/common/constants"
	"aipc/platform/platform-api/config"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"

	camerapb "aipc/platform/camera-daemon/proto"
	eventLoggerPkg "aipc/platform/common/events"
	"aipc/platform/common/logger"
	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"
)

// TimeHandler handles time-related requests
type TimeHandler struct {
	configPath  string
	eventLogger *eventLoggerPkg.Logger
	configMgr   *config.Manager
	mcuRaw      mcuRawRequester

	mcuSyncMu       sync.Mutex
	mcuSyncInFlight bool

	// Timezone cache (avoid repeated timedatectl calls)
	tzCacheMu   sync.Mutex
	tzCache     []TimezoneData
	tzCacheTime time.Time
}

type mcuRawRequester interface {
	McuRawRequest(ctx context.Context, in *camerapb.McuRawRequestMessage, opts ...grpc.CallOption) (*camerapb.McuRawResponseMessage, error)
}

// NewTimeHandler creates a new time handler. configMgr is optional: when nil
// (no DB / Config Controller disabled), every write falls back to the legacy
// direct os.WriteFile path. When set, file projection routes through the
// state machine for revision history + audit + atomic write + auto-restore.
func NewTimeHandler(configPath string, eventLogger *eventLoggerPkg.Logger, configMgr *config.Manager, cameraConn ...*grpc.ClientConn) *TimeHandler {
	var mcuRaw mcuRawRequester
	if len(cameraConn) > 0 && cameraConn[0] != nil {
		mcuRaw = camerapb.NewCameraControlClient(cameraConn[0])
	}
	return &TimeHandler{
		configPath:  configPath,
		eventLogger: eventLogger,
		configMgr:   configMgr,
		mcuRaw:      mcuRaw,
	}
}

// SetEventLogger sets the event logger (for dependency injection)
func (h *TimeHandler) SetEventLogger(logger *eventLoggerPkg.Logger) {
	h.eventLogger = logger
}

// SystemTime represents the current system time information
type SystemTime struct {
	CurrentTime     string `json:"current_time"` // RFC3339 format
	UnixTimestamp   int64  `json:"unix_timestamp"`
	Timezone        string `json:"timezone"`
	Uptime          uint64 `json:"uptime"`
	UptimeFormatted string `json:"uptime_formatted"`
}

// NTPConfig represents NTP configuration
type NTPConfig struct {
	Enabled         bool     `json:"enabled"`
	Server          string   `json:"server"`
	Interval        int      `json:"interval"` // sync interval in seconds
	Synced          bool     `json:"synced"`
	LastSync        string   `json:"last_sync,omitempty"` // RFC3339 format
	FallbackServers []string `json:"fallback_servers"`
}

// TimeConfig represents time configuration
type TimeConfig struct {
	Timezone   string    `json:"timezone"`
	TimeFormat string    `json:"time_format"` // "12h" or "24h"
	SyncMode   string    `json:"sync_mode"`   // "ntp", "manual", "local"
	NTP        NTPConfig `json:"ntp"`
	AutoSync   bool      `json:"auto_sync"`
}

// SetTimeRequest for setting system time manually
type SetTimeRequest struct {
	DateTime string `json:"datetime" binding:"required"` // RFC3339 format
}

// SetTimezoneRequest for setting timezone
type SetTimezoneRequest struct {
	Timezone string `json:"timezone" binding:"required"`
}

// SetNTPConfigRequest for configuring NTP
type SetNTPConfigRequest struct {
	Enabled  *bool  `json:"enabled,omitempty"`
	Server   string `json:"server,omitempty"`
	Interval int    `json:"interval,omitempty"` // sync interval in seconds
}

// SaveTimeConfigRequest for saving all time configuration at once
type SaveTimeConfigRequest struct {
	Timezone       string `json:"timezone" binding:"required"`
	TimeFormat     string `json:"time_format"`
	SyncMode       string `json:"sync_mode"`
	NTPServer      string `json:"ntp_server,omitempty"`
	NTPInterval    int    `json:"ntp_interval,omitempty"`
	ManualDatetime string `json:"manual_datetime,omitempty"`
}

// timeUserConfig stores user preferences not managed by systemd
type timeUserConfig struct {
	TimeFormat  string `json:"time_format"`
	SyncMode    string `json:"sync_mode"`
	NTPInterval int    `json:"ntp_interval"`
}

// TimezoneData represents available timezone
type TimezoneData struct {
	Name      string `json:"name"`
	Country   string `json:"country"`
	Offset    string `json:"offset"`     // Current UTC offset
	OffsetSec int    `json:"offset_sec"` // Offset in seconds
}

// Common timezones (fallback if system list unavailable)
var commonTimezones = []TimezoneData{
	{Name: "UTC", Country: "Universal", Offset: "UTC+00:00", OffsetSec: 0},
	{Name: "Asia/Shanghai", Country: "China", Offset: "UTC+08:00", OffsetSec: 28800},
	{Name: "Asia/Tokyo", Country: "Japan", Offset: "UTC+09:00", OffsetSec: 32400},
	{Name: "Asia/Seoul", Country: "Korea", Offset: "UTC+09:00", OffsetSec: 32400},
	{Name: "Asia/Singapore", Country: "Singapore", Offset: "UTC+08:00", OffsetSec: 28800},
	{Name: "Asia/Hong_Kong", Country: "Hong Kong", Offset: "UTC+08:00", OffsetSec: 28800},
	{Name: "Asia/Taipei", Country: "Taiwan", Offset: "UTC+08:00", OffsetSec: 28800},
	{Name: "Asia/Dubai", Country: "UAE", Offset: "UTC+04:00", OffsetSec: 14400},
	{Name: "Europe/London", Country: "UK", Offset: "UTC+00:00", OffsetSec: 0},
	{Name: "Europe/Paris", Country: "France", Offset: "UTC+01:00", OffsetSec: 3600},
	{Name: "Europe/Berlin", Country: "Germany", Offset: "UTC+01:00", OffsetSec: 3600},
	{Name: "Europe/Moscow", Country: "Russia", Offset: "UTC+03:00", OffsetSec: 10800},
	{Name: "America/New_York", Country: "USA", Offset: "UTC-05:00", OffsetSec: -18000},
	{Name: "America/Los_Angeles", Country: "USA", Offset: "UTC-08:00", OffsetSec: -28800},
	{Name: "America/Chicago", Country: "USA", Offset: "UTC-06:00", OffsetSec: -21600},
	{Name: "Australia/Sydney", Country: "Australia", Offset: "UTC+10:00", OffsetSec: 36000},
}

// Command timeout
const commandTimeout = 5 * time.Second

const (
	timesyncdPollMinSec     = 32
	defaultNTPPollMaxSec    = 3600
	ntpSyncVerificationWait = 15 * time.Second
	timesyncdProviderPath   = "/etc/systemd/ntp-units.d/10-aipc-timesyncd.list"

	hostLinkCmdRTCSet       = 0x0012
	mcuRTCSyncTimeout       = 5 * time.Second
	minPlausibleMCURTCYear  = 2024
	maxTwoDigitMCURTCYear   = 2099
	mcuRTCPayloadFieldCount = 7
)

// Default user config path
var defaultTimeUserConfigPath = constants.ConfigPath() + "/time-config.json"

// Last known time persistence path
var defaultLastKnownTimePath = constants.ConfigPath() + "/last-known-time.json"

// lastKnownTime stores the last known good system time for recovery after reboot
type lastKnownTime struct {
	UnixTimestamp int64  `json:"unix_timestamp"`
	Timezone      string `json:"timezone"`
	SavedAt       string `json:"saved_at"`
}

// runCommandWithTimeout executes a command with timeout
func runCommandWithTimeout(name string, args ...string) (string, error) {
	ctx, cancel := context.WithTimeout(context.Background(), commandTimeout)
	defer cancel()

	cmd := exec.CommandContext(ctx, name, args...)
	var out bytes.Buffer
	var stderr bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &stderr

	err := cmd.Run()
	if ctx.Err() == context.DeadlineExceeded {
		return "", fmt.Errorf("command timeout")
	}

	// Log stderr if there's any output
	if stderrStr := stderr.String(); stderrStr != "" {
		// Log stderr at debug level for troubleshooting
		logger.Debug("Command %s %v stderr: %s", name, args, strings.TrimSpace(stderrStr))
	}

	return strings.TrimSpace(out.String()), err
}

// GetSystemTime returns current system time information
func (h *TimeHandler) GetSystemTime(c *gin.Context) {
	now := time.Now()
	uptime := h.getUptime()

	timeInfo := SystemTime{
		CurrentTime:     now.Format(time.RFC3339),
		UnixTimestamp:   now.Unix(),
		Timezone:        h.getTimezoneFast(),
		Uptime:          uptime,
		UptimeFormatted: formatUptime(uptime),
	}

	Resp(c).OK(timeInfo)
}

// SetSystemTime sets the system time manually (requires admin)
func (h *TimeHandler) SetSystemTime(c *gin.Context) {
	var req SetTimeRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request format")
		return
	}

	// Parse RFC3339 datetime
	parsedTime, err := time.Parse(time.RFC3339, req.DateTime)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid datetime format, use RFC3339 format (e.g., 2024-01-01T12:00:00Z)")
		return
	}

	// Check if NTP is enabled - manual time setting requires NTP to be disabled
	if h.isNTPEnabled() {
		Resp(c).FailMsg(CodeInvalidRequest, "Cannot set time manually while NTP is enabled. Disable NTP first.")
		return
	}

	// Set system time using timedatectl
	timeStr := parsedTime.Format("2006-01-02 15:04:05")
	_, err = runCommandWithTimeout("timedatectl", "set-time", timeStr)
	if err != nil {
		logger.Error("Failed to set system time: %v", err)
		Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to set time: %v", err))
		return
	}

	logger.Info("System time set to %s", timeStr)

	h.persistCurrentTime()
	h.syncMCURTCFromHostAsync("manual_set")

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"time.manual_set",
			eventLoggerPkg.MessageParams{"datetime": timeStr},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OKMsg("Time updated successfully", nil)
}

// GetTimeConfig returns current time configuration
func (h *TimeHandler) GetTimeConfig(c *gin.Context) {
	ntpEnabled := h.isNTPEnabled()
	userCfg := h.loadTimeUserConfig()

	// Derive sync_mode from system state if not set
	syncMode := userCfg.SyncMode
	if syncMode == "" {
		if ntpEnabled {
			syncMode = "ntp"
		} else {
			syncMode = "manual"
		}
	}

	timeFormat := userCfg.TimeFormat
	if timeFormat == "" {
		timeFormat = "24h"
	}

	interval := userCfg.NTPInterval
	if interval <= 0 {
		interval = 3600 // default 1 hour
	}

	config := TimeConfig{
		Timezone:   h.getTimezoneFast(),
		TimeFormat: timeFormat,
		SyncMode:   syncMode,
		NTP: NTPConfig{
			Enabled:         ntpEnabled,
			Server:          h.getNTPServer(),
			Interval:        interval,
			Synced:          h.isNTPSynced(),
			LastSync:        h.getLastNTPSync(),
			FallbackServers: h.getNTPFallbackServers(),
		},
		AutoSync: ntpEnabled,
	}

	Resp(c).OK(config)
}

// SetTimezone sets the system timezone
func (h *TimeHandler) SetTimezone(c *gin.Context) {
	var req SetTimezoneRequest

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request format")
		return
	}

	// Validate timezone against system timezones
	valid := h.isValidTimezone(req.Timezone)
	if !valid {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid timezone: "+req.Timezone)
		return
	}

	// Set timezone using timedatectl
	_, err := runCommandWithTimeout("timedatectl", "set-timezone", req.Timezone)
	if err != nil {
		logger.Error("Failed to set timezone: %v", err)
		Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to set timezone: %v", err))
		return
	}

	// Ensure /etc/timezone is consistent (some embedded systems don't update
	// it). Route the projection through the Config Controller when available;
	// otherwise fall back to a direct write.
	if err := h.projectTimezone(c, req.Timezone); err != nil {
		logger.Warn("Failed to persist /etc/timezone: %v", err)
	}

	logger.Info("Timezone set to %s", req.Timezone)

	h.persistCurrentTime()

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"time.timezone.changed",
			eventLoggerPkg.MessageParams{"timezone": req.Timezone},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OKMsg("Timezone updated successfully", nil)
}

// SetNTPConfig configures NTP settings
func (h *TimeHandler) SetNTPConfig(c *gin.Context) {
	var req SetNTPConfigRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request format")
		return
	}
	if req.Interval < 0 {
		Resp(c).FailMsg(CodeInvalidRequest, "interval must be non-negative")
		return
	}

	desiredEnabled := h.isNTPEnabled()
	if req.Enabled != nil {
		desiredEnabled = *req.Enabled
	}

	if desiredEnabled {
		// Repair/create the provider and its D-Bus alias, then persistently
		// enable it. This also removes ntpd/chronyd conflicts. The file
		// projection (/etc/systemd/timesyncd.conf) routes through the Config
		// Controller when available; the exec (enable+restart) follows below.
		if err := h.projectNTPConfig(c, true, req.Server, req.Interval); err != nil {
			Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to apply NTP configuration: %v", err))
			return
		}
		if err := ensureTimesyncdEnabledAndRunning(true); err != nil {
			Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to apply NTP configuration: %v", err))
			return
		}
	}

	if !desiredEnabled {
		// Record the disabled desired-state through the Config Controller
		// (best-effort; the adapter writes no file when NTP is disabled).
		if err := h.projectNTPConfig(c, false, "", 0); err != nil {
			logger.Warn("Failed to record NTP-disabled desired-state: %v", err)
		}
		if err := disableNTPProviders(); err != nil {
			logger.Error("Failed to disable NTP: %v", err)
			Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to disable NTP: %v", err))
			return
		}
	}

	time.Sleep(100 * time.Millisecond)
	actualNTPState := h.isNTPEnabled()
	if actualNTPState != desiredEnabled {
		logger.Error("NTP state verification failed: expected %v, got %v", desiredEnabled, actualNTPState)
		Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("NTP state verification failed: expected %v, got %v", desiredEnabled, actualNTPState))
		return
	}
	if err := h.projectNTPUserConfig(c, desiredEnabled, req.Interval); err != nil {
		Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("NTP applied but failed to persist read-back config: %v", err))
		return
	}

	// Log with appropriate server info
	serverLog := req.Server
	if serverLog == "" {
		serverLog = "(默认)"
	}
	logger.Info("NTP configuration updated: enabled=%v, server=%s", desiredEnabled, serverLog)

	if h.eventLogger != nil {
		params := eventLoggerPkg.MessageParams{
			"enabled": desiredEnabled,
			"server":  serverLog,
		}
		h.eventLogger.LogWithCodeAsync(
			"time.ntp.config_changed",
			params,
			getUsernameFromContext(c),
		)
	}

	Resp(c).OKMsg("NTP configuration updated", nil)
}

// SyncNTP triggers manual NTP synchronization
func (h *TimeHandler) SyncNTP(c *gin.Context) {
	// Enabling NTP in timedated is only an intent flag; make the provider a
	// persistent boot service and restart it to trigger a fresh request.
	if err := ensureTimesyncdEnabledAndRunning(true); err != nil {
		logger.Error("Failed to start systemd-timesyncd: %v", err)
		Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to start NTP service: %v", err))
		return
	}
	// Do not report a successful synchronization merely because restart(2)
	// succeeded. Give the provider a bounded window to reach synchronized.
	deadline := time.Now().Add(ntpSyncVerificationWait)
	for !h.isNTPSynced() && time.Now().Before(deadline) {
		time.Sleep(500 * time.Millisecond)
	}
	if !h.isNTPSynced() {
		Resp(c).FailMsg(CodeServiceError, "NTP service is running but did not synchronize; check network, DNS and server reachability")
		return
	}

	logger.Info("NTP synchronization triggered")
	h.persistCurrentTime()
	h.syncMCURTCFromHostAsync("ntp_sync")

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"time.ntp.sync_triggered",
			nil, // No parameters needed
			getUsernameFromContext(c),
		)
	}

	Resp(c).OKMsg("NTP synchronization triggered", nil)
}

// GetTimezones returns available timezones
func (h *TimeHandler) GetTimezones(c *gin.Context) {
	// Try to get system timezones dynamically
	timezones := h.getSystemTimezones()
	if len(timezones) == 0 {
		// Fallback to common timezones
		timezones = commonTimezones
	}

	Resp(c).OK(timezones)
}

// SaveTimeConfig saves all time configuration at once
func (h *TimeHandler) SaveTimeConfig(c *gin.Context) {
	var req SaveTimeConfigRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request format")
		return
	}

	// Validate timezone
	if !h.isValidTimezone(req.Timezone) {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid timezone: "+req.Timezone)
		return
	}

	// Apply timezone if changed
	currentTZ := h.getTimezoneFast()
	if req.Timezone != currentTZ {
		if _, err := runCommandWithTimeout("timedatectl", "set-timezone", req.Timezone); err != nil {
			Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to set timezone: %v", err))
			return
		}
		if err := h.projectTimezone(c, req.Timezone); err != nil {
			logger.Warn("Failed to persist /etc/timezone: %v", err)
		}
	}

	// Determine NTP state based on sync_mode. When enabling, configure and
	// persistently start timesyncd directly instead of asking timedated to pick
	// a provider (the target image otherwise prefers the local-clock-only ntpd).
	ntpEnabled := req.SyncMode != "manual"

	// Compute interval for both timesyncd.conf and user config
	interval := req.NTPInterval
	if interval <= 0 {
		interval = 3600
	}

	// Apply NTP server + poll interval. The file projection
	// (/etc/systemd/timesyncd.conf) routes through the Config Controller when
	// available; the exec (enable+restart) follows below.
	if ntpEnabled {
		ntpServer := req.NTPServer
		if ntpServer == "" {
			ntpServer = h.getNTPServer() // preserve existing server
		}
		if err := h.projectNTPConfig(c, true, ntpServer, interval); err != nil {
			Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to apply NTP configuration: %v", err))
			return
		}
		if err := ensureTimesyncdEnabledAndRunning(true); err != nil {
			Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to apply NTP configuration: %v", err))
			return
		}

		// Verify the configured provider is actually running.
		time.Sleep(200 * time.Millisecond)
		if !serviceStateIs("is-active", "systemd-timesyncd.service") {
			Resp(c).FailMsg(CodeServiceError, "NTP configuration saved but no NTP service is running")
			return
		}
	}

	if !ntpEnabled {
		if err := h.projectNTPConfig(c, false, "", 0); err != nil {
			logger.Warn("Failed to record NTP-disabled desired-state: %v", err)
		}
		if err := disableNTPProviders(); err != nil {
			Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to disable NTP: %v", err))
			return
		}
	}

	if h.isNTPEnabled() != ntpEnabled {
		Resp(c).FailMsg(CodeServiceError, "NTP provider state does not match the saved configuration")
		return
	}

	timeChanged := false

	// Set manual time if sync_mode is manual and datetime provided
	if req.SyncMode == "manual" && req.ManualDatetime != "" {
		parsedTime, err := time.Parse(time.RFC3339, req.ManualDatetime)
		if err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, "Invalid datetime format, use RFC3339")
			return
		}
		timeStr := parsedTime.Format("2006-01-02 15:04:05")
		if _, err := runCommandWithTimeout("timedatectl", "set-time", timeStr); err != nil {
			Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to set time: %v", err))
			return
		}
		timeChanged = true
	}

	// Persist user preferences (time-config.json: time_format/sync_mode/
	// ntp_interval). Routes through the Config Controller when available.
	userCfg := timeUserConfig{
		TimeFormat:  req.TimeFormat,
		SyncMode:    req.SyncMode,
		NTPInterval: interval,
	}
	if err := h.projectTimeUserConfig(c, &userCfg); err != nil {
		logger.Warn("Failed to save time user config: %v", err)
	}

	h.persistCurrentTime()
	if timeChanged {
		h.syncMCURTCFromHostAsync("config_manual_set")
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"time.config.saved",
			eventLoggerPkg.MessageParams{
				"timezone":  req.Timezone,
				"sync_mode": req.SyncMode,
			},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OKMsg("Time configuration saved", nil)
}

// loadTimeUserConfig reads user preferences from disk
func (h *TimeHandler) loadTimeUserConfig() *timeUserConfig {
	cfg := &timeUserConfig{}
	configPath := h.configPath
	if configPath == "" {
		configPath = defaultTimeUserConfigPath
	}

	data, err := os.ReadFile(configPath)
	if err != nil {
		return cfg
	}

	_ = json.Unmarshal(data, cfg)
	return cfg
}

// saveTimeUserConfig writes user preferences to disk
func (h *TimeHandler) saveTimeUserConfig(cfg *timeUserConfig) error {
	configPath := h.configPath
	if configPath == "" {
		configPath = defaultTimeUserConfigPath
	}

	dir := filepath.Dir(configPath)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return err
	}

	data, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return err
	}

	return os.WriteFile(configPath, data, 0644)
}

// projectTimezone projects /etc/timezone through the Config Controller when
// available (revision history + audit + atomic write + auto-restore on verify
// failure), falling back to a direct os.WriteFile otherwise. timedatectl has
// already updated /etc/localtime; this keeps /etc/timezone consistent.
func (h *TimeHandler) projectTimezone(c *gin.Context, tz string) error {
	if h.configMgr != nil {
		desired, err := json.Marshal(struct {
			Timezone string `json:"timezone"`
		}{tz})
		if err != nil {
			return err
		}
		if _, _, err := h.configMgr.Apply(c.Request.Context(), "time", "timezone", string(desired), getUsernameFromContext(c)); err != nil {
			logger.Warn("time manager apply timezone failed, falling back to direct write: %v", err)
		} else {
			return nil
		}
	}
	return os.WriteFile("/etc/timezone", []byte(tz+"\n"), 0644)
}

// projectNTPConfig projects /etc/systemd/timesyncd.conf for an NTP change
// through the Config Controller when available, falling back to the legacy
// writeTimesyncdConfig (which also runs the service enable/restart) otherwise.
// When enabled is false the adapter records the desired state without writing
// the file; the caller is still responsible for disableNTPProviders(). The
// exec (ensureTimesyncdEnabledAndRunning / disableNTPProviders) is NOT done
// here — the caller drives it so the verify-then-restart ordering is preserved.
func (h *TimeHandler) projectNTPConfig(c *gin.Context, enabled bool, server string, interval int) error {
	type ntpDesired struct {
		Enabled  bool   `json:"enabled"`
		Server   string `json:"server"`
		Interval int    `json:"interval"`
	}
	if h.configMgr != nil {
		desired, err := json.Marshal(ntpDesired{enabled, server, interval})
		if err != nil {
			return err
		}
		if _, _, err := h.configMgr.Apply(c.Request.Context(), "time", "ntp", string(desired), getUsernameFromContext(c)); err != nil {
			if enabled {
				// Enabled needs the file on disk before the exec; fall back to
				// the legacy file+exec write so we do not leave timesyncd half
				// configured. (The caller will still call ensureTimesyncd...).
				logger.Warn("time manager apply ntp failed, falling back to legacy write: %v", err)
				return h.writeTimesyncdConfig(server, interval)
			}
			// Disabled: file projection is best-effort; return the error so the
			// caller can warn, but disableNTPProviders still runs.
			return err
		}
		return nil
	}
	if !enabled {
		return nil
	}
	return h.writeTimesyncdConfig(server, interval)
}

// projectTimeUserConfig projects time-config.json through the Config Controller
// when available, falling back to the direct saveTimeUserConfig write otherwise.
func (h *TimeHandler) projectTimeUserConfig(c *gin.Context, cfg *timeUserConfig) error {
	if h.configMgr != nil {
		desired, err := json.Marshal(struct {
			TimeFormat  string `json:"time_format"`
			SyncMode    string `json:"sync_mode"`
			NTPInterval int    `json:"ntp_interval"`
		}{cfg.TimeFormat, cfg.SyncMode, cfg.NTPInterval})
		if err != nil {
			return err
		}
		if _, _, err := h.configMgr.Apply(c.Request.Context(), "time", "user_config", string(desired), getUsernameFromContext(c)); err != nil {
			logger.Warn("time manager apply user_config failed, falling back to direct write: %v", err)
		} else {
			return nil
		}
	}
	return h.saveTimeUserConfig(cfg)
}

func (h *TimeHandler) projectNTPUserConfig(c *gin.Context, enabled bool, interval int) error {
	cfg := h.loadTimeUserConfig()
	if cfg.TimeFormat == "" {
		cfg.TimeFormat = "24h"
	}
	if enabled {
		cfg.SyncMode = "ntp"
	} else {
		cfg.SyncMode = "manual"
	}
	if interval > 0 {
		cfg.NTPInterval = interval
	} else if cfg.NTPInterval <= 0 {
		cfg.NTPInterval = defaultNTPPollMaxSec
	}
	return h.projectTimeUserConfig(c, cfg)
}

func (h *TimeHandler) syncMCURTCFromHostAsync(reason string) {
	if h.mcuRaw == nil {
		logger.Warn("MCU RTC sync skipped after %s: camera-control client is not available", reason)
		return
	}

	payload, err := mcuRTCPayloadFromTime(time.Now())
	if err != nil {
		logger.Warn("MCU RTC sync skipped after %s: %v", reason, err)
		return
	}

	h.mcuSyncMu.Lock()
	if h.mcuSyncInFlight {
		h.mcuSyncMu.Unlock()
		logger.Info("MCU RTC sync already in flight; skip duplicate request after %s", reason)
		return
	}
	h.mcuSyncInFlight = true
	h.mcuSyncMu.Unlock()

	go h.runMCURTCSync(reason, payload)
}

func (h *TimeHandler) runMCURTCSync(reason string, payload []byte) {
	defer func() {
		h.mcuSyncMu.Lock()
		h.mcuSyncInFlight = false
		h.mcuSyncMu.Unlock()
	}()

	ctx, cancel := context.WithTimeout(context.Background(), mcuRTCSyncTimeout)
	defer cancel()

	resp, err := h.mcuRaw.McuRawRequest(ctx, &camerapb.McuRawRequestMessage{
		Cmd:     hostLinkCmdRTCSet,
		Payload: payload,
	})
	if err != nil {
		logger.Warn("MCU RTC sync failed after %s: %v", reason, err)
		return
	}
	if resp == nil {
		logger.Warn("MCU RTC sync failed after %s: empty camera-control response", reason)
		return
	}
	if !resp.GetSuccess() {
		logger.Warn("MCU RTC sync failed after %s: %s (hal=%d)", reason, resp.GetMessage(), resp.GetHalCode())
		return
	}

	logger.Info("MCU RTC synced from host after %s", reason)
}

func mcuRTCPayloadFromTime(t time.Time) ([]byte, error) {
	utc := t.UTC()
	year := utc.Year()
	if year < minPlausibleMCURTCYear {
		return nil, fmt.Errorf("host year=%d is not plausible", year)
	}
	if year > maxTwoDigitMCURTCYear {
		return nil, fmt.Errorf("host year=%d exceeds MCU RTC two-digit year range", year)
	}

	return []byte{
		byte(year % 100),
		byte(utc.Month()),
		byte(utc.Day()),
		goWeekdayToMCU(utc.Weekday()),
		byte(utc.Hour()),
		byte(utc.Minute()),
		byte(utc.Second()),
	}, nil
}

func goWeekdayToMCU(w time.Weekday) byte {
	if w == time.Sunday {
		return 7
	}
	if w >= time.Monday && w <= time.Saturday {
		return byte(w)
	}
	return 1
}

// Helper functions

func (h *TimeHandler) getTimezone() string {
	out, err := runCommandWithTimeout("timedatectl", "show", "--property", "Timezone", "--value")
	if err != nil {
		return "UTC"
	}
	return out
}

func (h *TimeHandler) getTimezoneFast() string {
	// Resolve from /etc/localtime symlink (most reliable across distros)
	link, err := os.Readlink("/etc/localtime")
	if err == nil {
		// Typical: ../usr/share/zoneinfo/Asia/Shanghai or /usr/share/zoneinfo/Asia/Shanghai
		parts := strings.Split(link, "zoneinfo/")
		if len(parts) == 2 {
			tz := strings.TrimSpace(parts[1])
			if tz != "" {
				return tz
			}
		}
	}

	// Fallback: /etc/timezone (may be stale on some embedded systems)
	data, err := os.ReadFile("/etc/timezone")
	if err == nil && len(data) > 0 {
		tz := strings.TrimSpace(string(data))
		if tz != "" {
			return tz
		}
	}

	return h.getTimezone()
}

func (h *TimeHandler) isNTPEnabled() bool {
	return serviceStateIs("is-enabled", "systemd-timesyncd.service")
}

func (h *TimeHandler) isNTPSynced() bool {
	out, err := runCommandWithTimeout("timedatectl", "show", "--property", "NTPSynchronized", "--value")
	if err != nil {
		return false
	}
	return strings.TrimSpace(out) == "yes"
}

func (h *TimeHandler) getNTPServer() string {
	// Try to read from timesyncd.conf
	data, err := os.ReadFile("/etc/systemd/timesyncd.conf")
	if err == nil {
		lines := strings.Split(string(data), "\n")
		for _, line := range lines {
			line = strings.TrimSpace(line)
			if strings.HasPrefix(line, "NTP=") {
				return strings.TrimPrefix(line, "NTP=")
			}
		}
	}

	// Check timedatectl output
	out, err := runCommandWithTimeout("timedatectl", "show", "--property", "NTPServer", "--value")
	if err != nil {
		return "pool.ntp.org"
	}
	result := strings.TrimSpace(out)
	if result == "" {
		return "pool.ntp.org"
	}
	return result
}

// writeTimesyncdConfig writes NTP server and poll interval settings to
// /etc/systemd/timesyncd.conf in a single pass, then restarts the service once.
// This replaces the old setNTPServer + setNTPPollInterval pair which caused
// multiple rapid restarts and could silently skip settings if [Time] was missing.
func (h *TimeHandler) writeTimesyncdConfig(server string, interval int) error {
	const configPath = "/etc/systemd/timesyncd.conf"

	data, err := os.ReadFile(configPath)
	if err != nil {
		// Config file doesn't exist — create a minimal one
		data = []byte("[Time]\n")
	}

	updated, err := updateTimesyncdConfig(data, server, interval)
	if err != nil {
		return err
	}

	if err := os.WriteFile(configPath, updated, 0644); err != nil {
		return fmt.Errorf("failed to write timesyncd.conf: %w", err)
	}

	return ensureTimesyncdEnabledAndRunning(true)
}

// updateTimesyncdConfig applies the user-selected maximum poll interval while
// retaining a short initial poll interval. systemd-timesyncd requires Max > Min
// and starts polling at Min, so writing the same value to both fields can delay
// or invalidate synchronization after boot.
func updateTimesyncdConfig(data []byte, server string, interval int) ([]byte, error) {
	lines := strings.Split(string(data), "\n")
	pollMax := interval
	if pollMax <= 0 {
		pollMax = existingPollMax(lines)
	}
	if pollMax <= timesyncdPollMinSec {
		pollMax = defaultNTPPollMaxSec
	}
	pollMinStr := strconv.Itoa(timesyncdPollMinSec)
	pollMaxStr := strconv.Itoa(pollMax)

	// Track what we've updated
	foundNTP := false
	foundPollMin := false
	foundPollMax := false
	hasTimeSection := false
	timeSectionIdx := -1

	for i, line := range lines {
		trimmed := strings.TrimSpace(line)

		// Track [Time] section position
		if trimmed == "[Time]" {
			hasTimeSection = true
			timeSectionIdx = i
		}

		// Update existing uncommented entries
		if server != "" && strings.HasPrefix(trimmed, "NTP=") {
			lines[i] = "NTP=" + server
			foundNTP = true
		} else if strings.HasPrefix(trimmed, "PollIntervalMinSec=") {
			lines[i] = "PollIntervalMinSec=" + pollMinStr
			foundPollMin = true
		} else if strings.HasPrefix(trimmed, "PollIntervalMaxSec=") {
			lines[i] = "PollIntervalMaxSec=" + pollMaxStr
			foundPollMax = true
		}
	}

	// Collect lines that need to be inserted
	missing := []string{}
	if server != "" && !foundNTP {
		missing = append(missing, "NTP="+server)
	}
	if !foundPollMin {
		missing = append(missing, "PollIntervalMinSec="+pollMinStr)
	}
	if !foundPollMax {
		missing = append(missing, "PollIntervalMaxSec="+pollMaxStr)
	}

	if len(missing) > 0 {
		if !hasTimeSection {
			// No [Time] section found — append one at the end
			lines = append(lines, "[Time]")
			timeSectionIdx = len(lines) - 1
		}
		// Insert missing lines right after [Time]
		insertIdx := timeSectionIdx + 1
		tail := make([]string, len(lines[insertIdx:]))
		copy(tail, lines[insertIdx:])
		lines = append(lines[:insertIdx], missing...)
		lines = append(lines, tail...)
	}

	return []byte(strings.Join(lines, "\n")), nil
}

func existingPollMax(lines []string) int {
	for _, line := range lines {
		trimmed := strings.TrimSpace(line)
		if !strings.HasPrefix(trimmed, "PollIntervalMaxSec=") {
			continue
		}
		value, err := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(trimmed, "PollIntervalMaxSec=")))
		if err == nil && value > timesyncdPollMinSec {
			return value
		}
	}
	return defaultNTPPollMaxSec
}

func ensureTimesyncdEnabledAndRunning(restart bool) error {
	// timedated selects providers in lexical order from ntp-units.d. Explicitly
	// put timesyncd first so an external `timedatectl set-ntp true` cannot switch
	// the device back to ntpd, whose stock config only uses the local clock.
	if err := os.MkdirAll(filepath.Dir(timesyncdProviderPath), 0755); err != nil {
		return fmt.Errorf("create NTP provider directory: %w", err)
	}
	if err := os.WriteFile(timesyncdProviderPath, []byte("systemd-timesyncd.service\n"), 0644); err != nil {
		return fmt.Errorf("select systemd-timesyncd provider: %w", err)
	}
	if err := disableConflictingNTPProviders(); err != nil {
		return err
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if output, err := exec.CommandContext(ctx, "systemctl", "enable", "systemd-timesyncd.service").CombinedOutput(); err != nil {
		return fmt.Errorf("enable systemd-timesyncd: %w (%s)", err, strings.TrimSpace(string(output)))
	}
	if !serviceStateIs("is-enabled", "systemd-timesyncd.service") {
		return fmt.Errorf("systemd-timesyncd is not enabled after systemctl enable")
	}

	action := "start"
	if restart {
		action = "restart"
	}
	ctx, cancel = context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if output, err := exec.CommandContext(ctx, "systemctl", action, "systemd-timesyncd.service").CombinedOutput(); err != nil {
		return fmt.Errorf("%s systemd-timesyncd: %w (%s)", action, err, strings.TrimSpace(string(output)))
	}
	if !serviceStateIs("is-active", "systemd-timesyncd.service") {
		return fmt.Errorf("systemd-timesyncd is not active after %s", action)
	}
	return nil
}

func disableNTPProviders() error {
	if err := disableConflictingNTPProviders(); err != nil {
		return err
	}
	return systemctlUnit("disable", "--now", "systemd-timesyncd.service")
}

func disableConflictingNTPProviders() error {
	for _, unit := range []string{"ntpd.service", "chronyd.service"} {
		if !systemdUnitExists(unit) {
			continue
		}
		if err := systemctlUnit("disable", "--now", unit); err != nil {
			return fmt.Errorf("disable conflicting NTP provider %s: %w", unit, err)
		}
	}
	return nil
}

func systemdUnitExists(name string) bool {
	ctx, cancel := context.WithTimeout(context.Background(), commandTimeout)
	defer cancel()
	out, err := exec.CommandContext(ctx, "systemctl", "show", "--property", "LoadState", "--value", name).Output()
	return err == nil && strings.TrimSpace(string(out)) != "not-found"
}

func systemctlUnit(args ...string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	output, err := exec.CommandContext(ctx, "systemctl", args...).CombinedOutput()
	if err != nil {
		return fmt.Errorf("systemctl %s: %w (%s)", strings.Join(args, " "), err, strings.TrimSpace(string(output)))
	}
	return nil
}

func serviceStateIs(action, name string) bool {
	ctx, cancel := context.WithTimeout(context.Background(), commandTimeout)
	defer cancel()
	return exec.CommandContext(ctx, "systemctl", action, "--quiet", name).Run() == nil
}

func (h *TimeHandler) getLastNTPSync() string {
	// Get the last NTP sync time from systemd-timesyncd status
	// Use longer context for this command
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()

	cmd := exec.CommandContext(ctx, "timedatectl", "timesync-status")
	var out bytes.Buffer
	cmd.Stdout = &out
	if err := cmd.Run(); err != nil {
		return ""
	}

	// Parse output to extract last sync time
	lines := strings.Split(out.String(), "\n")
	for _, line := range lines {
		if strings.Contains(line, "Server time") {
			// Extract time from line like "Server time: Tue 2025-04-03 10:30:00 CST"
			parts := strings.Fields(line)
			if len(parts) >= 6 {
				return fmt.Sprintf("%s %s %s %s", parts[2], parts[3], parts[4], parts[5])
			}
		}
	}

	return ""
}

func (h *TimeHandler) getNTPFallbackServers() []string {
	// Return default fallback servers
	return []string{
		"0.pool.ntp.org",
		"1.pool.ntp.org",
		"2.pool.ntp.org",
		"3.pool.ntp.org",
	}
}

func (h *TimeHandler) getUptime() uint64 {
	// Read uptime from /proc/uptime
	data, err := os.ReadFile("/proc/uptime")
	if err != nil {
		return 0
	}

	fields := strings.Fields(string(data))
	if len(fields) == 0 {
		return 0
	}

	uptimeFloat, err := strconv.ParseFloat(fields[0], 64)
	if err != nil {
		return 0
	}

	return uint64(uptimeFloat)
}

func (h *TimeHandler) getSystemTimezones() []TimezoneData {
	// Check cache first — avoid repeated timedatectl calls
	h.tzCacheMu.Lock()
	if h.tzCache != nil && time.Since(h.tzCacheTime) < 1*time.Hour {
		cached := h.tzCache
		h.tzCacheMu.Unlock()
		return cached
	}
	h.tzCacheMu.Unlock()

	// Try to get all timezones from system
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	cmd := exec.CommandContext(ctx, "timedatectl", "list-timezones")
	var out bytes.Buffer
	var stderr bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = &stderr

	if err := cmd.Run(); err != nil {
		logger.Warn("Failed to get system timezones: %v, stderr: %s", err, stderr.String())
		return nil
	}

	// Log any stderr output (warnings, etc.)
	if stderrStr := stderr.String(); stderrStr != "" {
		logger.Debug("timedatectl list-timezones stderr: %s", strings.TrimSpace(stderrStr))
	}

	var timezones []TimezoneData
	lines := strings.Split(out.String(), "\n")

	for _, line := range lines {
		tzName := strings.TrimSpace(line)
		if tzName == "" {
			continue
		}

		// Skip lines that look like error messages or non-timezone data
		// Valid timezone names are like "Area/Location" (e.g., "Asia/Shanghai")
		if !strings.Contains(tzName, "/") {
			// Skip non-timezone entries (error messages, etc.)
			logger.Debug("Skipping invalid timezone entry: %s", tzName)
			continue
		}

		// Get offset for this timezone
		loc, err := time.LoadLocation(tzName)
		if err != nil {
			logger.Debug("Failed to load timezone %s: %v", tzName, err)
			continue
		}

		now := time.Now().In(loc)
		_, offset := now.Zone()

		// Format offset
		offsetStr := "UTC"
		if offset != 0 {
			sign := "+"
			if offset < 0 {
				sign = "-"
				offset = -offset
			}
			hours := offset / 3600
			minutes := (offset % 3600) / 60
			offsetStr = fmt.Sprintf("UTC%s%02d:%02d", sign, hours, minutes)
		}

		// Extract country from timezone name
		country := "Unknown"
		parts := strings.Split(tzName, "/")
		if len(parts) > 1 {
			country = strings.ReplaceAll(parts[0], "_", " ")
		}

		timezones = append(timezones, TimezoneData{
			Name:      tzName,
			Country:   country,
			Offset:    offsetStr,
			OffsetSec: offset,
		})
	}

	// Update cache
	h.tzCacheMu.Lock()
	h.tzCache = timezones
	h.tzCacheTime = time.Now()
	h.tzCacheMu.Unlock()

	return timezones
}

func (h *TimeHandler) isValidTimezone(tz string) bool {
	// Check against system timezones first
	timezones := h.getSystemTimezones()

	// Always check commonTimezones as supplement (covers short names like "UTC" vs "Etc/UTC")
	allTimezones := append(timezones, commonTimezones...)

	for _, t := range allTimezones {
		if t.Name == tz {
			return true
		}
	}

	return false
}

func formatUptime(seconds uint64) string {
	days := seconds / 86400
	hours := (seconds % 86400) / 3600
	minutes := (seconds % 3600) / 60

	if days > 0 {
		return fmt.Sprintf("%d days %d hours", days, hours)
	}
	if hours > 0 {
		return fmt.Sprintf("%d hours %d minutes", hours, minutes)
	}
	return fmt.Sprintf("%d minutes", minutes)
}

// persistCurrentTime saves the current system time to disk for recovery after reboot
func (h *TimeHandler) persistCurrentTime() {
	now := time.Now()
	tz := h.getTimezoneFast()

	entry := lastKnownTime{
		UnixTimestamp: now.Unix(),
		Timezone:      tz,
		SavedAt:       now.Format(time.RFC3339),
	}

	data, err := json.MarshalIndent(entry, "", "  ")
	if err != nil {
		logger.Warn("Failed to marshal last-known-time: %v", err)
		return
	}

	dir := filepath.Dir(defaultLastKnownTimePath)
	if err := os.MkdirAll(dir, 0755); err != nil {
		logger.Warn("Failed to create dir for last-known-time: %v", err)
		return
	}

	if err := os.WriteFile(defaultLastKnownTimePath, data, 0644); err != nil {
		logger.Warn("Failed to write last-known-time: %v", err)
	}
}

// RestoreTimeOnBoot restores system time from the persisted file if system time is stale
func (h *TimeHandler) RestoreTimeOnBoot() {
	data, err := os.ReadFile(defaultLastKnownTimePath)
	if err != nil {
		logger.Debug("No last-known-time file found, skipping boot restore")
		return
	}

	var entry lastKnownTime
	if err := json.Unmarshal(data, &entry); err != nil {
		logger.Warn("Failed to parse last-known-time: %v", err)
		return
	}

	if entry.UnixTimestamp <= 0 {
		return
	}

	// If system time is before the saved time, restore it
	now := time.Now().Unix()
	if now < entry.UnixTimestamp {
		savedTime := time.Unix(entry.UnixTimestamp, 0)
		timeStr := savedTime.Format("2006-01-02 15:04:05")
		logger.Info("System time (%d) is before saved time (%d), restoring to %s", now, entry.UnixTimestamp, timeStr)

		if err := disableConflictingNTPProviders(); err != nil {
			logger.Warn("Failed to disable conflicting NTP provider for time restore: %v", err)
		}
		if err := systemctlUnit("stop", "systemd-timesyncd.service"); err != nil {
			logger.Warn("Failed to pause timesyncd for time restore: %v", err)
		}
		userCfg := h.loadTimeUserConfig()
		if userCfg.SyncMode != "manual" {
			defer func() {
				if err := ensureTimesyncdEnabledAndRunning(false); err != nil {
					logger.Warn("Failed to re-enable NTP after time restore: %v", err)
				}
			}()
		}

		if _, err := runCommandWithTimeout("timedatectl", "set-time", timeStr); err != nil {
			logger.Warn("Failed to restore time: %v", err)
			return
		}

		logger.Info("System time restored to %s", timeStr)

		// Restore timezone if set
		if entry.Timezone != "" && entry.Timezone != h.getTimezone() {
			if _, err := runCommandWithTimeout("timedatectl", "set-timezone", entry.Timezone); err != nil {
				logger.Warn("Failed to restore timezone: %v", err)
			}
		}
	} else {
		logger.Debug("System time is current, no restore needed")
	}
}

// StartTimePersistLoop periodically saves the current time to disk with graceful shutdown
func (h *TimeHandler) StartTimePersistLoop(ctx context.Context) {
	h.persistCurrentTime()

	ticker := time.NewTicker(5 * time.Minute)
	defer ticker.Stop()

	for {
		select {
		case <-ticker.C:
			h.persistCurrentTime()
		case <-ctx.Done():
			logger.Info("Time persist loop stopped")
			h.persistCurrentTime()
			return
		}
	}
}

// SyncFromClientRequest for syncing time from browser
type SyncFromClientRequest struct {
	ClientTimestamp int64 `json:"client_timestamp" binding:"required"` // Unix seconds
}

// SyncFromClient syncs device time from client (browser) if significantly off
func (h *TimeHandler) SyncFromClient(c *gin.Context) {
	var req SyncFromClientRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request format")
		return
	}

	now := time.Now().Unix()
	diff := req.ClientTimestamp - now
	if diff < 0 {
		diff = -diff
	}

	// Only sync if difference is more than 30 seconds
	if diff <= 30 {
		Resp(c).OK(map[string]any{
			"synced":       false,
			"diff_seconds": diff,
			"message":      "Time is within acceptable range",
		})
		return
	}

	// Disable NTP temporarily to set time
	wasNTPEnabled := h.isNTPEnabled()
	if wasNTPEnabled {
		if err := systemctlUnit("stop", "systemd-timesyncd.service"); err != nil {
			logger.Warn("Failed to pause timesyncd for client sync: %v", err)
		}
		defer func() {
			if err := ensureTimesyncdEnabledAndRunning(false); err != nil {
				logger.Warn("Failed to re-enable NTP after client sync: %v", err)
			}
		}()
	}

	clientTime := time.Unix(req.ClientTimestamp, 0)
	timeStr := clientTime.Format("2006-01-02 15:04:05")
	if _, err := runCommandWithTimeout("timedatectl", "set-time", timeStr); err != nil {
		Resp(c).FailMsg(CodeServiceError, fmt.Sprintf("Failed to set time: %v", err))
		return
	}

	// Persist the synced time
	h.persistCurrentTime()
	h.syncMCURTCFromHostAsync("client_sync")

	logger.Info("Time synced from client: %s (diff=%ds)", timeStr, diff)

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"time.client_sync",
			eventLoggerPkg.MessageParams{"diff_seconds": diff},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(map[string]any{
		"synced":       true,
		"diff_seconds": diff,
		"message":      fmt.Sprintf("Time synced from browser (diff: %ds)", diff),
	})
}
