package main

import (
	"os"
	"path/filepath"
	"testing"
)

func writeGyroCalibrationFixture(t *testing.T) string {
	t.Helper()
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
	return path
}

func TestResolveGyroCalibrationRequiresExplicitUse(t *testing.T) {
	path := writeGyroCalibrationFixture(t)
	identity := [9]float64{1, 0, 0, 0, 1, 0, 0, 0, 1}

	mount, bias, hasBias, loaded, err := resolveGyroCalibration(GyroConfig{CalibrationPath: path}, identity)
	if err != nil {
		t.Fatalf("resolveGyroCalibration: %v", err)
	}
	if loaded {
		t.Fatal("calibration should not load when no use_calibration_* switch is enabled")
	}
	if mount != identity {
		t.Fatalf("mount changed without opt-in: got %v", mount)
	}
	if hasBias || bias != ([3]float64{}) {
		t.Fatalf("bias changed without opt-in: has=%v bias=%v", hasBias, bias)
	}
}

func TestResolveGyroCalibrationAppliesOnlySelectedFields(t *testing.T) {
	path := writeGyroCalibrationFixture(t)
	identity := [9]float64{1, 0, 0, 0, 1, 0, 0, 0, 1}
	wantBias := [3]float64{-0.00026183332518330324, -0.002437385549437321, -0.0013050431202490205}

	mount, bias, hasBias, loaded, err := resolveGyroCalibration(GyroConfig{
		CalibrationPath:    path,
		UseCalibrationBias: true,
	}, identity)
	if err != nil {
		t.Fatalf("resolveGyroCalibration bias-only: %v", err)
	}
	if !loaded || !hasBias || bias != wantBias {
		t.Fatalf("bias-only load: loaded=%v hasBias=%v bias=%v", loaded, hasBias, bias)
	}
	if mount != identity {
		t.Fatalf("bias-only should preserve mount: got %v", mount)
	}

	mount, bias, hasBias, loaded, err = resolveGyroCalibration(GyroConfig{
		CalibrationPath:     path,
		UseCalibrationMount: true,
	}, identity)
	if err != nil {
		t.Fatalf("resolveGyroCalibration mount-only: %v", err)
	}
	if !loaded || hasBias || bias != ([3]float64{}) {
		t.Fatalf("mount-only bias state: loaded=%v hasBias=%v bias=%v", loaded, hasBias, bias)
	}
	if mount == identity {
		t.Fatal("mount-only should apply the calibration mount matrix")
	}
}
