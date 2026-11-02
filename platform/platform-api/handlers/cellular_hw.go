package handlers

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"gopkg.in/yaml.v3"

	"aipc/platform/common/constants"
)

// CellularHWProfile is the board/module contract loaded from YAML.
// New platforms ship a new YAML; Go must not invent board topology.
type CellularHWProfile struct {
	Platform string          `yaml:"platform"`
	Module   string          `yaml:"module"`
	USB      CellularUSB     `yaml:"usb"`
	AT       CellularAT      `yaml:"at"`
	Control  CellularControl `yaml:"control"`
	GPIO     CellularGPIO    `yaml:"gpio"`
	Net      CellularNet     `yaml:"net"`
	Dialer   CellularDialer  `yaml:"dialer"`
	Paths    CellularPaths   `yaml:"paths"`
}

// CellularAT lists preferred AT character devices and an optional scan glob.
type CellularAT struct {
	Ports    []string `yaml:"ports"`     // preferred AT ports (ordered)
	PortGlob string   `yaml:"port_glob"` // e.g. /dev/ttyUSB* — empty disables glob scan
}

// CellularControl lists non-AT modem control nodes (QMI/MBIM cdc-wdm, etc.).
type CellularControl struct {
	Globs []string `yaml:"globs"`
}

type CellularUSB struct {
	Devices []CellularUSBDevice `yaml:"devices"`
}

type CellularUSBDevice struct {
	Vendor   string   `yaml:"vendor"`
	Products []string `yaml:"products"` // empty = any product for this vendor
}

type CellularGPIOLine struct {
	Name   string `yaml:"name"`
	Offset *int   `yaml:"offset"`
	Active string `yaml:"active"` // high | low
	Hold   bool   `yaml:"hold"`
}

type CellularGPIO struct {
	Chip                   string           `yaml:"chip"`
	Power                  CellularGPIOLine `yaml:"power"`
	Reset                  CellularGPIOLine `yaml:"reset"`
	ResetDeassertMs        int              `yaml:"reset_deassert_ms"`
	ModuleDetectTimeoutSec int              `yaml:"module_detect_timeout_sec"`
}

type CellularNet struct {
	Ifaces []string `yaml:"ifaces"`
}

// CellularDialer names dialer binaries and the CLI style used to build args.
// Style is an enum (aipc | quectel_cm), not a binary filename.
type CellularDialer struct {
	Binaries []string `yaml:"binaries"`
	Style    string   `yaml:"style"`
}

type CellularPaths struct {
	SysfsUSB   string `yaml:"sysfs_usb"`
	SysfsNet   string `yaml:"sysfs_net"`
	RunDir     string `yaml:"run_dir"`
	UserConfig string `yaml:"user_config"`
}

// DefaultCellularUserConfigPath resolves the Config Controller projection path
// for cellular dial JSON. Prefer device-control.yaml → cellular-hw.yaml
// paths.user_config (same board contract the handler loads); fall back to
// ConfigPath()/cellular.json when the profile is missing. Matches device_info
// registering with an explicit path so Register and the handler share one file.
func DefaultCellularUserConfigPath() string {
	fallback := constants.ConfigPath() + "/cellular.json"
	dcPath := constants.ConfigPath() + "/device-control.yaml"
	data, err := os.ReadFile(dcPath)
	if err != nil {
		return fallback
	}
	var doc struct {
		Capabilities struct {
			Cellular struct {
				HWProfile string `yaml:"hw_profile"`
			} `yaml:"cellular"`
		} `yaml:"capabilities"`
	}
	if err := yaml.Unmarshal(data, &doc); err != nil {
		return fallback
	}
	hwPath := strings.TrimSpace(doc.Capabilities.Cellular.HWProfile)
	if hwPath == "" {
		return fallback
	}
	hw, err := LoadCellularHWProfile(hwPath)
	if err != nil {
		return fallback
	}
	return hw.Paths.UserConfig
}

// LoadCellularHWProfile reads and normalizes a hardware profile YAML.
func LoadCellularHWProfile(path string) (*CellularHWProfile, error) {
	if strings.TrimSpace(path) == "" {
		return nil, fmt.Errorf("cellular hw profile path is empty")
	}
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	var hw CellularHWProfile
	if err := yaml.Unmarshal(data, &hw); err != nil {
		return nil, fmt.Errorf("parse cellular hw profile %s: %w", path, err)
	}
	hw.normalize()
	if err := hw.validate(); err != nil {
		return nil, err
	}
	return &hw, nil
}

