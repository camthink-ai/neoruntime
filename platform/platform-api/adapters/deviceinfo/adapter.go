// Package deviceinfo is the Config Controller adapter for the device_info
// domain. It owns the only writable device-identity field — the device name —
// which projects to two live surfaces: the system hostname (`hostname` exec +
// /etc/hostname) and the DEVICE_NAME= line in device.conf.
//
// The adapter preserves the existing UpdateDeviceName HTTP contract: the
// desired value is `{ "device_name": "<name>" }`, the name must match
// ^[a-zA-Z0-9_-]+$, and applying it updates both the hostname and device.conf.
// On Verify failure the Manager auto-invokes Restore, which writes back the
// prior device.conf bytes and resets the hostname to its pre-apply value.
package deviceinfo

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"regexp"
	"strings"

	"aipc/platform/platform-api/internal/atomicfile"
)

// nameRe mirrors the validation in handlers/device_info.go's UpdateDeviceName.
var nameRe = regexp.MustCompile(`^[a-zA-Z0-9_-]+$`)

// desiredEnvelope is the JSON shape accepted for the device_info domain.
type desiredEnvelope struct {
	DeviceName string `json:"device_name"`
}

var (
	// ErrInvalidJSON is returned when desiredJSON does not unmarshal.
	ErrInvalidJSON = errors.New("deviceinfo: invalid desired json")
	// ErrInvalidName is returned when the device name is empty or contains
	// characters outside [a-zA-Z0-9_-].
	ErrInvalidName = errors.New("deviceinfo: invalid device name")
)

// hostnameController abstracts the live hostname so tests can fake it without
// exec'ing `hostname` or touching /etc/hostname.
type hostnameController interface {
	// Set sets both the transient hostname and /etc/hostname.
	Set(name string) error
	// Get returns the current transient hostname.
	Get() string
}

// liveHostname is the production hostnameController. It shells out to
// `hostname <name>` for the transient name and writes /etc/hostname for
// persistence across reboots, matching the original handler behavior.
type liveHostname struct{}

func (liveHostname) Set(name string) error {
	if err := exec.Command("hostname", name).Run(); err != nil {
		return fmt.Errorf("set transient hostname: %w", err)
	}
	if err := os.WriteFile("/etc/hostname", []byte(name+"\n"), 0644); err != nil {
		return fmt.Errorf("write /etc/hostname: %w", err)
	}
	return nil
}

func (liveHostname) Get() string {
	h, err := os.Hostname()
	if err != nil {
		return ""
	}
	return h
}

// backupState captures what Restore needs to revert: the device.conf bytes
// (nil if the file did not exist pre-apply) and the prior hostname.
type backupState struct {
	confBytes []byte
	hostname  string
}

// rendered is the projection produced by Render: the full device.conf bytes
// with DEVICE_NAME replaced, plus the bare name for the hostname exec. Carrying
// the name avoids re-parsing it from the rendered bytes in Apply.
type rendered struct {
	bytes []byte
	name  string
}

// Adapter applies device-name changes to the live system. One instance is
// registered with the Config Manager under the "device_info" domain.
type Adapter struct {
	configPath string
	host       hostnameController
}

// New returns an Adapter that writes configPath and controls the live hostname.
// configPath is typically constants.ConfigPath() + "/device.conf".
func New(configPath string) *Adapter {
	return &Adapter{configPath: configPath, host: liveHostname{}}
}

// Validate checks that desiredJSON carries a well-formed, allowed device name.
func (a *Adapter) Validate(_ context.Context, _, desiredJSON string) error {
	var env desiredEnvelope
	if err := json.Unmarshal([]byte(desiredJSON), &env); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalidJSON, err)
	}
	if env.DeviceName == "" || !nameRe.MatchString(env.DeviceName) {
		return ErrInvalidName
	}
	return nil
}

// Backup captures the current device.conf bytes and hostname for Restore.
// A missing device.conf is not an error; confBytes stays nil so Restore knows
// to remove the file it created rather than overwrite a prior one.
func (a *Adapter) Backup(_ context.Context, _ string) (any, error) {
	b, err := os.ReadFile(a.configPath)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("read device.conf: %w", err)
	}
	return backupState{confBytes: b, hostname: a.host.Get()}, nil
}

