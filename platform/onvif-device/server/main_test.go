package main

import (
	"crypto/sha1" //nolint:gosec // ONVIF digest test fixture
	"encoding/base64"
	"encoding/xml"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"

	onvifserver "github.com/0x524a/onvif-go/server"

	"aipc/platform/onvif-device/config"
)

// testConfig returns a minimal valid config with one profile for helper tests.
func testConfig() *config.Config {
	return &config.Config{
		Service:  config.ServiceConfig{Enabled: true, HTTPPort: 8081, BasePath: "/onvif"},
		Network:  config.NetworkConfig{Interface: "eth0", MulticastAddr: "239.255.255.250", MulticastPort: 3702},
		Device:   config.DeviceConfig{Manufacturer: "CamThink", Model: "NE503", HardwareID: "NE503", Scopes: []string{"onvif://x"}},
		RTSP:     config.RTSPConfig{Port: 8554},
		Profiles: []config.ProfileConfig{{Token: "main", Name: "Main", Stream: "main", Width: 1920, Height: 1080, FPS: 30, Codec: "H264", Bitrate: 4096}},
		Auth:     config.AuthConfig{Mode: "none"},
	}
}

func TestRtspURI(t *testing.T) {
	// Arrange / Act
	got := rtspURI("10.0.0.5", testConfig(), config.ProfileConfig{Stream: "sub"})

	// Assert
	if got != "rtsp://10.0.0.5:8554/sub" {
		t.Errorf("rtspURI = %q, want rtsp://10.0.0.5:8554/sub", got)
	}
}

func TestDeviceXAddr(t *testing.T) {
	// Arrange / Act
	got := deviceXAddr("10.0.0.5", testConfig())

	// Assert
	if got != "http://10.0.0.5:8081/onvif/device_service" {
		t.Errorf("deviceXAddr = %q, want http://10.0.0.5:8081/onvif/device_service", got)
	}
}

func TestEnvOr(t *testing.T) {
	// Arrange / Act / Assert — present env var wins.
	t.Setenv("ONVIF_TEST_VAR", "from-env")
	if got := envOr("ONVIF_TEST_VAR", "fallback"); got != "from-env" {
		t.Errorf("envOr(present) = %q, want from-env", got)
	}
	// Absent env var falls back.
	if got := envOr("ONVIF_TEST_UNSET", "fallback"); got != "fallback" {
		t.Errorf("envOr(absent) = %q, want fallback", got)
	}
}

func TestResolveAuth(t *testing.T) {
	cases := []struct {
		name string
		mode string
		cfg  config.AuthConfig
		env  map[string]string
		want [2]string // username, password
	}{
		{
			name: "none-mode-no-auth",
			mode: "none",
			want: [2]string{"", ""},
		},
		{
			name: "none-mode-ignores-config-creds",
			mode: "none",
			cfg:  config.AuthConfig{Mode: "none", Username: "cfg", Password: "cfg"},
			want: [2]string{"", ""},
		},
		{
			name: "digest-uses-env-creds",
			mode: "digest",
			env:  map[string]string{"AIPC_AUTH_USERNAME": "envuser", "AIPC_AUTH_PASSWORD": "envpass"},
			want: [2]string{"envuser", "envpass"},
		},
		{
			name: "digest-falls-back-to-config-creds",
			mode: "digest",
			cfg:  config.AuthConfig{Mode: "digest", Username: "cfguser", Password: "cfgpass"},
			want: [2]string{"cfguser", "cfgpass"},
		},
		{
			name: "digest-missing-all-creds-disables-auth",
			mode: "digest",
			want: [2]string{"", ""},
		},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			// Arrange
			t.Setenv("AIPC_AUTH_USERNAME", "")
			t.Setenv("AIPC_AUTH_PASSWORD", "")
			for k, v := range tc.env {
				t.Setenv(k, v)
			}
			cfg := testConfig()
			cfg.Auth = tc.cfg
			if cfg.Auth.Mode == "" {
				cfg.Auth.Mode = tc.mode
			}

			// Act
			user, pass := resolveAuth(cfg)

			// Assert
			if user != tc.want[0] || pass != tc.want[1] {
				t.Errorf("resolveAuth() = (%q,%q), want (%q,%q)", user, pass, tc.want[0], tc.want[1])
			}
		})
	}
}

