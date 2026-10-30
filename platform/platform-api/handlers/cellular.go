// Cellular HTTP + runtime (see file list in this directory).
// Probe contract: device-control.yaml capabilities.cellular.hw_profile →
// cellular-hw.yaml. Capability is USB self-check, not a static YAML flag.
// AT dialects stay in cellular_at.go / cellular_identity.go.
package handlers

import (
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	eventLoggerPkg "aipc/platform/common/events"
	"aipc/platform/common/logger"
	"aipc/platform/platform-api/config"
	"aipc/platform/platform-api/internal/atomicfile"
	"github.com/gin-gonic/gin"
)

// CellularConfig is persisted user dial settings (APN/auth/usbnet/RAT).
// Hardware binding lives in CellularHWProfile YAML, not here.
type CellularConfig struct {
	Enabled        bool   `json:"enabled"`
	APN            string `json:"apn"`
	User           string `json:"user"`
	Password       string `json:"password"`
	Auth           string `json:"auth"`            // none|pap|chap
	IPType         string `json:"ip_type"`         // ipv4|ipv6|ipv4v6
	PDPCID         int    `json:"pdp_cid"`         // 1..16
	CarrierProfile string `json:"carrier_profile"` // generic|verizon
	Usbnet         string `json:"usbnet"`          // qmi|ecm|mbim
	NetworkMode    string `json:"network_mode"`    // auto|lte|wcdma
	Roaming        *bool  `json:"roaming"`         // nil → false
	PIN            string `json:"pin"`
}

// CellularStatus is the runtime view returned by GET /cellular/status.
type CellularStatus struct {
	ModulePresent bool   `json:"module_present"`
	Module        string `json:"module,omitempty"` // USB sysfs product when present
	Platform      string `json:"platform,omitempty"`
	SIMReady      bool   `json:"sim_ready"`
	Operator      string `json:"operator"`
	Network       string `json:"network,omitempty"`
	IMEI          string `json:"imei,omitempty"`
	ICCID         string `json:"iccid,omitempty"`
	IMSI          string `json:"imsi,omitempty"`
	MSISDN        string `json:"msisdn,omitempty"`
	Firmware      string `json:"firmware,omitempty"`
	SignalRSRP    *int   `json:"signal_rsrp"`
	SignalRSRQ    *int   `json:"signal_rsrq,omitempty"`
	RegState      string `json:"reg_state"`
	Connected     bool   `json:"connected"`
	Iface         string `json:"iface"`
	IPv4          string `json:"ipv4"`
	IPv6          string `json:"ipv6"`
	Message       string `json:"message"`
	Stub          bool   `json:"stub"`
	DialerReady   bool   `json:"dialer_ready"`
	Supported     bool   `json:"supported"`
	Capability    string `json:"capability"` // pending|supported|unsupported
}

// CellularHandler serves /api/v1/cellular/* endpoints.
type CellularHandler struct {
	eventLogger *eventLoggerPkg.Logger
	configMgr   *config.Manager
	hw          *CellularHWProfile
	hwPath      string
	configPath  string
	runDir      string
	sysfsUSB    string
	sysfsNet    string
	lookPath    func(string) (string, error)
	command     func(name string, arg ...string) *exec.Cmd
	now         func() time.Time

	mu           sync.Mutex
	dialerCmd    *exec.Cmd
	pwrHoldCmd   *exec.Cmd
	pwrHoldLevel *bool // level held by pwrHoldCmd (true=powered)

	configMu sync.Mutex

	autoDialStarted  int32
	autoDialKick     chan struct{}
	usbnetApplyBusy  int32 // blocks autodial while usbnet/CFUN resets
	dialFailUntil    time.Time
	dialBackoff      time.Duration
	simStateKnown    bool
	lastSIMReady     bool
	simAbsentSince   time.Time
	simRecoverAt     time.Time
	sessionWaitSince time.Time
	dialerStopExpect int32 // intentional stop; skip short-exit backoff
	lastIdentityMu   sync.Mutex
	lastIdentity     modemIdentity
	moduleSeenMu     sync.Mutex
	moduleSeenAt     time.Time
	lastModuleName   string

	capMu      sync.Mutex
	capability string
}

// NewCellularHandler creates a CellularHandler.
// hwProfilePath is the YAML describing USB/GPIO/ifaces/dialer for this board.
// configMgr may be nil when the Config Controller is disabled (no DB).
// When the profile cannot be loaded, APIs still respond but probing/power are inert.
func NewCellularHandler(eventLogger *eventLoggerPkg.Logger, hwProfilePath string, configMgr *config.Manager) *CellularHandler {
	h := &CellularHandler{
		eventLogger: eventLogger,
		configMgr:   configMgr,
		hwPath:      hwProfilePath,
		lookPath:    exec.LookPath,
		command:     exec.Command,
		now:         time.Now,
	}
	if hw, err := LoadCellularHWProfile(hwProfilePath); err != nil {
		logger.Warn("cellular: hw profile %q not loaded: %v (cellular APIs inert until profile is fixed)", hwProfilePath, err)
		setCellularProbePrefs(nil)
	} else {
		h.applyHW(hw)
	}
	h.initCapabilityState()
	return h
}

