package handlers

import (
	"testing"
	"time"
)

func newFakeSampler(fn func(time.Duration, bool) ([]float64, error)) *cpuSampler {
	return &cpuSampler{
		sampleFn: fn,
		stop:     make(chan struct{}),
	}
}

func waitFor(t *testing.T, fn func() bool) {
	t.Helper()
	deadline := time.After(3 * time.Second)
	for !fn() {
		select {
		case <-deadline:
			t.Fatal("condition not met within timeout")
		case <-time.After(5 * time.Millisecond):
		}
	}
}

func TestCPUSampler_CachesAndReturnsLastSample(t *testing.T) {
	s := newFakeSampler(func(time.Duration, bool) ([]float64, error) {
		return []float64{10.0, 20.0, 30.0, 40.0}, nil
	})
	s.start()
	defer s.stopSampler()

	waitFor(t, func() bool { return s.totalPercent() > 0 })

	// total% = mean of per-core = (10+20+30+40)/4 = 25
	if got := s.totalPercent(); got != 25.0 {
		t.Fatalf("totalPercent = %v, want 25", got)
	}
	per := s.perCPUPercents()
	if len(per) != 4 || per[0] != 10 || per[3] != 40 {
		t.Fatalf("perCPUPercents = %v, want [10 20 30 40]", per)
	}
}

func TestCPUSampler_UpdatesOnNewSample(t *testing.T) {
	// First sample returns 10/20, then 50/60 — cache must reflect the latest.
	step := 0
	s := newFakeSampler(func(time.Duration, bool) ([]float64, error) {
		step++
		if step == 1 {
			return []float64{10.0, 20.0}, nil
		}
		return []float64{50.0, 60.0}, nil
	})
	s.start()
	defer s.stopSampler()

	waitFor(t, func() bool { return s.totalPercent() == 15.0 }) // (10+20)/2
	// wait for the second sample to land
	waitFor(t, func() bool { return s.totalPercent() == 55.0 }) // (50+60)/2
}

func TestCPUSampler_SkipsEmptySample(t *testing.T) {
	s := newFakeSampler(func(time.Duration, bool) ([]float64, error) {
		return nil, nil // gopsutil returns empty on error
	})
	s.start()
	defer s.stopSampler()

	// give the sampler a couple of ticks to attempt sampling
	time.Sleep(50 * time.Millisecond)
	if got := s.totalPercent(); got != 0 {
		t.Fatalf("totalPercent = %v, want 0 when sampleFn returns empty", got)
	}
	if got := s.perCPUPercents(); got != nil {
		t.Fatalf("perCPUPercents = %v, want nil when sampleFn returns empty", got)
	}
}

func TestCPUSampler_PerCPUCopyNotShared(t *testing.T) {
	s := newFakeSampler(func(time.Duration, bool) ([]float64, error) {
		return []float64{1.0, 2.0}, nil
	})
	s.start()
	defer s.stopSampler()

	waitFor(t, func() bool { return len(s.perCPUPercents()) > 0 })

	a := s.perCPUPercents()
	a[0] = 999 // mutate the returned copy
	b := s.perCPUPercents()
	if b[0] == 999 {
		t.Fatal("caller mutated the cached slice; perCPUPercents must return a copy")
	}
}

func TestCPUSampler_NilSafe(t *testing.T) {
	var s *cpuSampler // nil receiver
	if got := s.totalPercent(); got != 0 {
		t.Fatalf("nil totalPercent = %v, want 0", got)
	}
	if got := s.perCPUPercents(); got != nil {
		t.Fatalf("nil perCPUPercents = %v, want nil", got)
	}
	s.start()       // must not panic
	s.stopSampler() // must not panic
}

func TestCPUSampler_StopIsIdempotent(t *testing.T) {
	s := newFakeSampler(func(time.Duration, bool) ([]float64, error) {
		return []float64{1.0}, nil
	})
	s.start()
	s.stopSampler()
	s.stopSampler() // second close must not panic (double-close guard)
}
