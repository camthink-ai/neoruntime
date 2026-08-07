package gyro

import (
	"math"
	"testing"
)

func TestAccelTiltDegreesLevel(t *testing.T) {
	pitch, roll, ok := accelTiltDegrees([3]float64{0.3123, -0.0772, 9.9206}, [9]float64{})
	if !ok {
		t.Fatal("level-ish accel should be valid")
	}
	if math.Abs(pitch-(-1.80)) > 0.2 {
		t.Fatalf("pitch = %.2f, want about -1.80", pitch)
	}
	if math.Abs(roll-(-0.45)) > 0.2 {
		t.Fatalf("roll = %.2f, want about -0.45", roll)
	}
}

func TestAccelTiltDegreesRejectsOutOfBandGravity(t *testing.T) {
	if _, _, ok := accelTiltDegrees([3]float64{0, 0, 20}, [9]float64{}); ok {
		t.Fatal("20 m/s^2 accel should be rejected")
	}
	if _, _, ok := accelTiltDegrees([3]float64{0, 0, 0}, [9]float64{}); ok {
		t.Fatal("free-fall accel should be rejected")
	}
}

func TestTiltEstimatorInitializesImmediatelyAndHoldsInvalid(t *testing.T) {
	var est tiltEstimator
	pitch, roll, ok := est.Update([3]float64{0.3123, -0.0772, 9.9206}, [9]float64{}, 0)
	if !ok {
		t.Fatal("first valid sample should initialize estimator")
	}
	if math.Abs(pitch-(-1.80)) > 0.2 || math.Abs(roll-(-0.45)) > 0.2 {
		t.Fatalf("first sample pitch/roll = %.2f/%.2f", pitch, roll)
	}

	pitch2, roll2, ok := est.Update([3]float64{0, 0, 20}, [9]float64{}, 10)
	if !ok {
		t.Fatal("invalid sample after initialization should hold last value")
	}
	if pitch2 != pitch || roll2 != roll {
		t.Fatalf("invalid sample changed tilt: %.2f/%.2f -> %.2f/%.2f", pitch, roll, pitch2, roll2)
	}
}

func TestLowPassAlphaUsesWallClockTime(t *testing.T) {
	aSlow := lowPassAlpha(0.05, defaultTiltTauSeconds)
	aFast := lowPassAlpha(2.0, defaultTiltTauSeconds)
	if !(aSlow > 0 && aSlow < 0.2) {
		t.Fatalf("alpha for 50ms = %.3f, want a small correction", aSlow)
	}
	if aFast < 0.98 {
		t.Fatalf("alpha for 2s = %.3f, want nearly immediate convergence", aFast)
	}
}
