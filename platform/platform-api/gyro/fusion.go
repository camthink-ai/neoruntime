package gyro

import "math"

// Gravity band (m/s^2) inside which the accelerometer is treated as a reliable
// tilt reference. Outside it (free-fall, large linear acceleration) the filter
// trusts the gyroscope alone for that step so wild motion can't corrupt tilt.
const (
	accelValidMin  = 5.0
	accelValidMax  = 15.0
	gravityNominal = 9.80665
)

// defaultFusionAlpha is used when the configured alpha is out of range.
const defaultFusionAlpha = 0.05

// Fusion is a complementary (gyro + accel) attitude estimator.
//
// State is a single normalized quaternion q mapping the sensor body frame to
// the spec world frame (X forward, Y right, Z up). The filter is NOT
// thread-safe: it is driven by exactly one read goroutine (see IIOSource).
//
// The fusion preserves gyroscope-integrated yaw. Because an accelerometer
// carries no yaw information, naively slerping the whole quaternion toward the
// accel-derived attitude would collapse yaw to zero within ~1 s. Instead only
// the tilt (roll/pitch) is corrected each step; yaw is left to the gyro.
type Fusion struct {
	q        [4]float64 // body -> world, normalized
	alpha    float64    // complementary coefficient in (0,1), ~0.02..0.1
	mount    [9]float64 // row-major 3x3 body -> spec frame
	gyroBias [3]float64 // body-frame static bias (rad/s), subtracted before mount
}

// NewFusion returns a Fusion starting at the identity attitude.
//
// alpha blends accel tilt into the gyro estimate each step: smaller = smoother
// but slower to correct drift, larger = noisier but snappier. Out-of-range
// values fall back to defaultFusionAlpha. mount is a row-major 3x3 matrix
// mapping the LSM6DSR body axes onto the spec world frame (default identity);
// it should be calibrated per PCB orientation so a level, front-facing device
// reads as the identity quaternion.
func NewFusion(alpha float64, mount [9]float64) *Fusion {
	if !(alpha > 0 && alpha < 1) {
		alpha = defaultFusionAlpha
	}
	return &Fusion{
		q:     [4]float64{0, 0, 0, 1},
		alpha: alpha,
		mount: normalizeMountMatrix(mount),
	}
}

// Quat returns the current fused orientation.
func (f *Fusion) Quat() [4]float64 { return f.q }

// Reset returns the estimator to the identity attitude.
func (f *Fusion) Reset() { f.q = [4]float64{0, 0, 0, 1} }

// SetGyroBias installs a body-frame static gyro bias (rad/s) estimated by
// startup calibration. It is subtracted from every subsequent raw gyro sample
// before the mount matrix is applied. Passing the zero value disables bias
// compensation.
func (f *Fusion) SetGyroBias(b [3]float64) { f.gyroBias = b }

// GyroBias returns the currently installed body-frame gyro bias (rad/s).
func (f *Fusion) GyroBias() [3]float64 { return f.gyroBias }

// Update folds one accel + gyro sample into the estimate and returns the new
// orientation.
//
// accel is in m/s^2, gyro in rad/s, dt in seconds. Both are given in the raw
// sensor body frame; the mount matrix is applied internally. A static gyro bias
// installed via SetGyroBias is subtracted from the raw gyro first (in the body
// frame, before the mount matrix), so a stationary sensor no longer integrates
// its zero-rate offset into unbounded yaw drift.
func (f *Fusion) Update(accel, gyro [3]float64, dt float64) [4]float64 {
	// 0. Subtract the static body-frame bias before any frame mapping.
	gyro[0] -= f.gyroBias[0]
	gyro[1] -= f.gyroBias[1]
	gyro[2] -= f.gyroBias[2]

	// 1. Map body -> spec world frame.
	accel = applyMatrix3(f.mount, accel)
	gyro = applyMatrix3(f.mount, gyro)

	// 2. Gyroscope integration (high-pass: smooth, drifts over time).
	if dt > 0 {
		omegaMag := math.Sqrt(gyro[0]*gyro[0] + gyro[1]*gyro[1] + gyro[2]*gyro[2])
		deltaQ := quatFromAxisAngle(gyro, omegaMag*dt)
		f.q = quatNormalize(quatMul(f.q, deltaQ))
	}

	// 3. Accelerometer tilt correction (low-pass: drift-free roll/pitch).
	//    Skipped outside the gravity band so free-fall / large linear accel
	//    cannot poison the tilt estimate.
	aMag := math.Sqrt(accel[0]*accel[0] + accel[1]*accel[1] + accel[2]*accel[2])
	if aMag > accelValidMin && aMag < accelValidMax {
		// Absolute roll (about X) and pitch (about Y) from the gravity
		// direction. These read 0 for a level device.
		roll := math.Atan2(accel[1], accel[2])
		pitch := math.Atan2(-accel[0], math.Sqrt(accel[1]*accel[1]+accel[2]*accel[2]))
		qAccel := quatFromEulerZYX(0, pitch, roll) // yaw unknown -> 0

		// Decompose current estimate into yaw + tilt so we only correct tilt.
		// qGyro = qYaw * qTilt; isolating qTilt lets us slerp tilt toward
		// qAccel without disturbing yaw.
		yaw, _, _ := quatToEulerZYX(f.q)
		qYaw := quatFromEulerZYX(yaw, 0, 0)
		qTilt := quatMul(quatConj(qYaw), f.q)
		qTiltFused := quatSlerp(qTilt, qAccel, f.alpha)
		f.q = quatNormalize(quatMul(qYaw, qTiltFused))
	}

	return f.q
}

// applyMatrix3 multiplies the row-major 3x3 matrix m by the column vector v.
func applyMatrix3(m [9]float64, v [3]float64) [3]float64 {
	return [3]float64{
		m[0]*v[0] + m[1]*v[1] + m[2]*v[2],
		m[3]*v[0] + m[4]*v[1] + m[5]*v[2],
		m[6]*v[0] + m[7]*v[1] + m[8]*v[2],
	}
}
