package auth

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"fmt"
	"net/http"
	"strings"
	"sync"
	"time"

	"aipc/platform/common/logger"
	"github.com/gin-gonic/gin"
)

// TokenValidator validates authentication tokens
type TokenValidator struct {
	tokenKey     string
	enabled      bool
	allowedPaths []string // Paths that don't require authentication
	sessionsMu   sync.RWMutex
	sessions     map[[sha256.Size]byte]session
}

type session struct {
	username string
}

// NewTokenValidator creates a new token validator
func NewTokenValidator(tokenKey string, enabled bool) *TokenValidator {
	return &TokenValidator{
		tokenKey: tokenKey,
		enabled:  enabled,
		sessions: make(map[[sha256.Size]byte]session),
		allowedPaths: []string{
			"/api/v1/system/health", // Health check doesn't need auth
		},
	}
}

// IssueToken creates a revocable token for an authenticated web session.
func (v *TokenValidator) IssueToken(username string) (string, error) {
	raw := make([]byte, 32)
	if _, err := rand.Read(raw); err != nil {
		return "", fmt.Errorf("generate session token: %w", err)
	}

	token := base64.RawURLEncoding.EncodeToString(raw)
	hash := sha256.Sum256([]byte(token))

	v.sessionsMu.Lock()
	v.sessions[hash] = session{username: username}
	v.sessionsMu.Unlock()

	return "Bearer " + token, nil
}

// RevokeToken invalidates a previously issued web session token.
func (v *TokenValidator) RevokeToken(token string) (string, bool) {
	token = strings.TrimPrefix(token, "Bearer ")
	hash := sha256.Sum256([]byte(token))

	v.sessionsMu.Lock()
	defer v.sessionsMu.Unlock()

	sess, ok := v.sessions[hash]
	if !ok {
		return "", false
	}
	delete(v.sessions, hash)
	return sess.username, true
}

// ValidateToken validates an authentication token
// Supports two modes:
// 1. Simple token match (if tokenKey is set)
// 2. HMAC-based token validation (if tokenKey is used as secret)
func (v *TokenValidator) ValidateToken(token string) bool {
	if !v.enabled {
		return true
	}

	if token == "" {
		return false
	}

	token = strings.TrimPrefix(token, "Bearer ")
	hash := sha256.Sum256([]byte(token))

	v.sessionsMu.RLock()
	_, ok := v.sessions[hash]
	v.sessionsMu.RUnlock()
	if ok {
		return true
	}

	// The configured token key remains available as a separate API key for
	// integrations. Web logins receive revocable session tokens instead.
	if v.tokenKey != "" {
		// If token matches the configured key, accept it
		if token == v.tokenKey {
			return true
		}

		// Support HMAC-based token (optional, for more secure tokens)
		// Format: timestamp:hmac(secret, timestamp)
		parts := strings.Split(token, ":")
		if len(parts) == 2 {
			timestamp := parts[0]
			expectedHMAC := parts[1]

			// Validate timestamp (within 5 minutes)
			var ts int64
			if _, err := fmt.Sscanf(timestamp, "%d", &ts); err == nil {
				now := time.Now().Unix()
				if ts > now-300 && ts < now+300 { // 5 minute window
					// Compute HMAC
					mac := hmac.New(sha256.New, []byte(v.tokenKey))
					mac.Write([]byte(timestamp))
					computedHMAC := hex.EncodeToString(mac.Sum(nil))

					if hmac.Equal([]byte(expectedHMAC), []byte(computedHMAC)) {
						return true
					}
				}
			}
		}
	}

	return false
}

// ExtractToken extracts a token from request headers or query parameters.
func (v *TokenValidator) ExtractToken(c *gin.Context) string {
	// Try Authorization header first
	authHeader := c.GetHeader("Authorization")
	if authHeader != "" {
		if strings.HasPrefix(authHeader, "Bearer ") {
			return strings.TrimPrefix(authHeader, "Bearer ")
		}
		return authHeader
	}

	// For WebSocket connections, also check query parameters
	if token := c.Query("token"); token != "" {
		return token
	}

	// Try X-API-Key header
	apiKey := c.GetHeader("X-API-Key")
	if apiKey != "" {
		return apiKey
	}

	return ""
}

// isAllowedPath checks if the path is in the allowed list
func (v *TokenValidator) isAllowedPath(path string) bool {
	for _, allowed := range v.allowedPaths {
		if path == allowed {
			return true
		}
	}
	return false
}

// Middleware creates an authentication middleware for Gin
func Middleware(validator *TokenValidator) gin.HandlerFunc {
	return func(c *gin.Context) {
		// If auth is disabled, allow all requests
		if !validator.enabled {
			c.Next()
			return
		}

		// Check if path is in allowed list
		if validator.isAllowedPath(c.Request.URL.Path) {
			c.Next()
			return
		}

		// Extract and validate token
		token := validator.ExtractToken(c)
		if !validator.ValidateToken(token) {
			logger.Warn("Unauthorized access attempt from %s to %s", c.ClientIP(), c.Request.URL.Path)
			c.Header("WWW-Authenticate", "Bearer")
			c.AbortWithStatusJSON(http.StatusUnauthorized, gin.H{
				"code":    2000, // CodeUnauthorized
				"message": "Unauthorized",
				"error": gin.H{
					"type":   "auth",
					"detail": "Invalid or missing authentication token",
				},
			})
			return
		}

		// Token is valid, proceed
		c.Next()
	}
}
