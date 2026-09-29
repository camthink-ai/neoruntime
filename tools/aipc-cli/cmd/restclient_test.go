package cmd

import (
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"
	"time"

	"github.com/spf13/viper"
)

// newUnixTestServer starts a token-free HTTP server on a unix socket and
// returns its path plus a recorder of received request lines.
func newUnixTestServer(t *testing.T) (string, <-chan string) {
	t.Helper()
	sockPath := filepath.Join(t.TempDir(), "platform-api.sock")
	listener, err := net.Listen("unix", sockPath)
	if err != nil {
		t.Fatalf("net.Listen(unix) error = %v", err)
	}

	requests := make(chan string, 4)
	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/ping", func(w http.ResponseWriter, r *http.Request) {
		requests <- fmt.Sprintf("%s %s Host=%s", r.Method, r.URL.Path, r.Host)
		fmt.Fprint(w, "pong")
	})

	server := &http.Server{Handler: mux, ReadTimeout: 5 * time.Second}
	go server.Serve(listener)
	t.Cleanup(func() { server.Close() })

	return sockPath, requests
}

func TestUseAPISocketRouting(t *testing.T) {
	sockPath, _ := newUnixTestServer(t)
	viper.Set("api.unix_socket", sockPath)
	t.Cleanup(func() { viper.Set("api.unix_socket", "") })

	cases := []struct {
		addr string
		want bool
	}{
		{"127.0.0.1:8080", true},
		{"localhost:8080", true},
		{"[::1]:8080", true},
		{"192.168.93.72:8080", false},
		{"example.com:8080", false},
		{"not-an-addr", false},
	}
	for _, tc := range cases {
		if got := useAPISocket(tc.addr); got != tc.want {
			t.Errorf("useAPISocket(%q) = %v, want %v", tc.addr, got, tc.want)
		}
	}

	// A configured but missing socket forces TCP even for loopback targets.
	viper.Set("api.unix_socket", filepath.Join(t.TempDir(), "absent.sock"))
	if useAPISocket("127.0.0.1:8080") {
		t.Error("useAPISocket(127.0.0.1:8080) with absent socket = true, want false")
	}
}

func TestAPIHTTPClientUsesUnixSocket(t *testing.T) {
	sockPath, requests := newUnixTestServer(t)
	viper.Set("api.unix_socket", sockPath)
	t.Cleanup(func() { viper.Set("api.unix_socket", "") })

	resp, err := apiHTTPClient.Get("http://localhost:8080/api/v1/ping")
	if err != nil {
		t.Fatalf("apiHTTPClient.Get error = %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want %d", resp.StatusCode, http.StatusOK)
	}

	select {
	case got := <-requests:
		if want := "GET /api/v1/ping Host=localhost:8080"; got != want {
			t.Fatalf("server saw %q, want %q", got, want)
		}
	case <-time.After(5 * time.Second):
		t.Fatal("no request arrived on the unix socket")
	}
}

func TestAPIHTTPClientFallsBackToTCP(t *testing.T) {
	// httptest servers bind a loopback address, so the socket path must be
	// absent for the client to dial TCP.
	viper.Set("api.unix_socket", filepath.Join(t.TempDir(), "absent.sock"))
	t.Cleanup(func() { viper.Set("api.unix_socket", "") })

	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, "pong")
	}))
	defer ts.Close()

	resp, err := apiHTTPClient.Get(ts.URL + "/api/v1/ping")
	if err != nil {
		t.Fatalf("apiHTTPClient.Get error = %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want %d", resp.StatusCode, http.StatusOK)
	}
}
