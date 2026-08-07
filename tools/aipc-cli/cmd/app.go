package cmd

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"os/signal"
	"syscall"
	"time"

	"github.com/spf13/cobra"
	"golang.org/x/term"
	"google.golang.org/protobuf/types/known/emptypb"

	apppb "aipc/platform/app-manager/proto"
	"aipc/tools/aipc-cli/pkg/output"
)

var appCmd = &cobra.Command{
	Use:   "app",
	Short: "Manage container applications",
	Long:  `Manage container applications: install, start, stop, remove, logs, stats, exec.`,
}

// connectAppManager ensures we have a connection to app-manager
func connectAppManager() error {
	return grpcCli.ConnectAppManager()
}

// ============ app list ============

var appListCmd = &cobra.Command{
	Use:   "list",
	Short: "List all installed applications",
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAppManager(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		result, err := grpcCli.AppManager.ListApps(ctx, &emptypb.Empty{})
		if err != nil {
			return fmt.Errorf("failed to list apps: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(protoAppListToMap(result))
		}

		if len(result.Apps) == 0 {
			printer.Info("No applications installed")
			return nil
		}

		table := output.NewTable("ID", "NAME", "VERSION", "STATE", "UPTIME")
		for _, app := range result.Apps {
			uptime := "-"
			if app.StartedAt > 0 && app.State == "running" {
				uptime = output.FormatDuration(time.Now().Unix() - app.StartedAt)
			}
			table.AddRow(
				app.Id,
				app.Name,
				app.Version,
				printer.FormatStatus(app.State),
				uptime,
			)
		}
		table.RenderTo(printer)
		return nil
	},
}

// ============ app info ============

var appInfoCmd = &cobra.Command{
	Use:   "info <app-id>",
	Short: "Show application details",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAppManager(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		app, err := grpcCli.AppManager.GetApp(ctx, &apppb.GetAppRequest{AppId: args[0]})
		if err != nil {
			return fmt.Errorf("failed to get app info: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(protoAppInfoToMap(app))
		}

		printer.Printf("Application: %s\n", app.Id)
		printer.Printf("  Name:         %s\n", app.Name)
		printer.Printf("  Version:      %s\n", app.Version)
		printer.Printf("  State:        %s\n", printer.FormatStatus(app.State))
		printer.Printf("  Container ID: %s\n", app.ContainerId)
		printer.Printf("  PID:          %d\n", app.Pid)
		printer.Printf("  Installed:    %s\n", output.FormatTimestamp(app.InstalledAt))
		printer.Printf("  Started:      %s\n", output.FormatTimestamp(app.StartedAt))
		printer.Printf("  Restarts:     %d\n", app.RestartCount)
		return nil
	},
}

// ============ app install ============

var appInstallCmd = &cobra.Command{
	Use:   "install <manifest> <image>",
	Short: "Install an application",
	Long: `Install an application from a manifest file and OCI container image.

The image must be a standard OCI image tarball (created by 'docker save').

Examples:
  aipc-cli app install app.yaml app-image.tar
`,
	Args: cobra.ExactArgs(2),
	RunE: func(cmd *cobra.Command, args []string) error {
		manifestPath := args[0]
		imagePath := args[1]

		// Check files exist
		if _, err := os.Stat(manifestPath); os.IsNotExist(err) {
			return fmt.Errorf("manifest file not found: %s", manifestPath)
		}
			// Only check file existence for local image paths
			isLocalImage := strings.HasPrefix(imagePath, "/") || strings.HasPrefix(imagePath, "./") ||
			strings.HasSuffix(imagePath, ".tar") || strings.HasSuffix(imagePath, ".tar.gz") || strings.HasSuffix(imagePath, ".tgz")
			if isLocalImage {
			if _, err := os.Stat(imagePath); os.IsNotExist(err) {
				return fmt.Errorf("image file not found: %s", imagePath)
			}
			abspath, _ := filepath.Abs(imagePath)
			imagePath = abspath
			}

		// Convert to absolute paths for gRPC server
		absManifest, _ := filepath.Abs(manifestPath)
		// absImage handled above

		if err := connectAppManager(); err != nil {
			return err
		}

		printer.Info("Installing application...")

		ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
		defer cancel()

		result, err := grpcCli.AppManager.InstallApp(ctx, &apppb.InstallRequest{
			ManifestPath: absManifest,
			ImagePath:    imagePath,
		})
		if err != nil {
			return fmt.Errorf("install failed: %w", err)
		}

		if !result.Status.Success {
			printer.Error("Install failed: %s", result.Status.Message)
			return fmt.Errorf("install failed")
		}

		printer.Success("Application installed: %s", result.AppId)
		return nil
	},
}

// ============ app start ============

var appStartCmd = &cobra.Command{
	Use:   "start <app-id>",
	Short: "Start an application",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAppManager(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()

		result, err := grpcCli.AppManager.StartApp(ctx, &apppb.StartRequest{AppId: args[0]})
		if err != nil {
			return fmt.Errorf("failed to start app: %w", err)
		}

		if !result.Success {
			printer.Error("Start failed: %s", result.Message)
			return fmt.Errorf("start failed")
		}

		printer.Success("Application started: %s", args[0])
		return nil
	},
}

// ============ app stop ============

var appStopCmd = &cobra.Command{
	Use:   "stop <app-id>",
	Short: "Stop a running application",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAppManager(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()

		result, err := grpcCli.AppManager.StopApp(ctx, &apppb.StopRequest{
			AppId:          args[0],
			TimeoutSeconds: 10,
		})
		if err != nil {
			return fmt.Errorf("failed to stop app: %w", err)
		}

		if !result.Success {
			printer.Error("Stop failed: %s", result.Message)
			return fmt.Errorf("stop failed")
		}

		printer.Success("Application stopped: %s", args[0])
		return nil
	},
}

// ============ app restart ============

var appRestartCmd = &cobra.Command{
	Use:   "restart <app-id>",
	Short: "Restart an application",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAppManager(); err != nil {
			return err
		}

		appID := args[0]

		printer.Info("Stopping %s...", appID)
		ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()

		// Stop
		if _, err := grpcCli.AppManager.StopApp(ctx, &apppb.StopRequest{
			AppId:          appID,
			TimeoutSeconds: 10,
		}); err != nil {
			printer.Warning("Stop failed (may already be stopped): %v", err)
		}

		time.Sleep(1 * time.Second)

		// Start
		printer.Info("Starting %s...", appID)
		result, err := grpcCli.AppManager.StartApp(ctx, &apppb.StartRequest{AppId: appID})
		if err != nil {
			return fmt.Errorf("failed to start app: %w", err)
		}

		if !result.Success {
			printer.Error("Start failed: %s", result.Message)
			return fmt.Errorf("restart failed")
		}

		printer.Success("Application restarted: %s", appID)
		return nil
	},
}

// ============ app remove ============

var appRemoveCmd = &cobra.Command{
	Use:     "remove <app-id>",
	Aliases: []string{"uninstall", "rm"},
	Short:   "Remove an application",
	Args:    cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAppManager(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()

		result, err := grpcCli.AppManager.UninstallApp(ctx, &apppb.UninstallRequest{
			AppId: args[0],
		})
		if err != nil {
			return fmt.Errorf("failed to remove app: %w", err)
		}

		if !result.Success {
			printer.Error("Remove failed: %s", result.Message)
			return fmt.Errorf("remove failed")
		}

		printer.Success("Application removed: %s", args[0])
		return nil
	},
}
	// ============ app update ============

	var appUpdateCmd = &cobra.Command{
		Use:   "update <app-id> <manifest> <image>",
		Short: "Update an installed application",
		Long: `Update an installed application with a new manifest and container image.
Preserves volume data, web URL, and restart count. If the app was running,
it will be automatically restarted after the update.

Examples:
  aipc-cli app update my-app app.yaml new-image.tar
`,
		Args: cobra.ExactArgs(3),
		RunE: func(cmd *cobra.Command, args []string) error {
			appID := args[0]
			manifestPath := args[1]
			imagePath := args[2]

			if _, err := os.Stat(manifestPath); os.IsNotExist(err) {
				return fmt.Errorf("manifest file not found: %s", manifestPath)
			}
			// Only check file existence for local image paths
			isLocalImage := strings.HasPrefix(imagePath, "/") || strings.HasPrefix(imagePath, "./") ||
				strings.HasSuffix(imagePath, ".tar") || strings.HasSuffix(imagePath, ".tar.gz") || strings.HasSuffix(imagePath, ".tgz")
			if isLocalImage {
				if _, err := os.Stat(imagePath); os.IsNotExist(err) {
					return fmt.Errorf("image file not found: %s", imagePath)
				}
				abspath, _ := filepath.Abs(imagePath)
				imagePath = abspath
			}

			absManifest, _ := filepath.Abs(manifestPath)
			// absImage handled above

			if err := connectAppManager(); err != nil {
				return err
			}

			printer.Info("Updating application %s...", appID)

			ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
			defer cancel()

			result, err := grpcCli.AppManager.InstallApp(ctx, &apppb.InstallRequest{
				ManifestPath: absManifest,
				ImagePath:    imagePath,
				Force:        true,
			})
			if err != nil {
				return fmt.Errorf("update failed: %w", err)
			}

			if !result.Status.Success {
				printer.Error("Update failed: %s", result.Status.Message)
				return fmt.Errorf("update failed")
			}

			if result.Updated {
				printer.Success("Application updated: %s", result.AppId)
			} else {
				printer.Success("Application installed: %s", result.AppId)
			}
			return nil
		},
	}

	// ============ app dev ============

	var appDevCmd = &cobra.Command{
		Use:   "dev <app-id>",
		Short: "Run app in dev mode with hot reload",
		Long: `Start an app in development mode. Source directories from the
host are bind-mounted, readonly rootfs is disabled, and
file changes trigger automatic reload.

Examples:
  aipc-cli app dev my-app
`,
		Args: cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			appID := args[0]

			if err := connectAppManager(); err != nil {
				return err
			}

			ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			defer cancel()

			printer.Info("Starting app %s in dev mode...", appID)

			result, err := grpcCli.AppManager.StartApp(ctx, &apppb.StartRequest{
				AppId: appID,
			})
			if err != nil {
				return fmt.Errorf("failed to start: %w", err)
			}

			if !result.Success {
				printer.Error("Start failed: %s", result.Message)
				return fmt.Errorf("start failed")
			}

			printer.Success("App %s running in dev mode", appID)
			printer.Info("Editing source files will trigger reload")
			printer.Info("Press Ctrl+C to stop")

			sigCh := make(chan os.Signal, 1)
			signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
			<-sigCh

			printer.Info("Stopping dev mode...")
			stopCtx, stopCancel := context.WithTimeout(context.Background(), 10*time.Second)
			defer stopCancel()

			if _, err := grpcCli.AppManager.StopApp(stopCtx, &apppb.StopRequest{
				AppId:          appID,
				TimeoutSeconds: 10,
			}); err != nil {
				printer.Warning("Stop error: %v", err)
			} else {
				printer.Success("App stopped")
			}
			return nil
		},
	}