func (h *CellularHandler) applyHW(hw *CellularHWProfile) {
	h.hw = hw
	h.configPath = hw.Paths.UserConfig
	h.runDir = hw.Paths.RunDir
	h.sysfsUSB = hw.Paths.SysfsUSB
	h.sysfsNet = hw.Paths.SysfsNet
	setCellularProbePrefs(hw)
}

func defaultCellularConfig() CellularConfig {
	return CellularConfig{
		Enabled:        true,
		APN:            "",
		User:           "",
		Password:       "",
		Auth:           "none",
		IPType:         "ipv4v6",
		PDPCID:         1,
		CarrierProfile: "generic",
		Usbnet:         "mbim",
		NetworkMode:    "auto",
		Roaming:        boolPtr(false),
		PIN:            "",
	}
}

func boolPtr(v bool) *bool { return &v }

func roamingEnabled(cfg CellularConfig) bool {
	if cfg.Roaming == nil {
		return false
	}
	return *cfg.Roaming
}

func (h *CellularHandler) GetStatus(c *gin.Context) {
	cfg, _ := h.loadConfig()
	Resp(c).OK(h.probeStatus(cfg))
}

// CellularCapabilityView is for GET /cellular/capability (sysfs only, no AT).
type CellularCapabilityView struct {
	Capability    string `json:"capability"`
	ModulePresent bool   `json:"module_present"`
}

func (h *CellularHandler) GetCapability(c *gin.Context) {
	Resp(c).OK(h.capabilityView())
}

// GetConfig returns the persisted cellular configuration.
func (h *CellularHandler) GetConfig(c *gin.Context) {
	cfg, err := h.loadConfig()
	if err != nil {
		logger.Warn("cellular: load config: %v", err)
		Resp(c).OK(defaultCellularConfig())
		return
	}
	Resp(c).OK(cfg)
}

