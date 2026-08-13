package main

import (
	"bytes"
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

// ttNS is the ONVIF schema ("tt") namespace that data-layer elements must use.
const ttNS = "http://www.onvif.org/ver10/schema"

// elemNamespace scans a SOAP response and returns the namespace URI of the
// first start element whose local name matches. Namespace URI (not prefix) is
// what strict WSDL proxies such as ONVIF Device Manager resolve against, so it
// is the correct thing to assert regardless of prefix.
func elemNamespace(body []byte, local string) string {
	dec := xml.NewDecoder(bytes.NewReader(body))
	for {
		tok, err := dec.Token()
		if err != nil {
			return ""
		}
		if se, ok := tok.(xml.StartElement); ok && se.Name.Local == local {
			return se.Name.Space
		}
	}
}

// postDeviceAction posts an action to the device service and returns the body.
func postDeviceAction(t *testing.T, srv *onvifserver.Server, action, inner string) string {
	t.Helper()
	mux := http.NewServeMux()
	registerDeviceRoutes(mux, srv)
	ts := httptest.NewServer(mux)
	defer ts.Close()
	body := soapEnvelope(action, deviceNS, inner)
	resp, err := http.Post(ts.URL+"/onvif/device_service", "application/soap+xml", strings.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	out, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("%s status = %d, want 200; body=%s", action, resp.StatusCode, out)
	}
	return string(out)
}

// TestGetDeviceInformation_NamespaceDepth is the regression test for ODM's
// "server returned invalid SOAP" on the Identification tab. GetDeviceInformation
// and all its children (Manufacturer, Model, …) are locally-declared xs:string
// elements in the device WSDL, so they all belong in device/wsdl — NOT the tt
// schema namespace. (An earlier fix wrongly pushed them into tt, which broke
// Maintenance and Identification.) The wrapper and every leaf must resolve to
// device/wsdl.
func TestGetDeviceInformation_NamespaceDepth(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}

	// Act
	body := postDeviceAction(t, srv, "GetDeviceInformation", "")

	// Assert — wrapper and every leaf in device/wsdl (locally-declared elements).
	if ns := elemNamespace([]byte(body), "GetDeviceInformationResponse"); ns != deviceNS {
		t.Errorf("wrapper namespace = %q, want %q (device/wsdl)", ns, deviceNS)
	}
	for _, leaf := range []string{"Manufacturer", "Model", "FirmwareVersion", "SerialNumber"} {
		if ns := elemNamespace([]byte(body), leaf); ns != deviceNS {
			t.Errorf("%s namespace = %q, want %q (device/wsdl); body=%s", leaf, ns, deviceNS, body)
		}
	}
}

// TestGetCapabilities_NamespaceDepth asserts the correct depth: the response
// wrapper and the locally-declared Capabilities bridge stay in device/wsdl,
// while the children defined inside the tt:Capabilities type (Device, Media) and
// their sub-children (XAddr) emit in the tt schema namespace.
func TestGetCapabilities_NamespaceDepth(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}

	// Act
	body := postDeviceAction(t, srv, "GetCapabilities", "<Category>All</Category>")

	// Assert
	if ns := elemNamespace([]byte(body), "GetCapabilitiesResponse"); ns != deviceNS {
		t.Errorf("wrapper namespace = %q, want %q (device/wsdl)", ns, deviceNS)
	}
	if ns := elemNamespace([]byte(body), "Capabilities"); ns != deviceNS {
		t.Errorf("Capabilities bridge namespace = %q, want %q (device/wsdl); body=%s", ns, deviceNS, body)
	}
	for _, el := range []string{"Device", "Media", "XAddr"} {
		if ns := elemNamespace([]byte(body), el); ns != ttNS {
			t.Errorf("%s namespace = %q, want %q (schema); body=%s", el, ns, ttNS, body)
		}
	}
}

