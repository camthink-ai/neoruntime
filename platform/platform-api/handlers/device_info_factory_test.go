package handlers

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"

	"aipc/platform/common/factoryeeprom"

	"github.com/gin-gonic/gin"
)

func TestGetDeviceInfoUsesFactoryEEPROM(t *testing.T) {
	gin.SetMode(gin.TestMode)
	script, stateDir := writeDeviceInfoFactoryScript(t)
	t.Setenv(factoryeeprom.EnvToolPath, script)
	writeFactoryState(t, stateDir, "SN", "CT2026-000812")
	writeFactoryState(t, stateDir, "HWREV", "2.1")
	writeFactoryState(t, stateDir, "PN", "NE503-A1")
	writeFactoryState(t, stateDir, "BATCH", "20260724")

	handler := NewDeviceInfoHandler(filepath.Join(t.TempDir(), "device.conf"), nil, nil)
	router := gin.New()
	router.GET("/device-info", handler.GetDeviceInfo)

	req := httptest.NewRequest(http.MethodGet, "/device-info", nil)
	req.Host = "192.168.93.72"
	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d body=%s", w.Code, w.Body.String())
	}

	var resp struct {
		Code int `json:"code"`
		Data struct {
			SerialNumber    string             `json:"serial_number"`
			HardwareVersion string             `json:"hardware_version"`
			Factory         factoryeeprom.Info `json:"factory"`
		} `json:"data"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	if resp.Code != CodeSuccess {
		t.Fatalf("code = %d", resp.Code)
	}
	if resp.Data.SerialNumber != "CT2026-000812" {
		t.Fatalf("serial_number = %q", resp.Data.SerialNumber)
	}
	if resp.Data.HardwareVersion != "2.1" {
		t.Fatalf("hardware_version = %q", resp.Data.HardwareVersion)
	}
	if resp.Data.Factory.ProductNumber != "NE503-A1" || resp.Data.Factory.Batch != "20260724" {
		t.Fatalf("factory = %+v", resp.Data.Factory)
	}
}

func TestUpdateFactoryFieldWritesAndVerifies(t *testing.T) {
	gin.SetMode(gin.TestMode)
	script, stateDir := writeDeviceInfoFactoryScript(t)
	t.Setenv(factoryeeprom.EnvToolPath, script)

	handler := NewDeviceInfoHandler(filepath.Join(t.TempDir(), "device.conf"), nil, nil)
	router := gin.New()
	router.POST("/device-info/factory", handler.UpdateFactoryField)

	req := httptest.NewRequest(http.MethodPost, "/device-info/factory", bytes.NewBufferString(`{"field":"PN","value":"NE503-A1"}`))
	req.Header.Set("Content-Type", "application/json")
	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d body=%s", w.Code, w.Body.String())
	}
	if got, err := os.ReadFile(filepath.Join(stateDir, "PN")); err != nil || string(got) != "NE503-A1" {
		t.Fatalf("stored PN = %q err=%v", got, err)
	}
}

func TestUpdateFactoryFieldRejectsBadRequests(t *testing.T) {
	gin.SetMode(gin.TestMode)
	script, _ := writeDeviceInfoFactoryScript(t)
	t.Setenv(factoryeeprom.EnvToolPath, script)

	handler := NewDeviceInfoHandler(filepath.Join(t.TempDir(), "device.conf"), nil, nil)
	router := gin.New()
	router.POST("/device-info/factory", handler.UpdateFactoryField)

	req := httptest.NewRequest(http.MethodPost, "/device-info/factory", bytes.NewBufferString(`{"field":"SN","value":"CT2026-000812"}`))
	req.Header.Set("Content-Type", "text/plain")
	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)
	if w.Code != http.StatusUnsupportedMediaType {
		t.Fatalf("text/plain status = %d body=%s", w.Code, w.Body.String())
	}

	req = httptest.NewRequest(http.MethodPost, "/device-info/factory", bytes.NewBufferString(`{"field":"UNKNOWN","value":"x"}`))
	req.Header.Set("Content-Type", "application/json")
	w = httptest.NewRecorder()
	router.ServeHTTP(w, req)
	if w.Code != http.StatusBadRequest {
		t.Fatalf("invalid field status = %d body=%s", w.Code, w.Body.String())
	}
}

func writeDeviceInfoFactoryScript(t *testing.T) (string, string) {
	t.Helper()

	dir := t.TempDir()
	stateDir := filepath.Join(dir, "state")
	if err := os.MkdirAll(stateDir, 0755); err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(dir, "factory-eeprom.sh")
	body := `#!/bin/sh
set -eu
if [ "${1:-}" = "-d" ]; then
  shift 2
fi
cmd="${1:-}"
field="${2:-}"
value="${3:-}"
state="` + stateDir + `"
case "$cmd" in
  get)
    file="$state/$field"
    [ -f "$file" ] || exit 1
    cat "$file"
    ;;
  set)
    [ -n "$field" ] || exit 2
    printf '%s' "$value" > "$state/$field"
    ;;
  *)
    exit 2
    ;;
esac
`
	if err := os.WriteFile(script, []byte(body), 0755); err != nil {
		t.Fatal(err)
	}
	return script, stateDir
}

func writeFactoryState(t *testing.T, stateDir, field, value string) {
	t.Helper()
	if err := os.WriteFile(filepath.Join(stateDir, field), []byte(value), 0644); err != nil {
		t.Fatal(err)
	}
}
