package gyro

import (
	"encoding/json"
	"fmt"
	"math"
	"os"
)

// Calibration is the subset of the shipped IMU calibration consumed by the
// platform-api attitude stream.
type Calibration struct {
	GyroBias    [3]float64
	MountMatrix [9]float64
}

type calibrationFile struct {
	GbiasX float64 `json:"gbias_x"`
	GbiasY float64 `json:"gbias_y"`
	GbiasZ float64 `json:"gbias_z"`
	RotX   float64 `json:"rot_x"`
	RotY   float64 `json:"rot_y"`
	RotZ   float64 `json:"rot_z"`
}

// LoadCalibrationFile reads the fleet IMU calibration JSON installed on Hailo
// devices. The rotation fields are radians and are interpreted with the legacy
// inverse X-Z-Y convention used by that file. Applying this mount is intentionally
// opt-in because field units may already match the platform-api body frame.
func LoadCalibrationFile(path string) (Calibration, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return Calibration{}, err
	}
	var f calibrationFile
	if err := json.Unmarshal(data, &f); err != nil {
		return Calibration{}, fmt.Errorf("parse calibration JSON: %w", err)
	}
	if !finite(f.GbiasX, f.GbiasY, f.GbiasZ, f.RotX, f.RotY, f.RotZ) {
		return Calibration{}, fmt.Errorf("calibration contains non-finite values")
	}
	return Calibration{
		GyroBias:    [3]float64{f.GbiasX, f.GbiasY, f.GbiasZ},
		MountMatrix: mountMatrixFromCalibrationRotation(f.RotX, f.RotY, f.RotZ),
	}, nil
}

func mountMatrixFromCalibrationRotation(rotX, rotY, rotZ float64) [9]float64 {
	r := matMul3(rotYMatrix(rotY), matMul3(rotZMatrix(rotZ), rotXMatrix(rotX)))
	return transpose3(r)
}

func rotXMatrix(a float64) [9]float64 {
	c, s := math.Cos(a), math.Sin(a)
	return [9]float64{
		1, 0, 0,
		0, c, -s,
		0, s, c,
	}
}

func rotYMatrix(a float64) [9]float64 {
	c, s := math.Cos(a), math.Sin(a)
	return [9]float64{
		c, 0, s,
		0, 1, 0,
		-s, 0, c,
	}
}

func rotZMatrix(a float64) [9]float64 {
	c, s := math.Cos(a), math.Sin(a)
	return [9]float64{
		c, -s, 0,
		s, c, 0,
		0, 0, 1,
	}
}

func matMul3(a, b [9]float64) [9]float64 {
	var out [9]float64
	for row := 0; row < 3; row++ {
		for col := 0; col < 3; col++ {
			out[row*3+col] =
				a[row*3+0]*b[0*3+col] +
					a[row*3+1]*b[1*3+col] +
					a[row*3+2]*b[2*3+col]
		}
	}
	return out
}

func transpose3(m [9]float64) [9]float64 {
	return [9]float64{
		m[0], m[3], m[6],
		m[1], m[4], m[7],
		m[2], m[5], m[8],
	}
}

func finite(vals ...float64) bool {
	for _, v := range vals {
		if math.IsNaN(v) || math.IsInf(v, 0) {
			return false
		}
	}
	return true
}
