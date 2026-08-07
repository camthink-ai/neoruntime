package utils

import (
	"fmt"
	"strconv"
	"strings"
	"time"
)

// ParseCPU parses CPU string like "50%" or "1.5" to float64
func ParseCPU(cpu string) (float64, error) {
	cpu = strings.TrimSpace(cpu)

	if strings.HasSuffix(cpu, "%") {
		// Parse percentage
		percentStr := strings.TrimSuffix(cpu, "%")
		percent, err := strconv.ParseFloat(percentStr, 64)
		if err != nil {
			return 0, fmt.Errorf("invalid CPU percentage: %s", cpu)
		}
		return percent / 100.0, nil
	}

	// Parse as core count
	cores, err := strconv.ParseFloat(cpu, 64)
	if err != nil {
		return 0, fmt.Errorf("invalid CPU value: %s", cpu)
	}

	return cores, nil
}

// ParseMemory parses memory string like "256Mi" or "1Gi" to bytes
func ParseMemory(memory string) (int64, error) {
	memory = strings.TrimSpace(memory)

	units := map[string]int64{
		"Ki": 1024,
		"Mi": 1024 * 1024,
		"Gi": 1024 * 1024 * 1024,
		"K":  1000,
		"M":  1000 * 1000,
		"G":  1000 * 1000 * 1000,
	}

	for suffix, multiplier := range units {
		if strings.HasSuffix(memory, suffix) {
			valueStr := strings.TrimSuffix(memory, suffix)
			value, err := strconv.ParseInt(valueStr, 10, 64)
			if err != nil {
				return 0, fmt.Errorf("invalid memory value: %s", memory)
			}
			return value * multiplier, nil
		}
	}

	// Parse as plain bytes
	bytes, err := strconv.ParseInt(memory, 10, 64)
	if err != nil {
		return 0, fmt.Errorf("invalid memory value: %s", memory)
	}

	return bytes, nil
}

// FormatBytes formats bytes to human readable string
func FormatBytes(bytes int64) string {
	const unit = 1024
	if bytes < unit {
		return fmt.Sprintf("%d B", bytes)
	}

	div, exp := int64(unit), 0
	for n := bytes / unit; n >= unit; n /= unit {
		div *= unit
		exp++
	}

	units := []string{"Ki", "Mi", "Gi", "Ti"}
	return fmt.Sprintf("%.1f %sB", float64(bytes)/float64(div), units[exp])
}

// ParseDuration parses duration string like "30s" or "5m"
func ParseDuration(duration string) (time.Duration, error) {
	// This is a simple wrapper, but could be enhanced
	return time.ParseDuration(duration)
}
