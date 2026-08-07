package gyro

import (
	"context"
	"encoding/binary"
	"fmt"
	"io"
	"log"
	"math"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"time"
)

const (
	defaultIIOBase = "/sys/bus/iio/devices"
	subBufferSize  = 8               // per-subscriber sample buffer
	scaleRefresh   = 5 * time.Second // re-read scale files this often
	probeInterval  = 2 * time.Second // re-discovery cadence when offline
	maxPollRateHz  = 1000
	defaultODRHz   = 26 // LSM6DSR ODR written to sampling_frequency (26 -> ~26 Hz, the responsive attitude-card regime; see writeODR)

	// Startup static gyro-bias calibration.
	defaultCalibrationMs = 2000   // default CalibrationMs when unset
	minCalSamples        = 12     // floor on the calibration sample count
	maxCalSamples        = 200    // ceiling on the calibration sample count
	stillGateRps         = 0.0175 // ~1 dps/axis; std above this hints the device moved
	rps2dps              = 180.0 / math.Pi
)

// IIOSourceConfig configures an IIOSource.
type IIOSourceConfig struct {
	PollRateHz    int        // sensor read cadence (default 100)
	ODRHz         int        // output data rate written to sampling_frequency; 0 → defaultODRHz
	FusionAlpha   float64    // complementary filter coefficient (default 0.05)
	MountMatrix   [9]float64 // row-major 3x3 body -> spec frame (default identity)
	GyroBias      [3]float64 // static body-frame gyro bias in rad/s
	HasGyroBias   bool       // true when GyroBias should suppress startup calibration
	AccelPath     string     // accel device dir override; "" = autodiscover
	GyroPath      string     // anglvel device dir override; "" = autodiscover
	IIOBase       string     // IIO sysfs root (default /sys/bus/iio/devices)
	CalibrateBias bool       // run a one-shot static gyro-bias calibration at startup
	CalibrationMs int        // calibration window length (ms); default 2000
}

// iioDevice holds open fds for one IIO device's three axes plus its scale.
type iioDevice struct {
	dir       string
	kind      string // "accel" or "anglvel"
	rawFiles  [3]*os.File
	scaleFile *os.File
	scale     float64
	scaleAt   time.Time
}

type iioReader interface {
	read(now time.Time) ([3]float64, error)
	close()
}

type iioBufferDevice struct {
	dir       string
	kind      string
	dataFile  *os.File
	scaleFile *os.File
	scale     float64
	scaleAt   time.Time
}

// IIOSource reads LSM6DSR accel + gyro samples from the Linux IIO sysfs tree,
// fuses them into an orientation quaternion via a single Fusion instance, and
// fans the result out to N SSE subscribers through a non-blocking broker.
//
// Exactly one read goroutine (Start) drives the Fusion and the file reads; all
// subscribers share it. This satisfies the gyro.Source contract.
type IIOSource struct {
	cfg    IIOSourceConfig
	fusion *Fusion

	mu     sync.Mutex
	status StatusCode
	subs   map[chan Sample]struct{}

	accelDev iioReader
	gyroDev  iioReader
}

// NewIIOSource builds an IIOSource. It does not open any files or start a loop;
// call Start to begin reading. Start performs the initial probe and ongoing
// re-discovery, so a missing sensor at startup is non-fatal.
func NewIIOSource(cfg IIOSourceConfig) *IIOSource {
	if cfg.IIOBase == "" {
		cfg.IIOBase = defaultIIOBase
	}
	if cfg.PollRateHz <= 0 {
		cfg.PollRateHz = 100
	}
	src := &IIOSource{
		cfg:    cfg,
		fusion: NewFusion(cfg.FusionAlpha, cfg.MountMatrix),
		subs:   make(map[chan Sample]struct{}),
		status: StatusOffline,
	}
	if cfg.HasGyroBias {
		src.fusion.SetGyroBias(cfg.GyroBias)
	}
	return src
}

