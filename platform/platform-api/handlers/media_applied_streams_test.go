package handlers

import (
	"os"
	"testing"

	camerapb "aipc/platform/camera-daemon/proto"
)

// streamCfg is a compact helper for building applied-stream entries.
func streamCfg(id string, w, h, bitrate, fps, gop uint32, codec string) *camerapb.PipelineStreamConfig {
	return &camerapb.PipelineStreamConfig{
		StreamId:         id,
		EncoderWidth:     w,
		EncoderHeight:    h,
		EncoderBitrate:   bitrate,
		EncoderFramerate: fps,
		EncoderGop:       gop,
		Codec:            codec,
	}
}

// readEncoders loads the encoders list from the persisted yaml as a
// name→entry map (order is asserted separately where it matters).
func readEncoders(t *testing.T, path string) map[string]map[string]interface{} {
	t.Helper()
	m := readYamlMap(t, path)
	raw, ok := m["encoders"].([]interface{})
	if !ok {
		t.Fatalf("encoders list missing or not a list: %T", m["encoders"])
	}
	out := make(map[string]map[string]interface{}, len(raw))
	for _, item := range raw {
		e, ok := item.(map[string]interface{})
		if !ok {
			t.Fatalf("non-map encoder entry: %T", item)
		}
		name, _ := e["stream_name"].(string)
		out[name] = e
	}
	return out
}

// TestWriteAppliedStreamsToConfigRebuild pins the AppliedStreams rebuild of
// writeAppliedStreamsToConfig: update-in-place preserves unrecognized fields
// (qp_min/qp_max/rc_mode/output_pool_max_buffers), reconfigure-added
// streams are materialized as full entries, and streams dropped from the
// applied layout are removed so they cannot resurrect as boot zombies.
func TestWriteAppliedStreamsToConfigRebuild(t *testing.T) {
	path := writeTempYaml(t, `encoders:
  - stream_name: main
    width: 3840
    height: 2160
    codec: h264
    bitrate: 12288
    fps: 30
    gop: 60
    qp_min: 8
    qp_max: 42
    rc_mode: cbr
    output_pool_max_buffers: 6
  - stream_name: sub
    width: 1280
    height: 720
    codec: h264
    bitrate: 2048
    fps: 25
    gop: 50
`)
	h := &MediaHandlers{configPath: path} // configMgr nil → direct os.WriteFile

	// Reconfigure drops sub and adds third.
	h.writeAppliedStreamsToConfig([]*camerapb.PipelineStreamConfig{
		streamCfg("main", 3840, 2160, 12288, 30, 60, "h264"),
		streamCfg("third", 640, 384, 512, 15, 30, "h264"),
	})

	got := readEncoders(t, path)
	if len(got) != 2 {
		t.Fatalf("want 2 encoders after rebuild, got %d: %v", len(got), got)
	}

	// main: updated in place — extra encoder fields must survive.
	mainE, ok := got["main"]
	if !ok {
		t.Fatal("main entry missing after rebuild")
	}
	for _, f := range []string{"qp_min", "qp_max", "rc_mode", "output_pool_max_buffers"} {
		if _, present := mainE[f]; !present {
			t.Errorf("main lost field %q during update-in-place: %v", f, mainE)
		}
	}
	if mainE["bitrate"] != 12288 {
		t.Errorf("main bitrate = %v, want 12288", mainE["bitrate"])
	}

	// third: materialized as a full new entry with enabled defaulting true.
	thirdE, ok := got["third"]
	if !ok {
		t.Fatal("reconfigure-added stream 'third' not persisted (would boot without its encoder)")
	}
	if thirdE["enabled"] != true {
		t.Errorf("third enabled = %v, want explicit true", thirdE["enabled"])
	}
	if thirdE["width"] != 640 || thirdE["height"] != 384 || thirdE["fps"] != 15 {
		t.Errorf("third dims/fps = %v/%v/%v, want 640/384/15", thirdE["width"], thirdE["height"], thirdE["fps"])
	}

	// sub: dropped from the applied layout → must be gone (no boot zombie).
	if _, still := got["sub"]; still {
		t.Error("removed stream 'sub' still persisted — would resurrect as a zombie encoder at boot")
	}
}

