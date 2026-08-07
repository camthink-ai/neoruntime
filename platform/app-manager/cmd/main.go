package main

import (
	"flag"
	"fmt"
	"os"
	"strings"

	"aipc/platform/app-manager/server"
	"aipc/platform/common/config"
	"aipc/platform/common/constants"
	"aipc/platform/common/logger"
)

var (
	configPath = flag.String("config", "/data/aipc/etc/app-manager.yaml", "Path to configuration file")
)

type Config struct {
	Service struct {
		Name     string `yaml:"name"`
		Listen   string `yaml:"listen"`
		LogLevel string `yaml:"log_level"`
		LogFile  string `yaml:"log_file"`
	} `yaml:"service"`

	Containerd struct {
		Address   string `yaml:"address"`
		Namespace string `yaml:"namespace"`
	} `yaml:"containerd"`

	Apps struct {
		RegistryPath  string `yaml:"registry_path"`
		InstancesPath string `yaml:"instances_path"`
		ManifestsPath string `yaml:"manifests_path"`
	} `yaml:"apps"`

	Security struct {
		SeccompProfile   string   `yaml:"seccomp_profile"`
		CapabilitiesDrop []string `yaml:"capabilities_drop"`
	} `yaml:"security"`

	AIRuntime struct {
		Enabled                 bool   `yaml:"enabled"`
		Endpoint                string `yaml:"endpoint"`
		AutoRegisterPermissions bool   `yaml:"auto_register_permissions"`
	} `yaml:"ai_runtime"`

	EventBus struct {
		Enabled       bool     `yaml:"enabled"`
		Endpoint      string   `yaml:"endpoint"`
		PublishEvents []string `yaml:"publish_events"`
	} `yaml:"event_bus"`
}

func (c *Config) Validate() error {
	if c.Service.Listen == "" {
		return fmt.Errorf("service.listen is required")
	}
	if c.Containerd.Address == "" {
		return fmt.Errorf("containerd.address is required")
	}
	return nil
}

func main() {
	flag.Parse()

	// Load configuration
	var cfg Config
	if err := config.LoadYAML(*configPath, &cfg); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to load config: %v\n", err)
		os.Exit(1)
	}

	// Derive root path from registry_path config
	if cfg.Apps.RegistryPath != "" && strings.Contains(cfg.Apps.RegistryPath, "/apps/registry") {
		root := strings.TrimSuffix(cfg.Apps.RegistryPath, "/apps/registry")
		if root != "" && root != constants.RootPath() {
			constants.SetRootPath(root)
			logger.Info("Install prefix: %s", root)
		}
	}

	// Setup logger
	logger.SetLevelFromString(cfg.Service.LogLevel)

	// Configure log file output if specified
	if cfg.Service.LogFile != "" {
		if err := logger.SetOutputFile(cfg.Service.LogFile); err != nil {
			fmt.Fprintf(os.Stderr, "Failed to set log file: %v\n", err)
		} else {
			logger.Info("Logging to file: %s", cfg.Service.LogFile)
		}
	}

	logger.Info("Starting %s", cfg.Service.Name)
	logger.Info("Config file: %s", *configPath)

	// Start gRPC API server (this will handle everything)
	serverConfig := &server.Config{
		Containerd: struct {
			Address   string
			Namespace string
		}{
			Address:   cfg.Containerd.Address,
			Namespace: cfg.Containerd.Namespace,
		},
		Apps: struct {
			RegistryPath  string
			InstancesPath string
			ManifestsPath string
		}{
			RegistryPath:  cfg.Apps.RegistryPath,
			InstancesPath: cfg.Apps.InstancesPath,
			ManifestsPath: cfg.Apps.ManifestsPath,
		},
		Security: struct {
			SeccompProfile string
		}{
			SeccompProfile: cfg.Security.SeccompProfile,
		},
		AIRuntime: struct {
			Enabled                 bool
			Endpoint                string
			AutoRegisterPermissions bool
		}{
			Enabled:                 cfg.AIRuntime.Enabled,
			Endpoint:                cfg.AIRuntime.Endpoint,
			AutoRegisterPermissions: cfg.AIRuntime.AutoRegisterPermissions,
		},
		EventBus: struct {
			Enabled       bool
			Endpoint      string
			PublishEvents []string
		}{
			Enabled:       cfg.EventBus.Enabled,
			Endpoint:      cfg.EventBus.Endpoint,
			PublishEvents: cfg.EventBus.PublishEvents,
		},
	}

	appServer, err := server.NewAppManagerServer(serverConfig)
	if err != nil {
		logger.Fatal("Failed to create app manager server: %v", err)
	}

	// Start gRPC server (blocking)
	if err := appServer.StartGRPCServer(cfg.Service.Listen); err != nil {
		logger.Fatal("Failed to start gRPC server: %v", err)
	}

	// Server will handle shutdown internally
}
