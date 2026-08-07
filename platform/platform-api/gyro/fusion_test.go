package gyro

import (
	"math"
	"testing"
)

// gravityLevel returns an at-rest, level accelerometer reading in the spec
// frame (+Z up): the LSM6DSR convention where a stationary device reads +g on Z.
func gravityLevel() [3]float64 {
	return [3]float64{0, 0, gravityNominal}
}

// tiltedAccel returns the reading that the fusion must invert to (roll=phi,
// pitch=theta, yaw unknown) per its atan2 tilt formulas, at full gravity.
func tiltedAccel(phi, theta float64) [3]float64 {
	g := gravityNominal
	return [3]float64{
		-g * math.Sin(theta),
		g * math.Sin(phi) * math.Cos(theta),
		g * math.Cos(phi) * math.Cos(theta),
	}
}

func identityMount() [9]float64 {
	return [9]float64{1, 0, 0, 0, 1, 0, 0, 0, 1}
}

func TestNewFusionAlphaClamp(t *testing.T) {
	cases := []struct {
		in   float64
		want float64
	}{
		{0.2, 0.2},
		{0, defaultFusionAlpha},
		{1.0, defaultFusionAlpha},
		{1.5, defaultFusionAlpha},
		{-0.1, defaultFusionAlpha},
	}
	for _, c := range cases {
		f := NewFusion(c.in, identityMount())
		if f.alpha != c.want {
			t.Errorf("alpha(%v) = %v, want %v", c.in, f.alpha, c.want)
		}
	}
}

func TestFusionStartsAtIdentity(t *testing.T) {
	f := NewFusion(0.05, identityMount())
	approxQuat(t, "initial", f.Quat(), [4]float64{0, 0, 0, 1}, 0)
}

func TestFusionReset(t *testing.T) {
	f := NewFusion(0.05, identityMount())
	// Perturb with a gyro step, then reset.
	f.Update(gravityLevel(), [3]float64{0.1, 0.2, 0.3}, 0.1)
	f.Reset()
	approxQuat(t, "after reset", f.Quat(), [4]float64{0, 0, 0, 1}, 0)
}

func TestFusionLevelStaysIdentity(t *testing.T) {
	// A level device with no rotation must remain at the identity orientation.
	f := NewFusion(0.05, identityMount())
	for i := 0; i < 200; i++ {
		f.Update(gravityLevel(), [3]float64{0, 0, 0}, 0.01)
	}
	approxQuat(t, "level steady-state", f.Quat(), [4]float64{0, 0, 0, 1}, 1e-9)
}

func TestFusionTiltCorrectionConverges(t *testing.T) {
	// Start rolled by the gyro, then present a level accelerometer; the tilt
	// correction must pull roll/pitch back toward the accel truth.
	f := NewFusion(0.2, identityMount())
	// Induce a roll with a brief gyro impulse.
	f.Update(gravityLevel(), [3]float64{2.0, 0, 0}, 0.1)
	_, _, rollBefore := quatToEulerZYX(f.Quat())
	if math.Abs(rollBefore) < 0.01 {
		t.Fatalf("expected non-zero roll after gyro impulse, got %v", rollBefore)
	}
	// Now hold still and level: accel should correct roll back toward 0.
	for i := 0; i < 600; i++ {
		f.Update(gravityLevel(), [3]float64{0, 0, 0}, 0.01)
	}
	_, _, rollAfter := quatToEulerZYX(f.Quat())
	if math.Abs(rollAfter) > 0.02 {
		t.Errorf("roll did not converge to 0: got %v", rollAfter)
	}
}

