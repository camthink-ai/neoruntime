package handlers

// Unit tests for the unified media-config envelope (media_config_io.go).
//
// clone_test.go covers the device-scope tar transport end to end, but the
// media envelope — the JSON snapshot of base_yaml + seven runtime JSONs — had
// no transport tests of its own. These pin its invariants:
//   - the eight dimensions survive a marshal/unmarshal cycle verbatim
//     (RawMessage is used precisely so float/int are not coerced),
//   - validateMediaEnvelope rejects wrong schema / version / empty payloads,
//   - payloadDims / assignRaw / isEmpty agree on the seven JSON dimensions,
//   - readJSONRaw / atomicWriteJSON are tolerant of miss/invalid input and
//     refuse to write invalid JSON.

import (
	"bytes"
	"encoding/json"
	"os"
	"path/filepath"
	"testing"
)

// TestMediaEnvelopeRoundTrip pins transport fidelity: schema, version and all
// eight config dimensions survive marshal→unmarshal with byte-identical
// RawMessages (no float/int coercion — the reason the dims are RawMessage).
func TestMediaEnvelopeRoundTrip(t *testing.T) {
	in := mediaConfigEnvelope{
		Schema:     mediaConfigSchema,
		Version:    mediaConfigVersion,
		ExportedAt: "2026-08-11T00:00:00Z",
		Device:     map[string]string{"hostname": "cam-1"},
		Config: mediaConfigPayload{
			BaseYAML:     map[string]interface{}{"encoders": map[string]interface{}{"width": 1920.0}},
			Osd:          json.RawMessage(`{"streams":[{"image_overlays":[{"image_path":"/data/aipc/etc/osd/logo.png"}]}]}`),
			PrivacyMask:  json.RawMessage(`{"masks":[{"id":"m1","verts":[]}]}`),
			Transform:    json.RawMessage(`{"flip":"none","mirror":false}`),
			Isp:          json.RawMessage(`{"brightness":0,"contrast":1}`),
			Profile:      json.RawMessage(`{"profile":"indoor","ae_mode":"auto"}`),
			ScalarFields: json.RawMessage(`{"frontend":{"hailort":{"use-hailort-service":true}}}`),
			Lens:         json.RawMessage(`{"iris_target":42}`),
		},
	}
	raw, err := json.MarshalIndent(in, "", "  ")
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	var out mediaConfigEnvelope
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if out.Schema != mediaConfigSchema {
		t.Errorf("schema = %q, want %q", out.Schema, mediaConfigSchema)
	}
	if out.Version != mediaConfigVersion {
		t.Errorf("version = %d, want %d", out.Version, mediaConfigVersion)
	}
	if out.Device["hostname"] != "cam-1" {
		t.Errorf("device hostname lost: %v", out.Device)
	}
	// Each JSON dimension must round-trip verbatim.
	wantDims := map[string]json.RawMessage{
		"osd": in.Config.Osd, "privacy_mask": in.Config.PrivacyMask,
		"transform": in.Config.Transform, "isp": in.Config.Isp,
		"profile": in.Config.Profile, "scalar_fields": in.Config.ScalarFields,
		"lens": in.Config.Lens,
	}
	gotDims := map[string]json.RawMessage{
		"osd": out.Config.Osd, "privacy_mask": out.Config.PrivacyMask,
		"transform": out.Config.Transform, "isp": out.Config.Isp,
		"profile": out.Config.Profile, "scalar_fields": out.Config.ScalarFields,
		"lens": out.Config.Lens,
	}
	for name, want := range wantDims {
		// Compare compact-canonical JSON, not raw bytes: MarshalIndent reformats
		// each RawMessage's whitespace, so byte-inequality is expected even when
		// the JSON value round-trips perfectly.
		if got := mediaCompactRaw(t, gotDims[name]); !bytes.Equal(got, mediaCompactRaw(t, want)) {
			t.Errorf("dimension %q not preserved\ngot:  %s\nwant: %s", name, gotDims[name], want)
		}
	}
	// base_yaml survives as a generic map.
	if out.Config.BaseYAML["encoders"] == nil {
		t.Error("base_yaml lost during round-trip")
	}
}

