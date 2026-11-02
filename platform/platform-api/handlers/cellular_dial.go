package handlers

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync/atomic"
	"syscall"
	"time"

	"aipc/platform/common/logger"
)

func (h *CellularHandler) findDialer() (string, error) {
	if h.hw == nil || len(h.hw.Dialer.Binaries) == 0 {
		return "", fmt.Errorf("no cellular dialer in hw profile")
	}
	for _, name := range h.hw.Dialer.Binaries {
		p, err := h.lookPath(name)
		if err == nil && p != "" {
			return p, nil
		}
	}
	return "", fmt.Errorf("no cellular dialer found")
}

// dialerAvailable: ECM needs udhcpc; QMI/MBIM need the profile dialer binary.
func (h *CellularHandler) dialerAvailable(cfg CellularConfig) bool {
	if strings.EqualFold(cfg.Usbnet, "ecm") || probeUsbnetDriver() == "ecm" {
		p, err := h.lookPath("udhcpc")
		return err == nil && p != ""
	}
	_, err := h.findDialer()
	return err == nil
}

func (h *CellularHandler) startDialer(bin string, cfg CellularConfig) error {
	useEcm := strings.EqualFold(cfg.Usbnet, "ecm") || probeUsbnetDriver() == "ecm"
	if useEcm {
		if err := h.prepareEcmPDP(cfg); err != nil {
			logger.Warn("cellular: ECM PDP prep: %v (continuing to DHCP)", err)
		}
	}

	h.mu.Lock()
	defer h.mu.Unlock()

	_ = h.stopDialerLocked()

	if useEcm {
		return h.startEcmDialerLocked(cfg)
	}

	style := "aipc"
	if h.hw != nil {
		style = h.hw.Dialer.Style
	}
	args := dialerArgs(style, cfg)
	cmd := h.command(bin, args...)
	cmd.Stdout = nil
	cmd.Stderr = nil
	if err := cmd.Start(); err != nil {
		return err
	}
	h.dialerCmd = cmd
	_ = h.writePID("cellular-dialer.pid", cmd.Process.Pid)
	logger.Info("cellular: dialer started bin=%s style=%s pid=%d args=%v", bin, style, cmd.Process.Pid, args)

	h.watchDialer(cmd)
	return nil
}

func (h *CellularHandler) watchDialer(cmd *exec.Cmd) {
	started := h.now()
	go func() {
		err := cmd.Wait()
		h.mu.Lock()
		if h.dialerCmd == cmd {
			h.dialerCmd = nil
			_ = os.Remove(filepath.Join(h.runDir, "cellular-dialer.pid"))
		}
		h.mu.Unlock()
		lived := h.now().Sub(started)
		intentional := atomic.SwapInt32(&h.dialerStopExpect, 0) == 1
		if err != nil {
			logger.Warn("cellular: dialer exited after %s: %v", lived.Round(time.Millisecond), err)
		} else {
			logger.Warn("cellular: dialer exited after %s", lived.Round(time.Millisecond))
		}
		// Instant exit (no AT/QMI) → back off so auto-dial does not spam.
		// Intentional stopDialer (SIM recover / config) must not back off.
		if !intentional && lived < 3*time.Second {
			h.noteDialFailure()
		}
	}()
}

func (h *CellularHandler) findEcmIface() string {
	if h.hw == nil || h.sysfsNet == "" {
		return ""
	}
	for _, n := range h.hw.Net.Ifaces {
		if n == "" {
			continue
		}
		link, err := os.Readlink(filepath.Join(h.sysfsNet, n, "device/driver"))
		if err != nil {
			continue
		}
		switch filepath.Base(link) {
		case "cdc_ether", "rndis_host":
			return n
		}
	}
	return ""
}

func (h *CellularHandler) prepareEcmPDP(cfg CellularConfig) error {
	atPortMu.Lock()
	defer atPortMu.Unlock()

	port := findATPort()
	if port == "" {
		return fmt.Errorf("no AT port")
	}
	cid := cfg.PDPCID
	if cid < 1 {
		cid = 1
	}
	ipType := "IPV4V6"
	switch strings.ToLower(cfg.IPType) {
	case "ipv4":
		ipType = "IP"
	case "ipv6":
		ipType = "IPV6"
	}
	apn := strings.TrimSpace(cfg.APN)
	if apn == "" {
		if cfg.CarrierProfile == "verizon" {
			apn = "vzwinternet"
		} else {
			apn = "internet"
		}
	}
	raw, err := atExchange(port, []string{
		fmt.Sprintf(`AT+CGDCONT=%d,"%s","%s"`, cid, ipType, apn),
		fmt.Sprintf("AT+CGACT=1,%d", cid),
	}, 2*time.Second)
	if err != nil {
		return err
	}
	if strings.Contains(raw, "ERROR") && !strings.Contains(raw, "OK") {
		return fmt.Errorf("modem rejected ECM PDP setup (%s)", strings.TrimSpace(raw))
	}
	return nil
}

