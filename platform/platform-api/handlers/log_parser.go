package handlers

import (
	"bufio"
	"bytes"
	"fmt"
	"os/exec"
	"regexp"
	"strconv"
	"strings"
	"time"
)

// LogLevel represents the severity level of a log entry
type LogLevel string

const (
	LogLevelFatal   LogLevel = "fatal"
	LogLevelError   LogLevel = "error"
	LogLevelWarning LogLevel = "warning"
	LogLevelInfo    LogLevel = "info"
	LogLevelDebug   LogLevel = "debug"
)

// LogCategory represents the type/category of a log entry
type LogCategory string

const (
	CategoryOperation LogCategory = "operation"
	CategorySecurity  LogCategory = "security"
	CategoryAlarm     LogCategory = "alarm"
	CategorySystem    LogCategory = "system"
)

// LogEntry represents a structured log entry
type LogEntry struct {
	ID        string                 `json:"id"`
	Timestamp time.Time              `json:"timestamp"`
	Level     LogLevel               `json:"level"`
	Category  LogCategory            `json:"category"`
	Source    string                 `json:"source"`
	TitleKey  string                 `json:"title_key"`
	Title     string                 `json:"title"`
	Message   string                 `json:"message"`
	Raw       string                 `json:"raw,omitempty"`
	Metadata  map[string]interface{} `json:"metadata,omitempty"`
}

// LogMetadata contains additional parsed information
type LogMetadata struct {
	ProcessID  int    `json:"process_id,omitempty"`
	Unit       string `json:"unit,omitempty"`
	BootID     string `json:"boot_id,omitempty"`
	MachineID  string `json:"machine_id,omitempty"`
	Hostname   string `json:"hostname,omitempty"`
	LineNumber int    `json:"line_number,omitempty"`
}

// LogParser parses and structures log entries
type LogParser struct {
	patterns []LogPattern
}

// LogPattern defines a pattern for matching and parsing log entries
type LogPattern struct {
	Regex       *regexp.Regexp
	Category    LogCategory
	Level       LogLevel
	TitleKey    string
	MessageTmpl string
	Priority    int // Higher priority patterns are checked first
}

// NewLogParser creates a new log parser with default patterns
func NewLogParser() *LogParser {
	p := &LogParser{
		patterns: make([]LogPattern, 0),
	}
	p.initDefaultPatterns()
	return p
}

