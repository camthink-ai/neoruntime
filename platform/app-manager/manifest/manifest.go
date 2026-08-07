package manifest

import (
	"fmt"
	"os"
	"strings"

	"aipc/platform/common/utils"
	"gopkg.in/yaml.v3"
)

// AppManifest represents the application manifest structure
type AppManifest struct {
	APIVersion string   `yaml:"apiVersion"`
	Kind       string   `yaml:"kind"`
	Metadata   Metadata `yaml:"metadata"`
	Spec       Spec     `yaml:"spec"`
}

type Metadata struct {
	ID          string `yaml:"id"`
	Name        string `yaml:"name"`
	Version     string `yaml:"version"`
	Description string `yaml:"description"`
	Author      string `yaml:"author"`
	Email       string `yaml:"email"`
}

type Spec struct {
	// Single container mode (backward compatible)
	Image              string             `yaml:"image"`
	Permissions        Permissions        `yaml:"permissions"`
	Resources          Resources          `yaml:"resources"`
	Env                []EnvVar           `yaml:"env"`
	Volumes            []Volume           `yaml:"volumes"`
	Autostart          bool               `yaml:"autostart"`
	RestartPolicy      string             `yaml:"restart_policy"`
	RestartMaxRetries  int                `yaml:"restart_max_retries"`
	Healthcheck        Healthcheck        `yaml:"healthcheck"`
	AutoRestart        AutoRestart        `yaml:"auto_restart"`
	Plugin             *PluginSpec        `yaml:"plugin"`
	PluginDependencies []PluginDependency `yaml:"plugin_dependencies"`
	Security           SecuritySpec       `yaml:"security"`

	// Multi-container mode (Main/Sub architecture)
	Containers map[string]ContainerSpec `yaml:"containers"`
	Networking NetworkingConfig         `yaml:"networking"`
	Lifecycle  LifecycleConfig          `yaml:"lifecycle"`

	// Dev mode for hot reload during development
	Dev *DevConfig `yaml:"dev,omitempty"`
}

// SecuritySpec allows per-app override of container security settings
type SecuritySpec struct {
	NoNewPrivileges *bool `yaml:"no_new_privileges"` // nil = true (default), false = allow privilege escalation
	ReadonlyRootfs  *bool `yaml:"readonly_rootfs"`   // nil = true (default), false = writable rootfs
}

// DevConfig enables hot reload for local development.
// When enabled, source directories are bind-mounted into the container,
// readonly rootfs is disabled, and a file watcher can auto-reload on changes.
type DevConfig struct {
	Enabled      bool           `yaml:"enabled"`
	WatchPath    string         `yaml:"watch_path"`    // path inside container to watch (default: /app)
	Sync         []DevSyncMount `yaml:"sync"`          // host→container bind mounts for source code
	ReloadSignal string         `yaml:"reload_signal"` // SIGHUP or SIGTERM (default: SIGTERM)
	DebugPort    int            `yaml:"debug_port"`    // optional debugpy port for IDE attach
}

// DevSyncMount maps a host source directory into the container.
type DevSyncMount struct {
	Host      string `yaml:"host"`      // relative path from the app directory
	Container string `yaml:"container"` // absolute path inside the container
}

type Resources struct {
	CPU    string `yaml:"cpu"`    // e.g., "50%" or "0.5"
	Memory string `yaml:"memory"` // e.g., "256Mi" or "1Gi"
}

type Permissions struct {
	Video     []string       `yaml:"video"`
	Inference InferencePerms `yaml:"inference"`
	Events    EventPerms     `yaml:"events"`
	Device    DevicePerms    `yaml:"device"`
	Network   NetworkPerms   `yaml:"network"`
}

type InferencePerms struct {
	Models        []string `yaml:"models"`
	MaxQPS        int      `yaml:"max_qps"`
	MaxConcurrent int      `yaml:"max_concurrent"`
	AllowRegister bool     `yaml:"allow_register_model"`
}

type EventPerms struct {
	Publish   []string `yaml:"publish"`
	Subscribe []string `yaml:"subscribe"`
}

type DevicePerms struct {
	Light bool      `yaml:"light"`
	IrCut bool      `yaml:"ir_cut"`
	PTZ   bool      `yaml:"ptz"`
	Lens  bool      `yaml:"lens"`
	GPIO  GPIOPerms `yaml:"gpio"`
}

