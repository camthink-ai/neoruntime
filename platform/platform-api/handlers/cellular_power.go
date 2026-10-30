package handlers

import (
	"bytes"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"aipc/platform/common/logger"
)

func (h *CellularHandler) ensurePower(on bool) error {
	if h.hw == nil {
		return fmt.Errorf("cellular hardware profile not configured")
	}
	if !h.hw.GPIO.Power.hasLine() {
		logger.Info("cellular: no gpio.power line in profile; skip power control")
		return nil
	}

	h.mu.Lock()
	defer h.mu.Unlock()

	if h.powerHoldAliveLocked() && h.pwrHoldLevel != nil && *h.pwrHoldLevel == on {
		return nil
	}
	_ = h.releasePowerLocked()

	if on && h.hw.GPIO.Reset.hasLine() {
		_ = h.driveLineOnce(h.hw.GPIO.Reset, false)
		time.Sleep(time.Duration(h.hw.GPIO.ResetDeassertMs) * time.Millisecond)
	}

	if h.hw.GPIO.Power.Hold {
		cmd, err := h.driveLineHold(h.hw.GPIO.Power, on)
		if err != nil {
			return err
		}
		h.pwrHoldCmd = cmd
		level := on
		h.pwrHoldLevel = &level
		_ = h.writePID("cellular-pwr.pid", cmd.Process.Pid)
		logger.Info("cellular: module power held %s via gpioset pid=%d", map[bool]string{true: "ON", false: "OFF"}[on], cmd.Process.Pid)
		return nil
	}
	return h.driveLineOnce(h.hw.GPIO.Power, on)
}

func (h *CellularHandler) powerHoldAliveLocked() bool {
	if h.pwrHoldCmd == nil || h.pwrHoldCmd.Process == nil {
		return false
	}
	return procAlive(h.pwrHoldCmd.Process.Pid)
}

func procAlive(pid int) bool {
	b, err := os.ReadFile(fmt.Sprintf("/proc/%d/stat", pid))
	if err != nil {
		return false
	}
	state := procStateFromStat(b)
	return state != "" && state != "Z" && state != "X"
}

func procStateFromStat(b []byte) string {
	rest := b
	if i := bytes.LastIndexByte(b, ')'); i >= 0 && i+2 < len(b) {
		rest = b[i+2:]
	}
	fields := strings.Fields(string(rest))
	if len(fields) == 0 {
		return ""
	}
	return fields[0]
}

func (h *CellularHandler) releasePowerLocked() error {
	if h.pwrHoldCmd != nil && h.pwrHoldCmd.Process != nil {
		_ = h.pwrHoldCmd.Process.Kill()
		_, _ = h.pwrHoldCmd.Process.Wait()
	}
	h.pwrHoldCmd = nil
	h.pwrHoldLevel = nil
	_ = os.Remove(filepath.Join(h.runDir, "cellular-pwr.pid"))
	return nil
}

func (h *CellularHandler) driveLineOnce(line CellularGPIOLine, on bool) error {
	chip, offset, err := h.resolveGPIOTarget(line)
	if err != nil {
		return err
	}
	val := line.driveValue(on)
	// Always pass numeric offset: libgpiod v1 gpioset rejects line names;
	// v2 accepts both, so offset is the portable form.
	cmd := h.command("gpioset", "-m", "exit", chip, fmt.Sprintf("%s=%d", offset, val))
	out, err := cmd.CombinedOutput()
	if err != nil {
		return fmt.Errorf("gpioset %s %s=%d: %w (%s)", chip, offset, val, err, strings.TrimSpace(string(out)))
	}
	return nil
}

