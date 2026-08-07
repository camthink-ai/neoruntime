package main

import (
	"aipc/platform/common/logger"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"fmt"
	"math/big"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"
)

// defaultAutoCertDir is where the self-signed certificate is generated when no
// user-supplied cert/key pair is configured. It lives on the persistent /data
// partition so the cert survives reboots and is reused across restarts.
const defaultAutoCertDir = "/data/aipc/etc/ssl"

// ensureCert resolves the cert/key paths for the HTTPS listener.
//
// If CertFile and KeyFile are both configured, they are treated as user-supplied
// (the operator owns rotation); the files are validated to be readable and
// returned as-is.
//
// Otherwise a self-signed ECDSA P-256 leaf certificate is generated once into
// AutoCertDir and reused on subsequent starts. The certificate embeds every
// non-loopback IPv4 on the device plus localhost/127.0.0.1 as SANs so it matches
// the address used to reach the device on the LAN. The private key is written
// with 0600 permissions.
func ensureCert(cfg TLSConfig) (certPath, keyPath string, err error) {
	// User-supplied cert path: validate and return as-is.
	if cfg.CertFile != "" || cfg.KeyFile != "" {
		if cfg.CertFile == "" || cfg.KeyFile == "" {
			return "", "", fmt.Errorf("cert_file and key_file must both be set when either is provided")
		}
		if err := validateReadable(cfg.CertFile); err != nil {
			return "", "", fmt.Errorf("cert_file unreadable: %w", err)
		}
		if err := validateReadable(cfg.KeyFile); err != nil {
			return "", "", fmt.Errorf("key_file unreadable: %w", err)
		}
		return cfg.CertFile, cfg.KeyFile, nil
	}

	dir := cfg.AutoCertDir
	if dir == "" {
		dir = defaultAutoCertDir
	}
	certPath = filepath.Join(dir, "server.crt")
	keyPath = filepath.Join(dir, "server.key")

	// Reuse existing pair so a restart never regenerates the cert (which would
	// force browsers to re-accept the warning).
	if fileExists(certPath) && fileExists(keyPath) {
		return certPath, keyPath, nil
	}

	if err := os.MkdirAll(dir, 0o700); err != nil {
		return "", "", fmt.Errorf("mkdir %s: %w", dir, err)
	}

	if err := generateSelfSigned(certPath, keyPath); err != nil {
		return "", "", fmt.Errorf("generate self-signed cert: %w", err)
	}
	logger.Info("generated self-signed HTTPS certificate at %s (SANs: all device IPv4 + localhost)", certPath)
	return certPath, keyPath, nil
}

// generateSelfSigned writes a fresh ECDSA P-256 self-signed certificate + key.
// The cert is marked IsCA so an operator may optionally import server.crt into
// a browser/device trust store to suppress the self-signed warning.
func generateSelfSigned(certPath, keyPath string) error {
	priv, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		return fmt.Errorf("generate key: %w", err)
	}

	serial, err := rand.Int(rand.Reader, new(big.Int).Lsh(big.NewInt(1), 128))
	if err != nil {
		return fmt.Errorf("serial: %w", err)
	}

	template := x509.Certificate{
		SerialNumber: serial,
		Subject: pkix.Name{
			CommonName:   "ne503",
			Organization: []string{"AIPC"},
		},
		NotBefore:             time.Now().Add(-time.Hour),
		NotAfter:              time.Now().AddDate(10, 0, 0), // 10 years
		KeyUsage:              x509.KeyUsageDigitalSignature | x509.KeyUsageCertSign,
		ExtKeyUsage:           []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		BasicConstraintsValid: true,
		IsCA:                  true,
	}

	// Collect SANs: localhost + all non-loopback IPv4 so the cert matches the
	// LAN address the device is actually reached on.
	template.DNSNames = []string{"localhost"}
	template.IPAddresses = []net.IP{net.IPv4(127, 0, 0, 1), net.IPv6loopback}
	template.IPAddresses = append(template.IPAddresses, localIPv4s()...)

	// Self-signed: the certificate is its own issuer.
	der, err := x509.CreateCertificate(rand.Reader, &template, &template, &priv.PublicKey, priv)
	if err != nil {
		return fmt.Errorf("create certificate: %w", err)
	}

	certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	if err := os.WriteFile(certPath, certPEM, 0o644); err != nil {
		return fmt.Errorf("write cert: %w", err)
	}

	keyDER, err := x509.MarshalECPrivateKey(priv)
	if err != nil {
		return fmt.Errorf("marshal key: %w", err)
	}
	keyPEM := pem.EncodeToMemory(&pem.Block{Type: "EC PRIVATE KEY", Bytes: keyDER})
	if err := os.WriteFile(keyPath, keyPEM, 0o600); err != nil {
		return fmt.Errorf("write key: %w", err)
	}

	return nil
}

// localIPv4s returns all non-loopback IPv4 addresses of the host so the
// self-signed cert matches the LAN IP used to reach the device.
func localIPv4s() []net.IP {
	var ips []net.IP
	ifaces, err := net.Interfaces()
	if err != nil {
		return ips
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		addrs, err := iface.Addrs()
		if err != nil {
			continue
		}
		for _, addr := range addrs {
			var ip net.IP
			switch v := addr.(type) {
			case *net.IPNet:
				ip = v.IP
			case *net.IPAddr:
				ip = v.IP
			}
			if ip == nil || ip.IsLoopback() {
				continue
			}
			if v4 := ip.To4(); v4 != nil {
				ips = append(ips, v4)
			}
		}
	}
	return ips
}

// httpRedirectHandler returns a handler that 301-redirects every request to the
// HTTPS listener. httpsAddr is the configured TLS listen address (e.g. ":443");
// the port is appended to the host unless it is the default 443.
func httpRedirectHandler(httpsAddr string) http.Handler {
	port := strings.TrimPrefix(httpsAddr, ":")
	hostSuffix := ""
	if port != "" && port != "443" {
		hostSuffix = ":" + port
	}
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		// Use only the host portion, stripping any existing port from the
		// request's Host header, then re-append the HTTPS port.
		host := r.Host
		if h, _, err := net.SplitHostPort(host); err == nil {
			host = h
		}
		target := "https://" + host + hostSuffix + r.URL.RequestURI()
		http.Redirect(w, r, target, http.StatusMovedPermanently)
	})
}

func fileExists(p string) bool {
	_, err := os.Stat(p)
	return err == nil
}

func validateReadable(p string) error {
	f, err := os.Open(p)
	if err != nil {
		return err
	}
	return f.Close()
}
