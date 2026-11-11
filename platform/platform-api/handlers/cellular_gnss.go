package handlers

import (
	"errors"
	"fmt"
	"regexp"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"aipc/platform/common/logger"
	"github.com/gin-gonic/gin"
)

var errATPortBusy = errors.New("AT port busy")

var (
	reQGPSState = regexp.MustCompile(`(?i)\+QGPS:\s*(\d+)`)
	reQGPSLOC   = regexp.MustCompile(`(?i)\+QGPSLOC:\s*([^\r\n]+)`)
)

// CellularGnssStatus is GET /cellular/gnss/status.
type CellularGnssStatus struct {
	Enabled    bool     `json:"enabled"` // config gnss_enabled
	Active     bool     `json:"active"`  // modem AT+QGPS state
	Fix        string   `json:"fix"`     // none|2d|3d
	Latitude   *float64 `json:"latitude"`
	Longitude  *float64 `json:"longitude"`
	Altitude   *float64 `json:"altitude"` // meters
	Hdop       *float64 `json:"hdop"`
	Satellites *int     `json:"satellites"`
	UTC        string   `json:"utc,omitempty"`
	Message    string   `json:"message"`
}

// GetGnssStatus returns live Quectel GNSS engine + fix info.
func (h *CellularHandler) GetGnssStatus(c *gin.Context) {
	cfg, err := h.loadConfig()
	if err != nil {
		cfg = defaultCellularConfig()
	}
	Resp(c).OK(h.probeGnssStatus(cfg))
}

const gnssCacheTTL = 2 * time.Minute

func (h *CellularHandler) probeGnssStatus(cfg CellularConfig) CellularGnssStatus {
	st := CellularGnssStatus{
		Enabled: gnssEnabled(cfg),
		Fix:     "none",
		Message: "GNSS is off.",
	}
	if !cfg.Enabled {
		st.Enabled = false
		st.Message = "Module power is off."
		h.clearGnssCache()
		return st
	}
	if !gnssEnabled(cfg) {
		h.clearGnssCache()
		return st
	}
	return h.readGnssFromModem(st)
}

func gnssStatusHasFix(st CellularGnssStatus) bool {
	return st.Fix != "none" && st.Latitude != nil && st.Longitude != nil
}

func (h *CellularHandler) clearGnssCache() {
	h.gnssCacheMu.Lock()
	h.gnssCache = CellularGnssStatus{}
	h.gnssCacheAt = time.Time{}
	h.gnssCacheMu.Unlock()
}

func (h *CellularHandler) rememberGnssFix(st CellularGnssStatus) {
	if !gnssStatusHasFix(st) {
		return
	}
	h.gnssCacheMu.Lock()
	h.gnssCache = st
	h.gnssCacheAt = time.Now()
	h.gnssCacheMu.Unlock()
}

// lastGnssFix returns a recent fix when the AT port is contended (dial/identity).
func (h *CellularHandler) lastGnssFix() (CellularGnssStatus, bool) {
	h.gnssCacheMu.Lock()
	defer h.gnssCacheMu.Unlock()
	if h.gnssCacheAt.IsZero() || time.Since(h.gnssCacheAt) > gnssCacheTTL {
		return CellularGnssStatus{}, false
	}
	if !gnssStatusHasFix(h.gnssCache) {
		return CellularGnssStatus{}, false
	}
	return h.gnssCache, true
}

