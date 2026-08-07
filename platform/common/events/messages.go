package events

// Module constants for log sources
const (
	ModuleAuth     = "auth"
	ModuleNetwork  = "network"
	ModuleStorage  = "storage"
	ModuleTime     = "time"
	ModuleSSH      = "ssh"
	ModuleAI       = "ai"
	ModuleApp      = "app"
	ModuleDevice   = "device"
	ModuleMedia    = "media"
	ModuleSystem   = "system"
	ModuleFirmware = "firmware"
	ModuleSecurity = "security"
	ModuleHardware = "hardware"
	ModulePlatform = "platform"
)

// ModuleDisplayNames contains human-readable module names in different languages
var ModuleDisplayNames = map[string]map[string]string{
	ModuleAuth:     {"zh": "认证", "en": "Auth"},
	ModuleNetwork:  {"zh": "网络", "en": "Network"},
	ModuleStorage:  {"zh": "存储", "en": "Storage"},
	ModuleTime:     {"zh": "时间", "en": "Time"},
	ModuleSSH:      {"zh": "SSH", "en": "SSH"},
	ModuleAI:       {"zh": "AI", "en": "AI"},
	ModuleApp:      {"zh": "应用", "en": "App"},
	ModuleDevice:   {"zh": "设备", "en": "Device"},
	ModuleMedia:    {"zh": "媒体", "en": "Media"},
	ModuleSystem:   {"zh": "系统", "en": "System"},
	ModuleFirmware: {"zh": "固件", "en": "Firmware"},
	ModuleSecurity: {"zh": "安全", "en": "Security"},
	ModuleHardware: {"zh": "硬件", "en": "Hardware"},
	ModulePlatform: {"zh": "平台", "en": "Platform"},
}

// GetModuleDisplayName returns the localized display name for a module
func GetModuleDisplayName(module string, locale string) string {
	if names, ok := ModuleDisplayNames[module]; ok {
		if locale == "zh" || locale == "zh-CN" || locale == "zh_CN" {
			if name, ok := names["zh"]; ok {
				return name
			}
		}
		if name, ok := names["en"]; ok {
			return name
		}
	}
	// Fallback to module name itself
	return module
}

// MessageTemplate defines the template for log messages
type MessageTemplate struct {
	Code     string        // Log code (e.g., "network.config.changed")
	Module   string        // Module name (e.g., "network", "time")
	Params   []string      // Parameter names for interpolation
	Default  string        // Default fallback message (English)
	Level    EventLevel    // Default severity level
	Category EventCategory // Event category
}

// MessageParams is a map of parameter names to values
type MessageParams map[string]interface{}

// Format formats the message template with given parameters
func (t *MessageTemplate) Format(params MessageParams, locale string) string {
	return GetLocalizer().Localize(t.Code, params, locale)
}

