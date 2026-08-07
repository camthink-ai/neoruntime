package gyro

import (
	"context"
	"math"
	"os"
	"path/filepath"
	"testing"
	"time"
)

// makeFakeDevice creates a fake iio:deviceK dir under base exposing the three
// axis raw files and the scale file for the given channel kind, with the
// supplied initial contents. It returns the device directory path.
func makeFakeDevice(t *testing.T, base, devName, kind, scale string, raw [3]string) string {
	t.Helper()
	dir := filepath.Join(base, devName)
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatalf("mkdir %s: %v", dir, err)
	}
	axes := [3]string{"x", "y", "z"}
	for i, ax := range axes {
		p := filepath.Join(dir, "in_"+kind+"_"+ax+"_raw")
		if err := os.WriteFile(p, []byte(raw[i]+"\n"), 0o644); err != nil {
			t.Fatalf("write %s: %v", p, err)
		}
	}
	sp := filepath.Join(dir, "in_"+kind+"_scale")
	if err := os.WriteFile(sp, []byte(scale+"\n"), 0o644); err != nil {
		t.Fatalf("write %s: %v", sp, err)
	}
	return dir
}

func writeFile(t *testing.T, path, content string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatalf("write %s: %v", path, err)
	}
}

func TestFileExists(t *testing.T) {
	tmp := t.TempDir()
	p := filepath.Join(tmp, "x")
	writeFile(t, p, "hi")
	if !fileExists(p) {
		t.Error("fileExists reported false for existing file")
	}
	if fileExists(filepath.Join(tmp, "missing")) {
		t.Error("fileExists reported true for missing file")
	}
}

func TestReadSeekHelpers(t *testing.T) {
	t.Run("int", func(t *testing.T) {
		f, err := os.OpenFile(filepath.Join(t.TempDir(), "v"), os.O_RDWR|os.O_CREATE, 0o644)
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		writeFile(t, f.Name(), "42\n")
		got, err := readSeekInt(f)
		if err != nil || got != 42 {
			t.Fatalf("readSeekInt: got %v err %v", got, err)
		}
		// Re-read after overwrite through the held fd.
		writeFile(t, f.Name(), "-7\n")
		got, err = readSeekInt(f)
		if err != nil || got != -7 {
			t.Fatalf("readSeekInt reread: got %v err %v", got, err)
		}
	})
	t.Run("float", func(t *testing.T) {
		f, err := os.OpenFile(filepath.Join(t.TempDir(), "s"), os.O_RDWR|os.O_CREATE, 0o644)
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		writeFile(t, f.Name(), "0.000244\n")
		got, err := readSeekFloat(f)
		if err != nil || got != 0.000244 {
			t.Fatalf("readSeekFloat: got %v err %v", got, err)
		}
	})
	t.Run("multi-read long line", func(t *testing.T) {
		// A value longer than the 32-byte read buffer exercises the read loop.
		f, err := os.OpenFile(filepath.Join(t.TempDir(), "long"), os.O_RDWR|os.O_CREATE, 0o644)
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		long := "12345678901234567890123456789012345678901234567890"
		writeFile(t, f.Name(), long+"\n")
		s, err := readSeek(f)
		if err != nil {
			t.Fatalf("readSeek: %v", err)
		}
		if s != long {
			t.Errorf("readSeek: got %q, want %q", s, long)
		}
	})
	t.Run("no trailing newline", func(t *testing.T) {
		f, err := os.OpenFile(filepath.Join(t.TempDir(), "nl"), os.O_RDWR|os.O_CREATE, 0o644)
		if err != nil {
			t.Fatal(err)
		}
		defer f.Close()
		writeFile(t, f.Name(), "99")
		got, err := readSeekInt(f)
		if err != nil || got != 99 {
			t.Fatalf("readSeekInt no-newline: got %v err %v", got, err)
		}
	})
}