// UpdateConfig validates and persists cellular configuration.
func (h *CellularHandler) UpdateConfig(c *gin.Context) {
	var req CellularConfig
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidJSON, err.Error())
		return
	}
	if err := validateCellularConfig(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidParameter, err.Error())
		return
	}

	h.configMu.Lock()
	defer h.configMu.Unlock()

	prev, _ := h.loadConfig()
	_, cfgStatErr := os.Stat(h.configPath)
	configExists := cfgStatErr == nil
	// Treat first-ever save (no user config yet) as an enable attempt too —
	// defaultCellularConfig().Enabled is true, which would otherwise skip the
	// module-present gate.
	enabling := req.Enabled && (!prev.Enabled || !configExists)
	usbnetChanged := prev.Usbnet != req.Usbnet
	modemPrefChanged := cellularModemPrefsChanged(prev, req)
	dialChanged := cellularDialSettingsChanged(prev, req)

	if req.Enabled && h.capabilityState() == cellularCapUnsupported {
		Resp(c).FailMsg(CodeServiceError,
			"Cellular hardware is not available on this device.")
		return
	}

	if err := h.projectCellularConfig(c, req); err != nil {
		Resp(c).FailMsg(CodeServiceError, "failed to save cellular config: "+err.Error())
		return
	}

	var powerErr error
	if req.Enabled {
		powerErr = h.ensurePower(true)
		if powerErr == nil && enabling {
			timeout := 25 * time.Second
			if h.hw != nil && h.hw.GPIO.ModuleDetectTimeoutSec > 0 {
				timeout = time.Duration(h.hw.GPIO.ModuleDetectTimeoutSec) * time.Second
			}
			if !h.waitForModule(timeout) {
				logger.Warn("cellular: enable aborted — module not on USB after %s", timeout)
				req.Enabled = false
				_ = h.projectCellularConfig(c, req)
				_ = h.stopDialer()
				clearATCache()
				_ = h.ensurePower(false)
				Resp(c).FailMsg(CodeServiceError,
					"No cellular module detected. Cellular has been turned off.")
				return
			}
			if _, _, path := h.findMatchedUSB(); path != "" {
				h.noteModuleSeen(readUSBProduct(path))
			} else {
				h.noteModuleSeen("")
			}
			h.clearStickyIdentity()
			clearATCache()
			h.settleAfterPowerOn()
		}
	} else {
		_ = h.stopDialer()
		clearATCache()
		h.clearStickyIdentity()
		powerErr = h.ensurePower(false)
	}

	var usbnetErr error
	var modemPrefErr error
	redial := false
	wantUsbnet := strings.ToLower(strings.TrimSpace(req.Usbnet))
	liveUsbnet := probeUsbnetDriver()
	moduleVID, _, _ := h.findMatchedUSB()
	needUsbnetApply := req.Enabled && powerErr == nil && wantUsbnet != "" && moduleVID != "" &&
		((usbnetChanged && liveUsbnet != wantUsbnet) || (liveUsbnet != "" && liveUsbnet != wantUsbnet))
	if needUsbnetApply {
		atomic.StoreInt32(&h.usbnetApplyBusy, 1)
		_ = h.stopDialer()
		usbnetErr = h.applyUsbnetMode(req.Usbnet)
		if usbnetErr != nil {
			atomic.StoreInt32(&h.usbnetApplyBusy, 0)
			logger.Warn("cellular: apply usbnet=%s: %v", req.Usbnet, usbnetErr)
		} else {
			_ = waitUsbnetTransition(liveUsbnet, wantUsbnet, 90*time.Second)
			_ = waitForATPort(60 * time.Second)
			atomic.StoreInt32(&h.usbnetApplyBusy, 0)
			redial = true
		}
	} else if usbnetChanged && req.Enabled && powerErr == nil && liveUsbnet == wantUsbnet {
		logger.Info("cellular: usbnet=%s already active; skip AT apply", wantUsbnet)
		redial = true
	}

	if modemPrefChanged && req.Enabled && powerErr == nil && usbnetErr == nil {
		_ = h.stopDialer()
		if moduleVID != "" && h.hasModemControl() {
			modemPrefErr = h.applyModemPrefs(req)
			if modemPrefErr != nil {
				logger.Warn("cellular: apply modem prefs: %v", modemPrefErr)
			}
		} else if moduleVID == "" {
			logger.Info("cellular: modem prefs saved; module not on USB yet")
		} else {
			logger.Warn("cellular: modem prefs saved; AT port not ready — apply on next dial")
		}
		redial = true
	} else if dialChanged && req.Enabled && powerErr == nil {
		_ = h.stopDialer()
		redial = true
	} else if req.Enabled && powerErr == nil && !redial {
		redial = true
	}

	if req.Enabled && powerErr == nil && (redial || usbnetErr != nil || modemPrefErr != nil) {
		h.kickAutoDial()
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("network.cellular.config", eventLoggerPkg.MessageParams{
			"enabled":      req.Enabled,
			"apn":          req.APN,
			"profile":      req.CarrierProfile,
			"usbnet":       req.Usbnet,
			"network_mode": req.NetworkMode,
			"roaming":      roamingEnabled(req),
		}, getUsernameFromContext(c))
	}

	if powerErr != nil {
		logger.Warn("cellular: power after config save: %v", powerErr)
		Resp(c).FailMsg(CodeServiceError,
			"Config saved, but GPIO power failed — line busy. Stop ct-cell (or kill foreign gpioset) and retry.")
		return
	}
	if usbnetErr != nil {
		Resp(c).FailMsg(CodeServiceError,
			"Config saved, but applying USB network mode failed: "+usbnetErr.Error())
		return
	}
	if modemPrefErr != nil {
		Resp(c).FailMsg(CodeServiceError,
			"Config saved, but applying network mode/roaming failed: "+modemPrefErr.Error())
		return
	}
	Resp(c).OK(req)
}

// Connect powers the module (if enabled) and starts the dialer when available.
func (h *CellularHandler) Connect(c *gin.Context) {
	cfg, err := h.loadConfig()
	if err != nil {
		cfg = defaultCellularConfig()
	}
	if !cfg.Enabled {
		Resp(c).OK(gin.H{"ok": false, "message": "Enable cellular before connecting."})
		return
	}
	if h.capabilityState() == cellularCapUnsupported {
		Resp(c).OK(gin.H{"ok": false, "message": "Cellular hardware is not available on this device."})
		return
	}
	if h.hw == nil {
		Resp(c).OK(gin.H{"ok": false, "message": "Cellular hardware profile is not configured."})
		return
	}

	if err := h.ensurePower(true); err != nil {
		Resp(c).OK(gin.H{"ok": false, "message": "Failed to power module: " + err.Error()})
		return
	}

	timeout := time.Duration(h.hw.GPIO.ModuleDetectTimeoutSec) * time.Second
	if !h.waitForModule(timeout) {
		Resp(c).OK(gin.H{
			"ok":      false,
			"message": "Module not detected on USB. Check insertion, drivers, and GPIO power.",
		})
		return
	}

	dialer, err := h.findDialer()
	if err != nil {
		Resp(c).OK(gin.H{
			"ok":      false,
			"message": "Cellular dialer is not available on this firmware yet.",
		})
		return
	}

	if err := h.startDialer(dialer, cfg); err != nil {
		Resp(c).OK(gin.H{"ok": false, "message": "Dialer start failed: " + err.Error()})
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("network.cellular.connect", eventLoggerPkg.MessageParams{
			"apn": cfg.APN,
		}, getUsernameFromContext(c))
	}
	Resp(c).OK(gin.H{"ok": true, "message": "Dialer started."})
}

