package config

import (
	"os"
	"path/filepath"
	"testing"
)

// TestLoad_MissingFileReturnsDefaults verifies a fresh device (no onvif.yaml)
// still gets working built-in defaults rather than an error.
func TestLoad_MissingFileReturnsDefaults(t *testing.T) {
	// Arrange
	missing := filepath.Join(t.TempDir(), "does-not-exist.yaml")

	// Act
	cfg, err := Load(missing)

	// Assert
	if err != nil {
		t.Fatalf("Load missing file returned error: %v", err)
	}
	if !cfg.Service.Enabled {
		t.Errorf("Service.Enabled default = false, want true")
	}
	if cfg.Service.HTTPPort != 8081 {
		t.Errorf("HTTPPort = %d, want 8081", cfg.Service.HTTPPort)
	}
	if cfg.Service.BasePath != "/onvif" {
		t.Errorf("BasePath = %q, want /onvif", cfg.Service.BasePath)
	}
	if cfg.Network.MulticastAddr != "239.255.255.250" || cfg.Network.MulticastPort != 3702 {
		t.Errorf("multicast = %s:%d, want 239.255.255.250:3702", cfg.Network.MulticastAddr, cfg.Network.MulticastPort)
	}
	if cfg.RTSP.Port != 8554 {
		t.Errorf("RTSP.Port = %d, want 8554", cfg.RTSP.Port)
	}
	if cfg.Auth.Mode != "none" {
		t.Errorf("Auth.Mode = %q, want none", cfg.Auth.Mode)
	}
	if len(cfg.Profiles) != 2 || cfg.Profiles[0].Token != "main" || cfg.Profiles[1].Token != "sub" {
		t.Errorf("Profiles = %+v, want [main, sub]", cfg.Profiles)
	}
}

// TestLoad_ParsesOverrides verifies user values win and empty fields fall back
// to defaults (codec uppercased, stream defaults to token, bitrate/fps filled).
func TestLoad_ParsesOverrides(t *testing.T) {
	// Arrange
	path := filepath.Join(t.TempDir(), "onvif.yaml")
	content := `
service:
  http_port: 9090
network:
  interface: wlan0
rtsp:
  port: 8555
device:
  manufacturer: Acme
profiles:
  - token: third
    name: Third
    width: 640
    height: 384
    codec: h265
auth:
  mode: DIGEST
  username: viewer
`
	if err := os.WriteFile(path, []byte(content), 0600); err != nil {
		t.Fatalf("write temp config: %v", err)
	}

	// Act
	cfg, err := Load(path)

	// Assert
	if err != nil {
		t.Fatalf("Load: %v", err)
	}
	if cfg.Service.HTTPPort != 9090 {
		t.Errorf("HTTPPort = %d, want 9090", cfg.Service.HTTPPort)
	}
	if cfg.Network.Interface != "wlan0" {
		t.Errorf("Interface = %q, want wlan0", cfg.Network.Interface)
	}
	if cfg.RTSP.Port != 8555 {
		t.Errorf("RTSP.Port = %d, want 8555", cfg.RTSP.Port)
	}
	if cfg.Device.Manufacturer != "Acme" {
		t.Errorf("Manufacturer = %q, want Acme", cfg.Device.Manufacturer)
	}
	if cfg.Auth.Mode != "digest" {
		t.Errorf("Auth.Mode not lowercased = %q, want digest", cfg.Auth.Mode)
	}
	if cfg.Auth.Username != "viewer" {
		t.Errorf("Auth.Username = %q, want viewer", cfg.Auth.Username)
	}

	// applyDefaults fills derived fields on the single profile.
	if len(cfg.Profiles) != 1 {
		t.Fatalf("Profiles len = %d, want 1", len(cfg.Profiles))
	}
	p := cfg.Profiles[0]
	if p.Codec != "H265" {
		t.Errorf("Codec not uppercased = %q, want H265", p.Codec)
	}
	if p.Stream != "third" { // stream defaulted to token when unset
		t.Errorf("Stream = %q, want third (default = token)", p.Stream)
	}
	if p.FPS != 30 { // fps defaulted
		t.Errorf("FPS = %d, want 30 (default)", p.FPS)
	}
	if p.Bitrate != 4096 { // bitrate defaulted
		t.Errorf("Bitrate = %d, want 4096 (default)", p.Bitrate)
	}
}

// TestLoad_InvalidYamlReturnsError verifies a malformed file is a hard error,
// not silently swallowed into defaults.
func TestLoad_InvalidYamlReturnsError(t *testing.T) {
	// Arrange
	path := filepath.Join(t.TempDir(), "bad.yaml")
	if err := os.WriteFile(path, []byte("service: [not valid yaml {{{"), 0600); err != nil {
		t.Fatalf("write: %v", err)
	}

	// Act
	_, err := Load(path)

	// Assert
	if err == nil {
		t.Fatal("Load invalid yaml returned nil error, want parse error")
	}
}
