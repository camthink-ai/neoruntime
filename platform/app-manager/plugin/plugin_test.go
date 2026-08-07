package plugin

import (
	"os"
	"path/filepath"
	"testing"
)

func TestDiscoveryManager(t *testing.T) {
	dir := t.TempDir()

	dm, err := NewDiscoveryManager(dir)
	if err != nil {
		t.Fatalf("NewDiscoveryManager: %v", err)
	}

	// Register a plugin
	entry := DiscoveryEntry{
		AppID:   "plugin-rtsp",
		Version: "1.0.0",
		State:   "running",
		Capabilities: []DiscoveryCapability{
			{
				ID:        "rtsp-server",
				Version:   "1.0",
				Transport: "both",
				GRPC: &DiscoveryGRPC{
					SocketPath: "/run/aipc/plugins/plugin-rtsp.sock",
					Service:    "rtsp.RtspService",
				},
				Event: &DiscoveryEvent{
					Publish: []string{"plugin/rtsp/stream-status"},
				},
			},
		},
	}

	if err := dm.RegisterPlugin(entry); err != nil {
		t.Fatalf("RegisterPlugin: %v", err)
	}

	// Verify file written
	path := filepath.Join(dir, DiscoveryFile)
	if _, err := os.Stat(path); err != nil {
		t.Fatalf("discovery.json not created: %v", err)
	}

	// Find by capability
	results := dm.FindByCapability("rtsp-server")
	if len(results) != 1 {
		t.Fatalf("FindByCapability: expected 1 result, got %d", len(results))
	}
	if results[0].AppID != "plugin-rtsp" {
		t.Fatalf("FindByCapability: expected plugin-rtsp, got %s", results[0].AppID)
	}

	// Update state
	if err := dm.SetPluginState("plugin-rtsp", "stopped"); err != nil {
		t.Fatalf("SetPluginState: %v", err)
	}
	e, ok := dm.GetPlugin("plugin-rtsp")
	if !ok {
		t.Fatal("GetPlugin: plugin-rtsp not found after state update")
	}
	if e.State != "stopped" {
		t.Fatalf("State: expected stopped, got %s", e.State)
	}

	// List
	all := dm.ListPlugins()
	if len(all) != 1 {
		t.Fatalf("ListPlugins: expected 1, got %d", len(all))
	}

	// Unregister
	if err := dm.UnregisterPlugin("plugin-rtsp"); err != nil {
		t.Fatalf("UnregisterPlugin: %v", err)
	}
	all = dm.ListPlugins()
	if len(all) != 0 {
		t.Fatalf("ListPlugins after unregister: expected 0, got %d", len(all))
	}

	// Reload from file
	dm2, err := NewDiscoveryManager(dir)
	if err != nil {
		t.Fatalf("NewDiscoveryManager reload: %v", err)
	}
	all2 := dm2.ListPlugins()
	if len(all2) != 0 {
		t.Fatalf("ListPlugins after reload: expected 0, got %d", len(all2))
	}

	t.Log("OK: DiscoveryManager all tests passed")
}
