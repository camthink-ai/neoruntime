package handlers

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"
	"google.golang.org/grpc"

	camerapb "aipc/platform/camera-daemon/proto"
)

// ============================================================
// Mock talkPlaybackClient + talkStreamClient
// ============================================================

type mockTalkStream struct {
	mu     sync.Mutex
	chunks []*camerapb.AudioPcmChunk
	closed bool
}

func (m *mockTalkStream) Send(c *camerapb.AudioPcmChunk) error {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.chunks = append(m.chunks, c)
	return nil
}

func (m *mockTalkStream) CloseAndRecv() (*camerapb.Status, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.closed = true
	return &camerapb.Status{Success: true}, nil
}

func (m *mockTalkStream) snapshot() []*camerapb.AudioPcmChunk {
	m.mu.Lock()
	defer m.mu.Unlock()
	out := make([]*camerapb.AudioPcmChunk, len(m.chunks))
	copy(out, m.chunks)
	return out
}

// mockTalkClient records Start/Stop calls and serves a mock PCM stream.
type mockTalkClient struct {
	mu sync.Mutex

	startReqs []*camerapb.AudioConfigRequest
	stopCalls int

	// Configurable outcomes.
	startStatus *camerapb.Status // returned by StartAudioPlayback (nil → success)
	startErr    error
	streamErr   error

	stream *mockTalkStream
	stopCh chan struct{} // closed when StopAudioPlayback is invoked
}

func newMockTalkClient() *mockTalkClient {
	return &mockTalkClient{
		stream: &mockTalkStream{},
		stopCh: make(chan struct{}),
	}
}

func (m *mockTalkClient) StartAudioPlayback(_ context.Context, in *camerapb.AudioConfigRequest, _ ...grpc.CallOption) (*camerapb.Status, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.startReqs = append(m.startReqs, in)
	if m.startErr != nil {
		return nil, m.startErr
	}
	if m.startStatus != nil {
		return m.startStatus, nil
	}
	return &camerapb.Status{Success: true, Message: "ok"}, nil
}

func (m *mockTalkClient) StopAudioPlayback(_ context.Context, _ *camerapb.Empty, _ ...grpc.CallOption) (*camerapb.Status, error) {
	m.mu.Lock()
	defer m.mu.Unlock()
	m.stopCalls++
	select {
	case <-m.stopCh:
	default:
		close(m.stopCh)
	}
	return &camerapb.Status{Success: true}, nil
}

func (m *mockTalkClient) StreamAudioPcm(_ context.Context, _ ...grpc.CallOption) (talkStreamClient, error) {
	if m.streamErr != nil {
		return nil, m.streamErr
	}
	return m.stream, nil
}

func (m *mockTalkClient) startCount() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return len(m.startReqs)
}
func (m *mockTalkClient) stopCount() int {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.stopCalls
}

// drainTalkSem removes any held talk slot so tests start from a clean state.
func drainTalkSem() {
	for {
		select {
		case <-talkSem:
		default:
			return
		}
	}
}

// newTalkTestServer wires the handler behind a real httptest server so the
// gorilla WS dialer can connect. Returns the server.
func newTalkTestServer(t *testing.T, mock *mockTalkClient) *httptest.Server {
	t.Helper()
	gin.SetMode(gin.TestMode)
	h := newAudioHandlersWithTalk(mock)
	engine := gin.New()
	engine.GET("/api/v1/audio/talk", h.HandleAudioTalkWebSocket)
	return httptest.NewServer(engine)
}

// dialTalk connects a WebSocket client to the test server's /audio/talk.
func dialTalk(t *testing.T, srv *httptest.Server) *websocket.Conn {
	t.Helper()
	u := "ws" + strings.TrimPrefix(srv.URL, "http") + "/api/v1/audio/talk"
	dialer := websocket.Dialer{HandshakeTimeout: 2 * time.Second}
	conn, _, err := dialer.Dial(u, nil)
	if err != nil {
		t.Fatalf("dial talk ws: %v", err)
	}
	return conn
}

// waitForStop blocks until the mock records a StopAudioPlayback call or times out.
func waitForStop(t *testing.T, mock *mockTalkClient) {
	t.Helper()
	select {
	case <-mock.stopCh:
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for StopAudioPlayback")
	}
}

// ============================================================
// Tests
// ============================================================

func TestAudioTalk_NoClient_ReturnsServiceUnavailable(t *testing.T) {
	drainTalkSem()
	gin.SetMode(gin.TestMode)
	h := &AudioHandlers{} // no camera client, no override

	engine := gin.New()
	engine.GET("/api/v1/audio/talk", h.HandleAudioTalkWebSocket)

	req := httptest.NewRequest(http.MethodGet, "/api/v1/audio/talk", nil)
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)

	if w.Code != http.StatusServiceUnavailable {
		t.Fatalf("expected 503, got %d body=%s", w.Code, w.Body.String())
	}
}

