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

// fakeTransformDaemon is an in-process CameraControl server that only implements
// the transform RPCs. It returns a fixed "current" config from Get and records
// the last Set call so the handler's read-modify-write merge can be asserted.
type fakeTransformDaemon struct {
	camerapb.UnimplementedCameraControlServer
	mu       sync.Mutex
	current  camerapb.TransformConfig
	lastSet  camerapb.TransformConfig
	setCalls int
}

func (f *fakeTransformDaemon) GetTransformConfig(context.Context, *camerapb.Empty) (*camerapb.TransformConfig, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	cp := f.current
	return &cp, nil
}

func (f *fakeTransformDaemon) SetTransformConfig(_ context.Context, req *camerapb.TransformConfig) (*camerapb.Status, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.lastSet = *req
	f.current = *req
	f.setCalls++
	return &camerapb.Status{Success: true, Message: "ok"}, nil
}

func (f *fakeTransformDaemon) snapshot() (camerapb.TransformConfig, int) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.lastSet, f.setCalls
}

// newTransformHandlerWithFakeDaemon wires UpdateTransformConfig to a bufconn
// gRPC server backed by fake, returning the handler + fake for assertions.
func newTransformHandlerWithFakeDaemon(t *testing.T, baseline camerapb.TransformConfig) (*MediaHandlers, *fakeTransformDaemon) {
	t.Helper()
	lis := bufconn.Listen(1024 * 1024)
	fake := &fakeTransformDaemon{current: baseline}
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

func putTransform(t *testing.T, h *MediaHandlers, body string) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	engine := gin.New()
	engine.PUT("/transform", h.UpdateTransformConfig)
	req := httptest.NewRequest(http.MethodPut, "/transform", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)
	return w
}

// baseline is fully non-default so any silent reset is observable.
var transformBaseline = camerapb.TransformConfig{Rotation: 2, Flip: 1, Dewarp: true, Grayscale: true}

// Regression for bug (2): toggling grayscale must not reset the other params.
func TestUpdateTransform_PartialGrayscalePreservesOthers(t *testing.T) {
	h, fake := newTransformHandlerWithFakeDaemon(t, transformBaseline)

	w := putTransform(t, h, `{"grayscale":false}`)
	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", w.Code, w.Body.String())
	}
	got, calls := fake.snapshot()
	if calls != 1 {
		t.Fatalf("expected exactly 1 SetTransformConfig call, got %d", calls)
	}
	if got.Rotation != 2 || got.Flip != 1 || got.Dewarp != true || got.Grayscale != false {
		t.Errorf("merge lost fields: %+v (want rotation=2 flip=1 dewarp=true grayscale=false)", got)
	}
}

// Regression for bug (3): toggling dewarp must not reset the other params.
func TestUpdateTransform_PartialDewarpPreservesOthers(t *testing.T) {
	h, fake := newTransformHandlerWithFakeDaemon(t, transformBaseline)

	w := putTransform(t, h, `{"dewarp":false}`)
	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", w.Code, w.Body.String())
	}
	got, calls := fake.snapshot()
	if calls != 1 {
		t.Fatalf("expected exactly 1 SetTransformConfig call, got %d", calls)
	}
	if got.Rotation != 2 || got.Flip != 1 || got.Dewarp != false || got.Grayscale != true {
		t.Errorf("merge lost fields: %+v (want rotation=2 flip=1 dewarp=false grayscale=true)", got)
	}
}

// Regression for bug (1): changing flip must not reset rotation (or others).
func TestUpdateTransform_PartialFlipPreservesRotation(t *testing.T) {
	h, fake := newTransformHandlerWithFakeDaemon(t, transformBaseline)

	w := putTransform(t, h, `{"flip":2}`)
	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", w.Code, w.Body.String())
	}
	got, _ := fake.snapshot()
	if got.Rotation != 2 || got.Flip != 2 || got.Dewarp != true || got.Grayscale != true {
		t.Errorf("merge lost fields: %+v (want rotation=2 flip=2 dewarp=true grayscale=true)", got)
	}
}

// Regression for bug (1): changing rotation must not reset flip (or others).
func TestUpdateTransform_PartialRotationPreservesFlip(t *testing.T) {
	h, fake := newTransformHandlerWithFakeDaemon(t, transformBaseline)

	w := putTransform(t, h, `{"rotation":1}`)
	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", w.Code, w.Body.String())
	}
	got, _ := fake.snapshot()
	if got.Rotation != 1 || got.Flip != 1 || got.Dewarp != true || got.Grayscale != true {
		t.Errorf("merge lost fields: %+v (want rotation=1 flip=1 dewarp=true grayscale=true)", got)
	}
}

// Sanity: a full request is honored verbatim (no merge needed).
func TestUpdateTransform_FullRequestHonored(t *testing.T) {
	h, fake := newTransformHandlerWithFakeDaemon(t, transformBaseline)

	w := putTransform(t, h, `{"rotation":0,"flip":0,"dewarp":false,"grayscale":false}`)
	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", w.Code, w.Body.String())
	}
	got, _ := fake.snapshot()
	if got.Rotation != 0 || got.Flip != 0 || got.Dewarp != false || got.Grayscale != false {
		t.Errorf("full request not honored: %+v", got)
	}
}