// ============ app stats ============

var appStatsCmd = &cobra.Command{
	Use:   "stats <app-id>",
	Short: "Show application resource usage",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAppManager(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		stats, err := grpcCli.AppManager.GetAppStats(ctx, &apppb.GetAppRequest{AppId: args[0]})
		if err != nil {
			return fmt.Errorf("failed to get stats: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(protoAppStatsToMap(stats))
		}

		printer.Printf("Application Stats: %s\n", args[0])
		printer.Printf("  CPU Usage:    %s\n", output.FormatPercent(stats.CpuUsagePercent))
		printer.Printf("  Memory:       %s / %s\n",
			output.FormatBytes(stats.MemoryUsageBytes),
			output.FormatBytes(stats.MemoryLimitBytes))
		printer.Printf("  Threads:      %d\n", stats.ThreadCount)
		printer.Printf("  Uptime:       %s\n", output.FormatDuration(stats.UptimeSeconds))
		return nil
	},
}

// ============ app logs ============

var (
	logFollow bool
	logTail   int
)

var appLogsCmd = &cobra.Command{
	Use:   "logs <app-id>",
	Short: "View application logs",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAppManager(); err != nil {
			return err
		}

		ctx := context.Background()
		if !logFollow {
			var cancel context.CancelFunc
			ctx, cancel = context.WithTimeout(ctx, cfg.GRPC.Timeout)
			defer cancel()
		}

		stream, err := grpcCli.AppManager.GetAppLogs(ctx, &apppb.GetLogsRequest{
			AppId:    args[0],
			MaxLines: int32(logTail),
			Follow:   logFollow,
		})
		if err != nil {
			return fmt.Errorf("failed to get logs: %w", err)
		}

		for {
			logLine, err := stream.Recv()
			if err == io.EOF {
				break
			}
			if err != nil {
				return nil // Stream ended
			}

			if logLine.Level != "" {
				fmt.Printf("[%s] %s\n", logLine.Level, logLine.Message)
			} else {
				fmt.Println(logLine.Message)
			}
		}

		return nil
	},
}

