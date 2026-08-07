package cmd

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/spf13/cobra"
	"github.com/spf13/viper"

	"aipc/tools/aipc-cli/pkg/output"
)

var monitorCmd = &cobra.Command{
	Use:   "monitor",
	Short: "System resource monitoring",
	Long:  `Monitor system resources: CPU, memory, disk, network.`,
}

var (
	monitorAPIBase string
)

// API response envelope
type apiResponse struct {
	Code    int             `json:"code"`
	Message string          `json:"message"`
	Data    json.RawMessage `json:"data"`
}

func doAPIGet(url string) (*apiResponse, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
	if err != nil {
		return nil, fmt.Errorf("failed to create request: %w", err)
	}

	if token := viper.GetString("auth.token"); token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}

	resp, err := http.DefaultClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("request failed: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("API error: %s", resp.Status)
	}

	var result apiResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return nil, fmt.Errorf("failed to decode response: %w", err)
	}
	return &result, nil
}

// ============ monitor summary ============

var monitorSummaryCmd = &cobra.Command{
	Use:   "summary",
	Short: "Show resource overview (CPU, memory, disk, network)",
	RunE: func(cmd *cobra.Command, args []string) error {
		resp, err := doAPIGet(monitorAPIBase + "/api/v1/monitor/summary")
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var summary struct {
			CPU struct {
				UsagePercent float64 `json:"usage_percent"`
				Cores        int     `json:"cores"`
			} `json:"cpu"`
			Memory struct {
				Virtual struct {
					Total         uint64  `json:"total"`
					Used          uint64  `json:"used"`
					UsagePercent  float64 `json:"usage_percent"`
				} `json:"virtual"`
			} `json:"memory"`
			Disk struct {
				Partitions []struct {
					Device       string  `json:"device"`
					Mountpoint   string  `json:"mountpoint"`
					Total        uint64  `json:"total"`
					Used         uint64  `json:"used"`
					UsagePercent float64 `json:"usage_percent"`
				} `json:"partitions"`
			} `json:"disk"`
			Network struct {
				Interfaces []struct {
					Name      string `json:"name"`
					BytesSent uint64 `json:"bytes_sent"`
					BytesRecv uint64 `json:"bytes_recv"`
				} `json:"interfaces"`
			} `json:"network"`
			Host struct {
				Hostname string `json:"hostname"`
				OS       string `json:"os"`
				Platform string `json:"platform"`
			} `json:"host"`
		}

		if err := json.Unmarshal(resp.Data, &summary); err != nil {
			return fmt.Errorf("failed to parse summary: %w", err)
		}

		printer.Println("System Resource Summary")
		printer.Println("───────────────────────")
		printer.Printf("Host:     %s (%s)\n", summary.Host.Hostname, summary.Host.Platform)
		printer.Printf("CPU:      %.1f%% (%d cores)\n", summary.CPU.UsagePercent, summary.CPU.Cores)
		printer.Printf("Memory:   %.1f%% (%s / %s)\n",
			summary.Memory.Virtual.UsagePercent,
			output.FormatBytes(int64(summary.Memory.Virtual.Used)),
			output.FormatBytes(int64(summary.Memory.Virtual.Total)))
		if len(summary.Disk.Partitions) > 0 {
			p := summary.Disk.Partitions[0]
			printer.Printf("Disk:     %.1f%% (%s / %s) [%s]\n",
				p.UsagePercent,
				output.FormatBytes(int64(p.Used)),
				output.FormatBytes(int64(p.Total)),
				p.Mountpoint)
		}
		if len(summary.Network.Interfaces) > 0 {
			iface := summary.Network.Interfaces[0]
			printer.Printf("Network:  %s (↑%s ↓%s)\n",
				iface.Name,
				output.FormatBytes(int64(iface.BytesSent)),
				output.FormatBytes(int64(iface.BytesRecv)))
		}
		return nil
	},
}

// ============ monitor cpu ============

var monitorCPUCmd = &cobra.Command{
	Use:   "cpu",
	Short: "Show CPU usage details",
	RunE: func(cmd *cobra.Command, args []string) error {
		resp, err := doAPIGet(monitorAPIBase + "/api/v1/monitor/cpu")
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var cpu struct {
			UsagePercent float64 `json:"usage_percent"`
			Cores        int     `json:"cores"`
			Arch         string  `json:"arch"`
			CoreList     []struct {
				Model        string  `json:"model"`
				Mhz          float64 `json:"mhz"`
				UsagePercent float64 `json:"usage_percent"`
			} `json:"core_list"`
		}

		if err := json.Unmarshal(resp.Data, &cpu); err != nil {
			return fmt.Errorf("failed to parse CPU data: %w", err)
		}

		printer.Printf("CPU Usage: %.1f%% | Arch: %s | Cores: %d\n", cpu.UsagePercent, cpu.Arch, cpu.Cores)
		if len(cpu.CoreList) > 0 {
			table := output.NewTable("CORE", "MODEL", "MHZ", "USAGE")
			for i, c := range cpu.CoreList {
				table.AddRow(fmt.Sprintf("%d", i), c.Model, fmt.Sprintf("%.0f", c.Mhz), fmt.Sprintf("%.1f%%", c.UsagePercent))
			}
			table.RenderTo(printer)
		}
		return nil
	},
}

