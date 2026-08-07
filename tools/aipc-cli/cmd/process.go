package cmd

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/spf13/cobra"

	"aipc/tools/aipc-cli/pkg/output"
)

var processCmd = &cobra.Command{
	Use:   "process",
	Short: "Process management",
	Long:  `Manage system processes: list, info, kill.`,
	Aliases: []string{"ps"},
}

var (
	processAPIBase string
	processSort    string
	processLimit   int
	processSearch  string
	processSignal  string
)

// ============ process list ============

var processListCmd = &cobra.Command{
	Use:   "list",
	Short: "List running processes",
	Aliases: []string{"ls"},
	RunE: func(cmd *cobra.Command, args []string) error {
		url := fmt.Sprintf("%s/api/v1/processes?sort=%s&limit=%d", processAPIBase, processSort, processLimit)
		if processSearch != "" {
			url += fmt.Sprintf("&search=%s", processSearch)
		}

		resp, err := doAPIGet(url)
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var result struct {
			Total     int `json:"total"`
			Processes []struct {
				PID        int32   `json:"pid"`
				Name       string  `json:"name"`
				Status     string  `json:"status"`
				CPUPercent float64 `json:"cpu_percent"`
				MemPercent float64 `json:"mem_percent"`
				MemRSS     uint64  `json:"mem_rss"`
				Username   string  `json:"username"`
				Cmdline    string  `json:"cmdline"`
			} `json:"processes"`
		}

		if err := json.Unmarshal(resp.Data, &result); err != nil {
			return fmt.Errorf("failed to parse process list: %w", err)
		}

		if len(result.Processes) == 0 {
			printer.Info("No processes found")
			return nil
		}

		printer.Printf("Processes (total: %d, showing: %d)\n", result.Total, len(result.Processes))
		table := output.NewTable("PID", "NAME", "USER", "CPU%", "MEM%", "RSS", "STATUS")
		for _, p := range result.Processes {
			table.AddRow(
				fmt.Sprintf("%d", p.PID),
				p.Name,
				p.Username,
				fmt.Sprintf("%.1f", p.CPUPercent),
				fmt.Sprintf("%.1f", p.MemPercent),
				output.FormatBytes(int64(p.MemRSS)),
				printer.FormatStatus(p.Status),
			)
		}
		table.RenderTo(printer)
		return nil
	},
}

// ============ process info ============

var processInfoCmd = &cobra.Command{
	Use:   "info <pid>",
	Short: "Show process details",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		pid := args[0]
		url := fmt.Sprintf("%s/api/v1/processes/%s", processAPIBase, pid)

		resp, err := doAPIGet(url)
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var p struct {
			PID        int32   `json:"pid"`
			PPID       int32   `json:"ppid"`
			Name       string  `json:"name"`
			Status     []string `json:"status"`
			CPUPercent float64 `json:"cpu_percent"`
			MemPercent float64 `json:"mem_percent"`
			Username   string  `json:"username"`
			Cmdline    string  `json:"cmdline"`
			Cwd        string  `json:"cwd"`
			Exe        string  `json:"exe"`
			NumThreads int     `json:"num_threads"`
			CreateTime int64   `json:"create_time"`
		}

		if err := json.Unmarshal(resp.Data, &p); err != nil {
			return fmt.Errorf("failed to parse process info: %w", err)
		}

		printer.Println("Process Details")
		printer.Println("───────────────")
		printer.Printf("  PID:       %d\n", p.PID)
		printer.Printf("  PPID:      %d\n", p.PPID)
		printer.Printf("  Name:      %s\n", p.Name)
		printer.Printf("  User:      %s\n", p.Username)
		printer.Printf("  Status:    %v\n", p.Status)
		printer.Printf("  CPU:       %.1f%%\n", p.CPUPercent)
		printer.Printf("  Memory:    %.1f%%\n", p.MemPercent)
		printer.Printf("  Threads:   %d\n", p.NumThreads)
		printer.Printf("  Executable: %s\n", p.Exe)
		printer.Printf("  CWD:       %s\n", p.Cwd)
		printer.Printf("  Command:   %s\n", p.Cmdline)
		return nil
	},
}

// ============ process kill ============

var processKillCmd = &cobra.Command{
	Use:   "kill <pid>",
	Short: "Terminate a process",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		pid := args[0]
		url := fmt.Sprintf("%s/api/v1/processes/%s/kill?signal=%s", processAPIBase, pid, processSignal)

		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "POST", url, nil)
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("request failed: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode == http.StatusNotFound {
			return fmt.Errorf("process not found: %s", pid)
		}
		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		printer.Success("Sent %s to process %s", processSignal, pid)
		return nil
	},
}

func init() {
	processCmd.PersistentFlags().StringVar(&processAPIBase, "api", "http://localhost:8080", "Platform API base URL")

	processListCmd.Flags().StringVar(&processSort, "sort", "cpu", "Sort by: cpu, mem, pid")
	processListCmd.Flags().IntVar(&processLimit, "limit", 50, "Max processes to show")
	processListCmd.Flags().StringVar(&processSearch, "search", "", "Filter by name")

	processKillCmd.Flags().StringVar(&processSignal, "signal", "SIGTERM", "Signal to send (SIGTERM, SIGKILL)")

	processCmd.AddCommand(processListCmd)
	processCmd.AddCommand(processInfoCmd)
	processCmd.AddCommand(processKillCmd)
}