// initDefaultPatterns initializes common log patterns
func (p *LogParser) initDefaultPatterns() {
	// High priority patterns first

	// === AI Runtime Patterns ===
	p.addPattern(
		regexp.MustCompile(`(?i)model.*failed|model.*error|load.*model.*failed`),
		CategoryAlarm, LogLevelError,
		"logs.messages.model_load_failed",
		"", PriorityHigh,
	)
	p.addPattern(
		regexp.MustCompile(`(?i)inference.*failed|inference.*error`),
		CategoryAlarm, LogLevelError,
		"logs.messages.inference_failed",
		"", PriorityHigh,
	)
	p.addPattern(
		regexp.MustCompile(`(?i)model.*loaded|inference.*started`),
		CategorySystem, LogLevelInfo,
		"logs.messages.service_started",
		"", PriorityNormal,
	)

	// === Storage Patterns ===
	p.addPattern(
		regexp.MustCompile(`(?i)disk.*full|no.*space.*left|storage.*full`),
		CategoryAlarm, LogLevelError,
		"logs.messages.storage_full",
		"", PriorityCritical,
	)
	p.addPattern(
		regexp.MustCompile(`(?i)low.*disk|low.*storage|warning.*space`),
		CategoryAlarm, LogLevelWarning,
		"logs.messages.storage_low",
		"", PriorityHigh,
	)

	// === Network Patterns ===
	p.addPattern(
		regexp.MustCompile(`(?i)network.*unavailable|connection.*lost|network.*down`),
		CategoryAlarm, LogLevelError,
		"logs.messages.network_down",
		"", PriorityHigh,
	)
	p.addPattern(
		regexp.MustCompile(`(?i)ntp.*sync.*failed|time.*sync.*failed`),
		CategorySystem, LogLevelWarning,
		"logs.messages.ntp_sync_failed",
		"", PriorityNormal,
	)
	p.addPattern(
		regexp.MustCompile(`(?i)ntp.*sync.*success|time.*sync.*success`),
		CategorySystem, LogLevelInfo,
		"logs.messages.ntp_sync_success",
		"", PriorityNormal,
	)

	// === Security/SSH Patterns ===
	p.addPattern(
		regexp.MustCompile(`(?i)accepted.*password|session.*opened|login.*success`),
		CategoryOperation, LogLevelInfo,
		"logs.messages.login_success",
		"", PriorityNormal,
	)
	p.addPattern(
		regexp.MustCompile(`(?i)failed.*password|authentication.*fail|login.*fail`),
		CategorySecurity, LogLevelWarning,
		"logs.messages.login_failed",
		"", PriorityHigh,
	)
	p.addPattern(
		regexp.MustCompile(`(?i)invalid.*user|illegal.*user`),
		CategorySecurity, LogLevelWarning,
		"logs.messages.invalid_user",
		"", PriorityNormal,
	)

	// === Service Status Patterns ===
	p.addPattern(
		regexp.MustCompile(`(?i)started.*service|Starting.*service`),
		CategorySystem, LogLevelInfo,
		"logs.messages.service_started",
		"", PriorityNormal,
	)
	p.addPattern(
		regexp.MustCompile(`(?i)stopped.*service|Stopping.*service`),
		CategorySystem, LogLevelInfo,
		"logs.messages.service_stopped",
		"", PriorityNormal,
	)
	p.addPattern(
		regexp.MustCompile(`(?i)service.*failed|service.*crashed|service.*exited`),
		CategoryAlarm, LogLevelError,
		"logs.messages.service_failed",
		"", PriorityCritical,
	)

	// === Configuration Patterns ===
	p.addPattern(
		regexp.MustCompile(`(?i)config.*changed|configuration.*updated|settings.*saved`),
		CategoryOperation, LogLevelInfo,
		"logs.messages.config_changed",
		"", PriorityNormal,
	)

	// === Firmware/Update Patterns ===
	p.addPattern(
		regexp.MustCompile(`(?i)firmware.*updated|update.*success|upgrade.*complete`),
		CategorySystem, LogLevelInfo,
		"logs.messages.firmware_updated",
		"", PriorityNormal,
	)
	p.addPattern(
		regexp.MustCompile(`(?i)firmware.*failed|update.*failed|upgrade.*failed`),
		CategoryAlarm, LogLevelError,
		"logs.messages.firmware_update_failed",
		"", PriorityHigh,
	)

	// === Temperature Patterns ===
	p.addPattern(
		regexp.MustCompile(`(?i)temperature.*critical|overheating|temp.*too.*high`),
		CategoryAlarm, LogLevelError,
		"logs.messages.temperature_critical",
		"", PriorityCritical,
	)
	p.addPattern(
		regexp.MustCompile(`(?i)temperature.*high|temp.*warning`),
		CategoryAlarm, LogLevelWarning,
		"logs.messages.temperature_high",
		"", PriorityNormal,
	)
}

const (
	PriorityCritical = 100
	PriorityHigh     = 50
	PriorityNormal   = 10
	PriorityLow      = 5
)

// addPattern adds a new pattern to the parser
func (p *LogParser) addPattern(regex *regexp.Regexp, category LogCategory, level LogLevel, titleKey, messageTmpl string, priority int) {
	p.patterns = append(p.patterns, LogPattern{
		Regex:       regex,
		Category:    category,
		Level:       level,
		TitleKey:    titleKey,
		MessageTmpl: messageTmpl,
		Priority:    priority,
	})
}