func TestAudioTalk_SecondSessionRejectedAsConflict(t *testing.T) {
	drainTalkSem()
	mock := newMockTalkClient()
	srv := newTalkTestServer(t, mock)
	defer srv.Close()

	// Occupy the single slot from another "session".
	talkSem <- struct{}{}
	defer drainTalkSem()

	// A real dial must still complete the HTTP handshake; the handler rejects
	// before upgrading, so the dial returns an error carrying the HTTP response.
	u := "ws" + strings.TrimPrefix(srv.URL, "http") + "/api/v1/audio/talk"
	_, resp, err := (&websocket.Dialer{HandshakeTimeout: 2 * time.Second}).Dial(u, nil)
	if err == nil {
		t.Fatal("expected dial to fail (handshake rejected), but it succeeded")
	}
	if resp == nil {
		t.Fatal("expected an HTTP response from the failed dial")
	}
	if resp.StatusCode != http.StatusTooManyRequests {
		t.Fatalf("expected 429 Too Many Requests, got %d", resp.StatusCode)
	}

	// Must not have touched the device at all.
	if got := mock.startCount(); got != 0 {
		t.Errorf("expected no StartAudioPlayback calls, got %d", got)
	}
	if got := mock.stopCount(); got != 0 {
		t.Errorf("expected no StopAudioPlayback calls, got %d", got)
	}
}

func TestAudioTalk_StartPlaybackFailure_ReturnsCameraError(t *testing.T) {
	drainTalkSem()
	mock := newMockTalkClient()
	mock.startStatus = &camerapb.Status{Success: false, Message: "device busy"}
	srv := newTalkTestServer(t, mock)
	defer srv.Close()

	u := "ws" + strings.TrimPrefix(srv.URL, "http") + "/api/v1/audio/talk"
	_, resp, err := (&websocket.Dialer{HandshakeTimeout: 2 * time.Second}).Dial(u, nil)
	if err == nil {
		t.Fatal("expected dial to fail, but it succeeded")
	}
	if resp == nil || resp.StatusCode != http.StatusInternalServerError {
		t.Fatalf("expected 500, got %+v", resp)
	}

	// Failed start must not trigger a stop (nothing to stop).
	if got := mock.stopCount(); got != 0 {
		t.Errorf("expected no StopAudioPlayback on start failure, got %d", got)
	}
}

func TestAudioTalk_ForwardsBinaryFramesInOrderAndStops(t *testing.T) {
	drainTalkSem()
	mock := newMockTalkClient()
	srv := newTalkTestServer(t, mock)
	defer srv.Close()

	conn := dialTalk(t, srv)

	// Send three binary frames; intersperse a text frame that must be ignored.
	frames := [][]byte{
		[]byte("frame-0"),
		[]byte("ignored-text"),
		[]byte("frame-1"),
		[]byte("frame-2"),
	}
	if err := conn.WriteMessage(websocket.BinaryMessage, frames[0]); err != nil {
		t.Fatalf("write frame 0: %v", err)
	}
	if err := conn.WriteMessage(websocket.TextMessage, frames[1]); err != nil {
		t.Fatalf("write text frame: %v", err)
	}
	if err := conn.WriteMessage(websocket.BinaryMessage, frames[2]); err != nil {
		t.Fatalf("write frame 1: %v", err)
	}
	if err := conn.WriteMessage(websocket.BinaryMessage, frames[3]); err != nil {
		t.Fatalf("write frame 2: %v", err)
	}

	// Close the client → handler's ReadMessage errors → teardown path runs.
	if err := conn.WriteMessage(websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.CloseNormalClosure, "bye")); err != nil {
		t.Fatalf("write close: %v", err)
	}
	_ = conn.Close()

	waitForStop(t, mock)

	// Exactly one Start and one Stop.
	if got, want := mock.startCount(), 1; got != want {
		t.Errorf("StartAudioPlayback calls = %d, want %d", got, want)
	}
	if got, want := mock.stopCount(), 1; got != want {
		t.Errorf("StopAudioPlayback calls = %d, want %d", got, want)
	}

	// Start request carries the default format.
	if len(mock.startReqs) != 1 {
		t.Fatalf("expected 1 start req, got %d", len(mock.startReqs))
	}
	if got := mock.startReqs[0].GetSampleRate(); got != 48000 {
		t.Errorf("start sample_rate = %d, want 48000", got)
	}
	if got := mock.startReqs[0].GetChannels(); got != 1 {
		t.Errorf("start channels = %d, want 1", got)
	}

	// Only the three binary frames forwarded, in order; text frame dropped.
	chunks := mock.stream.snapshot()
	if len(chunks) != 3 {
		t.Fatalf("forwarded chunks = %d, want 3", len(chunks))
	}
	for i, want := range [][]byte{frames[0], frames[2], frames[3]} {
		if string(chunks[i].GetData()) != string(want) {
			t.Errorf("chunk %d data = %q, want %q", i, chunks[i].GetData(), want)
		}
		if got := chunks[i].GetFormat(); got != talkFormat {
			t.Errorf("chunk %d format = %q, want %q", i, got, talkFormat)
		}
	}

	// Client stream was closed.
	if !mock.stream.closed {
		t.Error("expected gRPC stream CloseAndRecv to be called")
	}
}

