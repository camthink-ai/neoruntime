package handlers

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"github.com/gin-gonic/gin"
)

func writeTestHWProfile(t *testing.T, dir string) string {
	t.Helper()
	path := filepath.Join(dir, "cellular-hw.yaml")
	content := `
platform: test-board
module: test-modem
usb:
  devices:
    - vendor: "2c7c"
      products: ["0309", "0312"]
gpio:
  chip: gpiochip1
  power:
    name: wwan_pwr_on
    offset: 7
    active: high
    hold: true
  reset:
    name: wwan_reset_n
    offset: 5
    active: low
  module_detect_timeout_sec: 1
at:
  ports: [/dev/ttyUSB2]
  port_glob: /dev/ttyUSB*
control:
  globs: [/dev/cdc-wdm*]
net:
  ifaces: [wwan0, usb0]
dialer:
  binaries: [aipc-cellular, quectel-CM]
  style: quectel_cm
paths:
  sysfs_usb: ` + filepath.Join(dir, "usb") + `
  sysfs_net: ` + filepath.Join(dir, "net") + `
  run_dir: ` + filepath.Join(dir, "run") + `
  user_config: ` + filepath.Join(dir, "cellular.json") + `
`
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
	_ = os.MkdirAll(filepath.Join(dir, "usb"), 0755)
	_ = os.MkdirAll(filepath.Join(dir, "net"), 0755)
	return path
}

func newTestCellularHandler(t *testing.T) *CellularHandler {
	t.Helper()
	dir := t.TempDir()
	hwPath := writeTestHWProfile(t, dir)
	h := NewCellularHandler(nil, hwPath, nil)
	if h.hw == nil {
		t.Fatal("expected hw profile to load")
	}
	h.lookPath = func(string) (string, error) { return "", os.ErrNotExist }
	h.command = exec.Command
	h.now = time.Now
	return h
}

func TestCellularGetConfigDefaults(t *testing.T) {
	gin.SetMode(gin.TestMode)
	h := newTestCellularHandler(t)
	r := gin.New()
	r.GET("/cellular/config", h.GetConfig)

	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodGet, "/cellular/config", nil)
	r.ServeHTTP(w, req)
	if w.Code != 200 {
		t.Fatalf("status=%d body=%s", w.Code, w.Body.String())
	}
	var resp APIResponse
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	raw, _ := json.Marshal(resp.Data)
	var cfg CellularConfig
	if err := json.Unmarshal(raw, &cfg); err != nil {
		t.Fatal(err)
	}
	if !cfg.Enabled || cfg.Usbnet != "mbim" || cfg.PDPCID != 1 {
		t.Fatalf("unexpected defaults: %+v", cfg)
	}
	if cfg.NetworkMode != "auto" || roamingEnabled(cfg) {
		t.Fatalf("unexpected optional defaults: %+v", cfg)
	}
}

func TestCellularUpdateConfigAndStatus(t *testing.T) {
	gin.SetMode(gin.TestMode)
	h := newTestCellularHandler(t)
	// gpioset -m signal must stay alive past the 80ms hold check.
	// -m exit (reset pulse) must finish immediately — do not mock as sleep.
	h.command = func(name string, arg ...string) *exec.Cmd {
		if name == "gpioset" {
			for i, a := range arg {
				if a == "-m" && i+1 < len(arg) && arg[i+1] == "signal" {
					return exec.Command("sleep", "60")
				}
			}
			return exec.Command("true")
		}
		if name == "pgrep" {
			return exec.Command("false")
		}
		return exec.Command("true")
	}

	r := gin.New()
	r.PUT("/cellular/config", h.UpdateConfig)
	r.GET("/cellular/status", h.GetStatus)

	// Module must be on USB before / during enable wait, or enable auto-rolls back.
	dev := filepath.Join(h.sysfsUSB, "2-1")
	_ = os.MkdirAll(dev, 0755)
	_ = os.WriteFile(filepath.Join(dev, "idVendor"), []byte("2c7c\n"), 0644)
	_ = os.WriteFile(filepath.Join(dev, "idProduct"), []byte("0312\n"), 0644)
	_ = os.WriteFile(filepath.Join(dev, "product"), []byte("Quectel EM05-CN\n"), 0644)

	// Keep usbnet=mbim (factory default) so the test does not enter AT usbnet apply.
	body := `{"enabled":true,"apn":"internet","user":"","password":"","auth":"none","ip_type":"ipv4v6","pdp_cid":1,"carrier_profile":"generic","usbnet":"mbim","network_mode":"auto"}`
	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPut, "/cellular/config", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)
	if w.Code != 200 {
		t.Fatalf("update status=%d body=%s", w.Code, w.Body.String())
	}

	data, err := os.ReadFile(h.configPath)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(data), `"enabled": true`) {
		t.Fatalf("config not persisted: %s", data)
	}

	w2 := httptest.NewRecorder()
	req2 := httptest.NewRequest(http.MethodGet, "/cellular/status", nil)
	r.ServeHTTP(w2, req2)
	if w2.Code != 200 {
		t.Fatalf("status=%d body=%s", w2.Code, w2.Body.String())
	}
	var resp APIResponse
	if err := json.Unmarshal(w2.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	raw, _ := json.Marshal(resp.Data)
	var st CellularStatus
	if err := json.Unmarshal(raw, &st); err != nil {
		t.Fatal(err)
	}
	if !st.ModulePresent || st.Stub || st.Module != "Quectel EM05-CN" {
		t.Fatalf("unexpected status: %+v", st)
	}
}

