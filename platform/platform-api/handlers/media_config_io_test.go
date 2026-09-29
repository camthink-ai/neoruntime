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
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gin-gonic/gin"
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

// TestProjectMediaConfigHoldsApplyLock pins the P0 fix: projectMediaConfig must
// acquire the shared configApplyMu — the same lock the device-clone import
// (ImportDeviceConfig → applyTree) relies on to serialize against media writes.
// If a future change drops that acquisition, a clone restore could overwrite
// /data/aipc/etc mid-edit. We pre-hold configApplyMu, assert projectMediaConfig
// blocks, then release and assert it completes and writes.
func TestProjectMediaConfigHoldsApplyLock(t *testing.T) {
	path := writeTempYaml(t, "encoders:\n  width: 1920\n")
	h := &MediaHandlers{configPath: path} // configMgr nil → direct os.WriteFile

	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	// Pre-hold the shared apply lock so projectMediaConfig must block.
	configApplyMu.Lock()

	done := make(chan struct{})
	go func() {
		// With configApplyMu held by the test, this should block on the lock.
		_ = h.projectMediaConfig(ctx, "tester", "encoders:\n  width: 1280\n")
		close(done)
	}()

	select {
	case <-done:
		configApplyMu.Unlock()
		t.Fatal("projectMediaConfig returned while configApplyMu was held by the test — it did not acquire the shared apply lock (P0 regression)")
	case <-time.After(150 * time.Millisecond):
		// expected: blocked on configApplyMu
	}

	// Release → projectMediaConfig should now run to completion.
	configApplyMu.Unlock()

	select {
	case <-done:
		// good
	case <-time.After(2 * time.Second):
		t.Fatal("projectMediaConfig did not complete after configApplyMu was released")
	}

	// Sanity: the write actually happened (proves it ran the real body, not a no-op).
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read back written config: %v", err)
	}
	if !bytes.Contains(got, []byte("1280")) {
		t.Errorf("config not written after lock release; got %q", got)
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

// TestExportMediaConfigStreamsBareEnvelope pins the export→import transport
// contract: GET /config/export streams the bare envelope (schema at the top
// level), NOT an APIResponse {code,data} wrapper. The wrapper would be saved
// verbatim by the client and arrive at /config/import as `unsupported schema ""`
// because ShouldBindJSON binds schema from the top level. So the response body
// must unmarshal directly into mediaConfigEnvelope and then pass the same
// validation import runs — a clean round-trip with no client transformation.
func TestExportMediaConfigStreamsBareEnvelope(t *testing.T) {
	path := writeTempYaml(t, "encoders:\n  width: 1920\n  height: 1080\n")
	h := &MediaHandlers{configPath: path}

	gin.SetMode(gin.TestMode)
	engine := gin.New()
	engine.GET("/export", h.ExportMediaConfig)

	req := httptest.NewRequest(http.MethodGet, "/export", nil)
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d; body=%s", w.Code, http.StatusOK, w.Body.String())
	}
	if ct := w.Header().Get("Content-Type"); !strings.HasPrefix(ct, "application/json") {
		t.Fatalf("Content-Type = %q, want application/json", ct)
	}
	if cd := w.Header().Get("Content-Disposition"); !strings.Contains(cd, "attachment") {
		t.Fatalf("Content-Disposition = %q, want an attachment download", cd)
	}

	// The body must be the bare envelope: unmarshaling directly into
	// mediaConfigEnvelope yields the schema at the top level. A wrapped
	// {code,data} response would leave Schema empty here — the original bug.
	var env mediaConfigEnvelope
	if err := json.Unmarshal(w.Body.Bytes(), &env); err != nil {
		t.Fatalf("unmarshal export body: %v; body=%s", err, w.Body.String())
	}
	if env.Schema != mediaConfigSchema {
		t.Fatalf("schema = %q, want %q (export wrapped under {code,data}?); body=%s",
			env.Schema, mediaConfigSchema, w.Body.String())
	}
	if env.Version != mediaConfigVersion {
		t.Errorf("version = %d, want %d", env.Version, mediaConfigVersion)
	}
	// The exported body must pass the exact validation import runs.
	if err := validateMediaEnvelope(&env); err != nil {
		t.Fatalf("exported envelope fails import validation: %v", err)
	}
}