// ============ Proto to map conversion helpers ============

func protoAppListToMap(list *apppb.AppList) map[string]interface{} {
	apps := make([]map[string]interface{}, len(list.Apps))
	for i, app := range list.Apps {
		apps[i] = protoAppInfoToMap(app)
	}
	return map[string]interface{}{"apps": apps}
}

func protoAppInfoToMap(app *apppb.AppInfo) map[string]interface{} {
	return map[string]interface{}{
		"id":            app.Id,
		"name":          app.Name,
		"version":       app.Version,
		"state":         app.State,
		"container_id":  app.ContainerId,
		"pid":           app.Pid,
		"installed_at":  app.InstalledAt,
		"started_at":    app.StartedAt,
		"stopped_at":    app.StoppedAt,
		"restart_count": app.RestartCount,
	}
}

func protoAppStatsToMap(stats *apppb.AppStats) map[string]interface{} {
	return map[string]interface{}{
		"app_id":             stats.AppId,
		"cpu_usage_percent":  stats.CpuUsagePercent,
		"memory_usage_bytes": stats.MemoryUsageBytes,
		"memory_limit_bytes": stats.MemoryLimitBytes,
		"thread_count":       stats.ThreadCount,
		"uptime_seconds":     stats.UptimeSeconds,
	}
}

