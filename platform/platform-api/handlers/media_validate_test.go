package handlers

import (
	"testing"
)

func intPtr(v int32) *int32    { return &v }
func uintPtr(v uint32) *uint32 { return &v }
func boolPtr(v bool) *bool     { return &v }

// -- ValidateISPUpdate tests --

func TestValidateISPUpdate_AllFieldsValid(t *testing.T) {
	err := ValidateISPUpdate(
		intPtr(50), intPtr(50), intPtr(50), intPtr(50),
		intPtr(50), intPtr(50), intPtr(50),
		intPtr(10000), intPtr(100),
		intPtr(1),
		intPtr(5),
	)
	if err != nil {
		t.Fatalf("expected nil error, got: %v", err)
	}
}

func TestValidateISPUpdate_NilFieldsSkipped(t *testing.T) {
	// All nil pointers — "no change" semantics, should pass without error
	err := ValidateISPUpdate(
		nil, nil, nil, nil,
		nil, nil, nil,
		nil, nil,
		nil, nil,
	)
	if err != nil {
		t.Fatalf("expected nil error for all-nil fields, got: %v", err)
	}
}

func TestValidateISPUpdate_ZeroValuesValid(t *testing.T) {
	// Zero values are legitimate: brightness=0, noise_reduction=0, etc.
	// Proto optional lets us distinguish "not set" (nil) from "set to 0" (pointer to 0).
	err := ValidateISPUpdate(
		intPtr(0), intPtr(0), intPtr(0), intPtr(0),
		intPtr(0), intPtr(0), intPtr(0),
		nil, nil, // exposure/gain omitted
		intPtr(0), // powerline_freq=0 (off)
		intPtr(0), // awb_index=0
	)
	if err != nil {
		t.Fatalf("expected nil error for zero values, got: %v", err)
	}
}

func TestValidateISPUpdate_RangeFieldsOutOfBounds(t *testing.T) {
	tests := []struct {
		name       string
		brightness *int32
		contrast   *int32
		saturation *int32
		sharpness  *int32
		noiseRed   *int32
		wdrValue   *int32
		backlight  *int32
		wantErr    bool
	}{
		{"brightness below 0", intPtr(-1), nil, nil, nil, nil, nil, nil, true},
		{"brightness above 100", intPtr(101), nil, nil, nil, nil, nil, nil, true},
		{"contrast below 0", nil, intPtr(-1), nil, nil, nil, nil, nil, true},
		{"contrast above 100", nil, intPtr(101), nil, nil, nil, nil, nil, true},
		{"saturation below 0", nil, nil, intPtr(-1), nil, nil, nil, nil, true},
		{"saturation above 100", nil, nil, intPtr(101), nil, nil, nil, nil, true},
		{"sharpness below 0", nil, nil, nil, intPtr(-1), nil, nil, nil, true},
		{"sharpness above 100", nil, nil, nil, intPtr(101), nil, nil, nil, true},
		{"noise_reduction below 0", nil, nil, nil, nil, intPtr(-1), nil, nil, true},
		{"noise_reduction above 100", nil, nil, nil, nil, intPtr(101), nil, nil, true},
		{"wdr_value below 0", nil, nil, nil, nil, nil, intPtr(-1), nil, true},
		{"wdr_value above 100", nil, nil, nil, nil, nil, intPtr(101), nil, true},
		{"backlight below 0", nil, nil, nil, nil, nil, nil, intPtr(-1), true},
		{"backlight above 100", nil, nil, nil, nil, nil, nil, intPtr(101), true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := ValidateISPUpdate(
				tt.brightness, tt.contrast, tt.saturation, tt.sharpness,
				tt.noiseRed, tt.wdrValue, tt.backlight,
				nil, nil, nil, nil,
			)
			if tt.wantErr && err == nil {
				t.Errorf("expected error for %s, got nil", tt.name)
			}
			if !tt.wantErr && err != nil {
				t.Errorf("expected no error for %s, got: %v", tt.name, err)
			}
		})
	}
}

func TestValidateISPUpdate_ExposureTimeUs(t *testing.T) {
	tests := []struct {
		name    string
		val     *int32
		wantErr bool
	}{
		{"nil (no change)", nil, false},
		{"minimum 1", intPtr(1), false},
		{"maximum 1000000", intPtr(1000000), false},
		{"zero — out of range [1,1000000]", intPtr(0), true},
		{"above maximum", intPtr(1000001), true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := ValidateISPUpdate(nil, nil, nil, nil, nil, nil, nil, tt.val, nil, nil, nil)
			if tt.wantErr && err == nil {
				t.Errorf("expected error, got nil")
			}
			if !tt.wantErr && err != nil {
				t.Errorf("expected no error, got: %v", err)
			}
		})
	}
}