type GPIOPerms struct {
	Read  []int `yaml:"read"`
	Write []int `yaml:"write"`
}

type NetworkPerms struct {
	Outbound []string `yaml:"outbound"`
	Mode     string   `yaml:"mode"`    // "isolated" (default) or "host"
	Inbound  []int    `yaml:"inbound"` // Ports exposed to host (only when mode=host)
}

// ============================================
// Multi-Container Types (Main/Sub Architecture)
// ============================================

// ContainerSpec defines a single container in a multi-container application
type ContainerSpec struct {
	Image       string        `yaml:"image"`
	Role        string        `yaml:"role"`        // "main" or "sub" - main has platform access
	Permissions Permissions   `yaml:"permissions"` // Only valid for main container
	Resources   Resources     `yaml:"resources"`
	Env         []EnvVar      `yaml:"env"`
	Ports       []PortSpec    `yaml:"ports"`
	Command     []string      `yaml:"command"`
	Args        []string      `yaml:"args"`
	Healthcheck Healthcheck   `yaml:"healthcheck"`
	Volumes     []VolumeMount `yaml:"volumes"` // Container-specific volume mounts
	Security    SecuritySpec  `yaml:"security"`
}

// PortSpec defines a container port
type PortSpec struct {
	ContainerPort int    `yaml:"containerPort"`
	Protocol      string `yaml:"protocol"` // "TCP" or "UDP"
	Name          string `yaml:"name"`     // Service name for discovery
}

// VolumeMount defines a container-specific volume mount
type VolumeMount struct {
	Name      string `yaml:"name"`
	Container string `yaml:"container"`
	Readonly  bool   `yaml:"readonly"`
}

// NetworkingConfig defines networking for multi-container apps
type NetworkingConfig struct {
	Mode    string        `yaml:"mode"`    // "internal", "bridge", or "host"
	Ingress []IngressRule `yaml:"ingress"` // External access rules
}

// IngressRule defines how to expose a container port
type IngressRule struct {
	Port     int    `yaml:"port"`     // External port
	Target   string `yaml:"target"`   // "containerName:port"
	Protocol string `yaml:"protocol"` // "HTTP", "TCP", "UDP"
}

// LifecycleConfig defines startup/shutdown behavior
type LifecycleConfig struct {
	StartupOrder  []string `yaml:"startup_order"`  // Container start order
	ShutdownOrder []string `yaml:"shutdown_order"` // Container stop order
	RestartPolicy string   `yaml:"restart_policy"` // "always", "on-failure", "no"
}

// PluginSpec declares plugin capabilities
type PluginSpec struct {
	Capabilities []PluginCapability `yaml:"capabilities"`
}

// PluginCapability describes a single capability provided by a plugin
type PluginCapability struct {
	ID          string            `yaml:"id"`
	Version     string            `yaml:"version"`
	Transport   string            `yaml:"transport"` // "grpc", "event", "both"
	Description string            `yaml:"description"`
	Proto       string            `yaml:"proto"`  // gRPC service name (required for grpc/both)
	Topics      *CapabilityTopics `yaml:"topics"` // Event topics (required for event/both)
}

// CapabilityTopics defines event topics for a capability
type CapabilityTopics struct {
	Publish   []string `yaml:"publish"`
	Subscribe []string `yaml:"subscribe"`
}

// PluginDependency declares a dependency on a plugin capability
type PluginDependency struct {
	Capability string `yaml:"capability"`
	MinVersion string `yaml:"min_version"`
	Required   bool   `yaml:"required"`
}

type EnvVar struct {
	Name  string `yaml:"name"`
	Value string `yaml:"value"`
}

type Volume struct {
	Host      string `yaml:"host"`
	Container string `yaml:"container"`
	Readonly  bool   `yaml:"readonly"`
}

type Healthcheck struct {
	Enabled                    bool   `yaml:"enabled"`
	Type                       string `yaml:"type"`    // "command", "http", "tcp"
	Command                    string `yaml:"command"` // For command type
	Path                       string `yaml:"path"`    // For http type
	Port                       int    `yaml:"port"`    // For http/tcp type
	Interval                   string `yaml:"interval"`
	TimeoutSeconds             int    `yaml:"timeout_seconds"`
	Retries                    int    `yaml:"retries"`
	HealthCheckIntervalSeconds int    `yaml:"health_check_interval_seconds"`
}