// ============ app exec ============

var (
	execUser    string
	execWorkdir string
)

var appExecCmd = &cobra.Command{
	Use:   "exec <app-id> -- [command] [args...]",
	Short: "Execute a command in a running container",
	Long: `Execute a command inside a running application container.

Use '--' to separate CLI flags from the command to execute.

Examples:
  # Start an interactive shell
  aipc-cli app exec hello_world -- /bin/sh

  # Run a command
  aipc-cli app exec hello_world -- ls -la /app

  # Run with specific user
  aipc-cli app exec hello_world --user root -- /bin/sh

  # Check Python version
  aipc-cli app exec hello_world -- python3 --version
`,
	Args:               cobra.MinimumNArgs(1),
	DisableFlagParsing: false,
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectAppManager(); err != nil {
			return err
		}

		appID := args[0]

		// Get app info to verify it's running
		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		app, err := grpcCli.AppManager.GetApp(ctx, &apppb.GetAppRequest{AppId: appID})
		if err != nil {
			return fmt.Errorf("failed to get app info: %w", err)
		}

		if app.State != "running" {
			return fmt.Errorf("app %s is not running (state: %s)", appID, app.State)
		}

		if app.ContainerId == "" {
			return fmt.Errorf("app %s has no container ID", appID)
		}

		// Default command is /bin/sh
		execCmd := []string{"/bin/sh"}
		if len(args) > 1 {
			execCmd = args[1:]
		}

		// Build ctr command
		// ctr -n aipc tasks exec [--tty] --exec-id <id> <container> <cmd>
		execID := fmt.Sprintf("exec-%d", time.Now().UnixNano())
		ctrArgs := []string{
			"-n", "aipc",
			"tasks", "exec",
			"--exec-id", execID,
		}

		// Only use --tty if stdin is a terminal
		isTerminal := term.IsTerminal(int(os.Stdin.Fd()))
		if isTerminal {
			ctrArgs = append(ctrArgs, "--tty")
		}

		if execUser != "" {
			ctrArgs = append(ctrArgs, "--user", execUser)
		}
		if execWorkdir != "" {
			ctrArgs = append(ctrArgs, "--cwd", execWorkdir)
		}

		ctrArgs = append(ctrArgs, app.ContainerId)
		ctrArgs = append(ctrArgs, execCmd...)

		// Execute ctr command with interactive terminal
		ctrCmd := exec.Command("ctr", ctrArgs...)
		ctrCmd.Stdin = os.Stdin
		ctrCmd.Stdout = os.Stdout
		ctrCmd.Stderr = os.Stderr

		printer.Printf("Connecting to container %s...\n", app.ContainerId)

		if err := ctrCmd.Run(); err != nil {
			// Check if it's just an exit code from the shell
			if exitErr, ok := err.(*exec.ExitError); ok {
				os.Exit(exitErr.ExitCode())
			}
			return fmt.Errorf("exec failed: %w", err)
		}

		return nil
	},
}

func init() {
	// app logs flags
	appLogsCmd.Flags().BoolVarP(&logFollow, "follow", "f", false, "Follow log output")
	appLogsCmd.Flags().IntVar(&logTail, "tail", 100, "Number of lines to show")

	// app exec flags
	appExecCmd.Flags().StringVarP(&execUser, "user", "u", "", "User to run command as (e.g., root)")
	appExecCmd.Flags().StringVarP(&execWorkdir, "workdir", "w", "", "Working directory inside container")

	// Register subcommands
	appCmd.AddCommand(appListCmd)
	appCmd.AddCommand(appInfoCmd)
	appCmd.AddCommand(appInstallCmd)
	appCmd.AddCommand(appStartCmd)
	appCmd.AddCommand(appStopCmd)
	appCmd.AddCommand(appRestartCmd)
	appCmd.AddCommand(appRemoveCmd)
	appCmd.AddCommand(appUpdateCmd)
	appCmd.AddCommand(appDevCmd)
	appCmd.AddCommand(appDevCmd)
	appCmd.AddCommand(appStatsCmd)
	appCmd.AddCommand(appLogsCmd)
	appCmd.AddCommand(appExecCmd)
}
