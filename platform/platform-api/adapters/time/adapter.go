// Package time is the Config Controller adapter for the time domain. It owns
// the three files a time-config change touches on disk:
//
//   - /etc/timezone (the IANA zone name, e.g. "Asia/Shanghai")
//   - /etc/systemd/timesyncd.conf (NTP=server + PollInterval{Min,Max}Sec)
//   - <config>/time-config.json (user prefs not managed by systemd:
//     time_format, sync_mode, ntp_interval)
//
// The adapter is MULTI-KEY: the key selects which projection to run, so the
// individual setters (SetTimezone → "timezone", SetNTPConfig → "ntp") do not
// clobber files they do not own. The all-in-one SaveTimeConfig handler uses
// key "config", which renders every file the full blob touches in one job.
//
// The adapter only projects files. The live reconfiguration — `timedatectl
// set-time` / `set-timezone`, `systemctl enable|restart systemd-timesyncd`,
// NTP-state verification — stays in the handler post-Apply, exactly as the
// network adapter keeps `asyncRestartNetwork` in the handler. timedatectl does
// not drop the HTTP connection the way a network reload would, but keeping the
// exec with the handler preserves the existing orchestration (NTP-state verify
// loops, conflicting-provider teardown) verbatim and avoids re-implementing it
// inside the state machine. The Manager still records the revision + audit and
// auto-Restores file state on a Verify failure.
//
// SetSystemTime / SyncNTP / SyncFromClient are pure `timedatectl set-time`
// execs with no file projection — they are not routed through the Manager (no
// desired-state file to record). last-known-time.json is a periodic recovery
// snapshot, not user config, and is also left untouched.
package time

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"

	"aipc/platform/common/constants"
	"aipc/platform/platform-api/internal/atomicfile"
)

// timesyncd requires Max > Min and starts polling at Min; writing the same
// value to both fields delays or invalidates synchronization after boot.
const (
	timesyncdPollMinSec  = 32
	defaultNTPPollMaxSec = 3600
)

// keys. The adapter dispatches on key; an unknown key fails Validate fast.
const (
	keyTimezone   = "timezone"
	keyNTP        = "ntp"
	keyUserConfig = "user_config"
	keyConfig     = "config"
)

// --- desired shapes (one per key) ---------------------------------------

type desiredTimezone struct {
	Timezone string `json:"timezone"`
}

type desiredNTP struct {
	Enabled  bool   `json:"enabled"`
	Server   string `json:"server"`
	Interval int    `json:"interval"`
}

type desiredUserConfig struct {
	TimeFormat  string `json:"time_format"`
	SyncMode    string `json:"sync_mode"`
	NTPInterval int    `json:"ntp_interval"`
}

// desiredConfig mirrors handlers.SaveTimeConfigRequest for the all-in-one key.
type desiredConfig struct {
	Timezone       string `json:"timezone"`
	TimeFormat     string `json:"time_format"`
	SyncMode       string `json:"sync_mode"`
	NTPServer      string `json:"ntp_server"`
	NTPInterval    int    `json:"ntp_interval"`
	ManualDatetime string `json:"manual_datetime"`
}

var (
	// ErrInvalidJSON is returned when desiredJSON does not unmarshal.
	ErrInvalidJSON = errors.New("time: invalid desired json")
	// ErrUnknownKey is returned when the key is not one of timezone/ntp/
	// user_config/config.
	ErrUnknownKey = errors.New("time: unknown key")
	// ErrMissingTimezone is returned when the timezone field is empty.
	ErrMissingTimezone = errors.New("time: missing timezone")
	// ErrBadInterval is returned when the NTP interval is negative.
	ErrBadInterval = errors.New("time: invalid ntp interval")
)

// backupState / rendered hold per-file bytes. A nil slice means "do not touch
// this file" (in rendered) or "file did not exist pre-apply" (in backupState,
// so Restore removes the file Apply created). The three slices are independent:
// a "timezone" apply only ever sets timezoneBytes.
type backupState struct {
	timezoneBytes   []byte
	timesyncdBytes  []byte
	userConfigBytes []byte
}

type rendered struct {
	timezoneBytes   []byte
	timesyncdBytes  []byte
	userConfigBytes []byte
}

// Adapter applies time configuration to the live filesystem. One instance is
// registered with the Config Manager under the "time" domain; the per-apply
// key selects the projection.
type Adapter struct {
	timezonePath   string
	timesyncdPath  string
	userConfigPath string
}

// New returns a time Adapter. Empty path arguments fall back to the canonical
// defaults so tests can override only the paths they care about (or all three
// via t.TempDir()).
func New(timezonePath, timesyncdPath, userConfigPath string) *Adapter {
	if timezonePath == "" {
		timezonePath = "/etc/timezone"
	}
	if timesyncdPath == "" {
		timesyncdPath = "/etc/systemd/timesyncd.conf"
	}
	if userConfigPath == "" {
		userConfigPath = constants.ConfigPath() + "/time-config.json"
	}
	return &Adapter{
		timezonePath:   timezonePath,
		timesyncdPath:  timesyncdPath,
		userConfigPath: userConfigPath,
	}
}

