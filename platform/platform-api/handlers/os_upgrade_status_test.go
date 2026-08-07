package handlers

import (
	"net/http"
	"net/http/httptest"
	"testing"

	platformauth "aipc/platform/platform-api/auth"

	"github.com/gin-gonic/gin"
)

func TestOSUpgradeStatusRouteCanBePublic(t *testing.T) {
	gin.SetMode(gin.TestMode)
	h := NewOSUpgradeHandlers(t.TempDir())
	router := gin.New()
	router.GET("/api/v1/system/os-upgrade/status", h.Status)
	protected := router.Group("/api/v1")
	protected.Use(platformauth.Middleware(platformauth.NewTokenValidator("test-token", true)))
	protected.POST("/system/os-upgrade/reboot", h.Reboot)

	req := httptest.NewRequest(http.MethodGet, "/api/v1/system/os-upgrade/status", nil)
	w := httptest.NewRecorder()
	router.ServeHTTP(w, req)

	if w.Code != http.StatusOK {
		t.Fatalf("public OS upgrade status code = %d, want 200; body=%s", w.Code, w.Body.String())
	}

	protectedReq := httptest.NewRequest(http.MethodPost, "/api/v1/system/os-upgrade/reboot", nil)
	protectedResp := httptest.NewRecorder()
	router.ServeHTTP(protectedResp, protectedReq)
	if protectedResp.Code != http.StatusUnauthorized {
		t.Fatalf("protected OS upgrade reboot code = %d, want 401", protectedResp.Code)
	}
}
