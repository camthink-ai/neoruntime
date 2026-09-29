package cmd

import (
	"context"
	"net"
	"net/http"
	"os"
	"strings"
	"time"

	"github.com/spf13/viper"
)

// defaultAPIUnixSocket is the platform-api local auth-exempt unix socket face.
const defaultAPIUnixSocket = "/run/aipc/platform-api.sock"

// apiHTTPClient is the shared HTTP client for all REST commands. Its dialer
// redirects loopback-destined requests to the platform-api unix socket face
// when that socket exists (token-free local access, same trust model as the
// gRPC subcommands); everything else — remote --api targets, or a missing
// socket — dials plain TCP as before.
var apiHTTPClient = &http.Client{
	Transport: &http.Transport{
		Proxy:                 http.ProxyFromEnvironment,
		DialContext:           dialAPISocketOrTCP,
		MaxIdleConns:          100,
		IdleConnTimeout:       90 * time.Second,
		TLSHandshakeTimeout:   10 * time.Second,
		ExpectContinueTimeout: 1 * time.Second,
	},
	Timeout: 0, // per-command contexts govern; large uploads/downloads must not be cut off here
}

// dialAPISocketOrTCP dials the platform-api unix socket for loopback
// destinations when the socket is present, and falls back to a normal TCP
// dial otherwise.
func dialAPISocketOrTCP(ctx context.Context, network, addr string) (net.Conn, error) {
	dialer := &net.Dialer{Timeout: 10 * time.Second, KeepAlive: 30 * time.Second}
	if useAPISocket(addr) {
		return dialer.DialContext(ctx, "unix", apiUnixSocketPath())
	}
	return dialer.DialContext(ctx, network, addr)
}

// useAPISocket reports whether addr (host:port) should be redirected to the
// local platform-api unix socket: loopback destination and socket present.
func useAPISocket(addr string) bool {
	host, _, err := net.SplitHostPort(addr)
	if err != nil {
		return false
	}
	if host != "localhost" && !net.ParseIP(host).IsLoopback() {
		return false
	}
	_, err = os.Stat(apiUnixSocketPath())
	return err == nil
}

// apiUnixSocketPath resolves the socket path from the config key
// `api.unix_socket` (accepts a bare path or a unix:// URL), falling back to
// the packaged default. Point it at a non-existent path to force TCP.
func apiUnixSocketPath() string {
	p := viper.GetString("api.unix_socket")
	if p == "" {
		return defaultAPIUnixSocket
	}
	return strings.TrimPrefix(p, "unix://")
}