// Start runs the read/fuse/publish loop until ctx is canceled. It blocks; run
// it in a goroutine. It is safe to call Start at most once per source.
//
// When CalibrateBias is set, the first calSampleCount() good gyro samples are
// averaged (in the body frame, before the mount matrix) to estimate the static
// zero-rate bias, which is then subtracted from every subsequent sample. The
// device is assumed to be still during this window. Samples are still fused and
// published throughout, so the bias is live after at most one calibration
// window with no first-sample gap.
func (s *IIOSource) Start(ctx context.Context) {
	defer s.cleanup()

	s.probeAndOpen()

	poll := time.NewTicker(s.period())
	defer poll.Stop()
	probe := time.NewTicker(probeInterval)
	defer probe.Stop()

	calN := s.calSampleCount()
	var calSamples [][3]float64
	var biasReady [3]bool
	calibrated := !s.cfg.CalibrateBias || s.cfg.HasGyroBias // skip when disabled or file bias is installed
	var tilt tiltEstimator

	var last time.Time
	for {
		select {
		case <-ctx.Done():
			return
		case <-poll.C:
			if s.accelDev == nil || s.gyroDev == nil {
				continue // not ready; probe ticker will reopen
			}
			now := time.Now()
			accel, errA := s.accelDev.read(now)
			gyro, errG := s.gyroDev.read(now)
			if errA != nil || errG != nil {
				// Reopen fds on next probe; temporarily mark error.
				s.setStatus(StatusError)
				s.reopenLocked()
				last = time.Time{}
				continue
			}
			if s.status != StatusOnline {
				s.setStatus(StatusOnline)
			}
			var dt float64
			if !last.IsZero() {
				dt = now.Sub(last).Seconds()
			}
			last = now

			// One-shot static bias calibration: accumulate body-frame gyro
			// samples (raw, pre-mount, rad/s) until the window fills, then
			// install each still axis's per-axis mean via updateBiasFromWindow.
			// Noisy axes are preserved for a later retry, instead of permanently
			// accepting zero bias. Bias is a chip property, so after X/Y are ready
			// we stop recalibrating across transient disconnects.
			if !calibrated {
				calSamples = append(calSamples, gyro)
				if len(calSamples) >= calN {
					mean, std := meanStdGyro(calSamples)
					bias, ready, accepted := updateBiasFromWindow(s.fusion.GyroBias(), biasReady, mean, std)
					biasReady = ready
					s.fusion.SetGyroBias(bias)
					axes := [3]string{"X", "Y", "Z"}
					parts := [3]string{}
					for i := 0; i < 3; i++ {
						verb := "skip(noisy)"
						if accepted[i] {
							verb = "install"
						}
						parts[i] = fmt.Sprintf("%s mean=%+.3f std=%.3f dps %s", axes[i], mean[i]*rps2dps, std[i]*rps2dps, verb)
					}
					if tiltBiasReady(biasReady) {
						log.Printf("gyro: static tilt bias calibrated gate=%.3f dps/axis samples=%d | %s | %s | %s",
							stillGateRps*rps2dps, calN, parts[0], parts[1], parts[2])
						calibrated = true
					} else {
						log.Printf("gyro: WARN static bias calibration still moving; retrying gate=%.3f dps/axis samples=%d ready=%v | %s | %s | %s",
							stillGateRps*rps2dps, calN, biasReady, parts[0], parts[1], parts[2])
					}
					calSamples = nil
				}
			}

			q := s.fusion.Update(accel, gyro, dt)
			pitchDeg, rollDeg, tiltValid := tilt.Update(accel, s.cfg.MountMatrix, dt)
			s.publish(Sample{
				Timestamp: now,
				Quat:      q,
				PitchDeg:  pitchDeg,
				RollDeg:   rollDeg,
				TiltValid: tiltValid,
			})
		case <-probe.C:
			// Re-discover on hotplug or after a read failure.
			if s.accelDev == nil || s.gyroDev == nil {
				s.probeAndOpen()
			}
		}
	}
}

// Status returns the current sensor status (thread-safe).
func (s *IIOSource) Status() StatusCode {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.status
}

// Subscribe returns a channel receiving fused attitude samples. Pass the
// returned channel to Unsubscribe when done.
func (s *IIOSource) Subscribe() <-chan Sample {
	ch := make(chan Sample, subBufferSize)
	s.mu.Lock()
	s.subs[ch] = struct{}{}
	s.mu.Unlock()
	return ch
}

