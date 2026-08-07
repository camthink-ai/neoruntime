package handlers

import (
	"strings"
	"testing"

	"gopkg.in/yaml.v3"
)

func TestMarshalMediaConfig_NormalizesScientificNotationIntegers(t *testing.T) {
	var config map[string]interface{}
	if err := yaml.Unmarshal([]byte(`encoders:
  - stream_name: main
    codec: h264
    width: 1920
    height: 1080
    fps: 25
    bitrate: 4.032e+06
    gop: 50
  - stream_name: sub
    codec: h264
    width: 640
    height: 360
    fps: 15
    bitrate: 2e+06
    gop: 30
rtsp:
  port: 8.554e+03
watchdog:
  scan_interval_ms: 1e+02
ai_overlay:
  box_thickness: 2e+00
audio:
  sample_rate: 1.6e+04
  channels: 1e+00
  bitrate: 6.4e+04
`), &config); err != nil {
		t.Fatalf("unmarshal config: %v", err)
	}

	out, err := marshalMediaConfig(config)
	if err != nil {
		t.Fatalf("marshal config: %v", err)
	}
	got := string(out)

	if strings.Contains(got, "e+") {
		t.Fatalf("config still contains scientific notation:\n%s", got)
	}
	for _, want := range []string{
		"bitrate: 4032000",
		"bitrate: 2000000",
		"port: 8554",
		"sample_rate: 16000",
		"bitrate: 64000",
	} {
		if !strings.Contains(got, want) {
			t.Fatalf("marshaled config missing %q:\n%s", want, got)
		}
	}

	encoders := extractEncoders(config)
	if len(encoders) != 2 {
		t.Fatalf("got %d encoders, want 2", len(encoders))
	}
	if encoders[0].Bitrate != 4032000 || encoders[1].Bitrate != 2000000 {
		t.Fatalf("bitrates = %d/%d, want 4032000/2000000", encoders[0].Bitrate, encoders[1].Bitrate)
	}
}

func TestValidateMediaConfigEncodersRejectsCollapsedBitrate(t *testing.T) {
	config := map[string]interface{}{
		"encoders": []interface{}{
			map[string]interface{}{
				"stream_name": "main",
				"codec":       "h264",
				"width":       1920,
				"height":      1080,
				"fps":         25,
				"bitrate":     4,
				"gop":         50,
			},
		},
	}

	if err := normalizeMediaConfigNumbers(config); err != nil {
		t.Fatalf("normalize config: %v", err)
	}
	err := validateMediaConfigEncoders(config)
	if err == nil {
		t.Fatal("expected collapsed bitrate to be rejected")
	}
	if !strings.Contains(err.Error(), "below minimum") {
		t.Fatalf("unexpected validation error: %v", err)
	}
}

func TestNormalizeMediaConfigNumbersRejectsFractionalIntegerField(t *testing.T) {
	config := map[string]interface{}{
		"encoders": []interface{}{
			map[string]interface{}{
				"stream_name": "main",
				"fps":         29.97,
			},
		},
	}

	err := normalizeMediaConfigNumbers(config)
	if err == nil {
		t.Fatal("expected fractional fps to be rejected")
	}
	if !strings.Contains(err.Error(), "must be an integer") {
		t.Fatalf("unexpected normalization error: %v", err)
	}
}
