/*
Package cmd implements the CLI commands using cobra framework.
*/
package cmd

import (
	"bytes"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"github.com/spf13/cobra"
	"github.com/spf13/viper"

	"aipc/tools/aipc-cli/pkg/config"
	"aipc/tools/aipc-cli/pkg/grpcclient"
	"aipc/tools/aipc-cli/pkg/output"
)

const (
	version = "0.3.0"
)

var (
	// Global flags
	cfgFile   string
	outputFmt string
	verbose   bool

	// gRPC endpoint overrides
	appManagerAddr string
	eventBusAddr   string

	// Global instances
	cfg     *config.Config
	grpcCli *grpcclient.Client
	printer *output.Printer
)

// rootCmd represents the base command
var rootCmd = &cobra.Command{
	Use:   "aipc-cli",
	Short: "AIPC Platform Command Line Tool",
	Long: `AIPC CLI is a command line tool for managing the AIPC edge AI platform.

It communicates directly with platform services via gRPC:
  - app-manager: Container application lifecycle
  - ai-runtime:  AI model management and inference
  - event-bus:   Event pub/sub messaging
  - device-control: Hardware peripheral control

Configuration file location: ~/.aipc/config.yaml`,
	Version: version,
	PersistentPreRunE: func(cmd *cobra.Command, args []string) error {
		// Skip initialization for completion and help commands
		if cmd.Name() == "completion" || cmd.Parent().Name() == "completion" {
			return nil
		}
		return initializeGlobals()
	},
	PersistentPostRun: func(cmd *cobra.Command, args []string) {
		if grpcCli != nil {
			grpcCli.Close()
		}
	},
}

// Execute runs the root command
func Execute() {
	if err := rootCmd.Execute(); err != nil {
		os.Exit(1)
	}
}

func init() {
	cobra.OnInitialize(initConfig, ensureCompletion)

	// Global flags
	rootCmd.PersistentFlags().StringVar(&cfgFile, "config", "", "config file (default is ~/.aipc/config.yaml)")
	rootCmd.PersistentFlags().StringVarP(&outputFmt, "output", "o", "table", "Output format: table, json, yaml")
	rootCmd.PersistentFlags().BoolVarP(&verbose, "verbose", "v", false, "Enable verbose output")

	// gRPC endpoint flags
	rootCmd.PersistentFlags().StringVar(&appManagerAddr, "app-manager", "", "app-manager gRPC address (e.g., unix:///var/run/aipc/app-manager.sock or localhost:50051)")
	rootCmd.PersistentFlags().StringVar(&eventBusAddr, "event-bus", "", "event-bus gRPC address (e.g., unix:///var/run/aipc/event-bus.sock or localhost:50052)")

	// Bind flags to viper
	viper.BindPFlag("output.format", rootCmd.PersistentFlags().Lookup("output"))
	viper.BindPFlag("verbose", rootCmd.PersistentFlags().Lookup("verbose"))
	viper.BindPFlag("grpc.app_manager", rootCmd.PersistentFlags().Lookup("app-manager"))
	viper.BindPFlag("grpc.event_bus", rootCmd.PersistentFlags().Lookup("event-bus"))

	// Add subcommands
	rootCmd.AddCommand(appCmd)
	rootCmd.AddCommand(modelCmd)
	rootCmd.AddCommand(deviceCmd)
	rootCmd.AddCommand(streamCmd)
	rootCmd.AddCommand(eventCmd)
	rootCmd.AddCommand(systemCmd)
	rootCmd.AddCommand(monitorCmd)
	rootCmd.AddCommand(mediaCmd)
	rootCmd.AddCommand(filesCmd)
	rootCmd.AddCommand(processCmd)
	rootCmd.AddCommand(logsCmd)
	rootCmd.AddCommand(eventLogCmd)
	rootCmd.AddCommand(completionCmd)
}

// initConfig reads in config file and ENV variables
func initConfig() {
	if cfgFile != "" {
		viper.SetConfigFile(cfgFile)
	} else {
		home, err := os.UserHomeDir()
		if err != nil {
			fmt.Fprintln(os.Stderr, "Warning: Cannot find home directory:", err)
			return
		}

		configDir := filepath.Join(home, ".aipc")
		configPath := filepath.Join(configDir, "config.yaml")

		if err := os.MkdirAll(configDir, 0755); err != nil {
			fmt.Fprintln(os.Stderr, "Warning: Cannot create config directory:", err)
		}

		viper.SetConfigFile(configPath)
	}

	// Environment variables
	viper.SetEnvPrefix("AIPC")
	viper.AutomaticEnv()

	// Set defaults
	viper.SetDefault("grpc.app_manager", "unix:///var/run/aipc/app-manager.sock")
	viper.SetDefault("grpc.ai_runtime", "unix:///var/run/aipc/ai-runtime.sock")
	viper.SetDefault("grpc.event_bus", "unix:///var/run/aipc/event-bus.sock")
	viper.SetDefault("grpc.device_control", "unix:///var/run/aipc/device-control.sock")
	viper.SetDefault("grpc.timeout", "30s")
	viper.SetDefault("output.format", "table")
	viper.SetDefault("output.color", true)

	// Read config file (ignore if not found)
	if err := viper.ReadInConfig(); err == nil {
		if verbose {
			fmt.Fprintln(os.Stderr, "Using config file:", viper.ConfigFileUsed())
		}
	}
}