// ParseLine parses a single log line and returns a structured entry
func (p *LogParser) ParseLine(line, source string) *LogEntry {
	if strings.TrimSpace(line) == "" {
		return nil
	}

	// Try to parse journalctl format first
	entry := p.parseJournalctlLine(line, source)
	if entry != nil {
		return p.enrichWithPatterns(entry, line)
	}

	// Fallback to generic parsing
	entry = &LogEntry{
		ID:        generateLogID(),
		Timestamp: time.Now(),
		Level:     LogLevelInfo,
		Category:  CategorySystem,
		Source:    source,
		Raw:       line,
		Title:     "System Log",
		Message:   truncateString(line, 200),
		Metadata:  make(map[string]interface{}),
	}

	return p.enrichWithPatterns(entry, line)
}

// parseJournalctlLine attempts to parse a journalctl formatted line
func (p *LogParser) parseJournalctlLine(line, source string) *LogEntry {
	// Journalctl format example:
	// Apr 03 14:23:15 hostname systemd[1]: Started Example Service.
	// Or with boot ID:
	// BOOT_ID=... hostname service[pid]: message

	// Simple regex for timestamp + hostname + rest
	re := regexp.MustCompile(`^([A-Z][a-z]{2}\s+\d{1,2}\s+\d{2}:\d{2}:\d{2})\s+(\S+)\s+(.+)`)
	matches := re.FindStringSubmatch(line)

	if len(matches) < 4 {
		// Try alternative format (with process info)
		reAlt := regexp.MustCompile(`^(\S+)\s+(\S+)\s+(\S+)\[(\d+)\]:\s+(.+)`)
		matchesAlt := reAlt.FindStringSubmatch(line)
		if len(matchesAlt) >= 6 {
			timestamp, err := p.parseTimestamp(matchesAlt[1])
			if err != nil {
				return nil
			}

			return &LogEntry{
				ID:        generateLogID(),
				Timestamp: timestamp,
				Level:     p.detectLevel(matchesAlt[5]),
				Category:  CategorySystem, // Will be enriched
				Source:    source,
				Raw:       line,
				Title:     "System Event",
				Message:   matchesAlt[5],
				Metadata: map[string]interface{}{
					"hostname":   matchesAlt[2],
					"process":    matchesAlt[3],
					"process_id": matchesAlt[4],
				},
			}
		}
		return nil
	}

	// Parse timestamp
	timestamp, err := p.parseTimestamp(matches[1])
	if err != nil {
		return nil
	}

	rest := matches[3]
	message := rest

	// Try to extract process info
	process := ""
	processID := ""

	reProc := regexp.MustCompile(`^(\S+)\[(\d+)\]:\s+(.+)`)
	procMatches := reProc.FindStringSubmatch(rest)
	if len(procMatches) >= 4 {
		process = procMatches[1]
		processID = procMatches[2]
		message = procMatches[3]
	}

	return &LogEntry{
		ID:        generateLogID(),
		Timestamp: timestamp,
		Level:     p.detectLevel(message),
		Category:  CategorySystem, // Will be enriched by patterns
		Source:    source,
		Raw:       line,
		Title:     "System Event",
		Message:   message,
		Metadata: map[string]interface{}{
			"hostname":   matches[2],
			"process":    process,
			"process_id": processID,
		},
	}
}