func TestBuildServerConfig_MapsIdentityAndProfiles(t *testing.T) {
	// Arrange
	cfg := testConfig()

	// Act
	sc := buildServerConfig(cfg, "CT503-0001", "9.9.9", "10.0.0.5")

	// Assert
	if sc.Host != "10.0.0.5" {
		t.Errorf("Host = %q, want 10.0.0.5 (advertised LAN IP, not 0.0.0.0)", sc.Host)
	}
	if sc.Port != 8081 {
		t.Errorf("Port = %d, want 8081", sc.Port)
	}
	if sc.BasePath != "/onvif" {
		t.Errorf("BasePath = %q, want /onvif", sc.BasePath)
	}
	if sc.DeviceInfo.SerialNumber != "CT503-0001" {
		t.Errorf("SerialNumber = %q, want CT503-0001", sc.DeviceInfo.SerialNumber)
	}
	if sc.DeviceInfo.FirmwareVersion != "9.9.9" {
		t.Errorf("FirmwareVersion = %q, want 9.9.9", sc.DeviceInfo.FirmwareVersion)
	}
	if sc.SupportPTZ || sc.SupportImaging || sc.SupportEvents {
		t.Errorf("Phase 1 must disable PTZ/Imaging/Events; got PTZ=%v Imaging=%v Events=%v",
			sc.SupportPTZ, sc.SupportImaging, sc.SupportEvents)
	}
	if len(sc.Profiles) != 1 {
		t.Fatalf("Profiles len = %d, want 1", len(sc.Profiles))
	}
	if sc.Profiles[0].VideoEncoder.Encoding != "H264" {
		t.Errorf("profile codec = %q, want H264", sc.Profiles[0].VideoEncoder.Encoding)
	}
}

func TestBuildDiscoveryConfig_AssemblesXAddrAndScopes(t *testing.T) {
	// Arrange
	cfg := testConfig()

	// Act
	dc := buildDiscoveryConfig(cfg, "urn:uuid:EP", "10.0.0.5")

	// Assert
	if dc.EndpointUUID != "urn:uuid:EP" {
		t.Errorf("EndpointUUID = %q", dc.EndpointUUID)
	}
	if dc.Types != "dp0:NetworkVideoTransmitter" {
		t.Errorf("Types = %q, want NetworkVideoTransmitter", dc.Types)
	}
	if len(dc.XAddrs) != 1 || dc.XAddrs[0] != "http://10.0.0.5:8081/onvif/device_service" {
		t.Errorf("XAddrs = %v, want [http://10.0.0.5:8081/onvif/device_service]", dc.XAddrs)
	}
	if len(dc.Scopes) != 1 || dc.Scopes[0] != "onvif://x" {
		t.Errorf("Scopes = %v", dc.Scopes)
	}
}

const (
	mediaNS  = "http://www.onvif.org/ver10/media/wsdl"
	deviceNS = "http://www.onvif.org/ver10/device/wsdl"
)

// soapEnvelope wraps a body element in a minimal SOAP 1.2 envelope for tests.
func soapEnvelope(action, ns, inner string) string {
	return `<?xml version="1.0" encoding="UTF-8"?>
<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">
  <s:Body>
    <` + action + ` xmlns="` + ns + `">` + inner + `</` + action + `>
  </s:Body>
</s:Envelope>`
}

// TestGetStreamUri_RoundTripReturnsSpecCasingAndURI is the regression test for the
// library casing bug: previously GetStreamUri faulted with "No handler for
// action: GetStreamUri". It must now return a GetStreamUriResponse (lowercase
// Uri) carrying the RTSP URI.
func TestGetStreamUri_RoundTripReturnsSpecCasingAndURI(t *testing.T) {
	// Arrange — server with one profile, RTSP URI overridden to the LAN IP.
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "127.0.0.1"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	if err := srv.UpdateStreamURI("main", "rtsp://127.0.0.1:8554/main"); err != nil {
		t.Fatalf("UpdateStreamURI: %v", err)
	}

	mux := http.NewServeMux()
	registerMediaRoutes(mux, srv)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	body := soapEnvelope("GetStreamUri", mediaNS, "<ProfileToken>main</ProfileToken>")

	// Act
	resp, err := http.Post(ts.URL+"/onvif/media_service", "application/soap+xml", strings.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	respBody, _ := io.ReadAll(resp.Body)

	// Assert
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", resp.StatusCode, respBody)
	}
	if strings.Contains(string(respBody), "No handler for action") {
		t.Fatalf("GetStreamUri not routed (casing bug regressed); body=%s", respBody)
	}
	if !strings.Contains(string(respBody), "GetStreamUriResponse") {
		t.Errorf("response missing spec-correct GetStreamUriResponse element; body=%s", respBody)
	}
	if strings.Contains(string(respBody), "GetStreamURIResponse") {
		t.Errorf("response uses non-conformant capital GetStreamURIResponse; body=%s", respBody)
	}
	if !strings.Contains(string(respBody), "rtsp://127.0.0.1:8554/main") {
		t.Errorf("response missing RTSP URI; body=%s", respBody)
	}
}