func TestDiscoverDevice(t *testing.T) {
	base := t.TempDir()
	makeFakeDevice(t, base, "iio:device0", "anglvel", "0.0001", [3]string{"1", "2", "3"})
	makeFakeDevice(t, base, "iio:device1", "accel", "0.0002", [3]string{"4", "5", "6"})

	if d, err := discoverDevice(base, "anglvel"); err != nil || filepath.Base(d) != "iio:device0" {
		t.Fatalf("anglvel: dir=%q err=%v", d, err)
	}
	if d, err := discoverDevice(base, "accel"); err != nil || filepath.Base(d) != "iio:device1" {
		t.Fatalf("accel: dir=%q err=%v", d, err)
	}
	if _, err := discoverDevice(base, "mag"); err == nil {
		t.Error("expected error for missing kind")
	}
	if _, err := discoverDevice(filepath.Join(base, "nope"), "accel"); err == nil {
		t.Error("expected error for missing base")
	}
}

func TestOpenDeviceRead(t *testing.T) {
	base := t.TempDir()
	dir := makeFakeDevice(t, base, "iio:device0", "anglvel", "0.001", [3]string{"1000", "-500", "0"})

	d, err := openDevice(dir, "anglvel", 0)
	if err != nil {
		t.Fatalf("openDevice: %v", err)
	}
	defer d.close()

	now := time.Now()
	v, err := d.read(now)
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	// raw * scale per axis.
	if v[0] != 1.0 || v[1] != -0.5 || v[2] != 0 {
		t.Errorf("read values: got %v, want [1, -0.5, 0]", v)
	}

	// Overwrite a raw value and re-read through the held fd (seek+read).
	writeFile(t, filepath.Join(dir, "in_anglvel_x_raw"), "2000\n")
	v, _ = d.read(now)
	if v[0] != 2.0 {
		t.Errorf("reread x: got %v, want 2.0", v[0])
	}
}

func TestOpenDeviceScaleRefresh(t *testing.T) {
	base := t.TempDir()
	dir := makeFakeDevice(t, base, "iio:device0", "anglvel", "0.001", [3]string{"1000", "0", "0"})
	scalePath := filepath.Join(dir, "in_anglvel_scale") // same file the open fd points at

	d, err := openDevice(dir, "anglvel", 0)
	if err != nil {
		t.Fatalf("openDevice: %v", err)
	}
	defer d.close()

	// Force the scale to be considered stale so read() re-reads it.
	writeFile(t, scalePath, "0.002\n")
	d.scaleAt = time.Time{} // zero time => now - scaleAt >> scaleRefresh

	v, err := d.read(time.Now())
	if err != nil {
		t.Fatalf("read: %v", err)
	}
	if v[0] != 2.0 { // 1000 * 0.002 after refresh
		t.Errorf("scale refresh: got %v, want 2.0", v[0])
	}
}

func TestOpenDeviceMissingFile(t *testing.T) {
	base := t.TempDir()
	// accel dir with only x_raw present -> openDevice must fail on y.
	dir := filepath.Join(base, "iio:device0")
	if err := os.MkdirAll(dir, 0o755); err != nil {
		t.Fatal(err)
	}
	writeFile(t, filepath.Join(dir, "in_accel_x_raw"), "1\n")
	if _, err := openDevice(dir, "accel", 0); err == nil {
		t.Error("expected openDevice error for incomplete device")
	}
}

func TestWriteODR(t *testing.T) {
	t.Run("skip when hz <= 0", func(t *testing.T) {
		dir := t.TempDir()
		writeODR(dir, 0)
		writeODR(dir, -5)
		if fileExists(filepath.Join(dir, "sampling_frequency")) {
			t.Error("writeODR(<=0) wrote sampling_frequency")
		}
	})
	t.Run("writes ASCII int when hz > 0", func(t *testing.T) {
		dir := t.TempDir()
		writeODR(dir, 416)
		b, err := os.ReadFile(filepath.Join(dir, "sampling_frequency"))
		if err != nil {
			t.Fatalf("read back sampling_frequency: %v", err)
		}
		if string(b) != "416" {
			t.Errorf("sampling_frequency: got %q, want %q", b, "416")
		}
	})
	t.Run("missing dir is best-effort (no panic)", func(t *testing.T) {
		// Real devices always have sampling_frequency, but a fake/incomplete dir
		// must not break openDevice — writeODR logs and continues.
		writeODR(filepath.Join(t.TempDir(), "does-not-exist"), 416)
	})
}