type AutoRestart struct {
	Enabled                    bool    `yaml:"enabled"`
	MaxRetries                 int     `yaml:"max_retries"`
	RetryDelaySeconds          int     `yaml:"retry_delay_seconds"`
	BackoffMultiplier          float64 `yaml:"backoff_multiplier"`
	HealthCheckIntervalSeconds int     `yaml:"health_check_interval_seconds"`
}

// LoadManifest loads and parses an app manifest file
func LoadManifest(path string) (*AppManifest, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("failed to read manifest: %w", err)
	}

	var manifest AppManifest
	if err := yaml.Unmarshal(data, &manifest); err != nil {
		return nil, fmt.Errorf("failed to parse manifest: %w", err)
	}

	if err := manifest.Validate(); err != nil {
		return nil, fmt.Errorf("manifest validation failed: %w", err)
	}

	return &manifest, nil
}

// Validate validates the manifest
func (m *AppManifest) Validate() error {
	// Check API version
	if m.APIVersion != "v1" {
		return fmt.Errorf("unsupported API version: %s", m.APIVersion)
	}

	// Check kind
	if m.Kind != "Application" && m.Kind != "ModelService" && m.Kind != "BusinessService" {
		return fmt.Errorf("invalid kind: %s", m.Kind)
	}

	// Check metadata
	if m.Metadata.ID == "" {
		return fmt.Errorf("metadata.id is required")
	}
	if m.Metadata.Name == "" {
		return fmt.Errorf("metadata.name is required")
	}
	if m.Metadata.Version == "" {
		return fmt.Errorf("metadata.version is required")
	}

	// Check spec
	// For single-container mode, image is required at spec.image
	// For multi-container mode, image is in each container spec
	if !m.IsMultiContainer() {
		if m.Spec.Image == "" {
			return fmt.Errorf("spec.image is required for single-container applications")
		}
	} else {
		// Multi-container: validate each container has an image
		for name, c := range m.Spec.Containers {
			if c.Image == "" {
				return fmt.Errorf("containers.%s.image is required", name)
			}
		}
	}

	// Validate resources
	if err := m.Spec.Resources.Validate(); err != nil {
		return fmt.Errorf("resources validation failed: %w", err)
	}

	// Validate plugin
	if err := m.ValidatePlugin(); err != nil {
		return fmt.Errorf("plugin validation failed: %w", err)
	}

	// Validate plugin dependencies
	if err := m.ValidatePluginDependencies(); err != nil {
		return fmt.Errorf("plugin dependencies validation failed: %w", err)
	}

	// Validate network mode
	if err := m.ValidateNetworkMode(); err != nil {
		return fmt.Errorf("network validation failed: %w", err)
	}

	// Validate multi-container configuration
	if m.IsMultiContainer() {
		if err := m.ValidateMultiContainer(); err != nil {
			return fmt.Errorf("multi-container validation failed: %w", err)
		}
	}

	return nil
}

// Validate validates resources
// CPU and Memory are optional - only validate format if provided
func (r *Resources) Validate() error {
	// Validate CPU format only if provided
	if r.CPU != "" {
		if _, err := r.GetCPUQuota(); err != nil {
			return fmt.Errorf("invalid CPU format: %w", err)
		}
	}

	// Validate Memory format only if provided
	if r.Memory != "" {
		if _, err := r.GetMemoryBytes(); err != nil {
			return fmt.Errorf("invalid memory format: %w", err)
		}
	}

	return nil
}

// GetCPUQuota converts CPU string to numeric value
// Supports formats: "50%" -> 0.5, "1.5" -> 1.5, "2" -> 2.0
func (r *Resources) GetCPUQuota() (float64, error) {
	if r.CPU == "" {
		return 0, fmt.Errorf("cpu is empty")
	}
	return utils.ParseCPU(r.CPU)
}

// GetMemoryBytes converts memory string to bytes
// Supports formats: "256Mi", "1Gi", "512M", "2G", "1024" (bytes)
func (r *Resources) GetMemoryBytes() (int64, error) {
	if r.Memory == "" {
		return 0, fmt.Errorf("memory is empty")
	}
	return utils.ParseMemory(r.Memory)
}

