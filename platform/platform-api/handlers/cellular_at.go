package handlers

import (
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"aipc/platform/common/logger"
	"golang.org/x/sys/unix"
)

// AT transport + modem config helpers (usbnet / RAT).
// All device/iface/dialer names come from CellularHWProfile via setCellularProbePrefs.

var (
	atCacheMu  sync.Mutex
	atCache    modemIdentity
	atCacheTTL = 20 * time.Second
	atPortMu   sync.Mutex

	// Filled from CellularHWProfile (single in-process handler).
	cellularPreferredATPorts []string
	cellularATPortGlob       string
	cellularControlGlobs     []string
	cellularNetIfaces        []string
	cellularSysfsNet         string
	cellularDialerNames      []string
)

func setCellularProbePrefs(hw *CellularHWProfile) {
	cellularPreferredATPorts = nil
	cellularATPortGlob = ""
	cellularControlGlobs = nil
	cellularNetIfaces = nil
	cellularSysfsNet = ""
	cellularDialerNames = nil
	if hw == nil {
		return
	}
	if len(hw.AT.Ports) > 0 {
		cellularPreferredATPorts = append([]string(nil), hw.AT.Ports...)
	}
	cellularATPortGlob = hw.AT.PortGlob
	if len(hw.Control.Globs) > 0 {
		cellularControlGlobs = append([]string(nil), hw.Control.Globs...)
	}
	if len(hw.Net.Ifaces) > 0 {
		cellularNetIfaces = append([]string(nil), hw.Net.Ifaces...)
	}
	cellularSysfsNet = hw.Paths.SysfsNet
	cellularDialerNames = hw.dialerBasenames()
}

func anyDialerRunning(command func(string, ...string) *exec.Cmd) bool {
	if command == nil {
		return false
	}
	for _, name := range cellularDialerNames {
		if command("pgrep", "-x", name).Run() == nil {
			return true
		}
	}
	return false
}

func clearATCache() {
	atCacheMu.Lock()
	atCache = modemIdentity{}
	atCacheMu.Unlock()
}

func lockATPort(wait time.Duration) bool {
	deadline := time.Now().Add(wait)
	for {
		if atPortMu.TryLock() {
			return true
		}
		if time.Now().After(deadline) {
			return false
		}
		time.Sleep(40 * time.Millisecond)
	}
}

func listATPortCandidates() []string {
	var candidates []string
	seen := map[string]bool{}
	for _, p := range cellularPreferredATPorts {
		if st, err := os.Stat(p); err == nil && st.Mode()&os.ModeCharDevice != 0 {
			candidates = append(candidates, p)
			seen[p] = true
		}
	}
	if cellularATPortGlob != "" {
		matches, _ := filepath.Glob(cellularATPortGlob)
		for _, m := range matches {
			if !seen[m] {
				candidates = append(candidates, m)
				seen[m] = true
			}
		}
	}
	return candidates
}

func findATPort() string {
	for _, p := range listATPortCandidates() {
		if raw, err := atExchangeWait(p, []string{"AT"}, 150*time.Millisecond, 200*time.Millisecond); err == nil && strings.Contains(raw, "OK") {
			return p
		}
	}
	return ""
}

func atExchange(port string, cmds []string, perCmd time.Duration) (string, error) {
	return atExchangeWait(port, cmds, perCmd, 4*time.Second)
}

func atExchangeWait(port string, cmds []string, perCmd, lockWait time.Duration) (string, error) {
	deadline := time.Now().Add(lockWait)
	var fd int
	var err error
	for {
		fd, err = unix.Open(port, unix.O_RDWR|unix.O_NOCTTY|unix.O_NONBLOCK, 0)
		if err == nil {
			if err = unix.Flock(fd, unix.LOCK_EX|unix.LOCK_NB); err == nil {
				break
			}
			_ = unix.Close(fd)
		}
		if time.Now().After(deadline) {
			if err == nil {
				err = fmt.Errorf("resource temporarily unavailable")
			}
			return "", err
		}
		time.Sleep(80 * time.Millisecond)
	}
	owned := true
	defer func() {
		if owned {
			_ = unix.Close(fd)
		}
	}()
	defer unix.Flock(fd, unix.LOCK_UN)

	if err := setTTYRaw115200(fd); err != nil {
		return "", err
	}
	flags, err := unix.FcntlInt(uintptr(fd), unix.F_GETFL, 0)
	if err == nil {
		_, _ = unix.FcntlInt(uintptr(fd), unix.F_SETFL, flags&^unix.O_NONBLOCK)
	}

	f := os.NewFile(uintptr(fd), port)
	owned = false // f.Close owns the fd
	defer f.Close()

	_ = drainAT(f, 40*time.Millisecond)

	var b strings.Builder
	for _, cmd := range cmds {
		_ = drainAT(f, 15*time.Millisecond)
		if _, err := f.Write([]byte(cmd + "\r")); err != nil {
			return b.String(), err
		}
		// Synthetic command marker: parsers section replies when ATE0 hides echo.
		b.WriteString(cmd)
		b.WriteByte('\n')
		b.WriteString(readATUntil(f, perCmd))
		b.WriteByte('\n')
	}
	return b.String(), nil
}

