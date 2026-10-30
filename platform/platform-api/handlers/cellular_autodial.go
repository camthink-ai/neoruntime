package handlers

import (
	"os"
	"path/filepath"
	"strings"
	"sync/atomic"
	"time"

	"aipc/platform/common/logger"
)

const (
	cellularAutoDialInterval = 2 * time.Second
	cellularDialMinBackoff   = 5 * time.Second
	cellularDialMaxBackoff   = 2 * time.Minute
	cellularSessionStall     = 45 * time.Second
	cellularSIMRecoverAfter  = 4 * time.Second
	cellularSIMRecoverEvery  = 30 * time.Second
)

// settleAfterPowerOn waits for control nodes, then CFUN-settles if SIM is not READY.
func (h *CellularHandler) settleAfterPowerOn() {
	deadline := h.now().Add(8 * time.Second)
	for h.now().Before(deadline) {
		if h.hasModemControl() {
			break
		}
		time.Sleep(200 * time.Millisecond)
	}
	time.Sleep(800 * time.Millisecond)
	clearATCache()
	ready, ok := h.probeSIMReady()
	if ok && ready {
		h.clearSIMAbsent()
		h.clearDialBackoff()
		return
	}
	logger.Info("cellular: post-power SIM not ready (ok=%v ready=%v); CFUN settle", ok, ready)
	h.recoverRFAfterSIMInsert()
	ready, ok = h.probeSIMReady()
	if ok && ready {
		h.clearSIMAbsent()
	}
	h.clearDialBackoff()
}

func (h *CellularHandler) StartAutoDial() {
	if !atomic.CompareAndSwapInt32(&h.autoDialStarted, 0, 1) {
		return
	}
	h.autoDialKick = make(chan struct{}, 1)
	h.maybePromoteCapabilityFromUSB()
	go func() {
		if !h.cellularSupported() {
			h.runCapabilityProbe()
		}
		if h.cellularSupported() {
			if cfg, err := h.loadConfig(); err == nil && cfg.Enabled {
				if err := h.ensurePower(true); err != nil {
					logger.Warn("cellular: auto-dial initial power: %v", err)
				}
			}
		} else {
			logger.Info("cellular: capability unsupported after boot probe; supervisor idle until USB appears")
		}
		go h.autoDialLoop()
		h.kickAutoDial()
		logger.Info("cellular: auto-dial supervisor started")
	}()
}

func (h *CellularHandler) kickAutoDial() {
	if h.autoDialKick == nil {
		return
	}
	select {
	case h.autoDialKick <- struct{}{}:
	default:
	}
}

func (h *CellularHandler) dialerRunning() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.dialerCmd != nil && h.dialerCmd.Process != nil && procAlive(h.dialerCmd.Process.Pid) {
		return true
	}
	if pid := h.readPIDFile("cellular-dialer.pid"); pid > 0 && procAlive(pid) {
		return true
	}
	return false
}

func (h *CellularHandler) autoDialLoop() {
	ticker := time.NewTicker(cellularAutoDialInterval)
	defer ticker.Stop()
	for {
		select {
		case <-ticker.C:
		case <-h.autoDialKick:
		}
		if !h.cellularSupported() {
			h.maybePromoteCapabilityFromUSB()
			if !h.cellularSupported() {
				if h.dialerRunning() {
					_ = h.stopDialer()
				}
				continue
			}
		}
		cfg, err := h.loadConfig()
		if err != nil {
			cfg = defaultCellularConfig()
		}
		if !cfg.Enabled {
			if h.dialerRunning() {
				_ = h.stopDialer()
			}
			continue
		}
		if atomic.LoadInt32(&h.usbnetApplyBusy) != 0 {
			continue
		}

		vid, _, _ := h.findMatchedUSB()
		if vid == "" {
			if h.dialerRunning() {
				_ = h.stopDialer()
			}
			h.clearDialBackoff()
			h.resetSIMWatch()
			continue
		}

		if !h.hasModemControl() {
			continue
		}

		simReady, simOK := h.probeSIMReady()
		if simOK {
			h.handleSIMTransition(simReady)
			if !simReady {
				h.noteSIMAbsent()
				h.markStickySIMAbsent()
				_, v4, v6 := h.findNetIface()
				hasPDP := v4 != "" || v6 != ""
				if h.dialerRunning() && !hasPDP {
					logger.Info("cellular: SIM not ready; stopping dialer")
					_ = h.stopDialer()
				}
				h.sessionWaitSince = time.Time{}
				h.maybeRecoverSIM()
				continue
			}
			h.clearSIMAbsent()
		} else if !h.lastSIMReadyLocked() {
			h.maybeRecoverSIM()
			if !h.dialerRunning() {
				continue
			}
		}

		if h.dialerRunning() {
			_, v4, v6 := h.findNetIface()
			hasPDP := v4 != "" || v6 != ""
			if hasPDP {
				h.sessionWaitSince = time.Time{}
				continue
			}
			if h.sessionWaitSince.IsZero() {
				h.sessionWaitSince = h.now()
			} else if h.now().Sub(h.sessionWaitSince) >= cellularSessionStall {
				logger.Warn("cellular: dialer stalled without PDP for %s; restarting",
					cellularSessionStall)
				_ = h.stopDialer()
				h.sessionWaitSince = time.Time{}
				clearATCache()
				h.recoverRFAfterSIMInsert()
				continue
			} else {
				continue
			}
		}

		if simOK {
			if !simReady {
				continue
			}
		} else if !h.lastSIMReadyLocked() {
			continue
		}

		if until := h.dialBackoffUntil(); !until.IsZero() && h.now().Before(until) {
			continue
		}
		useEcm := strings.EqualFold(cfg.Usbnet, "ecm") || probeUsbnetDriver() == "ecm"
		dialer := ""
		if !useEcm {
			var err error
			dialer, err = h.findDialer()
			if err != nil {
				continue
			}
		} else if !h.dialerAvailable(cfg) {
			continue
		}
		if atomic.LoadInt32(&h.usbnetApplyBusy) != 0 {
			continue
		}
		if err := h.ensurePower(true); err != nil {
			logger.Warn("cellular: auto-dial power: %v", err)
			continue
		}
		if atomic.LoadInt32(&h.usbnetApplyBusy) != 0 {
			continue
		}
		h.refreshStickyIdentity()
		if err := h.startDialer(dialer, cfg); err != nil {
			logger.Warn("cellular: auto-dial start: %v", err)
			h.noteDialFailure()
			continue
		}
		h.clearDialBackoff()
		h.sessionWaitSince = h.now()
		logger.Info("cellular: auto-dial started (enabled + module present)")
	}
}

