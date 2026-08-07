package cmd

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/spf13/cobra"

	"aipc/tools/aipc-cli/pkg/output"
)

var eventLogCmd = &cobra.Command{
	Use:   "event-log",
	Short: "Event log management",
	Long:  `View and manage event/operation logs.`,
}

var (
	eventLogAPIBase   string
	eventLogCategory  string
	eventLogLevel     string
	eventLogLimit     int
	eventLogStartTime string
	eventLogDays      int
)

// ============ event-log list ============

var eventLogListCmd = &cobra.Command{
	Use:   "list",
	Short: "List event logs",
	Aliases: []string{"ls"},
	RunE: func(cmd *cobra.Command, args []string) error {
		url := fmt.Sprintf("%s/api/v1/event-logs?limit=%d", eventLogAPIBase, eventLogLimit)
		if eventLogCategory != "" {
			url += fmt.Sprintf("&category=%s", eventLogCategory)
		}
		if eventLogLevel != "" {
			url += fmt.Sprintf("&level=%s", eventLogLevel)
		}
		if eventLogStartTime != "" {
			url += fmt.Sprintf("&start_time=%s", eventLogStartTime)
		}

		resp, err := doAPIGet(url)
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var result struct {
			Total   int `json:"total"`
			Entries []struct {
				ID        int    `json:"id"`
				Timestamp string `json:"timestamp"`
				EventType string `json:"event_type"`
				Level     string `json:"level"`
				Category  string `json:"category"`
				Source    string `json:"source"`
				Message   string `json:"message"`
				User      string `json:"user"`
			} `json:"entries"`
		}

		if err := json.Unmarshal(resp.Data, &result); err != nil {
			return fmt.Errorf("failed to parse event logs: %w", err)
		}

		if len(result.Entries) == 0 {
			printer.Info("No event logs found")
			return nil
		}

		printer.Printf("Event Logs (total: %d, showing: %d)\n", result.Total, len(result.Entries))
		table := output.NewTable("ID", "TIME", "LEVEL", "CATEGORY", "SOURCE", "MESSAGE")
		for _, e := range result.Entries {
			msg := e.Message
			if len(msg) > 60 {
				msg = msg[:57] + "..."
			}
			table.AddRow(
				fmt.Sprintf("%d", e.ID),
				e.Timestamp,
				e.Level,
				e.Category,
				e.Source,
				msg,
			)
		}
		table.RenderTo(printer)
		return nil
	},
}

// ============ event-log stats ============

var eventLogStatsCmd = &cobra.Command{
	Use:   "stats",
	Short: "Show event log statistics",
	RunE: func(cmd *cobra.Command, args []string) error {
		resp, err := doAPIGet(eventLogAPIBase + "/api/v1/event-logs/statistics")
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var stats struct {
			TodayErrors     int `json:"today_errors"`
			TodayWarnings   int `json:"today_warnings"`
			Operations      int `json:"operations"`
			SecurityEvents  int `json:"security_events"`
			AlarmEvents     int `json:"alarm_events"`
			SystemEvents    int `json:"system_events"`
			TotalEntries    int `json:"total_entries"`
		}

		if err := json.Unmarshal(resp.Data, &stats); err != nil {
			return fmt.Errorf("failed to parse statistics: %w", err)
		}

		printer.Println("Event Log Statistics")
		printer.Println("────────────────────")
		printer.Printf("  Total entries:     %d\n", stats.TotalEntries)
		printer.Printf("  Today errors:      %d\n", stats.TodayErrors)
		printer.Printf("  Today warnings:    %d\n", stats.TodayWarnings)
		printer.Printf("  Operations:        %d\n", stats.Operations)
		printer.Printf("  Security events:   %d\n", stats.SecurityEvents)
		printer.Printf("  Alarm events:      %d\n", stats.AlarmEvents)
		printer.Printf("  System events:     %d\n", stats.SystemEvents)
		return nil
	},
}

// ============ event-log cleanup ============

var eventLogCleanupCmd = &cobra.Command{
	Use:   "cleanup",
	Short: "Clean up old event logs",
	RunE: func(cmd *cobra.Command, args []string) error {
		body := map[string]int{"days": eventLogDays}
		payload, _ := json.Marshal(body)

		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "DELETE", eventLogAPIBase+"/api/v1/event-logs", bytes.NewReader(payload))
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}
		req.Header.Set("Content-Type", "application/json")

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("request failed: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		printer.Success("Cleaned up event logs older than %d days", eventLogDays)
		return nil
	},
}

func init() {
	eventLogCmd.PersistentFlags().StringVar(&eventLogAPIBase, "api", "http://localhost:8080", "Platform API base URL")

	eventLogListCmd.Flags().StringVar(&eventLogCategory, "category", "", "Filter by category: operation, security, alarm, system")
	eventLogListCmd.Flags().StringVar(&eventLogLevel, "level", "", "Filter by level: error, warn, info")
	eventLogListCmd.Flags().IntVar(&eventLogLimit, "limit", 50, "Max entries to show")
	eventLogListCmd.Flags().StringVar(&eventLogStartTime, "start", "", "Start time (RFC3339)")

	eventLogCleanupCmd.Flags().IntVar(&eventLogDays, "days", 30, "Delete entries older than N days")

	eventLogCmd.AddCommand(eventLogListCmd)
	eventLogCmd.AddCommand(eventLogStatsCmd)
	eventLogCmd.AddCommand(eventLogCleanupCmd)
}
