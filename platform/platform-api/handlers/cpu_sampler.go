package handlers

import (
	"sync"
	"time"

	"github.com/shirou/gopsutil/v4/cpu"
)

// cpuSampleInterval is how often the background sampler refreshes the cached
// /proc/stat delta. 2s matches the dashboard snapshot poll cadence so the
// reading stays fresh without per-request sampling.
const cpuSampleInterval = 2 * time.Second

// cpuPrimeWindow is the shorter window used for the first sample so a reading
// is available shortly after startup instead of after a full interval. It runs
// once on the background goroutine — never on a request path.
const cpuPrimeWindow = 500 * time.Millisecond

// cpuSampler caches CPU utilization computed by a single background goroutine.
//
// Monitor HTTP handlers read the cache instantly instead of each running their
// own 500ms blocking cpu.Percent sample. Two problems with the old per-request
// model are fixed:
//   - Self-pollution: every dashboard load fired 2-3 concurrent 500ms
//     cpu.Percent samples, and that burst was captured by the first sample's
//     own window, so the first dashboard reading read high.
//   - Blocking: each /monitor/* request blocked ~500ms; with several dashboard
//     clients polling that is real wasted work.
//
// The sampler reads /proc/stat on its own fixed cadence, decoupled from HTTP
// traffic, so opening the dashboard no longer measures itself.
type cpuSampler struct {
	// sampleFn is gopsutil's cpu.Percent by default; injectable for tests.
	sampleFn func(window time.Duration, percpu bool) ([]float64, error)

	mu      sync.RWMutex
	total   float64   // total CPU% = mean of per-core, last sample window
	perCPU  []float64 // per-core CPU%, last sample window
	updated time.Time
	stop    chan struct{}
	started bool
}

// newCPUSampler builds a sampler backed by gopsutil's cpu.Percent.
func newCPUSampler() *cpuSampler {
	return &cpuSampler{
		sampleFn: cpu.Percent,
		stop:     make(chan struct{}),
	}
}

// start launches the background sampling goroutine. Safe to call on a nil
// receiver and idempotent. The goroutine runs until stopSampler is called
// (or the process exits, since it is a daemon).
func (s *cpuSampler) start() {
	if s == nil || s.started {
		return
	}
	s.started = true
	go s.loop()
}

// loop samples once with the prime window, then on every cpuSampleInterval
// tick until stopped.
func (s *cpuSampler) loop() {
	s.sampleOnce(cpuPrimeWindow)
	t := time.NewTicker(cpuSampleInterval)
	defer t.Stop()
	for {
		select {
		case <-s.stop:
			return
		case <-t.C:
			s.sampleOnce(cpuSampleInterval)
		}
	}
}

// sampleOnce takes a single two-sample delta over window and caches the result.
// percpu=true returns one percentage per core; total% is the mean across cores,
// which matches gopsutil's cpu.Percent(false) semantics.
func (s *cpuSampler) sampleOnce(window time.Duration) {
	perCPU, err := s.sampleFn(window, true)
	if err != nil || len(perCPU) == 0 {
		return
	}
	var sum float64
	for _, p := range perCPU {
		sum += p
	}
	now := time.Now()
	s.mu.Lock()
	s.perCPU = perCPU
	s.total = sum / float64(len(perCPU))
	s.updated = now
	s.mu.Unlock()
}

// totalPercent returns the cached total CPU% from the most recent background
// sample. Returns 0 before the first sample has landed.
func (s *cpuSampler) totalPercent() float64 {
	if s == nil {
		return 0
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	return s.total
}

// perCPUPercents returns a copy of the cached per-core CPU% slice, or nil
// before the first sample has landed. A copy is returned so callers cannot
// mutate the cached slice.
func (s *cpuSampler) perCPUPercents() []float64 {
	if s == nil {
		return nil
	}
	s.mu.RLock()
	defer s.mu.RUnlock()
	if len(s.perCPU) == 0 {
		return nil
	}
	out := make([]float64, len(s.perCPU))
	copy(out, s.perCPU)
	return out
}

// stopSampler signals the background goroutine to exit. Idempotent.
func (s *cpuSampler) stopSampler() {
	if s == nil || !s.started {
		return
	}
	select {
	case <-s.stop:
		// already closed
	default:
		close(s.stop)
	}
}