func TestNewIIOSourceDefaults(t *testing.T) {
	s := NewIIOSource(IIOSourceConfig{})
	if s.cfg.IIOBase != defaultIIOBase {
		t.Errorf("IIOBase default: got %q, want %q", s.cfg.IIOBase, defaultIIOBase)
	}
	if s.cfg.PollRateHz != 100 {
		t.Errorf("PollRateHz default: got %d, want 100", s.cfg.PollRateHz)
	}
	if s.Status() != StatusOffline {
		t.Errorf("initial status: got %q, want offline", s.Status())
	}
}

func TestIIOSourcePeriodClamp(t *testing.T) {
	cases := []struct {
		hz   int
		want time.Duration
	}{
		{100, time.Second / 100},
		{0, time.Second / 100},     // -> default 100
		{-1, time.Second / 100},    // -> default 100
		{2000, time.Second / 1000}, // clamped to 1000
	}
	for _, c := range cases {
		s := NewIIOSource(IIOSourceConfig{PollRateHz: c.hz})
		if got := s.period(); got != c.want {
			t.Errorf("period(%d): got %v, want %v", c.hz, got, c.want)
		}
	}
}

func TestProbeAndOpenOfflineToOnline(t *testing.T) {
	base := t.TempDir() // empty: no devices
	s := NewIIOSource(IIOSourceConfig{IIOBase: base, PollRateHz: 100})

	s.probeAndOpen()
	if s.Status() != StatusOffline {
		t.Fatalf("empty base: status=%q, want offline", s.Status())
	}

	// Hot-plug the fake devices and re-probe.
	makeFakeDevice(t, base, "iio:device0", "anglvel", "0.0001", [3]string{"0", "0", "0"})
	makeFakeDevice(t, base, "iio:device1", "accel", "0.0002", [3]string{"0", "0", "0"})
	s.probeAndOpen()
	if s.Status() != StatusOnline {
		t.Fatalf("after devices: status=%q, want online", s.Status())
	}
	s.mu.Lock()
	hasDevs := s.accelDev != nil && s.gyroDev != nil
	s.mu.Unlock()
	if !hasDevs {
		t.Error("devices not opened despite online status")
	}

	// setStatus path + Status() read.
	s.setStatus(StatusError)
	if s.Status() != StatusError {
		t.Errorf("setStatus error: got %q", s.Status())
	}
}

func TestProbeAndOpenExplicitPaths(t *testing.T) {
	base := t.TempDir()
	accelDir := makeFakeDevice(t, base, "iio:device9", "accel", "0.001", [3]string{"1", "2", "3"})
	gyroDir := makeFakeDevice(t, base, "iio:device7", "anglvel", "0.001", [3]string{"4", "5", "6"})
	s := NewIIOSource(IIOSourceConfig{
		IIOBase:   base,
		AccelPath: accelDir,
		GyroPath:  gyroDir,
	})
	s.probeAndOpen()
	if s.Status() != StatusOnline {
		t.Fatalf("explicit paths: status=%q, want online", s.Status())
	}
}

func TestProbeAndOpenPartialFailsCleanly(t *testing.T) {
	base := t.TempDir()
	accelDir := makeFakeDevice(t, base, "iio:device0", "accel", "0.001", [3]string{"1", "2", "3"})
	// Gyro dir missing entirely -> probe must fail and leave status non-online,
	// not crash.
	s := NewIIOSource(IIOSourceConfig{IIOBase: base, AccelPath: accelDir})
	s.probeAndOpen()
	if s.Status() == StatusOnline {
		t.Errorf("expected non-online status when gyro missing, got %q", s.Status())
	}
}

