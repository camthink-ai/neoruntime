package main

import (
	"context"
	"net"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"
	"time"

	"github.com/gin-gonic/gin"

	"aipc/platform/platform-api/auth"
)

// TestUnixFaceServesEngineWithoutToken verifies the local unix socket face
// end-to-end: a tokenless request dialed over the socket passes auth
// middleware, exactly like apiHTTPClient routes loopback calls.
func TestUnixFaceServesEngineWithoutToken(t *testing.T) {
	gin.SetMode(gin.TestMode)
	engine := gin.New()
	engine.GET("/api/v1/ping", auth.Middleware(auth.NewTokenValidator("integration-api-key", true)), func(c *gin.Context) {
		c.String(http.StatusOK, "pong")
	})

	sockPath := filepath.Join(t.TempDir(), "platform-api.sock")
	listener, err := net.Listen("unix", sockPath)
	if err != nil {
		t.Fatalf("net.Listen(unix) error = %v", err)
	}

	server := &http.Server{Handler: localTrustHandler(engine), ReadTimeout: 5 * time.Second}
	go server.Serve(listener)
	defer server.Close()

	unixClient := &http.Client{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, network, addr string) (net.Conn, error) {
				return net.Dial("unix", sockPath)
			},
		},
		Timeout: 5 * time.Second,
	}

	// The URL targets a loopback TCP host:port, but the dialer routes the
	// connection to the unix socket, exactly like apiHTTPClient does.
	resp, err := unixClient.Get("http://127.0.0.1:8080/api/v1/ping")
	if err != nil {
		t.Fatalf("unix-face GET error = %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("unix-face tokenless request: got status %d, want %d", resp.StatusCode, http.StatusOK)
	}
}

// TestLocalTrustHandlerInjectsMark verifies the wrapper injects the
// server-side trust mark into the request context before dispatch, while
// the plain engine (TCP face) does not get it.
func TestLocalTrustHandlerInjectsMark(t *testing.T) {
	marked := make(chan bool, 1)
	inner := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		marked <- auth.IsLocalSocket(r.Context())
		w.WriteHeader(http.StatusOK)
	})

	localTrustHandler(inner).ServeHTTP(httptest.NewRecorder(), httptest.NewRequest(http.MethodGet, "/", nil))
	if <-marked != true {
		t.Fatal("localTrustHandler did not inject the local-trust context mark")
	}

	inner2 := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		marked <- auth.IsLocalSocket(r.Context())
		w.WriteHeader(http.StatusOK)
	})
	inner2.ServeHTTP(httptest.NewRecorder(), httptest.NewRequest(http.MethodGet, "/", nil))
	if <-marked != false {
		t.Fatal("plain handler context unexpectedly carried the local-trust mark")
	}
}
