package handlers

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"
)

// newTestStreamHandlers builds a StreamHandlers from inline configs (no YAML
// on disk) with a fixed rtsp base, mirroring how server/main.go wires it.
func newTestStreamHandlers() *StreamHandlers {
	streams := []StreamConfig{
		{ID: "main", Name: "Main Stream (2160p)", Codec: "h264", Width: 3840,
			Height: 2160, FPS: 30, Bitrate: 12000000, GOP: 60, Enabled: true, Status: "active"},
		{ID: "sub", Name: "Sub Stream (720p)", Codec: "h264", Width: 1280,
			Height: 720, FPS: 15, Bitrate: 1000000, GOP: 30, Enabled: false, Status: "stopped"},
	}
	return NewStreamHandlers(streams, "rtsp://192.168.1.10:8554", "/run/aipc/encoded")
}

func serveStream(t *testing.T, method, path string, h *StreamHandlers) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	engine := gin.New()
	engine.GET("/media/streams", h.ListStreams)
	engine.GET("/media/streams/:name", h.GetStream)
	req := httptest.NewRequest(method, path, nil)
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)
	return w
}

func TestListStreamsReturnsEnvelopeWithURLs(t *testing.T) {
	h := newTestStreamHandlers()

	w := serveStream(t, http.MethodGet, "/media/streams", h)
	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", w.Code, w.Body.String())
	}

	var body struct {
		Code    int    `json:"code"`
		Message string `json:"message"`
		Data    struct {
			Streams []map[string]interface{} `json:"streams"`
		} `json:"data"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode envelope: %v body=%s", err, w.Body.String())
	}
	if body.Code != 0 {
		t.Fatalf("code = %d, want 0", body.Code)
	}
	if len(body.Data.Streams) != 2 {
		t.Fatalf("streams = %d, want 2", len(body.Data.Streams))
	}

	byID := map[string]map[string]interface{}{}
	for _, s := range body.Data.Streams {
		byID[s["id"].(string)] = s
	}

	main := byID["main"]
	if main["rtsp_url"] != "rtsp://192.168.1.10:8554/main" {
		t.Errorf("main rtsp_url = %v", main["rtsp_url"])
	}
	if main["h264_ws_path"] != "/api/v1/h264/main" {
		t.Errorf("main h264_ws_path = %v", main["h264_ws_path"])
	}
	if main["status"] != "active" || main["enabled"] != true {
		t.Errorf("main enabled/status = %v/%v", main["enabled"], main["status"])
	}

	sub := byID["sub"]
	if sub["status"] != "stopped" || sub["enabled"] != false {
		t.Errorf("sub enabled/status = %v/%v", sub["enabled"], sub["status"])
	}
	if sub["width"].(float64) != 1280 || sub["fps"].(float64) != 15 {
		t.Errorf("sub width/fps = %v/%v", sub["width"], sub["fps"])
	}
}

func TestGetStreamFound(t *testing.T) {
	h := newTestStreamHandlers()

	w := serveStream(t, http.MethodGet, "/media/streams/main", h)
	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", w.Code, w.Body.String())
	}

	var body struct {
		Code int `json:"code"`
		Data struct {
			ID      string `json:"id"`
			RtspURL string `json:"rtsp_url"`
			WsPath  string `json:"h264_ws_path"`
			Codec   string `json:"codec"`
			Bitrate int    `json:"bitrate"`
		} `json:"data"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode envelope: %v", err)
	}
	if body.Data.ID != "main" || body.Data.Codec != "h264" {
		t.Errorf("id/codec = %s/%s", body.Data.ID, body.Data.Codec)
	}
	if body.Data.RtspURL != "rtsp://192.168.1.10:8554/main" {
		t.Errorf("rtsp_url = %q", body.Data.RtspURL)
	}
	if body.Data.WsPath != "/api/v1/h264/main" {
		t.Errorf("h264_ws_path = %q", body.Data.WsPath)
	}

	// socket_path is internal (json:"-") and must not leak into responses.
	if strings.Contains(w.Body.String(), "socket_path") {
		t.Errorf("response leaks socket_path: %s", w.Body.String())
	}
}

func TestGetStreamNotFound(t *testing.T) {
	h := newTestStreamHandlers()

	w := serveStream(t, http.MethodGet, "/media/streams/nope", h)
	if w.Code != http.StatusNotFound {
		t.Fatalf("expected 404, got %d body=%s", w.Code, w.Body.String())
	}
	var body struct {
		Code  int `json:"code"`
		Error struct {
			Detail string `json:"detail"`
		} `json:"error"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode envelope: %v", err)
	}
	if body.Code != CodeNotFound {
		t.Errorf("code = %d, want %d", body.Code, CodeNotFound)
	}
	if !strings.Contains(body.Error.Detail, "nope") {
		t.Errorf("error detail = %q, want stream name", body.Error.Detail)
	}
}