func TestReopenLocked(t *testing.T) {
	base := t.TempDir()
	makeFakeDevice(t, base, "iio:device0", "anglvel", "0.0001", [3]string{"0", "0", "0"})
	makeFakeDevice(t, base, "iio:device1", "accel", "0.0002", [3]string{"0", "0", "0"})
	s := NewIIOSource(IIOSourceConfig{IIOBase: base, PollRateHz: 100})
	s.probeAndOpen()
	if s.Status() != StatusOnline {
		t.Fatalf("setup: status=%q, want online", s.Status())
	}

	s.reopenLocked()
	s.mu.Lock()
	empty := s.accelDev == nil && s.gyroDev == nil
	s.mu.Unlock()
	if !empty {
		t.Error("reopenLocked did not clear device handles")
	}

	// A subsequent probe reopens the devices (mirrors the Start() recovery path).
	s.probeAndOpen()
	if s.Status() != StatusOnline {
		t.Errorf("after reopen+probe: status=%q, want online", s.Status())
	}
}

func TestIIOSourceStartLifecycle(t *testing.T) {
	base := t.TempDir()
	// Accel ~ level gravity (raw * 0.000244 ~= 9.76 on Z, inside the gravity
	// band), gyro zero -> the estimator should stay near identity.
	makeFakeDevice(t, base, "iio:device0", "anglvel", "0.0001", [3]string{"0", "0", "0"})
	makeFakeDevice(t, base, "iio:device1", "accel", "0.000244", [3]string{"0", "0", "40000"})

	s := NewIIOSource(IIOSourceConfig{IIOBase: base, PollRateHz: 200})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { s.Start(ctx); close(done) }()

	sub := s.Subscribe()
	select {
	case got := <-sub:
		if s.Status() != StatusOnline {
			t.Errorf("status after first sample: %q, want online", s.Status())
		}
		q := got.Quat
		n := math.Sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3])
		if math.Abs(n-1) > 1e-6 {
			t.Errorf("published quaternion not normalized: |q|=%v", n)
		}
		if !got.TiltValid {
			t.Fatal("published sample should include fast accelerometer tilt")
		}
		if math.Abs(got.PitchDeg) > 0.1 || math.Abs(got.RollDeg) > 0.1 {
			t.Fatalf("published level tilt = %.3f/%.3f, want near zero", got.PitchDeg, got.RollDeg)
		}
	case <-time.After(2 * time.Second):
		cancel()
		t.Fatalf("no sample received; status=%q", s.Status())
	}

	cancel()
	select {
	case <-done:
	case <-time.After(2 * time.Second):
		t.Fatal("Start did not return after context cancel")
	}
	if s.Status() != StatusOffline {
		t.Errorf("after stop: status=%q, want offline", s.Status())
	}
}

// --- broker ---

func TestBrokerPubSub(t *testing.T) {
	s := NewIIOSource(IIOSourceConfig{PollRateHz: 100})
	ch := s.Subscribe()
	defer s.Unsubscribe(ch)

	want := Sample{Quat: [4]float64{0.1, 0.2, 0.3, 0.4}}
	s.publish(want)

	select {
	case got := <-ch:
		if got.Quat != want.Quat {
			t.Errorf("got %v, want %v", got.Quat, want.Quat)
		}
	case <-time.After(time.Second):
		t.Fatal("timed out waiting for sample")
	}
}

func TestBrokerDropOldest(t *testing.T) {
	// With no reader draining, publishing more than the buffer must drop the
	// oldest samples and never block the read loop.
	s := NewIIOSource(IIOSourceConfig{PollRateHz: 100})
	ch := s.Subscribe()
	defer s.Unsubscribe(ch)

	const n = subBufferSize + 2
	for i := 0; i < n; i++ {
		s.publish(Sample{Quat: [4]float64{float64(i), 0, 0, 1}})
	}

	// The buffer should now hold the newest subBufferSize samples: indices 2..n-1.
	var firstIdx float64 = -1
	for i := 0; i < subBufferSize; i++ {
		select {
		case got := <-ch:
			if firstIdx == -1 {
				firstIdx = got.Quat[0]
			}
		case <-time.After(time.Second):
			t.Fatalf("timed out draining sample %d", i)
		}
	}
	if firstIdx != 2 {
		t.Errorf("drop-oldest first index: got %v, want 2", firstIdx)
	}

	// Channel must be empty now.
	select {
	case extra := <-ch:
		t.Errorf("channel not empty after drain; extra=%v", extra)
	default:
	}
}