// ToContainerEnv converts manifest env vars to container environment format.
// Values containing ${VAR} references are expanded from the app-manager
// process environment (e.g. PLATFORM_API_TOKEN=${AIPC_TOKEN_KEY} resolves
// to the actual token). Unresolvable references are left as-is so the
// container can detect the misconfiguration rather than silently receiving
// an empty string.
func (m *AppManifest) ToContainerEnv() []string {
	env := make([]string, 0, len(m.Spec.Env))
	for _, envVar := range m.Spec.Env {
		env = append(env, fmt.Sprintf("%s=%s", envVar.Name, ExpandEnvRefs(envVar.Value)))
	}
	return env
}

// ExpandEnvRefs replaces ${VAR} patterns in s with os.Getenv("VAR").
// It only handles the ${VAR} form (not $VAR) to match the app.yaml convention.
// Unresolvable refs (empty or unset env) are left unchanged.
func ExpandEnvRefs(s string) string {
	return os.Expand(s, func(key string) string {
		v := os.Getenv(key)
		if v == "" {
			// Leave the reference intact so the container sees "${KEY}"
			// instead of silently receiving an empty string.
			return "${" + key + "}"
		}
		return v
	})
}

// HasPermission checks if app has specific permission
func (m *AppManifest) HasPermission(category, item string) bool {
	switch category {
	case "video":
		for _, v := range m.Spec.Permissions.Video {
			if v == item {
				return true
			}
		}
	case "model":
		for _, model := range m.Spec.Permissions.Inference.Models {
			if model == item {
				return true
			}
		}
	case "device.light":
		return m.Spec.Permissions.Device.Light
	case "device.ptz":
		return m.Spec.Permissions.Device.PTZ
	case "device.lens":
		return m.Spec.Permissions.Device.Lens
	}

	return false
}

// CanPublishEvent checks if app can publish to topic
func (m *AppManifest) CanPublishEvent(topic string) bool {
	for _, pattern := range m.Spec.Permissions.Events.Publish {
		if matchTopic(topic, pattern) {
			return true
		}
	}
	return false
}

// CanSubscribeEvent checks if app can subscribe to topic
func (m *AppManifest) CanSubscribeEvent(topic string) bool {
	for _, pattern := range m.Spec.Permissions.Events.Subscribe {
		if matchTopic(topic, pattern) {
			return true
		}
	}
	return false
}

// matchTopic performs wildcard matching for topic patterns
// Uses common utils package for consistency
func matchTopic(topic, pattern string) bool {
	return utils.MatchTopic(topic, pattern)
}

// NormalizeImageName ensures the image name has a registry prefix
// containerd requires full image references (e.g., docker.io/aipc/app:1.0)
// This function adds "docker.io/" prefix if no registry is specified
func NormalizeImageName(image string) string {
	if image == "" {
		return image
	}

	// Check if image already has a registry prefix
	// A registry prefix contains a dot (.) or colon (:) before the first slash
	// Examples with registry: docker.io/aipc/app, ghcr.io/org/app, localhost:5000/app
	// Examples without registry: aipc/app, ubuntu, nginx:latest
	firstSlash := strings.Index(image, "/")
	if firstSlash == -1 {
		// No slash, single name like "ubuntu" or "nginx:latest"
		// Add docker.io/library/ prefix
		return "docker.io/library/" + image
	}

	prefix := image[:firstSlash]
	// Check if prefix looks like a registry (contains . or :)
	if strings.Contains(prefix, ".") || strings.Contains(prefix, ":") {
		// Already has registry prefix
		return image
	}

	// No registry prefix, add docker.io/
	return "docker.io/" + image
}

// GetNormalizedImage returns the image name with registry prefix
func (m *AppManifest) GetNormalizedImage() string {
	return NormalizeImageName(m.Spec.Image)
}

// EffectiveRestartPolicy returns the effective restart policy with compatibility fallbacks.
// Supported values: "always", "on-failure", "no".
func (m *AppManifest) EffectiveRestartPolicy() string {
	if m.Spec.AutoRestart.Enabled {
		return "always"
	}
	switch strings.ToLower(strings.TrimSpace(m.Spec.RestartPolicy)) {
	case "always":
		return "always"
	case "on-failure", "on_failure":
		return "on-failure"
	case "no", "never":
		return "no"
	default:
		return "no"
	}
}

// IsAutoRestartEnabled returns whether the app should be auto-restarted.
func (m *AppManifest) IsAutoRestartEnabled() bool {
	return m.EffectiveRestartPolicy() != "no"
}

