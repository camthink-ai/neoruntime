package manifest

import (
	"os"
	"testing"
)

const testPluginManifest = `
apiVersion: v1
kind: Application
metadata:
  id: plugin-rtsp
  name: RTSP Streaming Server
  version: 1.0.0
spec:
  image: registry.local/plugin-rtsp:1.0.0
  resources:
    cpu: "25%"
    memory: "128Mi"
    shm: true
  plugin:
    capabilities:
      - id: rtsp-server
        version: "1.0"
        transport: both
        description: RTSP streaming server
        proto: "rtsp.RtspService"
        topics:
          publish:
            - "plugin/rtsp/stream-status"
  permissions:
    video:
      - cam0_main
    events:
      publish:
        - "plugin/rtsp/**"
    network:
      mode: host
      inbound:
        - 8554
  healthcheck:
    enabled: true
    type: tcp
    port: 8554
`

const testConsumerManifest = `
apiVersion: v1
kind: Application
metadata:
  id: my-recorder
  name: Video Recorder
  version: 1.0.0
spec:
  image: registry.local/recorder:1.0
  plugin_dependencies:
    - capability: rtsp-server
      min_version: "1.0"
      required: true
`

func TestPluginManifestParsing(t *testing.T) {
	// Write temp file
	f, err := os.CreateTemp("", "manifest-*.yaml")
	if err != nil {
		t.Fatal(err)
	}
	defer os.Remove(f.Name())
	f.WriteString(testPluginManifest)
	f.Close()

	m, err := LoadManifest(f.Name())
	if err != nil {
		t.Fatalf("LoadManifest: %v", err)
	}

	if !m.IsPlugin() {
		t.Fatal("IsPlugin() should return true")
	}
	if !m.IsHostNetwork() {
		t.Fatal("IsHostNetwork() should return true")
	}
	if m.HasPluginDependencies() {
		t.Fatal("HasPluginDependencies() should return false for plugin manifest")
	}
	if len(m.Spec.Plugin.Capabilities) != 1 {
		t.Fatalf("Expected 1 capability, got %d", len(m.Spec.Plugin.Capabilities))
	}

	cap := m.Spec.Plugin.Capabilities[0]
	if cap.ID != "rtsp-server" {
		t.Fatalf("Capability ID: expected rtsp-server, got %s", cap.ID)
	}
	if cap.Transport != "both" {
		t.Fatalf("Transport: expected both, got %s", cap.Transport)
	}
	if cap.Proto != "rtsp.RtspService" {
		t.Fatalf("Proto: expected rtsp.RtspService, got %s", cap.Proto)
	}

	if len(m.Spec.Permissions.Network.Inbound) != 1 || m.Spec.Permissions.Network.Inbound[0] != 8554 {
		t.Fatalf("Inbound: expected [8554], got %v", m.Spec.Permissions.Network.Inbound)
	}

	socketPath := m.GetPluginSocketPath()
	if socketPath != "/run/aipc/plugins/plugin-rtsp.sock" {
		t.Fatalf("SocketPath: expected /run/aipc/plugins/plugin-rtsp.sock, got %s", socketPath)
	}

	t.Log("OK: Plugin manifest parsed and validated")
}

func TestConsumerManifestParsing(t *testing.T) {
	f, err := os.CreateTemp("", "manifest-*.yaml")
	if err != nil {
		t.Fatal(err)
	}
	defer os.Remove(f.Name())
	f.WriteString(testConsumerManifest)
	f.Close()

	m, err := LoadManifest(f.Name())
	if err != nil {
		t.Fatalf("LoadManifest: %v", err)
	}

	if m.IsPlugin() {
		t.Fatal("IsPlugin() should return false for consumer")
	}
	if !m.HasPluginDependencies() {
		t.Fatal("HasPluginDependencies() should return true")
	}
	if len(m.Spec.PluginDependencies) != 1 {
		t.Fatalf("Expected 1 dependency, got %d", len(m.Spec.PluginDependencies))
	}

	dep := m.Spec.PluginDependencies[0]
	if dep.Capability != "rtsp-server" {
		t.Fatalf("Dependency capability: expected rtsp-server, got %s", dep.Capability)
	}
	if !dep.Required {
		t.Fatal("Dependency should be required")
	}

	t.Log("OK: Consumer manifest parsed and validated")
}

func TestInvalidNetworkMode(t *testing.T) {
	yaml := `
apiVersion: v1
kind: Application
metadata:
  id: bad-app
  name: Bad App
  version: 1.0.0
spec:
  image: registry.local/bad:1.0
  permissions:
    network:
      inbound:
        - 8080
`
	f, _ := os.CreateTemp("", "manifest-*.yaml")
	defer os.Remove(f.Name())
	f.WriteString(yaml)
	f.Close()

	_, err := LoadManifest(f.Name())
	if err == nil {
		t.Fatal("Expected validation error for inbound without host mode")
	}
	t.Logf("OK: Correctly rejected: %v", err)
}