// TestGetProfiles_NamespaceDepth asserts the correct depth for media profiles:
// the response wrapper and the locally-declared Profiles bridge stay in
// media/wsdl, while the children defined inside tt:Profile (Name,
// VideoEncoderConfiguration, Encoding, …) emit in the tt schema namespace —
// which is what lets ODM decode profiles for live video.
func TestGetProfiles_NamespaceDepth(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	mux := http.NewServeMux()
	registerMediaRoutes(mux, srv)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// Act
	body := soapEnvelope("GetProfiles", mediaNS, "")
	resp, err := http.Post(ts.URL+"/onvif/media_service", "application/soap+xml", strings.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	out, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", resp.StatusCode, out)
	}

	// Assert — wrapper and Profiles bridge in media/wsdl; tt-typed children in schema.
	respBody := string(out)
	if ns := elemNamespace([]byte(respBody), "GetProfilesResponse"); ns != mediaNS {
		t.Errorf("wrapper namespace = %q, want %q (media/wsdl)", ns, mediaNS)
	}
	if ns := elemNamespace([]byte(respBody), "Profiles"); ns != mediaNS {
		t.Errorf("Profiles bridge namespace = %q, want %q (media/wsdl); body=%s", ns, mediaNS, respBody)
	}
	for _, el := range []string{"Name", "VideoEncoderConfiguration", "Encoding"} {
		if ns := elemNamespace([]byte(respBody), el); ns != ttNS {
			t.Errorf("%s namespace = %q, want %q (schema); body=%s", el, ns, ttNS, respBody)
		}
	}
}

// TestGetProfiles_H264ProfileIsHigh locks in the blemish fix: the onvif-go
// library hardcodes H264Profile="Main" (server/media.go), but the Hailo encoder
// emits High profile for both streams (confirmed via live SDP profile-level-id:
// main High@L5.1, sub High@L3.1). loadProfiles normalizes the configured value
// to "High" so NVRs that read H264Profile from the ONVIF config (rather than the
// in-band SPS) select the correct decoder.
func TestGetProfiles_H264ProfileIsHigh(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	mux := http.NewServeMux()
	registerMediaRoutes(mux, srv)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// Act
	body := soapEnvelope("GetProfiles", mediaNS, "")
	resp, err := http.Post(ts.URL+"/onvif/media_service", "application/soap+xml", strings.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	out, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", resp.StatusCode, out)
	}

	// Assert — configured H264 profile is High, not the library default Main.
	respBody := string(out)
	if !strings.Contains(respBody, "H264Profile>High<") {
		t.Errorf("H264Profile not High; body=%s", respBody)
	}
	if strings.Contains(respBody, "H264Profile>Main<") {
		t.Errorf("H264Profile still library default Main; body=%s", respBody)
	}
}

// fakeStreamStatus is a test-only streamStatusProvider returning a fixed param
// map (nil map simulates "no active stream" → enrichProfiles falls back).
type fakeStreamStatus struct {
	params map[string]*streamParams
}

func (f fakeStreamStatus) RunningParams() map[string]*streamParams { return f.params }

// postMediaAction posts a media-service action and returns the response body.
func postMediaAction(t *testing.T, srv *onvifserver.Server, action, inner string) string {
	t.Helper()
	mux := http.NewServeMux()
	registerMediaRoutes(mux, srv)
	ts := httptest.NewServer(mux)
	defer ts.Close()
	body := soapEnvelope(action, mediaNS, inner)
	resp, err := http.Post(ts.URL+"/onvif/media_service", "application/soap+xml", strings.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	out, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("%s status = %d, want 200; body=%s", action, resp.StatusCode, out)
	}
	return string(out)
}

// TestGetProfiles_LiveOverlayReplacesStaticParams locks in the dynamic-metadata
// feature: with a camera-daemon provider reporting the "main" stream at 4K,
// GetProfiles must advertise 3840×2160 + the live bitrate — NOT the static
// onvif.yaml 1920×1080. This is what lets an NVR read the real encoder config
// after a runtime web-UI resolution change without an onvif.yaml edit.
func TestGetProfiles_LiveOverlayReplacesStaticParams(t *testing.T) {
	// Arrange — static config declares main=1920×1080; live provider reports 4K.
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	prev := liveStreams
	liveStreams = fakeStreamStatus{params: map[string]*streamParams{
		"main": {Codec: "h264", Width: 3840, Height: 2160, Fps: 30, BitrateKbps: 8000, Gop: 30},
	}}
	t.Cleanup(func() { liveStreams = prev })

	// Act
	respBody := postMediaAction(t, srv, "GetProfiles", "")

	// Assert — live 4K params win over static 1080p.
	if !strings.Contains(respBody, "Width>3840<") || !strings.Contains(respBody, "Height>2160<") {
		t.Errorf("live 4K overlay (3840×2160) missing; body=%s", respBody)
	}
	if strings.Contains(respBody, "Width>1920<") || strings.Contains(respBody, "Height>1080<") {
		t.Errorf("static 1080p not overridden by live params; body=%s", respBody)
	}
	// Bitrate overlay: 8000 kbps live vs static 4096.
	if !strings.Contains(respBody, "BitrateLimit>8000<") {
		t.Errorf("live bitrate overlay (8000) missing; body=%s", respBody)
	}
	// H264Profile must stay High under the overlay (f68bc85 must not regress).
	if !strings.Contains(respBody, "H264Profile>High<") {
		t.Errorf("H264Profile not High under live overlay; body=%s", respBody)
	}
}