// Unsubscribe removes a subscriber. It does not close the channel; the broker
// stops sending to it immediately and the channel is garbage-collected once the
// caller drops its reference.
func (s *IIOSource) Unsubscribe(ch <-chan Sample) {
	s.mu.Lock()
	for k := range s.subs {
		if k == ch {
			delete(s.subs, k)
			break
		}
	}
	s.mu.Unlock()
}

// period derives the poll interval from PollRateHz with sane clamps.
func (s *IIOSource) period() time.Duration {
	hz := s.cfg.PollRateHz
	if hz < 1 {
		hz = 100
	}
	if hz > maxPollRateHz {
		hz = maxPollRateHz
	}
	return time.Second / time.Duration(hz)
}

// calSampleCount derives the startup calibration sample count from CalibrationMs
// and PollRateHz, clamped to [minCalSamples, maxCalSamples].
func (s *IIOSource) calSampleCount() int {
	ms := s.cfg.CalibrationMs
	if ms <= 0 {
		ms = defaultCalibrationMs
	}
	hz := s.cfg.PollRateHz
	if hz < 1 {
		hz = 100
	}
	n := int(math.Round(float64(ms) / 1000.0 * float64(hz)))
	if n < minCalSamples {
		n = minCalSamples
	}
	if n > maxCalSamples {
		n = maxCalSamples
	}
	return n
}

// meanStdGyro returns the per-axis population mean and population standard
// deviation of the given body-frame gyro samples (rad/s). It is pure for unit
// testing.
func meanStdGyro(samples [][3]float64) (mean, std [3]float64) {
	n := len(samples)
	if n == 0 {
		return
	}
	for _, smp := range samples {
		mean[0] += smp[0]
		mean[1] += smp[1]
		mean[2] += smp[2]
	}
	mean[0] /= float64(n)
	mean[1] /= float64(n)
	mean[2] /= float64(n)
	for _, smp := range samples {
		d0, d1, d2 := smp[0]-mean[0], smp[1]-mean[1], smp[2]-mean[2]
		std[0] += d0 * d0
		std[1] += d1 * d1
		std[2] += d2 * d2
	}
	std[0] = math.Sqrt(std[0] / float64(n))
	std[1] = math.Sqrt(std[1] / float64(n))
	std[2] = math.Sqrt(std[2] / float64(n))
	return mean, std
}

// decideBias returns the per-axis static gyro bias to install from a calibration
// window and, for each axis, whether that axis's window was still enough to
// accept. Each axis is gated INDEPENDENTLY at stillGateRps: a clean axis
// contributes its sample mean; a noisy axis (the device moved on that DOF, or
// the gyro regime is too noisy there) contributes zero.
//
// Per-axis gating is essential for tilt (pitch/roll) accuracy. The X and Y body
// rates drive tilt and their bias is usually clean even when the Z (yaw) axis is
// vibrating. Under the prior all-or-nothing gate a noisy Z zeroed the clean X/Y
// bias too, leaving it uncompensated; the complementary filter then integrated
// that X/Y offset, and because the accel correction pulls tilt back only alpha
// per step the result was a large steady-state tilt error on a physically level
// device (b_h * dt / alpha, ~20° at low publish fps). Yaw cannot be corrected by
// an accelerometer anyway, so leaving a rejected Z bias at zero only costs some
// yaw drift — the known no-magnetometer limitation. Pure for unit testing.
func decideBias(mean, std [3]float64) (bias [3]float64, accepted [3]bool) {
	for i := 0; i < 3; i++ {
		if std[i] <= stillGateRps {
			bias[i] = mean[i]
			accepted[i] = true
		}
	}
	return bias, accepted
}

func updateBiasFromWindow(currentBias [3]float64, currentReady [3]bool, mean, std [3]float64) (bias [3]float64, ready, accepted [3]bool) {
	windowBias, accepted := decideBias(mean, std)
	bias = currentBias
	ready = currentReady
	for i := 0; i < 3; i++ {
		if accepted[i] {
			bias[i] = windowBias[i]
			ready[i] = true
		}
	}
	return bias, ready, accepted
}

func tiltBiasReady(ready [3]bool) bool {
	return ready[0] && ready[1]
}

func (s *IIOSource) setStatus(st StatusCode) {
	s.mu.Lock()
	s.status = st
	s.mu.Unlock()
}

