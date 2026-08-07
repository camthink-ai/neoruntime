package gyro

import (
	"math"
	"testing"
)

// approxQuat reports whether two quaternions are element-wise close.
func approxQuat(t *testing.T, name string, got, want [4]float64, tol float64) {
	t.Helper()
	for i := 0; i < 4; i++ {
		if math.Abs(got[i]-want[i]) > tol {
			t.Errorf("%s: got %v, want %v (tol %g)", name, got, want, tol)
			return
		}
	}
}

// quatAngle returns the rotation angle (radians) of a unit quaternion.
func quatAngle(q [4]float64) float64 {
	w := math.Abs(q[3])
	if w > 1 {
		w = 1
	}
	return 2 * math.Acos(w)
}

func TestQuatNormalize(t *testing.T) {
	tests := []struct {
		name string
		in   [4]float64
		want [4]float64
	}{
		{"already-unit", [4]float64{0, 0, 0, 1}, [4]float64{0, 0, 0, 1}},
		{"scaled", [4]float64{0, 0, 0, 5}, [4]float64{0, 0, 0, 1}},
		{"half-turn-x", [4]float64{2, 0, 0, 0}, [4]float64{1, 0, 0, 0}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := quatNormalize(tc.in)
			// Sign-insensitive comparison: q and -q represent the same rotation.
			if got[3]*tc.want[3] < 0 {
				for i := range got {
					got[i] = -got[i]
				}
			}
			approxQuat(t, "normalized", got, tc.want, 1e-12)
		})
	}
}

func TestQuatNormalizeZero(t *testing.T) {
	// A zero quaternion cannot be normalized; the function must return it
	// unchanged rather than producing NaN.
	got := quatNormalize([4]float64{0, 0, 0, 0})
	approxQuat(t, "zero", got, [4]float64{0, 0, 0, 0}, 0)
}

func TestQuatMulIdentity(t *testing.T) {
	q := quatNormalize([4]float64{0.1, 0.2, 0.3, 0.9})
	id := [4]float64{0, 0, 0, 1}
	approxQuat(t, "q*id", quatMul(q, id), q, 1e-12)
	approxQuat(t, "id*q", quatMul(id, q), q, 1e-12)
}

func TestQuatMulAssociative(t *testing.T) {
	a := quatNormalize([4]float64{0.20, 0.10, 0.30, 0.90})
	b := quatNormalize([4]float64{0.10, 0.40, 0.20, 0.80})
	c := quatNormalize([4]float64{0.30, 0.20, 0.10, 0.85})

	left := quatMul(quatMul(a, b), c)
	right := quatMul(a, quatMul(b, c))
	approxQuat(t, "associative", left, right, 1e-12)
}

func TestQuatConjIsInverse(t *testing.T) {
	// For a unit quaternion, conjugate == inverse, so q * conj(q) == identity.
	q := quatNormalize([4]float64{0.5, -0.2, 0.3, 0.75})
	prod := quatMul(q, quatConj(q))
	approxQuat(t, "q*conj(q)", prod, [4]float64{0, 0, 0, 1}, 1e-12)
}

func TestQuatFromAxisAngle(t *testing.T) {
	t.Run("known rotation", func(t *testing.T) {
		// 90 degrees about +Z -> yaw of pi/2.
		q := quatFromAxisAngle([3]float64{0, 0, 1}, math.Pi/2)
		want := [4]float64{0, 0, math.Sin(math.Pi / 4), math.Cos(math.Pi / 4)}
		approxQuat(t, "90z", q, want, 1e-12)
		if a := quatAngle(q); math.Abs(a-math.Pi/2) > 1e-9 {
			t.Errorf("angle: got %v, want %v", a, math.Pi/2)
		}
	})
	t.Run("unnormalized axis", func(t *testing.T) {
		// An unnormalized axis must yield the same rotation as the normalized one.
		q1 := quatFromAxisAngle([3]float64{0, 0, 2}, math.Pi/2)
		q2 := quatFromAxisAngle([3]float64{0, 0, 1}, math.Pi/2)
		approxQuat(t, "unnormalized", q1, q2, 1e-12)
	})
	t.Run("zero axis is identity", func(t *testing.T) {
		q := quatFromAxisAngle([3]float64{0, 0, 0}, 1.0)
		approxQuat(t, "zero-axis", q, [4]float64{0, 0, 0, 1}, 0)
	})
}

func TestQuatSlerp(t *testing.T) {
	t.Run("endpoints", func(t *testing.T) {
		a := [4]float64{0, 0, 0, 1}
		b := quatFromAxisAngle([3]float64{0, 0, 1}, math.Pi/2)
		approxQuat(t, "t=0", quatSlerp(a, b, 0), a, 1e-12)
		approxQuat(t, "t=1", quatSlerp(a, b, 1), b, 1e-12)
	})
	t.Run("midpoint angle", func(t *testing.T) {
		a := [4]float64{0, 0, 0, 1}
		b := quatFromAxisAngle([3]float64{0, 0, 1}, math.Pi/2)
		mid := quatSlerp(a, b, 0.5)
		// Halfway rotation should be pi/4 about Z.
		want := math.Pi / 4
		if got := quatAngle(mid); math.Abs(got-want) > 1e-9 {
			t.Errorf("midpoint angle: got %v, want %v", got, want)
		}
	})
	t.Run("shortest arc", func(t *testing.T) {
		// b and -b encode the same attitude; slerp must take the short way
		// regardless of sign, so slerp(a, b, 0.5) == slerp(a, -b, 0.5) in angle.
		a := [4]float64{0, 0, 0, 1}
		b := quatFromAxisAngle([3]float64{1, 0, 0}, math.Pi/2)
		negB := [4]float64{-b[0], -b[1], -b[2], -b[3]}
		if math.Abs(quatAngle(quatSlerp(a, b, 0.5))-quatAngle(quatSlerp(a, negB, 0.5))) > 1e-9 {
			t.Error("slerp did not take the shortest arc for negated target")
		}
	})
	t.Run("near-parallel lerp fallback", func(t *testing.T) {
		// Two very close quaternions must still produce a unit result.
		a := [4]float64{0, 0, 0, 1}
		b := quatNormalize([4]float64{1e-6, 0, 0, 1})
		out := quatSlerp(a, b, 0.5)
		n := math.Sqrt(out[0]*out[0] + out[1]*out[1] + out[2]*out[2] + out[3]*out[3])
		if math.Abs(n-1) > 1e-9 {
			t.Errorf("lerp fallback not unit: |q|=%v", n)
		}
	})
}

