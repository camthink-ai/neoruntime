package handlers

import (
	"bufio"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// Identity probe + sticky fields for GET /cellular/status.
// Port I/O lives in cellular_at.go.

type modemIdentity struct {
	SIMReady   bool
	Operator   string
	Network    string
	IMEI       string
	ICCID      string
	IMSI       string
	MSISDN     string
	Firmware   string
	RegState   string
	SignalRSRP *int
	SignalRSRQ *int
	ProbedAt   time.Time
	OK         bool
}

// probeSIMReady returns (ready, ok). ok=false means probe failed (busy/error),
// not a definitive SIM-absent answer.
func (h *CellularHandler) probeSIMReady() (ready bool, ok bool) {
	if !atPortMu.TryLock() {
		return false, false
	}
	defer atPortMu.Unlock()
	port := findATPort()
	if port == "" {
		return false, false
	}
	raw, err := atExchange(port, []string{"AT+CPIN?"}, 800*time.Millisecond)
	if err != nil || strings.TrimSpace(raw) == "" {
		return false, false
	}
	ready, ok = parseCPINResponse(raw)
	if !ok {
		return false, false
	}
	atCacheMu.Lock()
	if atCache.OK {
		atCache.SIMReady = ready
		if !ready {
			atCache.ICCID = ""
			atCache.IMSI = ""
			atCache.Operator = ""
			atCache.MSISDN = ""
		}
	}
	atCacheMu.Unlock()
	return ready, true
}

// parseCPINResponse: CME 10/13/14/15 are definitive not-ready; other CME → ok=false.
func parseCPINResponse(raw string) (ready bool, ok bool) {
	norm := strings.ReplaceAll(raw, "\r", "\n")
	if m := reCPIN.FindStringSubmatch(norm); len(m) == 2 {
		return strings.EqualFold(m[1], "READY"), true
	}
	if m := reCMEError.FindStringSubmatch(norm); len(m) == 2 {
		switch m[1] {
		case "10", "13", "14", "15":
			return false, true
		}
		// Other CME errors are ambiguous (e.g. busy/operation not allowed).
		return false, false
	}
	return false, false
}

var (
	reDigits   = regexp.MustCompile(`\d{10,}`)
	reCOPS     = regexp.MustCompile(`\+COPS:\s*\d+,\d+,"([^"]*)"`)
	reCPIN     = regexp.MustCompile(`\+CPIN:\s*(\S+)`)
	reCMEError = regexp.MustCompile(`\+CME ERROR:\s*(\d+)`)
	reCGSN     = regexp.MustCompile(`(?i)\+CGSN:\s*"?(\d{14,17})"?`)
	reCIMI     = regexp.MustCompile(`(?i)\+CIMI:\s*"?(\d{14,16})"?`)
	reCCID     = regexp.MustCompile(`\+(?:Q)?CCID:\s*(\d+)`)
	reCNUM     = regexp.MustCompile(`\+CNUM:\s*[^,]*,"([^"]*)"`)
	reCEREG    = regexp.MustCompile(`\+CEREG:\s*\d+,(\d+)`)
	reQCSQ     = regexp.MustCompile(`\+QCSQ:\s*(.+)`)
	reATIRev   = regexp.MustCompile(`(?m)^Revision:\s*(.+)$`)
)

func identityIncomplete(id modemIdentity) bool {
	if !id.OK {
		return true
	}
	return id.SIMReady && (id.ICCID == "" || id.IMSI == "")
}

func statusIdentityIncomplete(st *CellularStatus) bool {
	if st == nil {
		return true
	}
	if st.Connected || st.SIMReady {
		return st.ICCID == "" || st.IMSI == "" || st.Operator == ""
	}
	return st.IMEI == ""
}

func (h *CellularHandler) enrichFromAT(st *CellularStatus) {
	if st == nil || !st.ModulePresent || !h.hasModemControl() {
		return
	}
	h.applyStickyIdentity(st)

	wait := 150 * time.Millisecond
	force := statusIdentityIncomplete(st)
	if force {
		wait = 2 * time.Second
	}
	if !lockATPort(wait) {
		h.applyAutodialSIMOverride(st)
		return
	}
	defer atPortMu.Unlock()
	id := h.probeModemIdentityForce(force)
	if !id.OK {
		h.applyAutodialSIMOverride(st)
		return
	}
	h.storeStickyIdentity(id)
	h.applyIdentity(st, id)
	h.applyAutodialSIMOverride(st)
}

// refreshStickyIdentity probes identity while the dialer is not holding AT.
func (h *CellularHandler) refreshStickyIdentity() {
	if !lockATPort(3 * time.Second) {
		return
	}
	defer atPortMu.Unlock()
	clearATCache()
	id := probeATIdentity()
	if !id.OK {
		return
	}
	id.ProbedAt = h.now()
	h.storeStickyIdentity(id)
	if !identityIncomplete(id) {
		atCacheMu.Lock()
		atCache = id
		atCacheMu.Unlock()
	}
}

// applyAutodialSIMOverride clears SIM UI fields when autodial knows SIM is gone.
func (h *CellularHandler) applyAutodialSIMOverride(st *CellularStatus) {
	if st == nil || st.Connected {
		return
	}
	h.mu.Lock()
	known := h.simStateKnown
	ready := h.lastSIMReady
	h.mu.Unlock()
	if !known || ready {
		return
	}
	st.SIMReady = false
	st.ICCID = ""
	st.IMSI = ""
	st.MSISDN = ""
	st.Operator = ""
	st.Network = ""
	st.SignalRSRP = nil
	st.SignalRSRQ = nil
	if st.RegState == "" || st.RegState == "registered" || st.RegState == "roaming" {
		st.RegState = "not_registered"
	}
}

func (h *CellularHandler) markStickySIMAbsent() {
	h.lastIdentityMu.Lock()
	defer h.lastIdentityMu.Unlock()
	id := h.lastIdentity
	if !id.OK && id.IMEI == "" && id.Firmware == "" {
		return
	}
	id.SIMReady = false
	id.ICCID = ""
	id.IMSI = ""
	id.MSISDN = ""
	id.Operator = ""
	id.Network = ""
	id.SignalRSRP = nil
	id.SignalRSRQ = nil
	id.RegState = "not_registered"
	h.lastIdentity = id
}

func (h *CellularHandler) storeStickyIdentity(id modemIdentity) {
	h.lastIdentityMu.Lock()
	defer h.lastIdentityMu.Unlock()
	prev := h.lastIdentity
	if id.IMEI == "" {
		id.IMEI = prev.IMEI
	}
	if id.Firmware == "" {
		id.Firmware = prev.Firmware
	}
	if id.SIMReady {
		if id.ICCID == "" {
			id.ICCID = prev.ICCID
		}
		if id.IMSI == "" {
			id.IMSI = prev.IMSI
		}
		if id.MSISDN == "" {
			id.MSISDN = prev.MSISDN
		}
		if id.Operator == "" {
			id.Operator = prev.Operator
		}
	} else {
		id.ICCID = ""
		id.IMSI = ""
		id.MSISDN = ""
		id.Operator = ""
		id.Network = ""
		id.SignalRSRP = nil
		id.SignalRSRQ = nil
		if id.RegState == "" || id.RegState == "registered" || id.RegState == "roaming" {
			id.RegState = "not_registered"
		}
	}
	h.lastIdentity = id
}

func (h *CellularHandler) applyStickyIdentity(st *CellularStatus) {
	h.lastIdentityMu.Lock()
	id := h.lastIdentity
	h.lastIdentityMu.Unlock()
	if !id.OK && id.IMEI == "" && id.Firmware == "" {
		return
	}
	h.applyIdentity(st, id)
}

func (h *CellularHandler) clearStickyIdentity() {
	h.lastIdentityMu.Lock()
	h.lastIdentity = modemIdentity{}
	h.lastIdentityMu.Unlock()
}

func (h *CellularHandler) applyIdentity(st *CellularStatus, id modemIdentity) {
	st.SIMReady = id.SIMReady
	if id.Operator != "" {
		st.Operator = id.Operator
	}
	if id.Network != "" {
		st.Network = id.Network
	}
	if id.IMEI != "" {
		st.IMEI = id.IMEI
	}
	st.ICCID = id.ICCID
	st.IMSI = id.IMSI
	st.MSISDN = id.MSISDN
	if id.Firmware != "" {
		st.Firmware = id.Firmware
	}
	if id.SignalRSRP != nil {
		st.SignalRSRP = id.SignalRSRP
	}
	if id.SignalRSRQ != nil {
		st.SignalRSRQ = id.SignalRSRQ
	}
	if id.RegState != "" {
		if st.RegState == "" || st.RegState == "unknown" || !st.Connected {
			st.RegState = id.RegState
		}
	}
	if st.Connected {
		st.SIMReady = true
	}
}

func (h *CellularHandler) probeModemIdentity() modemIdentity {
	return h.probeModemIdentityForce(false)
}

func (h *CellularHandler) probeModemIdentityForce(force bool) modemIdentity {
	atCacheMu.Lock()
	defer atCacheMu.Unlock()
	now := h.now()
	if !force && atCache.OK && now.Sub(atCache.ProbedAt) < atCacheTTL && !identityIncomplete(atCache) {
		return atCache
	}
	id := probeATIdentity()
	id.ProbedAt = now
	if id.OK && (id.IMEI == "" || identityIncomplete(id)) {
		return id
	}
	if id.OK {
		atCache = id
	}
	return id
}

func probeATIdentity() modemIdentity {
	port := findATPort()
	if port == "" {
		return modemIdentity{}
	}
	raw, err := atExchange(port, []string{
		"ATE0",
		"ATI",
		"AT+CPIN?",
		"AT+CGSN",
		"AT+CIMI",
		"AT+CCID",
		"AT+COPS?",
		"AT+CEREG?",
		"AT+QCSQ",
		"AT+CNUM",
	}, 400*time.Millisecond)
	if err != nil || strings.TrimSpace(raw) == "" {
		return modemIdentity{}
	}
	return parseModemIdentity(raw)
}

func parseModemIdentity(raw string) modemIdentity {
	id := modemIdentity{OK: true}
	norm := strings.ReplaceAll(raw, "\r", "\n")

	if ready, cpinOK := parseCPINResponse(norm); cpinOK {
		id.SIMReady = ready
	}
	if m := reATIRev.FindStringSubmatch(norm); len(m) == 2 {
		id.Firmware = strings.TrimSpace(m[1])
	}
	if m := reCOPS.FindStringSubmatch(norm); len(m) == 2 {
		id.Operator = strings.TrimSpace(m[1])
	}
	if m := reCCID.FindStringSubmatch(norm); len(m) == 2 {
		id.ICCID = m[1]
	}
	if m := reCNUM.FindStringSubmatch(norm); len(m) == 2 {
		id.MSISDN = strings.TrimSpace(m[1])
	}
	if m := reCEREG.FindStringSubmatch(norm); len(m) == 2 {
		id.RegState = ceregState(m[1])
	}

	id.IMEI = extractIMEI(norm)
	id.IMSI = extractIMSI(norm, id.IMEI)

	if m := reQCSQ.FindStringSubmatch(norm); len(m) == 2 {
		parseQCSQ(m[1], &id)
	}

	if !id.SIMReady && (id.ICCID != "" || (id.IMSI != "" && id.IMSI != id.IMEI)) {
		id.SIMReady = true
	}
	return id
}

func extractIMEI(norm string) string {
	if m := reCGSN.FindStringSubmatch(norm); len(m) == 2 {
		return m[1]
	}
	if d := firstBareDigits(norm, "AT+CGSN", 14, 17, ""); d != "" {
		return d
	}
	if d := firstBareDigits(norm, "AT+GSN", 14, 17, ""); d != "" {
		return d
	}
	return ""
}

func extractIMSI(norm, skipIMEI string) string {
	if m := reCIMI.FindStringSubmatch(norm); len(m) == 2 {
		if m[1] != skipIMEI {
			return m[1]
		}
	}
	return firstBareDigits(norm, "AT+CIMI", 14, 16, skipIMEI)
}

func firstBareDigits(norm, afterCmd string, minLen, maxLen int, skip string) string {
	idx := strings.Index(norm, afterCmd)
	if idx < 0 {
		return ""
	}
	search := norm[idx+len(afterCmd):]
	sc := bufio.NewScanner(strings.NewReader(search))
	for sc.Scan() {
		line := strings.TrimSpace(sc.Text())
		if line == "" || line == "OK" || line == "ERROR" || strings.HasPrefix(line, "+CME") {
			continue
		}
		if strings.HasPrefix(line, "AT") {
			if line == afterCmd || strings.HasPrefix(line, afterCmd) {
				continue
			}
			break
		}
		if strings.HasPrefix(line, "+") {
			if i := strings.Index(line, ":"); i >= 0 {
				line = strings.TrimSpace(line[i+1:])
			} else {
				continue
			}
		}
		d := reDigits.FindString(line)
		if len(d) >= minLen && len(d) <= maxLen && d != skip {
			return d
		}
	}
	return ""
}

func parseQCSQ(body string, id *modemIdentity) {
	parts := splitATCSV(strings.TrimSpace(body))
	if len(parts) == 0 {
		return
	}
	mode := strings.Trim(parts[0], `"`)
	if mode == "" || strings.EqualFold(mode, "NOSERVICE") {
		return
	}
	id.Network = mode
	if len(parts) >= 3 {
		if v, err := strconv.Atoi(parts[2]); err == nil {
			id.SignalRSRP = &v
		}
	}
	if len(parts) >= 5 {
		if v, err := strconv.Atoi(parts[4]); err == nil {
			id.SignalRSRQ = &v
		}
	}
}

func splitATCSV(s string) []string {
	var out []string
	var cur strings.Builder
	inQ := false
	for _, r := range s {
		switch {
		case r == '"':
			inQ = !inQ
			cur.WriteRune(r)
		case r == ',' && !inQ:
			out = append(out, strings.TrimSpace(cur.String()))
			cur.Reset()
		default:
			cur.WriteRune(r)
		}
	}
	if cur.Len() > 0 {
		out = append(out, strings.TrimSpace(cur.String()))
	}
	return out
}

func ceregState(stat string) string {
	switch stat {
	case "1":
		return "registered"
	case "5":
		return "roaming"
	case "2":
		return "searching"
	case "3":
		return "denied"
	case "0":
		return "not_registered"
	default:
		return "unknown"
	}
}