func cellularDialSettingsChanged(prev, next CellularConfig) bool {
	return prev.APN != next.APN ||
		prev.User != next.User ||
		prev.Password != next.Password ||
		prev.Auth != next.Auth ||
		prev.IPType != next.IPType ||
		prev.PDPCID != next.PDPCID ||
		prev.CarrierProfile != next.CarrierProfile ||
		prev.Usbnet != next.Usbnet ||
		prev.PIN != next.PIN ||
		cellularModemPrefsChanged(prev, next)
}

func cellularModemPrefsChanged(prev, next CellularConfig) bool {
	return strings.ToLower(strings.TrimSpace(prev.NetworkMode)) != strings.ToLower(strings.TrimSpace(next.NetworkMode)) ||
		roamingEnabled(prev) != roamingEnabled(next)
}

func validateCellularConfig(cfg *CellularConfig) error {
	switch cfg.Auth {
	case "none", "pap", "chap":
	default:
		return fmt.Errorf("auth must be none, pap, or chap")
	}
	switch cfg.IPType {
	case "ipv4", "ipv6", "ipv4v6":
	default:
		return fmt.Errorf("ip_type must be ipv4, ipv6, or ipv4v6")
	}
	switch cfg.CarrierProfile {
	case "generic", "verizon":
	default:
		return fmt.Errorf("carrier_profile must be generic or verizon")
	}
	switch cfg.Usbnet {
	case "qmi", "ecm", "mbim":
	default:
		return fmt.Errorf("usbnet must be qmi, ecm, or mbim")
	}
	if cfg.NetworkMode == "" {
		cfg.NetworkMode = "auto"
	}
	switch strings.ToLower(strings.TrimSpace(cfg.NetworkMode)) {
	case "auto", "lte", "wcdma":
		cfg.NetworkMode = strings.ToLower(strings.TrimSpace(cfg.NetworkMode))
	default:
		return fmt.Errorf("network_mode must be auto, lte, or wcdma")
	}
	if cfg.Roaming == nil {
		cfg.Roaming = boolPtr(false)
	}
	if cfg.PDPCID < 1 || cfg.PDPCID > 16 {
		return fmt.Errorf("pdp_cid must be between 1 and 16")
	}
	cfg.APN = strings.TrimSpace(cfg.APN)
	cfg.User = strings.TrimSpace(cfg.User)
	cfg.PIN = strings.TrimSpace(cfg.PIN)
	if len(cfg.PIN) > 8 {
		return fmt.Errorf("pin must be at most 8 digits")
	}
	for _, r := range cfg.PIN {
		if r < '0' || r > '9' {
			return fmt.Errorf("pin must be numeric")
		}
	}
	return nil
}

func (h *CellularHandler) loadConfig() (CellularConfig, error) {
	cfg := defaultCellularConfig()
	data, err := os.ReadFile(h.configPath)
	if err != nil {
		if os.IsNotExist(err) {
			return cfg, nil
		}
		return cfg, err
	}
	if err := json.Unmarshal(data, &cfg); err != nil {
		return defaultCellularConfig(), err
	}
	_ = validateCellularConfig(&cfg)
	return cfg, nil
}

func (h *CellularHandler) saveConfig(cfg CellularConfig) error {
	if err := os.MkdirAll(filepath.Dir(h.configPath), 0755); err != nil {
		return err
	}
	data, err := json.MarshalIndent(cfg, "", "  ")
	if err != nil {
		return err
	}
	data = append(data, '\n')
	return atomicfile.Write(h.configPath, data, 0644)
}

// projectCellularConfig persists dial settings through the Config Controller
// when available (desired-state + revision), falling back to a direct write.
func (h *CellularHandler) projectCellularConfig(c *gin.Context, cfg CellularConfig) error {
	if h.configMgr != nil {
		data, err := json.MarshalIndent(cfg, "", "  ")
		if err != nil {
			return err
		}
		data = append(data, '\n')
		if _, _, err := h.configMgr.Apply(c.Request.Context(), "cellular", "config", string(data), getUsernameFromContext(c)); err != nil {
			logger.Warn("cellular: config manager apply failed, falling back to direct write: %v", err)
		} else {
			return nil
		}
	}
	return h.saveConfig(cfg)
}