// probeAndOpen discovers (or honors configured overrides for) the accel and
// gyro devices and opens their fds. Updates status: offline if absent, error if
// found but unopenable, online on success.
func (s *IIOSource) probeAndOpen() {
	base := s.cfg.IIOBase
	accelDir := s.cfg.AccelPath
	gyroDir := s.cfg.GyroPath

	if accelDir == "" {
		d, err := discoverDevice(base, "accel")
		if err != nil {
			s.setStatus(StatusOffline)
			return
		}
		accelDir = d
	}
	if gyroDir == "" {
		d, err := discoverDevice(base, "anglvel")
		if err != nil {
			s.setStatus(StatusOffline)
			return
		}
		gyroDir = d
	}

	ad, errA := openBestDevice(accelDir, "accel", s.cfg.ODRHz)
	gd, errG := openBestDevice(gyroDir, "anglvel", s.cfg.ODRHz)
	if errA != nil || errG != nil {
		if ad != nil {
			ad.close()
		}
		if gd != nil {
			gd.close()
		}
		s.setStatus(StatusError)
		return
	}

	s.mu.Lock()
	s.accelDev = ad
	s.gyroDev = gd
	s.status = StatusOnline
	s.mu.Unlock()
}

// reopenLocked closes and clears the device fds so the probe ticker reopens
// them.
func (s *IIOSource) reopenLocked() {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.accelDev != nil {
		s.accelDev.close()
		s.accelDev = nil
	}
	if s.gyroDev != nil {
		s.gyroDev.close()
		s.gyroDev = nil
	}
}

func (s *IIOSource) cleanup() {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.subs = nil
	if s.accelDev != nil {
		s.accelDev.close()
		s.accelDev = nil
	}
	if s.gyroDev != nil {
		s.gyroDev.close()
		s.gyroDev = nil
	}
	s.status = StatusOffline
}

// publish fans a sample out to every subscriber without blocking the read loop.
// A full channel triggers drop-oldest: one stale sample is discarded and the
// fresh one is retried once.
func (s *IIOSource) publish(sample Sample) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for ch := range s.subs {
		select {
		case ch <- sample:
		default:
			select {
			case <-ch: // drop oldest
			default:
			}
			select {
			case ch <- sample:
			default:
			}
		}
	}
}

// discoverDevice returns the first iio:device* dir under base that exposes the
// given channel kind ("accel" or "anglvel"). Device numbers can shift across
// boots, so nothing is hardcoded.
func discoverDevice(base, kind string) (string, error) {
	matches, err := filepath.Glob(filepath.Join(base, "iio:device*"))
	if err != nil {
		return "", fmt.Errorf("glob iio devices under %s: %w", base, err)
	}
	for _, d := range matches {
		if fileExists(filepath.Join(d, "in_"+kind+"_x_raw")) {
			return d, nil
		}
	}
	return "", fmt.Errorf("no iio device exposing in_%s_*_raw under %s", kind, base)
}

func openBestDevice(dir, kind string, odrHz int) (iioReader, error) {
	if bd, err := openBufferDevice(dir, kind, odrHz); err == nil {
		log.Printf("gyro: opened %s via IIO buffer (%s)", kind, dir)
		return bd, nil
	}
	return openDevice(dir, kind, odrHz)
}

func openBufferDevice(dir, kind string, odrHz int) (*iioBufferDevice, error) {
	if odrHz <= 0 {
		odrHz = defaultODRHz
	}
	if err := validateSimpleS16Scan(dir, kind); err != nil {
		return nil, err
	}
	writeODR(dir, odrHz)
	if err := configureBuffer(dir, kind, true); err != nil {
		configureBuffer(dir, kind, false)
		return nil, err
	}

	dataPath := filepath.Join("/dev", filepath.Base(dir))
	df, err := os.Open(dataPath)
	if err != nil {
		configureBuffer(dir, kind, false)
		return nil, fmt.Errorf("open %s buffer data %s: %w", kind, dataPath, err)
	}
	sf, err := os.Open(filepath.Join(dir, "in_"+kind+"_scale"))
	if err != nil {
		df.Close()
		configureBuffer(dir, kind, false)
		return nil, fmt.Errorf("open %s scale: %w", kind, err)
	}
	sc, err := readSeekFloat(sf)
	if err != nil {
		df.Close()
		sf.Close()
		configureBuffer(dir, kind, false)
		return nil, fmt.Errorf("read %s scale: %w", kind, err)
	}
	return &iioBufferDevice{
		dir:       dir,
		kind:      kind,
		dataFile:  df,
		scaleFile: sf,
		scale:     sc,
		scaleAt:   time.Now(),
	}, nil
}