// TestGetProfiles_FallsBackToStaticWhenNoLiveProvider asserts the silent-fallback
// contract: when camera-daemon is unreachable (no provider wired), onvif-device
// serves the static onvif.yaml params — ONVIF never breaks because of camera-daemon.
func TestGetProfiles_FallsBackToStaticWhenNoLiveProvider(t *testing.T) {
	// Arrange — no live provider (initLiveStreams never ran / socket empty).
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	prev := liveStreams
	liveStreams = nil
	t.Cleanup(func() { liveStreams = prev })

	// Act
	respBody := postMediaAction(t, srv, "GetProfiles", "")

	// Assert — static 1080p served intact.
	if !strings.Contains(respBody, "Width>1920<") || !strings.Contains(respBody, "Height>1080<") {
		t.Errorf("static fallback (1920×1080) missing with no provider; body=%s", respBody)
	}
}

// TestGetProfiles_FallsBackWhenNoActiveStream covers the in-between case: the
// provider is wired but camera-daemon reports no active encoder (RunningParams
// returns nil) — e.g. streams stopped. enrichProfiles must fall back to static.
func TestGetProfiles_FallsBackWhenNoActiveStream(t *testing.T) {
	// Arrange — provider present but yields no active stream.
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	prev := liveStreams
	liveStreams = fakeStreamStatus{params: nil}
	t.Cleanup(func() { liveStreams = prev })

	// Act
	respBody := postMediaAction(t, srv, "GetProfiles", "")

	// Assert — static 1080p served, not some empty/zero overlay.
	if !strings.Contains(respBody, "Width>1920<") || !strings.Contains(respBody, "Height>1080<") {
		t.Errorf("static fallback missing when provider has no active stream; body=%s", respBody)
	}
}

// TestGetProfile_NamespaceDepth asserts the singular GetProfile response that
// ODM requests when starting live video: wrapper + trt:Profile bridge in
// media/wsdl, tt-typed children in schema.
func TestGetProfile_NamespaceDepth(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	mux := http.NewServeMux()
	registerMediaRoutes(mux, srv)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// Act — request the "main" profile by token (namespaces declared on the
	// envelope, the way real clients send it).
	body := soapEnvelope("GetProfile", mediaNS,
		`<ProfileToken xmlns="`+mediaNS+`">main</ProfileToken>`)
	resp, err := http.Post(ts.URL+"/onvif/media_service", "application/soap+xml", strings.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	out, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", resp.StatusCode, out)
	}

	// Assert — wrapper + Profile bridge in media/wsdl; tt-typed children in schema.
	respBody := string(out)
	if ns := elemNamespace([]byte(respBody), "GetProfileResponse"); ns != mediaNS {
		t.Errorf("wrapper namespace = %q, want %q (media/wsdl)", ns, mediaNS)
	}
	if ns := elemNamespace([]byte(respBody), "Profile"); ns != mediaNS {
		t.Errorf("Profile bridge namespace = %q, want %q (media/wsdl); body=%s", ns, mediaNS, respBody)
	}
	for _, el := range []string{"Name", "VideoEncoderConfiguration", "Encoding"} {
		if ns := elemNamespace([]byte(respBody), el); ns != ttNS {
			t.Errorf("%s namespace = %q, want %q (schema); body=%s", el, ns, ttNS, respBody)
		}
	}
}