// All message templates defined in the system
var messageTemplates = map[string]*MessageTemplate{
	// User & Auth
	"user.login.success": {
		Code:     "user.login.success",
		Module:   "auth",
		Params:   []string{"username", "ip"},
		Default:  "User {{.username}} logged in from {{.ip}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"user.login.failed": {
		Code:     "user.login.failed",
		Module:   "auth",
		Params:   []string{"username", "reason", "ip"},
		Default:  "Login failed for {{.username}}: {{.reason}} from {{.ip}}",
		Level:    LevelWarning,
		Category: CategoryOperation,
	},
	"user.logout": {
		Code:     "user.logout",
		Module:   "auth",
		Params:   []string{"username"},
		Default:  "User {{.username}} logged out",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},

	// Config changes
	"config.changed": {
		Code:     "config.changed",
		Module:   "system",
		Params:   []string{"section", "changes"},
		Default:  "Configuration changed: {{.section}} - {{.changes}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"password.changed": {
		Code:     "password.changed",
		Module:   "auth",
		Params:   []string{"username"},
		Default:  "Password changed for user {{.username}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},

	// Network
	"network.config.changed": {
		Code:     "network.config.changed",
		Module:   "network",
		Params:   []string{"interface", "mode", "ip"},
		Default:  "Network configuration updated: {{.interface}} ({{.mode}}, {{.ip}})",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"network.connected": {
		Code:     "network.connected",
		Module:   "network",
		Params:   []string{"interface", "ip"},
		Default:  "Network connected: {{.interface}} at {{.ip}}",
		Level:    LevelInfo,
		Category: CategorySystem,
	},
	"network.disconnected": {
		Code:     "network.disconnected",
		Module:   "network",
		Params:   []string{"interface"},
		Default:  "Network disconnected: {{.interface}}",
		Level:    LevelWarning,
		Category: CategorySystem,
	},

	// Storage
	"storage.disk.mounted": {
		Code:     "storage.disk.mounted",
		Module:   "storage",
		Params:   []string{"device", "target"},
		Default:  "Disk mounted: {{.device}} at {{.target}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"storage.disk.unmounted": {
		Code:     "storage.disk.unmounted",
		Module:   "storage",
		Params:   []string{"target"},
		Default:  "Disk unmounted: {{.target}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"storage.disk.formatted": {
		Code:     "storage.disk.formatted",
		Module:   "storage",
		Params:   []string{"device", "fstype"},
		Default:  "Disk formatted: {{.device}} as {{.fstype}}",
		Level:    LevelWarning,
		Category: CategoryOperation,
	},
	"storage.low": {
		Code:     "storage.low",
		Module:   "storage",
		Params:   []string{"path", "usage_percent"},
		Default:  "Storage low: {{.path}} at {{.usage_percent}}%",
		Level:    LevelWarning,
		Category: CategorySystem,
	},
	"storage.full": {
		Code:     "storage.full",
		Module:   "storage",
		Params:   []string{"path"},
		Default:  "Storage full: {{.path}}",
		Level:    LevelCritical,
		Category: CategorySystem,
	},

	// Time
	"time.manual_set": {
		Code:     "time.manual_set",
		Module:   "time",
		Params:   []string{"datetime"},
		Default:  "System time manually set to {{.datetime}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"time.timezone.changed": {
		Code:     "time.timezone.changed",
		Module:   "time",
		Params:   []string{"timezone"},
		Default:  "Timezone changed to {{.timezone}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"time.ntp.config_changed": {
		Code:     "time.ntp.config_changed",
		Module:   "time",
		Params:   []string{"enabled", "server"},
		Default:  "NTP configuration updated: enabled={{.enabled}}, server={{.server}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"time.ntp.sync_triggered": {
		Code:     "time.ntp.sync_triggered",
		Module:   "time",
		Params:   []string{},
		Default:  "NTP synchronization triggered",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"time.config.saved": {
		Code:     "time.config.saved",
		Module:   "time",
		Params:   []string{"timezone", "sync_mode"},
		Default:  "Time configuration saved: timezone={{.timezone}}, sync_mode={{.sync_mode}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"time.client_sync": {
		Code:     "time.client_sync",
		Module:   "time",
		Params:   []string{"diff_seconds"},
		Default:  "Client time synced: offset={{.diff_seconds}}s",
		Level:    LevelInfo,
		Category: CategorySystem,
	},
	"ntp.sync.failed": {
		Code:     "ntp.sync.failed",
		Module:   "time",
		Params:   []string{"server", "error"},
		Default:  "NTP sync failed: {{.server}} - {{.error}}",
		Level:    LevelWarning,
		Category: CategorySystem,
	},

	// SSH
	"ssh.config.changed": {
		Code:     "ssh.config.changed",
		Module:   "ssh",
		Params:   []string{"changes"},
		Default:  "SSH configuration changed: {{.changes}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"ssh.login.success": {
		Code:     "ssh.login.success",
		Module:   "ssh",
		Params:   []string{"username", "ip"},
		Default:  "SSH login: {{.username}} from {{.ip}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"ssh.login.failed": {
		Code:     "ssh.login.failed",
		Module:   "ssh",
		Params:   []string{"username", "ip"},
		Default:  "SSH login failed: {{.username}} from {{.ip}}",
		Level:    LevelWarning,
		Category: CategorySystem,
	},

	// AI / Models
	"ai.model.uploaded": {
		Code:     "ai.model.uploaded",
		Module:   "ai",
		Params:   []string{"model_id", "filename", "size"},
		Default:  "AI model uploaded: {{.model_id}} ({{.filename}}, {{.size}} bytes)",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"ai.model.loaded": {
		Code:     "ai.model.loaded",
		Module:   "ai",
		Params:   []string{"model_id", "model_path"},
		Default:  "AI model loaded: {{.model_id}} from {{.model_path}}",
		Level:    LevelInfo,
		Category: CategorySystem,
	},
	"ai.model.unloaded": {
		Code:     "ai.model.unloaded",
		Module:   "ai",
		Params:   []string{"model_id"},
		Default:  "AI model unloaded: {{.model_id}}",
		Level:    LevelInfo,
		Category: CategorySystem,
	},
	"ai.model.deleted": {
		Code:     "ai.model.deleted",
		Module:   "ai",
		Params:   []string{"model_id"},
		Default:  "AI model deleted: {{.model_id}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"ai.inference.failed": {
		Code:     "ai.inference.failed",
		Module:   "ai",
		Params:   []string{"model_id", "error"},
		Default:  "AI inference failed: {{.model_id}} - {{.error}}",
		Level:    LevelError,
		Category: CategorySystem,
	},

	// Apps / Containers
	"app.installed": {
		Code:     "app.installed",
		Module:   "app",
		Params:   []string{"app_id", "version"},
		Default:  "App installed: {{.app_id}} version {{.version}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"app.started": {
		Code:     "app.started",
		Module:   "app",
		Params:   []string{"app_id"},
		Default:  "App started: {{.app_id}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"app.stopped": {
		Code:     "app.stopped",
		Module:   "app",
		Params:   []string{"app_id", "reason"},
		Default:  "App stopped: {{.app_id}} - {{.reason}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"app.uninstalled": {
		Code:     "app.uninstalled",
		Module:   "app",
		Params:   []string{"app_id"},
		Default:  "App uninstalled: {{.app_id}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"app.crashed": {
		Code:     "app.crashed",
		Module:   "app",
		Params:   []string{"app_id", "error"},
		Default:  "App crashed: {{.app_id}} - {{.error}}",
		Level:    LevelError,
		Category: CategorySystem,
	},
	"container.started": {
		Code:     "container.started",
		Module:   "app",
		Params:   []string{"container_id", "app_id"},
		Default:  "Container started: {{.container_id}} for app {{.app_id}}",
		Level:    LevelInfo,
		Category: CategorySystem,
	},
	"container.stopped": {
		Code:     "container.stopped",
		Module:   "app",
		Params:   []string{"container_id", "exit_code"},
		Default:  "Container stopped: {{.container_id}} (exit code: {{.exit_code}})",
		Level:    LevelInfo,
		Category: CategorySystem,
	},

	// Device control
	"device.control": {
		Code:     "device.control",
		Module:   "device",
		Params:   []string{"device", "action"},
		Default:  "Device control: {{.device}} - {{.action}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"device.ptz.control": {
		Code:     "device.ptz.control",
		Module:   "device",
		Params:   []string{"action", "direction", "speed"},
		Default:  "PTZ control: {{.action}} direction={{.direction}} speed={{.speed}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"device.zoom.changed": {
		Code:     "device.zoom.changed",
		Module:   "device",
		Params:   []string{"speed"},
		Default:  "Zoom changed: speed={{.speed}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"device.focus.changed": {
		Code:     "device.focus.changed",
		Module:   "device",
		Params:   []string{"speed"},
		Default:  "Focus changed: speed={{.speed}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"device.autofocus.changed": {
		Code:     "device.autofocus.changed",
		Module:   "device",
		Params:   []string{"enable"},
		Default:  "Autofocus {{.enable}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"device.lens.oneshot_af": {
		Code:     "device.lens.oneshot_af",
		Module:   "device",
		Params:   []string{},
		Default:  "One-shot autofocus triggered",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"device.gpio.write": {
		Code:     "device.gpio.write",
		Module:   "device",
		Params:   []string{"pin", "value"},
		Default:  "GPIO write: pin {{.pin}} = {{.value}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"device.lens.init": {
		Code:     "device.lens.init",
		Module:   "device",
		Params:   []string{"action"},
		Default:  "Lens {{.action}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"device.lens.goto": {
		Code:     "device.lens.goto",
		Module:   "device",
		Params:   []string{"zoom_ratio", "focus_distance_m"},
		Default:  "Lens goto: zoom={{.zoom_ratio}}x focus={{.focus_distance_m}}m",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"device.ptz.moved": {
		Code:     "device.ptz.moved",
		Module:   "device",
		Params:   []string{"pan", "tilt", "zoom"},
		Default:  "PTZ moved: pan={{.pan}}, tilt={{.tilt}}, zoom={{.zoom}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"device.io.triggered": {
		Code:     "device.io.triggered",
		Module:   "device",
		Params:   []string{"port", "state"},
		Default:  "IO triggered: port {{.port}} = {{.state}}",
		Level:    LevelInfo,
		Category: CategorySystem,
	},

	// Media / Camera
	"media.config.changed": {
		Code:     "media.config.changed",
		Module:   "media",
		Params:   []string{"stream", "changes"},
		Default:  "Media configuration changed: {{.stream}} - {{.changes}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"media.osd.changed": {
		Code:     "media.osd.changed",
		Module:   "media",
		Params:   []string{"streams"},
		Default:  "OSD configuration updated for {{.streams}} stream(s)",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"media.ai_overlay.changed": {
		Code:     "media.ai_overlay.changed",
		Module:   "media",
		Params:   []string{"enabled"},
		Default:  "AI overlay {{.enabled}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"camera.stream.started": {
		Code:     "camera.stream.started",
		Module:   "media",
		Params:   []string{"stream_name", "resolution"},
		Default:  "Camera stream started: {{.stream_name}} ({{.resolution}})",
		Level:    LevelInfo,
		Category: CategorySystem,
	},
	"camera.stream.stopped": {
		Code:     "camera.stream.stopped",
		Module:   "media",
		Params:   []string{"stream_name", "reason"},
		Default:  "Camera stream stopped: {{.stream_name}} - {{.reason}}",
		Level:    LevelWarning,
		Category: CategorySystem,
	},
	"media.pipeline.reconfigured": {
		Code:     "media.pipeline.reconfigured",
		Module:   "media",
		Params:   []string{"stream_count"},
		Default:  "Media pipeline reconfigured: {{.stream_count}} stream(s)",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"media.stream.added": {
		Code:     "media.stream.added",
		Module:   "media",
		Params:   []string{"stream"},
		Default:  "Media stream added: {{.stream}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"media.stream.removed": {
		Code:     "media.stream.removed",
		Module:   "media",
		Params:   []string{"stream"},
		Default:  "Media stream removed: {{.stream}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"media.stream.enabled": {
		Code:     "media.stream.enabled",
		Module:   "media",
		Params:   []string{"stream"},
		Default:  "Media stream enabled: {{.stream}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"media.stream.disabled": {
		Code:     "media.stream.disabled",
		Module:   "media",
		Params:   []string{"stream"},
		Default:  "Media stream disabled: {{.stream}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},

	// System
	"system.reboot": {
		Code:     "system.reboot",
		Module:   "system",
		Params:   []string{"triggered_by", "reason"},
		Default:  "System reboot initiated by {{.triggered_by}}: {{.reason}}",
		Level:    LevelWarning,
		Category: CategoryOperation,
	},
	"system.shutdown": {
		Code:     "system.shutdown",
		Module:   "system",
		Params:   []string{"triggered_by"},
		Default:  "System shutdown initiated by {{.triggered_by}}",
		Level:    LevelWarning,
		Category: CategoryOperation,
	},
	"service.started": {
		Code:     "service.started",
		Module:   "system",
		Params:   []string{"service_name"},
		Default:  "Service started: {{.service_name}}",
		Level:    LevelInfo,
		Category: CategorySystem,
	},
	"service.stopped": {
		Code:     "service.stopped",
		Module:   "system",
		Params:   []string{"service_name"},
		Default:  "Service stopped: {{.service_name}}",
		Level:    LevelInfo,
		Category: CategorySystem,
	},
	"service.failed": {
		Code:     "service.failed",
		Module:   "system",
		Params:   []string{"service_name", "error"},
		Default:  "Service failed: {{.service_name}} - {{.error}}",
		Level:    LevelError,
		Category: CategorySystem,
	},
	"service.restarted": {
		Code:     "service.restarted",
		Module:   "system",
		Params:   []string{"service_name"},
		Default:  "Service restarted: {{.service_name}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},

	// Firmware
	"firmware.updated": {
		Code:     "firmware.updated",
		Module:   "system",
		Params:   []string{"version", "previous_version"},
		Default:  "Firmware updated to {{.version}} (was {{.previous_version}})",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},
	"firmware.update.failed": {
		Code:     "firmware.update.failed",
		Module:   "system",
		Params:   []string{"version", "error"},
		Default:  "Firmware update failed: {{.version}} - {{.error}}",
		Level:    LevelError,
		Category: CategorySystem,
	},
	"firmware.update.started": {
		Code:     "firmware.update.started",
		Module:   "system",
		Params:   []string{"version"},
		Default:  "Firmware update started: {{.version}}",
		Level:    LevelInfo,
		Category: CategoryOperation,
	},

	// Security
	"security.auth.failed": {
		Code:     "security.auth.failed",
		Module:   "security",
		Params:   []string{"username", "ip", "reason"},
		Default:  "Authentication failed: {{.username}} from {{.ip}} - {{.reason}}",
		Level:    LevelWarning,
		Category: CategorySystem,
	},
	"security.permission.denied": {
		Code:     "security.permission.denied",
		Module:   "security",
		Params:   []string{"user", "resource", "action"},
		Default:  "Permission denied: {{.user}} attempted {{.action}} on {{.resource}}",
		Level:    LevelWarning,
		Category: CategorySystem,
	},
	"security.ssl.cert.expiring": {
		Code:     "security.ssl.cert.expiring",
		Module:   "security",
		Params:   []string{"certificate", "days_remaining"},
		Default:  "SSL certificate expiring: {{.certificate}} ({{.days_remaining}} days remaining)",
		Level:    LevelWarning,
		Category: CategorySystem,
	},

	// Hardware
	"hardware.temperature.high": {
		Code:     "hardware.temperature.high",
		Module:   "hardware",
		Params:   []string{"device", "temperature", "threshold"},
		Default:  "High temperature: {{.device}} at {{.temperature}}°C (threshold: {{.threshold}}°C)",
		Level:    LevelWarning,
		Category: CategorySystem,
	},
	"hardware.temperature.critical": {
		Code:     "hardware.temperature.critical",
		Module:   "hardware",
		Params:   []string{"device", "temperature"},
		Default:  "Critical temperature: {{.device}} at {{.temperature}}°C",
		Level:    LevelCritical,
		Category: CategorySystem,
	},
	"hardware.fan.failed": {
		Code:     "hardware.fan.failed",
		Module:   "hardware",
		Params:   []string{"fan_id"},
		Default:  "Fan failure detected: {{.fan_id}}",
		Level:    LevelCritical,
		Category: CategorySystem,
	},
}

// GetAllTemplates returns all registered message templates
func GetAllTemplates() map[string]*MessageTemplate {
	return messageTemplates
}

// GetTemplate returns the message template for a given code
func GetTemplate(code string) *MessageTemplate {
	if tmpl, ok := messageTemplates[code]; ok {
		return tmpl
	}
	// Return a default template for unknown codes
	return &MessageTemplate{
		Code:     code,
		Default:  code,
		Level:    LevelInfo,
		Category: CategorySystem,
	}
}

// GetDefaultLevel returns the default severity level for an event code
func GetDefaultLevel(code string) EventLevel {
	return GetTemplate(code).Level
}

// GetDefaultCategory returns the default category for an event code
func GetDefaultCategory(code string) EventCategory {
	return GetTemplate(code).Category
}