func (h *CellularHandler) resetSIMWatch() {
	h.mu.Lock()
	h.simStateKnown = false
	h.lastSIMReady = false
	h.simAbsentSince = time.Time{}
	h.simRecoverAt = time.Time{}
	h.sessionWaitSince = time.Time{}
	h.mu.Unlock()
}

func (h *CellularHandler) lastSIMReadyLocked() bool {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.lastSIMReady
}

func (h *CellularHandler) noteSIMAbsent() {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.simAbsentSince.IsZero() {
		h.simAbsentSince = h.now()
	}
}

func (h *CellularHandler) clearSIMAbsent() {
	h.mu.Lock()
	h.simAbsentSince = time.Time{}
	h.mu.Unlock()
}

func (h *CellularHandler) maybeRecoverSIM() {
	h.mu.Lock()
	absentSince := h.simAbsentSince
	next := h.simRecoverAt
	now := h.now()
	if absentSince.IsZero() {
		h.simAbsentSince = now
		absentSince = now
	}
	readyForRecover := now.Sub(absentSince) >= cellularSIMRecoverAfter
	cooldownOK := next.IsZero() || !now.Before(next)
	if !readyForRecover || !cooldownOK {
		h.mu.Unlock()
		return
	}
	h.simRecoverAt = now.Add(cellularSIMRecoverEvery)
	h.mu.Unlock()

	logger.Info("cellular: SIM not ready for %s; soft-cycling RF", now.Sub(absentSince).Round(time.Second))
	if h.dialerRunning() {
		_ = h.stopDialer()
	}
	h.recoverRFAfterSIMInsert()
	clearATCache()
	h.clearDialBackoff()
	h.kickAutoDial()
}

func (h *CellularHandler) handleSIMTransition(simReady bool) {
	h.mu.Lock()
	known := h.simStateKnown
	prev := h.lastSIMReady
	h.simStateKnown = true
	h.lastSIMReady = simReady
	h.mu.Unlock()

	if !known {
		if simReady {
			h.refreshStickyIdentity()
		}
		return
	}
	if prev && !simReady {
		logger.Info("cellular: SIM removed")
		clearATCache()
		h.markStickySIMAbsent()
		h.noteSIMAbsent()
		return
	}
	if !prev && simReady {
		logger.Info("cellular: SIM became READY; probing identity then dial")
		clearATCache()
		h.clearSIMAbsent()
		h.clearDialBackoff()
		h.sessionWaitSince = time.Time{}
		h.refreshStickyIdentity()
		h.kickAutoDial()
	}
}

func (h *CellularHandler) recoverRFAfterSIMInsert() {
	if !atPortMu.TryLock() {
		return
	}
	defer atPortMu.Unlock()
	port := findATPort()
	if port == "" {
		return
	}
	logger.Info("cellular: AT+CFUN=0/1 to re-read SIM")
	if _, err := atExchange(port, []string{"AT+CFUN=0"}, 3*time.Second); err != nil {
		logger.Warn("cellular: SIM recover CFUN=0: %v", err)
	}
	time.Sleep(2 * time.Second)
	if _, err := atExchange(port, []string{"AT+CFUN=1"}, 3*time.Second); err != nil {
		logger.Warn("cellular: SIM recover CFUN=1: %v", err)
	}
	time.Sleep(3 * time.Second)
	clearATCache()
}

func (h *CellularHandler) dialBackoffUntil() time.Time {
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.dialFailUntil
}

func (h *CellularHandler) noteDialFailure() {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.dialBackoff < cellularDialMinBackoff {
		h.dialBackoff = cellularDialMinBackoff
	} else {
		h.dialBackoff *= 2
		if h.dialBackoff > cellularDialMaxBackoff {
			h.dialBackoff = cellularDialMaxBackoff
		}
	}
	h.dialFailUntil = h.now().Add(h.dialBackoff)
	logger.Warn("cellular: dialer backoff %s (until control plane / next retry)", h.dialBackoff)
}

func (h *CellularHandler) clearDialBackoff() {
	h.mu.Lock()
	defer h.mu.Unlock()
	h.dialFailUntil = time.Time{}
	h.dialBackoff = 0
}

// hasModemControl is true when a profile-listed AT or control device node exists.
func (h *CellularHandler) hasModemControl() bool {
	for _, p := range cellularPreferredATPorts {
		if st, err := os.Stat(p); err == nil && st.Mode()&os.ModeCharDevice != 0 {
			return true
		}
	}
	if cellularATPortGlob != "" {
		if matches, _ := filepath.Glob(cellularATPortGlob); len(matches) > 0 {
			return true
		}
	}
	for _, g := range cellularControlGlobs {
		if matches, _ := filepath.Glob(g); len(matches) > 0 {
			return true
		}
	}
	return false
}
