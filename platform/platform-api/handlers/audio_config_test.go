package handlers

import (
	"context"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"

	camerapb "aipc/platform/camera-daemon/proto"
)

// fakeAudioDaemon records the last SetAudioConfig and always succeeds.
type fakeAudioDaemon struct {
	camerapb.UnimplementedCameraControlServer
	mu      sync.Mutex
	lastSet *camerapb.AudioConfigRequest
}

func (f *fakeAudioDaemon) SetAudioConfig(_ context.Context, req *camerapb.AudioConfigRequest) (*camerapb.Status, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.lastSet = req
	return &camerapb.Status{Success: true, Message: "ok"}, nil
}

func (f *fakeAudioDaemon) snapshot() *camerapb.AudioConfigRequest {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.lastSet
}

// newAudioHandlerWithFakeDaemon wires SetConfig to a bufconn gRPC server and a
// temp yaml so persistence can be asserted after a successful gRPC call.
func newAudioHandlerWithFakeDaemon(t *testing.T, yamlContent string) (*AudioHandlers, *fakeAudioDaemon, string) {
	t.Helper()
	lis := bufconn.Listen(1024 * 1024)
	fake := &fakeAudioDaemon{}
	srv := grpc.NewServer()
	camerapb.RegisterCameraControlServer(srv, fake)
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)

	conn, err := grpc.DialContext(context.Background(), "bufnet",
		grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) { return lis.Dial() }),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		t.Fatalf("dial bufnet: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	path := writeTempYaml(t, yamlContent)
	return NewAudioHandlers(conn, path, nil), fake, path
}

func putAudioConfig(t *testing.T, h *AudioHandlers, body string) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	engine := gin.New()
	engine.PUT("/config", h.SetConfig)
	req := httptest.NewRequest(http.MethodPut, "/config", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)
	return w
}

const audioBaseline = `audio:
  enabled: true
  capture_device: "default"
  sample_rate: 48000
  channels: 1
  codec: pcm
  bitrate: 128000
  volume: 1.0
  mute: false
`

// asFloat coerces a yaml-decoded scalar (int or float) to float64 so volume
// assertions survive yaml's int/float round-tripping (1.0 decodes as int 1).
func asFloat(v interface{}) float64 {
	switch n := v.(type) {
	case int:
		return float64(n)
	case int64:
		return float64(n)
	case float64:
		return n
	case float32:
		return float64(n)
	}
	return 0
}

// TestWriteAudioConfig_FullUpdatePersistsAllFields asserts a full config
// request writes every field (with device→capture_device translation) and
// preserves audio.enabled.
func TestWriteAudioConfig_FullUpdatePersistsAllFields(t *testing.T) {
	gin.SetMode(gin.TestMode)
	path := writeTempYaml(t, audioBaseline)
	h := &AudioHandlers{configPath: path}

	vol := float32(0.5)
	mute := true
	h.writeAudioConfig(context.Background(), "carol", audioConfigReq{
		Device: "hw:1,0", SampleRate: 16000, Channels: 2, Codec: "aac", Bitrate: 64000,
		Volume: &vol, Mute: &mute,
	})

	a := readYamlMap(t, path)["audio"].(map[string]interface{})
	if a["capture_device"] != "hw:1,0" {
		t.Errorf("capture_device = %v, want hw:1,0 (translated from device)", a["capture_device"])
	}
	if a["sample_rate"] != 16000 || a["channels"] != 2 || a["bitrate"] != 64000 {
		t.Errorf("numeric fields = sr=%v ch=%v br=%v, want 16000/2/64000", a["sample_rate"], a["channels"], a["bitrate"])
	}
	if a["codec"] != "aac" {
		t.Errorf("codec = %v, want aac", a["codec"])
	}
	if asFloat(a["volume"]) != 0.5 {
		t.Errorf("volume = %v, want 0.5", a["volume"])
	}
	if a["mute"] != true {
		t.Errorf("mute = %v, want true", a["mute"])
	}
	if a["enabled"] != true {
		t.Errorf("enabled = %v, want preserved true (request has no enabled field)", a["enabled"])
	}
}

// TestWriteAudioConfig_PointerGateVolumeOnly asserts a partial request that
// carries ONLY volume (no device/codec/etc, mute nil) does not zero the other
// fields — the value-type fields are gated on non-zero/non-empty.
func TestWriteAudioConfig_PointerGateVolumeOnly(t *testing.T) {
	gin.SetMode(gin.TestMode)
	path := writeTempYaml(t, audioBaseline)
	h := &AudioHandlers{configPath: path}

	vol := float32(0.25)
	h.writeAudioConfig(context.Background(), "", audioConfigReq{Volume: &vol})

	a := readYamlMap(t, path)["audio"].(map[string]interface{})
	if asFloat(a["volume"]) != 0.25 {
		t.Errorf("volume = %v, want 0.25", a["volume"])
	}
	// Untouched value fields must retain baseline.
	if a["capture_device"] != "default" {
		t.Errorf("capture_device = %v, want untouched 'default'", a["capture_device"])
	}
	if a["sample_rate"] != 48000 || a["channels"] != 1 || a["codec"] != "pcm" || a["bitrate"] != 128000 {
		t.Errorf("baseline numeric/codec fields corrupted by partial request: %+v", a)
	}
	if a["mute"] != false {
		t.Errorf("mute = %v, want untouched false (mute pointer was nil)", a["mute"])
	}
}

// TestWriteAudioConfig_PointerGateMuteOnly asserts mute-only (nil volume) flips
// mute and leaves volume intact.
func TestWriteAudioConfig_PointerGateMuteOnly(t *testing.T) {
	gin.SetMode(gin.TestMode)
	path := writeTempYaml(t, audioBaseline)
	h := &AudioHandlers{configPath: path}

	mute := true
	h.writeAudioConfig(context.Background(), "", audioConfigReq{Mute: &mute})

	a := readYamlMap(t, path)["audio"].(map[string]interface{})
	if a["mute"] != true {
		t.Errorf("mute = %v, want true", a["mute"])
	}
	if asFloat(a["volume"]) != 1.0 {
		t.Errorf("volume = %v, want untouched 1.0 (volume pointer was nil)", a["volume"])
	}
}

// TestSetConfig_PersistsAfterGrpcSuccess wires a fake gRPC daemon + temp yaml,
// sends a full PUT, and asserts the gRPC call succeeded AND the yaml was
// written (end-to-end handler path, not just the helper).
func TestSetConfig_PersistsAfterGrpcSuccess(t *testing.T) {
	h, fake, path := newAudioHandlerWithFakeDaemon(t, audioBaseline)

	w := putAudioConfig(t, h, `{"device":"hw:2,0","sample_rate":8000,"channels":1,"codec":"opus","bitrate":32000,"volume":0.8,"mute":false}`)
	if w.Code != http.StatusOK {
		t.Fatalf("status = %d body=%s, want 200", w.Code, w.Body.String())
	}
	if got := fake.snapshot(); got == nil || got.GetDevice() != "hw:2,0" {
		t.Fatalf("gRPC SetAudioConfig not invoked as expected: %+v", got)
	}
	a := readYamlMap(t, path)["audio"].(map[string]interface{})
	if a["capture_device"] != "hw:2,0" || a["codec"] != "opus" || asFloat(a["volume"]) != 0.8 {
		t.Fatalf("yaml not persisted after successful gRPC: %+v", a)
	}
}
