package handlers

import (
	"time"

	"aipc/platform/common/logger"
)

const (
	cellularCapPending     = "pending"
	cellularCapSupported   = "supported"
	cellularCapUnsupported = "unsupported"
)

func (h *CellularHandler) initCapabilityState() {
	h.capMu.Lock()
	defer h.capMu.Unlock()
	if h.hw == nil {
		h.capability = cellularCapUnsupported
		return
	}
	h.capability = cellularCapPending
}

func (h *CellularHandler) capabilityState() string {
	h.capMu.Lock()
	defer h.capMu.Unlock()
	if h.capability == "" {
		return cellularCapPending
	}
	return h.capability
}

func (h *CellularHandler) cellularSupported() bool {
	return h.capabilityState() == cellularCapSupported
}

func (h *CellularHandler) setCapability(state string) {
	h.capMu.Lock()
	defer h.capMu.Unlock()
	h.capability = state
}

func (h *CellularHandler) maybePromoteCapabilityFromUSB() {
	if h.hw == nil {
		return
	}
	if h.capabilityState() == cellularCapSupported {
		return
	}
	vid, pid, _ := h.findMatchedUSB()
	if vid == "" {
		return
	}
	h.setCapability(cellularCapSupported)
	logger.Info("cellular: capability=supported (USB %s:%s)", vid, pid)
	h.kickAutoDial()
}

func (h *CellularHandler) runCapabilityProbe() {
	if h.hw == nil {
		h.setCapability(cellularCapUnsupported)
		logger.Info("cellular: capability=unsupported (no hardware profile)")
		return
	}

	if vid, _, _ := h.findMatchedUSB(); vid != "" {
		h.setCapability(cellularCapSupported)
		logger.Info("cellular: capability=supported (USB already present %s)", vid)
		return
	}

	if h.hw.GPIO.Power.hasLine() {
		if err := h.ensurePower(true); err != nil {
			_ = h.ensurePower(false)
			h.setCapability(cellularCapUnsupported)
			logger.Warn("cellular: capability=unsupported (power failed: %v)", err)
			return
		}
	}

	timeout := time.Duration(h.hw.GPIO.ModuleDetectTimeoutSec) * time.Second
	if timeout <= 0 {
		timeout = 25 * time.Second
	}
	logger.Info("cellular: boot capability probe waiting up to %s for USB module", timeout)
	if h.waitForModule(timeout) {
		vid, pid, _ := h.findMatchedUSB()
		h.setCapability(cellularCapSupported)
		logger.Info("cellular: capability=supported (USB %s:%s)", vid, pid)
		return
	}

	_ = h.stopDialer()
	clearATCache()
	if err := h.ensurePower(false); err != nil {
		logger.Warn("cellular: power off after failed capability probe: %v", err)
	}
	h.setCapability(cellularCapUnsupported)
	logger.Info("cellular: capability=unsupported (no module on USB within %s); dialer disabled this boot", timeout)
}