// enrichWithPatterns applies pattern matching to enrich the entry
func (p *LogParser) enrichWithPatterns(entry *LogEntry, rawLine string) *LogEntry {
	// Sort patterns by priority (highest first)
	sortedPatterns := make([]LogPattern, len(p.patterns))
	copy(sortedPatterns, p.patterns)

	// Simple bubble sort by priority (could be optimized)
	for i := 0; i < len(sortedPatterns)-1; i++ {
		for j := 0; j < len(sortedPatterns)-i-1; j++ {
			if sortedPatterns[j].Priority < sortedPatterns[j+1].Priority {
				sortedPatterns[j], sortedPatterns[j+1] = sortedPatterns[j+1], sortedPatterns[j]
			}
		}
	}

	// Try patterns in priority order
	for _, pattern := range sortedPatterns {
		if pattern.Regex.MatchString(rawLine) {
			entry.Category = pattern.Category
			if entry.Level == LogLevelInfo {
				// Only override level if not already set to error/warning
				entry.Level = pattern.Level
			}
			entry.TitleKey = pattern.TitleKey
			// Title will be translated on frontend
			entry.Title = pattern.TitleKey
			if pattern.MessageTmpl != "" {
				entry.Message = pattern.MessageTmpl
			}
			break
		}
	}

	return entry
}

// detectLevel attempts to detect log level from message content
func (p *LogParser) detectLevel(message string) LogLevel {
	msgLower := strings.ToLower(message)

	switch {
	case strings.Contains(msgLower, "error") || strings.Contains(msgLower, "fail") ||
		strings.Contains(msgLower, "critical") || strings.Contains(msgLower, "fatal"):
		return LogLevelError
	case strings.Contains(msgLower, "warn") || strings.Contains(msgLower, "alert"):
		return LogLevelWarning
	case strings.Contains(msgLower, "debug") || strings.Contains(msgLower, "trace"):
		return LogLevelDebug
	default:
		return LogLevelInfo
	}
}

// parseTimestamp parses various timestamp formats
func (p *LogParser) parseTimestamp(ts string) (time.Time, error) {
	// Try journalctl format: "Apr 03 14:23:15"
	currentYear := time.Now().Year()
	formats := []string{
		"2006-01-02 15:04:05",
		"2006-01-02T15:04:05",
		"2006-01-02 15:04:05.000",
	}

	// Try "Jan 02 15:04:05" format (add current year)
	re := regexp.MustCompile(`^([A-Z][a-z]{2})\s+(\d{1,2})\s+(\d{2}:\d{2}:\d{2})`)
	matches := re.FindStringSubmatch(ts)
	if len(matches) == 4 {
		// Map month name to number
		months := map[string]string{
			"Jan": "01", "Feb": "02", "Mar": "03", "Apr": "04",
			"May": "05", "Jun": "06", "Jul": "07", "Aug": "08",
			"Sep": "09", "Oct": "10", "Nov": "11", "Dec": "12",
		}
		monthNum := months[matches[1]]
		day := matches[2]
		dayNum, _ := strconv.Atoi(day)
		timePart := matches[3]
		ts = fmt.Sprintf("%d-%s-%02d %s", currentYear, monthNum, dayNum, timePart)
	}

	for _, format := range formats {
		t, err := time.Parse(format, ts)
		if err == nil {
			return t, nil
		}
	}

	// If all else fails, return current time
	return time.Now(), nil
}

// generateLogID generates a unique ID for a log entry
func generateLogID() string {
	return fmt.Sprintf("log-%d", time.Now().UnixNano())
}

// truncateString truncates a string to a maximum length
func truncateString(s string, maxLen int) string {
	if len(s) <= maxLen {
		return s
	}
	return s[:maxLen] + "..."
}

// ParseServiceLogs parses logs from a systemd service
func (p *LogParser) ParseServiceLogs(service string, lines int) ([]LogEntry, error) {
	cmd := exec.Command("journalctl", "-u", service, "-n", strconv.Itoa(lines), "--no-pager")
	output, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("failed to read service logs: %w", err)
	}

	return p.parseLogLines(string(output), service), nil
}

// ParseLogFile parses logs from a file
func (p *LogParser) ParseLogFile(filepath string, lines int) ([]LogEntry, error) {
	cmd := exec.Command("tail", "-n", strconv.Itoa(lines), filepath)
	output, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("failed to read log file: %w", err)
	}

	return p.parseLogLines(string(output), filepath), nil
}

