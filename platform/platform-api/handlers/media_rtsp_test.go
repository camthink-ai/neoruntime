package handlers

import (
	"context"
	"os"
	"path/filepath"
	"testing"

	"github.com/gin-gonic/gin"
	"gopkg.in/yaml.v3"
)

// writeTempYaml seeds a temp camera-daemon.yaml with the given content and
// returns its path. Tests pass it as the handler configPath with configMgr nil
// so writeback uses the direct os.WriteFile fallback (deterministic, no DB).
func writeTempYaml(t *testing.T, content string) string {
	t.Helper()
	dir := t.TempDir()
	p := filepath.Join(dir, "camera-daemon.yaml")
	if err := os.WriteFile(p, []byte(content), 0644); err != nil {
		t.Fatalf("seed yaml: %v", err)
	}
	return p
}

// readYamlMap loads a yaml file into a generic map for field assertions.
func readYamlMap(t *testing.T, path string) map[string]interface{} {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read yaml: %v", err)
	}
	var m map[string]interface{}
	if err := yaml.Unmarshal(data, &m); err != nil {
		t.Fatalf("unmarshal yaml: %v", err)
	}
	return m
}

// TestWriteRtspConfig_TogglesEnabledAndPreservesRest seeds a yaml with RTSP on
// and other top-level sections, flips RTSP off, and asserts only rtsp.enabled
// changed while sibling sections (audio, ai_overlay) are untouched.
func TestWriteRtspConfig_TogglesEnabledAndPreservesRest(t *testing.T) {
	gin.SetMode(gin.TestMode)
	path := writeTempYaml(t, `rtsp:
  enabled: true
audio:
  enabled: true
  capture_device: "default"
ai_overlay:
  enabled: true
  box_thickness: 2
`)
	h := &MediaHandlers{configPath: path}

	h.writeRtspConfig(context.Background(), "alice", false)

	m := readYamlMap(t, path)
	rtsp, ok := m["rtsp"].(map[string]interface{})
	if !ok || rtsp["enabled"] != false {
		t.Fatalf("rtsp.enabled = %v, want false", rtsp["enabled"])
	}
	// Sibling sections preserved (read-modify-write did not clobber them).
	if audio, ok := m["audio"].(map[string]interface{}); !ok || audio["capture_device"] != "default" {
		t.Fatalf("audio section corrupted: %+v", m["audio"])
	}
	if ov, ok := m["ai_overlay"].(map[string]interface{}); !ok || ov["box_thickness"] != 2 {
		t.Fatalf("ai_overlay section corrupted: %+v", m["ai_overlay"])
	}
}

// TestWriteRtspConfig_CreatesSectionIfMissing asserts the helper tolerates a
// yaml that has no rtsp section yet (creates it) rather than dropping the flag.
func TestWriteRtspConfig_CreatesSectionIfMissing(t *testing.T) {
	gin.SetMode(gin.TestMode)
	path := writeTempYaml(t, `audio:
  enabled: true
`)
	h := &MediaHandlers{configPath: path}

	h.writeRtspConfig(context.Background(), "", true)

	m := readYamlMap(t, path)
	rtsp, ok := m["rtsp"].(map[string]interface{})
	if !ok || rtsp["enabled"] != true {
		t.Fatalf("rtsp.enabled = %v, want true (section should be created)", rtsp["enabled"])
	}
}