func TestBrokerUnsubscribeStopsDelivery(t *testing.T) {
	s := NewIIOSource(IIOSourceConfig{PollRateHz: 100})
	sub := s.Subscribe()
	s.publish(Sample{Quat: [4]float64{1, 0, 0, 1}})
	<-sub // drain the pre-unsubscribe sample
	s.Unsubscribe(sub)

	// Must not panic and must not deliver after unsubscribe.
	s.publish(Sample{Quat: [4]float64{2, 0, 0, 1}})
	select {
	case got := <-sub:
		t.Errorf("received sample after unsubscribe: %v", got)
	case <-time.After(50 * time.Millisecond):
	}
}

func TestBrokerMultipleSubscribers(t *testing.T) {
	s := NewIIOSource(IIOSourceConfig{PollRateHz: 100})
	a := s.Subscribe()
	b := s.Subscribe()
	defer s.Unsubscribe(a)
	defer s.Unsubscribe(b)

	s.publish(Sample{Quat: [4]float64{5, 0, 0, 1}})
	for _, ch := range []<-chan Sample{a, b} {
		select {
		case <-ch:
		case <-time.After(time.Second):
			t.Fatal("subscriber did not receive broadcast")
		}
	}
}

func TestMeanStdGyro(t *testing.T) {
	// Known symmetric samples: population mean {2,2,2}, population std {1,1,1}.
	samples := [][3]float64{
		{1, 1, 1},
		{3, 3, 3},
	}
	mean, std := meanStdGyro(samples)
	wantMean := [3]float64{2, 2, 2}
	wantStd := [3]float64{1, 1, 1}
	const tol = 1e-9
	for i := 0; i < 3; i++ {
		if math.Abs(mean[i]-wantMean[i]) > tol {
			t.Errorf("mean[%d]: got %v, want %v", i, mean[i], wantMean[i])
		}
		if math.Abs(std[i]-wantStd[i]) > tol {
			t.Errorf("std[%d]: got %v, want %v", i, std[i], wantStd[i])
		}
	}

	// Empty input must return zero vectors without dividing by zero.
	mean0, std0 := meanStdGyro(nil)
	if mean0 != ([3]float64{}) || std0 != ([3]float64{}) {
		t.Errorf("empty: mean=%v std=%v, want zero vectors", mean0, std0)
	}
}

