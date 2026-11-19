package handlers

import (
	"archive/tar"
	"compress/gzip"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/gin-gonic/gin"

	"aipc/platform/common/constants"
	"aipc/platform/common/logger"
)

const (
	qlogDefaultMaxMB    = 32
	qlogDefaultMaxFiles = 8
	qlogSystemBin       = "/usr/bin/qlog"
	qlogSystemFilterCfg = "/usr/share/qlog/conf/default.cfg"
)

type cellularQLogStartRequest struct {
	Port     string `json:"port"`
	MaxMB    int    `json:"max_mb"`
	MaxFiles int    `json:"max_files"`
}

func (h *CellularHandler) qlogDir() string {
	return filepath.Join(constants.LogPath(), "qlog")
}

func isExecutableFile(path string) bool {
	st, err := os.Stat(path)
	return err == nil && !st.IsDir()
}

func (h *CellularHandler) findQLogBinary() (string, error) {
	if isExecutableFile(qlogSystemBin) {
		return qlogSystemBin, nil
	}
	for _, name := range []string{"qlog", "QLog"} {
		if p, err := h.lookPath(name); err == nil && p != "" {
			return p, nil
		}
	}
	fallback := filepath.Join(constants.RootPath(), "bin", "qlog")
	if isExecutableFile(fallback) {
		return fallback, nil
	}
	return "", fmt.Errorf("qlog binary not found (expect %s from quectel-qlog package)", qlogSystemBin)
}

func qlogFilterCfg() string {
	for _, p := range []string{
		qlogSystemFilterCfg,
		filepath.Join(constants.RootPath(), "etc", "qlog", "default.cfg"),
	} {
		if st, err := os.Stat(p); err == nil && !st.IsDir() {
			return p
		}
	}
	return ""
}

func findDMPort(preferAT []string) string {
	atSet := map[string]struct{}{}
	for _, p := range preferAT {
		atSet[p] = struct{}{}
	}
	candidates := []string{
		"/dev/ttyUSB0",
		"/dev/ttyUSB1",
		"/dev/ttyUSB2",
		"/dev/ttyUSB3",
	}
	var first string
	for _, p := range candidates {
		if _, err := os.Stat(p); err != nil {
			continue
		}
		if first == "" {
			first = p
		}
		if _, isAT := atSet[p]; isAT {
			continue
		}
		return p
	}
	return first
}

func (h *CellularHandler) preferredATPorts() []string {
	if h.hw != nil && len(h.hw.AT.Ports) > 0 {
		return h.hw.AT.Ports
	}
	return nil
}

func qlogDirStats(dir string) (files int, bytes int64) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return 0, 0
	}
	for _, e := range entries {
		if e.IsDir() || e.Name() == "qlog-runner.log" {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		files++
		bytes += info.Size()
	}
	return files, bytes
}

func (h *CellularHandler) qlogPIDs() []int {
	out, err := h.command("pidof", "qlog").CombinedOutput()
	if err != nil {
		return nil
	}
	var pids []int
	for _, f := range strings.Fields(string(out)) {
		pid, err := strconv.Atoi(f)
		if err != nil || pid <= 1 {
			continue
		}
		pids = append(pids, pid)
	}
	return pids
}

func (h *CellularHandler) qlogProcessAlive() bool {
	return len(h.qlogPIDs()) > 0
}

func (h *CellularHandler) qlogRunningLocked() bool {
	alive := h.qlogProcessAlive()
	if h.qlogCmd != nil && h.qlogCmd.Process != nil {
		if err := h.qlogCmd.Process.Signal(syscall.Signal(0)); err != nil {
			h.qlogCmd = nil
			if !alive {
				h.qlogPort = ""
				h.qlogStartedAt = time.Time{}
			}
		} else {
			return true
		}
	}
	return alive
}

func (h *CellularHandler) signalQLogPIDs(sig syscall.Signal) {
	for _, pid := range h.qlogPIDs() {
		_ = syscall.Kill(pid, sig)
	}
	_ = h.command("killall", fmt.Sprintf("-%d", sig), "qlog").Run()
}

func pokeDMPort(port string) {
	port = strings.TrimSpace(port)
	if port == "" || !strings.HasPrefix(port, "/dev/") {
		return
	}
	f, err := os.OpenFile(port, os.O_RDWR|syscall.O_NONBLOCK, 0)
	if err != nil {
		return
	}
	_ = f.Close()
}

func (h *CellularHandler) killAllQLog() {
	port := h.qlogPort
	if port == "" {
		port = findDMPort(h.preferredATPorts())
	}
	if h.qlogCmd != nil && h.qlogCmd.Process != nil {
		_ = h.qlogCmd.Process.Signal(syscall.SIGTERM)
	}
	h.qlogCmd = nil
	h.qlogPort = ""
	h.qlogStartedAt = time.Time{}

	h.signalQLogPIDs(syscall.SIGTERM)
	h.signalQLogPIDs(syscall.SIGKILL)

	if h.qlogProcessAlive() {
		go func(p string) {
			for i := 0; i < 25 && len(h.qlogPIDs()) > 0; i++ {
				h.signalQLogPIDs(syscall.SIGKILL)
				pokeDMPort(p)
				time.Sleep(200 * time.Millisecond)
			}
		}(port)
	}
}

