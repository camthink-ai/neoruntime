package unit

import (
	"testing"

	"aipc/platform/common/utils"
)

func TestParseCPU(t *testing.T) {
	tests := []struct {
		input    string
		expected float64
		wantErr  bool
	}{
		{"50%", 0.5, false},
		{"100%", 1.0, false},
		{"25%", 0.25, false},
		{"1.5", 1.5, false},
		{"2", 2.0, false},
		{"0.5", 0.5, false},
		{"invalid", 0, true},
		{"", 0, true},
	}

	for _, tt := range tests {
		result, err := utils.ParseCPU(tt.input)

		if tt.wantErr {
			if err == nil {
				t.Errorf("ParseCPU(%s) expected error, got nil", tt.input)
			}
		} else {
			if err != nil {
				t.Errorf("ParseCPU(%s) unexpected error: %v", tt.input, err)
			}
			if result != tt.expected {
				t.Errorf("ParseCPU(%s) = %v, want %v", tt.input, result, tt.expected)
			}
		}
	}
}

func TestParseMemory(t *testing.T) {
	tests := []struct {
		input    string
		expected int64
		wantErr  bool
	}{
		{"256Mi", 256 * 1024 * 1024, false},
		{"1Gi", 1024 * 1024 * 1024, false},
		{"512Ki", 512 * 1024, false},
		{"100M", 100 * 1000 * 1000, false},
		{"1G", 1000 * 1000 * 1000, false},
		{"1024", 1024, false},
		{"invalid", 0, true},
	}

	for _, tt := range tests {
		result, err := utils.ParseMemory(tt.input)

		if tt.wantErr {
			if err == nil {
				t.Errorf("ParseMemory(%s) expected error, got nil", tt.input)
			}
		} else {
			if err != nil {
				t.Errorf("ParseMemory(%s) unexpected error: %v", tt.input, err)
			}
			if result != tt.expected {
				t.Errorf("ParseMemory(%s) = %v, want %v", tt.input, result, tt.expected)
			}
		}
	}
}

func TestFormatBytes(t *testing.T) {
	tests := []struct {
		input    int64
		expected string
	}{
		{1023, "1023 B"},
		{1024, "1.0 KiB"},
		{1024 * 1024, "1.0 MiB"},
		{256 * 1024 * 1024, "256.0 MiB"},
		{1024 * 1024 * 1024, "1.0 GiB"},
	}

	for _, tt := range tests {
		result := utils.FormatBytes(tt.input)
		if result != tt.expected {
			t.Errorf("FormatBytes(%d) = %s, want %s", tt.input, result, tt.expected)
		}
	}
}
