package gyro

import (
	"math"
	"os"
	"path/filepath"
	"testing"
)

func TestLoadCalibrationFile(t *testing.T) {
	path := filepath.Join(t.TempDir(), "final_calibration.json")
	const body = `{
  "gbias_x": -0.00026183332518330324,
  "gbias_y": -0.002437385549437321,
  "gbias_z": -0.0013050431202490205,
  "rot_x": 1.2052200746701804,
  "rot_y": -1.2308287877005744,
  "rot_z": 1.1957927249313691
}`
	if err := os.WriteFile(path, []byte(body), 0o644); err != nil {
		t.Fatal(err)
	}

	cal, err := LoadCalibrationFile(path)
	if err != nil {
		t.Fatalf("LoadCalibrationFile: %v", err)
	}

	wantBias := [3]float64{
		-0.00026183332518330324,
		-0.002437385549437321,
		-0.0013050431202490205,
	}
	if cal.GyroBias != wantBias {
		t.Errorf("GyroBias = %v, want %v", cal.GyroBias, wantBias)
	}

	wantMount := [9]float64{
		0.122137069273305, 0.930506302642615, 0.345312260210521,
		-0.991388011935337, 0.130939058399363, -0.002184668484594,
		-0.047247709999821, -0.342071606140964, 0.938485306316467,
	}
	for i := range wantMount {
		if math.Abs(cal.MountMatrix[i]-wantMount[i]) > 1e-12 {
			t.Fatalf("MountMatrix[%d] = %.15f, want %.15f", i, cal.MountMatrix[i], wantMount[i])
		}
	}
}

func TestCalibrationMountIsNotSafeDefaultForObservedLevelSample(t *testing.T) {
	mount := mountMatrixFromCalibrationRotation(
		1.2052200746701804,
		-1.2308287877005744,
		1.1957927249313691,
	)
	rawAccel := [3]float64{0.3123, -0.0772, 9.9206}
	corrected := applyMatrix3(mount, rawAccel)

	rawTilt := tiltDegreesFromAccel(rawAccel)
	correctedTilt := tiltDegreesFromAccel(corrected)
	if rawTilt > 3 {
		t.Fatalf("raw tilt = %.2f, want an observed level-ish sample", rawTilt)
	}
	if correctedTilt < 15 {
		t.Fatalf("corrected tilt = %.2f, want large enough to prove this mount is opt-in only", correctedTilt)
	}
}

func tiltDegreesFromAccel(a [3]float64) float64 {
	mag := math.Sqrt(a[0]*a[0] + a[1]*a[1] + a[2]*a[2])
	return math.Acos(a[2]/mag) * 180 / math.Pi
}
