package discover

import (
	"fmt"
	"net"
	"sort"
	"strings"
	"sync"
	"time"
)

// resolveKey returns the primary identity key for an announce.
// MAC is preferred as it's a stable hardware identifier; SN is the fallback.
func resolveKey(a Announce) string {
	if a.MAC != "" {
		return a.MAC
	}
	return a.SN
}

type DeviceInfo struct {
	Announce  Announce
	FirstSeen time.Time
	LastSeen  time.Time
	Online    bool
}

type Registry struct {
	mu      sync.RWMutex
	devices map[string]*DeviceInfo // key: MAC (fallback to SN)
}

func NewRegistry() *Registry {
	return &Registry{
		devices: make(map[string]*DeviceInfo),
	}
}

func (r *Registry) Update(a Announce) *DeviceInfo {
	r.mu.Lock()
	defer r.mu.Unlock()

	key := resolveKey(a)
	now := time.Now()
	if dev, ok := r.devices[key]; ok {
		dev.Announce = a
		dev.LastSeen = now
		dev.Online = true
		return dev
	}

	dev := &DeviceInfo{
		Announce:  a,
		FirstSeen: now,
		LastSeen:  now,
		Online:    true,
	}
	r.devices[key] = dev
	return dev
}

func (r *Registry) List() []*DeviceInfo {
	r.mu.RLock()
	defer r.mu.RUnlock()

	result := make([]*DeviceInfo, 0, len(r.devices))
	for _, dev := range r.devices {
		result = append(result, dev)
	}
	sortDevicesByIP(result)
	return result
}

// GetByMAC looks up a device by its MAC address (the primary map key).
func (r *Registry) GetByMAC(mac string) (*DeviceInfo, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	dev, ok := r.devices[mac]
	return dev, ok
}

// Get looks up a device by serial number (iterates the map).
// Prefer GetByMAC when the MAC is known.
func (r *Registry) Get(sn string) (*DeviceInfo, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()
	for _, dev := range r.devices {
		if dev.Announce.SN == sn {
			return dev, true
		}
	}
	return nil, false
}

func (r *Registry) CheckTimeouts(timeout time.Duration) []string {
	r.mu.Lock()
	defer r.mu.Unlock()

	var expired []string
	now := time.Now()
	for key, dev := range r.devices {
		if dev.Online && now.Sub(dev.LastSeen) > timeout {
			dev.Online = false
			expired = append(expired, key)
		}
	}
	return expired
}

func (r *Registry) Filter(product, sn, mac string) []*DeviceInfo {
	r.mu.RLock()
	defer r.mu.RUnlock()

	result := make([]*DeviceInfo, 0)
	for _, dev := range r.devices {
		if product != "" && dev.Announce.Product != product {
			continue
		}
		if sn != "" && !strings.Contains(dev.Announce.SN, sn) {
			continue
		}
		if mac != "" && !strings.Contains(dev.Announce.MAC, mac) {
			continue
		}
		result = append(result, dev)
	}
	sortDevicesByIP(result)
	return result
}

func sortDevicesByIP(devices []*DeviceInfo) {
	sort.Slice(devices, func(i, j int) bool {
		return compareDevicesByIP(devices[i], devices[j]) < 0
	})
}

func compareDevicesByIP(a, b *DeviceInfo) int {
	if c := compareIPv4(a.Announce.IP, b.Announce.IP); c != 0 {
		return c
	}
	if c := strings.Compare(a.Announce.MAC, b.Announce.MAC); c != 0 {
		return c
	}
	if c := strings.Compare(a.Announce.SN, b.Announce.SN); c != 0 {
		return c
	}
	return strings.Compare(a.Announce.Product, b.Announce.Product)
}

func compareIPv4(a, b string) int {
	aIP, aOK := parseIPv4(a)
	bIP, bOK := parseIPv4(b)
	switch {
	case aOK && bOK:
		for i := range aIP {
			if aIP[i] < bIP[i] {
				return -1
			}
			if aIP[i] > bIP[i] {
				return 1
			}
		}
		return 0
	case aOK:
		return -1
	case bOK:
		return 1
	default:
		return strings.Compare(a, b)
	}
}

func parseIPv4(ip string) ([4]byte, bool) {
	var key [4]byte
	parsed := net.ParseIP(strings.TrimSpace(ip)).To4()
	if parsed == nil {
		return key, false
	}
	copy(key[:], parsed)
	return key, true
}

func (d *DeviceInfo) CapsString() string {
	return strings.Join(d.Announce.Caps, ",")
}

func (d *DeviceInfo) Summary() string {
	return fmt.Sprintf("%s (%s) [%s] %s:%d", d.Announce.MAC, d.Announce.SN, d.Announce.Product, d.Announce.IP, d.Announce.Port)
}