// EffectiveRestartMaxRetries returns restart max retries with compatibility fallback.
// 0 means unlimited retries.
func (m *AppManifest) EffectiveRestartMaxRetries() int {
	if m.Spec.AutoRestart.MaxRetries > 0 {
		return m.Spec.AutoRestart.MaxRetries
	}
	if m.Spec.RestartMaxRetries > 0 {
		return m.Spec.RestartMaxRetries
	}
	return 0
}

// IsPlugin returns true if this manifest declares plugin capabilities
func (m *AppManifest) IsPlugin() bool {
	return m.Spec.Plugin != nil && len(m.Spec.Plugin.Capabilities) > 0
}

// HasPluginDependencies returns true if this manifest declares plugin dependencies
func (m *AppManifest) HasPluginDependencies() bool {
	return len(m.Spec.PluginDependencies) > 0
}

// IsHostNetwork returns true if the app requires host network mode
func (m *AppManifest) IsHostNetwork() bool {
	return m.Spec.Permissions.Network.Mode == "host"
}

// ============================================
// Multi-Container Helper Methods
// ============================================

// IsMultiContainer returns true if this is a multi-container application
func (m *AppManifest) IsMultiContainer() bool {
	return len(m.Spec.Containers) > 0
}

// GetMainContainer returns the main container name and spec
// Returns empty string and nil if no main container found
func (m *AppManifest) GetMainContainer() (string, *ContainerSpec) {
	for name, c := range m.Spec.Containers {
		if c.Role == "main" {
			return name, &c
		}
	}
	return "", nil
}

// GetSubContainers returns all sub containers
func (m *AppManifest) GetSubContainers() map[string]*ContainerSpec {
	subs := make(map[string]*ContainerSpec)
	for name, c := range m.Spec.Containers {
		if c.Role == "sub" {
			// Create a copy to return pointer
			container := c
			subs[name] = &container
		}
	}
	return subs
}

// GetStartupOrder returns the container startup order
// If not specified, defaults to: sub containers first, then main
func (m *AppManifest) GetStartupOrder() []string {
	if len(m.Spec.Lifecycle.StartupOrder) > 0 {
		return m.Spec.Lifecycle.StartupOrder
	}

	// Default order: sub containers first, then main
	var order []string
	for name, c := range m.Spec.Containers {
		if c.Role == "sub" {
			order = append(order, name)
		}
	}
	if name, _ := m.GetMainContainer(); name != "" {
		order = append(order, name)
	}
	return order
}

// GetShutdownOrder returns the container shutdown order
// If not specified, defaults to reverse of startup order
func (m *AppManifest) GetShutdownOrder() []string {
	if len(m.Spec.Lifecycle.ShutdownOrder) > 0 {
		return m.Spec.Lifecycle.ShutdownOrder
	}

	// Default: reverse of startup order (main first, then subs)
	startup := m.GetStartupOrder()
	shutdown := make([]string, len(startup))
	for i, name := range startup {
		shutdown[len(startup)-1-i] = name
	}
	return shutdown
}

// GetPluginSocketPath returns the expected socket path for a plugin
func (m *AppManifest) GetPluginSocketPath() string {
	return fmt.Sprintf("/run/aipc/plugins/%s.sock", m.Metadata.ID)
}

// ValidatePlugin validates plugin-specific fields
func (m *AppManifest) ValidatePlugin() error {
	if m.Spec.Plugin == nil {
		return nil
	}

	for i, cap := range m.Spec.Plugin.Capabilities {
		if cap.ID == "" {
			return fmt.Errorf("plugin.capabilities[%d].id is required", i)
		}
		if cap.Version == "" {
			return fmt.Errorf("plugin.capabilities[%d].version is required", i)
		}
		if cap.Transport == "" {
			return fmt.Errorf("plugin.capabilities[%d].transport is required", i)
		}

		switch cap.Transport {
		case "grpc":
			if cap.Proto == "" {
				return fmt.Errorf("plugin.capabilities[%d].proto is required for grpc transport", i)
			}
		case "event":
			if cap.Topics == nil || (len(cap.Topics.Publish) == 0 && len(cap.Topics.Subscribe) == 0) {
				return fmt.Errorf("plugin.capabilities[%d].topics is required for event transport", i)
			}
		case "both":
			if cap.Proto == "" {
				return fmt.Errorf("plugin.capabilities[%d].proto is required for both transport", i)
			}
		default:
			return fmt.Errorf("plugin.capabilities[%d].transport must be grpc, event, or both", i)
		}

		// Verify event topics are covered by permissions.events
		if cap.Topics != nil {
			for _, topic := range cap.Topics.Publish {
				if !m.CanPublishEvent(topic) {
					return fmt.Errorf("plugin capability %q publishes topic %q not covered by permissions.events.publish", cap.ID, topic)
				}
			}
			for _, topic := range cap.Topics.Subscribe {
				if !m.CanSubscribeEvent(topic) {
					return fmt.Errorf("plugin capability %q subscribes topic %q not covered by permissions.events.subscribe", cap.ID, topic)
				}
			}
		}
	}

	return nil
}