func validateSimpleS16Scan(dir, kind string) error {
	for i, ax := range [3]string{"x", "y", "z"} {
		prefix := filepath.Join(dir, "scan_elements", "in_"+kind+"_"+ax)
		typ, err := os.ReadFile(prefix + "_type")
		if err != nil {
			return fmt.Errorf("read %s scan type: %w", kind, err)
		}
		if strings.TrimSpace(string(typ)) != "le:s16/16>>0" {
			return fmt.Errorf("unsupported %s %s scan type %q", kind, ax, strings.TrimSpace(string(typ)))
		}
		idx, err := os.ReadFile(prefix + "_index")
		if err != nil {
			return fmt.Errorf("read %s scan index: %w", kind, err)
		}
		if strings.TrimSpace(string(idx)) != strconv.Itoa(i) {
			return fmt.Errorf("unsupported %s %s scan index %q", kind, ax, strings.TrimSpace(string(idx)))
		}
	}
	return nil
}

func configureBuffer(dir, kind string, enable bool) error {
	bufEnable := filepath.Join(dir, "buffer", "enable")
	if err := writeSysfsValue(bufEnable, "0"); err != nil && enable {
		return fmt.Errorf("disable %s buffer before configure: %w", kind, err)
	}
	for _, ax := range [3]string{"x", "y", "z"} {
		val := "0"
		if enable {
			val = "1"
		}
		if err := writeSysfsValue(filepath.Join(dir, "scan_elements", "in_"+kind+"_"+ax+"_en"), val); err != nil && enable {
			return fmt.Errorf("set %s %s scan enable: %w", kind, ax, err)
		}
	}
	_ = writeSysfsValue(filepath.Join(dir, "scan_elements", "in_timestamp_en"), "0")
	if !enable {
		return nil
	}
	_ = writeSysfsValue(filepath.Join(dir, "buffer", "length"), "8")
	_ = writeSysfsValue(filepath.Join(dir, "buffer", "watermark"), "1")
	if err := writeSysfsValue(bufEnable, "1"); err != nil {
		return fmt.Errorf("enable %s buffer: %w", kind, err)
	}
	return nil
}

func writeSysfsValue(path, value string) error {
	return os.WriteFile(path, []byte(value), 0o644)
}

// openDevice opens the three axis raw files and the scale file for a device,
// reading the scale immediately.
func openDevice(dir, kind string, odrHz int) (*iioDevice, error) {
	if odrHz <= 0 {
		odrHz = defaultODRHz
	}
	d := &iioDevice{dir: dir, kind: kind}
	writeODR(dir, odrHz)
	axes := [3]string{"x", "y", "z"}
	for i, ax := range axes {
		f, err := os.Open(filepath.Join(dir, "in_"+kind+"_"+ax+"_raw"))
		if err != nil {
			d.close()
			return nil, fmt.Errorf("open %s %s raw: %w", kind, ax, err)
		}
		d.rawFiles[i] = f
	}
	sf, err := os.Open(filepath.Join(dir, "in_"+kind+"_scale"))
	if err != nil {
		d.close()
		return nil, fmt.Errorf("open %s scale: %w", kind, err)
	}
	d.scaleFile = sf
	sc, err := readSeekFloat(sf)
	if err != nil {
		d.close()
		return nil, fmt.Errorf("read %s scale: %w", kind, err)
	}
	d.scale = sc
	d.scaleAt = time.Now()
	return d, nil
}