func (h *CellularHandler) startEcmDialerLocked(cfg CellularConfig) error {
	iface := h.findEcmIface()
	if iface == "" {
		return fmt.Errorf("no ECM network interface among profile net.ifaces")
	}
	udhcpc, err := h.lookPath("udhcpc")
	if err != nil || udhcpc == "" {
		return fmt.Errorf("udhcpc not found")
	}
	_ = h.command("ip", "link", "set", "dev", iface, "up").Run()

	// Foreground DHCP client acts as the "dialer" process we supervise.
	cmd := h.command(udhcpc, "-i", iface, "-f", "-S")
	cmd.Stdout = nil
	cmd.Stderr = nil
	if err := cmd.Start(); err != nil {
		return err
	}
	h.dialerCmd = cmd
	_ = h.writePID("cellular-dialer.pid", cmd.Process.Pid)
	logger.Info("cellular: ECM dialer started udhcpc pid=%d iface=%s apn=%s", cmd.Process.Pid, iface, cfg.APN)
	h.watchDialer(cmd)
	return nil
}

func (h *CellularHandler) stopDialer() error {
	atomic.StoreInt32(&h.dialerStopExpect, 1)
	h.mu.Lock()
	defer h.mu.Unlock()
	return h.stopDialerLocked()
}

func (h *CellularHandler) stopDialerLocked() error {
	// SIGTERM lets the dialer tear down the PDP; SIGKILL can leave addresses up.
	pid := 0
	if h.dialerCmd != nil && h.dialerCmd.Process != nil {
		pid = h.dialerCmd.Process.Pid
		_ = h.dialerCmd.Process.Signal(syscall.SIGTERM)
	}
	if pid == 0 {
		pid = h.readPIDFile("cellular-dialer.pid")
		if pid > 0 {
			_ = syscall.Kill(pid, syscall.SIGTERM)
		}
	}

	deadline := h.now().Add(3 * time.Second)
	for h.now().Before(deadline) {
		alive := false
		if pid > 0 && procAlive(pid) {
			alive = true
		}
		if h.dialerCmd != nil && h.dialerCmd.Process != nil && procAlive(h.dialerCmd.Process.Pid) {
			alive = true
		}
		if !alive {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}

	if h.dialerCmd != nil && h.dialerCmd.Process != nil && procAlive(h.dialerCmd.Process.Pid) {
		_ = h.dialerCmd.Process.Kill()
	}
	if pid > 0 && procAlive(pid) {
		_ = syscall.Kill(pid, syscall.SIGKILL)
	}
	h.killDialerBinaries()

	h.dialerCmd = nil
	_ = os.Remove(filepath.Join(h.runDir, "cellular-dialer.pid"))
	h.flushNetIfaces()
	return nil
}

func (h *CellularHandler) readPIDFile(name string) int {
	b, err := os.ReadFile(filepath.Join(h.runDir, name))
	if err != nil {
		return 0
	}
	pid, err := strconv.Atoi(strings.TrimSpace(string(b)))
	if err != nil || pid <= 1 {
		return 0
	}
	return pid
}

func (h *CellularHandler) killDialerBinaries() {
	names := h.hw.dialerBasenames()
	if len(names) == 0 {
		return
	}
	for _, name := range names {
		_ = h.command("pkill", "-TERM", "-x", name).Run()
	}
	deadline := h.now().Add(1500 * time.Millisecond)
	for h.now().Before(deadline) {
		any := false
		for _, name := range names {
			if h.command("pgrep", "-x", name).Run() == nil {
				any = true
				break
			}
		}
		if !any {
			return
		}
		time.Sleep(100 * time.Millisecond)
	}
	for _, name := range names {
		_ = h.command("pkill", "-KILL", "-x", name).Run()
	}
}

func (h *CellularHandler) flushNetIfaces() {
	if h.hw == nil || h.sysfsNet == "" {
		return
	}
	for _, n := range h.hw.Net.Ifaces {
		if _, err := os.Stat(filepath.Join(h.sysfsNet, n)); err != nil {
			continue
		}
		_ = h.command("ip", "addr", "flush", "dev", n).Run()
		_ = h.command("ip", "link", "set", "dev", n, "down").Run()
	}
}

func dialerArgs(style string, cfg CellularConfig) []string {
	apn := cfg.APN
	if apn == "" {
		if cfg.CarrierProfile == "verizon" {
			apn = "vzwinternet"
		} else {
			apn = "internet"
		}
	}

	switch strings.ToLower(style) {
	case "quectel_cm":
		// Matches QConnectManager V1.6.x:
		//   -s [apn [user password auth]]  auth: 1=pap, 2=chap
		//   -4 / -6                        IP family
		//   -n pdn                         PDP / PDN context
		args := []string{"-s", apn}
		if cfg.Auth != "none" {
			auth := "0"
			switch cfg.Auth {
			case "pap":
				auth = "1"
			case "chap":
				auth = "2"
			}
			args = append(args, cfg.User, cfg.Password, auth)
		}
		switch strings.ToLower(cfg.IPType) {
		case "ipv4":
			args = append(args, "-4")
		case "ipv6":
			args = append(args, "-6")
		case "ipv4v6":
			args = append(args, "-4", "-6")
		}
		if cfg.PDPCID >= 0 {
			args = append(args, "-n", fmt.Sprintf("%d", cfg.PDPCID))
		}
		if pin := strings.TrimSpace(cfg.PIN); pin != "" {
			args = append(args, "-p", pin)
		}
		return args
	default: // aipc
		args := []string{"connect", "--apn", apn, "--cid", fmt.Sprintf("%d", cfg.PDPCID), "--ip-type", cfg.IPType}
		if cfg.Auth != "none" {
			args = append(args, "--auth", cfg.Auth, "--user", cfg.User, "--password", cfg.Password)
		}
		if pin := strings.TrimSpace(cfg.PIN); pin != "" {
			args = append(args, "--pin", pin)
		}
		return args
	}
}