func (hw *CellularHWProfile) normalize() {
	// Timing defaults only — not board topology.
	if hw.GPIO.ResetDeassertMs <= 0 {
		hw.GPIO.ResetDeassertMs = 50
	}
	if hw.GPIO.ModuleDetectTimeoutSec <= 0 {
		hw.GPIO.ModuleDetectTimeoutSec = 25
	}

	style := strings.ToLower(strings.TrimSpace(hw.Dialer.Style))
	if style == "quectel-cm" {
		style = "quectel_cm"
	}
	hw.Dialer.Style = style

	hw.AT.PortGlob = strings.TrimSpace(hw.AT.PortGlob)
	hw.GPIO.Chip = strings.TrimSpace(hw.GPIO.Chip)
	hw.GPIO.Power.Active = strings.ToLower(strings.TrimSpace(hw.GPIO.Power.Active))
	hw.GPIO.Reset.Active = strings.ToLower(strings.TrimSpace(hw.GPIO.Reset.Active))
	hw.Paths.SysfsUSB = strings.TrimSpace(hw.Paths.SysfsUSB)
	hw.Paths.SysfsNet = strings.TrimSpace(hw.Paths.SysfsNet)
	hw.Paths.RunDir = strings.TrimSpace(hw.Paths.RunDir)
	hw.Paths.UserConfig = strings.TrimSpace(hw.Paths.UserConfig)

	for i := range hw.USB.Devices {
		hw.USB.Devices[i].Vendor = strings.ToLower(strings.TrimSpace(hw.USB.Devices[i].Vendor))
		for j := range hw.USB.Devices[i].Products {
			hw.USB.Devices[i].Products[j] = strings.ToLower(strings.TrimSpace(hw.USB.Devices[i].Products[j]))
		}
	}
	for i := range hw.AT.Ports {
		hw.AT.Ports[i] = strings.TrimSpace(hw.AT.Ports[i])
	}
	hw.Control.Globs = trimNonEmpty(hw.Control.Globs)
	hw.Net.Ifaces = trimNonEmpty(hw.Net.Ifaces)
	hw.Dialer.Binaries = trimNonEmpty(hw.Dialer.Binaries)
}

func trimNonEmpty(in []string) []string {
	out := in[:0]
	for _, s := range in {
		if s = strings.TrimSpace(s); s != "" {
			out = append(out, s)
		}
	}
	return out
}

func (hw *CellularHWProfile) validate() error {
	if len(hw.USB.Devices) == 0 {
		return fmt.Errorf("usb.devices must list at least one vendor/product rule")
	}
	for _, d := range hw.USB.Devices {
		if d.Vendor == "" {
			return fmt.Errorf("usb.devices[].vendor is required")
		}
	}
	if hw.Paths.SysfsUSB == "" || hw.Paths.SysfsNet == "" || hw.Paths.RunDir == "" || hw.Paths.UserConfig == "" {
		return fmt.Errorf("paths.sysfs_usb, paths.sysfs_net, paths.run_dir, and paths.user_config are required")
	}
	if len(hw.Net.Ifaces) == 0 {
		return fmt.Errorf("net.ifaces must list at least one interface name")
	}
	if len(hw.Dialer.Binaries) == 0 {
		return fmt.Errorf("dialer.binaries must list at least one dialer binary")
	}
	switch hw.Dialer.Style {
	case "aipc", "quectel_cm":
	default:
		return fmt.Errorf("dialer.style must be aipc or quectel_cm, got %q", hw.Dialer.Style)
	}
	if hw.GPIO.Power.hasLine() || hw.GPIO.Reset.hasLine() {
		needsChip := (hw.GPIO.Power.Offset != nil && strings.TrimSpace(hw.GPIO.Power.Name) == "") ||
			(hw.GPIO.Reset.Offset != nil && strings.TrimSpace(hw.GPIO.Reset.Name) == "")
		if needsChip && hw.GPIO.Chip == "" {
			return fmt.Errorf("gpio.chip is required when a line uses offset without name")
		}
	}
	for _, item := range []struct {
		name string
		line CellularGPIOLine
	}{
		{"gpio.power", hw.GPIO.Power},
		{"gpio.reset", hw.GPIO.Reset},
	} {
		if !item.line.hasLine() {
			continue
		}
		if err := item.line.validateActive(item.name); err != nil {
			return err
		}
	}
	return nil
}

// dialerBasenames returns process names used with pgrep/pkill -x.
func (hw *CellularHWProfile) dialerBasenames() []string {
	if hw == nil {
		return nil
	}
	var out []string
	seen := map[string]bool{}
	for _, b := range hw.Dialer.Binaries {
		base := filepath.Base(b)
		if base == "" || seen[base] {
			continue
		}
		seen[base] = true
		out = append(out, base)
	}
	return out
}

func (line CellularGPIOLine) validateActive(field string) error {
	switch line.Active {
	case "high", "low":
		return nil
	case "":
		return fmt.Errorf("%s: active must be high or low", field)
	default:
		return fmt.Errorf("%s: active must be high or low, got %q", field, line.Active)
	}
}

// driveValue maps logical on/off to the physical 0/1 for this line.
// Active polarity comes from the board profile (see validateActive).
func (line CellularGPIOLine) driveValue(on bool) int {
	activeHigh := line.Active == "high"
	if on == activeHigh {
		return 1
	}
	return 0
}

func (line CellularGPIOLine) hasLine() bool {
	return strings.TrimSpace(line.Name) != "" || line.Offset != nil
}