func TestDecideBias(t *testing.T) {
	// Clean window: every axis std inside the still-gate -> install each mean.
	cleanMean := [3]float64{0.01, -0.02, 0.005}
	cleanStd := [3]float64{stillGateRps * 0.5, stillGateRps * 0.5, stillGateRps * 0.5}
	bias, accepted := decideBias(cleanMean, cleanStd)
	if accepted != ([3]bool{true, true, true}) {
		t.Errorf("clean window accepted: got %v, want all true", accepted)
	}
	if bias != cleanMean {
		t.Errorf("clean window bias: got %v, want %v", bias, cleanMean)
	}

	// Noisy Z only: X/Y still accepted (they drive tilt), Z skipped (drives yaw,
	// which accel cannot correct anyway). This is the case that produced the
	// level-device tilt offset before per-axis gating.
	noisyStd := [3]float64{stillGateRps * 0.5, stillGateRps * 0.5, stillGateRps * 5}
	bias, accepted = decideBias(cleanMean, noisyStd)
	if want := [3]bool{true, true, false}; accepted != want {
		t.Errorf("noisy-Z accepted: got %v, want %v", accepted, want)
	}
	wantBias := [3]float64{cleanMean[0], cleanMean[1], 0}
	if bias != wantBias {
		t.Errorf("noisy-Z bias: got %v, want %v (Z zeroed)", bias, wantBias)
	}

	// Every axis noisy -> all skipped; the caller should retry instead of
	// permanently treating zero as a valid calibration.
	allNoisy := [3]float64{stillGateRps * 5, stillGateRps * 5, stillGateRps * 5}
	bias, accepted = decideBias(cleanMean, allNoisy)
	if accepted != ([3]bool{false, false, false}) {
		t.Errorf("all-noisy accepted: got %v, want all false", accepted)
	}
	if bias != ([3]float64{}) {
		t.Errorf("all-noisy bias: got %v, want zero vector", bias)
	}

	// Exactly at the gate is the boundary; treat > as noisy, so == is accepted.
	borderStd := [3]float64{stillGateRps, stillGateRps, stillGateRps}
	if _, accepted := decideBias(cleanMean, borderStd); accepted != ([3]bool{true, true, true}) {
		t.Error("boundary std == gate should still pass on every axis")
	}
}

func TestUpdateBiasFromWindowAccumulatesReadyAxes(t *testing.T) {
	currentBias := [3]float64{}
	currentReady := [3]bool{}

	mean1 := [3]float64{0.01, -0.02, 0.03}
	stdXOnlyClean := [3]float64{stillGateRps * 0.5, stillGateRps * 5, stillGateRps * 5}
	bias, ready, accepted := updateBiasFromWindow(currentBias, currentReady, mean1, stdXOnlyClean)
	if accepted != ([3]bool{true, false, false}) {
		t.Fatalf("first window accepted: got %v, want X only", accepted)
	}
	if ready != ([3]bool{true, false, false}) {
		t.Fatalf("first window ready: got %v, want X only", ready)
	}
	if bias != ([3]float64{mean1[0], 0, 0}) {
		t.Fatalf("first window bias: got %v", bias)
	}
	if tiltBiasReady(ready) {
		t.Fatal("tilt bias should not be ready until both X and Y have clean windows")
	}

	mean2 := [3]float64{0.11, -0.22, 0.33}
	stdYOnlyClean := [3]float64{stillGateRps * 5, stillGateRps * 0.5, stillGateRps * 5}
	bias, ready, accepted = updateBiasFromWindow(bias, ready, mean2, stdYOnlyClean)
	if accepted != ([3]bool{false, true, false}) {
		t.Fatalf("second window accepted: got %v, want Y only", accepted)
	}
	if ready != ([3]bool{true, true, false}) {
		t.Fatalf("second window ready: got %v, want X/Y ready", ready)
	}
	if bias != ([3]float64{mean1[0], mean2[1], 0}) {
		t.Fatalf("second window bias: got %v", bias)
	}
	if !tiltBiasReady(ready) {
		t.Fatal("tilt bias should be ready once X and Y have clean windows")
	}
}

func TestCalSampleCount(t *testing.T) {
	cases := []struct {
		hz   int
		ms   int
		want int
	}{
		{12, 2000, 24},   // default-ish: 2s * 12Hz
		{100, 200, 20},   // 0.2s * 100Hz
		{200, 2000, 200}, // clamped to ceiling
		{1, 1000, 12},    // 1 sample -> floored to 12
		{100, 0, 200},    // ms unset -> 2000ms * 100Hz = 200 (ceiling)
	}
	for _, c := range cases {
		s := NewIIOSource(IIOSourceConfig{PollRateHz: c.hz, CalibrationMs: c.ms})
		if got := s.calSampleCount(); got != c.want {
			t.Errorf("calSampleCount(hz=%d ms=%d): got %d, want %d", c.hz, c.ms, got, c.want)
		}
	}
}

