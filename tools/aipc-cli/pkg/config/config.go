/*
Package config handles CLI configuration management.
*/
package config

import (
	"fmt"
	"time"

	"github.com/spf13/viper"
)

// Config represents the CLI configuration
type Config struct {
	GRPC   GRPCConfig   `mapstructure:"grpc"`
	Auth   AuthConfig   `mapstructure:"auth"`
	Output OutputConfig `mapstructure:"output"`
}

// GRPCConfig holds gRPC connection configuration
type GRPCConfig struct {
	AppManager    string        `mapstructure:"app_manager"`
	AIRuntime     string        `mapstructure:"ai_runtime"`
	EventBus      string        `mapstructure:"event_bus"`
	DeviceControl string        `mapstructure:"device_control"`
	Timeout       time.Duration `mapstructure:"timeout"`
}

// AuthConfig holds authentication configuration
type AuthConfig struct {
	Token string `mapstructure:"token"`
}

// OutputConfig holds output formatting configuration
type OutputConfig struct {
	Format string `mapstructure:"format"` // table, json, yaml
	Color  bool   `mapstructure:"color"`
}

// LoadFromViper loads configuration from viper
func LoadFromViper() (*Config, error) {
	cfg := &Config{
		GRPC: GRPCConfig{
			AppManager:    viper.GetString("grpc.app_manager"),
			AIRuntime:     viper.GetString("grpc.ai_runtime"),
			EventBus:      viper.GetString("grpc.event_bus"),
			DeviceControl: viper.GetString("grpc.device_control"),
			Timeout:       viper.GetDuration("grpc.timeout"),
		},
		Auth: AuthConfig{
			Token: viper.GetString("auth.token"),
		},
		Output: OutputConfig{
			Format: viper.GetString("output.format"),
			Color:  viper.GetBool("output.color"),
		},
	}

	// Validate
	if err := cfg.Validate(); err != nil {
		return nil, err
	}

	return cfg, nil
}

// Validate validates the configuration
func (c *Config) Validate() error {
	if c.GRPC.AppManager == "" {
		c.GRPC.AppManager = "unix:///var/run/aipc/app-manager.sock"
	}
	if c.GRPC.AIRuntime == "" {
		c.GRPC.AIRuntime = "unix:///var/run/aipc/ai-runtime.sock"
	}
	if c.GRPC.EventBus == "" {
		c.GRPC.EventBus = "unix:///var/run/aipc/event-bus.sock"
	}
	if c.GRPC.DeviceControl == "" {
		c.GRPC.DeviceControl = "unix:///var/run/aipc/device-control.sock"
	}

	if c.GRPC.Timeout <= 0 {
		c.GRPC.Timeout = 30 * time.Second
	}

	// Validate output format
	switch c.Output.Format {
	case "table", "json", "yaml", "":
		// Valid formats
	default:
		return fmt.Errorf("invalid output format: %s (must be table, json, or yaml)", c.Output.Format)
	}

	if c.Output.Format == "" {
		c.Output.Format = "table"
	}

	return nil
}

// DefaultConfig returns the default configuration
func DefaultConfig() *Config {
	return &Config{
		GRPC: GRPCConfig{
			AppManager:    "unix:///var/run/aipc/app-manager.sock",
			AIRuntime:     "unix:///var/run/aipc/ai-runtime.sock",
			EventBus:      "unix:///var/run/aipc/event-bus.sock",
			DeviceControl: "unix:///var/run/aipc/device-control.sock",
			Timeout:       30 * time.Second,
		},
		Output: OutputConfig{
			Format: "table",
			Color:  true,
		},
	}
}

// SaveDefault saves the default configuration file
func SaveDefault(path string) error {
	viper.Set("grpc.app_manager", "unix:///var/run/aipc/app-manager.sock")
	viper.Set("grpc.ai_runtime", "unix:///var/run/aipc/ai-runtime.sock")
	viper.Set("grpc.event_bus", "unix:///var/run/aipc/event-bus.sock")
	viper.Set("grpc.device_control", "unix:///var/run/aipc/device-control.sock")
	viper.Set("grpc.timeout", "30s")
	viper.Set("auth.token", "")
	viper.Set("output.format", "table")
	viper.Set("output.color", true)

	return viper.WriteConfigAs(path)
}