// TestGetSnapshotUri_UsesSpecCasing asserts the action routes under lowercase
// "Uri" rather than faulting with "No handler". (Snapshot itself is unsupported
// in Phase 1, so a handler-error fault is expected and acceptable.)
func TestGetSnapshotUri_UsesSpecCasing(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "127.0.0.1"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	mux := http.NewServeMux()
	registerMediaRoutes(mux, srv)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// Act
	body := soapEnvelope("GetSnapshotUri", mediaNS, "<ProfileToken>main</ProfileToken>")
	resp, err := http.Post(ts.URL+"/onvif/media_service", "application/soap+xml", strings.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	respBody, _ := io.ReadAll(resp.Body)

	// Assert — routing under the spec key must succeed; only the snapshot
	// payload is unsupported.
	if strings.Contains(string(respBody), "No handler for action") {
		t.Fatalf("GetSnapshotUri not routed (casing bug regressed); body=%s", respBody)
	}
}

// TestGetCapabilities_AdvertisesConfiguredHostNotLocalhost is the regression test
// for the localhost XAddr bug: with Host set to the LAN IP, GetCapabilities must
// advertise that IP, never "localhost".
func TestGetCapabilities_AdvertisesConfiguredHostNotLocalhost(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	mux := http.NewServeMux()
	registerDeviceRoutes(mux, srv)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// Act
	body := soapEnvelope("GetCapabilities", deviceNS, "<Category>All</Category>")
	resp, err := http.Post(ts.URL+"/onvif/device_service", "application/soap+xml", strings.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	respBody, _ := io.ReadAll(resp.Body)

	// Assert
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", resp.StatusCode, respBody)
	}
	if strings.Contains(string(respBody), "localhost") {
		t.Errorf("GetCapabilities advertises localhost; body=%s", respBody)
	}
	if !strings.Contains(string(respBody), "192.168.1.50:8081") {
		t.Errorf("GetCapabilities does not advertise configured host 192.168.1.50:8081; body=%s", respBody)
	}
}

// pongResponse is a minimal response type for dispatcher unit tests.
type pongResponse struct {
	XMLName xml.Name `xml:"http://example.com/ping Pong"`
	OK      bool     `xml:"OK"`
}

// newTestDispatcher builds a dispatcher with a single "Ping" action for branch
// tests. creds are empty unless overridden.
func newTestDispatcher(t *testing.T, user, pass string) *soapDispatcher {
	t.Helper()
	d := newSOAPDispatcher(user, pass)
	d.handle("Ping", func(body interface{}) (interface{}, error) {
		return &pongResponse{OK: true}, nil
	})
	return d
}

// wsseHeader builds a WS-Security UsernameToken header with a valid ONVIF digest.
func wsseHeader(user, pass string) string {
	nonce := []byte("0123456789abcdef") // fixed for deterministic tests
	created := "2026-08-11T00:00:00Z"
	h := sha1.New() //nolint:gosec // ONVIF digest fixture
	h.Write(nonce)
	h.Write([]byte(created))
	h.Write([]byte(pass))
	digest := base64.StdEncoding.EncodeToString(h.Sum(nil))
	return fmt.Sprintf(`<Security xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-secext-1.0.xsd">
      <UsernameToken>
        <Username>%s</Username>
        <Password Type="...PasswordDigest">%s</Password>
        <Nonce EncodingType="...Base64Binary">%s</Nonce>
        <Created xmlns="http://docs.oasis-open.org/wss/2004/01/oasis-200401-wss-wssecurity-utility-1.0.xsd">%s</Created>
      </UsernameToken>
    </Security>`, user, digest, base64.StdEncoding.EncodeToString(nonce), created)
}

// soapRequestWithHeader builds a SOAP envelope containing an optional header and
// a body action, for dispatcher tests.
func soapRequestWithHeader(header, action, inner string) string {
	hdr := ""
	if header != "" {
		hdr = "<s:Header>" + header + "</s:Header>"
	}
	return fmt.Sprintf(`<?xml version="1.0"?><s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope">%s
      <s:Body><%s xmlns="http://example.com/ping">%s</%s></s:Body>
    </s:Envelope>`, hdr, action, inner, action)
}

func postSOAP(t *testing.T, ts *httptest.Server, path, body string) (int, string) {
	t.Helper()
	resp, err := http.Post(ts.URL+path, "application/soap+xml", strings.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	b, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(b)
}

func TestSOAPDispatcher_MethodNotAllowed(t *testing.T) {
	ts := httptest.NewServer(newTestDispatcher(t, "", ""))
	defer ts.Close()
	resp, err := http.Get(ts.URL + "/x")
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	defer resp.Body.Close()
	if resp.StatusCode != http.StatusMethodNotAllowed {
		t.Errorf("GET status = %d, want 405", resp.StatusCode)
	}
}

func TestSOAPDispatcher_UnknownActionIsFault(t *testing.T) {
	ts := httptest.NewServer(newTestDispatcher(t, "", ""))
	defer ts.Close()
	status, body := postSOAP(t, ts, "/x", soapRequestWithHeader("", "Bogus", ""))
	if !strings.Contains(body, "No handler for action") {
		t.Errorf("expected no-handler fault; body=%s", body)
	}
	if status != http.StatusInternalServerError {
		t.Errorf("status = %d, want 500", status)
	}
}

func TestSOAPDispatcher_NoCredsSkipsAuth(t *testing.T) {
	ts := httptest.NewServer(newTestDispatcher(t, "", ""))
	defer ts.Close()
	status, body := postSOAP(t, ts, "/x", soapRequestWithHeader("", "Ping", ""))
	if status != http.StatusOK {
		t.Errorf("status = %d, want 200; body=%s", status, body)
	}
	if !strings.Contains(body, "Pong") {
		t.Errorf("expected Pong response; body=%s", body)
	}
}

func TestSOAPDispatcher_CredsRejectMissingToken(t *testing.T) {
	ts := httptest.NewServer(newTestDispatcher(t, "admin", "secret"))
	defer ts.Close()
	status, body := postSOAP(t, ts, "/x", soapRequestWithHeader("", "Ping", ""))
	if status != http.StatusInternalServerError {
		t.Errorf("status = %d, want 500 (auth fault)", status)
	}
	if !strings.Contains(body, "Authentication failed") {
		t.Errorf("expected auth fault; body=%s", body)
	}
}

func TestSOAPDispatcher_CredsAcceptValidDigest(t *testing.T) {
	ts := httptest.NewServer(newTestDispatcher(t, "admin", "secret"))
	defer ts.Close()
	req := soapRequestWithHeader(wsseHeader("admin", "secret"), "Ping", "")
	status, body := postSOAP(t, ts, "/x", req)
	if status != http.StatusOK {
		t.Errorf("status = %d, want 200; body=%s", status, body)
	}
	if !strings.Contains(body, "Pong") {
		t.Errorf("expected Pong after valid digest; body=%s", body)
	}
}

func TestSOAPDispatcher_CredsRejectBadPassword(t *testing.T) {
	ts := httptest.NewServer(newTestDispatcher(t, "admin", "secret"))
	defer ts.Close()
	req := soapRequestWithHeader(wsseHeader("admin", "wrong"), "Ping", "")
	status, body := postSOAP(t, ts, "/x", req)
	if status != http.StatusInternalServerError {
		t.Errorf("status = %d, want 500", status)
	}
	if !strings.Contains(body, "Authentication failed") {
		t.Errorf("expected auth fault for bad password; body=%s", body)
	}
}

func TestFirstElementName(t *testing.T) {
	cases := []struct{ in, want string }{
		{`<GetStreamUri xmlns="x"><ProfileToken>main</ProfileToken></GetStreamUri>`, "GetStreamUri"},
		{`  <tns:GetProfiles xmlns:tns="y"/>`, "GetProfiles"},
		{`   `, ""},
		{`not xml`, ""},
	}
	for _, tc := range cases {
		if got := firstElementName([]byte(tc.in)); got != tc.want {
			t.Errorf("firstElementName(%q) = %q, want %q", tc.in, got, tc.want)
		}
	}
}