func TestFusionTiltFromAccel(t *testing.T) {
	// Feeding a tilted accelerometer with no gyro must converge to the tilt
	// implied by that gravity direction.
	wantPhi := 0.25   // roll
	wantTheta := 0.30 // pitch
	accel := tiltedAccel(wantPhi, wantTheta)

	f := NewFusion(0.1, identityMount())
	for i := 0; i < 800; i++ {
		f.Update(accel, [3]float64{0, 0, 0}, 0.01)
	}
	yaw, pitch, roll := quatToEulerZYX(f.Quat())
	const tol = 0.01
	if math.Abs(roll-wantPhi) > tol {
		t.Errorf("roll: got %.4f, want %.4f", roll, wantPhi)
	}
	if math.Abs(pitch-wantTheta) > tol {
		t.Errorf("pitch: got %.4f, want %.4f", pitch, wantTheta)
	}
	// slerp between identity and a quaternion carrying both roll and pitch does
	// not stay in the yaw=0 submanifold, so a small bounded yaw artifact (~1°)
	// is expected and acceptable. It must not grow unbounded (see PreservesYaw).
	if math.Abs(yaw) > 0.05 {
		t.Errorf("yaw leaked: got %.4f, want ~0", yaw)
	}
}

func TestFusionPreservesYaw(t *testing.T) {
	// The headline property: an accelerometer has no yaw information, so the
	// filter must leave gyro-integrated yaw untouched. A naive whole-quaternion
	// slerp toward the accel attitude would collapse yaw to 0 within ~1s.
	f := NewFusion(0.05, identityMount())

	// Spin up yaw to a known angle about +Z while level.
	const omega = 1.0
	const dt = 0.02
	const spinSteps = 100
	targetYaw := omega * dt * spinSteps // = 2.0 rad
	for i := 0; i < spinSteps; i++ {
		f.Update(gravityLevel(), [3]float64{0, 0, omega}, dt)
	}
	yaw, _, _ := quatToEulerZYX(f.Quat())
	if math.Abs(yaw-targetYaw) > 1e-6 {
		t.Fatalf("yaw after spin: got %.6f, want %.6f", yaw, targetYaw)
	}

	// Hold level + stationary for many steps. Yaw must NOT decay toward 0.
	for i := 0; i < 300; i++ {
		f.Update(gravityLevel(), [3]float64{0, 0, 0}, dt)
	}
	yaw, _, _ = quatToEulerZYX(f.Quat())
	if math.Abs(yaw-targetYaw) > 1e-3 {
		t.Errorf("yaw collapsed during hold: got %.6f, want %.6f (decayed %.4f)",
			yaw, targetYaw, targetYaw-yaw)
	}
}

func TestFusionGravityBandSkipsCorrection(t *testing.T) {
	// Outside the gravity band (free-fall or violent acceleration) the
	// accelerometer must not corrupt the estimate; the filter trusts gyro only.
	f := NewFusion(0.5, identityMount())

	// Establish a yaw angle via gyro.
	f.Update(gravityLevel(), [3]float64{0, 0, 1.0}, 0.5) // yaw += 0.5 rad
	yawSet, _, _ := quatToEulerZYX(f.Quat())

	// Feed out-of-band accel that, if trusted, would impose a large bogus tilt.
	outOfBand := [][3]float64{
		{0, 0, 0},  // free-fall (aMag=0)
		{20, 0, 0}, // aMag=20 > 15
		{0, 0, 4},  // aMag=4 < 5
	}
	for _, a := range outOfBand {
		qBefore := f.Quat()
		f.Update(a, [3]float64{0, 0, 0}, 0.01)
		// Orientation must be unchanged (gyro was zero, accel ignored).
		approxQuat(t, "out-of-band", f.Quat(), qBefore, 1e-12)
	}
	// Yaw is preserved.
	yawNow, _, _ := quatToEulerZYX(f.Quat())
	if math.Abs(yawNow-yawSet) > 1e-9 {
		t.Errorf("yaw changed during out-of-band step: %.6f -> %.6f", yawSet, yawNow)
	}
}

