package handlers

import (
	"fmt"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"

	"aipc/platform/common/logger"
)

const (
	verizonInternetAPN = "vzwinternet"
	verizonDataCID     = 3
	verizonIMSIPrefix  = "311480"
	verizonRegWait     = 10 * time.Second
)

var (
	cgdcontLineRe  = regexp.MustCompile(`(?i)\+CGDCONT:\s*(\d+)\s*,\s*"([^"]*)"\s*,\s*"([^"]*)"`)
	cregStatRe     = regexp.MustCompile(`(?i)\+(?:CEREG|CREG):\s*\d+\s*,\s*(\d+)`)
	qmbnListFullRe = regexp.MustCompile(`(?i)\+QMBNCFG:\s*"List"\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*"([^"]*)"`)
)

type pdpContext struct {
	CID     int
	PDPType string
	APN     string
}

func isVerizonProfile(profile string) bool {
	return strings.EqualFold(strings.TrimSpace(profile), "verizon")
}

func isVerizonIMSI(imsi string) bool {
	return strings.HasPrefix(strings.TrimSpace(imsi), verizonIMSIPrefix)
}

func looksLikeVerizonNetwork(imsi, operator string) bool {
	return isVerizonIMSI(imsi) || strings.Contains(strings.ToLower(operator), "verizon")
}

func isVerizonMBNName(name string) bool {
	return strings.Contains(strings.ToLower(strings.TrimSpace(name)), "verizon")
}

func verizonMBNActiveFromAT(raw string) bool {
	for _, e := range parseMBNList(raw) {
		if isVerizonMBNName(e.Name) && e.Selected && e.Activated {
			return true
		}
	}
	return false
}

func shouldUseVerizonDial(profile, imsi, operator string, mbnActive bool) bool {
	if isVerizonProfile(profile) {
		return true
	}
	p := strings.TrimSpace(profile)
	if p == "" || strings.EqualFold(p, "generic") {
		return looksLikeVerizonNetwork(imsi, operator) || mbnActive
	}
	return false
}

func normalizeVerizonConfig(cfg *CellularConfig) {
	if cfg == nil || !isVerizonProfile(cfg.CarrierProfile) {
		return
	}
	cfg.CarrierProfile = "verizon"
	cfg.PDPCID = verizonDataCID
	if strings.TrimSpace(cfg.APN) == "" {
		cfg.APN = verizonInternetAPN
	}
	cfg.IPType = "ipv4v6"
	if cfg.Auth == "" {
		cfg.Auth = "none"
	}
	if strings.EqualFold(strings.TrimSpace(cfg.Usbnet), "mbim") || strings.TrimSpace(cfg.Usbnet) == "" {
		cfg.Usbnet = "qmi"
	}
}

func networkRegisteredFromAT(raw string) bool {
	for _, m := range cregStatRe.FindAllStringSubmatch(raw, -1) {
		if len(m) >= 2 && (m[1] == "1" || m[1] == "5") {
			return true
		}
	}
	return false
}

func parseCGDCONTContexts(raw string) []pdpContext {
	var out []pdpContext
	for _, line := range strings.Split(raw, "\n") {
		m := cgdcontLineRe.FindStringSubmatch(line)
		if m == nil {
			continue
		}
		cid, err := strconv.Atoi(m[1])
		if err != nil || cid < 1 {
			continue
		}
		out = append(out, pdpContext{
			CID:     cid,
			PDPType: strings.ToUpper(strings.TrimSpace(m[2])),
			APN:     strings.ToLower(strings.TrimSpace(m[3])),
		})
	}
	return out
}

func contextByCID(contexts []pdpContext, cid int) (pdpContext, bool) {
	for _, c := range contexts {
		if c.CID == cid {
			return c, true
		}
	}
	return pdpContext{}, false
}

func verizonDataContextOK(contexts []pdpContext) bool {
	c, ok := contextByCID(contexts, verizonDataCID)
	return ok && c.APN == verizonInternetAPN
}

func verizonIMSLooksBroken(contexts []pdpContext) bool {
	c, ok := contextByCID(contexts, 1)
	return ok && (c.APN == verizonInternetAPN || c.APN == "internet")
}

