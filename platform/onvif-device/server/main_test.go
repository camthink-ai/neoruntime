package main

import (
	"testing"

	"aipc/platform/onvif-device/config"
)

// testConfig returns a minimal valid config with one profile for helper tests.
func testConfig() *config.Config {
	return &config.Config{
		Service:  config.ServiceConfig{Enabled: true, HTTPPort: 8081, BasePath: "/onvif"},
		Network:  config.NetworkConfig{Interface: "eth0", MulticastAddr: "239.255.255.250", MulticastPort: 3702},
		Device:   config.DeviceConfig{Manufacturer: "CamThink", Model: "NE503", HardwareID: "NE503", Scopes: []string{"onvif://x"}},
		RTSP:     config.RTSPConfig{Port: 8554},
		Profiles: []config.ProfileConfig{{Token: "main", Name: "Main", Stream: "main", Width: 1920, Height: 1080, FPS: 30, Codec: "H264", Bitrate: 4096}},
		Auth:     config.AuthConfig{Mode: "none"},
	}
}

func TestRtspURI(t *testing.T) {
	// Arrange / Act
	got := rtspURI("10.0.0.5", testConfig(), config.ProfileConfig{Stream: "sub"})

	// Assert
	if got != "rtsp://10.0.0.5:8554/sub" {
		t.Errorf("rtspURI = %q, want rtsp://10.0.0.5:8554/sub", got)
	}
}

func TestDeviceXAddr(t *testing.T) {
	// Arrange / Act
	got := deviceXAddr("10.0.0.5", testConfig())

	// Assert
	if got != "http://10.0.0.5:8081/onvif/device_service" {
		t.Errorf("deviceXAddr = %q, want http://10.0.0.5:8081/onvif/device_service", got)
	}
}

func TestEnvOr(t *testing.T) {
	// Arrange / Act / Assert — present env var wins.
	t.Setenv("ONVIF_TEST_VAR", "from-env")
	if got := envOr("ONVIF_TEST_VAR", "fallback"); got != "from-env" {
		t.Errorf("envOr(present) = %q, want from-env", got)
	}
	// Absent env var falls back.
	if got := envOr("ONVIF_TEST_UNSET", "fallback"); got != "fallback" {
		t.Errorf("envOr(absent) = %q, want fallback", got)
	}
}

func TestResolveAuth(t *testing.T) {
	cases := []struct {
		name string
		mode string
		cfg  config.AuthConfig
		env  map[string]string
		want [2]string // username, password
	}{
		{
			name: "none-mode-no-auth",
			mode: "none",
			want: [2]string{"", ""},
		},
		{
			name: "none-mode-ignores-config-creds",
			mode: "none",
			cfg:  config.AuthConfig{Mode: "none", Username: "cfg", Password: "cfg"},
			want: [2]string{"", ""},
		},
		{
			name: "digest-uses-env-creds",
			mode: "digest",
			env:  map[string]string{"AIPC_AUTH_USERNAME": "envuser", "AIPC_AUTH_PASSWORD": "envpass"},
			want: [2]string{"envuser", "envpass"},
		},
		{
			name: "digest-falls-back-to-config-creds",
			mode: "digest",
			cfg:  config.AuthConfig{Mode: "digest", Username: "cfguser", Password: "cfgpass"},
			want: [2]string{"cfguser", "cfgpass"},
		},
		{
			name: "digest-missing-all-creds-disables-auth",
			mode: "digest",
			want: [2]string{"", ""},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			// Arrange
			t.Setenv("AIPC_AUTH_USERNAME", "")
			t.Setenv("AIPC_AUTH_PASSWORD", "")
			for k, v := range tc.env {
				t.Setenv(k, v)
			}
			cfg := testConfig()
			cfg.Auth = tc.cfg
			if cfg.Auth.Mode == "" {
				cfg.Auth.Mode = tc.mode
			}

			// Act
			user, pass := resolveAuth(cfg)

			// Assert
			if user != tc.want[0] || pass != tc.want[1] {
				t.Errorf("resolveAuth() = (%q,%q), want (%q,%q)", user, pass, tc.want[0], tc.want[1])
			}
		})
	}
}

func TestBuildServerConfig_MapsIdentityAndProfiles(t *testing.T) {
	// Arrange
	cfg := testConfig()

	// Act
	sc := buildServerConfig(cfg, "CT503-0001", "9.9.9")

	// Assert
	if sc.Host != "0.0.0.0" {
		t.Errorf("Host = %q, want 0.0.0.0", sc.Host)
	}
	if sc.Port != 8081 {
		t.Errorf("Port = %d, want 8081", sc.Port)
	}
	if sc.BasePath != "/onvif" {
		t.Errorf("BasePath = %q, want /onvif", sc.BasePath)
	}
	if sc.DeviceInfo.SerialNumber != "CT503-0001" {
		t.Errorf("SerialNumber = %q, want CT503-0001", sc.DeviceInfo.SerialNumber)
	}
	if sc.DeviceInfo.FirmwareVersion != "9.9.9" {
		t.Errorf("FirmwareVersion = %q, want 9.9.9", sc.DeviceInfo.FirmwareVersion)
	}
	if sc.SupportPTZ || sc.SupportImaging || sc.SupportEvents {
		t.Errorf("Phase 1 must disable PTZ/Imaging/Events; got PTZ=%v Imaging=%v Events=%v",
			sc.SupportPTZ, sc.SupportImaging, sc.SupportEvents)
	}
	if len(sc.Profiles) != 1 {
		t.Fatalf("Profiles len = %d, want 1", len(sc.Profiles))
	}
	if sc.Profiles[0].VideoEncoder.Encoding != "H264" {
		t.Errorf("profile codec = %q, want H264", sc.Profiles[0].VideoEncoder.Encoding)
	}
}

func TestBuildDiscoveryConfig_AssemblesXAddrAndScopes(t *testing.T) {
	// Arrange
	cfg := testConfig()

	// Act
	dc := buildDiscoveryConfig(cfg, "urn:uuid:EP", "10.0.0.5")

	// Assert
	if dc.EndpointUUID != "urn:uuid:EP" {
		t.Errorf("EndpointUUID = %q", dc.EndpointUUID)
	}
	if dc.Types != "dp0:NetworkVideoTransmitter" {
		t.Errorf("Types = %q, want NetworkVideoTransmitter", dc.Types)
	}
	if len(dc.XAddrs) != 1 || dc.XAddrs[0] != "http://10.0.0.5:8081/onvif/device_service" {
		t.Errorf("XAddrs = %v, want [http://10.0.0.5:8081/onvif/device_service]", dc.XAddrs)
	}
	if len(dc.Scopes) != 1 || dc.Scopes[0] != "onvif://x" {
		t.Errorf("Scopes = %v", dc.Scopes)
	}
}
