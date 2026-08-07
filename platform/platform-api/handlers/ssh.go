package handlers

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"strconv"
	"strings"

	eventLoggerPkg "aipc/platform/common/events"
	"github.com/gin-gonic/gin"
)

// SSHHandler handles SSH configuration management.
type SSHHandler struct {
	configPath       string
	eventLogger      *eventLoggerPkg.Logger
	validateConfigFn func(string) error
	restartServiceFn func() error
}

// NewSSHHandler creates a new SSHHandler.
func NewSSHHandler(eventLogger *eventLoggerPkg.Logger) *SSHHandler {
	return &SSHHandler{
		configPath:       "/etc/ssh/sshd_config",
		eventLogger:      eventLogger,
		validateConfigFn: validateSSHDConfig,
		restartServiceFn: restartSSHService,
	}
}

// SetEventLogger sets the event logger (for dependency injection)
func (h *SSHHandler) SetEventLogger(logger *eventLoggerPkg.Logger) {
	h.eventLogger = logger
}

// GetConfig reads and returns the current sshd configuration.
func (h *SSHHandler) GetConfig(c *gin.Context) {
	config, err := h.parseSSHConfig()
	if err != nil {
		Resp(c).FailMsg(CodeSSHConfigError, err.Error())
		return
	}
	Resp(c).OK(gin.H{"config": config})
}