func TestCellularUpdateRejectsBadAuth(t *testing.T) {
	gin.SetMode(gin.TestMode)
	h := newTestCellularHandler(t)
	r := gin.New()
	r.PUT("/cellular/config", h.UpdateConfig)

	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPut, "/cellular/config", strings.NewReader(
		`{"enabled":false,"apn":"","user":"","password":"","auth":"foo","ip_type":"ipv4","pdp_cid":1,"carrier_profile":"generic","usbnet":"mbim"}`,
	))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)
	if w.Code == 200 {
		t.Fatalf("expected validation error, got 200: %s", w.Body.String())
	}
}

func TestCellularEnableNoModuleAutoDisables(t *testing.T) {
	gin.SetMode(gin.TestMode)
	h := newTestCellularHandler(t)
	h.command = func(name string, arg ...string) *exec.Cmd {
		if name == "gpioset" {
			for i, a := range arg {
				if a == "-m" && i+1 < len(arg) && arg[i+1] == "signal" {
					return exec.Command("sleep", "60")
				}
			}
			return exec.Command("true")
		}
		return exec.Command("true")
	}
	disabled := defaultCellularConfig()
	disabled.Enabled = false
	if err := h.saveConfig(disabled); err != nil {
		t.Fatal(err)
	}

	r := gin.New()
	r.PUT("/cellular/config", h.UpdateConfig)
	body := `{"enabled":true,"apn":"internet","user":"","password":"","auth":"none","ip_type":"ipv4v6","pdp_cid":1,"carrier_profile":"generic","usbnet":"mbim","network_mode":"auto"}`
	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPut, "/cellular/config", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)

	var resp APIResponse
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	if resp.Code == 0 {
		t.Fatalf("expected failure when module missing, body=%s", w.Body.String())
	}
	if !strings.Contains(w.Body.String(), "No cellular module detected") {
		t.Fatalf("expected module-missing message, body=%s", w.Body.String())
	}
	cfg, err := h.loadConfig()
	if err != nil {
		t.Fatal(err)
	}
	if cfg.Enabled {
		t.Fatal("expected enabled=false after auto-disable")
	}
}

func TestCellularBootProbeUnsupportedPowersOff(t *testing.T) {
	h := newTestCellularHandler(t)
	powerOff := 0
	h.command = func(name string, arg ...string) *exec.Cmd {
		if name == "gpioset" {
			for i, a := range arg {
				if a == "-m" && i+1 < len(arg) && arg[i+1] == "signal" {
					// Hold ON uses signal; OFF may also use signal depending on drive.
					joined := strings.Join(arg, " ")
					if strings.Contains(joined, "=0") {
						powerOff++
					}
					return exec.Command("sleep", "60")
				}
			}
			return exec.Command("true")
		}
		return exec.Command("true")
	}
	h.runCapabilityProbe()
	if h.capabilityState() != cellularCapUnsupported || h.cellularSupported() {
		t.Fatalf("capability=%s want unsupported", h.capabilityState())
	}
	st := h.probeStatus(defaultCellularConfig())
	if st.Supported || st.Capability != cellularCapUnsupported {
		t.Fatalf("status=%+v", st)
	}
	if powerOff == 0 {
		t.Fatal("expected power-off after failed probe")
	}
}

func TestCellularBootProbeSupportedWhenUSBPresent(t *testing.T) {
	h := newTestCellularHandler(t)
	dev := filepath.Join(h.sysfsUSB, "1-1")
	_ = os.MkdirAll(dev, 0755)
	_ = os.WriteFile(filepath.Join(dev, "idVendor"), []byte("2c7c\n"), 0644)
	_ = os.WriteFile(filepath.Join(dev, "idProduct"), []byte("0309\n"), 0644)

	h.runCapabilityProbe()
	if !h.cellularSupported() || h.capabilityState() != cellularCapSupported {
		t.Fatalf("capability=%s", h.capabilityState())
	}
	st := h.probeStatus(defaultCellularConfig())
	if !st.Supported || st.Capability != cellularCapSupported {
		t.Fatalf("status=%+v", st)
	}
}

