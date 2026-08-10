// Package config loads the onvif-device service configuration.
//
// The onvif-device service exposes the NE503 AIPC as an ONVIF Profile S
// device. It is a thin signalling layer: it answers WS-Discovery and SOAP
// (Device/Media) requests and hands out the RTSP URIs served by
// camera-daemon. Video never flows through this service.
package config

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"gopkg.in/yaml.v3"

	"aipc/platform/common/constants"
)

// Config is the full onvif-device configuration loaded from onvif.yaml.
type Config struct {
	Service     ServiceConfig   `yaml:"service"`
	Network     NetworkConfig   `yaml:"network"`
	Device      DeviceConfig    `yaml:"device"`
	RTSP        RTSPConfig      `yaml:"rtsp"`
	Profiles    []ProfileConfig `yaml:"profiles"`
	Auth        AuthConfig      `yaml:"auth"`
	VersionFile string          `yaml:"version_file"`
}

// ServiceConfig holds the SOAP/HTTP server settings.
type ServiceConfig struct {
	Enabled  bool   `yaml:"enabled"`
	HTTPPort int    `yaml:"http_port"`
	BasePath string `yaml:"base_path"`
	LogLevel string `yaml:"log_level"`
}

// NetworkConfig holds the WS-Discovery multicast settings.
type NetworkConfig struct {
	Interface     string `yaml:"interface"`
	MulticastAddr string `yaml:"multicast_addr"`
	MulticastPort int    `yaml:"multicast_port"`
}

// DeviceConfig holds the advertised device identity and ONVIF scopes.
type DeviceConfig struct {
	Manufacturer    string   `yaml:"manufacturer"`
	Model           string   `yaml:"model"`
	FirmwareVersion string   `yaml:"firmware_version"`
	SerialNumber    string   `yaml:"serial_number"`
	HardwareID      string   `yaml:"hardware_id"`
	Scopes          []string `yaml:"scopes"`
}

// RTSPConfig points at the camera-daemon RTSP server.
type RTSPConfig struct {
	Port int `yaml:"port"`
}

// ProfileConfig maps an ONVIF media profile to a camera-daemon RTSP stream.
type ProfileConfig struct {
	Token   string `yaml:"token"`
	Name    string `yaml:"name"`
	Stream  string `yaml:"stream"` // RTSP path component, e.g. "main"
	Width   int    `yaml:"width"`
	Height  int    `yaml:"height"`
	FPS     int    `yaml:"fps"`
	Codec   string `yaml:"codec"`
	Bitrate int    `yaml:"bitrate"`
}

// AuthConfig controls WS-Security UsernameToken enforcement.
//
// The onvif-go SOAP handler enforces digest auth only when both Username and
// Password are non-empty. Leave both empty for "none" mode, which maximises
// NVR onboarding interoperability (many clients call GetCapabilities before
// they can compute a digest).
type AuthConfig struct {
	Mode     string `yaml:"mode"` // "none" (default) or "digest"
	Username string `yaml:"username"`
	Password string `yaml:"password"`
}

// Default returns the built-in NE503 defaults. Values mirror the stream
// layout documented in docs/services/CAMERA_DAEMON_DESIGN.md.
func Default() *Config {
	return &Config{
		Service: ServiceConfig{
			Enabled:  true,
			HTTPPort: 8081,
			BasePath: "/onvif",
			LogLevel: "info",
		},
		Network: NetworkConfig{
			Interface:     "eth0",
			MulticastAddr: "239.255.255.250",
			MulticastPort: 3702,
		},
		Device: DeviceConfig{
			Manufacturer: "CamThink",
			Model:        "NE503",
			HardwareID:   "NE503",
			Scopes: []string{
				"onvif://www.onvif.org/Profile/Streaming",
				"onvif://www.onvif.org/Profile/T",
				"onvif://www.onvif.org/name/CamThink/NE503",
			},
		},
		RTSP: RTSPConfig{Port: 8554},
		Profiles: []ProfileConfig{
			{Token: "main", Name: "Main Stream", Stream: "main", Width: 1920, Height: 1080, FPS: 30, Codec: "H264", Bitrate: 4096},
			{Token: "sub", Name: "Sub Stream", Stream: "sub", Width: 1280, Height: 720, FPS: 30, Codec: "H264", Bitrate: 2048},
		},
		Auth:        AuthConfig{Mode: "none"},
		VersionFile: filepath.Join(constants.RootPath(), "VERSION"),
	}
}

// Load reads a YAML config file and applies defaults for any missing fields.
// A missing file is not an error: the defaults are returned so the service
// can run on a freshly flashed device before /data/aipc/etc/onvif.yaml exists.
func Load(path string) (*Config, error) {
	cfg := Default()

	data, err := os.ReadFile(path)
	if err != nil {
		if os.IsNotExist(err) {
			return cfg, nil
		}
		return nil, fmt.Errorf("read config %s: %w", path, err)
	}

	if err := yaml.Unmarshal(data, cfg); err != nil {
		return nil, fmt.Errorf("parse config %s: %w", path, err)
	}

	applyDefaults(cfg)
	return cfg, nil
}

// applyDefaults fills in derived/empty fields after unmarshalling.
func applyDefaults(cfg *Config) {
	if cfg.Service.HTTPPort == 0 {
		cfg.Service.HTTPPort = 8081
	}
	if cfg.Service.BasePath == "" {
		cfg.Service.BasePath = "/onvif"
	}
	if cfg.Network.Interface == "" {
		cfg.Network.Interface = "eth0"
	}
	if cfg.Network.MulticastAddr == "" {
		cfg.Network.MulticastAddr = "239.255.255.250"
	}
	if cfg.Network.MulticastPort == 0 {
		cfg.Network.MulticastPort = 3702
	}
	if cfg.RTSP.Port == 0 {
		cfg.RTSP.Port = 8554
	}
	if cfg.VersionFile == "" {
		cfg.VersionFile = filepath.Join(constants.RootPath(), "VERSION")
	}
	if cfg.Auth.Mode == "" {
		cfg.Auth.Mode = "none"
	}
	cfg.Auth.Mode = strings.ToLower(cfg.Auth.Mode)

	for i := range cfg.Profiles {
		p := &cfg.Profiles[i]
		if p.Codec == "" {
			p.Codec = "H264"
		}
		p.Codec = strings.ToUpper(p.Codec)
		if p.Bitrate == 0 {
			p.Bitrate = 4096
		}
		if p.FPS == 0 {
			p.FPS = 30
		}
		if p.Stream == "" {
			p.Stream = p.Token
		}
	}
}