func TestFusionMountMatrix(t *testing.T) {
	// The mount matrix remaps raw sensor axes before fusion. A reading on the
	// sensor +X axis must be interpreted differently under identity vs swap-XY.
	g := gravityNominal
	raw := [3]float64{g, 0, 0}                      // sensor reads +g on X
	swapXY := [9]float64{0, 1, 0, 1, 0, 0, 0, 0, 1} // raw X -> world Y

	fid := NewFusion(0.1, identityMount())
	fsw := NewFusion(0.1, swapXY)
	for i := 0; i < 800; i++ {
		fid.Update(raw, [3]float64{0, 0, 0}, 0.01)
		fsw.Update(raw, [3]float64{0, 0, 0}, 0.01)
	}
	_, pid, _ := quatToEulerZYX(fid.Quat())
	_, _, rsw := quatToEulerZYX(fsw.Quat())

	// Under identity, raw +X gravity => pitch ~= -pi/2 (nose down).
	// Under swapXY, raw +X maps to world +Y => roll ~= +pi/2.
	const tol = 0.02
	if math.Abs(pid-(-math.Pi/2)) > tol {
		t.Errorf("identity pitch: got %.4f, want -pi/2", pid)
	}
	if math.Abs(rsw-(math.Pi/2)) > tol {
		t.Errorf("swapXY roll: got %.4f, want +pi/2", rsw)
	}
}

func TestFusionGyroIntegrationOnly(t *testing.T) {
	// With dt==0 the gyro is not integrated, so orientation is frozen even if a
	// non-zero rate is supplied.
	f := NewFusion(0.05, identityMount())
	for i := 0; i < 10; i++ {
		f.Update(gravityLevel(), [3]float64{0, 0, 5.0}, 0)
	}
	approxQuat(t, "dt=0 frozen", f.Quat(), [4]float64{0, 0, 0, 1}, 1e-12)
}

func TestUpdate_SubtractsGyroBias(t *testing.T) {
	// A constant gyro reading equal to the installed bias must integrate to net
	// zero rotation on every axis once the bias is subtracted (body frame,
	// before the mount matrix). The device stays at the identity attitude.
	bias := [3]float64{0.01, -0.02, 0.015} // rad/s static bias
	const steps = 1000
	const dt = 0.01

	f := NewFusion(0.05, identityMount())
	f.SetGyroBias(bias)
	for i := 0; i < steps; i++ {
		f.Update(gravityLevel(), bias, dt) // measured gyro == bias -> net zero
	}
	yaw, pitch, roll := quatToEulerZYX(f.Quat())
	const tol = 0.005 // ~0.3 deg
	if math.Abs(yaw) > tol || math.Abs(pitch) > tol || math.Abs(roll) > tol {
		t.Errorf("bias subtracted: yaw=%.4f pitch=%.4f roll=%.4f, want all ~0 (tol %.4f)", yaw, pitch, roll, tol)
	}

	// Contrast: without bias subtraction the same input drives unbounded yaw,
	// because the accelerometer has no yaw reference to correct it. Z bias
	// (0.015 rad/s) over 10 s = ~0.15 rad.
	f2 := NewFusion(0.05, identityMount())
	for i := 0; i < steps; i++ {
		f2.Update(gravityLevel(), bias, dt)
	}
	yaw2, _, _ := quatToEulerZYX(f2.Quat())
	if math.Abs(yaw2) < 0.05 {
		t.Errorf("no-bias yaw did not drift as expected: got %.4f, want |yaw| > 0.05", yaw2)
	}
}

func TestSetGyroBias_RoundTrip(t *testing.T) {
	f := NewFusion(0.05, identityMount())
	var want [3]float64
	if got := f.GyroBias(); got != want {
		t.Errorf("default bias: got %v, want zero", got)
	}
	want = [3]float64{0.1, -0.2, 0.3}
	f.SetGyroBias(want)
	got := f.GyroBias()
	if got != want {
		t.Errorf("round-trip: got %v, want %v", got, want)
	}
}