// Validate parses desiredJSON for the key and runs cheap field checks. It does
// not touch the filesystem; full timezone validity (against the system zone
// list) stays in the handler, which has the cached timedatectl list-timezones.
func (a *Adapter) Validate(_ context.Context, key, desiredJSON string) error {
	switch key {
	case keyTimezone:
		var d desiredTimezone
		if err := json.Unmarshal([]byte(desiredJSON), &d); err != nil {
			return fmt.Errorf("%w: %v", ErrInvalidJSON, err)
		}
		if d.Timezone == "" {
			return ErrMissingTimezone
		}
	case keyNTP:
		var d desiredNTP
		if err := json.Unmarshal([]byte(desiredJSON), &d); err != nil {
			return fmt.Errorf("%w: %v", ErrInvalidJSON, err)
		}
		if d.Interval < 0 {
			return ErrBadInterval
		}
	case keyUserConfig:
		var d desiredUserConfig
		if err := json.Unmarshal([]byte(desiredJSON), &d); err != nil {
			return fmt.Errorf("%w: %v", ErrInvalidJSON, err)
		}
		if d.NTPInterval < 0 {
			return ErrBadInterval
		}
	case keyConfig:
		var d desiredConfig
		if err := json.Unmarshal([]byte(desiredJSON), &d); err != nil {
			return fmt.Errorf("%w: %v", ErrInvalidJSON, err)
		}
		if d.Timezone == "" {
			return ErrMissingTimezone
		}
		if d.NTPInterval < 0 {
			return ErrBadInterval
		}
	default:
		return fmt.Errorf("%w: %s", ErrUnknownKey, key)
	}
	return nil
}

// Backup reads the file(s) the key will touch. Missing files are not errors;
// the corresponding slice stays nil so Restore removes the file Apply created.
func (a *Adapter) Backup(_ context.Context, key string) (any, error) {
	bs := backupState{}
	switch key {
	case keyTimezone:
		b, err := readMaybeMissing(a.timezonePath)
		if err != nil {
			return nil, fmt.Errorf("backup /etc/timezone: %w", err)
		}
		bs.timezoneBytes = b
	case keyNTP:
		b, err := readMaybeMissing(a.timesyncdPath)
		if err != nil {
			return nil, fmt.Errorf("backup timesyncd.conf: %w", err)
		}
		bs.timesyncdBytes = b
	case keyUserConfig:
		b, err := readMaybeMissing(a.userConfigPath)
		if err != nil {
			return nil, fmt.Errorf("backup time-config.json: %w", err)
		}
		bs.userConfigBytes = b
	case keyConfig:
		tb, err := readMaybeMissing(a.timezonePath)
		if err != nil {
			return nil, fmt.Errorf("backup /etc/timezone: %w", err)
		}
		xb, err := readMaybeMissing(a.timesyncdPath)
		if err != nil {
			return nil, fmt.Errorf("backup timesyncd.conf: %w", err)
		}
		ub, err := readMaybeMissing(a.userConfigPath)
		if err != nil {
			return nil, fmt.Errorf("backup time-config.json: %w", err)
		}
		bs = backupState{timezoneBytes: tb, timesyncdBytes: xb, userConfigBytes: ub}
	default:
		return nil, fmt.Errorf("%w: %s", ErrUnknownKey, key)
	}
	return bs, nil
}

