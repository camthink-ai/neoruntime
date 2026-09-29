package auth

import (
	"context"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"
)

func TestSessionTokenLifecycle(t *testing.T) {
	validator := NewTokenValidator("integration-api-key", true)

	first, err := validator.IssueToken("admin")
	if err != nil {
		t.Fatalf("IssueToken() error = %v", err)
	}
	second, err := validator.IssueToken("admin")
	if err != nil {
		t.Fatalf("IssueToken() second error = %v", err)
	}
	if first == second {
		t.Fatal("IssueToken() returned the same token for two sessions")
	}
	if !strings.HasPrefix(first, "Bearer ") {
		t.Fatalf("IssueToken() = %q, want Bearer token", first)
	}
	if !validator.ValidateToken(first) || !validator.ValidateToken(second) {
		t.Fatal("issued session token was not accepted")
	}

	username, revoked := validator.RevokeToken(first)
	if !revoked || username != "admin" {
		t.Fatalf("RevokeToken() = (%q, %v), want (admin, true)", username, revoked)
	}
	if validator.ValidateToken(first) {
		t.Fatal("revoked session token is still valid")
	}
	if !validator.ValidateToken(second) {
		t.Fatal("revoking one session invalidated another session")
	}
	if _, revokedAgain := validator.RevokeToken(first); revokedAgain {
		t.Fatal("repeated RevokeToken() unexpectedly reported a revocation")
	}
}

func TestConfiguredAPIKeyRemainsIndependent(t *testing.T) {
	validator := NewTokenValidator("integration-api-key", true)
	if !validator.ValidateToken("integration-api-key") {
		t.Fatal("configured API key was rejected")
	}
	if !validator.ValidateToken("Bearer integration-api-key") {
		t.Fatal("configured Bearer API key was rejected")
	}
}

func TestMiddlewareLocalSocketTrust(t *testing.T) {
	gin.SetMode(gin.TestMode)
	router := gin.New()
	router.GET("/api/v1/ping", Middleware(NewTokenValidator("integration-api-key", true)), func(c *gin.Context) {
		c.String(http.StatusOK, "pong")
	})

	// Plain (TCP-equivalent) request without a token is rejected.
	w := httptest.NewRecorder()
	router.ServeHTTP(w, httptest.NewRequest(http.MethodGet, "/api/v1/ping", nil))
	if w.Code != http.StatusUnauthorized {
		t.Fatalf("tokenless TCP request: got status %d, want %d", w.Code, http.StatusUnauthorized)
	}

	// A request marked as arriving on the local unix socket face skips auth.
	req := httptest.NewRequest(http.MethodGet, "/api/v1/ping", nil)
	req = req.WithContext(WithLocalTrust(req.Context()))
	w = httptest.NewRecorder()
	router.ServeHTTP(w, req)
	if w.Code != http.StatusOK || w.Body.String() != "pong" {
		t.Fatalf("local socket request: got status %d body %q, want 200 %q", w.Code, w.Body.String(), "pong")
	}

	// The TCP face keeps accepting the configured API key.
	req = httptest.NewRequest(http.MethodGet, "/api/v1/ping", nil)
	req.Header.Set("Authorization", "Bearer integration-api-key")
	w = httptest.NewRecorder()
	router.ServeHTTP(w, req)
	if w.Code != http.StatusOK {
		t.Fatalf("API-key TCP request: got status %d, want %d", w.Code, http.StatusOK)
	}
}

func TestIsLocalSocketDefaultContext(t *testing.T) {
	if IsLocalSocket(context.Background()) {
		t.Fatal("IsLocalSocket() on a plain context reported true; the mark must only come from WithLocalTrust")
	}
	if !IsLocalSocket(WithLocalTrust(context.Background())) {
		t.Fatal("IsLocalSocket() on a WithLocalTrust context reported false")
	}
}