// TestGetVideoSourceConfiguration_NamespaceDepth is the regression test for the
// operation that was blocking ODM live video: ODM sends
// GetVideoSourceConfiguration(vs_main) and, before this handler existed, it
// faulted 8x and ODM never reached RTSP. The wrapper stays in media/wsdl; the
// tt:VideoSourceConfiguration subtree (Name, SourceToken) is in the schema ns.
func TestGetVideoSourceConfiguration_NamespaceDepth(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	mux := http.NewServeMux()
	registerMediaRoutes(mux, srv)
	ts := httptest.NewServer(mux)
	defer ts.Close()

	// Act — request the "main" profile's video-source config by its token (vs_main).
	body := soapEnvelope("GetVideoSourceConfiguration", mediaNS,
		`<ConfigurationToken xmlns="`+mediaNS+`">vs_main</ConfigurationToken>`)
	resp, err := http.Post(ts.URL+"/onvif/media_service", "application/soap+xml", strings.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	out, _ := io.ReadAll(resp.Body)
	if resp.StatusCode != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", resp.StatusCode, out)
	}
	respBody := string(out)
	if strings.Contains(respBody, "No handler for action") {
		t.Fatalf("GetVideoSourceConfiguration not routed; body=%s", respBody)
	}

	// Assert — wrapper in media/wsdl; tt:VideoSourceConfiguration subtree in schema.
	if ns := elemNamespace([]byte(respBody), "GetVideoSourceConfigurationResponse"); ns != mediaNS {
		t.Errorf("wrapper namespace = %q, want %q (media/wsdl)", ns, mediaNS)
	}
	if ns := elemNamespace([]byte(respBody), "VideoSourceConfiguration"); ns != ttNS {
		t.Errorf("VideoSourceConfiguration namespace = %q, want %q (schema); body=%s", ns, ttNS, respBody)
	}
	for _, el := range []string{"Name", "SourceToken"} {
		if ns := elemNamespace([]byte(respBody), el); ns != ttNS {
			t.Errorf("%s namespace = %q, want %q (schema); body=%s", el, ns, ttNS, respBody)
		}
	}
	if !strings.Contains(respBody, "vs_main") {
		t.Errorf("response missing the requested source token vs_main; body=%s", respBody)
	}
}

// TestGetVideoEncoderConfigurationOptions_UsesH264OptionsElement guards the ver10
// element name "H264Options" inside the Options response. ODM and other ver10
// clients look for H264Options to populate the encoder editor's
// resolution/profile/range dropdowns; emitting the ver20 "H264" name (which is
// the *config* element name, not the options element name) makes them find
// nothing and the editor never populates. The Options response must carry
// H264Options and must NOT carry a bare H264 options container.
func TestGetVideoEncoderConfigurationOptions_UsesH264OptionsElement(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	body := soapEnvelope("GetVideoEncoderConfigurationOptions", mediaNS,
		`<ConfigurationToken xmlns="`+mediaNS+`">main_encoder</ConfigurationToken>`)

	// Act
	code, respBody := postMediaSOAP(t, srv, body)
	if code != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", code, respBody)
	}
	if strings.Contains(respBody, "No handler for action") {
		t.Fatalf("GetVideoEncoderConfigurationOptions not routed; body=%s", respBody)
	}

	// Assert — ver10 options element name + its sub-elements.
	if !strings.Contains(respBody, "<tt:H264Options>") {
		t.Errorf("response missing <tt:H264Options> (ver10 options element); body=%s", respBody)
	}
	if !strings.Contains(respBody, "<tt:ResolutionsAvailable>") {
		t.Errorf("response missing ResolutionsAvailable under options; body=%s", respBody)
	}
	if !strings.Contains(respBody, "<tt:H264ProfilesSupported>High") {
		t.Errorf("response missing High H264ProfilesSupported; body=%s", respBody)
	}
	// Regression guard: a bare <tt:H264> options container (the ver20 name in a
	// ver10 response) must not appear. <tt:H264> is the config element and has no
	// place in the Options response — its presence is exactly what broke ODM.
	if strings.Contains(respBody, "<tt:H264>") {
		t.Errorf("response has bare <tt:H264> options container; want <tt:H264Options>; body=%s", respBody)
	}
	// Regression guard: the ver10 VideoEncoderConfigurationOptions type has NO
	// <Encoding> child — QualityRange is the required first element and the encoding
	// is implied by which option block (H264Options) is present. An out-of-sequence
	// <tt:Encoding> breaks ODM's strict .NET deserializer and it then renders the
	// encoder editor read-only, so Options must not carry it.
	if strings.Contains(respBody, "<tt:Encoding>") {
		t.Errorf("response has <tt:Encoding> inside Options; ver10 Options has no such element (breaks ODM editor); body=%s", respBody)
	}
}