// ValidatePluginDependencies validates plugin dependency fields
func (m *AppManifest) ValidatePluginDependencies() error {
	for i, dep := range m.Spec.PluginDependencies {
		if dep.Capability == "" {
			return fmt.Errorf("plugin_dependencies[%d].capability is required", i)
		}
	}
	return nil
}

// ValidateNetworkMode validates network mode settings
func (m *AppManifest) ValidateNetworkMode() error {
	mode := m.Spec.Permissions.Network.Mode
	if mode != "" && mode != "isolated" && mode != "host" {
		return fmt.Errorf("permissions.network.mode must be 'isolated' or 'host', got %q", mode)
	}
	if mode != "host" && len(m.Spec.Permissions.Network.Inbound) > 0 {
		return fmt.Errorf("permissions.network.inbound requires network.mode=host")
	}
	return nil
}

// ValidateMultiContainer validates multi-container specific configuration
func (m *AppManifest) ValidateMultiContainer() error {
	// 1. Must have exactly one main container
	mainCount := 0
	for name, c := range m.Spec.Containers {
		switch c.Role {
		case "main":
			mainCount++
		case "sub":
			// Valid role
		default:
			return fmt.Errorf("container %q has invalid role %q, must be 'main' or 'sub'", name, c.Role)
		}
	}

	if mainCount == 0 {
		return fmt.Errorf("multi-container app must have at least one 'main' container")
	}
	if mainCount > 1 {
		return fmt.Errorf("multi-container app can only have one 'main' container, found %d", mainCount)
	}

	// 2. Sub containers cannot have permissions
	for name, c := range m.Spec.Containers {
		if c.Role == "sub" {
			if err := validateSubContainerNoPermissions(name, c); err != nil {
				return err
			}
		}
	}

	// 3. Validate startup order references valid containers
	validNames := make(map[string]bool)
	for name := range m.Spec.Containers {
		validNames[name] = true
	}

	for i, name := range m.Spec.Lifecycle.StartupOrder {
		if !validNames[name] {
			return fmt.Errorf("startup_order[%d] references unknown container %q", i, name)
		}
	}

	for i, name := range m.Spec.Lifecycle.ShutdownOrder {
		if !validNames[name] {
			return fmt.Errorf("shutdown_order[%d] references unknown container %q", i, name)
		}
	}

	// 4. Validate networking mode
	if m.Spec.Networking.Mode != "" {
		if m.Spec.Networking.Mode != "internal" &&
			m.Spec.Networking.Mode != "bridge" &&
			m.Spec.Networking.Mode != "host" {
			return fmt.Errorf("networking.mode must be 'internal', 'bridge', or 'host', got %q", m.Spec.Networking.Mode)
		}
	}

	return nil
}

// validateSubContainerNoPermissions ensures sub containers have no permissions
func validateSubContainerNoPermissions(name string, c ContainerSpec) error {
	// Check if permissions are empty (zero value)
	if len(c.Permissions.Video) > 0 {
		return fmt.Errorf("sub container %q cannot have video permissions", name)
	}
	if len(c.Permissions.Inference.Models) > 0 {
		return fmt.Errorf("sub container %q cannot have inference permissions", name)
	}
	if len(c.Permissions.Events.Publish) > 0 || len(c.Permissions.Events.Subscribe) > 0 {
		return fmt.Errorf("sub container %q cannot have event permissions", name)
	}
	if c.Permissions.Device.Light || c.Permissions.Device.IrCut ||
		c.Permissions.Device.PTZ || c.Permissions.Device.Lens {
		return fmt.Errorf("sub container %q cannot have device permissions", name)
	}
	if len(c.Permissions.Device.GPIO.Read) > 0 || len(c.Permissions.Device.GPIO.Write) > 0 {
		return fmt.Errorf("sub container %q cannot have GPIO permissions", name)
	}
	return nil
}