// parseLogLines parses multiple log lines
func (p *LogParser) parseLogLines(content, source string) []LogEntry {
	entries := make([]LogEntry, 0)
	scanner := bufio.NewScanner(bytes.NewBufferString(content))

	for scanner.Scan() {
		line := scanner.Text()
		if strings.TrimSpace(line) == "" {
			continue
		}
		entry := p.ParseLine(line, source)
		if entry != nil {
			entries = append(entries, *entry)
		}
	}

	return entries
}

// GetLogStatistics returns statistics about log entries
type LogStatistics struct {
	TodayErrors    int `json:"today_errors"`
	TodayWarnings  int `json:"today_warnings"`
	Operations     int `json:"operations"`
	SecurityEvents int `json:"security_events"`
	AlarmEvents    int `json:"alarm_events"`
	SystemEvents   int `json:"system_events"`
	TotalEntries   int `json:"total_entries"`
}

// CalculateStatistics calculates statistics from log entries
func CalculateStatistics(entries []LogEntry) *LogStatistics {
	stats := &LogStatistics{}
	now := time.Now()
	today := time.Date(now.Year(), now.Month(), now.Day(), 0, 0, 0, 0, now.Location())

	for _, entry := range entries {
		stats.TotalEntries++

		// Count today's errors and warnings
		if entry.Timestamp.After(today) || entry.Timestamp.Equal(today) {
			if entry.Level == LogLevelError || entry.Level == LogLevelFatal {
				stats.TodayErrors++
			} else if entry.Level == LogLevelWarning {
				stats.TodayWarnings++
			}
		}

		// Count by category
		switch entry.Category {
		case CategoryOperation:
			stats.Operations++
		case CategorySecurity:
			stats.SecurityEvents++
		case CategoryAlarm:
			stats.AlarmEvents++
		case CategorySystem:
			stats.SystemEvents++
		}
	}

	return stats
}

// FilterOptions defines filtering options for log entries
type FilterOptions struct {
	Category  LogCategory `json:"category,omitempty"`
	Level     LogLevel    `json:"level,omitempty"`
	StartTime string      `json:"start_time,omitempty"`
	EndTime   string      `json:"end_time,omitempty"`
	Search    string      `json:"search,omitempty"`
	Limit     int         `json:"limit,omitempty"`
	Offset    int         `json:"offset,omitempty"`
}

// FilterEntries filters log entries based on the given options
func FilterEntries(entries []LogEntry, opts FilterOptions) []LogEntry {
	result := make([]LogEntry, 0)

	for _, entry := range entries {
		// Filter by category
		if opts.Category != "" && entry.Category != opts.Category {
			continue
		}

		// Filter by level
		if opts.Level != "" && entry.Level != opts.Level {
			continue
		}

		// Filter by time range
		if opts.StartTime != "" {
			startTime, _ := time.Parse(time.RFC3339, opts.StartTime)
			if entry.Timestamp.Before(startTime) {
				continue
			}
		}
		if opts.EndTime != "" {
			endTime, _ := time.Parse(time.RFC3339, opts.EndTime)
			if entry.Timestamp.After(endTime) {
				continue
			}
		}

		// Filter by search term
		if opts.Search != "" {
			searchLower := strings.ToLower(opts.Search)
			if !strings.Contains(strings.ToLower(entry.Message), searchLower) &&
				!strings.Contains(strings.ToLower(entry.Raw), searchLower) {
				continue
			}
		}

		result = append(result, entry)
	}

	// Apply offset and limit
	if opts.Offset > 0 {
		if opts.Offset >= len(result) {
			return []LogEntry{}
		}
		result = result[opts.Offset:]
	}

	if opts.Limit > 0 && opts.Limit < len(result) {
		result = result[:opts.Limit]
	}

	return result
}
