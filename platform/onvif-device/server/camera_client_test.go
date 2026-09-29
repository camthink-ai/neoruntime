package main

import (
	"os"
	"path/filepath"
	"testing"

	"gopkg.in/yaml.v3"
)

// sampleDaemonYAML mirrors camera-daemon.yaml's encoders[] shape, plus a
// non-encoder top-level section (rtsp) to assert the atomic rewrite preserves
// unrelated config sections.
const sampleDaemonYAML = `encoders:
    - bitrate: 8000000
      codec: h264
      enabled: true
      fps: 30
      gop: 30
      height: 2160
      stream_name: main
      width: 3840
    - bitrate: 2000000
      codec: h264
      enabled: true
      fps: 30
      gop: 60
      height: 720
      stream_name: sub
      width: 1280
rtsp:
    port: 8554
`

// parsedEncoder is a minimal typed view of camera-daemon.yaml for assertions.
type parsedEncoder struct {
	StreamName string `yaml:"stream_name"`
	Codec      string `yaml:"codec"`
	Width      int    `yaml:"width"`
	Height     int    `yaml:"height"`
	FPS        int    `yaml:"fps"`
	Bitrate    int    `yaml:"bitrate"`
	GOP        int    `yaml:"gop"`
}

type parsedDaemonYAML struct {
	Encoders []parsedEncoder `yaml:"encoders"`
	RTSP     struct {
		Port int `yaml:"port"`
	} `yaml:"rtsp"`
}

func writeTempDaemonYAML(t *testing.T, content string) string {
	t.Helper()
	p := filepath.Join(t.TempDir(), "camera-daemon.yaml")
	if err := os.WriteFile(p, []byte(content), 0o644); err != nil {
		t.Fatalf("write temp yaml: %v", err)
	}
	return p
}

func parseDaemonYAML(t *testing.T, path string) parsedDaemonYAML {
	t.Helper()
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read %s: %v", path, err)
	}
	var got parsedDaemonYAML
	if err := yaml.Unmarshal(data, &got); err != nil {
		t.Fatalf("parse %s: %v", path, err)
	}
	return got
}

func findEncoder(p parsedDaemonYAML, name string) (parsedEncoder, bool) {
	for _, e := range p.Encoders {
		if e.StreamName == name {
			return e, true
		}
	}
	return parsedEncoder{}, false
}

// TestPersistStreamConfig_UpdatesMatchingStream is the core regression: an ONVIF
// Set 4K->1080P must update main in the yaml (so the web /media/config page and a
// reboot reflect it), leave sub untouched, and preserve unrelated sections (rtsp).
func TestPersistStreamConfig_UpdatesMatchingStream(t *testing.T) {
	// Arrange
	p := writeTempDaemonYAML(t, sampleDaemonYAML)

	// Act: BitrateLimit is kbps, so 8000 = 8 Mbps = 8_000_000 bps.
	err := persistStreamConfig(p, "main", streamParams{
		Codec: "H264", Width: 1920, Height: 1080, Fps: 30, BitrateKbps: 8000, Gop: 30,
	})

	// Assert
	if err != nil {
		t.Fatalf("persistStreamConfig: %v", err)
	}
	got := parseDaemonYAML(t, p)

	main, ok := findEncoder(got, "main")
	if !ok {
		t.Fatal("main encoder missing after persist")
	}
	if main.Width != 1920 || main.Height != 1080 {
		t.Errorf("main resolution = %dx%d, want 1920x1080", main.Width, main.Height)
	}
	if main.Bitrate != 8_000_000 {
		t.Errorf("main bitrate = %d, want 8000000 (bps); BitrateKbps must convert *1000", main.Bitrate)
	}
	if main.Codec != "h264" {
		t.Errorf("main codec = %q, want lowercase h264", main.Codec)
	}

	sub, ok := findEncoder(got, "sub")
	if !ok {
		t.Fatal("sub encoder missing after persist")
	}
	if sub.Width != 1280 || sub.Height != 720 || sub.Bitrate != 2_000_000 || sub.GOP != 60 {
		t.Errorf("sub changed unexpectedly: %+v", sub)
	}

	if got.RTSP.Port != 8554 {
		t.Errorf("rtsp.port = %d, want 8554 (unrelated section must survive the rewrite)", got.RTSP.Port)
	}
}

// TestPersistStreamConfig_ZeroFieldsLeftUnchanged enforces the "0 = no change"
// contract shared with platform-api's writeStreamToConfig: a partial ONVIF update
// (only Width sent) must not zero out the other fields.
func TestPersistStreamConfig_ZeroFieldsLeftUnchanged(t *testing.T) {
	// Arrange
	p := writeTempDaemonYAML(t, sampleDaemonYAML)

	// Act: only Width set; Height/Fps/Bitrate/Gop are 0.
	err := persistStreamConfig(p, "main", streamParams{Width: 1920})
	if err != nil {
		t.Fatalf("persistStreamConfig: %v", err)
	}

	// Assert: width updated, height/bitrate/gop preserved.
	got := parseDaemonYAML(t, p)
	main, _ := findEncoder(got, "main")
	if main.Width != 1920 {
		t.Errorf("width = %d, want 1920", main.Width)
	}
	if main.Height != 2160 {
		t.Errorf("height = %d, want 2160 (0 must leave it unchanged)", main.Height)
	}
	if main.Bitrate != 8_000_000 || main.GOP != 30 {
		t.Errorf("bitrate/gop changed: bitrate=%d gop=%d", main.Bitrate, main.GOP)
	}
}

// TestPersistStreamConfig_LandscapeCanonicalization mirrors platform-api's
// canonicalEncoderDims: a portrait-transposed pair (height>width, the
// rotation-residual signature) is stored back as landscape.
func TestPersistStreamConfig_LandscapeCanonicalization(t *testing.T) {
	// Arrange
	p := writeTempDaemonYAML(t, sampleDaemonYAML)

	// Act: send a transposed 1080P (width=1080, height=1920).
	err := persistStreamConfig(p, "main", streamParams{Width: 1080, Height: 1920})
	if err != nil {
		t.Fatalf("persistStreamConfig: %v", err)
	}

	// Assert: stored as landscape 1920x1080.
	got := parseDaemonYAML(t, p)
	main, _ := findEncoder(got, "main")
	if main.Width != 1920 || main.Height != 1080 {
		t.Errorf("portrait pair stored as %dx%d, want canonical 1920x1080", main.Width, main.Height)
	}
}

// TestPersistStreamConfig_EmptyConfigPathNoop confirms persistence is disabled
// (not an error) when no path is configured.
func TestPersistStreamConfig_EmptyConfigPathNoop(t *testing.T) {
	err := persistStreamConfig("", "main", streamParams{Width: 1920, Height: 1080})
	if err != nil {
		t.Errorf("empty configPath should be a no-op, got error: %v", err)
	}
}

// TestPersistStreamConfig_UnknownStreamErrors so an ONVIF Set against a token
// that maps to no yaml entry surfaces a clear error rather than silently
// succeeding (the runtime apply still happened; this only governs persistence).
func TestPersistStreamConfig_UnknownStreamErrors(t *testing.T) {
	p := writeTempDaemonYAML(t, sampleDaemonYAML)
	err := persistStreamConfig(p, "nope", streamParams{Width: 1920})
	if err == nil {
		t.Fatal("expected error for unknown stream, got nil")
	}
}