// ============ monitor memory ============

var monitorMemoryCmd = &cobra.Command{
	Use:   "memory",
	Short: "Show memory and swap usage",
	Aliases: []string{"mem"},
	RunE: func(cmd *cobra.Command, args []string) error {
		resp, err := doAPIGet(monitorAPIBase + "/api/v1/monitor/memory")
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var mem struct {
			Virtual struct {
				Total        uint64  `json:"total"`
				Used         uint64  `json:"used"`
				Available    uint64  `json:"available"`
				UsagePercent float64 `json:"usage_percent"`
			} `json:"virtual"`
			Swap struct {
				Total        uint64  `json:"total"`
				Used         uint64  `json:"used"`
				UsagePercent float64 `json:"usage_percent"`
			} `json:"swap"`
		}

		if err := json.Unmarshal(resp.Data, &mem); err != nil {
			return fmt.Errorf("failed to parse memory data: %w", err)
		}

		printer.Println("Memory Usage")
		printer.Println("─────────────")
		printer.Printf("Virtual: %.1f%% (%s used / %s total / %s available)\n",
			mem.Virtual.UsagePercent,
			output.FormatBytes(int64(mem.Virtual.Used)),
			output.FormatBytes(int64(mem.Virtual.Total)),
			output.FormatBytes(int64(mem.Virtual.Available)))
		printer.Printf("Swap:    %.1f%% (%s used / %s total)\n",
			mem.Swap.UsagePercent,
			output.FormatBytes(int64(mem.Swap.Used)),
			output.FormatBytes(int64(mem.Swap.Total)))
		return nil
	},
}

// ============ monitor disk ============

var monitorDiskCmd = &cobra.Command{
	Use:   "disk",
	Short: "Show disk partition usage",
	RunE: func(cmd *cobra.Command, args []string) error {
		resp, err := doAPIGet(monitorAPIBase + "/api/v1/monitor/disk")
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var disk struct {
			Partitions []struct {
				Device       string  `json:"device"`
				Mountpoint   string  `json:"mountpoint"`
				Fstype       string  `json:"fstype"`
				Total        uint64  `json:"total"`
				Used         uint64  `json:"used"`
				UsagePercent float64 `json:"usage_percent"`
				IsSystem     bool    `json:"is_system"`
			} `json:"partitions"`
		}

		if err := json.Unmarshal(resp.Data, &disk); err != nil {
			return fmt.Errorf("failed to parse disk data: %w", err)
		}

		if len(disk.Partitions) == 0 {
			printer.Info("No disk partitions found")
			return nil
		}

		table := output.NewTable("DEVICE", "MOUNT", "TYPE", "TOTAL", "USED", "USAGE", "SYSTEM")
		for _, p := range disk.Partitions {
			sysMark := ""
			if p.IsSystem {
				sysMark = "*"
			}
			table.AddRow(
				p.Device,
				p.Mountpoint,
				p.Fstype,
				output.FormatBytes(int64(p.Total)),
				output.FormatBytes(int64(p.Used)),
				fmt.Sprintf("%.1f%%", p.UsagePercent),
				sysMark,
			)
		}
		table.RenderTo(printer)
		return nil
	},
}

// ============ monitor network ============

var monitorNetworkCmd = &cobra.Command{
	Use:   "network",
	Short: "Show network interface statistics",
	Aliases: []string{"net"},
	RunE: func(cmd *cobra.Command, args []string) error {
		resp, err := doAPIGet(monitorAPIBase + "/api/v1/monitor/network")
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var net struct {
			Interfaces []struct {
				Name         string `json:"name"`
				BytesSent    uint64 `json:"bytes_sent"`
				BytesRecv    uint64 `json:"bytes_recv"`
				PacketsSent  uint64 `json:"packets_sent"`
				PacketsRecv  uint64 `json:"packets_recv"`
			} `json:"interfaces"`
		}

		if err := json.Unmarshal(resp.Data, &net); err != nil {
			return fmt.Errorf("failed to parse network data: %w", err)
		}

		if len(net.Interfaces) == 0 {
			printer.Info("No network interfaces found")
			return nil
		}

		table := output.NewTable("INTERFACE", "BYTES SENT", "BYTES RECV", "PACKETS SENT", "PACKETS RECV")
		for _, i := range net.Interfaces {
			table.AddRow(
				i.Name,
				output.FormatBytes(int64(i.BytesSent)),
				output.FormatBytes(int64(i.BytesRecv)),
				fmt.Sprintf("%d", i.PacketsSent),
				fmt.Sprintf("%d", i.PacketsRecv),
			)
		}
		table.RenderTo(printer)
		return nil
	},
}

func init() {
	monitorCmd.PersistentFlags().StringVar(&monitorAPIBase, "api", "http://localhost:8080", "Platform API base URL")

	monitorCmd.AddCommand(monitorSummaryCmd)
	monitorCmd.AddCommand(monitorCPUCmd)
	monitorCmd.AddCommand(monitorMemoryCmd)
	monitorCmd.AddCommand(monitorDiskCmd)
	monitorCmd.AddCommand(monitorNetworkCmd)
}
