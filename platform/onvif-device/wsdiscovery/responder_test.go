package wsdiscovery

import (
	"errors"
	"net"
	"strings"
	"testing"
)

func TestNew_AppliesDefaults(t *testing.T) {
	// Arrange / Act
	r := New(Config{XAddrs: []string{"http://1.2.3.4:8081/onvif/device_service"}})

	// Assert
	if r.cfg.MulticastAddr != "239.255.255.250" {
		t.Errorf("MulticastAddr = %q, want 239.255.255.250", r.cfg.MulticastAddr)
	}
	if r.cfg.MulticastPort != 3702 {
		t.Errorf("MulticastPort = %d, want 3702", r.cfg.MulticastPort)
	}
	if r.cfg.Types != "dp0:NetworkVideoTransmitter" {
		t.Errorf("Types = %q, want NetworkVideoTransmitter", r.cfg.Types)
	}
}

func TestSetXAddrs_UpdatesAndCopies(t *testing.T) {
	// Arrange
	r := New(Config{})
	src := []string{"http://a/device_service"}

	// Act
	r.SetXAddrs(src)
	src[0] = "http://mutated/device_service" // mutate caller slice after SetXAddrs

	// Assert — responder holds a defensive copy, not an alias.
	got := r.snapshotXAddrs()
	if len(got) != 1 || got[0] != "http://a/device_service" {
		t.Errorf("snapshotXAddrs = %v, want defensive copy", got)
	}
}

func TestBodyElement_RendersAllFields(t *testing.T) {
	// Act
	out := bodyElement("d:Hello", "urn:uuid:EP", "dp0:NetworkVideoTransmitter",
		[]string{"scope1", "scope2"}, []string{"http://x/dev"})

	// Assert
	for _, want := range []string{"<d:Hello>", "urn:uuid:EP", "dp0:NetworkVideoTransmitter", "scope1 scope2", "http://x/dev", "<d:MetadataVersion>1</d:MetadataVersion>"} {
		if !strings.Contains(out, want) {
			t.Errorf("bodyElement missing %q in:\n%s", want, out)
		}
	}
}

func TestBuildMessage_Hello(t *testing.T) {
	// Arrange
	r := New(Config{EndpointUUID: "urn:uuid:HELLO-EP", XAddrs: []string{"http://h/dev"}})

	// Act
	msg := string(r.buildMessage(actionHello, "", r.helloBody()))

	// Assert
	for _, want := range []string{actionHello, "<a:MessageID>urn:uuid:", discoveryTo, "<d:Hello>", "urn:uuid:HELLO-EP", "http://h/dev"} {
		if !strings.Contains(msg, want) {
			t.Errorf("Hello message missing %q", want)
		}
	}
	if strings.Contains(msg, "<a:RelatesTo>") {
		t.Errorf("Hello should not contain RelatesTo")
	}
}

func TestBuildMessage_ProbeMatch_HasRelatesToAndAnonymousTo(t *testing.T) {
	// Arrange
	r := New(Config{EndpointUUID: "urn:uuid:PM-EP", XAddrs: []string{"http://pm/dev"}, Scopes: []string{"onvif://x"}})

	// Act
	msg := string(r.buildMessage(actionProbeMatches, "urn:uuid:probe-123", r.probeMatchesBody()))

	// Assert
	for _, want := range []string{actionProbeMatches, anonymousAddress, "<a:RelatesTo>urn:uuid:probe-123</a:RelatesTo>", "<d:ProbeMatches>", "<d:ProbeMatch>", "onvif://x"} {
		if !strings.Contains(msg, want) {
			t.Errorf("ProbeMatch missing %q in:\n%s", want, msg)
		}
	}
}

func TestBuildMessage_EmptyRelatesToOmitted(t *testing.T) {
	// Arrange
	r := New(Config{})

	// Act
	msg := string(r.buildMessage(actionProbeMatches, "", r.probeMatchesBody()))

	// Assert
	if strings.Contains(msg, "<a:RelatesTo>") {
		t.Errorf("empty relatesTo should omit RelatesTo:\n%s", msg)
	}
}

func TestIsProbe(t *testing.T) {
	// Arrange
	cases := []struct {
		name string
		body string
		want bool
	}{
		{"probe", `<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/Probe</a:Action>`, true},
		{"probematches", `<a:Action>http://schemas.xmlsoap.org/ws/2005/04/discovery/ProbeMatches</a:Action>`, false},
		{"garbage", "not xml at all", false},
		{"empty", "", false},
	}
	for _, tc := range cases {
		// Act
		got := isProbe([]byte(tc.body))
		// Assert
		if got != tc.want {
			t.Errorf("isProbe(%q) = %v, want %v", tc.name, got, tc.want)
		}
	}
}

func TestExtractMessageID(t *testing.T) {
	// Arrange
	cases := []struct {
		name string
		body string
		want string
	}{
		{"a-prefix", `<a:MessageID>urn:uuid:abc-1</a:MessageID>`, "urn:uuid:abc-1"},
		{"wsdl-prefix", `<wsa:MessageID>xyz</wsa:MessageID>`, "xyz"},
		{"absent", `<s:Header><a:Action>x</a:Action></s:Header>`, ""},
		{"empty-body", "", ""},
	}
	for _, tc := range cases {
		// Act
		got := extractMessageID([]byte(tc.body))
		// Assert
		if got != tc.want {
			t.Errorf("extractMessageID(%q) = %q, want %q", tc.name, got, tc.want)
		}
	}
}

func TestByeBody_RendersByeWrapper(t *testing.T) {
	// Arrange
	r := New(Config{EndpointUUID: "urn:uuid:BYE-EP", XAddrs: []string{"http://b/dev"}})

	// Act
	out := r.byeBody()

	// Assert
	for _, want := range []string{"<d:Bye>", "urn:uuid:BYE-EP", "http://b/dev", "</d:Bye>"} {
		if !strings.Contains(out, want) {
			t.Errorf("byeBody missing %q in:\n%s", want, out)
		}
	}
	if strings.Contains(out, "<d:Hello>") || strings.Contains(out, "<d:ProbeMatches>") {
		t.Errorf("byeBody should use Bye wrapper only:\n%s", out)
	}
}

func TestIfaceName(t *testing.T) {
	// Arrange / Act / Assert
	if got := ifaceName(nil); got != "default" {
		t.Errorf("ifaceName(nil) = %q, want default", got)
	}
	if got := ifaceName(&net.Interface{Name: "eth0"}); got != "eth0" {
		t.Errorf("ifaceName(eth0) = %q, want eth0", got)
	}
}

type timeoutErr struct{}

func (timeoutErr) Error() string   { return "i/o timeout" }
func (timeoutErr) Timeout() bool   { return true }
func (timeoutErr) Temporary() bool { return false }

func TestIsTimeout(t *testing.T) {
	// Arrange / Act / Assert — a net.Error with Timeout()==true is a timeout.
	if !isTimeout(timeoutErr{}) {
		t.Error("isTimeout(net.Error timeout) = false, want true")
	}
	if isTimeout(errors.New("not a timeout")) {
		t.Error("isTimeout(plain error) = true, want false")
	}
}

func TestActionTo(t *testing.T) {
	// Arrange / Act / Assert
	if got := actionTo(actionProbeMatches); got != anonymousAddress {
		t.Errorf("actionTo(ProbeMatches) = %q, want %q", got, anonymousAddress)
	}
	if got := actionTo(actionHello); got != discoveryTo {
		t.Errorf("actionTo(Hello) = %q, want %q", got, discoveryTo)
	}
}
