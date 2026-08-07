// Package network is the Config Controller adapter for the network domain.
// It owns the per-interface systemd-networkd config
// (/etc/systemd/network/10-<iface>.network) and the shared ifupdown fallback
// (/etc/network/interfaces), projecting a desired NetworkConfig onto both.
//
// The adapter preserves the existing UpdateConfig HTTP contract: the desired
// value is a NetworkConfig JSON, mode is dhcp|static, and static mode requires
// valid ip_address + subnet_mask (+ optional gateway/DNS). Apply writes both
// files atomically. Verify is a file-readback (NOT a live `networkctl reload`):
// on 93.72 the management interface IS eth0, so a synchronous reload inside
// Verify could drop the HTTP connection mid-request and prevent the handler's
// async bounce from ever firing (and thus its rollback). The actual network
// reconfiguration + rollback stays in the handler's async post-Apply path,
// unchanged from today. The Manager records the revision + audit; the adapter
// only owns file projection.
//
// /etc/network/interfaces is shared across all interfaces. The Manager holds a
// single per-domain mutex for "network", so concurrent applies for different
// interfaces serialize — the shared file is never written by two applies at
// once.
package network

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"os"
	"path/filepath"
	"strings"

	"aipc/platform/platform-api/internal/atomicfile"
)

// config is the desired-state shape for one interface. It mirrors
// handlers.NetworkConfig field-for-field so the handler can marshal its
// existing request struct and hand the JSON to the Manager verbatim.
type config struct {
	Interface  string `json:"interface"`
	Mode       string `json:"mode"` // "dhcp" or "static"
	IPAddress  string `json:"ip_address"`
	SubnetMask string `json:"subnet_mask"`
	Gateway    string `json:"gateway"`
	DNS1       string `json:"dns1"`
	DNS2       string `json:"dns2"`
	MACAddress string `json:"mac_address"`
}

var (
	// ErrInvalidJSON is returned when desiredJSON does not unmarshal.
	ErrInvalidJSON = errors.New("network: invalid desired json")
	// ErrInvalidMode is returned when mode is neither dhcp nor static.
	ErrInvalidMode = errors.New("network: mode must be dhcp or static")
	// ErrMissingField is returned when a required static field is empty.
	ErrMissingField = errors.New("network: missing required static field")
	// ErrBadIP is returned when an IP/mask/gateway/DNS is not parseable.
	ErrBadIP = errors.New("network: invalid ip format")
)

// backupState captures both projected files so Restore can revert them. A nil
// slice means the file did not exist pre-apply, so Restore removes the file
// Apply created rather than overwriting a prior one.
type backupState struct {
	networkBytes    []byte // /etc/systemd/network/10-<iface>.network
	interfacesBytes []byte // /etc/network/interfaces
}

// rendered is the projection produced by Render: the bytes for both files plus
// the resolved interface name. Carrying the iface avoids re-parsing it in Apply.
type rendered struct {
	networkBytes    []byte
	interfacesBytes []byte
	iface           string
}

// Adapter applies network configuration to the live filesystem. One instance
// is registered with the Config Manager under the "network" domain; the key
// per-apply is the interface name (e.g. "eth0").
type Adapter struct {
	networkDir     string // parent of 10-<iface>.network
	interfacesPath string
}

// New returns a network Adapter. networkDir is the systemd-networkd config
// directory (defaults to /etc/systemd/network); interfacesPath is the shared
// ifupdown file (defaults to /etc/network/interfaces). Both are overridable so
// tests can point them at a temp dir instead of touching the live /etc.
func New(networkDir, interfacesPath string) *Adapter {
	if networkDir == "" {
		networkDir = "/etc/systemd/network"
	}
	if interfacesPath == "" {
		interfacesPath = "/etc/network/interfaces"
	}
	return &Adapter{networkDir: networkDir, interfacesPath: interfacesPath}
}

// configPath returns the systemd-networkd file path for an interface.
func (a *Adapter) configPath(iface string) string {
	if iface == "" {
		iface = "eth0"
	}
	return filepath.Join(a.networkDir, fmt.Sprintf("10-%s.network", iface))
}

