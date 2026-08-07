package auth

import (
	"strings"
	"testing"
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