// Render produces the per-file bytes for the key. A nil slice in the result
// means Apply must not touch that file (e.g. NTP-disabled does not rewrite
// timesyncd.conf, matching the handler's disableNTPProviders path).
func (a *Adapter) Render(_ context.Context, key, desiredJSON string) (any, error) {
	switch key {
	case keyTimezone:
		var d desiredTimezone
		if err := json.Unmarshal([]byte(desiredJSON), &d); err != nil {
			return nil, fmt.Errorf("%w: %v", ErrInvalidJSON, err)
		}
		return rendered{timezoneBytes: []byte(d.Timezone + "\n")}, nil
	case keyNTP:
		var d desiredNTP
		if err := json.Unmarshal([]byte(desiredJSON), &d); err != nil {
			return nil, fmt.Errorf("%w: %v", ErrInvalidJSON, err)
		}
		// NTP-disabled does not rewrite timesyncd.conf; the handler tears the
		// provider down. Only enabled re-projects server + poll interval.
		if !d.Enabled {
			return rendered{}, nil
		}
		existing, err := readMaybeMissing(a.timesyncdPath)
		if err != nil {
			return nil, fmt.Errorf("render timesyncd.conf: %w", err)
		}
		updated, err := updateTimesyncdConfig(existing, d.Server, d.Interval)
		if err != nil {
			return nil, err
		}
		return rendered{timesyncdBytes: updated}, nil
	case keyUserConfig:
		var d desiredUserConfig
		if err := json.Unmarshal([]byte(desiredJSON), &d); err != nil {
			return nil, fmt.Errorf("%w: %v", ErrInvalidJSON, err)
		}
		data, err := json.MarshalIndent(d, "", "  ")
		if err != nil {
			return nil, fmt.Errorf("render time-config.json: %w", err)
		}
		return rendered{userConfigBytes: data}, nil
	case keyConfig:
		var d desiredConfig
		if err := json.Unmarshal([]byte(desiredJSON), &d); err != nil {
			return nil, fmt.Errorf("%w: %v", ErrInvalidJSON, err)
		}
		r := rendered{}
		if d.Timezone != "" {
			r.timezoneBytes = []byte(d.Timezone + "\n")
		}
		// sync_mode == "manual" disables NTP → do not re-project timesyncd.conf.
		if d.SyncMode != "manual" {
			existing, err := readMaybeMissing(a.timesyncdPath)
			if err != nil {
				return nil, fmt.Errorf("render timesyncd.conf: %w", err)
			}
			server := d.NTPServer
			interval := d.NTPInterval
			if interval <= 0 {
				interval = defaultNTPPollMaxSec
			}
			updated, err := updateTimesyncdConfig(existing, server, interval)
			if err != nil {
				return nil, err
			}
			r.timesyncdBytes = updated
		}
		uc := desiredUserConfig{
			TimeFormat:  d.TimeFormat,
			SyncMode:    d.SyncMode,
			NTPInterval: d.NTPInterval,
		}
		data, err := json.MarshalIndent(uc, "", "  ")
		if err != nil {
			return nil, fmt.Errorf("render time-config.json: %w", err)
		}
		r.userConfigBytes = data
		return r, nil
	default:
		return nil, fmt.Errorf("%w: %s", ErrUnknownKey, key)
	}
}

// Apply atomically writes every file the rendered value carries. Parent dirs
// are created if missing (timesyncd.conf lives under /etc/systemd which may
// not exist on a minimal image).
func (a *Adapter) Apply(_ context.Context, _ string, renderedVal any) error {
	r, ok := renderedVal.(rendered)
	if !ok {
		return fmt.Errorf("time: rendered payload is not time.rendered")
	}
	if r.timezoneBytes != nil {
		if err := writeFile(a.timezonePath, r.timezoneBytes); err != nil {
			return err
		}
	}
	if r.timesyncdBytes != nil {
		if err := writeFile(a.timesyncdPath, r.timesyncdBytes); err != nil {
			return err
		}
	}
	if r.userConfigBytes != nil {
		if err := writeFile(a.userConfigPath, r.userConfigBytes); err != nil {
			return err
		}
	}
	return nil
}

// Verify re-renders the desired bytes for the touched files and compares them
// to what landed on disk. Files the apply did not touch are skipped.
func (a *Adapter) Verify(_ context.Context, key, desiredJSON string) error {
	r, err := a.Render(context.Background(), key, desiredJSON)
	if err != nil {
		return err
	}
	rd := r.(rendered)
	if rd.timezoneBytes != nil {
		got, err := os.ReadFile(a.timezonePath)
		if err != nil {
			return fmt.Errorf("verify: read /etc/timezone: %w", err)
		}
		if string(got) != string(rd.timezoneBytes) {
			return fmt.Errorf("verify: /etc/timezone content mismatch")
		}
	}
	if rd.timesyncdBytes != nil {
		got, err := os.ReadFile(a.timesyncdPath)
		if err != nil {
			return fmt.Errorf("verify: read timesyncd.conf: %w", err)
		}
		if string(got) != string(rd.timesyncdBytes) {
			return fmt.Errorf("verify: timesyncd.conf content mismatch")
		}
	}
	if rd.userConfigBytes != nil {
		got, err := os.ReadFile(a.userConfigPath)
		if err != nil {
			return fmt.Errorf("verify: read time-config.json: %w", err)
		}
		if string(got) != string(rd.userConfigBytes) {
			return fmt.Errorf("verify: time-config.json content mismatch")
		}
	}
	return nil
}

