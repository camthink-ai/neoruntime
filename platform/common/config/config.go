package config

import (
	"fmt"
	"os"

	"gopkg.in/yaml.v3"
)

// Config interface for all configs
type Config interface {
	Validate() error
}

// LoadYAML loads config from YAML file and applies defaults
func LoadYAML(path string, cfg interface{}) error {
	data, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("failed to read config file: %w", err)
	}

	if err := yaml.Unmarshal(data, cfg); err != nil {
		return fmt.Errorf("failed to parse YAML: %w", err)
	}

	// Apply default values
	SetDefaults(cfg)

	// Validate if config implements Config interface
	if validator, ok := cfg.(Config); ok {
		if err := validator.Validate(); err != nil {
			return fmt.Errorf("config validation failed: %w", err)
		}
	}

	return nil
}

// SaveYAML saves config to YAML file
func SaveYAML(path string, cfg interface{}) error {
	data, err := yaml.Marshal(cfg)
	if err != nil {
		return fmt.Errorf("failed to marshal YAML: %w", err)
	}

	if err := os.WriteFile(path, data, 0644); err != nil {
		return fmt.Errorf("failed to write config file: %w", err)
	}

	return nil
}

// GetEnv gets environment variable with default value
func GetEnv(key, defaultValue string) string {
	if value := os.Getenv(key); value != "" {
		return value
	}
	return defaultValue
}