func (h *CellularHandler) readGnssFromModem(st CellularGnssStatus) CellularGnssStatus {
	if !lockATPort(3 * time.Second) {
		if cached, ok := h.lastGnssFix(); ok {
			cached.Message = "GNSS fix acquired."
			return cached
		}
		st.Message = "AT port busy; retry shortly."
		return st
	}
	defer atPortMu.Unlock()

	port := findATPort()
	if port == "" {
		st.Message = "No AT port."
		return st
	}

	raw, err := atExchange(port, []string{"AT+QGPS?"}, 2*time.Second)
	if err != nil {
		st.Message = "Failed to query GNSS state: " + err.Error()
		return st
	}
	active, ok := parseQGPSState(raw)
	if !ok {
		st.Message = "Unexpected AT+QGPS? reply."
		return st
	}
	st.Active = active
	if !active {
		// Allow boot/reconcile to retry AT+QGPS=1 (flag can be stale after CFUN/USB).
		atomic.StoreInt32(&h.gnssEngineApplied, 0)
		h.clearGnssCache()
		st.Message = "GNSS enabled in config; engine not started yet."
		return st
	}

	locRaw, err := atExchange(port, []string{"AT+QGPSLOC=2"}, 3*time.Second)
	if err != nil {
		st.Message = "GNSS active; location query failed."
		return st
	}
	up := strings.ToUpper(locRaw)
	if strings.Contains(up, "ERROR: 516") || strings.Contains(up, "NOT FIXED") {
		st.Message = "Waiting for GNSS fix…"
		return st
	}
	if !parseQGPSLOC(locRaw, &st) {
		st.Message = "GNSS active; no fix yet."
		return st
	}
	st.Message = "GNSS fix acquired."
	h.rememberGnssFix(st)
	return st
}

func parseQGPSState(raw string) (active bool, ok bool) {
	m := reQGPSState.FindStringSubmatch(raw)
	if m == nil {
		return false, false
	}
	return m[1] == "1", true
}

// parseQGPSLOC parses AT+QGPSLOC=2 (decimal degrees) into st.
// Format: UTC,lat,lon,hdop,alt,fix,cog,spkm,spkn,date,nsat
func parseQGPSLOC(raw string, st *CellularGnssStatus) bool {
	m := reQGPSLOC.FindStringSubmatch(raw)
	if m == nil {
		return false
	}
	parts := strings.Split(m[1], ",")
	if len(parts) < 11 {
		return false
	}
	lat, err1 := strconv.ParseFloat(strings.TrimSpace(parts[1]), 64)
	lon, err2 := strconv.ParseFloat(strings.TrimSpace(parts[2]), 64)
	if err1 != nil || err2 != nil {
		return false
	}
	st.UTC = strings.TrimSpace(parts[0])
	st.Latitude = floatPtr(lat)
	st.Longitude = floatPtr(lon)
	if hdop, err := strconv.ParseFloat(strings.TrimSpace(parts[3]), 64); err == nil {
		st.Hdop = floatPtr(hdop)
	}
	if alt, err := strconv.ParseFloat(strings.TrimSpace(parts[4]), 64); err == nil {
		st.Altitude = floatPtr(alt)
	}
	fixMode := strings.TrimSpace(parts[5])
	switch fixMode {
	case "2":
		st.Fix = "2d"
	case "3":
		st.Fix = "3d"
	default:
		if lat != 0 || lon != 0 {
			st.Fix = "2d"
		} else {
			st.Fix = "none"
		}
	}
	if nsat, err := strconv.Atoi(strings.TrimSpace(parts[10])); err == nil {
		st.Satellites = gnssIntPtr(nsat)
	}
	return st.Fix != "none"
}

func floatPtr(v float64) *float64 { return &v }
func gnssIntPtr(v int) *int       { return &v }

func (h *CellularHandler) markGnssEngineApplied(on bool) {
	if on {
		atomic.StoreInt32(&h.gnssEngineApplied, 1)
	} else {
		atomic.StoreInt32(&h.gnssEngineApplied, 0)
		h.clearGnssCache()
	}
}

