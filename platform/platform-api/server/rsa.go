package main

import (
	"crypto/rand"
	"crypto/rsa"
	"crypto/x509"
	"encoding/pem"
	"fmt"
	"os"
	"path/filepath"

	"aipc/platform/common/logger"
	"aipc/platform/platform-api/internal/atomicfile"
)

// defaultRSAKeyDir is where the password-encryption RSA keypair is generated
// when no path is supplied. It lives on the persistent /data partition so the
// keypair survives reboots and is reused across restarts — regenerating it
// would silently invalidate every cached public key on connected browsers.
const defaultRSAKeyDir = "/data/aipc/etc/rsa"

// RSA keypair filenames inside rsaDir.
const (
	rsaPrivateKeyFile = "password.key"
	rsaPublicKeyFile  = "password.pub"
)

// ensureRSAKeyPair resolves the password-encryption RSA keypair under rsaDir.
//
// If the private key file already exists it is loaded and reused — a restart
// never regenerates the key, otherwise any browser that cached the public key
// (see web/src/utils/crypto.ts) would start failing logins until its cache
// expires. Otherwise a fresh 2048-bit RSA keypair is generated and written: the
// private key as a PKCS#8 PEM (0600) and the public key as a PKIX PEM (0644),
// both atomically, in a 0700 directory — mirroring how tls.go persists the
// self-signed certificate key.
//
// Returns the private key plus the public key as a ready-to-serve PEM string
// (the exact "-----BEGIN PUBLIC KEY-----" format jsencrypt expects), so the
// Login handler can decrypt with priv while GetPublicKey returns pubPEM verbatim.
func ensureRSAKeyPair(rsaDir string) (priv *rsa.PrivateKey, pubPEM string, err error) {
	if rsaDir == "" {
		rsaDir = defaultRSAKeyDir
	}
	privPath := filepath.Join(rsaDir, rsaPrivateKeyFile)

	if fileExists(privPath) {
		priv, pubPEM, err = loadRSAKeyPair(privPath)
		if err == nil {
			return priv, pubPEM, nil
		}
		logger.Warn("server: existing RSA key unreadable (%v); regenerating", err)
	}

	if err := os.MkdirAll(rsaDir, 0o700); err != nil {
		return nil, "", fmt.Errorf("mkdir %s: %w", rsaDir, err)
	}

	priv, err = rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		return nil, "", fmt.Errorf("generate RSA key: %w", err)
	}
	pubPath := filepath.Join(rsaDir, rsaPublicKeyFile)
	if err := writeRSAKeyPair(privPath, pubPath, priv); err != nil {
		return nil, "", err
	}
	logger.Info("server: generated RSA password keypair at %s", privPath)
	return priv, publicKeyPEM(&priv.PublicKey), nil
}

// loadRSAKeyPair reads the PKCS#8 private key PEM at privPath and derives the
// public key PEM from it. The on-disk public key file is informational only;
// the private key is authoritative (so a mismatched .pub can never cause the
// server to publish a key it cannot decrypt with).
func loadRSAKeyPair(privPath string) (*rsa.PrivateKey, string, error) {
	data, err := os.ReadFile(privPath)
	if err != nil {
		return nil, "", fmt.Errorf("read %s: %w", privPath, err)
	}
	block, _ := pem.Decode(data)
	if block == nil {
		return nil, "", fmt.Errorf("%s: no PEM block", privPath)
	}
	key, err := x509.ParsePKCS8PrivateKey(block.Bytes)
	if err != nil {
		return nil, "", fmt.Errorf("parse PKCS8 key: %w", err)
	}
	rsaKey, ok := key.(*rsa.PrivateKey)
	if !ok {
		return nil, "", fmt.Errorf("%s: not an RSA key", privPath)
	}
	return rsaKey, publicKeyPEM(&rsaKey.PublicKey), nil
}

// writeRSAKeyPair writes the private (PKCS#8, 0600) and public (PKIX, 0644) key
// PEMs atomically.
func writeRSAKeyPair(privPath, pubPath string, priv *rsa.PrivateKey) error {
	keyDER, err := x509.MarshalPKCS8PrivateKey(priv)
	if err != nil {
		return fmt.Errorf("marshal PKCS8 key: %w", err)
	}
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})
	if err := atomicfile.Write(privPath, keyPEM, 0o600); err != nil {
		return fmt.Errorf("write %s: %w", privPath, err)
	}

	pubPEM, err := publicKeyPEMBytes(&priv.PublicKey)
	if err != nil {
		return fmt.Errorf("marshal PKIX public key: %w", err)
	}
	if err := atomicfile.Write(pubPath, pubPEM, 0o644); err != nil {
		return fmt.Errorf("write %s: %w", pubPath, err)
	}
	return nil
}

// publicKeyPEM returns the PKIX PEM string for an RSA public key. An empty
// string is returned only if marshalling fails, which cannot happen for a key
// produced by rsa.GenerateKey.
func publicKeyPEM(pub *rsa.PublicKey) string {
	b, err := publicKeyPEMBytes(pub)
	if err != nil {
		return ""
	}
	return string(b)
}

func publicKeyPEMBytes(pub *rsa.PublicKey) ([]byte, error) {
	der, err := x509.MarshalPKIXPublicKey(pub)
	if err != nil {
		return nil, err
	}
	return pem.EncodeToMemory(&pem.Block{Type: "PUBLIC KEY", Bytes: der}), nil
}
