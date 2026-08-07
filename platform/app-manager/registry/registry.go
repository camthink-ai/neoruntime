package registry

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
)

type AppState string

const (
	AppStateInstalled AppState = "installed"
	AppStateRunning   AppState = "running"
	AppStateStopped   AppState = "stopped"
	AppStateFailed    AppState = "failed"
)

type AppInfo struct {
	ID           string    `json:"id"`
	Name         string    `json:"name"`
	Version      string    `json:"version"`
	Image        string    `json:"image"`
	State        AppState  `json:"state"`
	ContainerID  string    `json:"container_id"`
	PID          int       `json:"pid"`
	InstalledAt  time.Time `json:"installed_at"`
	StartedAt    time.Time `json:"started_at"`
	StoppedAt    time.Time `json:"stopped_at"`
	RestartCount int       `json:"restart_count"`
	ManifestPath string    `json:"manifest_path"`
	InstancePath string    `json:"instance_path"`
	// Plugin fields
	IsPlugin     bool             `json:"is_plugin"`
	Capabilities []CapabilityInfo `json:"capabilities,omitempty"`
	Dependencies []DependencyInfo `json:"dependencies,omitempty"`
	// Multi-container fields
	IsMultiContainer bool `json:"is_multi_container"`
	// SDK-registered web path
	WebURL string `json:"web_url,omitempty"`
}

// CapabilityInfo records a capability provided by a plugin app
type CapabilityInfo struct {
	ID        string `json:"id"`
	Version   string `json:"version"`
	Transport string `json:"transport"`
}

// DependencyInfo records a dependency on a plugin capability
type DependencyInfo struct {
	Capability string `json:"capability"`
	MinVersion string `json:"min_version"`
	Required   bool   `json:"required"`
}

type Registry struct {
	mu       sync.RWMutex
	apps     map[string]*AppInfo
	dataPath string
}

func NewRegistry(dataPath string) (*Registry, error) {
	if err := os.MkdirAll(dataPath, 0755); err != nil {
		return nil, fmt.Errorf("failed to create registry directory: %w", err)
	}

	r := &Registry{
		apps:     make(map[string]*AppInfo),
		dataPath: dataPath,
	}

	// Load existing apps
	if err := r.load(); err != nil {
		return nil, fmt.Errorf("failed to load registry: %w", err)
	}

	return r, nil
}

func (r *Registry) Exists(appID string) bool {
	r.mu.RLock()
	defer r.mu.RUnlock()
	_, exists := r.apps[appID]
	return exists
}

func (r *Registry) Register(app *AppInfo) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	if _, exists := r.apps[app.ID]; exists {
		return fmt.Errorf("app %s already registered", app.ID)
	}

	app.InstalledAt = time.Now()
	app.State = AppStateInstalled

	r.apps[app.ID] = app

	return r.save()
}

func (r *Registry) Unregister(appID string) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	if _, exists := r.apps[appID]; !exists {
		return fmt.Errorf("app %s not found", appID)
	}

	delete(r.apps, appID)

	return r.save()
}

func (r *Registry) Get(appID string) (*AppInfo, error) {
	r.mu.RLock()
	defer r.mu.RUnlock()

	app, exists := r.apps[appID]
	if !exists {
		return nil, fmt.Errorf("app %s not found", appID)
	}

	// Return a copy to prevent callers from mutating the internal state
	cp := *app
	return &cp, nil
}

func (r *Registry) Update(app *AppInfo) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	if _, exists := r.apps[app.ID]; !exists {
		return fmt.Errorf("app %s not found", app.ID)
	}

	r.apps[app.ID] = app

	return r.save()
}

func (r *Registry) List() []*AppInfo {
	r.mu.RLock()
	defer r.mu.RUnlock()

	apps := make([]*AppInfo, 0, len(r.apps))
	for _, app := range r.apps {
		cp := *app
		apps = append(apps, &cp)
	}

	return apps
}

// GetByContainerID finds an app by its container ID
func (r *Registry) GetByContainerID(containerID string) (*AppInfo, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()

	for _, app := range r.apps {
		if app.ContainerID == containerID {
			return app, true
		}
	}

	return nil, false
}

func (r *Registry) SetState(appID string, state AppState) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	app, exists := r.apps[appID]
	if !exists {
		return fmt.Errorf("app %s not found", appID)
	}

	app.State = state

	switch state {
	case AppStateRunning:
		app.StartedAt = time.Now()
	case AppStateStopped:
		app.StoppedAt = time.Now()
	}

	return r.save()
}

