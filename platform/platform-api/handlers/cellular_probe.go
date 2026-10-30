package handlers

import (
	"net"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const cellularModuleSticky = 20 * time.Second

// resolveModulePresence is sysfs-only. When allowSticky, remembers/reuses the
// last USB product across brief re-enumeration.
func (h *CellularHandler) resolveModulePresence(allowSticky bool) (present, live bool, name string) {
	h.maybePromoteCapabilityFromUSB()
	if vid, _, path := h.findMatchedUSB(); vid != "" {
		name = readUSBProduct(path)
		if allowSticky {
			h.noteModuleSeen(name)
		}
		return true, true, name
	}
	if allowSticky && h.moduleSeenRecently(cellularModuleSticky) {
		return true, false, h.stickyModuleName()
	}
	return false, false, ""
}

func (h *CellularHandler) capabilityView() CellularCapabilityView {
	if h.hw == nil {
		return CellularCapabilityView{Capability: cellularCapUnsupported}
	}
	cfg, _ := h.loadConfig()
	present, _, _ := h.resolveModulePresence(cfg.Enabled)
	cap := h.capabilityState()
	if cap == cellularCapUnsupported {
		return CellularCapabilityView{Capability: cellularCapUnsupported}
	}
	return CellularCapabilityView{
		Capability:    cap,
		ModulePresent: present,
	}
}

func (h *CellularHandler) probeStatus(cfg CellularConfig) CellularStatus {
	st := CellularStatus{RegState: "unknown"}
	if h.hw != nil {
		st.Platform = h.hw.Platform
	}

	present, liveUSB, module := h.resolveModulePresence(cfg.Enabled)
	cap := h.capabilityState()
	st.Capability = cap
	st.Supported = cap == cellularCapSupported
	st.ModulePresent = present
	st.Module = module

	if h.hw == nil || cap == cellularCapUnsupported {
		if h.hw == nil {
			st.Message = "Cellular hardware profile is not configured."
		} else {
			st.Message = "No cellular hardware detected on this device."
		}
		st.Capability = cellularCapUnsupported
		st.Supported = false
		st.ModulePresent = false
		st.Module = ""
		return st
	}

	if cap == cellularCapPending {
		st.Message = "Detecting cellular hardware…"
	}

	if h.dialerAvailable(cfg) {
		st.DialerReady = true
	}

	if !cfg.Enabled {
		h.clearModuleSeen()
		h.clearStickyIdentity()
		st.Connected = false
		st.Iface = ""
		st.IPv4 = ""
		st.IPv6 = ""
		st.SIMReady = false
		st.ModulePresent = false
		st.Module = ""
		st.Operator = ""
		st.Network = ""
		st.IMEI = ""
		st.ICCID = ""
		st.IMSI = ""
		st.MSISDN = ""
		st.Firmware = ""
		st.SignalRSRP = nil
		st.SignalRSRQ = nil
		st.Message = "Cellular is disabled."
		return st
	}

	iface, v4, v6 := h.findNetIface()
	hasControl := h.hasModemControl()
	if liveUSB && hasControl && iface != "" && (v4 != "" || v6 != "") {
		st.Iface = iface
		st.IPv4 = v4
		st.IPv6 = v6
		st.Connected = true
		st.RegState = "registered"
	}

	switch {
	case !st.ModulePresent:
		st.Message = "Power enabled but module not on USB. Check drivers / GPIO / insertion."
	case !liveUSB:
		st.Message = "Module re-enumerating on USB…"
	case !hasControl:
		st.Message = "Module on USB but no AT/QMI/MBIM control device."
	case !st.DialerReady:
		st.Message = "Module detected. Dialer binary from hardware profile is not installed yet."
	case !st.Connected:
		if h.dialerRunning() {
			st.Message = "Dialer running; waiting for data session."
		} else {
			st.Message = "Module ready; auto-dial will start shortly."
		}
	default:
		st.Message = "Cellular data session is up."
	}

	h.enrichFromAT(&st)

	if st.ModulePresent && !st.Connected && !st.SIMReady {
		h.mu.Lock()
		knownAbsent := h.simStateKnown && !h.lastSIMReady
		h.mu.Unlock()
		if knownAbsent {
			st.Message = "No SIM detected. Insert a SIM to dial."
		} else {
			st.Message = "SIM not ready; modem is re-reading the card…"
		}
	}

	return st
}

func (h *CellularHandler) noteModuleSeen(moduleName string) {
	h.moduleSeenMu.Lock()
	h.moduleSeenAt = h.now()
	if moduleName != "" {
		h.lastModuleName = moduleName
	}
	h.moduleSeenMu.Unlock()
}

func (h *CellularHandler) clearModuleSeen() {
	h.moduleSeenMu.Lock()
	h.moduleSeenAt = time.Time{}
	h.lastModuleName = ""
	h.moduleSeenMu.Unlock()
}

func (h *CellularHandler) stickyModuleName() string {
	h.moduleSeenMu.Lock()
	defer h.moduleSeenMu.Unlock()
	return h.lastModuleName
}

func readUSBProduct(devPath string) string {
	if devPath == "" {
		return ""
	}
	return readFileTrim(filepath.Join(devPath, "product"))
}

func (h *CellularHandler) moduleSeenRecently(d time.Duration) bool {
	h.moduleSeenMu.Lock()
	at := h.moduleSeenAt
	h.moduleSeenMu.Unlock()
	if at.IsZero() {
		return false
	}
	return h.now().Sub(at) <= d
}

func (h *CellularHandler) findMatchedUSB() (vid, pid, path string) {
	if h.hw == nil {
		return "", "", ""
	}
	entries, err := os.ReadDir(h.sysfsUSB)
	if err != nil {
		return "", "", ""
	}

	type hit struct{ vid, pid, path string }
	var vendorOnly *hit

	for _, e := range entries {
		dir := filepath.Join(h.sysfsUSB, e.Name())
		v := strings.ToLower(readFileTrim(filepath.Join(dir, "idVendor")))
		p := strings.ToLower(readFileTrim(filepath.Join(dir, "idProduct")))
		if v == "" || p == "" {
			continue
		}
		for _, rule := range h.hw.USB.Devices {
			if !strings.EqualFold(rule.Vendor, v) {
				continue
			}
			if len(rule.Products) == 0 {
				if vendorOnly == nil {
					vendorOnly = &hit{v, p, dir}
				}
				continue
			}
			for _, want := range rule.Products {
				if want == p {
					return v, p, dir
				}
			}
		}
	}
	if vendorOnly != nil {
		return vendorOnly.vid, vendorOnly.pid, vendorOnly.path
	}
	return "", "", ""
}

func (h *CellularHandler) findNetIface() (name, ipv4, ipv6 string) {
	if h.hw == nil {
		return "", "", ""
	}
	for _, n := range h.hw.Net.Ifaces {
		if _, err := os.Stat(filepath.Join(h.sysfsNet, n)); err != nil {
			continue
		}
		ifi, err := net.InterfaceByName(n)
		if err != nil {
			continue
		}
		addrs, err := ifi.Addrs()
		if err != nil {
			continue
		}
		var v4, v6 string
		for _, a := range addrs {
			ipNet, ok := a.(*net.IPNet)
			if !ok || ipNet.IP.IsLoopback() {
				continue
			}
			if ipNet.IP.IsLinkLocalUnicast() || ipNet.IP.IsLinkLocalMulticast() {
				continue
			}
			if ip4 := ipNet.IP.To4(); ip4 != nil {
				if v4 == "" {
					v4 = ip4.String()
				}
			} else if ipNet.IP.To16() != nil {
				if v6 == "" {
					v6 = ipNet.IP.String()
				}
			}
		}
		if v4 != "" || v6 != "" || name == "" {
			name, ipv4, ipv6 = n, v4, v6
			if v4 != "" || v6 != "" {
				return name, ipv4, ipv6
			}
		}
	}
	return name, ipv4, ipv6
}

func readFileTrim(path string) string {
	b, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}