// TestGetVideoEncoderConfiguration_HasFixedFalse guards the required fixed
// attribute on the VideoEncoderConfiguration. The ONVIF Configuration base type
// marks fixed use="required"; a strict client (ODM's .NET proxy) cannot
// deserialize a config that omits it and renders the editor read-only. We emit
// fixed="false" (modifiable) since we honour SetVideoEncoderConfiguration.
func TestGetVideoEncoderConfiguration_HasFixedFalse(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	body := soapEnvelope("GetVideoEncoderConfiguration", mediaNS,
		`<ConfigurationToken xmlns="`+mediaNS+`">main_encoder</ConfigurationToken>`)

	// Act
	code, respBody := postMediaSOAP(t, srv, body)
	if code != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", code, respBody)
	}
	if strings.Contains(respBody, "No handler for action") {
		t.Fatalf("GetVideoEncoderConfiguration not routed; body=%s", respBody)
	}

	// Assert — the config element carries the required, modifiable fixed="false".
	if !strings.Contains(respBody, `<tt:VideoEncoderConfiguration token="main_encoder" fixed="false">`) {
		t.Errorf("response missing required fixed=\"false\" on VideoEncoderConfiguration; body=%s", respBody)
	}
	// Regression guard: Multicast is a REQUIRED element of
	// VideoEncoderConfiguration (between the codec block and SessionTimeout).
	// Omitting it makes ODM's Video streaming panel dereference a null
	// Multicast → NullReferenceException. The inert no-multicast block must be
	// present and schema-complete (Address/Type/IPv4Address/Port/TTL/AutoStart).
	if !strings.Contains(respBody, "<tt:Multicast>") ||
		!strings.Contains(respBody, "<tt:Type>IPv4</tt:Type>") ||
		!strings.Contains(respBody, "<tt:IPv4Address>0.0.0.0</tt:IPv4Address>") ||
		!strings.Contains(respBody, "<tt:AutoStart>false</tt:AutoStart>") {
		t.Errorf("response missing required/complete <tt:Multicast> block; body=%s", respBody)
	}
}

// TestSOAPFault_PrefixedSOAP12 guards the fault envelope format. ODM's .NET/WCF
// fault deserializer raised a NullReferenceException ("未将对象引用到对象的实例")
// on the old default-namespace fault with an unqualified <Value>Receiver</Value>;
// the fault must use the same prefixed <s:Envelope> as success responses and a
// qualified Value QName (s:Sender/s:Receiver). Hitting an unregistered action
// exercises the no-handler fault path.
func TestSOAPFault_PrefixedSOAP12(t *testing.T) {
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	body := soapEnvelope("NoSuchAction", mediaNS, "")

	code, respBody := postMediaSOAP(t, srv, body)
	// SOAP 1.2 faults are HTTP 500.
	if code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500 (fault); body=%s", code, respBody)
	}
	// Prefixed envelope matching success responses — NOT the default namespace.
	if !strings.Contains(respBody, "<s:Envelope") {
		t.Errorf("fault not in prefixed <s:Envelope>; body=%s", respBody)
	}
	if strings.Contains(respBody, `<Envelope xmlns=`) {
		t.Errorf("fault uses default-namespace <Envelope> (breaks ODM); body=%s", respBody)
	}
	if !strings.Contains(respBody, "<s:Fault>") {
		t.Errorf("fault missing <s:Fault>; body=%s", respBody)
	}
	// Value MUST be a qualified QName (s:Sender), not bare "Sender".
	if !strings.Contains(respBody, "<s:Value>s:Sender</s:Value>") {
		t.Errorf("fault Value not a qualified QName (want s:Sender); body=%s", respBody)
	}
	if strings.Contains(respBody, "<Value>Sender</Value>") || strings.Contains(respBody, "<Value>Receiver</Value>") {
		t.Errorf("fault has unqualified <Value> (invalid SOAP 1.2); body=%s", respBody)
	}
	// Must start with an XML declaration, like writePrefixedSOAP.
	if !strings.HasPrefix(respBody, `<?xml`) {
		t.Errorf("fault missing <?xml?> declaration; body=%s", respBody)
	}
}

