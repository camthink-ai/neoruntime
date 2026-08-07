package discovery

import (
	"sync"
	"time"

	pb "aipc/platform/device-discovery/proto"
)

const defaultTimeout = 30 * time.Second

// resolveKey returns the primary identity key for an announce.
// MAC is preferred as it's a stable hardware identifier; SN is the fallback
// for devices that may not have a MAC in the announce (e.g. very old firmware).
func resolveKey(a Announce) string {
	if a.MAC != "" {
		return a.MAC
	}
	return a.SN
}

type Registry struct {
	mu      sync.RWMutex
	devices map[string]*pb.DiscoveredDevice // keyed by MAC address (fallback to serial_number)
	timeout time.Duration
}

func NewRegistry() *Registry {
	return &Registry{
		devices: make(map[string]*pb.DiscoveredDevice),
		timeout: defaultTimeout,
	}
}

func (r *Registry) Update(a Announce) (*pb.DiscoveredDevice, bool) {
	r.mu.Lock()
	defer r.mu.Unlock()

	key := resolveKey(a)
	now := time.Now().Unix()
	isNew := false

	existing, ok := r.devices[key]
	if !ok {
		isNew = true
		existing = &pb.DiscoveredDevice{
			SerialNumber: a.SN,
			FirstSeen:    now,
			Status:       pb.DeviceStatus_DEVICE_ONLINE,
		}
	}

	existing.Product = a.Product
	existing.MacAddress = a.MAC
	existing.IpAddress = a.IP
	existing.ApiPort = int32(a.Port)
	existing.FirmwareVersion = a.FW
	existing.HardwarePlatform = a.HW
	existing.Capabilities = a.Caps
	existing.LastSeen = now
	existing.Status = pb.DeviceStatus_DEVICE_ONLINE

	r.devices[key] = existing
	return existing, isNew
}

func (r *Registry) List(filterProduct string, filterStatus pb.DeviceStatus) []*pb.DiscoveredDevice {
	r.mu.RLock()
	defer r.mu.RUnlock()

	result := make([]*pb.DiscoveredDevice, 0, len(r.devices))
	for _, d := range r.devices {
		if filterProduct != "" && d.Product != filterProduct {
			continue
		}
		if filterStatus != pb.DeviceStatus_DEVICE_ONLINE && d.Status != filterStatus {
			continue
		}
		result = append(result, d)
	}
	return result
}

// GetByMAC looks up a device by its MAC address (the primary map key).
func (r *Registry) GetByMAC(mac string) (*pb.DiscoveredDevice, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()

	d, ok := r.devices[mac]
	return d, ok
}

// Get looks up a device by serial number (iterates the map).
// Prefer GetByMAC when the MAC is known.
func (r *Registry) Get(sn string) (*pb.DiscoveredDevice, bool) {
	r.mu.RLock()
	defer r.mu.RUnlock()

	for _, d := range r.devices {
		if d.SerialNumber == sn {
			return d, true
		}
	}
	return nil, false
}

func (r *Registry) CheckTimeouts() []*pb.DiscoveredDevice {
	r.mu.Lock()
	defer r.mu.Unlock()

	var expired []*pb.DiscoveredDevice
	now := time.Now().Unix()
	threshold := int64(r.timeout.Seconds())

	for _, d := range r.devices {
		if d.Status == pb.DeviceStatus_DEVICE_ONLINE && (now-d.LastSeen) > threshold {
			d.Status = pb.DeviceStatus_DEVICE_OFFLINE
			expired = append(expired, d)
		}
	}
	return expired
}
