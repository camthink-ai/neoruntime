package handlers

import (
	"context"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"

	camerapb "aipc/platform/camera-daemon/proto"
	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"
)

type fakePrivacyMaskDaemon struct {
	camerapb.UnimplementedCameraControlServer
	mu       sync.Mutex
	current  camerapb.PrivacyMaskConfig
	lastSet  camerapb.PrivacyMaskConfig
	setCalls int
}

func (f *fakePrivacyMaskDaemon) GetPrivacyMaskConfig(context.Context, *camerapb.Empty) (*camerapb.PrivacyMaskConfig, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	cp := f.current
	return &cp, nil
}

func (f *fakePrivacyMaskDaemon) SetPrivacyMaskConfig(_ context.Context, req *camerapb.PrivacyMaskConfig) (*camerapb.Status, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.lastSet = *req
	f.current = *req
	f.setCalls++
	return &camerapb.Status{Success: true, Message: "ok"}, nil
}

func (f *fakePrivacyMaskDaemon) snapshot() (camerapb.PrivacyMaskConfig, int) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.lastSet, f.setCalls
}

func newPrivacyMaskHandlerWithFakeDaemon(t *testing.T, baseline camerapb.PrivacyMaskConfig) (*MediaHandlers, *fakePrivacyMaskDaemon) {
	t.Helper()
	lis := bufconn.Listen(1024 * 1024)
	fake := &fakePrivacyMaskDaemon{current: baseline}
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

	return NewMediaHandlers("", conn, nil, nil), fake
}

func putPrivacyMask(t *testing.T, h *MediaHandlers, body string) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	engine := gin.New()
	engine.PUT("/privacy-mask", h.UpdatePrivacyMaskConfig)
	req := httptest.NewRequest(http.MethodPut, "/privacy-mask", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)
	return w
}

func TestUpdatePrivacyMaskAcceptsCamelCaseDPMFields(t *testing.T) {
	h, fake := newPrivacyMaskHandlerWithFakeDaemon(t, camerapb.PrivacyMaskConfig{
		Enabled:    false,
		Color:      1,
		BlurRadius: 2,
		DpmMode:    "mosaic",
		DpmColor:   0x111111,
	})

	w := putPrivacyMask(t, h, `{"dpmEnabled":true,"dpmLabels":"person,face","dpmMode":"overlay","dpmColor":65280}`)
	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", w.Code, w.Body.String())
	}
	got, calls := fake.snapshot()
	if calls != 1 {
		t.Fatalf("expected one SetPrivacyMaskConfig call, got %d", calls)
	}
	if !got.DpmEnabled || got.DpmLabels != "person,face" || got.DpmMode != "overlay" || got.DpmColor != 65280 {
		t.Fatalf("DPM fields not applied: %+v", got)
	}
}

func TestUpdatePrivacyMaskRejectsUnknownFields(t *testing.T) {
	h, fake := newPrivacyMaskHandlerWithFakeDaemon(t, camerapb.PrivacyMaskConfig{})

	w := putPrivacyMask(t, h, `{"dpm_enabledd":true}`)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d body=%s", w.Code, w.Body.String())
	}
	_, calls := fake.snapshot()
	if calls != 0 {
		t.Fatalf("unexpected SetPrivacyMaskConfig calls: %d", calls)
	}
}

func TestUpdatePrivacyMaskRejectsNonJSONContentType(t *testing.T) {
	h, fake := newPrivacyMaskHandlerWithFakeDaemon(t, camerapb.PrivacyMaskConfig{})

	gin.SetMode(gin.TestMode)
	engine := gin.New()
	engine.PUT("/privacy-mask", h.UpdatePrivacyMaskConfig)
	req := httptest.NewRequest(http.MethodPut, "/privacy-mask", strings.NewReader(`{"dpmEnabled":true}`))
	req.Header.Set("Content-Type", "text/plain")
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)

	if w.Code != http.StatusUnsupportedMediaType {
		t.Fatalf("expected 415, got %d body=%s", w.Code, w.Body.String())
	}
	_, calls := fake.snapshot()
	if calls != 0 {
		t.Fatalf("unexpected SetPrivacyMaskConfig calls: %d", calls)
	}
}