func TestCellularPromoteUnsupportedWhenUSBAppears(t *testing.T) {
	h := newTestCellularHandler(t)
	h.setCapability(cellularCapUnsupported)
	st := h.probeStatus(defaultCellularConfig())
	if st.Supported || st.Capability != cellularCapUnsupported {
		t.Fatalf("expected unsupported without USB: %+v", st)
	}

	dev := filepath.Join(h.sysfsUSB, "1-1")
	_ = os.MkdirAll(dev, 0755)
	_ = os.WriteFile(filepath.Join(dev, "idVendor"), []byte("2c7c\n"), 0644)
	_ = os.WriteFile(filepath.Join(dev, "idProduct"), []byte("0312\n"), 0644) // EM05-CN

	st = h.probeStatus(defaultCellularConfig())
	if !st.Supported || st.Capability != cellularCapSupported || !st.ModulePresent {
		t.Fatalf("expected promote on USB: %+v", st)
	}
}

func TestCellularCapabilityViewNoAT(t *testing.T) {
	h := newTestCellularHandler(t)
	h.setCapability(cellularCapSupported)
	dev := filepath.Join(h.sysfsUSB, "1-1")
	_ = os.MkdirAll(dev, 0755)
	_ = os.WriteFile(filepath.Join(dev, "idVendor"), []byte("2c7c\n"), 0644)
	_ = os.WriteFile(filepath.Join(dev, "idProduct"), []byte("0312\n"), 0644)
	_ = os.WriteFile(filepath.Join(dev, "product"), []byte("Quectel EM05-G\n"), 0644)

	v := h.capabilityView()
	if v.Capability != cellularCapSupported || !v.ModulePresent {
		t.Fatalf("capability view: %+v", v)
	}
}

func TestCellularStickyModuleAcrossReenum(t *testing.T) {
	h := newTestCellularHandler(t)
	h.setCapability(cellularCapSupported)
	fixed := time.Date(2026, 10, 29, 14, 0, 0, 0, time.UTC)
	h.now = func() time.Time { return fixed }

	dev := filepath.Join(h.sysfsUSB, "1-1")
	_ = os.MkdirAll(dev, 0755)
	_ = os.WriteFile(filepath.Join(dev, "idVendor"), []byte("2c7c\n"), 0644)
	_ = os.WriteFile(filepath.Join(dev, "idProduct"), []byte("0312\n"), 0644)
	_ = os.WriteFile(filepath.Join(dev, "product"), []byte("Quectel EM05-G\n"), 0644)

	cfg := defaultCellularConfig()
	st := h.probeStatus(cfg)
	if !st.ModulePresent || st.Module != "Quectel EM05-G" {
		t.Fatalf("live USB: %+v", st)
	}

	_ = os.RemoveAll(dev)
	fixed = fixed.Add(5 * time.Second)
	st = h.probeStatus(cfg)
	if !st.ModulePresent || st.Module != "Quectel EM05-G" {
		t.Fatalf("sticky during re-enum: %+v", st)
	}
}

func TestCellularUpdateRejectsWhenUnsupported(t *testing.T) {
	gin.SetMode(gin.TestMode)
	h := newTestCellularHandler(t)
	h.setCapability(cellularCapUnsupported)
	r := gin.New()
	r.PUT("/cellular/config", h.UpdateConfig)
	body := `{"enabled":true,"apn":"internet","user":"","password":"","auth":"none","ip_type":"ipv4v6","pdp_cid":1,"carrier_profile":"generic","usbnet":"mbim","network_mode":"auto"}`
	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPut, "/cellular/config", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	r.ServeHTTP(w, req)
	var resp APIResponse
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatal(err)
	}
	if resp.Code == 0 {
		t.Fatalf("expected reject, body=%s", w.Body.String())
	}
	if !strings.Contains(w.Body.String(), "not available") {
		t.Fatalf("body=%s", w.Body.String())
	}
}

func TestCellularConnectRequiresEnabled(t *testing.T) {
	gin.SetMode(gin.TestMode)
	h := newTestCellularHandler(t)
	disabled := defaultCellularConfig()
	disabled.Enabled = false
	if err := h.saveConfig(disabled); err != nil {
		t.Fatal(err)
	}
	r := gin.New()
	r.POST("/cellular/connect", h.Connect)

	w := httptest.NewRecorder()
	req := httptest.NewRequest(http.MethodPost, "/cellular/connect", nil)
	r.ServeHTTP(w, req)
	if w.Code != 200 {
		t.Fatalf("status=%d body=%s", w.Code, w.Body.String())
	}
	if !strings.Contains(w.Body.String(), "Enable cellular") {
		t.Fatalf("body=%s", w.Body.String())
	}
}

