package plugin

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"sync"
	"time"
)

const (
	DiscoveryDir  = "/run/aipc/plugins"
	DiscoveryFile = "discovery.json"
)

// DiscoveryEntry represents one plugin in the discovery file
type DiscoveryEntry struct {
	AppID        string                `json:"app_id"`
	Version      string                `json:"version"`
	State        string                `json:"state"` // "running", "stopped"
	Capabilities []DiscoveryCapability `json:"capabilities"`
	UpdatedAt    time.Time             `json:"updated_at"`
}

// DiscoveryCapability describes a single capability endpoint
type DiscoveryCapability struct {
	ID        string          `json:"id"`
	Version   string          `json:"version"`
	Transport string          `json:"transport"`
	GRPC      *DiscoveryGRPC  `json:"grpc,omitempty"`
	Event     *DiscoveryEvent `json:"event,omitempty"`
}

// DiscoveryGRPC describes the gRPC endpoint for a capability
type DiscoveryGRPC struct {
	SocketPath string `json:"socket_path"`
	Service    string `json:"service"`
}

// DiscoveryEvent describes event topics for a capability
type DiscoveryEvent struct {
	Publish   []string `json:"publish"`
	Subscribe []string `json:"subscribe"`
}

// DiscoveryData is the root structure of discovery.json
type DiscoveryData struct {
	Version   string                    `json:"version"`
	UpdatedAt time.Time                 `json:"updated_at"`
	Plugins   map[string]DiscoveryEntry `json:"plugins"` // keyed by app_id
}

// DiscoveryManager manages the discovery.json file atomically
type DiscoveryManager struct {
	mu   sync.Mutex
	dir  string
	data *DiscoveryData
}

// NewDiscoveryManager creates a new DiscoveryManager
func NewDiscoveryManager(dir string) (*DiscoveryManager, error) {
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, fmt.Errorf("failed to create discovery dir: %w", err)
	}

	dm := &DiscoveryManager{
		dir: dir,
		data: &DiscoveryData{
			Version: "1",
			Plugins: make(map[string]DiscoveryEntry),
		},
	}

	// Load existing discovery file
	if err := dm.load(); err != nil {
		// Not fatal: start fresh
		_ = err
	}

	return dm, nil
}

// RegisterPlugin adds or updates a plugin entry
func (dm *DiscoveryManager) RegisterPlugin(entry DiscoveryEntry) error {
	dm.mu.Lock()
	defer dm.mu.Unlock()

	entry.UpdatedAt = time.Now()
	dm.data.Plugins[entry.AppID] = entry
	return dm.save()
}

// UnregisterPlugin removes a plugin entry
func (dm *DiscoveryManager) UnregisterPlugin(appID string) error {
	dm.mu.Lock()
	defer dm.mu.Unlock()

	delete(dm.data.Plugins, appID)
	return dm.save()
}

// SetPluginState updates the state of a plugin
func (dm *DiscoveryManager) SetPluginState(appID, state string) error {
	dm.mu.Lock()
	defer dm.mu.Unlock()

	entry, ok := dm.data.Plugins[appID]
	if !ok {
		return fmt.Errorf("plugin %s not found in discovery", appID)
	}

	entry.State = state
	entry.UpdatedAt = time.Now()
	dm.data.Plugins[appID] = entry
	return dm.save()
}

// GetPlugin returns a plugin entry by app ID
func (dm *DiscoveryManager) GetPlugin(appID string) (DiscoveryEntry, bool) {
	dm.mu.Lock()
	defer dm.mu.Unlock()

	entry, ok := dm.data.Plugins[appID]
	return entry, ok
}

// FindByCapability returns plugins providing a specific capability
func (dm *DiscoveryManager) FindByCapability(capabilityID string) []DiscoveryEntry {
	dm.mu.Lock()
	defer dm.mu.Unlock()

	var results []DiscoveryEntry
	for _, entry := range dm.data.Plugins {
		for _, cap := range entry.Capabilities {
			if cap.ID == capabilityID {
				results = append(results, entry)
				break
			}
		}
	}
	return results
}

// ListPlugins returns all registered plugins
func (dm *DiscoveryManager) ListPlugins() map[string]DiscoveryEntry {
	dm.mu.Lock()
	defer dm.mu.Unlock()

	result := make(map[string]DiscoveryEntry, len(dm.data.Plugins))
	for k, v := range dm.data.Plugins {
		result[k] = v
	}
	return result
}

// save writes discovery.json atomically (tmp + rename)
func (dm *DiscoveryManager) save() error {
	dm.data.UpdatedAt = time.Now()

	data, err := json.MarshalIndent(dm.data, "", "  ")
	if err != nil {
		return fmt.Errorf("failed to marshal discovery data: %w", err)
	}

	targetPath := filepath.Join(dm.dir, DiscoveryFile)
	tmpPath := targetPath + ".tmp"

	if err := os.WriteFile(tmpPath, data, 0644); err != nil {
		return fmt.Errorf("failed to write discovery temp file: %w", err)
	}

	if err := os.Rename(tmpPath, targetPath); err != nil {
		os.Remove(tmpPath)
		return fmt.Errorf("failed to rename discovery file: %w", err)
	}

	return nil
}

// load reads the existing discovery.json file
func (dm *DiscoveryManager) load() error {
	path := filepath.Join(dm.dir, DiscoveryFile)

	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return nil
		}
		return err
	}

	var dd DiscoveryData
	if err := json.Unmarshal(data, &dd); err != nil {
		return err
	}

	dm.data = &dd
	if dm.data.Plugins == nil {
		dm.data.Plugins = make(map[string]DiscoveryEntry)
	}

	return nil
}