// Validate mirrors the field checks in handlers.UpdateConfig. It does not touch
// the filesystem. The interface name in the JSON may be empty; Apply/Render
// fall back to the key (the per-apply interface name) in that case.
func (a *Adapter) Validate(_ context.Context, _, desiredJSON string) error {
	var cfg config
	if err := json.Unmarshal([]byte(desiredJSON), &cfg); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalidJSON, err)
	}
	if cfg.Mode != "dhcp" && cfg.Mode != "static" {
		return ErrInvalidMode
	}
	if cfg.Mode == "static" {
		if cfg.IPAddress == "" || cfg.SubnetMask == "" {
			return fmt.Errorf("%w: ip_address and subnet_mask required for static", ErrMissingField)
		}
		if net.ParseIP(cfg.IPAddress) == nil || net.ParseIP(cfg.SubnetMask) == nil {
			return ErrBadIP
		}
		if cfg.Gateway != "" && net.ParseIP(cfg.Gateway) == nil {
			return ErrBadIP
		}
	}
	if cfg.DNS1 != "" && net.ParseIP(cfg.DNS1) == nil {
		return ErrBadIP
	}
	if cfg.DNS2 != "" && net.ParseIP(cfg.DNS2) == nil {
		return ErrBadIP
	}
	return nil
}

// Backup reads the current .network and interfaces files. Missing files are
// not errors; the corresponding slice stays nil so Restore knows to remove the
// file Apply created.
func (a *Adapter) Backup(_ context.Context, key string) (any, error) {
	nb, err := os.ReadFile(a.configPath(key))
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("backup .network: %w", err)
	}
	ib, err := os.ReadFile(a.interfacesPath)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("backup interfaces: %w", err)
	}
	return backupState{networkBytes: nb, interfacesBytes: ib}, nil
}

// renderNetwork ports handlers.writeNetworkConfig's content builder. It writes
// the systemd-networkd [Match]/[Network] stanza for one interface.
func renderNetwork(cfg *config, iface string) []byte {
	var content strings.Builder
	content.WriteString("# Network configuration managed by AIPC Platform\n")
	content.WriteString("[Match]\n")
	content.WriteString(fmt.Sprintf("Name=%s\n\n", iface))

	content.WriteString("[Network]\n")
	if cfg.Mode == "dhcp" {
		content.WriteString("DHCP=yes\n")
	} else {
		content.WriteString("DHCP=no\n")
		if prefix := maskToPrefix(cfg.SubnetMask); prefix > 0 {
			content.WriteString(fmt.Sprintf("Address=%s/%d\n", cfg.IPAddress, prefix))
		} else {
			content.WriteString(fmt.Sprintf("Address=%s\n", cfg.IPAddress))
		}
		if cfg.Gateway != "" {
			content.WriteString(fmt.Sprintf("Gateway=%s\n", cfg.Gateway))
		}
		if cfg.DNS1 != "" {
			content.WriteString(fmt.Sprintf("DNS=%s\n", cfg.DNS1))
		}
		if cfg.DNS2 != "" {
			content.WriteString(fmt.Sprintf("DNS=%s\n", cfg.DNS2))
		}
	}
	return []byte(content.String())
}

// renderInterfaces ports handlers.writeInterfacesConfig's content builder. It
// rewrites the whole /etc/network/interfaces for the given interface stanza.
func renderInterfaces(cfg *config, iface string) []byte {
	var sb strings.Builder
	sb.WriteString("# Interfaces file managed by AIPC Platform\n")
	sb.WriteString("auto lo\n")
	sb.WriteString("iface lo inet loopback\n\n")
	sb.WriteString(fmt.Sprintf("auto %s\n", iface))
	if cfg.Mode == "dhcp" {
		sb.WriteString(fmt.Sprintf("iface %s inet dhcp\n", iface))
	} else {
		sb.WriteString(fmt.Sprintf("iface %s inet static\n", iface))
		sb.WriteString(fmt.Sprintf("\taddress %s\n", cfg.IPAddress))
		sb.WriteString(fmt.Sprintf("\tnetmask %s\n", cfg.SubnetMask))
		if cfg.Gateway != "" {
			sb.WriteString(fmt.Sprintf("\tgateway %s\n", cfg.Gateway))
		}
		if cfg.DNS1 != "" {
			sb.WriteString(fmt.Sprintf("\tdns-nameservers %s", cfg.DNS1))
			if cfg.DNS2 != "" {
				sb.WriteString(fmt.Sprintf(" %s", cfg.DNS2))
			}
			sb.WriteString("\n")
		}
	}
	return []byte(sb.String())
}

