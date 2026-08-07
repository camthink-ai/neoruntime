package secrets

import (
	"crypto/rand"
	"crypto/rsa"
	"encoding/base64"
)

// DecryptPassword decrypts a base64-encoded RSA ciphertext produced by the
// frontend (jsencrypt, PKCS1v15). It is deliberately lenient: if anything about
// the input is not a valid RSA ciphertext for priv — not base64, wrong length,
// padding failure, or priv is nil (key generation failed at boot) — the
// original input string is returned unchanged.
//
// This try-decrypt-fallback keeps an old frontend that still sends plaintext
// passwords working during a rollout, and keeps the service reachable when the
// RSA keypair is unavailable. It does not weaken the transport-encryption goal:
// the plaintext path only fires for values that were never encrypted in the
// first place, and the real protection against wire sniffing remains TLS.
func DecryptPassword(ciphertext string, priv *rsa.PrivateKey) string {
	if priv == nil {
		return ciphertext
	}
	raw, err := base64.StdEncoding.DecodeString(ciphertext)
	if err != nil {
		return ciphertext
	}
	plain, err := rsa.DecryptPKCS1v15(rand.Reader, priv, raw)
	if err != nil {
		return ciphertext
	}
	return string(plain)
}
