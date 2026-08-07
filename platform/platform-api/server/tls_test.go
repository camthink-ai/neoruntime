package main

import (
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
)

// TestEnsureCert_GeneratesAndReuses covers the device auto-generate path: the
// first call creates the pair, the second call reuses it without regenerating.
func TestEnsureCert_GeneratesAndReuses(t *testing.T) {
	dir := t.TempDir()
	cfg := TLSConfig{Enabled: true, AutoCertDir: dir}

	certPath, keyPath, err := ensureCert(cfg)
	if err != nil {
		t.Fatalf("first ensureCert: %v", err)
	}
	if certPath != filepath.Join(dir, "server.crt") || keyPath != filepath.Join(dir, "server.key") {
		t.Fatalf("unexpected paths: %s, %s", certPath, keyPath)
	}

	// Stat before second call to detect regeneration.
	firstInfo, err := os.Stat(certPath)
	if err != nil {
		t.Fatalf("stat cert: %v", err)
	}

	// Second call must reuse the existing pair (same path, no regeneration).
	if _, _, err := ensureCert(cfg); err != nil {
		t.Fatalf("second ensureCert: %v", err)
	}
	secondInfo, err := os.Stat(certPath)
	if err != nil {
		t.Fatalf("stat cert after reuse: %v", err)
	}
	if !firstInfo.ModTime().Equal(secondInfo.ModTime()) {
		t.Fatalf("cert was regenerated on second call (mtime changed): %s -> %s", firstInfo.ModTime(), secondInfo.ModTime())
	}
}

// TestEnsureCert_SelfSignedSANs verifies the generated leaf embeds localhost +
// 127.0.0.1 as SANs so it matches local access, and is a CA (importable).
func TestEnsureCert_SelfSignedSANs(t *testing.T) {
	dir := t.TempDir()
	cfg := TLSConfig{Enabled: true, AutoCertDir: dir}

	certPath, keyPath, err := ensureCert(cfg)
	if err != nil {
		t.Fatalf("ensureCert: %v", err)
	}

	certPEM, err := os.ReadFile(certPath)
	if err != nil {
		t.Fatalf("read cert: %v", err)
	}
	block, _ := pem.Decode(certPEM)
	if block == nil {
		t.Fatalf("cert is not valid PEM")
	}
	cert, err := x509.ParseCertificate(block.Bytes)
	if err != nil {
		t.Fatalf("parse cert: %v", err)
	}

	if !cert.IsCA {
		t.Errorf("cert should be a CA so it can be imported as a trusted root")
	}

	// localhost + loopback must always be present (independent of NIC state).
	hasLocalhost := false
	for _, name := range cert.DNSNames {
		if name == "localhost" {
			hasLocalhost = true
		}
	}
	if !hasLocalhost {
		t.Errorf("cert DNS SAN missing localhost; got %v", cert.DNSNames)
	}
	hasLoopback := false
	for _, ip := range cert.IPAddresses {
		if ip.String() == "127.0.0.1" {
			hasLoopback = true
		}
	}
	if !hasLoopback {
		t.Errorf("cert IP SAN missing 127.0.0.1; got %v", cert.IPAddresses)
	}

	// The pair must be loadable as a tls.Certificate.
	if _, err := tls.LoadX509KeyPair(certPath, keyPath); err != nil {
		t.Fatalf("LoadX509KeyPair: %v", err)
	}
}

// TestEnsureCert_KeyPermissions asserts the private key is 0600.
func TestEnsureCert_KeyPermissions(t *testing.T) {
	dir := t.TempDir()
	_, keyPath, err := ensureCert(TLSConfig{Enabled: true, AutoCertDir: dir})
	if err != nil {
		t.Fatalf("ensureCert: %v", err)
	}
	info, err := os.Stat(keyPath)
	if err != nil {
		t.Fatalf("stat key: %v", err)
	}
	if mode := info.Mode().Perm(); mode != 0o600 {
		t.Errorf("key permissions = %o, want 0600", mode)
	}
}

// TestEnsureCert_UserSuppliedPassthrough verifies operator-owned certs are
// validated and returned as-is (no takeover, no regeneration).
func TestEnsureCert_UserSuppliedPassthrough(t *testing.T) {
	dir := t.TempDir()
	// Generate a valid pair into a separate dir to act as "operator" cert.
	gen := TLSConfig{Enabled: true, AutoCertDir: filepath.Join(dir, "src")}
	certPath, keyPath, err := ensureCert(gen)
	if err != nil {
		t.Fatalf("seed operator cert: %v", err)
	}

	got, gotKey, err := ensureCert(TLSConfig{Enabled: true, CertFile: certPath, KeyFile: keyPath})
	if err != nil {
		t.Fatalf("user-supplied ensureCert: %v", err)
	}
	if got != certPath || gotKey != keyPath {
		t.Errorf("passthrough returned %s,%s; want %s,%s", got, gotKey, certPath, keyPath)
	}
}

// TestEnsureCert_HalfSuppliedErrors verifies that specifying only one of
// cert_file/key_file is rejected.
func TestEnsureCert_HalfSuppliedErrors(t *testing.T) {
	_, _, err := ensureCert(TLSConfig{Enabled: true, CertFile: "/tmp/x.crt"})
	if err == nil {
		t.Fatal("expected error when only cert_file is set")
	}
}

// TestEnsureCert_UserSuppliedUnreadable verifies a missing operator path errors.
func TestEnsureCert_UserSuppliedUnreadable(t *testing.T) {
	_, _, err := ensureCert(TLSConfig{
		Enabled:  true,
		CertFile: filepath.Join(t.TempDir(), "nope.crt"),
		KeyFile:  filepath.Join(t.TempDir(), "nope.key"),
	})
	if err == nil {
		t.Fatal("expected error for unreadable operator cert")
	}
}

// TestHTTPRedirectHandler_DefaultPort verifies 443 omits the port from the
// Location and preserves path + query.
func TestHTTPRedirectHandler_DefaultPort(t *testing.T) {
	h := httpRedirectHandler(":443")
	req := httptest.NewRequest(http.MethodGet, "/foo?x=1", nil)
	req.Host = "192.0.2.72"
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	if rec.Code != http.StatusMovedPermanently {
		t.Fatalf("status = %d, want 301", rec.Code)
	}
	loc := rec.Header().Get("Location")
	want := "https://192.0.2.72/foo?x=1"
	if loc != want {
		t.Errorf("Location = %q, want %q", loc, want)
	}
}

// TestHTTPRedirectHandler_CustomPort verifies a non-443 port is appended and the
// existing port on the Host header is stripped first.
func TestHTTPRedirectHandler_CustomPort(t *testing.T) {
	h := httpRedirectHandler(":8443")
	req := httptest.NewRequest(http.MethodGet, "/path", nil)
	req.Host = "device.lan:8080" // old HTTP port must be stripped
	rec := httptest.NewRecorder()
	h.ServeHTTP(rec, req)

	loc := rec.Header().Get("Location")
	want := "https://device.lan:8443/path"
	if loc != want {
		t.Errorf("Location = %q, want %q", loc, want)
	}
}