func TestAudioTalk_IdleClientTimesOutAndStops(t *testing.T) {
	drainTalkSem()
	oldTimeout := talkReadIdleTimeout
	talkReadIdleTimeout = 50 * time.Millisecond
	defer func() {
		talkReadIdleTimeout = oldTimeout
		drainTalkSem()
	}()

	mock := newMockTalkClient()
	srv := newTalkTestServer(t, mock)
	defer srv.Close()

	conn := dialTalk(t, srv)
	defer conn.Close()

	waitForStop(t, mock)

	if got, want := mock.startCount(), 1; got != want {
		t.Errorf("StartAudioPlayback calls = %d, want %d", got, want)
	}
	if got, want := mock.stopCount(), 1; got != want {
		t.Errorf("StopAudioPlayback calls = %d, want %d", got, want)
	}
	if !mock.stream.closed {
		t.Error("expected idle gRPC stream to be closed")
	}
}

func TestAudioTalk_QueryParamsOverrideFormat(t *testing.T) {
	drainTalkSem()
	mock := newMockTalkClient()
	srv := newTalkTestServer(t, mock)
	defer srv.Close()

	u := "ws" + strings.TrimPrefix(srv.URL, "http") + "/api/v1/audio/talk?sample_rate=16000&channels=1&device=hw:1,0"
	conn, _, err := (&websocket.Dialer{HandshakeTimeout: 2 * time.Second}).Dial(u, nil)
	if err != nil {
		t.Fatalf("dial: %v", err)
	}
	_ = conn.WriteMessage(websocket.BinaryMessage, []byte("x"))
	_ = conn.WriteMessage(websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.CloseNormalClosure, ""))
	_ = conn.Close()

	waitForStop(t, mock)

	if len(mock.startReqs) != 1 {
		t.Fatalf("expected 1 start req, got %d", len(mock.startReqs))
	}
	if got, want := mock.startReqs[0].GetSampleRate(), uint32(16000); got != want {
		t.Errorf("start sample_rate = %d, want %d", got, want)
	}
	if got, want := mock.startReqs[0].GetDevice(), "hw:1,0"; got != want {
		t.Errorf("start device = %q, want %q", got, want)
	}
}

func TestParseTalkConfig_DefaultsAndOverrides(t *testing.T) {
	gin.SetMode(gin.TestMode)

	t.Run("defaults", func(t *testing.T) {
		w := httptest.NewRecorder()
		c, _ := gin.CreateTestContext(w)
		c.Request = httptest.NewRequest(http.MethodGet, "/api/v1/audio/talk", nil)
		cfg := parseTalkConfig(c)
		if cfg.SampleRate != 48000 || cfg.Channels != 1 || cfg.Device != "" {
			t.Errorf("unexpected defaults: %+v", cfg)
		}
	})

	t.Run("overrides_clamped", func(t *testing.T) {
		w := httptest.NewRecorder()
		c, _ := gin.CreateTestContext(w)
		// channels=9 invalid → ignored (kept default 1); sample_rate=0 invalid → ignored.
		c.Request = httptest.NewRequest(http.MethodGet, "/api/v1/audio/talk?channels=9&sample_rate=0", nil)
		cfg := parseTalkConfig(c)
		if cfg.Channels != 1 {
			t.Errorf("invalid channels should clamp to 1, got %d", cfg.Channels)
		}
		if cfg.SampleRate != 48000 {
			t.Errorf("invalid sample_rate should keep default, got %d", cfg.SampleRate)
		}
	})

	t.Run("stereo_allowed", func(t *testing.T) {
		w := httptest.NewRecorder()
		c, _ := gin.CreateTestContext(w)
		c.Request = httptest.NewRequest(http.MethodGet, "/api/v1/audio/talk?channels=2", nil)
		cfg := parseTalkConfig(c)
		if cfg.Channels != 2 {
			t.Errorf("channels=2 should be allowed, got %d", cfg.Channels)
		}
	})
}
