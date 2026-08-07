package handlers

import (
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"
)

func postMediaConfig(t *testing.T, h *MediaHandlers, contentType, body string) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	engine := gin.New()
	engine.POST("/media/config", h.SetConfig)

	req := httptest.NewRequest(http.MethodPost, "/media/config", strings.NewReader(body))
	if contentType != "" {
		req.Header.Set("Content-Type", contentType)
	}
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)
	return w
}

func TestSetMediaConfigRejectsNonJSONContentType(t *testing.T) {
	path := writeTempYaml(t, `rtsp:
  enabled: true
`)
	h, _ := newTransformHandlerWithFakeDaemon(t, transformBaseline)
	h.configPath = path

	w := postMediaConfig(t, h, "text/plain", `{"rtsp":{"enabled":false}}`)
	if w.Code != http.StatusUnsupportedMediaType {
		t.Fatalf("status = %d, want %d; body=%s", w.Code, http.StatusUnsupportedMediaType, w.Body.String())
	}
}

func TestSetMediaConfigAcceptsJSONContentTypeWithCharset(t *testing.T) {
	path := writeTempYaml(t, `rtsp:
  enabled: true
`)
	h, _ := newTransformHandlerWithFakeDaemon(t, transformBaseline)
	h.configPath = path

	w := postMediaConfig(t, h, "application/json; charset=utf-8", `{"rtsp":{"enabled":false}}`)
	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want %d; body=%s", w.Code, http.StatusOK, w.Body.String())
	}

	m := readYamlMap(t, path)
	rtsp, ok := m["rtsp"].(map[string]interface{})
	if !ok || rtsp["enabled"] != false {
		t.Fatalf("rtsp.enabled = %v, want false", rtsp["enabled"])
	}
}