func TestEulerRoundTrip(t *testing.T) {
	// quatFromEulerZYX then quatToEulerZYX must recover the angles (away from
	// gimbal lock).
	cases := []struct{ yaw, pitch, roll float64 }{
		{0, 0, 0},
		{0.7, -0.3, 0.5},
		{-1.2, 0.4, -0.8},
		{2.0, 0.2, -1.0},
		{-0.1, 1.2, 0.3},
	}
	for _, c := range cases {
		q := quatFromEulerZYX(c.yaw, c.pitch, c.roll)
		yaw, pitch, roll := quatToEulerZYX(q)
		const tol = 1e-6
		if math.Abs(yaw-c.yaw) > tol || math.Abs(pitch-c.pitch) > tol || math.Abs(roll-c.roll) > tol {
			t.Errorf("round-trip (%.3f,%.3f,%.3f): got (%.6f,%.6f,%.6f)",
				c.yaw, c.pitch, c.roll, yaw, pitch, roll)
		}
	}
}

func TestQuatToEulerDegrees(t *testing.T) {
	q := quatFromEulerZYX(0, 0, math.Pi/2) // 90 deg roll
	rollX, pitchY, yawZ := QuatToEulerZYXDegrees(q)
	const tol = 1e-6
	if math.Abs(rollX-90) > tol || math.Abs(pitchY) > tol || math.Abs(yawZ) > tol {
		t.Errorf("QuatToEulerZYXDegrees: got (roll=%.4f pitch=%.4f yaw=%.4f), want roll=90", rollX, pitchY, yawZ)
	}
}

// TestQuatToTiltDegrees checks the two-axis level-tilt projection consumed by
// the calibration card SSE handler. Conventions (docs/references/gyro-attitude-sse.md §2):
//   - level device reads (0, 0)
//   - front dips  -> positive pitch  (rotation about +Y)
//   - right dips  -> positive roll   (rotation about -X)
//   - pure yaw (about Z) leaves pitch/roll unchanged while the device is level
func TestQuatToTiltDegrees(t *testing.T) {
	const (
		deg = math.Pi / 180
		tol = 1e-6
	)
	cases := []struct {
		name      string
		yaw, p, r float64 // radians, fed to quatFromEulerZYX
		wantPitch float64 // degrees
		wantRoll  float64 // degrees
	}{
		{"level", 0, 0, 0, 0, 0},
		{"front dips 10", 0, 10 * deg, 0, 10, 0},
		{"back dips 10", 0, -10 * deg, 0, -10, 0},
		{"right dips 10", 0, 0, -10 * deg, 0, 10},
		{"left dips 10", 0, 0, 10 * deg, 0, -10},
		{"pure yaw level", 30 * deg, 0, 0, 0, 0},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			pitch, roll := QuatToTiltDegrees(quatFromEulerZYX(c.yaw, c.p, c.r))
			if math.Abs(pitch-c.wantPitch) > tol || math.Abs(roll-c.wantRoll) > tol {
				t.Errorf("got (pitch=%.4f, roll=%.4f), want (%.4f, %.4f)",
					pitch, roll, c.wantPitch, c.wantRoll)
			}
		})
	}
}

func TestVecNormalize(t *testing.T) {
	v := vecNormalize([3]float64{0, 0, 3})
	approxQuat(t, "vec", [4]float64{v[0], v[1], v[2], 0}, [4]float64{0, 0, 1, 0}, 1e-12)
	if got := vecNormalize([3]float64{0, 0, 0}); got != ([3]float64{0, 0, 0}) {
		t.Errorf("zero vec not preserved: %v", got)
	}
}

func TestApplyMatrix3(t *testing.T) {
	id := [9]float64{1, 0, 0, 0, 1, 0, 0, 0, 1}
	swapXY := [9]float64{0, 1, 0, 1, 0, 0, 0, 0, 1}
	tests := []struct {
		name    string
		m       [9]float64
		v, want [3]float64
	}{
		{"identity", id, [3]float64{1, 2, 3}, [3]float64{1, 2, 3}},
		{"swap xy", swapXY, [3]float64{1, 2, 3}, [3]float64{2, 1, 3}},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			got := applyMatrix3(tc.m, tc.v)
			for i := 0; i < 3; i++ {
				if math.Abs(got[i]-tc.want[i]) > 1e-12 {
					t.Errorf("got %v, want %v", got, tc.want)
				}
			}
		})
	}
}