// initializeGlobals initializes global instances
func initializeGlobals() error {
	var err error

	// Load configuration
	cfg, err = config.LoadFromViper()
	if err != nil {
		return fmt.Errorf("failed to load config: %w", err)
	}

	// Override with command line flags
	if outputFmt != "" {
		cfg.Output.Format = outputFmt
	}
	if appManagerAddr != "" {
		cfg.GRPC.AppManager = appManagerAddr
	}
	if eventBusAddr != "" {
		cfg.GRPC.EventBus = eventBusAddr
	}

	// Create gRPC client
	grpcCli, err = grpcclient.NewClient(&grpcclient.Config{
		AppManagerAddr:    cfg.GRPC.AppManager,
		AIRuntimeAddr:     cfg.GRPC.AIRuntime,
		EventBusAddr:      cfg.GRPC.EventBus,
		DeviceControlAddr: cfg.GRPC.DeviceControl,
		Timeout:           cfg.GRPC.Timeout,
	})
	if err != nil {
		return fmt.Errorf("failed to create gRPC client: %w", err)
	}

	// Create output printer
	printer = output.NewPrinter(cfg.Output.Format, cfg.Output.Color)

	return nil
}

// completionCmd generates shell completion scripts
var completionCmd = &cobra.Command{
	Use:   "completion [bash|zsh|fish|powershell]",
	Short: "Generate shell completion scripts",
	Long: `Generate shell completion scripts for aipc-cli.

To load completions:

Bash:
  $ source <(aipc-cli completion bash)

Zsh:
  $ source <(aipc-cli completion zsh)

Fish:
  $ aipc-cli completion fish | source
`,
	DisableFlagsInUseLine: true,
	ValidArgs:             []string{"bash", "zsh", "fish", "powershell"},
	Args:                  cobra.ExactValidArgs(1),
	Run: func(cmd *cobra.Command, args []string) {
		switch args[0] {
		case "bash":
			rootCmd.GenBashCompletion(os.Stdout)
		case "zsh":
			rootCmd.GenZshCompletion(os.Stdout)
		case "fish":
			rootCmd.GenFishCompletion(os.Stdout, true)
		case "powershell":
			rootCmd.GenPowerShellCompletionWithDesc(os.Stdout)
		}
	},
}

// ensureCompletion automatically installs shell completion on first run
func ensureCompletion() {
	home, err := os.UserHomeDir()
	if err != nil {
		return
	}

	shell := os.Getenv("SHELL")
	if shell == "" {
		return
	}

	// Resolve symlinks to get the real shell
	realShell, err := filepath.EvalSymlinks(shell)
	if err != nil {
		realShell = shell
	}
	shellName := filepath.Base(realShell)

	// Marker file to track if completion was installed
	markerFile := filepath.Join(home, ".aipc", ".completion_installed")
	if _, err := os.Stat(markerFile); err == nil {
		return // Already installed
	}

	var rcFile string
	var completionLine string

	switch shellName {
	case "zsh":
		rcFile = filepath.Join(home, ".zshrc")
		completionLine = `source <(aipc-cli completion zsh)`
	case "bash":
		// Try .bashrc first, then .bash_profile
		rcFile = filepath.Join(home, ".bashrc")
		if _, err := os.Stat(rcFile); os.IsNotExist(err) {
			rcFile = filepath.Join(home, ".bash_profile")
		}
		completionLine = `source <(aipc-cli completion bash)`
	case "fish":
		rcFile = filepath.Join(home, ".config", "fish", "config.fish")
		completionLine = `aipc-cli completion fish | source`
	case "dash", "sh", "ash", "busybox":
		// These shells don't support programmable completion
		// Create marker to avoid repeated checks
		os.MkdirAll(filepath.Dir(markerFile), 0755)
		os.WriteFile(markerFile, []byte("unsupported"), 0644)
		return
	default:
		// Check if shell name contains known shell names (e.g., /usr/local/bin/zsh)
		switch {
		case strings.Contains(shell, "zsh"):
			rcFile = filepath.Join(home, ".zshrc")
			completionLine = `source <(aipc-cli completion zsh)`
		case strings.Contains(shell, "bash"):
			rcFile = filepath.Join(home, ".bashrc")
			completionLine = `source <(aipc-cli completion bash)`
		case strings.Contains(shell, "fish"):
			rcFile = filepath.Join(home, ".config", "fish", "config.fish")
			completionLine = `aipc-cli completion fish | source`
		default:
			return // Unsupported shell
		}
	}

	// Check if already configured
	content, err := os.ReadFile(rcFile)
	if err == nil && strings.Contains(string(content), "aipc-cli completion") {
		// Already configured, just create marker
		os.MkdirAll(filepath.Dir(markerFile), 0755)
		os.WriteFile(markerFile, []byte("installed"), 0644)
		return
	}

	// Append completion to rc file
	f, err := os.OpenFile(rcFile, os.O_APPEND|os.O_CREATE|os.O_WRONLY, 0644)
	if err != nil {
		return
	}
	defer f.Close()

	// Add completion line
	completionBlock := fmt.Sprintf("\n# AIPC CLI auto-completion\n%s\n", completionLine)
	if _, err := f.WriteString(completionBlock); err != nil {
		return
	}

	// Create marker file
	os.MkdirAll(filepath.Dir(markerFile), 0755)
	os.WriteFile(markerFile, []byte("installed"), 0644)

	// Notify user (only on stderr to not interfere with output)
	fmt.Fprintln(os.Stderr, "✓ Shell completion installed. Restart your shell or run: source", rcFile)
}

// getCompletionScript returns the completion script for the detected shell
func getCompletionScript(shell string) string {
	var buf bytes.Buffer
	switch {
	case strings.Contains(shell, "zsh"):
		rootCmd.GenZshCompletion(&buf)
	case strings.Contains(shell, "bash"):
		rootCmd.GenBashCompletion(&buf)
	case strings.Contains(shell, "fish"):
		rootCmd.GenFishCompletion(&buf, true)
	}
	return buf.String()
}
