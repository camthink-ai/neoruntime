package utils

import "testing"

func TestParseCPURejectsNonPositiveAndNonFiniteValues(t *testing.T) {
	for _, value := range []string{"0", "0%", "-1", "-10%", "NaN", "+Inf"} {
		t.Run(value, func(t *testing.T) {
			if _, err := ParseCPU(value); err == nil {
				t.Fatalf("ParseCPU(%q) succeeded, want rejection", value)
			}
		})
	}
	for _, value := range []string{"1%", "0.25", "2"} {
		t.Run("valid_"+value, func(t *testing.T) {
			if _, err := ParseCPU(value); err != nil {
				t.Fatalf("ParseCPU(%q) error: %v", value, err)
			}
		})
	}
}

func TestParseMemoryRejectsNonPositiveAndOverflow(t *testing.T) {
	for _, value := range []string{"0", "0Mi", "-1", "-1Gi", "9223372036854775807Gi"} {
		t.Run(value, func(t *testing.T) {
			if _, err := ParseMemory(value); err == nil {
				t.Fatalf("ParseMemory(%q) succeeded, want rejection", value)
			}
		})
	}
	for _, value := range []string{"1", "256Mi", "4G"} {
		t.Run("valid_"+value, func(t *testing.T) {
			if _, err := ParseMemory(value); err != nil {
				t.Fatalf("ParseMemory(%q) error: %v", value, err)
			}
		})
	}
}
