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
	// ay < 0 means world-up leans toward body -Y, i.e. the right side dips:
	// positive roll per the card convention.
	if math.Abs(roll-0.45) > 0.2 {
		t.Fatalf("roll = %.2f, want about +0.45", roll)
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
	if math.Abs(pitch-(-1.80)) > 0.2 || math.Abs(roll-0.45) > 0.2 {
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

// TestAccelTiltDegreesSigns locks the card tilt convention on the fast
// accelerometer path (docs/references/gyro-attitude-sse.md §2/§7):
//   - level device reads (0, 0)
//   - front dips  -> positive pitch
//   - right dips  -> positive roll
//
// Expected values are cross-checked against QuatToTiltDegrees for the same
// physical attitude: the accel sample is synthesized as the world-up gravity
// reaction rotated into the body frame, which is exactly what the sensor reads.
func TestAccelTiltDegreesSigns(t *testing.T) {
	const (
		deg = math.Pi / 180
		tol = 1e-6
	)
	id := [9]float64{1, 0, 0, 0, 1, 0, 0, 0, 1}
	cases := []struct {
		name      string
		pitchRad  float64 // euler pitch about +Y fed to quatFromEulerZYX
		rollRad   float64 // euler roll about +X fed to quatFromEulerZYX
		wantPitch float64 // degrees
		wantRoll  float64 // degrees
	}{
		{"level", 0, 0, 0, 0},
		{"front dips 10", 10 * deg, 0, 10, 0},
		{"back dips 10", -10 * deg, 0, -10, 0},
		{"right dips 10", 0, -10 * deg, 0, 10},
		{"left dips 10", 0, 10 * deg, 0, -10},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			q := quatFromEulerZYX(0, c.pitchRad, c.rollRad)
			accel := quatRotateVec(quatConj(q), [3]float64{0, 0, gravityNominal})
			pitch, roll, ok := accelTiltDegrees(accel, id)
			if !ok {
				t.Fatal("synthesized gravity sample rejected as invalid")
			}
			if math.Abs(pitch-c.wantPitch) > tol || math.Abs(roll-c.wantRoll) > tol {
				t.Errorf("got (pitch=%.4f, roll=%.4f), want (%.4f, %.4f)",
					pitch, roll, c.wantPitch, c.wantRoll)
			}
			qPitch, qRoll := QuatToTiltDegrees(q)
			if math.Abs(pitch-qPitch) > tol || math.Abs(roll-qRoll) > tol {
				t.Errorf("accel tilt (%.4f, %.4f) disagrees with quat tilt (%.4f, %.4f)",
					pitch, roll, qPitch, qRoll)
			}
		})
	}
}
