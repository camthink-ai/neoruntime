package identity

import (
	"net"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/google/uuid"
)

// writeVersion writes a temp VERSION-style file and returns its path.
func writeVersion(t *testing.T, lines ...string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "VERSION")
	if err := os.WriteFile(path, []byte(strings.Join(lines, "\n")), 0600); err != nil {
		t.Fatalf("write version file: %v", err)
	}
	return path
}

func TestResolveSN_OverrideWins(t *testing.T) {
	// Arrange / Act
	got := ResolveSN("CT503-OVERRIDE", "/no/such/file")

	// Assert
	if got != "CT503-OVERRIDE" {
		t.Errorf("ResolveSN with override = %q, want CT503-OVERRIDE", got)
	}
}

func TestReadVersionField_SerialAndVersion(t *testing.T) {
	// Arrange — readVersionField is the deterministic parse path.
	path := writeVersion(t, "version=1.2.3", "serial=CT503-0001", "other=ignored")

	// Act / Assert
	if got := readVersionField(path, "serial="); got != "CT503-0001" {
		t.Errorf("serial = %q, want CT503-0001", got)
	}
	if got := readVersionField(path, "version="); got != "1.2.3" {
		t.Errorf("version = %q, want 1.2.3", got)
	}
	if got := readVersionField(path, "missing="); got != "" {
		t.Errorf("missing field = %q, want empty", got)
	}
}

func TestReadFirmwareVersion_FromFile(t *testing.T) {
	// Arrange
	path := writeVersion(t, "version=9.9.9")

	// Act
	got := ReadFirmwareVersion(path)

	// Assert
	if got != "9.9.9" {
		t.Errorf("ReadFirmwareVersion = %q, want 9.9.9", got)
	}
}

func TestReadFirmwareVersion_MissingFileFallsBackToUnknown(t *testing.T) {
	// Act
	got := ReadFirmwareVersion(filepath.Join(t.TempDir(), "missing"))

	// Assert
	if got != "unknown" {
		t.Errorf("ReadFirmwareVersion(missing) = %q, want unknown", got)
	}
}

func TestDeviceUUID_DeterministicAndValid(t *testing.T) {
	// Arrange
	const sn = "CT503-UNIT-42"

	// Act
	a := DeviceUUID(sn)
	b := DeviceUUID(sn)
	c := DeviceUUID("CT503-UNIT-43")

	// Assert — same serial yields a stable, parseable UUIDv5.
	if a != b {
		t.Errorf("DeviceUUID not deterministic: %q != %q", a, b)
	}
	if a == c {
		t.Errorf("DeviceUUID collision across different serials")
	}
	if _, err := uuid.Parse(a); err != nil {
		t.Errorf("DeviceUUID(%q) = %q is not a valid UUID: %v", sn, a, err)
	}
}

func TestResolveLanIP_BogusInterfaceErrors(t *testing.T) {
	// Act
	ip, err := ResolveLanIP("definitely-not-a-real-iface-0")

	// Assert
	if err == nil {
		t.Fatalf("ResolveLanIP(bogus) = %q, want error", ip)
	}
	if ip != "" {
		t.Errorf("ResolveLanIP(bogus) ip = %q, want empty on error", ip)
	}
}

// TestResolveLanIP_RealInterface exercises the success path (firstGlobalIPv4)
// using whatever real interface the test host has. Skipped on hosts with none.
func TestResolveLanIP_RealInterface(t *testing.T) {
	// Arrange — find a real up, non-loopback interface with an IPv4 address.
	name := findRealIPv4Interface(t)
	if name == "" {
		t.Skip("no up non-loopback interface with IPv4 on this host")
	}

	// Act
	ip, err := ResolveLanIP(name)

	// Assert
	if err != nil {
		t.Fatalf("ResolveLanIP(%s): %v", name, err)
	}
	if net.ParseIP(ip).To4() == nil {
		t.Errorf("ResolveLanIP(%s) = %q, not a valid IPv4", name, ip)
	}
}

// findRealIPv4Interface returns the name of a usable interface or "".
func findRealIPv4Interface(t *testing.T) string {
	t.Helper()
	ifaces, err := net.Interfaces()
	if err != nil {
		return ""
	}
	for _, iface := range ifaces {
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		if hasGlobalIPv4(iface) {
			return iface.Name
		}
	}
	return ""
}

func hasGlobalIPv4(iface net.Interface) bool {
	addrs, err := iface.Addrs()
	if err != nil {
		return false
	}
	for _, addr := range addrs {
		ipnet, ok := addr.(*net.IPNet)
		if !ok {
			continue
		}
		if ip := ipnet.IP.To4(); ip != nil && !ip.IsLoopback() && !ip.IsLinkLocalUnicast() {
			return true
		}
	}
	return false
}