func (h *CellularHandler) qlogStatusPayload() gin.H {
	h.qlogMu.Lock()
	defer h.qlogMu.Unlock()

	dir := h.qlogDir()
	files, bytes := qlogDirStats(dir)
	running := h.qlogRunningLocked()
	bin, binErr := h.findQLogBinary()
	available := binErr == nil
	dm := findDMPort(h.preferredATPorts())

	out := gin.H{
		"ok":         true,
		"available":  available,
		"running":    running,
		"dir":        dir,
		"files":      files,
		"bytes":      bytes,
		"dm_port":    dm,
		"message":    "OK",
		"binary":     "",
		"port":       "",
		"started_at": "",
	}
	if available {
		out["binary"] = bin
	} else {
		out["message"] = binErr.Error()
	}
	if running {
		port := h.qlogPort
		if port == "" {
			port = dm
		}
		out["port"] = port
		if !h.qlogStartedAt.IsZero() {
			out["started_at"] = h.qlogStartedAt.UTC().Format(time.RFC3339)
		}
	}
	return out
}

// ensureDbgctlForDial repairs leftover dbgctl=1 (blocks EM05 MBIM/QMI dial).
func (h *CellularHandler) ensureDbgctlForDial() {
	atPortMu.Lock()
	defer atPortMu.Unlock()

	port := findATPort()
	if port == "" {
		return
	}
	raw, err := atExchange(port, []string{`AT+QCFG="dbgctl"`}, 800*time.Millisecond)
	if err != nil {
		return
	}
	if !strings.Contains(raw, `"dbgctl",1`) {
		return
	}
	setRaw, setErr := atExchange(port, []string{`AT+QCFG="dbgctl",0`}, 800*time.Millisecond)
	if setErr != nil || !strings.Contains(setRaw, "OK") {
		logger.Warn("cellular: clear dbgctl=1 failed: %v (%s)", setErr, strings.TrimSpace(setRaw))
		return
	}
	logger.Info("cellular: cleared leftover dbgctl=1 before dial")
}

func writeQLogTarGz(w io.Writer, dir string) (int, error) {
	entries, err := os.ReadDir(dir)
	if err != nil {
		return 0, err
	}
	gw := gzip.NewWriter(w)
	tw := tar.NewWriter(gw)
	count := 0
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		name := e.Name()
		if name == "qlog-runner.log" {
			continue
		}
		path := filepath.Join(dir, name)
		info, err := e.Info()
		if err != nil {
			continue
		}
		hdr, err := tar.FileInfoHeader(info, "")
		if err != nil {
			continue
		}
		hdr.Name = name
		if err := tw.WriteHeader(hdr); err != nil {
			_ = tw.Close()
			_ = gw.Close()
			return count, err
		}
		f, err := os.Open(path)
		if err != nil {
			continue
		}
		_, copyErr := io.Copy(tw, f)
		_ = f.Close()
		if copyErr != nil {
			_ = tw.Close()
			_ = gw.Close()
			return count, copyErr
		}
		count++
	}
	if err := tw.Close(); err != nil {
		_ = gw.Close()
		return count, err
	}
	if err := gw.Close(); err != nil {
		return count, err
	}
	return count, nil
}

func (h *CellularHandler) DebugQLogStatus(c *gin.Context) {
	Resp(c).OK(h.qlogStatusPayload())
}