// SetConfig updates sshd configuration and optionally restarts the service.
func (h *SSHHandler) SetConfig(c *gin.Context) {
	if !requireJSONContentType(c) {
		return
	}

	var req sshConfigRequest
	if err := decodeStrictJSONBody(c, &req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	updates, err := req.normalizedUpdates()
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}
	if len(updates) == 0 {
		Resp(c).FailMsg(CodeInvalidRequest, "No supported SSH configuration fields were provided")
		return
	}

	// Read current config
	content, err := os.ReadFile(h.configPath)
	if err != nil {
		Resp(c).FailMsg(CodeSSHConfigError, "Failed to read sshd_config: "+err.Error())
		return
	}

	lines := strings.Split(string(content), "\n")
	matchIndex := firstSSHMatchLineIndex(lines)

	// Apply updates to lines
	applied := map[string]bool{}
	for i, line := range lines {
		if matchIndex >= 0 && i >= matchIndex {
			break
		}
		trimmed := strings.TrimSpace(line)
		for key, val := range updates {
			if strings.HasPrefix(trimmed, key+" ") || strings.HasPrefix(trimmed, "#"+key+" ") || strings.HasPrefix(trimmed, "# "+key+" ") {
				lines[i] = key + " " + val
				applied[key] = true
				break
			}
		}
	}

	// Append any un-applied settings
	var appended []string
	for _, key := range sshDirectiveOrder {
		if !applied[key] {
			if val, ok := updates[key]; ok {
				appended = append(appended, key+" "+val)
			}
		}
	}
	if len(appended) > 0 {
		insertAt := len(lines)
		if matchIndex >= 0 {
			insertAt = matchIndex
		}
		lines = append(lines[:insertAt], append(appended, lines[insertAt:]...)...)
	}

	updatedContent := []byte(strings.Join(lines, "\n"))

	// Write back
	if err := os.WriteFile(h.configPath, updatedContent, 0644); err != nil {
		Resp(c).FailMsg(CodeSSHConfigError, "Failed to write sshd_config: "+err.Error())
		return
	}
	validator := h.validateConfigFn
	if validator == nil {
		validator = validateSSHDConfig
	}
	if err := validator(h.configPath); err != nil {
		_ = os.WriteFile(h.configPath, content, 0644)
		Resp(c).FailMsg(CodeSSHConfigError, "Invalid sshd_config after update; restored previous config: "+err.Error())
		return
	}

	readBack, err := h.parseSSHConfig()
	if err != nil {
		Resp(c).FailMsg(CodeSSHConfigError, "Failed to read back sshd_config: "+err.Error())
		return
	}
	for key, val := range updates {
		if readBack[key] != val {
			Resp(c).FailMsg(CodeSSHConfigError, fmt.Sprintf("SSH config read-back mismatch for %s: wrote %q, read %q", key, val, readBack[key]))
			return
		}
	}

	result := gin.H{"status": "updated", "changes": updates}

	// Restart sshd if requested
	restarted := false
	if req.RestartService {
		restarter := h.restartServiceFn
		if restarter == nil {
			restarter = restartSSHService
		}
		if err := restarter(); err != nil {
			Resp(c).FailMsg(CodeSSHServiceError, "SSH config updated but service restart failed: "+err.Error())
			return
		}
		result["restarted"] = true
		restarted = true
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"ssh.config.changed",
			eventLoggerPkg.MessageParams{
				"changes":   fmt.Sprint(updates),
				"restarted": restarted,
			},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(result)
}

type sshConfigRequest struct {
	Port                         string `json:"port"`
	PortDirective                string `json:"Port"`
	PermitRootLogin              string `json:"permit_root_login"`
	PermitRootLoginDirective     string `json:"PermitRootLogin"`
	PasswordAuth                 string `json:"password_authentication"`
	PasswordAuthDirective        string `json:"PasswordAuthentication"`
	PubkeyAuth                   string `json:"pubkey_authentication"`
	PubkeyAuthDirective          string `json:"PubkeyAuthentication"`
	MaxAuthTries                 string `json:"max_auth_tries"`
	MaxAuthTriesCamel            string `json:"maxAuthTries"`
	MaxAuthTriesDirective        string `json:"MaxAuthTries"`
	ClientAliveInterval          string `json:"client_alive_interval"`
	ClientAliveIntervalCamel     string `json:"clientAliveInterval"`
	ClientAliveIntervalDirective string `json:"ClientAliveInterval"`
	ClientAliveCountMax          string `json:"client_alive_count_max"`
	ClientAliveCountMaxCamel     string `json:"clientAliveCountMax"`
	ClientAliveCountMaxDirective string `json:"ClientAliveCountMax"`
	RestartService               bool   `json:"restart_service"`
}

var sshDirectiveOrder = []string{
	"Port",
	"PermitRootLogin",
	"PasswordAuthentication",
	"PubkeyAuthentication",
	"MaxAuthTries",
	"ClientAliveInterval",
	"ClientAliveCountMax",
}

func (r sshConfigRequest) normalizedUpdates() (map[string]string, error) {
	fields := []struct {
		key     string
		value   string
		aliases []string
	}{
		{"Port", r.Port, []string{r.PortDirective}},
		{"PermitRootLogin", r.PermitRootLogin, []string{r.PermitRootLoginDirective}},
		{"PasswordAuthentication", r.PasswordAuth, []string{r.PasswordAuthDirective}},
		{"PubkeyAuthentication", r.PubkeyAuth, []string{r.PubkeyAuthDirective}},
		{"MaxAuthTries", r.MaxAuthTries, []string{r.MaxAuthTriesCamel, r.MaxAuthTriesDirective}},
		{"ClientAliveInterval", r.ClientAliveInterval, []string{r.ClientAliveIntervalCamel, r.ClientAliveIntervalDirective}},
		{"ClientAliveCountMax", r.ClientAliveCountMax, []string{r.ClientAliveCountMaxCamel, r.ClientAliveCountMaxDirective}},
	}
	updates := make(map[string]string)
	for _, field := range fields {
		value, err := mergeSSHStringAliases(field.key, field.value, field.aliases...)
		if err != nil {
			return nil, err
		}
		if value == "" {
			continue
		}
		normalized, err := normalizeSSHDirectiveValue(field.key, value)
		if err != nil {
			return nil, err
		}
		updates[field.key] = normalized
	}
	return updates, nil
}

func firstSSHMatchLineIndex(lines []string) int {
	for i, line := range lines {
		fields := strings.Fields(strings.TrimSpace(line))
		if len(fields) > 0 && strings.EqualFold(fields[0], "Match") {
			return i
		}
	}
	return -1
}

func mergeSSHStringAliases(name, primary string, aliases ...string) (string, error) {
	value := strings.TrimSpace(primary)
	for _, alias := range aliases {
		alias = strings.TrimSpace(alias)
		if alias == "" {
			continue
		}
		if value != "" && value != alias {
			return "", fmt.Errorf("%s aliases disagree", name)
		}
		value = alias
	}
	return value, nil
}

func normalizeSSHDirectiveValue(key, value string) (string, error) {
	value = strings.TrimSpace(value)
	switch key {
	case "Port":
		n, err := parseSSHInt(key, value, 1, 65535)
		if err != nil {
			return "", err
		}
		return strconv.Itoa(n), nil
	case "MaxAuthTries":
		n, err := parseSSHInt(key, value, 1, 10)
		if err != nil {
			return "", err
		}
		return strconv.Itoa(n), nil
	case "ClientAliveInterval":
		n, err := parseSSHInt(key, value, 0, 86400)
		if err != nil {
			return "", err
		}
		return strconv.Itoa(n), nil
	case "ClientAliveCountMax":
		n, err := parseSSHInt(key, value, 0, 100)
		if err != nil {
			return "", err
		}
		return strconv.Itoa(n), nil
	case "PermitRootLogin":
		return normalizeSSHEnum(key, value, "yes", "no", "prohibit-password", "without-password", "forced-commands-only")
	case "PasswordAuthentication", "PubkeyAuthentication":
		return normalizeSSHEnum(key, value, "yes", "no")
	default:
		return "", fmt.Errorf("unsupported SSH directive %s", key)
	}
}

func parseSSHInt(name, value string, min, max int) (int, error) {
	n, err := strconv.Atoi(value)
	if err != nil || n < min || n > max {
		return 0, fmt.Errorf("%s must be an integer in [%d, %d]", name, min, max)
	}
	return n, nil
}

func normalizeSSHEnum(name, value string, allowed ...string) (string, error) {
	normalized := strings.ToLower(strings.TrimSpace(value))
	for _, candidate := range allowed {
		if normalized == candidate {
			return normalized, nil
		}
	}
	return "", fmt.Errorf("%s must be one of: %s", name, strings.Join(allowed, ", "))
}

func validateSSHDConfig(configPath string) error {
	sshd, err := exec.LookPath("sshd")
	if err != nil {
		return nil
	}
	cmd := exec.Command(sshd, "-t", "-f", configPath)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("%w: %s", err, strings.TrimSpace(string(out)))
	}
	return nil
}