// TestWriteAppliedStreamsToConfigKeepsDisabledStubs pins the H1 review fix:
// DisableStream persists an enabled:false stub so EnableStream can re-create
// the encoder later. Disabled streams never appear in AppliedStreams, so the
// rebuild must retain enabled:false entries — dropping them broke the
// disable→reconfigure→enable round-trip with a 404 on re-enable.
func TestWriteAppliedStreamsToConfigKeepsDisabledStubs(t *testing.T) {
	path := writeTempYaml(t, `encoders:
  - stream_name: main
    width: 3840
    height: 2160
    codec: h264
    bitrate: 12288
    fps: 30
  - stream_name: sub
    enabled: false
    width: 1280
    height: 720
    codec: h264
    bitrate: 2048
    fps: 25
`)
	h := &MediaHandlers{configPath: path}

	// Reconfigure naming only main (sub disabled → absent from AppliedStreams).
	h.writeAppliedStreamsToConfig([]*camerapb.PipelineStreamConfig{
		streamCfg("main", 3840, 2160, 12288, 30, 60, "h264"),
	})

	got := readEncoders(t, path)
	subE, ok := got["sub"]
	if !ok {
		t.Fatal("disabled stub 'sub' dropped by rebuild — disable→reconfigure→enable round-trip broken (H1 regression)")
	}
	if enabled, _ := subE["enabled"].(bool); enabled {
		t.Error("sub stub enabled=true; disable state must be preserved")
	}
	if subE["bitrate"] != 2048 {
		t.Errorf("sub stub lost params: bitrate = %v, want 2048 (EnableStream re-creates from these)", subE["bitrate"])
	}
}

// TestWriteAppliedStreamsToConfigCanonicalizesPortraitDims: applied dims come
// from live codec contexts, which report rotated geometry while a portrait
// transform is active — the persist must store the canonical landscape pair.
func TestWriteAppliedStreamsToConfigCanonicalizesPortraitDims(t *testing.T) {
	path := writeTempYaml(t, "encoders: []\n")
	h := &MediaHandlers{configPath: path}

	h.writeAppliedStreamsToConfig([]*camerapb.PipelineStreamConfig{
		streamCfg("sub", 720, 1280, 2048, 25, 50, "h264"), // portrait echo
	})

	got := readEncoders(t, path)
	subE := got["sub"]
	if subE["width"] != 1280 || subE["height"] != 720 {
		t.Errorf("portrait dims persisted as %v x %v, want canonical 1280 x 720",
			subE["width"], subE["height"])
	}
}

// TestWriteAppliedStreamsToConfigDegradedYaml: encoders: null / absent and
// non-map entries must not block persisting an otherwise-valid applied layout.
func TestWriteAppliedStreamsToConfigDegradedYaml(t *testing.T) {
	t.Run("encoders key absent", func(t *testing.T) {
		path := writeTempYaml(t, "media:\n  sensor: default\n")
		h := &MediaHandlers{configPath: path}
		h.writeAppliedStreamsToConfig([]*camerapb.PipelineStreamConfig{
			streamCfg("main", 3840, 2160, 12288, 30, 60, "h264"),
		})
		got := readEncoders(t, path)
		if _, ok := got["main"]; !ok {
			t.Fatal("ADD path blocked by absent encoders key — stream would never persist")
		}
		// Non-encoders sections must survive the rewrite.
		m := readYamlMap(t, path)
		media, _ := m["media"].(map[string]interface{})
		if media == nil || media["sensor"] != "default" {
			t.Errorf("unrelated 'media' section lost: %v", m["media"])
		}
	})

	t.Run("non-map entries skipped", func(t *testing.T) {
		path := writeTempYaml(t, `encoders:
  - "garbage string entry"
  - 42
  - stream_name: main
    width: 3840
    height: 2160
`)
		h := &MediaHandlers{configPath: path}
		h.writeAppliedStreamsToConfig([]*camerapb.PipelineStreamConfig{
			streamCfg("main", 3840, 2160, 12288, 30, 60, "h264"),
		})
		got := readEncoders(t, path)
		if len(got) != 1 || got["main"] == nil {
			t.Fatalf("want exactly the main entry after rebuild, got %v", got)
		}
	})
}

// TestWriteAppliedStreamsToConfigEmptyGuard: an empty applied slice is never a
// legitimate "drop all" (the HTTP layer requires 1-4 streams; nil routes to
// the YAML-reload fallback at the caller) — the file must be left untouched.
func TestWriteAppliedStreamsToConfigEmptyGuard(t *testing.T) {
	path := writeTempYaml(t, `encoders:
  - stream_name: main
    width: 3840
    height: 2160
`)
	before, _ := os.ReadFile(path)

	h := &MediaHandlers{configPath: path}
	h.writeAppliedStreamsToConfig([]*camerapb.PipelineStreamConfig{})

	after, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read back: %v", err)
	}
	if string(before) != string(after) {
		t.Error("empty applied slice rewrote the config — guard against accidental drop-all missing")
	}
}