func (h *CellularHandler) DebugQLogStart(c *gin.Context) {
	var req cellularQLogStartRequest
	_ = c.ShouldBindJSON(&req) // body optional
	if h.capabilityState() == cellularCapUnsupported {
		Resp(c).OK(gin.H{"ok": false, "message": "Cellular hardware is not available."})
		return
	}

	bin, err := h.findQLogBinary()
	if err != nil {
		Resp(c).OK(gin.H{"ok": false, "message": err.Error()})
		return
	}

	port := strings.TrimSpace(req.Port)
	if port == "" {
		port = findDMPort(h.preferredATPorts())
	}
	if port == "" {
		Resp(c).OK(gin.H{"ok": false, "message": "No DM port found (/dev/ttyUSB*)."})
		return
	}
	if !strings.HasPrefix(port, "/dev/") || strings.ContainsAny(port, " \t\r\n") {
		Resp(c).OK(gin.H{"ok": false, "message": "Invalid DM port."})
		return
	}
	if _, err := os.Stat(port); err != nil {
		Resp(c).OK(gin.H{"ok": false, "message": "DM port not present: " + port})
		return
	}

	maxMB := req.MaxMB
	if maxMB <= 0 {
		maxMB = qlogDefaultMaxMB
	}
	if maxMB < 2 {
		maxMB = 2
	}
	if maxMB > 512 {
		maxMB = 512
	}
	maxFiles := req.MaxFiles
	if maxFiles <= 0 {
		maxFiles = qlogDefaultMaxFiles
	}
	if maxFiles > 512 {
		maxFiles = 512
	}

	dir := h.qlogDir()
	if err := os.MkdirAll(dir, 0o755); err != nil {
		Resp(c).OK(gin.H{"ok": false, "message": "Cannot create log dir: " + err.Error()})
		return
	}

	h.qlogMu.Lock()
	defer h.qlogMu.Unlock()
	if h.qlogRunningLocked() {
		Resp(c).OK(gin.H{
			"ok":      true,
			"message": "QLog already running",
			"running": true,
			"port":    h.qlogPort,
			"dir":     dir,
		})
		return
	}
	h.killAllQLog()

	args := []string{
		"-p", port,
		"-s", dir,
		"-m", fmt.Sprintf("%d", maxMB),
		"-n", fmt.Sprintf("%d", maxFiles),
	}
	if cfg := qlogFilterCfg(); cfg != "" {
		args = append(args, "-f", cfg)
	}
	cmd := h.command(bin, args...)
	logFile, err := os.OpenFile(filepath.Join(dir, "qlog-runner.log"),
		os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0o644)
	if err == nil {
		cmd.Stdout = logFile
		cmd.Stderr = logFile
	}
	if err := cmd.Start(); err != nil {
		if logFile != nil {
			_ = logFile.Close()
		}
		Resp(c).OK(gin.H{"ok": false, "message": "Failed to start qlog: " + err.Error()})
		return
	}
	h.qlogCmd = cmd
	h.qlogPort = port
	h.qlogStartedAt = h.now()
	logger.Info("cellular: QLog started bin=%s port=%s dir=%s pid=%d", bin, port, dir, cmd.Process.Pid)

	go func(waitCmd *exec.Cmd, lf *os.File) {
		_ = waitCmd.Wait()
		if lf != nil {
			_ = lf.Close()
		}
		h.qlogMu.Lock()
		defer h.qlogMu.Unlock()
		if h.qlogCmd == waitCmd {
			h.qlogCmd = nil
			h.qlogPort = ""
			h.qlogStartedAt = time.Time{}
		}
	}(cmd, logFile)

	Resp(c).OK(gin.H{
		"ok":      true,
		"message": "QLog started",
		"running": true,
		"port":    port,
		"dir":     dir,
		"binary":  bin,
	})
}

func (h *CellularHandler) DebugQLogStop(c *gin.Context) {
	h.qlogMu.Lock()
	wasAlive := h.qlogRunningLocked()
	h.killAllQLog()
	stillAlive := h.qlogProcessAlive()
	h.qlogMu.Unlock()

	if !wasAlive && !stillAlive {
		Resp(c).OK(gin.H{"ok": true, "message": "QLog not running", "running": false})
		return
	}
	if stillAlive {
		logger.Warn("cellular: QLog stop signaled; process still exiting")
		Resp(c).OK(gin.H{
			"ok":      true,
			"message": "QLog stop signaled; waiting for process to exit",
			"running": true,
		})
		return
	}
	logger.Info("cellular: QLog stopped")
	Resp(c).OK(gin.H{"ok": true, "message": "QLog stopped", "running": false})
}

func (h *CellularHandler) DebugQLogExport(c *gin.Context) {
	dir := h.qlogDir()
	files, bytes := qlogDirStats(dir)
	if files == 0 || bytes == 0 {
		Resp(c).FailMsg(CodeNotFound, "No QLog files to export")
		return
	}
	name := fmt.Sprintf("cellular-qlog-%s.tar.gz", time.Now().UTC().Format("20060102-150405"))
	c.Header("Content-Type", "application/gzip")
	c.Header("Content-Disposition", fmt.Sprintf(`attachment; filename="%s"`, name))
	if _, err := writeQLogTarGz(c.Writer, dir); err != nil {
		logger.Warn("cellular: QLog export: %v", err)
	}
}

func (h *CellularHandler) DebugQLogClear(c *gin.Context) {
	h.qlogMu.Lock()
	defer h.qlogMu.Unlock()
	if h.qlogRunningLocked() {
		Resp(c).OK(gin.H{"ok": false, "message": "Stop QLog before clearing logs."})
		return
	}
	h.killAllQLog()
	dir := h.qlogDir()
	entries, err := os.ReadDir(dir)
	if err != nil {
		if os.IsNotExist(err) {
			Resp(c).OK(gin.H{"ok": true, "message": "Nothing to clear", "removed": 0})
			return
		}
		Resp(c).OK(gin.H{"ok": false, "message": err.Error()})
		return
	}
	removed := 0
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		path := filepath.Join(dir, e.Name())
		_ = os.Chmod(path, 0o644)
		if err := os.Remove(path); err == nil {
			removed++
		}
	}
	Resp(c).OK(gin.H{"ok": true, "message": "Cleared", "removed": removed})
}