func restartSSHService() error {
	var errs []string
	for _, unit := range []string{"sshd", "ssh"} {
		if err := exec.Command("systemctl", "restart", unit).Run(); err == nil {
			return nil
		} else {
			errs = append(errs, unit+": "+err.Error())
		}
	}
	return errors.New(strings.Join(errs, "; "))
}

// GetStatus returns the sshd service status.
func (h *SSHHandler) GetStatus(c *gin.Context) {
	out, err := exec.Command("systemctl", "is-active", "sshd").Output()
	status := strings.TrimSpace(string(out))
	if err != nil {
		// Try "ssh" service name (Debian/Ubuntu)
		out, err = exec.Command("systemctl", "is-active", "ssh").Output()
		status = strings.TrimSpace(string(out))
		if err != nil {
			status = "unknown"
		}
	}

	Resp(c).OK(gin.H{"status": status})
}

// GetLogs returns recent SSH login log entries.
func (h *SSHHandler) GetLogs(c *gin.Context) {
	// Try journalctl first
	out, err := exec.Command("journalctl", "-u", "sshd", "--no-pager", "-n", "50", "--output=short").Output()
	if err != nil {
		out, err = exec.Command("journalctl", "-u", "ssh", "--no-pager", "-n", "50", "--output=short").Output()
	}

	var logs []string
	if err == nil {
		lines := strings.Split(string(out), "\n")
		for _, line := range lines {
			if strings.TrimSpace(line) != "" {
				logs = append(logs, line)
			}
		}
	} else {
		// Fallback: read /var/log/auth.log
		f, err := os.Open("/var/log/auth.log")
		if err != nil {
			Resp(c).OK(gin.H{"logs": []string{}, "error": "Unable to read SSH logs"})
			return
		}
		defer f.Close()

		scanner := bufio.NewScanner(f)
		var allLines []string
		for scanner.Scan() {
			line := scanner.Text()
			if strings.Contains(line, "sshd") {
				allLines = append(allLines, line)
			}
		}
		// Last 50 lines
		if len(allLines) > 50 {
			allLines = allLines[len(allLines)-50:]
		}
		logs = allLines
	}

	Resp(c).OK(gin.H{"logs": logs})
}

// parseSSHConfig reads and parses sshd_config into a map.
func (h *SSHHandler) parseSSHConfig() (map[string]string, error) {
	f, err := os.Open(h.configPath)
	if err != nil {
		return nil, fmt.Errorf("cannot open %s: %w", h.configPath, err)
	}
	defer f.Close()

	config := make(map[string]string)
	scanner := bufio.NewScanner(f)
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		fields := strings.Fields(line)
		if len(fields) > 0 && strings.EqualFold(fields[0], "Match") {
			break
		}
		parts := strings.SplitN(line, " ", 2)
		if len(parts) == 2 {
			config[parts[0]] = strings.TrimSpace(parts[1])
		}
	}
	return config, scanner.Err()
}
