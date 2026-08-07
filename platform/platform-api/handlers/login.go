package handlers

import (
	"time"

	"github.com/gin-gonic/gin"

	"aipc/platform/common/events"
	"aipc/platform/platform-api/internal/secrets"
)

type LoginRequest struct {
	Username  string `json:"username" binding:"required"`
	Password  string `json:"password" binding:"required"` // base64(RSA-2048/PKCS1v15) ciphertext, or plaintext for legacy clients
	Timestamp int64  `json:"timestamp"`                   // unix seconds; 0 = legacy client, replay check skipped
}

// Login handles user authentication and token issuing.
//
// The password is normally RSA-encrypted by the frontend (the public key comes
// from /api/v1/auth/public-key); decryption is lenient — an undecryptable value
// is treated as plaintext so a legacy frontend keeps working during a rollout.
// The stored credential is compared via bcrypt when it is a hash, falling back
// to constant-time plaintext equality for env-injected or not-yet-migrated
// values. A non-zero timestamp is checked against the freshness window to
// defeat captured-ciphertext replay.
func (h *APIHandlers) Login(c *gin.Context) {
	var req LoginRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid input format")
		return
	}

	// Replay protection: reject timestamped requests whose clock skew exceeds the
	// freshness window. A zero timestamp (older frontend) skips the check.
	if req.Timestamp != 0 {
		if err := secrets.ValidateTimestamp(req.Timestamp, time.Now()); err != nil {
			if h.eventLogger != nil {
				h.eventLogger.LogWithCodeAsync(
					string(events.EventUserLoginFailed),
					events.MessageParams{"username": req.Username, "ip": c.ClientIP(), "reason": "timestamp_out_of_window"},
					req.Username,
				)
			}
			Resp(c).FailMsg(CodeInvalidTimestamp, "Request timestamp out of allowed window")
			return
		}
	}

	// Decrypt the password (lenient fallback to plaintext for legacy clients).
	plainPassword := secrets.DecryptPassword(req.Password, h.rsaPriv)

	// In the event that no username/password is configured, fallback to basic security
	expectedUser := h.authUser
	if expectedUser == "" {
		expectedUser = "admin"
	}
	expectedPass := h.authPass
	if expectedPass == "" {
		expectedPass = "admin"
	}

	if req.Username != expectedUser || !secrets.VerifyPassword(plainPassword, expectedPass) {
		// Log failed login attempt
		if h.eventLogger != nil {
			h.eventLogger.LogWithCodeAsync(
				string(events.EventUserLoginFailed),
				events.MessageParams{"username": req.Username, "ip": c.ClientIP(), "reason": "invalid_credentials"},
				req.Username,
			)
		}
		Resp(c).FailMsg(CodeUnauthorized, "Invalid username or password")
		return
	}

	if h.tokenValidator == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Authentication service unavailable")
		return
	}
	tokenStr, err := h.tokenValidator.IssueToken(req.Username)
	if err != nil {
		Resp(c).FailMsg(CodeUnknownError, "Failed to create login session")
		return
	}

	// Log successful login
	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			string(events.EventUserLoginSuccess),
			events.MessageParams{"username": req.Username, "ip": c.ClientIP()},
			req.Username,
		)
	}

	// Reply matching what frontend expects
	Resp(c).OK(map[string]interface{}{
		"token":    tokenStr,
		"username": req.Username,
	})
}

// Logout invalidates the current web session. It is intentionally idempotent:
// missing, expired, or already-revoked tokens still receive a successful response.
func (h *APIHandlers) Logout(c *gin.Context) {
	if h.tokenValidator != nil {
		username, revoked := h.tokenValidator.RevokeToken(h.tokenValidator.ExtractToken(c))
		if revoked && h.eventLogger != nil {
			h.eventLogger.LogWithCodeAsync(
				string(events.EventUserLogout),
				events.MessageParams{"username": username},
				username,
			)
		}
	}
	Resp(c).OK(nil)
}
