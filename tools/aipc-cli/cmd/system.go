package cmd

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os/exec"
	"strings"
	"time"

	"github.com/spf13/cobra"
	"google.golang.org/protobuf/types/known/emptypb"

	inferencepb "aipc/platform/ai-runtime/proto"
	devicepb "aipc/platform/device-control/proto"
	eventpb "aipc/platform/event-bus/proto"
	"aipc/tools/aipc-cli/pkg/output"
)

var systemCmd = &cobra.Command{
	Use:   "system",
	Short: "System management",
	Long:  `System management: info, stats, health check.`,
}

var (
	systemAPIBase string
)

// ============ system info ============

var systemInfoCmd = &cobra.Command{
	Use:   "info",
	Short: "Show system information",
	RunE: func(cmd *cobra.Command, args []string) error {
		// Try to get info from platform-api
		url := systemAPIBase + "/api/v1/system/info"
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
		if err == nil {
			resp, err := apiHTTPClient.Do(req)
			if err == nil && resp.StatusCode == http.StatusOK {
				defer resp.Body.Close()
				var info map[string]interface{}
				if json.NewDecoder(resp.Body).Decode(&info) == nil {
					if outputFmt == "json" || outputFmt == "yaml" {
						return printer.Print(info)
					}
					printer.Println("AIPC Platform System Information")
					printer.Println("─────────────────────────────────")
					if v, ok := info["version"]; ok {
						printer.Printf("Version: %v\n", v)
					}
					if services, ok := info["services"].(map[string]interface{}); ok {
						printer.Println("\nServices:")
						for name, status := range services {
							statusStr := "unavailable"
							if s, ok := status.(bool); ok && s {
								statusStr = "available"
							}
							printer.Printf("  %-18s %s\n", name+":", printer.FormatStatus(statusStr))
						}
					}
					return nil
				}
			}
		}

		// Fallback to local config
		printer.Println("AIPC Platform System Information")
		printer.Println("─────────────────────────────────")
		printer.Printf("CLI Version: %s\n", version)
		printer.Println()
		printer.Println("gRPC Endpoints:")
		printer.Printf("  app-manager:    %s\n", cfg.GRPC.AppManager)
		printer.Printf("  ai-runtime:     %s\n", cfg.GRPC.AIRuntime)
		printer.Printf("  event-bus:      %s\n", cfg.GRPC.EventBus)
		printer.Printf("  device-control: %s\n", cfg.GRPC.DeviceControl)
		return nil
	},
}

// ============ system stats ============