// reconcileGnssEngine starts the modem GNSS session when config wants it.
// Quectel does not persist AT+QGPS across reboot; UpdateConfig alone is not enough.
// Caller should ensure the module is present and the AT port is available.
func (h *CellularHandler) reconcileGnssEngine(cfg CellularConfig) {
	want := gnssEnabled(cfg)
	if !want {
		if atomic.LoadInt32(&h.gnssEngineApplied) != 1 {
			return
		}
		if err := h.applyGnssEnabledWait(false, 3*time.Second); err != nil {
			if !errors.Is(err, errATPortBusy) {
				logger.Warn("cellular: reconcile GNSS off: %v", err)
			}
		}
		return
	}
	if atomic.LoadInt32(&h.gnssEngineApplied) == 1 {
		return
	}
	if err := h.applyGnssEnabledWait(true, 3*time.Second); err != nil {
		if !errors.Is(err, errATPortBusy) && !strings.Contains(err.Error(), "no AT port") {
			logger.Warn("cellular: reconcile GNSS on: %v", err)
		}
		return
	}
	logger.Info("cellular: GNSS engine started (boot/reconcile)")
}

// applyGnssEnabled starts or stops the Quectel GNSS engine.
// EM05: ON = AT+QGPS=1, OFF = AT+QGPSEND (AT+QGPS=0 is invalid → CME 501).
func (h *CellularHandler) applyGnssEnabled(on bool) error {
	return h.applyGnssEnabledWait(on, 20*time.Second)
}

func (h *CellularHandler) applyGnssEnabledWait(on bool, lockWait time.Duration) error {
	if !lockATPort(lockWait) {
		return errATPortBusy
	}
	defer atPortMu.Unlock()

	port := findATPort()
	if port == "" {
		if !on {
			h.markGnssEngineApplied(false)
			return nil
		}
		return fmt.Errorf("no AT port")
	}

	if on {
		raw, err := atExchange(port, []string{
			`AT+QGPSCFG="outport","usbnmea"`,
			"AT+QGPS=1",
		}, 3*time.Second)
		if err != nil {
			return err
		}
		up := strings.ToUpper(raw)
		// 504 = session already ongoing
		if strings.Contains(up, "OK") || strings.Contains(up, "ERROR: 504") {
			h.markGnssEngineApplied(true)
			logger.Info("cellular: GNSS enabled")
			return nil
		}
		return fmt.Errorf("AT+QGPS=1 failed: %s", summarizeAT(raw))
	}

	raw, err := atExchange(port, []string{"AT+QGPSEND"}, 3*time.Second)
	if err != nil {
		return err
	}
	up := strings.ToUpper(raw)
	// 505 = session not active (already off)
	if strings.Contains(up, "OK") || strings.Contains(up, "ERROR: 505") {
		h.markGnssEngineApplied(false)
		logger.Info("cellular: GNSS disabled")
		return nil
	}
	return fmt.Errorf("AT+QGPSEND failed: %s", summarizeAT(raw))
}

// applyGnssEnabledOrDefer applies GNSS now; if AT is busy, retry in background.
func (h *CellularHandler) applyGnssEnabledOrDefer(on bool) error {
	err := h.applyGnssEnabled(on)
	if err == nil {
		return nil
	}
	if errors.Is(err, errATPortBusy) || strings.Contains(err.Error(), "no AT port") {
		logger.Warn("cellular: GNSS apply deferred (want=%v): %v", on, err)
		go h.retryGnssApply(on)
		return nil
	}
	return err
}

func (h *CellularHandler) retryGnssApply(on bool) {
	for i := 0; i < 12; i++ {
		time.Sleep(2 * time.Second)
		cfg, err := h.loadConfig()
		if err != nil || !cfg.Enabled || gnssEnabled(cfg) != on {
			return
		}
		if err := h.applyGnssEnabledWait(on, 8*time.Second); err != nil {
			logger.Warn("cellular: GNSS retry %d/12: %v", i+1, err)
			continue
		}
		return
	}
	logger.Warn("cellular: GNSS apply gave up after retries (want=%v)", on)
}

func summarizeAT(raw string) string {
	s := strings.TrimSpace(raw)
	if len(s) > 160 {
		return s[:160] + "…"
	}
	return s
}