// Render produces the bytes for both projected files. It is a pure projection
// from desiredJSON; reading the current state happens in Backup, not here.
func (a *Adapter) Render(_ context.Context, key, desiredJSON string) (any, error) {
	var cfg config
	if err := json.Unmarshal([]byte(desiredJSON), &cfg); err != nil {
		return nil, fmt.Errorf("%w: %v", ErrInvalidJSON, err)
	}
	iface := cfg.Interface
	if iface == "" {
		iface = key
	}
	return rendered{
		networkBytes:    renderNetwork(&cfg, iface),
		interfacesBytes: renderInterfaces(&cfg, iface),
		iface:           iface,
	}, nil
}

// Apply atomically writes both files. Parent dirs are created if missing (the
// systemd-networkd dir may not exist on Yocto minimal images).
func (a *Adapter) Apply(_ context.Context, _ string, renderedVal any) error {
	r, ok := renderedVal.(rendered)
	if !ok {
		return fmt.Errorf("network: rendered payload is not network.rendered")
	}
	netPath := a.configPath(r.iface)
	if err := os.MkdirAll(filepath.Dir(netPath), 0755); err != nil {
		return fmt.Errorf("mkdir %s: %w", filepath.Dir(netPath), err)
	}
	if err := atomicfile.Write(netPath, r.networkBytes, 0644); err != nil {
		return fmt.Errorf("write .network: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(a.interfacesPath), 0755); err != nil {
		return fmt.Errorf("mkdir %s: %w", filepath.Dir(a.interfacesPath), err)
	}
	if err := atomicfile.Write(a.interfacesPath, r.interfacesBytes, 0644); err != nil {
		return fmt.Errorf("write interfaces: %w", err)
	}
	return nil
}

// Verify is a file-readback: it re-renders the desired .network bytes and
// compares them to what landed on disk. This catches write/parse failures
// without bouncing the live network (see package doc for why a live reload
// here is unsafe on the management interface).
func (a *Adapter) Verify(_ context.Context, key, desiredJSON string) error {
	var cfg config
	if err := json.Unmarshal([]byte(desiredJSON), &cfg); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalidJSON, err)
	}
	iface := cfg.Interface
	if iface == "" {
		iface = key
	}
	got, err := os.ReadFile(a.configPath(iface))
	if err != nil {
		return fmt.Errorf("verify: read .network: %w", err)
	}
	want := renderNetwork(&cfg, iface)
	if string(got) != string(want) {
		return fmt.Errorf("verify: .network content mismatch")
	}
	return nil
}

// Restore writes the pre-apply bytes back (or removes the file if none existed)
// for both the .network file and /etc/network/interfaces.
func (a *Adapter) Restore(_ context.Context, key string, backup any) error {
	bs, ok := backup.(backupState)
	if !ok {
		return fmt.Errorf("network: backup payload is not network.backupState")
	}
	netPath := a.configPath(key)
	if bs.networkBytes != nil {
		if err := atomicfile.Write(netPath, bs.networkBytes, 0644); err != nil {
			return fmt.Errorf("restore .network: %w", err)
		}
	} else {
		_ = os.Remove(netPath)
	}
	if bs.interfacesBytes != nil {
		if err := atomicfile.Write(a.interfacesPath, bs.interfacesBytes, 0644); err != nil {
			return fmt.Errorf("restore interfaces: %w", err)
		}
	} else {
		_ = os.Remove(a.interfacesPath)
	}
	return nil
}

// maskToPrefix converts a dotted-decimal subnet mask to a CIDR prefix length.
// Ported from handlers.NetworkHandler.maskToPrefix so the renderer is self-contained.
func maskToPrefix(mask string) int {
	ip := net.ParseIP(mask)
	if ip == nil {
		return 0
	}
	ones, _ := net.IPMask(ip.To4()).Size()
	return ones
}
