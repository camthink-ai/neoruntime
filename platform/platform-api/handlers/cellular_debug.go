package handlers

import (
	"fmt"
	"regexp"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
)

var (
	debugPingHostRe  = regexp.MustCompile(`(?i)^[a-z0-9][a-z0-9._:-]{0,253}$`)
	debugPingIfaceRe = regexp.MustCompile(`(?i)^[a-z][a-z0-9._-]{0,15}$`)
)

type cellularATRequest struct {
	Command string `json:"command" binding:"required"`
}

type cellularPingRequest struct {
	Host  string `json:"host"`
	Count int    `json:"count"`
	Iface string `json:"iface"`
}

func (h *CellularHandler) DebugAT(c *gin.Context) {
	var req cellularATRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidJSON, err.Error())
		return
	}
	cmd, err := sanitizeATCommand(req.Command)
	if err != nil {
		Resp(c).OK(gin.H{"ok": false, "message": err.Error(), "response": ""})
		return
	}
	if h.capabilityState() == cellularCapUnsupported {
		Resp(c).OK(gin.H{"ok": false, "message": "Cellular hardware is not available.", "response": ""})
		return
	}

	atPortMu.Lock()
	defer atPortMu.Unlock()

	ports := make([]string, 0, 5)
	if lastATPort != "" {
		ports = append(ports, lastATPort)
	}
	for _, p := range listATPortCandidates() {
		if p != lastATPort {
			ports = append(ports, p)
		}
	}
	if len(ports) == 0 {
		Resp(c).OK(gin.H{"ok": false, "message": "No AT port available (module busy or offline).", "response": ""})
		return
	}

	var (
		raw     string
		exchErr error
		port    string
	)
	for _, p := range ports {
		raw, exchErr = atExchangeWait(p, []string{cmd}, 800*time.Millisecond, 200*time.Millisecond)
		if exchErr != nil {
			continue
		}
		if strings.Contains(raw, "OK") || strings.Contains(raw, "ERROR") {
			port = p
			lastATPort = p
			break
		}
	}
	if port == "" {
		msg := "No AT port available (module busy or offline)."
		if exchErr != nil {
			msg = "AT exchange failed: " + exchErr.Error()
		}
		Resp(c).OK(gin.H{"ok": false, "message": msg, "response": strings.TrimSpace(raw)})
		return
	}
	Resp(c).OK(gin.H{
		"ok":       true,
		"message":  "OK",
		"response": strings.TrimSpace(raw),
	})
}

func (h *CellularHandler) DebugPing(c *gin.Context) {
	var req cellularPingRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidJSON, err.Error())
		return
	}
	host := strings.TrimSpace(req.Host)
	if host == "" {
		host = "8.8.8.8"
	}
	if !debugPingHostRe.MatchString(host) {
		Resp(c).OK(gin.H{"ok": false, "message": "Invalid host.", "output": "", "iface": ""})
		return
	}
	count := req.Count
	if count <= 0 {
		count = 4
	}
	if count > 10 {
		count = 10
	}

	iface, err := h.resolveCellularPingIface(strings.TrimSpace(req.Iface))
	if err != nil {
		Resp(c).OK(gin.H{"ok": false, "message": err.Error(), "output": "", "iface": ""})
		return
	}

	args := []string{"-c", fmt.Sprintf("%d", count), "-W", "3", "-I", iface, host}
	out, pingErr := h.command("ping", args...).CombinedOutput()
	output := strings.TrimSpace(string(out))
	if pingErr != nil {
		msg := "ping failed"
		if output == "" {
			msg = pingErr.Error()
		}
		Resp(c).OK(gin.H{"ok": false, "message": msg, "output": output, "iface": iface})
		return
	}
	Resp(c).OK(gin.H{"ok": true, "message": "OK", "output": output, "iface": iface})
}

// resolveCellularPingIface picks a cellular netdev that currently has an address.
// Never falls back to eth0 / the default route.
func (h *CellularHandler) resolveCellularPingIface(requested string) (string, error) {
	if h.hw == nil || len(h.hw.Net.Ifaces) == 0 {
		return "", fmt.Errorf("Cellular interface profile is not configured")
	}
	allowed := make(map[string]struct{}, len(h.hw.Net.Ifaces))
	for _, n := range h.hw.Net.Ifaces {
		allowed[n] = struct{}{}
	}
	if requested != "" {
		if !debugPingIfaceRe.MatchString(requested) {
			return "", fmt.Errorf("Invalid iface")
		}
		if _, ok := allowed[requested]; !ok {
			return "", fmt.Errorf("Iface is not a cellular interface")
		}
	}

	name, v4, v6 := h.findNetIface()
	if name == "" || (v4 == "" && v6 == "") {
		return "", fmt.Errorf("Cellular data session is not up")
	}
	if requested != "" && requested != name {
		return "", fmt.Errorf("Cellular data session is not up on %s", requested)
	}
	return name, nil
}

func sanitizeATCommand(raw string) (string, error) {
	cmd := strings.TrimSpace(raw)
	if cmd == "" {
		return "", fmt.Errorf("command is empty")
	}
	if len(cmd) > 256 {
		return "", fmt.Errorf("command too long")
	}
	if strings.ContainsAny(cmd, "\r\n") {
		return "", fmt.Errorf("only a single AT command is allowed")
	}
	if !strings.HasPrefix(strings.ToUpper(cmd), "AT") {
		return "", fmt.Errorf("command must start with AT")
	}
	return cmd, nil
}
