package events

// EventType represents the type of event
type EventType string

const (
	// User operations (audit log)
	EventUserLoginSuccess EventType = "user.login.success"
	EventUserLoginFailed  EventType = "user.login.failed"
	EventUserLogout       EventType = "user.logout"
	EventConfigChanged    EventType = "config.changed"
	EventPasswordChanged  EventType = "password.changed"
	EventAppInstalled     EventType = "app.installed"
	EventAppStarted       EventType = "app.started"
	EventAppStopped       EventType = "app.stopped"
	EventAppUninstalled   EventType = "app.uninstalled"
	EventContainerStarted EventType = "container.started"
	EventContainerStopped EventType = "container.stopped"
	EventDeviceControl    EventType = "device.control"
	EventSystemReboot     EventType = "system.reboot"

	// System events (runtime log)
	EventAIModelLoaded        EventType = "ai.model.loaded"
	EventAIInferenceFailed    EventType = "ai.inference.failed"
	EventStorageLow           EventType = "storage.low"
	EventStorageFull          EventType = "storage.full"
	EventNetworkConnected     EventType = "network.connected"
	EventNetworkDisconnected  EventType = "network.disconnected"
	EventNTPSyncFailed        EventType = "ntp.sync.failed"
	EventServiceStarted       EventType = "service.started"
	EventServiceStopped       EventType = "service.stopped"
	EventServiceFailed        EventType = "service.failed"
	EventFirmwareUpdated      EventType = "firmware.updated"
	EventFirmwareUpdateFailed EventType = "firmware.update.failed"
)

// EventLevel represents the severity level of an event
type EventLevel string

const (
	LevelInfo     EventLevel = "info"
	LevelWarning  EventLevel = "warning"
	LevelError    EventLevel = "error"
	LevelCritical EventLevel = "critical"
)

// EventCategory represents the category of an event
type EventCategory string

const (
	CategoryOperation EventCategory = "operation" // User operations (audit log)
	CategorySystem    EventCategory = "system"    // System runtime events
)

// Event represents a structured event
type Event struct {
	EventID   string                 `json:"event_id"`
	Timestamp string                 `json:"timestamp"`
	Type      EventType              `json:"event_type"`
	Level     EventLevel             `json:"level"`
	Category  EventCategory          `json:"category"`
	Source    string                 `json:"source"`
	User      string                 `json:"user,omitempty"`
	Message   string                 `json:"message"`
	Data      map[string]interface{} `json:"data,omitempty"`
}