// writeODR sets the sensor output data rate by writing ASCII hz to the device's
// sampling_frequency. hz <= 0 skips the write; openDevice resolves config zero
// to defaultODRHz before calling this helper. The lsm6dsr driver accepts the
// bare integer (26 -> ~26 Hz).
//
// Trade-off on this part: the driver blocks each in_*_raw read until the next
// fresh conversion, so per-read latency is ~1/ODR. A HIGH ODR makes each read
// return fast (more publish fps) BUT on this board the gyro's per-sample
// mean-bias and noise explode above ~26 Hz (416 Hz shows a ~23 dps zero-rate
// mean violation + 27 dps std), and the complementary filter integrates that
// garbage into visible drift when pitch/roll are taken from the fused quaternion.
// The attitude card now publishes accelerometer-derived pitch/roll, so 26 Hz is
// a better default for visible responsiveness while keeping the gyro far away
// from the very noisy 416 Hz regime. The setting is not persistent, so a non-zero
// value is re-applied on every open. Best-effort: a missing or read-only
// sampling_frequency file is logged and ignored.
func writeODR(dir string, hz int) {
	if hz <= 0 {
		return
	}
	p := filepath.Join(dir, "sampling_frequency")
	if err := os.WriteFile(p, []byte(strconv.Itoa(hz)), 0o644); err != nil {
		log.Printf("gyro: set ODR %s=%d: %v", p, hz, err)
		return
	}
	if b, err := os.ReadFile(p); err == nil {
		got, parseErr := strconv.ParseFloat(strings.TrimSpace(string(b)), 64)
		if parseErr == nil && math.Abs(got-float64(hz)) > math.Max(1, float64(hz)*0.1) {
			log.Printf("gyro: WARN ODR readback %s=%g after requesting %d", p, got, hz)
		}
	}
}

// read returns the three axes in physical units, refreshing the scale
// periodically.
func (d *iioDevice) read(now time.Time) ([3]float64, error) {
	var v [3]float64
	if now.Sub(d.scaleAt) > scaleRefresh {
		if sc, err := readSeekFloat(d.scaleFile); err == nil {
			d.scale = sc
			d.scaleAt = now
		}
	}
	for i, f := range d.rawFiles {
		raw, err := readSeekInt(f)
		if err != nil {
			return v, fmt.Errorf("read %s axis %d: %w", d.kind, i, err)
		}
		v[i] = float64(raw) * d.scale
	}
	return v, nil
}

func (d *iioDevice) close() {
	for _, f := range d.rawFiles {
		if f != nil {
			f.Close()
		}
	}
	if d.scaleFile != nil {
		d.scaleFile.Close()
	}
}

func (d *iioBufferDevice) read(now time.Time) ([3]float64, error) {
	var v [3]float64
	if now.Sub(d.scaleAt) > scaleRefresh {
		if sc, err := readSeekFloat(d.scaleFile); err == nil {
			d.scale = sc
			d.scaleAt = now
		}
	}
	var frame [6]byte
	if _, err := io.ReadFull(d.dataFile, frame[:]); err != nil {
		return v, fmt.Errorf("read %s buffer frame: %w", d.kind, err)
	}
	for i := 0; i < 3; i++ {
		raw := int16(binary.LittleEndian.Uint16(frame[i*2 : i*2+2]))
		v[i] = float64(raw) * d.scale
	}
	return v, nil
}

func (d *iioBufferDevice) close() {
	if d.dataFile != nil {
		d.dataFile.Close()
	}
	if d.scaleFile != nil {
		d.scaleFile.Close()
	}
	configureBuffer(d.dir, d.kind, false)
}

// readSeek re-reads a small sysfs value by seeking to the start, returning the
// trimmed content. Handles partial reads and the trailing newline.
func readSeek(f *os.File) (string, error) {
	if _, err := f.Seek(0, 0); err != nil {
		return "", err
	}
	var sb strings.Builder
	buf := make([]byte, 32)
	for {
		n, err := f.Read(buf)
		if n > 0 {
			sb.Write(buf[:n])
			if strings.ContainsRune(sb.String(), '\n') {
				break
			}
		}
		if err != nil {
			if err == io.EOF {
				break
			}
			return "", err
		}
	}
	return strings.TrimSpace(sb.String()), nil
}

func readSeekInt(f *os.File) (int64, error) {
	s, err := readSeek(f)
	if err != nil {
		return 0, err
	}
	return strconv.ParseInt(s, 10, 64)
}

func readSeekFloat(f *os.File) (float64, error) {
	s, err := readSeek(f)
	if err != nil {
		return 0, err
	}
	return strconv.ParseFloat(s, 64)
}

func fileExists(path string) bool {
	_, err := os.Stat(path)
	return err == nil
}