// --- SetVideoEncoderConfiguration (write path) ---

// fakeReconfigurer records the last ReconfigureEncoder call so tests can assert
// the ONVIF params were mapped correctly. err is returned to the handler.
type fakeReconfigurer struct {
	gotStream string
	gotParams streamParams
	called    bool
	err       error
}

func (f *fakeReconfigurer) ReconfigureEncoder(stream string, p streamParams) error {
	f.called = true
	f.gotStream = stream
	f.gotParams = p
	return f.err
}

// setReconfigurer swaps the package-level reconfigurer for a test and restores
// it on cleanup.
func setReconfigurer(t *testing.T, r streamReconfigurer) {
	t.Helper()
	prev := reconfigurer
	reconfigurer = r
	t.Cleanup(func() { reconfigurer = prev })
}

// postMediaSOAP posts a raw SOAP body to the media service and returns the HTTP
// status + body (does not fatal on non-200, so fault paths can be asserted).
func postMediaSOAP(t *testing.T, srv *onvifserver.Server, body string) (int, string) {
	t.Helper()
	mux := http.NewServeMux()
	registerMediaRoutes(mux, srv)
	ts := httptest.NewServer(mux)
	defer ts.Close()
	resp, err := http.Post(ts.URL+"/onvif/media_service", "application/soap+xml", strings.NewReader(body))
	if err != nil {
		t.Fatalf("Post: %v", err)
	}
	defer resp.Body.Close()
	out, _ := io.ReadAll(resp.Body)
	return resp.StatusCode, string(out)
}

// setEncoderConfigBody builds a SetVideoEncoderConfiguration envelope carrying a
// full encoder config (the shape an NVR sends back after a Get, with edits).
func setEncoderConfigBody(token, encoding string, w, h, fps, bitrate, gop int) string {
	return fmt.Sprintf(`<?xml version="1.0"?>`+
		`<s:Envelope xmlns:s="http://www.w3.org/2003/05/soap-envelope" `+
		`xmlns:trt="http://www.onvif.org/ver10/media/wsdl" `+
		`xmlns:tt="http://www.onvif.org/ver10/schema">`+
		`<s:Body><trt:SetVideoEncoderConfiguration>`+
		`<trt:Configuration token="%s">`+
		`<tt:Encoding>%s</tt:Encoding>`+
		`<tt:Resolution><tt:Width>%d</tt:Width><tt:Height>%d</tt:Height></tt:Resolution>`+
		`<tt:RateControl><tt:FrameRateLimit>%d</tt:FrameRateLimit>`+
		`<tt:EncodingInterval>1</tt:EncodingInterval><tt:BitrateLimit>%d</tt:BitrateLimit></tt:RateControl>`+
		`<tt:H264><tt:GovLength>%d</tt:GovLength><tt:H264Profile>High</tt:H264Profile></tt:H264>`+
		`</trt:Configuration><trt:ForcePersistence>true</trt:ForcePersistence>`+
		`</trt:SetVideoEncoderConfiguration></s:Body></s:Envelope>`,
		token, encoding, w, h, fps, bitrate, gop)
}