// Restore writes the pre-apply bytes back (or removes the file if none existed)
// for every file the key's Backup captured.
func (a *Adapter) Restore(_ context.Context, key string, backup any) error {
	bs, ok := backup.(backupState)
	if !ok {
		return fmt.Errorf("time: backup payload is not time.backupState")
	}
	restoreOne := func(path string, b []byte) error {
		if b != nil {
			if err := writeFile(path, b); err != nil {
				return err
			}
		} else {
			_ = os.Remove(path)
		}
		return nil
	}
	switch key {
	case keyTimezone:
		return restoreOne(a.timezonePath, bs.timezoneBytes)
	case keyNTP:
		return restoreOne(a.timesyncdPath, bs.timesyncdBytes)
	case keyUserConfig:
		return restoreOne(a.userConfigPath, bs.userConfigBytes)
	case keyConfig:
		if err := restoreOne(a.timezonePath, bs.timezoneBytes); err != nil {
			return err
		}
		if err := restoreOne(a.timesyncdPath, bs.timesyncdBytes); err != nil {
			return err
		}
		return restoreOne(a.userConfigPath, bs.userConfigBytes)
	default:
		return fmt.Errorf("%w: %s", ErrUnknownKey, key)
	}
}

// --- helpers ------------------------------------------------------------

// readMaybeMissing reads a file, returning (nil, nil) when it does not exist
// so callers can distinguish "no prior file" from a real read error.
func readMaybeMissing(path string) ([]byte, error) {
	b, err := os.ReadFile(path)
	if err != nil {
		if errors.Is(err, os.ErrNotExist) {
			return nil, nil
		}
		return nil, err
	}
	return b, nil
}

// writeFile wraps atomicfile.Write + parent-dir creation.
func writeFile(path string, data []byte) error {
	dir := dirOf(path)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return fmt.Errorf("mkdir %s: %w", dir, err)
	}
	if err := atomicfile.Write(path, data, 0644); err != nil {
		return fmt.Errorf("write %s: %w", path, err)
	}
	return nil
}

// dirOf is filepath.Dir without importing path/filepath (kept inline so the
// import block stays minimal and matches the network adapter's style).
func dirOf(path string) string {
	if i := strings.LastIndexByte(path, '/'); i >= 0 {
		if i == 0 {
			return "/"
		}
		return path[:i]
	}
	return "."
}

// updateTimesyncdConfig is ported verbatim from handlers.updateTimesyncdConfig.
// It applies the user-selected maximum poll interval while retaining a short
// initial poll interval (Max > Min, starts polling at Min).
func updateTimesyncdConfig(data []byte, server string, interval int) ([]byte, error) {
	lines := strings.Split(string(data), "\n")
	pollMax := interval
	if pollMax <= 0 {
		pollMax = existingPollMax(lines)
	}
	if pollMax <= timesyncdPollMinSec {
		pollMax = defaultNTPPollMaxSec
	}
	pollMinStr := strconv.Itoa(timesyncdPollMinSec)
	pollMaxStr := strconv.Itoa(pollMax)

	foundNTP := false
	foundPollMin := false
	foundPollMax := false
	hasTimeSection := false
	timeSectionIdx := -1

	for i, line := range lines {
		trimmed := strings.TrimSpace(line)
		if trimmed == "[Time]" {
			hasTimeSection = true
			timeSectionIdx = i
		}
		if server != "" && strings.HasPrefix(trimmed, "NTP=") {
			lines[i] = "NTP=" + server
			foundNTP = true
		} else if strings.HasPrefix(trimmed, "PollIntervalMinSec=") {
			lines[i] = "PollIntervalMinSec=" + pollMinStr
			foundPollMin = true
		} else if strings.HasPrefix(trimmed, "PollIntervalMaxSec=") {
			lines[i] = "PollIntervalMaxSec=" + pollMaxStr
			foundPollMax = true
		}
	}

	missing := []string{}
	if server != "" && !foundNTP {
		missing = append(missing, "NTP="+server)
	}
	if !foundPollMin {
		missing = append(missing, "PollIntervalMinSec="+pollMinStr)
	}
	if !foundPollMax {
		missing = append(missing, "PollIntervalMaxSec="+pollMaxStr)
	}

	if len(missing) > 0 {
		if !hasTimeSection {
			lines = append(lines, "[Time]")
			timeSectionIdx = len(lines) - 1
		}
		insertIdx := timeSectionIdx + 1
		tail := make([]string, len(lines[insertIdx:]))
		copy(tail, lines[insertIdx:])
		lines = append(lines[:insertIdx], missing...)
		lines = append(lines, tail...)
	}
	return []byte(strings.Join(lines, "\n")), nil
}

// existingPollMax is ported verbatim from handlers.existingPollMax.
func existingPollMax(lines []string) int {
	for _, line := range lines {
		trimmed := strings.TrimSpace(line)
		if !strings.HasPrefix(trimmed, "PollIntervalMaxSec=") {
			continue
		}
		value, err := strconv.Atoi(strings.TrimSpace(strings.TrimPrefix(trimmed, "PollIntervalMaxSec=")))
		if err == nil && value > timesyncdPollMinSec {
			return value
		}
	}
	return defaultNTPPollMaxSec
}
