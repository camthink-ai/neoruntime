package gyro

import "math"

const defaultTiltTauSeconds = 0.5

type tiltEstimator struct {
	initialized bool
	pitchDeg    float64
	rollDeg     float64
}

func (t *tiltEstimator) Update(accel [3]float64, mount [9]float64, dt float64) (pitchDeg, rollDeg float64, ok bool) {
	rawPitch, rawRoll, valid := accelTiltDegrees(accel, mount)
	if !valid {
		return t.pitchDeg, t.rollDeg, t.initialized
	}
	if !t.initialized || dt <= 0 {
		t.pitchDeg = rawPitch
		t.rollDeg = rawRoll
		t.initialized = true
		return t.pitchDeg, t.rollDeg, true
	}

	a := lowPassAlpha(dt, defaultTiltTauSeconds)
	t.pitchDeg += a * (rawPitch - t.pitchDeg)
	t.rollDeg += a * (rawRoll - t.rollDeg)
	return t.pitchDeg, t.rollDeg, true
}

func accelTiltDegrees(accel [3]float64, mount [9]float64) (pitchDeg, rollDeg float64, ok bool) {
	accel = applyMatrix3(normalizeMountMatrix(mount), accel)
	aMag := math.Sqrt(accel[0]*accel[0] + accel[1]*accel[1] + accel[2]*accel[2])
	if !(aMag > accelValidMin && aMag < accelValidMax) {
		return 0, 0, false
	}

	const deg = 180 / math.Pi
	pitchDeg = math.Atan2(-accel[0], math.Sqrt(accel[1]*accel[1]+accel[2]*accel[2])) * deg
	rollDeg = math.Atan2(accel[1], accel[2]) * deg
	return clampTiltDeg(pitchDeg), clampTiltDeg(rollDeg), true
}

func lowPassAlpha(dt, tau float64) float64 {
	if dt <= 0 || tau <= 0 {
		return 1
	}
	a := 1 - math.Exp(-dt/tau)
	if a < 0 {
		return 0
	}
	if a > 1 {
		return 1
	}
	return a
}

func clampTiltDeg(v float64) float64 {
	if v < -90 {
		return -90
	}
	if v > 90 {
		return 90
	}
	return v
}

func normalizeMountMatrix(m [9]float64) [9]float64 {
	if m == ([9]float64{}) {
		return [9]float64{1, 0, 0, 0, 1, 0, 0, 0, 1}
	}
	return m
}
