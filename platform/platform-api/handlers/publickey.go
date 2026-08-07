package handlers

import (
	"time"

	"github.com/gin-gonic/gin"
)

// GetPublicKey returns the device's RSA public key (PKIX PEM) so the frontend
// can encrypt the password before sending it over the wire. It is mounted on
// the engine root (no auth) because the browser needs the key before it can
// log in.
//
// A 503/CodeServiceError is returned when the keypair was not initialized at
// boot (e.g. the persistent key directory is unwritable); login then degrades
// to plaintext comparison, which still works over TLS but loses the
// log-leakage protection.
func (h *APIHandlers) GetPublicKey(c *gin.Context) {
	if h.rsaPubPEM == "" || h.rsaPriv == nil {
		Resp(c).FailMsg(CodeServiceError, "RSA keypair not initialized on this device")
		return
	}
	Resp(c).OK(gin.H{
		"public_key":     h.rsaPubPEM,
		"algorithm":      "RSA-2048/PKCS1v15",
		"unix_timestamp": time.Now().Unix(),
	})
}