func (h *CellularHandler) driveLineHold(line CellularGPIOLine, on bool) (*exec.Cmd, error) {
	chip, offset, err := h.resolveGPIOTarget(line)
	if err != nil {
		return nil, err
	}
	val := line.driveValue(on)
	// Do not pass -b: libgpiod's background mode double-forks, leaving a
	// short-lived parent that looks like a zombie while the real holder is
	// reparented. Keep gpioset as a direct child so Kill/Wait stay reliable.
	cmd := h.command("gpioset", "-m", "signal", chip, fmt.Sprintf("%s=%d", offset, val))
	if err := cmd.Start(); err != nil {
		return nil, fmt.Errorf("gpioset hold %s %s=%d: %w", chip, offset, val, err)
	}
	// Invalid args make gpioset exit immediately; wait briefly and reject zombies.
	time.Sleep(80 * time.Millisecond)
	pid := cmd.Process.Pid
	b, err := os.ReadFile(fmt.Sprintf("/proc/%d/stat", pid))
	if err != nil {
		_, _ = cmd.Process.Wait()
		return nil, fmt.Errorf("gpioset hold %s %s=%d exited immediately", chip, offset, val)
	}
	rest := b
	if i := bytes.LastIndexByte(b, ')'); i >= 0 && i+2 < len(b) {
		rest = b[i+2:]
	}
	fields := strings.Fields(string(rest))
	if len(fields) == 0 || fields[0] == "Z" || fields[0] == "X" {
		state := "?"
		if len(fields) > 0 {
			state = fields[0]
		}
		_, _ = cmd.Process.Wait()
		return nil, fmt.Errorf("gpioset hold %s %s=%d died (proc state=%s; line likely busy)", chip, offset, val, state)
	}
	return cmd, nil
}

// resolveGPIOTarget prefers YAML offset, else gpiofind by line name.
// Chip comes from the board profile or gpiofind — never a package default.
func (h *CellularHandler) resolveGPIOTarget(line CellularGPIOLine) (chip, offset string, err error) {
	if h.hw == nil {
		return "", "", fmt.Errorf("cellular hw profile not loaded")
	}
	chip = strings.TrimSpace(h.hw.GPIO.Chip)
	if line.Offset != nil {
		if chip == "" {
			return "", "", fmt.Errorf("gpio.chip is required when using line offset")
		}
		return chip, strconv.Itoa(*line.Offset), nil
	}
	name := strings.TrimSpace(line.Name)
	if name == "" {
		return "", "", fmt.Errorf("gpio line has neither name nor offset")
	}
	foundChip, foundOff, ok := h.gpiofindTarget(name)
	if !ok {
		return "", "", fmt.Errorf("gpio line %q: gpiofind failed (need YAML offset or DTS line-name)", name)
	}
	if foundChip != "" {
		chip = foundChip
	}
	if chip == "" {
		return "", "", fmt.Errorf("gpio line %q: no chip from gpiofind and gpio.chip unset", name)
	}
	return chip, foundOff, nil
}

func (h *CellularHandler) resolveGPIOSpec(line CellularGPIOLine) (string, error) {
	_, offset, err := h.resolveGPIOTarget(line)
	return offset, err
}

func (h *CellularHandler) gpiofindTarget(name string) (chip, offset string, ok bool) {
	cmd := h.command("gpiofind", name)
	out, err := cmd.CombinedOutput()
	if err != nil {
		return "", "", false
	}
	// gpiofind prints: "<chip> <offset>" (libgpiod v1/v2)
	fields := strings.Fields(strings.TrimSpace(string(out)))
	if len(fields) < 2 {
		return "", "", false
	}
	offset = fields[len(fields)-1]
	if _, err := strconv.Atoi(offset); err != nil {
		return "", "", false
	}
	chip = strings.Join(fields[:len(fields)-1], " ")
	return chip, offset, true
}

func (h *CellularHandler) writePID(name string, pid int) error {
	if err := os.MkdirAll(h.runDir, 0755); err != nil {
		return err
	}
	return os.WriteFile(filepath.Join(h.runDir, name), []byte(strconv.Itoa(pid)+"\n"), 0644)
}

func (h *CellularHandler) waitForModule(d time.Duration) bool {
	deadline := h.now().Add(d)
	for h.now().Before(deadline) {
		vid, _, _ := h.findMatchedUSB()
		if vid != "" {
			return true
		}
		time.Sleep(250 * time.Millisecond)
	}
	vid, _, _ := h.findMatchedUSB()
	return vid != ""
}