// TestIIOSourceCalibratesBias drives Start past the calibration window and
// asserts the injected constant gyro offset becomes the installed bias.
func TestIIOSourceCalibratesBias(t *testing.T) {
	base := t.TempDir()
	// scale=0.001, raw {10,-20,15} -> body-frame offset {0.01,-0.02,0.015} rad/s.
	const scale = 0.001
	wantBias := [3]float64{10 * scale, -20 * scale, 15 * scale}
	makeFakeDevice(t, base, "iio:device0", "anglvel", "0.001", [3]string{"10", "-20", "15"})
	makeFakeDevice(t, base, "iio:device1", "accel", "0.000244", [3]string{"0", "0", "40000"})

	s := NewIIOSource(IIOSourceConfig{
		IIOBase:       base,
		PollRateHz:    200,
		CalibrateBias: true,
		CalibrationMs: 200, // ~40 samples at 200Hz, within [12,200]
	})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { s.Start(ctx); close(done) }()

	sub := s.Subscribe()
	deadline := time.After(2 * time.Second)
	for i := 0; i < 60; i++ { // drain past the ~40-sample calibration window
		select {
		case <-sub:
		case <-deadline:
			cancel()
			<-done
			t.Fatalf("only got %d samples before timeout; status=%q", i, s.Status())
		}
	}
	cancel()
	<-done

	got := s.fusion.GyroBias()
	const tol = 1e-6
	for i := 0; i < 3; i++ {
		if math.Abs(got[i]-wantBias[i]) > tol {
			t.Errorf("bias[%d]: got %.6f, want %.6f", i, got[i], wantBias[i])
		}
	}
}

// TestIIOSourceNoCalibrationWhenDisabled asserts the bias stays zero when
// CalibrateBias is false even though the gyro reports a non-zero offset.
func TestIIOSourceNoCalibrationWhenDisabled(t *testing.T) {
	base := t.TempDir()
	makeFakeDevice(t, base, "iio:device0", "anglvel", "0.001", [3]string{"10", "-20", "15"})
	makeFakeDevice(t, base, "iio:device1", "accel", "0.000244", [3]string{"0", "0", "40000"})

	s := NewIIOSource(IIOSourceConfig{
		IIOBase:       base,
		PollRateHz:    200,
		CalibrateBias: false,
	})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { s.Start(ctx); close(done) }()

	sub := s.Subscribe()
	timer := time.After(2 * time.Second)
	for i := 0; i < 30; i++ { // enough samples that calibration would have fired if enabled
		select {
		case <-sub:
		case <-timer:
			cancel()
			<-done
			t.Fatalf("only got %d samples; status=%q", i, s.Status())
		}
	}
	cancel()
	<-done

	if got := s.fusion.GyroBias(); got != ([3]float64{}) {
		t.Errorf("bias should stay zero when disabled: got %v", got)
	}
}

func TestIIOSourceProvidedBiasSkipsStartupCalibration(t *testing.T) {
	base := t.TempDir()
	makeFakeDevice(t, base, "iio:device0", "anglvel", "0.001", [3]string{"10", "-20", "15"})
	makeFakeDevice(t, base, "iio:device1", "accel", "0.000244", [3]string{"0", "0", "40000"})

	wantBias := [3]float64{0.001, 0.002, 0.003}
	s := NewIIOSource(IIOSourceConfig{
		IIOBase:       base,
		PollRateHz:    200,
		GyroBias:      wantBias,
		HasGyroBias:   true,
		CalibrateBias: true,
		CalibrationMs: 200,
	})
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan struct{})
	go func() { s.Start(ctx); close(done) }()

	sub := s.Subscribe()
	timer := time.After(2 * time.Second)
	for i := 0; i < 60; i++ {
		select {
		case <-sub:
		case <-timer:
			cancel()
			<-done
			t.Fatalf("only got %d samples; status=%q", i, s.Status())
		}
	}
	cancel()
	<-done

	if got := s.fusion.GyroBias(); got != wantBias {
		t.Errorf("provided bias should not be overwritten: got %v, want %v", got, wantBias)
	}
}