// renderConf produces the new device.conf content with the DEVICE_NAME line
// replaced, or appended if absent. Other keys (MODEL, HARDWARE_VERSION, ...)
// are preserved verbatim.
func renderConf(existing []byte, name string) []byte {
	lines := strings.Split(string(existing), "\n")
	found := false
	for i, line := range lines {
		if strings.HasPrefix(line, "DEVICE_NAME=") {
			lines[i] = "DEVICE_NAME=" + name
			found = true
			break
		}
	}
	if !found {
		lines = append(lines, "DEVICE_NAME="+name)
	}
	return []byte(strings.Join(lines, "\n"))
}

// Render reads the current device.conf and produces the projected bytes plus
// the bare name. Reading here (rather than in Apply) keeps Apply a pure push.
func (a *Adapter) Render(_ context.Context, _, desiredJSON string) (any, error) {
	var env desiredEnvelope
	if err := json.Unmarshal([]byte(desiredJSON), &env); err != nil {
		return nil, fmt.Errorf("%w: %v", ErrInvalidJSON, err)
	}
	existing, err := os.ReadFile(a.configPath)
	if err != nil && !errors.Is(err, os.ErrNotExist) {
		return nil, fmt.Errorf("read device.conf for render: %w", err)
	}
	return rendered{bytes: renderConf(existing, env.DeviceName), name: env.DeviceName}, nil
}

// Apply sets the hostname and atomically writes device.conf.
func (a *Adapter) Apply(_ context.Context, _ string, renderedVal any) error {
	r, ok := renderedVal.(rendered)
	if !ok {
		return fmt.Errorf("deviceinfo: rendered payload is not deviceinfo.rendered")
	}
	if err := a.host.Set(r.name); err != nil {
		return err
	}
	return atomicfile.Write(a.configPath, r.bytes, 0644)
}

// extractDeviceName pulls the DEVICE_NAME value from device.conf bytes.
func extractDeviceName(data []byte) string {
	for _, line := range strings.Split(string(data), "\n") {
		if strings.HasPrefix(line, "DEVICE_NAME=") {
			return strings.TrimSpace(strings.TrimPrefix(line, "DEVICE_NAME="))
		}
	}
	return ""
}

// Verify reads back device.conf and the live hostname and confirms both match
// the desired name. A mismatch triggers auto-Restore in the Manager.
func (a *Adapter) Verify(_ context.Context, _, desiredJSON string) error {
	var env desiredEnvelope
	if err := json.Unmarshal([]byte(desiredJSON), &env); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalidJSON, err)
	}
	data, err := os.ReadFile(a.configPath)
	if err != nil {
		return fmt.Errorf("verify: read device.conf: %w", err)
	}
	if got := extractDeviceName(data); got != env.DeviceName {
		return fmt.Errorf("verify: device.conf %q != desired %q", got, env.DeviceName)
	}
	if got := a.host.Get(); got != env.DeviceName {
		return fmt.Errorf("verify: hostname %q != desired %q", got, env.DeviceName)
	}
	return nil
}

// Restore reverts device.conf to its pre-apply bytes (or removes it if it did
// not exist) and resets the hostname. It is best-effort: a hostname-reset
// failure does not fail the restore if device.conf was reverted, since the
// system is left in a safe (older) state.
func (a *Adapter) Restore(_ context.Context, _ string, backup any) error {
	bs, ok := backup.(backupState)
	if !ok {
		return fmt.Errorf("deviceinfo: backup payload is not deviceinfo.backupState")
	}
	if bs.confBytes != nil {
		if err := atomicfile.Write(a.configPath, bs.confBytes, 0644); err != nil {
			return fmt.Errorf("restore device.conf: %w", err)
		}
	} else {
		// No prior file existed; remove the one Apply created.
		_ = os.Remove(a.configPath)
	}
	if bs.hostname != "" {
		if err := a.host.Set(bs.hostname); err != nil {
			return fmt.Errorf("restore hostname: %w", err)
		}
	}
	return nil
}
