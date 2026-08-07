package gyro

import "math"

// Quaternions are fixed-size arrays [4]float64 laid out as [x, y, z, w].
//
// They represent the body-to-world rotation in a right-handed frame. All
// quaternions leaving this package are normalized. Rotation convention for the
// euler helpers is ZYX (intrinsic): R = Rz(yaw) * Ry(pitch) * Rx(roll).

// quatMul returns the Hamilton product a*b.
func quatMul(a, b [4]float64) [4]float64 {
	ax, ay, az, aw := a[0], a[1], a[2], a[3]
	bx, by, bz, bw := b[0], b[1], b[2], b[3]
	return [4]float64{
		aw*bx + ax*bw + ay*bz - az*by,
		aw*by - ax*bz + ay*bw + az*bx,
		aw*bz + ax*by - ay*bx + az*bw,
		aw*bw - ax*bx - ay*by - az*bz,
	}
}

// quatNormalize returns the unit quaternion of q. If q has zero length it is
// returned unchanged (callers must avoid passing zero-length quaternions).
func quatNormalize(q [4]float64) [4]float64 {
	n := math.Sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3])
	if n == 0 {
		return q
	}
	inv := 1.0 / n
	return [4]float64{q[0] * inv, q[1] * inv, q[2] * inv, q[3] * inv}
}

// quatConj returns the conjugate (inverse for unit quaternions).
func quatConj(q [4]float64) [4]float64 {
	return [4]float64{-q[0], -q[1], -q[2], q[3]}
}

// quatFromAxisAngle returns the unit quaternion for a rotation of angleRad
// about axis. axis need not be normalized; a zero axis yields the identity.
func quatFromAxisAngle(axis [3]float64, angleRad float64) [4]float64 {
	axis = vecNormalize(axis)
	if axis[0] == 0 && axis[1] == 0 && axis[2] == 0 {
		return [4]float64{0, 0, 0, 1}
	}
	half := angleRad * 0.5
	s := math.Sin(half)
	return [4]float64{axis[0] * s, axis[1] * s, axis[2] * s, math.Cos(half)}
}

// quatSlerp returns the spherical linear interpolation between a and b at
// parameter t in [0,1]. The shortest arc is always taken.
func quatSlerp(a, b [4]float64, t float64) [4]float64 {
	dot := a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3]
	// Choose the shortest direction.
	if dot < 0 {
		b = [4]float64{-b[0], -b[1], -b[2], -b[3]}
		dot = -dot
	}
	// If the inputs are very close, fall back to a normalized lerp.
	const threshold = 0.9995
	if dot > threshold {
		out := [4]float64{
			a[0] + t*(b[0]-a[0]),
			a[1] + t*(b[1]-a[1]),
			a[2] + t*(b[2]-a[2]),
			a[3] + t*(b[3]-a[3]),
		}
		return quatNormalize(out)
	}
	theta0 := math.Acos(dot)
	sinTheta0 := math.Sin(theta0)
	theta := theta0 * t
	s0 := math.Cos(theta) - dot*math.Sin(theta)/sinTheta0
	s1 := math.Sin(theta) / sinTheta0
	return [4]float64{
		s0*a[0] + s1*b[0],
		s0*a[1] + s1*b[1],
		s0*a[2] + s1*b[2],
		s0*a[3] + s1*b[3],
	}
}

// quatFromEulerZYX builds a quaternion from intrinsic ZYX Tait-Bryan angles
// (radians): yaw about Z, pitch about Y, roll about X.
//
// R = Rz(yaw) * Ry(pitch) * Rx(roll)
func quatFromEulerZYX(yaw, pitch, roll float64) [4]float64 {
	cy, sy := math.Cos(yaw*0.5), math.Sin(yaw*0.5)
	cp, sp := math.Cos(pitch*0.5), math.Sin(pitch*0.5)
	cr, sr := math.Cos(roll*0.5), math.Sin(roll*0.5)
	return [4]float64{
		sr*cp*cy - cr*sp*sy, // x
		cr*sp*cy + sr*cp*sy, // y
		cr*cp*sy - sr*sp*cy, // z
		cr*cp*cy + sr*sp*sy, // w
	}
}