func setTTYRaw115200(fd int) error {
	var t unix.Termios
	if err := unix.IoctlSetTermios(fd, unix.TCGETS, &t); err != nil {
		return err
	}
	t.Iflag &^= unix.IGNBRK | unix.BRKINT | unix.PARMRK | unix.ISTRIP |
		unix.INLCR | unix.IGNCR | unix.ICRNL | unix.IXON
	t.Oflag &^= unix.OPOST
	t.Lflag &^= unix.ECHO | unix.ECHONL | unix.ICANON | unix.ISIG | unix.IEXTEN
	t.Cflag &^= unix.CSIZE | unix.PARENB | unix.CBAUD
	t.Cflag |= unix.CS8 | unix.CREAD | unix.CLOCAL | unix.B115200
	t.Ispeed = unix.B115200
	t.Ospeed = unix.B115200
	t.Cc[unix.VMIN] = 0
	t.Cc[unix.VTIME] = 1
	return unix.IoctlSetTermios(fd, unix.TCSETS, &t)
}

func drainAT(f *os.File, d time.Duration) error {
	deadline := time.Now().Add(d)
	buf := make([]byte, 256)
	for time.Now().Before(deadline) {
		_ = f.SetReadDeadline(time.Now().Add(15 * time.Millisecond))
		n, err := f.Read(buf)
		if n == 0 || err != nil {
			return nil
		}
	}
	return nil
}

func readATUntil(f *os.File, d time.Duration) string {
	deadline := time.Now().Add(d)
	var b strings.Builder
	buf := make([]byte, 256)
	for time.Now().Before(deadline) {
		_ = f.SetReadDeadline(time.Now().Add(40 * time.Millisecond))
		n, err := f.Read(buf)
		if n > 0 {
			b.Write(buf[:n])
			s := b.String()
			if strings.Contains(s, "OK") || strings.Contains(s, "ERROR") {
				break
			}
			continue
		}
		if err != nil && !os.IsTimeout(err) {
			break
		}
	}
	return b.String()
}
func usbnetATCode(mode string) (int, error) {
	switch strings.ToLower(strings.TrimSpace(mode)) {
	case "qmi":
		return 0, nil
	case "ecm":
		return 1, nil
	case "mbim":
		return 2, nil
	default:
		return -1, fmt.Errorf("unsupported usbnet %q", mode)
	}
}

// probeUsbnetDriver maps the live USB net driver to qmi|mbim|ecm.
func probeUsbnetDriver() string {
	if cellularSysfsNet == "" || len(cellularNetIfaces) == 0 {
		return ""
	}
	for _, iface := range cellularNetIfaces {
		link, err := os.Readlink(filepath.Join(cellularSysfsNet, iface, "device/driver"))
		if err != nil {
			continue
		}
		switch filepath.Base(link) {
		case "qmi_wwan":
			return "qmi"
		case "cdc_mbim":
			return "mbim"
		case "cdc_ether", "rndis_host":
			return "ecm"
		}
	}
	return ""
}

func waitForATPort(d time.Duration) bool {
	deadline := time.Now().Add(d)
	for time.Now().Before(deadline) {
		if findATPort() != "" {
			return true
		}
		time.Sleep(300 * time.Millisecond)
	}
	return findATPort() != ""
}