func TestValidateISPUpdate_Gain(t *testing.T) {
	tests := []struct {
		name    string
		val     *int32
		wantErr bool
	}{
		{"nil (no change)", nil, false},
		{"minimum 0", intPtr(0), false},
		{"maximum 1000", intPtr(1000), false},
		{"below minimum", intPtr(-1), true},
		{"above maximum", intPtr(1001), true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := ValidateISPUpdate(nil, nil, nil, nil, nil, nil, nil, nil, tt.val, nil, nil)
			if tt.wantErr && err == nil {
				t.Errorf("expected error, got nil")
			}
			if !tt.wantErr && err != nil {
				t.Errorf("expected no error, got: %v", err)
			}
		})
	}
}

func TestValidateISPUpdate_PowerlineFreq(t *testing.T) {
	tests := []struct {
		name    string
		val     *int32
		wantErr bool
	}{
		{"nil (no change)", nil, false},
		{"0 (off)", intPtr(0), false},
		{"1 (50Hz)", intPtr(1), false},
		{"2 (60Hz)", intPtr(2), false},
		{"3 — invalid", intPtr(3), true},
		{"-1 — invalid", intPtr(-1), true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := ValidateISPUpdate(nil, nil, nil, nil, nil, nil, nil, nil, nil, tt.val, nil)
			if tt.wantErr && err == nil {
				t.Errorf("expected error, got nil")
			}
			if !tt.wantErr && err != nil {
				t.Errorf("expected no error, got: %v", err)
			}
		})
	}
}

func TestValidateISPUpdate_AwbIndex(t *testing.T) {
	tests := []struct {
		name    string
		val     *int32
		wantErr bool
	}{
		{"nil (no change)", nil, false},
		{"-1 (auto AWB)", intPtr(-1), false},
		{"0", intPtr(0), false},
		{"16 (max)", intPtr(16), false},
		{"-2 — below range", intPtr(-2), true},
		{"17 — above range", intPtr(17), true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := ValidateISPUpdate(nil, nil, nil, nil, nil, nil, nil, nil, nil, nil, tt.val)
			if tt.wantErr && err == nil {
				t.Errorf("expected error, got nil")
			}
			if !tt.wantErr && err != nil {
				t.Errorf("expected no error, got: %v", err)
			}
		})
	}
}

// -- ValidateTransformUpdate tests --

func TestValidateTransformUpdate_AllNil(t *testing.T) {
	err := ValidateTransformUpdate(nil, nil)
	if err != nil {
		t.Fatalf("expected nil error for nil fields, got: %v", err)
	}
}

func TestValidateTransformUpdate_ValidRotations(t *testing.T) {
	for _, r := range []uint32{0, 1, 2, 3} {
		err := ValidateTransformUpdate(uintPtr(r), nil)
		if err != nil {
			t.Errorf("rotation %d should be valid, got: %v", r, err)
		}
	}
}

func TestValidateTransformUpdate_ValidFlips(t *testing.T) {
	for _, f := range []uint32{0, 1, 2, 3} {
		err := ValidateTransformUpdate(nil, uintPtr(f))
		if err != nil {
			t.Errorf("flip %d should be valid, got: %v", f, err)
		}
	}
}

func TestValidateTransformUpdate_InvalidRotation(t *testing.T) {
	tests := []struct {
		name    string
		val     *uint32
		wantErr bool
	}{
		{"4 — out of range", uintPtr(4), true},
		{"5 — out of range", uintPtr(5), true},
		{"100 — out of range", uintPtr(100), true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := ValidateTransformUpdate(tt.val, nil)
			if tt.wantErr && err == nil {
				t.Errorf("expected error for %s, got nil", tt.name)
			}
		})
	}
}

func TestValidateTransformUpdate_InvalidFlip(t *testing.T) {
	tests := []struct {
		name    string
		val     *uint32
		wantErr bool
	}{
		{"4 — out of range", uintPtr(4), true},
		{"5 — out of range", uintPtr(5), true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := ValidateTransformUpdate(nil, tt.val)
			if tt.wantErr && err == nil {
				t.Errorf("expected error for %s, got nil", tt.name)
			}
		})
	}
}

func TestValidateTransformUpdate_BothInvalid(t *testing.T) {
	err := ValidateTransformUpdate(uintPtr(4), uintPtr(5))
	if err == nil {
		t.Errorf("expected error when both rotation and flip are invalid")
	}
}