func (h *CellularHandler) prepareVerizonCarrier(cfg CellularConfig) error {
	atPortMu.Lock()
	defer atPortMu.Unlock()

	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if !anyDialerRunning(h.command) {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	time.Sleep(150 * time.Millisecond)

	port := findATPort()
	if port == "" {
		return fmt.Errorf("no AT port")
	}

	if raw, err := atExchange(port, []string{`AT+QMBNCFG="AutoSel",1`}, 800*time.Millisecond); err != nil {
		logger.Warn("cellular: Verizon QMBNCFG AutoSel: %v", err)
	} else if strings.Contains(raw, "ERROR") && !strings.Contains(raw, "OK") {
		logger.Info("cellular: Verizon QMBNCFG AutoSel unsupported/rejected (%s)", summarizeAT(raw))
	}

	raw, err := atExchange(port, []string{"AT+CGDCONT?"}, 2*time.Second)
	if err != nil {
		return fmt.Errorf("CGDCONT?: %w", err)
	}
	contexts := parseCGDCONTContexts(raw)
	if verizonIMSLooksBroken(contexts) {
		cid1APN := ""
		if c, ok := contextByCID(contexts, 1); ok {
			cid1APN = c.APN
		}
		logger.Warn("cellular: Verizon PDP#1 APN looks wrong (%q); leaving IMS untouched", cid1APN)
	}
	if !verizonDataContextOK(contexts) {
		apn := strings.TrimSpace(cfg.APN)
		if apn == "" {
			apn = verizonInternetAPN
		}
		set := fmt.Sprintf(`AT+CGDCONT=%d,"IPV4V6","%s"`, verizonDataCID, apn)
		setRaw, err := atExchange(port, []string{set}, 2*time.Second)
		if err != nil {
			return fmt.Errorf("set PDP#%d: %w", verizonDataCID, err)
		}
		if strings.Contains(setRaw, "ERROR") && !strings.Contains(setRaw, "OK") {
			return fmt.Errorf("modem rejected %s (%s)", set, summarizeAT(setRaw))
		}
		logger.Info("cellular: Verizon ensured PDP#%d IPV4V6/%s", verizonDataCID, apn)
	}

	if !h.waitNetworkRegisteredLocked(port, verizonRegWait) {
		logger.Warn("cellular: Verizon network not registered yet; continuing to dial")
	}
	return nil
}

func (h *CellularHandler) probeVerizonMBNActive() bool {
	atPortMu.Lock()
	defer atPortMu.Unlock()
	port := findATPort()
	if port == "" {
		return false
	}
	raw, err := atExchange(port, []string{`AT+QMBNCFG="List"`}, 2*time.Second)
	if err != nil {
		return false
	}
	return verizonMBNActiveFromAT(raw)
}

type cellularMBNEntry struct {
	Index     int    `json:"index"`
	Selected  bool   `json:"selected"`
	Activated bool   `json:"activated"`
	Name      string `json:"name"`
}

func parseMBNList(raw string) []cellularMBNEntry {
	var out []cellularMBNEntry
	for _, m := range qmbnListFullRe.FindAllStringSubmatch(raw, -1) {
		if len(m) < 5 {
			continue
		}
		idx, _ := strconv.Atoi(m[1])
		sel, _ := strconv.Atoi(m[2])
		act, _ := strconv.Atoi(m[3])
		out = append(out, cellularMBNEntry{
			Index:     idx,
			Selected:  sel != 0,
			Activated: act != 0,
			Name:      m[4],
		})
	}
	return out
}

func (h *CellularHandler) ListMBN(c *gin.Context) {
	if h.capabilityState() == cellularCapUnsupported {
		Resp(c).OK(gin.H{"ok": false, "message": "Cellular hardware is not available.", "entries": []cellularMBNEntry{}})
		return
	}
	atPortMu.Lock()
	defer atPortMu.Unlock()
	port := findATPort()
	if port == "" {
		Resp(c).OK(gin.H{"ok": false, "message": "No AT port available.", "entries": []cellularMBNEntry{}})
		return
	}
	raw, err := atExchange(port, []string{`AT+QMBNCFG="List"`}, 3*time.Second)
	if err != nil {
		Resp(c).OK(gin.H{"ok": false, "message": err.Error(), "entries": []cellularMBNEntry{}})
		return
	}
	entries := parseMBNList(raw)
	Resp(c).OK(gin.H{"ok": true, "message": "OK", "entries": entries})
}

func (h *CellularHandler) waitNetworkRegisteredLocked(port string, timeout time.Duration) bool {
	deadline := time.Now().Add(timeout)
	for {
		raw, err := atExchange(port, []string{"AT+CEREG?", "AT+CREG?"}, 800*time.Millisecond)
		if err == nil && networkRegisteredFromAT(raw) {
			return true
		}
		if !time.Now().Before(deadline) {
			return false
		}
		time.Sleep(1 * time.Second)
	}
}
