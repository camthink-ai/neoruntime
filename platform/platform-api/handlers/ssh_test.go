package handlers

import (
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"
)

func postSSHConfig(t *testing.T, h *SSHHandler, body string) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	engine := gin.New()
	engine.POST("/ssh/config", h.SetConfig)
	req := httptest.NewRequest(http.MethodPost, "/ssh/config", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)
	return w
}

func newTestSSHHandler(t *testing.T, config string) (*SSHHandler, string) {
	t.Helper()
	path := filepath.Join(t.TempDir(), "sshd_config")
	if err := os.WriteFile(path, []byte(config), 0644); err != nil {
		t.Fatalf("write sshd_config fixture: %v", err)
	}
	return &SSHHandler{
		configPath:       path,
		validateConfigFn: func(string) error { return nil },
		restartServiceFn: func() error { return nil },
	}, path
}

func TestSSHSetConfigUpdatesClientAliveCountMax(t *testing.T) {
	h, path := newTestSSHHandler(t, `#Port 22
PermitRootLogin yes
ClientAliveInterval 15
ClientAliveCountMax 4
`)

	w := postSSHConfig(t, h, `{"client_alive_count_max":"2"}`)
	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", w.Code, w.Body.String())
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read sshd_config: %v", err)
	}
	if !strings.Contains(string(data), "ClientAliveCountMax 2") {
		t.Fatalf("ClientAliveCountMax was not updated:\n%s", string(data))
	}
}

func TestSSHSetConfigRejectsUnknownFields(t *testing.T) {
	h, path := newTestSSHHandler(t, "PermitRootLogin yes\n")

	w := postSSHConfig(t, h, `{"ClientAliveCountMaximum":"2"}`)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d body=%s", w.Code, w.Body.String())
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read sshd_config: %v", err)
	}
	if string(data) != "PermitRootLogin yes\n" {
		t.Fatalf("config changed after rejected request:\n%s", string(data))
	}
}

func TestSSHSetConfigRejectsNoOpPayload(t *testing.T) {
	h, _ := newTestSSHHandler(t, "PermitRootLogin yes\n")

	w := postSSHConfig(t, h, `{"restart_service":true}`)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d body=%s", w.Code, w.Body.String())
	}
}

func TestSSHSetConfigRejectsNonJSONContentType(t *testing.T) {
	h, path := newTestSSHHandler(t, "ClientAliveCountMax 4\n")
	gin.SetMode(gin.TestMode)
	engine := gin.New()
	engine.POST("/ssh/config", h.SetConfig)
	req := httptest.NewRequest(http.MethodPost, "/ssh/config", strings.NewReader(`{"client_alive_count_max":"2"}`))
	req.Header.Set("Content-Type", "text/plain")
	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)

	if w.Code != http.StatusUnsupportedMediaType {
		t.Fatalf("expected 415, got %d body=%s", w.Code, w.Body.String())
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read sshd_config: %v", err)
	}
	if string(data) != "ClientAliveCountMax 4\n" {
		t.Fatalf("config changed after rejected request:\n%s", string(data))
	}
}

func TestSSHSetConfigInsertsMissingDirectiveBeforeMatchBlock(t *testing.T) {
	h, path := newTestSSHHandler(t, `Port 22
Match User backup
  X11Forwarding no
`)

	w := postSSHConfig(t, h, `{"client_alive_count_max":"3"}`)
	if w.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d body=%s", w.Code, w.Body.String())
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read sshd_config: %v", err)
	}
	got := string(data)
	want := "Port 22\nClientAliveCountMax 3\nMatch User backup\n  X11Forwarding no\n"
	if got != want {
		t.Fatalf("updated config:\n%s\nwant:\n%s", got, want)
	}
}