func (r *Registry) IncrementRestartCount(appID string) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	app, exists := r.apps[appID]
	if !exists {
		return fmt.Errorf("app %s not found", appID)
	}

	app.RestartCount++

	return r.save()
}

func (r *Registry) SetContainerID(appID, containerID string) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	app, exists := r.apps[appID]
	if !exists {
		return fmt.Errorf("app %s not found", appID)
	}

	app.ContainerID = containerID

	return r.save()
}

func (r *Registry) SetWebURL(appID, webURL string) error {
	r.mu.Lock()
	defer r.mu.Unlock()

	app, exists := r.apps[appID]
	if !exists {
		return fmt.Errorf("app %s not found", appID)
	}

	app.WebURL = webURL

	return r.save()
}

func (r *Registry) load() error {
	path := filepath.Join(r.dataPath, "registry.json")

	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			// No existing registry, start fresh
			return nil
		}
		return err
	}

	var apps map[string]*AppInfo
	if err := json.Unmarshal(data, &apps); err != nil {
		return err
	}

	r.apps = apps

	return nil
}

func (r *Registry) save() error {
	path := filepath.Join(r.dataPath, "registry.json")

	data, err := json.MarshalIndent(r.apps, "", "  ")
	if err != nil {
		return err
	}

	return os.WriteFile(path, data, 0644)
}

// ListPlugins returns all registered plugin apps
func (r *Registry) ListPlugins() []*AppInfo {
	r.mu.RLock()
	defer r.mu.RUnlock()

	var plugins []*AppInfo
	for _, app := range r.apps {
		if app.IsPlugin {
			plugins = append(plugins, app)
		}
	}
	return plugins
}

// PluginRegistry manages capability-to-app mappings for plugin discovery
type PluginRegistry struct {
	mu       sync.RWMutex
	registry *Registry
	capToApp map[string]string // capabilityID -> appID
}

// NewPluginRegistry creates a PluginRegistry backed by the app registry
func NewPluginRegistry(reg *Registry) *PluginRegistry {
	pr := &PluginRegistry{
		registry: reg,
		capToApp: make(map[string]string),
	}
	// Rebuild capability map from existing apps
	pr.rebuild()
	return pr
}

// rebuild reconstructs the capability map from the registry
func (pr *PluginRegistry) rebuild() {
	pr.mu.Lock()
	defer pr.mu.Unlock()

	pr.capToApp = make(map[string]string)
	for _, app := range pr.registry.List() {
		if app.IsPlugin {
			for _, cap := range app.Capabilities {
				pr.capToApp[cap.ID] = app.ID
			}
		}
	}
}

// Register registers a plugin's capabilities
func (pr *PluginRegistry) Register(appID string, caps []CapabilityInfo) {
	pr.mu.Lock()
	defer pr.mu.Unlock()

	for _, cap := range caps {
		pr.capToApp[cap.ID] = appID
	}
}

// Unregister removes a plugin's capabilities
func (pr *PluginRegistry) Unregister(appID string) {
	pr.mu.Lock()
	defer pr.mu.Unlock()

	for capID, owner := range pr.capToApp {
		if owner == appID {
			delete(pr.capToApp, capID)
		}
	}
}

// FindByCapability returns the app ID providing a given capability
func (pr *PluginRegistry) FindByCapability(capabilityID string) (string, bool) {
	pr.mu.RLock()
	defer pr.mu.RUnlock()

	appID, ok := pr.capToApp[capabilityID]
	return appID, ok
}

// ListCapabilities returns all registered capabilities and their providers
func (pr *PluginRegistry) ListCapabilities() map[string]string {
	pr.mu.RLock()
	defer pr.mu.RUnlock()

	result := make(map[string]string, len(pr.capToApp))
	for k, v := range pr.capToApp {
		result[k] = v
	}
	return result
}

// FindDependents returns app IDs that depend on a given capability
func (pr *PluginRegistry) FindDependents(capabilityID string) []string {
	apps := pr.registry.List()
	var dependents []string
	for _, app := range apps {
		for _, dep := range app.Dependencies {
			if dep.Capability == capabilityID {
				dependents = append(dependents, app.ID)
				break
			}
		}
	}
	return dependents
}