func TestDialerArgsQuectel(t *testing.T) {
	args := dialerArgs("quectel_cm", CellularConfig{
		APN: "vzwinternet", Auth: "none", PDPCID: 3, IPType: "ipv4v6",
	})
	want := []string{"-s", "vzwinternet", "-4", "-6", "-n", "3"}
	if len(args) != len(want) {
		t.Fatalf("args=%v want=%v", args, want)
	}
	for i := range want {
		if args[i] != want[i] {
			t.Fatalf("args=%v want=%v", args, want)
		}
	}
}

func TestDialerArgsQuectelPAP(t *testing.T) {
	args := dialerArgs("quectel_cm", CellularConfig{
		APN: "3gnet", User: "u1", Password: "p1", Auth: "pap", PDPCID: 1, IPType: "ipv4",
	})
	want := []string{"-s", "3gnet", "u1", "p1", "1", "-4", "-n", "1"}
	if len(args) != len(want) {
		t.Fatalf("args=%v want=%v", args, want)
	}
	for i := range want {
		if args[i] != want[i] {
			t.Fatalf("args=%v want=%v", args, want)
		}
	}
}

func TestFindMatchedUSBFromProfile(t *testing.T) {
	h := newTestCellularHandler(t)
	dev := filepath.Join(h.sysfsUSB, "1-1.2")
	_ = os.MkdirAll(dev, 0755)
	_ = os.WriteFile(filepath.Join(dev, "idVendor"), []byte("2c7c"), 0644)
	_ = os.WriteFile(filepath.Join(dev, "idProduct"), []byte("0309"), 0644)
	vid, pid, _ := h.findMatchedUSB()
	if vid != "2c7c" || pid != "0309" {
		t.Fatalf("got %s:%s", vid, pid)
	}
}

func TestLoadCellularHWProfile(t *testing.T) {
	dir := t.TempDir()
	path := writeTestHWProfile(t, dir)
	hw, err := LoadCellularHWProfile(path)
	if err != nil {
		t.Fatal(err)
	}
	if hw.Platform != "test-board" || hw.Module != "test-modem" {
		t.Fatalf("%+v", hw)
	}
	if hw.Dialer.Style != "quectel_cm" {
		t.Fatalf("style=%s", hw.Dialer.Style)
	}
	if hw.AT.PortGlob != "/dev/ttyUSB*" || len(hw.Control.Globs) != 1 {
		t.Fatalf("at=%+v control=%+v", hw.AT, hw.Control)
	}
	if hw.GPIO.Power.Active != "high" || hw.GPIO.Reset.Active != "low" {
		t.Fatalf("gpio polarity power=%q reset=%q", hw.GPIO.Power.Active, hw.GPIO.Reset.Active)
	}
}

func TestLoadCellularHWProfileRequiresBoardFields(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "bad.yaml")
	content := `
usb:
  devices:
    - vendor: "2c7c"
dialer:
  binaries: [aipc-cellular]
  style: aipc
`
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := LoadCellularHWProfile(path); err == nil {
		t.Fatal("expected validation error for missing paths/net")
	}
}

func TestResolveGPIOTargetPrefersYAMLOffset(t *testing.T) {
	h := newTestCellularHandler(t)
	called := []string{}
	h.command = func(name string, arg ...string) *exec.Cmd {
		called = append(called, name)
		return exec.Command("false")
	}
	chip, offset, err := h.resolveGPIOTarget(h.hw.GPIO.Power)
	if err != nil {
		t.Fatal(err)
	}
	if chip != "gpiochip1" || offset != "7" {
		t.Fatalf("got chip=%q offset=%q", chip, offset)
	}
	for _, c := range called {
		if c == "gpioinfo" || c == "gpiofind" {
			t.Fatalf("YAML offset should not invoke %s", c)
		}
	}
}

func TestResolveGPIOTargetViaGpiofind(t *testing.T) {
	h := newTestCellularHandler(t)
	h.hw.GPIO.Reset.Offset = nil
	h.hw.GPIO.Reset.Name = "wwan_reset_n"
	h.hw.GPIO.Chip = "gpiochip0" // stale; gpiofind should win
	h.command = func(name string, arg ...string) *exec.Cmd {
		if name == "gpiofind" {
			return exec.Command("echo", "gpiochip1 5")
		}
		return exec.Command("false")
	}
	chip, offset, err := h.resolveGPIOTarget(h.hw.GPIO.Reset)
	if err != nil {
		t.Fatal(err)
	}
	if chip != "gpiochip1" || offset != "5" {
		t.Fatalf("got chip=%q offset=%q", chip, offset)
	}
}