// QuatToEulerZYXDegrees converts a normalized quaternion to intrinsic ZYX
// Tait-Bryan angles in degrees. Returns (rollX, pitchY, yawZ): rotation about
// X, about Y, about Z. This is the inverse of quatFromEulerZYX expressed in
// degrees for SSE clients that request format=euler.
func QuatToEulerZYXDegrees(q [4]float64) (rollX, pitchY, yawZ float64) {
	yaw, pitch, roll := quatToEulerZYX(q)
	const deg = 180 / math.Pi
	return roll * deg, pitch * deg, yaw * deg
}

// quatToEulerZYX extracts intrinsic ZYX Tait-Bryan angles (radians) from a
// unit quaternion. Returns (yaw, pitch, roll): yaw about Z, pitch about Y,
// roll about X. It is the inverse of quatFromEulerZYX.
func quatToEulerZYX(q [4]float64) (yaw, pitch, roll float64) {
	x, y, z, w := q[0], q[1], q[2], q[3]
	// Roll (X axis).
	sinrCosp := 2 * (w*x + y*z)
	cosrCosp := 1 - 2*(x*x+y*y)
	roll = math.Atan2(sinrCosp, cosrCosp)
	// Pitch (Y axis), clamped against gimbal lock.
	sinp := 2 * (w*y - z*x)
	switch {
	case sinp >= 1:
		pitch = math.Pi / 2
	case sinp <= -1:
		pitch = -math.Pi / 2
	default:
		pitch = math.Asin(sinp)
	}
	// Yaw (Z axis).
	sinyCosp := 2 * (w*z + x*y)
	cosyCosp := 1 - 2*(y*y+z*z)
	yaw = math.Atan2(sinyCosp, cosyCosp)
	return yaw, pitch, roll
}

// QuatToTiltDegrees projects a normalized body-to-world quaternion onto the
// two-axis level-tilt reported to the calibration card. It rotates the body +Z
// axis (the device top-face normal) into the world frame and derives:
//
//	pitch = atan2(up.x, up.z) — front-back tilt; positive = front dips (前倾).
//	roll  = atan2(up.y, up.z) — left-right tilt; positive = right dips (右倾).
//
// Yaw is intentionally dropped: the card only cares how far the device is off
// level, not its heading, so a pure rotation about Z leaves pitch/roll unchanged.
// Both angles are clamped to [-90, 90], matching the frontend's MAX_TILT_DEG.
// See docs/references/gyro-attitude-sse.md §2.
func QuatToTiltDegrees(q [4]float64) (pitch, roll float64) {
	up := quatRotateVec(q, [3]float64{0, 0, 1})
	const deg = 180 / math.Pi
	pitch = math.Atan2(up[0], up[2]) * deg
	roll = math.Atan2(up[1], up[2]) * deg
	if pitch < -90 {
		pitch = -90
	} else if pitch > 90 {
		pitch = 90
	}
	if roll < -90 {
		roll = -90
	} else if roll > 90 {
		roll = 90
	}
	return pitch, roll
}

// quatRotateVec rotates vector v by the unit quaternion q (body-to-world) using
// the optimized form v' = v + 2w(u×v) + 2(u×(u×v)) where u is q's vector part.
func quatRotateVec(q [4]float64, v [3]float64) [3]float64 {
	u := [3]float64{q[0], q[1], q[2]}
	w := q[3]
	uxv := vec3Cross(u, v)
	uxuxv := vec3Cross(u, uxv)
	return [3]float64{
		v[0] + 2*w*uxv[0] + 2*uxuxv[0],
		v[1] + 2*w*uxv[1] + 2*uxuxv[1],
		v[2] + 2*w*uxv[2] + 2*uxuxv[2],
	}
}

// vec3Cross returns the cross product a × b.
func vec3Cross(a, b [3]float64) [3]float64 {
	return [3]float64{
		a[1]*b[2] - a[2]*b[1],
		a[2]*b[0] - a[0]*b[2],
		a[0]*b[1] - a[1]*b[0],
	}
}

// vecNormalize returns the unit vector of v; a zero vector is returned as-is.
func vecNormalize(v [3]float64) [3]float64 {
	n := math.Sqrt(v[0]*v[0] + v[1]*v[1] + v[2]*v[2])
	if n == 0 {
		return v
	}
	inv := 1.0 / n
	return [3]float64{v[0] * inv, v[1] * inv, v[2] * inv}
}
