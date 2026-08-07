// Package gyro provides real-time device attitude (orientation) data sourced
// from the on-board LSM6DSR IMU via the Linux IIO sysfs interface.
//
// The package fuses accelerometer + gyroscope samples with a complementary
// filter into a normalized orientation quaternion expressed in the Z-up world
// frame expected by the web console (X forward, Y right, Z up).
//
// A Source is the pluggable data contract consumed by the SSE handler. The
// default implementation is IIOSource (sysfs reader); future implementations
// may source data from device-control gRPC or the event bus without changing
// the handler.
package gyro

import "time"

// StatusCode reports the health of the underlying sensor.
type StatusCode string

const (
	// StatusOnline means the sensor is producing samples.
	StatusOnline StatusCode = "online"
	// StatusOffline means the sensor has not been detected / is unavailable.
	StatusOffline StatusCode = "offline"
	// StatusError means the sensor was detected but reads are failing.
	StatusError StatusCode = "error"
)

// Sample is one attitude observation.
//
// Quat is a normalized quaternion [x, y, z, w] representing the body-to-world
// rotation in the Z-up world frame. It is always normalized before publishing.
// PitchDeg/RollDeg are the fast level-card tilt values when TiltValid is true.
// They are computed directly from the accelerometer gravity vector with a
// time-constant low-pass, so they converge by wall-clock time instead of by the
// slow sysfs sample count.
type Sample struct {
	Timestamp time.Time
	Quat      [4]float64 // [x, y, z, w], normalized
	PitchDeg  float64
	RollDeg   float64
	TiltValid bool
}

// Source is the attitude data contract consumed by the SSE handler.
//
// The channel returned by Subscribe carries the high-frequency orientation
// stream only. Status changes are queried via Status() and are low-frequency;
// the handler polls them on a slow ticker.
type Source interface {
	// Status returns the current sensor status.
	Status() StatusCode
	// Subscribe returns a channel that receives fused attitude samples.
	// Each returned channel must be passed back to Unsubscribe when no longer
	// needed so the source can release its resources.
	Subscribe() <-chan Sample
	// Unsubscribe removes and closes a previously subscribed channel.
	Unsubscribe(ch <-chan Sample)
}