// TestSetVideoEncoderConfiguration_AppliesParamsToStream is the core write-path
// test: a valid Set for the main encoder must reach camera-daemon as
// ReconfigureEncoder("main", …) with the ONVIF fields mapped, and return an
// empty success response.
func TestSetVideoEncoderConfiguration_AppliesParamsToStream(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	fake := &fakeReconfigurer{}
	setReconfigurer(t, fake)

	// Act — NVR drops main to 1280×720, 15 fps, 2048 kbps, GOP 60.
	body := setEncoderConfigBody("main_encoder", "H264", 1280, 720, 15, 2048, 60)
	status, respBody := postMediaSOAP(t, srv, body)

	// Assert — success response + the setter received the mapped params.
	if status != http.StatusOK {
		t.Fatalf("status = %d, want 200; body=%s", status, respBody)
	}
	if !strings.Contains(respBody, "SetVideoEncoderConfigurationResponse") {
		t.Errorf("missing success response element; body=%s", respBody)
	}
	if !fake.called {
		t.Fatal("ReconfigureEncoder was not called")
	}
	if fake.gotStream != "main" {
		t.Errorf("stream = %q, want main (stripped from main_encoder)", fake.gotStream)
	}
	if fake.gotParams.Width != 1280 || fake.gotParams.Height != 720 {
		t.Errorf("resolution = %dx%d, want 1280x720", fake.gotParams.Width, fake.gotParams.Height)
	}
	if fake.gotParams.Codec != "H264" {
		t.Errorf("codec = %q, want H264 (lower-cased only inside the client)", fake.gotParams.Codec)
	}
	if fake.gotParams.Fps != 15 {
		t.Errorf("fps = %d, want 15", fake.gotParams.Fps)
	}
	if fake.gotParams.BitrateKbps != 2048 {
		t.Errorf("bitrate = %d kbps, want 2048", fake.gotParams.BitrateKbps)
	}
	if fake.gotParams.Gop != 60 {
		t.Errorf("gop = %d, want 60", fake.gotParams.Gop)
	}
}

// TestSetVideoEncoderConfiguration_FaultsWhenNoCameraDaemon asserts the NVR is
// told (via a SOAP fault) that the change could not be applied when
// camera-daemon is unavailable — never a silent success.
func TestSetVideoEncoderConfiguration_FaultsWhenNoCameraDaemon(t *testing.T) {
	// Arrange — no reconfigurer wired (initLiveStreams never ran).
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	setReconfigurer(t, nil)

	// Act
	status, respBody := postMediaSOAP(t, srv, setEncoderConfigBody("main_encoder", "H264", 1920, 1080, 30, 4096, 30))

	// Assert — fault, not 200, with a clear reason.
	if status == http.StatusOK {
		t.Errorf("status = 200, want 5xx fault when camera-daemon is down; body=%s", respBody)
	}
	if !strings.Contains(respBody, "Fault") {
		t.Errorf("expected a SOAP Fault; body=%s", respBody)
	}
	if !strings.Contains(respBody, "camera-daemon not available") {
		t.Errorf("fault reason missing camera-daemon note; body=%s", respBody)
	}
}

// TestSetVideoEncoderConfiguration_FaultsOnSetterFailure asserts a
// camera-daemon rejection (e.g. unsupported resolution) surfaces as a fault
// carrying the real reason, rather than success.
func TestSetVideoEncoderConfiguration_FaultsOnSetterFailure(t *testing.T) {
	// Arrange
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	setReconfigurer(t, &fakeReconfigurer{err: fmt.Errorf("unsupported resolution 9999x9999")})

	// Act
	status, respBody := postMediaSOAP(t, srv, setEncoderConfigBody("main_encoder", "H264", 9999, 9999, 30, 4096, 30))

	// Assert
	if status == http.StatusOK {
		t.Errorf("status = 200, want 5xx fault on setter failure; body=%s", respBody)
	}
	if !strings.Contains(respBody, "unsupported resolution 9999x9999") {
		t.Errorf("fault detail missing camera-daemon reason; body=%s", respBody)
	}
}

// TestSetVideoEncoderConfiguration_FaultsOnUnknownToken asserts a Set for a
// non-existent encoder token is rejected before reaching camera-daemon.
func TestSetVideoEncoderConfiguration_FaultsOnUnknownToken(t *testing.T) {
	// Arrange — fake present so a token-bypass bug would let the call through.
	cfg := testConfig()
	srv, err := onvifserver.New(buildServerConfig(cfg, "SN1", "1.0", "192.168.1.50"))
	if err != nil {
		t.Fatalf("onvifserver.New: %v", err)
	}
	fake := &fakeReconfigurer{}
	setReconfigurer(t, fake)

	// Act
	status, respBody := postMediaSOAP(t, srv, setEncoderConfigBody("bogus_encoder", "H264", 1280, 720, 30, 2048, 30))

	// Assert — rejected as unknown, camera-daemon never contacted.
	if status == http.StatusOK {
		t.Errorf("status = 200, want 5xx fault for unknown token; body=%s", respBody)
	}
	if !strings.Contains(respBody, "not found") {
		t.Errorf("fault reason missing not-found note; body=%s", respBody)
	}
	if fake.called {
		t.Error("ReconfigureEncoder must not be called for an unknown token")
	}
}