func waitUsbnetTransition(oldMode, want string, d time.Duration) bool {
	want = strings.ToLower(strings.TrimSpace(want))
	oldMode = strings.ToLower(strings.TrimSpace(oldMode))
	deadline := time.Now().Add(d)
	seenDrop := oldMode == "" || oldMode == want
	for time.Now().Before(deadline) {
		live := probeUsbnetDriver()
		if !seenDrop {
			if live == "" || live != oldMode {
				seenDrop = true
				logger.Info("cellular: usbnet transition: left %s (now %q)", oldMode, live)
			}
		} else if live == want {
			logger.Info("cellular: usbnet transition: %s active", want)
			return true
		}
		time.Sleep(300 * time.Millisecond)
	}
	live := probeUsbnetDriver()
	ok := live == want
	if !ok {
		logger.Warn("cellular: usbnet transition timeout want=%s live=%q", want, live)
	}
	return ok
}

func networkModeATCode(mode string) (int, error) {
	switch strings.ToLower(strings.TrimSpace(mode)) {
	case "", "auto":
		return 0, nil
	case "wcdma":
		return 2, nil
	case "lte":
		return 3, nil
	default:
		return -1, fmt.Errorf("unsupported network_mode %q", mode)
	}
}

func (h *CellularHandler) applyModemPrefs(cfg CellularConfig) error {
	scan, err := networkModeATCode(cfg.NetworkMode)
	if err != nil {
		return err
	}
	roam := 2 // disable
	if roamingEnabled(cfg) {
		roam = 1 // enable
	}

	atPortMu.Lock()
	defer atPortMu.Unlock()

	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if !anyDialerRunning(h.command) {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	time.Sleep(200 * time.Millisecond)

	settle := time.Now().Add(30 * time.Second)
	var port string
	for {
		port = findATPort()
		if port != "" {
			break
		}
		if time.Now().After(settle) {
			return fmt.Errorf("no AT port available")
		}
		time.Sleep(250 * time.Millisecond)
	}

	cmds := []string{
		fmt.Sprintf(`AT+QCFG="nwscanmode",%d`, scan),
		fmt.Sprintf(`AT+QCFG="roamservice",%d`, roam),
	}
	if pin := strings.TrimSpace(cfg.PIN); pin != "" {
		cmds = append(cmds, fmt.Sprintf(`AT+CPIN="%s"`, pin))
	}
	raw, err := atExchange(port, cmds, 800*time.Millisecond)
	if err != nil {
		return err
	}
	if !strings.Contains(raw, "OK") {
		return fmt.Errorf("modem rejected network prefs (%s)", strings.TrimSpace(raw))
	}
	if _, err := atExchange(port, []string{"AT+CFUN=0"}, 3*time.Second); err != nil {
		logger.Warn("cellular: CFUN=0 after prefs: %v", err)
	}
	time.Sleep(1500 * time.Millisecond)
	if _, err := atExchange(port, []string{"AT+CFUN=1"}, 3*time.Second); err != nil {
		logger.Warn("cellular: CFUN=1 after prefs: %v", err)
	}
	clearATCache()
	logger.Info("cellular: applied network_mode=%s roaming=%v", cfg.NetworkMode, roamingEnabled(cfg))
	time.Sleep(2 * time.Second)
	return nil
}

func (h *CellularHandler) applyUsbnetMode(mode string) error {
	code, err := usbnetATCode(mode)
	if err != nil {
		return err
	}

	atPortMu.Lock()
	defer atPortMu.Unlock()

	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		if !anyDialerRunning(h.command) {
			break
		}
		time.Sleep(100 * time.Millisecond)
	}
	if anyDialerRunning(h.command) {
		_ = h.stopDialer()
		time.Sleep(200 * time.Millisecond)
	}
	time.Sleep(300 * time.Millisecond)

	var port string
	settle := time.Now().Add(60 * time.Second)
	for {
		port = findATPort()
		if port != "" {
			break
		}
		if time.Now().After(settle) {
			if !h.hasModemControl() {
				return fmt.Errorf("no AT/QMI control device")
			}
			return fmt.Errorf("no AT port available")
		}
		time.Sleep(300 * time.Millisecond)
	}
	raw, err := atExchange(port, []string{
		fmt.Sprintf(`AT+QCFG="usbnet",%d`, code),
		"AT+CFUN=1,1",
	}, 800*time.Millisecond)
	if err != nil {
		return err
	}
	if !strings.Contains(raw, "OK") {
		return fmt.Errorf("modem rejected usbnet=%s (%s)", mode, strings.TrimSpace(raw))
	}
	clearATCache()
	logger.Info("cellular: applied usbnet=%s (AT code %d); modem resetting", mode, code)
	return nil
}
