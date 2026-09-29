// Package identity resolves NE503 device identity for ONVIF advertisement.
//
// It portably mirrors the resolution helpers in device-discovery/server/main.go
// (resolveSN / readFirmwareVersion) so the ONVIF service and the CT-Disc
// service advertise the same serial number and firmware version. The intent
// is to eventually hoist these into platform/common/identity; for now they
// are kept local to avoid touching the proven device-discovery entry point.
package identity

import (
	"context"
	"fmt"
	"log"
	"net"
	"os"
	"strings"

	"github.com/google/uuid"

	"aipc/platform/common/factoryeeprom"
)

// ResolveSN returns the device serial number, trying in order:
//  1. an explicit override (config or flag),
//  2. the factory EEPROM,
//  3. the "serial=" line of the VERSION file,
//  4. the system hostname.
func ResolveSN(override, versionFile string) string {
	if v := strings.TrimSpace(override); v != "" {
		return v
	}
	if info, err := factoryeeprom.DefaultClient().Read(context.Background()); err == nil {
		if serial := strings.TrimSpace(info.SerialNumber); serial != "" {
			return serial
		}
	}
	if v := readVersionField(versionFile, "serial="); v != "" {
		return v
	}
	if hostname, err := os.Hostname(); err == nil && hostname != "" {
		return hostname
	}
	return "unknown"
}

// ReadFirmwareVersion returns the firmware version from the VERSION file
// ("version=" line), falling back to "unknown".
func ReadFirmwareVersion(versionFile string) string {
	if v := readVersionField(versionFile, "version="); v != "" {
		return v
	}
	return "unknown"
}

// DeviceUUID returns a stable, deterministic UUID for the ONVIF
// EndpointReference. It is derived (UUIDv5) from the serial number so the
// same hardware always advertises the same endpoint across reboots, which
// NVRs rely on to deduplicate devices.
func DeviceUUID(serialNumber string) string {
	return uuid.NewSHA1(uuid.NameSpaceDNS, []byte(serialNumber)).String()
}

// ResolveLanIP returns the first global (non-loopback, non-link-local) IPv4
// address on the named interface. If ifaceName is empty the first suitable
// up non-loopback interface is used.
func ResolveLanIP(ifaceName string) (string, error) {
	ifaces, err := net.Interfaces()
	if err != nil {
		return "", fmt.Errorf("list interfaces: %w", err)
	}

	for _, iface := range ifaces {
		if ifaceName != "" && iface.Name != ifaceName {
			continue
		}
		if ifaceName == "" && (iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagLoopback != 0) {
			continue
		}

		ip, ok := firstGlobalIPv4(&iface)
		if ok {
			return ip, nil
		}
	}

	if ifaceName != "" {
		return "", fmt.Errorf("no IPv4 address on interface %s", ifaceName)
	}
	return "", fmt.Errorf("no interface with a global IPv4 address")
}

// firstGlobalIPv4 returns the first non-loopback, non-link-local IPv4 of the
// interface.
func firstGlobalIPv4(iface *net.Interface) (string, bool) {
	addrs, err := iface.Addrs()
	if err != nil {
		return "", false
	}
	for _, addr := range addrs {
		ipnet, ok := addr.(*net.IPNet)
		if !ok {
			continue
		}
		ip := ipnet.IP.To4()
		if ip == nil || ip.IsLoopback() || ip.IsLinkLocalUnicast() {
			continue
		}
		return ip.String(), true
	}
	return "", false
}

// DisableRpFilter sets rp_filter=0 on the given interface and persists it to
// /etc/sysctl.d so multicast from a different subnet is not dropped by the
// kernel's reverse-path filter. Mirrors device-discovery's behaviour; safe to
// fail (logged) on hosts without the sysctl or without root.
func DisableRpFilter(ifaceName string) {
	if ifaceName == "" {
		ifaceName = "eth0"
	}
	path := fmt.Sprintf("/proc/sys/net/ipv4/conf/%s/rp_filter", ifaceName)
	if err := os.WriteFile(path, []byte("0"), 0644); err != nil {
		log.Printf("[onvif] WARNING: failed to set rp_filter=0 on %s: %v", ifaceName, err)
		return
	}
	persist := fmt.Sprintf("net.ipv4.conf.%s.rp_filter=0\n", ifaceName)
	_ = os.WriteFile("/etc/sysctl.d/99-onvif-rp_filter.conf", []byte(persist), 0644)
}

// readVersionField scans the VERSION file for a "<prefix><value>" line and
// returns the trimmed value.
func readVersionField(versionFile, prefix string) string {
	data, err := os.ReadFile(versionFile)
	if err != nil {
		return ""
	}
	for line := range strings.SplitSeq(string(data), "\n") {
		if value, ok := strings.CutPrefix(line, prefix); ok {
			return strings.TrimSpace(value)
		}
	}
	return ""
}