var systemStatsCmd = &cobra.Command{
	Use:   "stats",
	Short: "Show system statistics",
	RunE: func(cmd *cobra.Command, args []string) error {
		url := systemAPIBase + "/api/v1/system/stats"
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}

		resp, err := apiHTTPClient.Do(req)
		if err != nil {
			return fmt.Errorf("failed to get stats: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		var stats map[string]interface{}
		if err := json.NewDecoder(resp.Body).Decode(&stats); err != nil {
			return fmt.Errorf("failed to decode response: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(stats)
		}

		printer.Println("System Statistics")
		printer.Println("─────────────────")
		if ts, ok := stats["timestamp"]; ok {
			printer.Printf("Timestamp: %v\n", ts)
		}
		if services, ok := stats["services"].(map[string]interface{}); ok {
			for name, data := range services {
				printer.Printf("\n%s:\n", name)
				if m, ok := data.(map[string]interface{}); ok {
					for k, v := range m {
						printer.Printf("  %s: %v\n", k, v)
					}
				}
			}
		}
		return nil
	},
}

// ============ system health ============

var systemHealthCmd = &cobra.Command{
	Use:   "health",
	Short: "Check system health",
	RunE: func(cmd *cobra.Command, args []string) error {
		results := make(map[string]string)
		allHealthy := true

		// Check app-manager
		if err := grpcCli.ConnectAppManager(); err != nil {
			results["app-manager"] = "unreachable"
			allHealthy = false
		} else {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			_, err := grpcCli.AppManager.ListApps(ctx, &emptypb.Empty{})
			cancel()
			if err != nil {
				results["app-manager"] = "error"
				allHealthy = false
			} else {
				results["app-manager"] = "healthy"
			}
		}

		// Check ai-runtime
		if err := grpcCli.ConnectAIRuntime(); err != nil {
			results["ai-runtime"] = "unreachable"
			allHealthy = false
		} else {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			_, err := grpcCli.AIRuntime.ListModels(ctx, &inferencepb.Empty{})
			cancel()
			if err != nil {
				results["ai-runtime"] = "error"
				allHealthy = false
			} else {
				results["ai-runtime"] = "healthy"
			}
		}

		// Check event-bus
		if err := grpcCli.ConnectEventBus(); err != nil {
			results["event-bus"] = "unreachable"
			allHealthy = false
		} else {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			_, err := grpcCli.EventBus.ListTopics(ctx, &eventpb.Empty{})
			cancel()
			if err != nil {
				results["event-bus"] = "error"
				allHealthy = false
			} else {
				results["event-bus"] = "healthy"
			}
		}

		// Check device-control
		if err := grpcCli.ConnectDeviceControl(); err != nil {
			results["device-control"] = "unreachable"
			allHealthy = false
		} else {
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			_, err := grpcCli.DeviceControl.GetDeviceStatus(ctx, &devicepb.Empty{})
			cancel()
			if err != nil {
				results["device-control"] = "error"
				allHealthy = false
			} else {
				results["device-control"] = "healthy"
			}
		}

		// Check platform-api (HTTP). The server exposes its health endpoint at
		// /api/v1/system/health (no auth); there is no bare /health route.
		url := systemAPIBase + "/api/v1/system/health"
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		req, _ := http.NewRequestWithContext(ctx, "GET", url, nil)
		resp, err := apiHTTPClient.Do(req)
		cancel()
		if err != nil || resp.StatusCode != http.StatusOK {
			results["platform-api"] = "unreachable"
			allHealthy = false
		} else {
			resp.Body.Close()
			results["platform-api"] = "healthy"
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(map[string]interface{}{
				"healthy":  allHealthy,
				"services": results,
			})
		}

		printer.Println("System Health Check")
		printer.Println("───────────────────")
		table := output.NewTable("SERVICE", "STATUS")
		for name, status := range results {
			table.AddRow(name, printer.FormatStatus(status))
		}
		table.RenderTo(printer)

		if allHealthy {
			printer.Success("\nAll services healthy")
		} else {
			printer.Error("\nSome services unhealthy")
			return fmt.Errorf("health check failed")
		}
		return nil
	},
}

func init() {
	// Global system flags
	systemCmd.PersistentFlags().StringVar(&systemAPIBase, "api", "http://localhost:8080", "Platform API base URL")

	systemCmd.AddCommand(systemInfoCmd)
	systemCmd.AddCommand(systemStatsCmd)
	systemCmd.AddCommand(systemHealthCmd)
	systemCmd.AddCommand(serviceStartCmd)
	systemCmd.AddCommand(serviceStopCmd)
	systemCmd.AddCommand(serviceRestartCmd)
	systemCmd.AddCommand(serviceStatusCmd)
	systemCmd.AddCommand(serviceEnableCmd)
	systemCmd.AddCommand(serviceDisableCmd)
}

// ============ service management ============

// aipcServices mirrors SERVICES in scripts/aipc-autostart.sh (dependency
// order; stop reverses it). system_test.go fails the build when the two
// lists drift, so `system stop/disable` always covers the full boot set.
var aipcServices = []string{
	"aipc-healthmon",
	"event-bus",
	"camera-daemon",
	"ai-runtime",
	"device-control",
	"device-discovery",
	"platform-api",
	"app-manager",
	"aipc-nginx-gateway",
}

// aipc-autostart re-runs `systemctl enable` + `start` for aipcServices on
// every boot, so `system disable` must disable it too or the next reboot
// silently undoes the command. A redeploy (or the OS-upgrade verify boot)
// re-enables it by design: that is the recovery path, not a bug.
const aipcAutostartUnit = "aipc-autostart.service"

// unitInstalled reports whether the unit exists at all (systemctl cat loads
// it from any search path). The CLI skips absent units instead of failing so
// it keeps working on installs that predate some of the services.
func unitInstalled(svc string) bool {
	return exec.Command("systemctl", "cat", svc+".service").Run() == nil
}

var serviceStartCmd = &cobra.Command{
	Use:   "start",
	Short: "Start all AIPC services",
	Long:  `Start all AIPC platform services in dependency order.`,
	RunE: func(cmd *cobra.Command, args []string) error {
		printer.Println("Starting AIPC services...")
		printer.Println("─────────────────────")
		return manageServices("start")
	},
}

var serviceStopCmd = &cobra.Command{
	Use:   "stop",
	Short: "Stop all AIPC services",
	Long:  `Stop all AIPC platform services in reverse dependency order.`,
	RunE: func(cmd *cobra.Command, args []string) error {
		printer.Println("Stopping AIPC services...")
		printer.Println("────────────────────")
		return manageServices("stop")
	},
}

var serviceRestartCmd = &cobra.Command{
	Use:   "restart",
	Short: "Restart all AIPC services",
	Long:  `Stop then start all AIPC platform services.`,
	RunE: func(cmd *cobra.Command, args []string) error {
		printer.Println("Restarting AIPC services...")
		printer.Println("──────────────────────────")
		if err := manageServices("stop"); err != nil {
			return err
		}
		printer.Println()
		return manageServices("start")
	},
}

var serviceStatusCmd = &cobra.Command{
	Use:   "status",
	Short: "Show status of all AIPC services",
	RunE: func(cmd *cobra.Command, args []string) error {
		printer.Println("AIPC Service Status")
		printer.Println("───────────────────")
		table := output.NewTable("SERVICE", "ACTIVE", "PID", "UPTIME")
		for _, svc := range aipcServices {
			active, pid, uptime := getServiceInfo(svc)
			table.AddRow(svc, printer.FormatStatus(active), pid, uptime)
		}
		table.RenderTo(printer)
		return nil
	},
}

func manageServices(action string) error {
	services := append([]string(nil), aipcServices...)
	// Stop in reverse order
	if action == "stop" {
		for i, j := 0, len(services)-1; i < j; i, j = i+1, j-1 {
			services[i], services[j] = services[j], services[i]
		}
	}

	var result error
	for _, svc := range services {
		if !unitInstalled(svc) {
			printer.Printf("  %-18s %s\n", svc+":", printer.FormatStatus("not installed"))
			continue
		}
		cmd := exec.Command("systemctl", action, svc+".service")
		out, err := cmd.CombinedOutput()
		if err != nil {
			printer.Printf("  %-18s %s (%s)\n", svc+":", printer.FormatStatus("failed"), strings.TrimSpace(string(out)))
			serviceErr := fmt.Errorf("%s %s failed: %s", action, svc, strings.TrimSpace(string(out)))
			if action != "stop" {
				return serviceErr
			}
			result = errors.Join(result, serviceErr)
			continue
		}
		printer.Printf("  %-18s %s\n", svc+":", printer.FormatStatus("ok"))
	}
	if result != nil {
		return result
	}
	printer.Success("\nAll services %sed", action)
	return nil
}

var serviceEnableCmd = &cobra.Command{
	Use:   "enable",
	Short: "Enable and start all AIPC services",
	Long: `Enable all AIPC services for auto-start on boot and start them immediately.

Also re-enables aipc-autostart.service, the boot unit that keeps the
service set enabled across reboots.`,
	RunE: func(cmd *cobra.Command, args []string) error {
		printer.Println("Enabling and starting AIPC services...")
		printer.Println("────────────────────────────────────")
		// Enable auto-start
		if err := enableDisableServices("enable"); err != nil {
			return err
		}
		if err := setAutostart("enable", false); err != nil {
			return err
		}
		// Start services now (dependency order)
		return manageServices("start")
	},
}

var serviceDisableCmd = &cobra.Command{
	Use:   "disable",
	Short: "Stop and disable all AIPC services",
	Long: `Stop all running AIPC services and disable them from starting on boot.

Also disables aipc-autostart.service, which would otherwise re-enable the
whole service set at the next reboot. A redeploy or an OS-upgrade verify
boot re-enables the platform by design (that is the recovery path).`,
	RunE: func(cmd *cobra.Command, args []string) error {
		printer.Println("Stopping and disabling AIPC services...")
		printer.Println("────────────────────────────────────")
		var result error
		// Quiesce the boot controller first so it cannot race with the
		// following stop/disable operations and requeue runtime units.
		if err := setAutostart("disable", true); err != nil {
			printer.Error("Warning: failed to disable boot auto-start: %v", err)
			result = errors.Join(result, err)
		}
		// Stop running services first (reverse dependency order)
		if err := manageServices("stop"); err != nil {
			printer.Error("Warning: some services failed to stop: %v", err)
			result = errors.Join(result, err)
		}
		// Then disable auto-start
		if err := enableDisableServices("disable"); err != nil {
			result = errors.Join(result, err)
		}
		return result
	},
}

func enableDisableServices(action string) error {
	var result error
	for _, svc := range aipcServices {
		if !unitInstalled(svc) {
			printer.Printf("  %-18s %s\n", svc+":", printer.FormatStatus("not installed"))
			continue
		}
		cmd := exec.Command("systemctl", action, svc+".service")
		out, err := cmd.CombinedOutput()
		if err != nil {
			printer.Printf("  %-18s %s (%s)\n", svc+":", printer.FormatStatus("failed"), strings.TrimSpace(string(out)))
			serviceErr := fmt.Errorf("%s %s failed: %s", action, svc, strings.TrimSpace(string(out)))
			if action != "disable" {
				return serviceErr
			}
			result = errors.Join(result, serviceErr)
			continue
		}
		label := "enabled"
		if action == "disable" {
			label = "disabled"
		}
		printer.Printf("  %-18s %s\n", svc+":", printer.FormatStatus(label))
	}
	if result != nil {
		return result
	}
	printer.Success("\nAll runtime services %sd", action)
	return nil
}

func setAutostart(action string, now bool) error {
	if unitInstalled(strings.TrimSuffix(aipcAutostartUnit, ".service")) {
		args := []string{action}
		if now {
			args = append(args, "--now")
		}
		args = append(args, aipcAutostartUnit)
		cmd := exec.Command("systemctl", args...)
		out, err := cmd.CombinedOutput()
		if err != nil {
			printer.Printf("  %-18s %s (%s)\n", aipcAutostartUnit+":", printer.FormatStatus("failed"), strings.TrimSpace(string(out)))
			return fmt.Errorf("%s %s failed: %s", action, aipcAutostartUnit, strings.TrimSpace(string(out)))
		}
		printer.Printf("  %-18s %s\n", aipcAutostartUnit+":", printer.FormatStatus("(boot auto-start)"))
	}
	return nil
}

func getServiceInfo(svc string) (active, pid, uptime string) {
	// Active state
	cmd := exec.Command("systemctl", "show", svc+".service", "--property=ActiveState", "--value")
	if out, err := cmd.Output(); err == nil {
		state := strings.TrimSpace(string(out))
		if state == "active" {
			active = "active"
		} else if state == "activating" {
			active = "activating"
		} else if state == "failed" {
			active = "failed"
		} else {
			active = state
		}
	} else {
		active = "unknown"
	}

	// Main PID
	cmd = exec.Command("systemctl", "show", svc+".service", "--property=MainPID", "--value")
	if out, err := cmd.Output(); err == nil {
		p := strings.TrimSpace(string(out))
		if p == "0" {
			pid = "-"
		} else {
			pid = p
		}
	}

	// Active enter timestamp (uptime)
	cmd = exec.Command("systemctl", "show", svc+".service", "--property=ActiveEnterTimestamp", "--value")
	if out, err := cmd.Output(); err == nil {
		ts := strings.TrimSpace(string(out))
		if ts != "" && ts != "n/a" {
			pid_ts, err := time.Parse("Mon 2006-01-02 15:04:05 MST", ts)
			if err == nil {
				uptime = fmt.Sprintf("%.0f min", time.Since(pid_ts).Minutes())
			} else {
				uptime = ts
			}
		}
	}
	return
}
