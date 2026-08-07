package cmd

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"time"

	"github.com/spf13/cobra"

	"aipc/tools/aipc-cli/pkg/output"
)

var logsCmd = &cobra.Command{
	Use:   "logs",
	Short: "System logs",
	Long:  `View and manage system logs: list services, view content, download.`,
}

var (
	logsAPIBase  string
	logsLines    int
	logsLevel    string
)

// ============ logs services ============

var logsServicesCmd = &cobra.Command{
	Use:   "services",
	Short: "List available log services",
	RunE: func(cmd *cobra.Command, args []string) error {
		resp, err := doAPIGet(logsAPIBase + "/api/v1/logs/services")
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var services []struct {
			ID   string `json:"id"`
			Name string `json:"name"`
		}

		if err := json.Unmarshal(resp.Data, &services); err != nil {
			return fmt.Errorf("failed to parse services: %w", err)
		}

		if len(services) == 0 {
			printer.Info("No log services found")
			return nil
		}

		table := output.NewTable("ID", "NAME")
		for _, s := range services {
			table.AddRow(s.ID, s.Name)
		}
		table.RenderTo(printer)
		return nil
	},
}

// ============ logs files ============

var logsFilesCmd = &cobra.Command{
	Use:   "files",
	Short: "List log files",
	RunE: func(cmd *cobra.Command, args []string) error {
		resp, err := doAPIGet(logsAPIBase + "/api/v1/logs/files")
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var files []struct {
			Path        string `json:"path"`
			Name        string `json:"name"`
			Size        int64  `json:"size"`
			ModifiedTime string `json:"modified_time"`
			Service     string `json:"service"`
		}

		if err := json.Unmarshal(resp.Data, &files); err != nil {
			return fmt.Errorf("failed to parse log files: %w", err)
		}

		if len(files) == 0 {
			printer.Info("No log files found")
			return nil
		}

		table := output.NewTable("NAME", "SERVICE", "SIZE", "MODIFIED")
		for _, f := range files {
			table.AddRow(f.Name, f.Service, output.FormatBytes(int64(f.Size)), f.ModifiedTime)
		}
		table.RenderTo(printer)
		return nil
	},
}

// ============ logs show ============

var logsShowCmd = &cobra.Command{
	Use:   "show [service]",
	Short: "Show log content for a service",
	Long: `Show log content for a specific service.

Examples:
  aipc-cli logs show ai-runtime
  aipc-cli logs show ai-runtime --lines 500
  aipc-cli logs show camera-daemon --level error`,
	Args: cobra.MaximumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		target := "system"
		if len(args) > 0 {
			target = args[0]
		}

		url := fmt.Sprintf("%s/api/v1/logs/content?type=service&target=%s&lines=%d", logsAPIBase, target, logsLines)
		if logsLevel != "" {
			url += fmt.Sprintf("&level=%s", logsLevel)
		}

		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("request failed: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		// Try to parse as structured response first
		var result struct {
			Content string `json:"content"`
		}
		if json.NewDecoder(resp.Body).Decode(&result) == nil && result.Content != "" {
			fmt.Print(result.Content)
			return nil
		}

		// Fallback: raw output
		_, err = io.Copy(os.Stdout, resp.Body)
		return err
	},
}

// ============ logs download ============

var logsDownloadCmd = &cobra.Command{
	Use:   "download <file>",
	Short: "Download a log file",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		logFile := args[0]
		url := fmt.Sprintf("%s/api/v1/logs/download?file=%s", logsAPIBase, logFile)

		ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("download failed: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		localName := logFile
		_, err = fmt.Sscanf(logFile, "/%s", &localName)
		f, err := os.Create(localName)
		if err != nil {
			return fmt.Errorf("failed to create file: %w", err)
		}
		defer f.Close()

		if _, err := io.Copy(f, resp.Body); err != nil {
			return fmt.Errorf("failed to write file: %w", err)
		}

		printer.Success("Downloaded: %s", localName)
		return nil
	},
}

func init() {
	logsCmd.PersistentFlags().StringVar(&logsAPIBase, "api", "http://localhost:8080", "Platform API base URL")

	logsShowCmd.Flags().IntVar(&logsLines, "lines", 200, "Number of log lines to show")
	logsShowCmd.Flags().StringVar(&logsLevel, "level", "", "Filter by level: error, warn, info")

	logsCmd.AddCommand(logsServicesCmd)
	logsCmd.AddCommand(logsFilesCmd)
	logsCmd.AddCommand(logsShowCmd)
	logsCmd.AddCommand(logsDownloadCmd)
}