// TestMediaEnvelopeValidation covers the schema/version/emptiness gates. The
// valid case carries JSON dims only (no base_yaml) so it skips the encoder
// validator (tested separately in media_config_numbers_test.go).
func TestMediaEnvelopeValidation(t *testing.T) {
	validOsd := json.RawMessage(`{"streams":[]}`)
	cases := []struct {
		name    string
		env     mediaConfigEnvelope
		wantErr bool
	}{
		{"wrong schema", mediaConfigEnvelope{Schema: "nope", Version: mediaConfigVersion, Config: mediaConfigPayload{Osd: validOsd}}, true},
		{"wrong version", mediaConfigEnvelope{Schema: mediaConfigSchema, Version: 99, Config: mediaConfigPayload{Osd: validOsd}}, true},
		{"empty payload", mediaConfigEnvelope{Schema: mediaConfigSchema, Version: mediaConfigVersion}, true},
		{"valid json-only", mediaConfigEnvelope{Schema: mediaConfigSchema, Version: mediaConfigVersion, Config: mediaConfigPayload{Osd: validOsd}}, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := validateMediaEnvelope(&tc.env)
			if tc.wantErr && err == nil {
				t.Error("expected validation error, got nil")
			}
			if !tc.wantErr && err != nil {
				t.Errorf("unexpected validation error: %v", err)
			}
		})
	}
}

// TestMediaPayloadDimsAssignEmpty pins the seven-JSON-dimension model:
// payloadDims always reports seven entries, assignRaw resolves each path to its
// field, and isEmpty flips as soon as any dimension (or base_yaml) is set.
func TestMediaPayloadDimsAssignEmpty(t *testing.T) {
	var p mediaConfigPayload
	if !p.isEmpty() {
		t.Error("zero-value payload should be empty")
	}
	if n := len(p.payloadDims()); n != 7 {
		t.Errorf("payloadDims = %d entries, want 7", n)
	}

	// assignRaw maps each of the seven runtime-JSON paths to its field.
	p = mediaConfigPayload{}
	p.assignRaw(osdConfigPath, json.RawMessage(`{}`))
	p.assignRaw(privacyMaskCfgPath, json.RawMessage(`{}`))
	p.assignRaw(transformCfgPath, json.RawMessage(`{}`))
	p.assignRaw(ispCfgPath, json.RawMessage(`{}`))
	p.assignRaw(profileCfgPath, json.RawMessage(`{}`))
	p.assignRaw(scalarFieldsCfgPath, json.RawMessage(`{}`))
	p.assignRaw(lensCfgPath, json.RawMessage(`{}`))
	for i, d := range p.payloadDims() {
		if len(d.raw) == 0 {
			t.Errorf("dim %d (%s) not assigned", i, d.name)
		}
	}
	if p.isEmpty() {
		t.Error("payload with all seven dims set should not be empty")
	}

	// base_yaml alone also makes the payload non-empty.
	if (mediaConfigPayload{BaseYAML: map[string]interface{}{"a": 1}}).isEmpty() {
		t.Error("payload with base_yaml should not be empty")
	}

	// An unknown path is a no-op (defensive: no field silently cleared).
	p2 := mediaConfigPayload{}
	p2.assignRaw("/data/aipc/etc/unknown.json", json.RawMessage(`{}`))
	if !p2.isEmpty() {
		t.Error("unknown path should leave payload empty")
	}
}

// TestReadJSONRawAndAtomicWrite covers the tolerant read and the atomic write:
// missing/invalid files yield nil on read, invalid JSON is refused on write,
// and a valid write is readable back byte-for-byte.
func TestReadJSONRawAndAtomicWrite(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "cfg.json")

	// Missing file → nil (best-effort, mirrors camera-daemon loaders).
	if r := readJSONRaw(path); r != nil {
		t.Errorf("missing file should yield nil, got %s", r)
	}
	// Write valid JSON, read it back verbatim.
	if err := atomicWriteJSON(path, json.RawMessage(`{"k":1}`)); err != nil {
		t.Fatalf("atomicWriteJSON: %v", err)
	}
	if r := readJSONRaw(path); string(r) != `{"k":1}` {
		t.Errorf("read back = %q, want {\"k\":1}", r)
	}
	// Invalid JSON refused on write (no truncated config left behind).
	bad := filepath.Join(dir, "bad.json")
	if err := atomicWriteJSON(bad, json.RawMessage(`{not json`)); err == nil {
		t.Error("atomicWriteJSON accepted invalid JSON")
	}
	if _, err := os.Stat(bad); err == nil {
		t.Error("invalid JSON write left a file behind")
	}
	// Invalid JSON on disk skipped on read (nil, not an abort).
	_ = os.WriteFile(bad, []byte(`{broken`), 0644)
	if r := readJSONRaw(bad); r != nil {
		t.Errorf("invalid JSON on disk should yield nil, got %s", r)
	}
}

// mediaCompactRaw canonicalizes a RawMessage to compact JSON so two dimensions
// compare equal even when MarshalIndent reformats one's whitespace during a
// marshal→unmarshal round-trip.
func mediaCompactRaw(t *testing.T, raw json.RawMessage) []byte {
	t.Helper()
	var buf bytes.Buffer
	if err := json.Compact(&buf, raw); err != nil {
		t.Fatalf("compact invalid JSON %q: %v", raw, err)
	}
	return buf.Bytes()
}
