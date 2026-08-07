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

var streamCmd = &cobra.Command{
	Use:   "stream",
	Short: "Video stream management",
	Long:  `Manage video streams: list, info, URLs.`,
}

var (
	streamAPIBase string
)

// ============ stream list ============

var streamListCmd = &cobra.Command{
	Use:   "list",
	Short: "List available streams",
	RunE: func(cmd *cobra.Command, args []string) error {
		url := streamAPIBase + "/api/v1/streams"

		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("failed to get streams: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		var result struct {
			Streams []struct {
				ID      string `json:"id"`
				Name    string `json:"name"`
				Width   int    `json:"width"`
				Height  int    `json:"height"`
				FPS     int    `json:"fps"`
				HLSURL  string `json:"hls_url"`
				RTSPURL string `json:"rtsp_url"`
				Status  string `json:"status"`
			} `json:"streams"`
		}

		if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
			return fmt.Errorf("failed to decode response: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(result)
		}

		if len(result.Streams) == 0 {
			printer.Info("No streams available")
			return nil
		}

		table := output.NewTable("ID", "NAME", "RESOLUTION", "FPS", "STATUS")
		for _, s := range result.Streams {
			resolution := fmt.Sprintf("%dx%d", s.Width, s.Height)
			table.AddRow(
				s.ID,
				s.Name,
				resolution,
				fmt.Sprintf("%d", s.FPS),
				printer.FormatStatus(s.Status),
			)
		}
		table.RenderTo(printer)
		return nil
	},
}

// ============ stream info ============

var streamInfoCmd = &cobra.Command{
	Use:   "info <stream-id>",
	Short: "Show stream details",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		streamID := args[0]
		url := streamAPIBase + "/api/v1/streams/" + streamID

		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("failed to get stream info: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode == http.StatusNotFound {
			return fmt.Errorf("stream not found: %s", streamID)
		}
		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		var stream struct {
			ID      string `json:"id"`
			Name    string `json:"name"`
			Width   int    `json:"width"`
			Height  int    `json:"height"`
			FPS     int    `json:"fps"`
			HLSURL  string `json:"hls_url"`
			RTSPURL string `json:"rtsp_url"`
			Status  string `json:"status"`
		}

		if err := json.NewDecoder(resp.Body).Decode(&stream); err != nil {
			return fmt.Errorf("failed to decode response: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(stream)
		}

		printer.Printf("Stream: %s\n", stream.ID)
		printer.Printf("  Name:       %s\n", stream.Name)
		printer.Printf("  Resolution: %dx%d\n", stream.Width, stream.Height)
		printer.Printf("  FPS:        %d\n", stream.FPS)
		printer.Printf("  Status:     %s\n", printer.FormatStatus(stream.Status))
		printer.Printf("\n  URLs:\n")
		printer.Printf("    HLS:  %s%s\n", streamAPIBase, stream.HLSURL)
		printer.Printf("    RTSP: %s\n", stream.RTSPURL)
		return nil
	},
}

// ============ stream url ============

var (
	streamURLFormat string
)

var streamURLCmd = &cobra.Command{
	Use:   "url <stream-id>",
	Short: "Get stream URL",
	Long: `Get the URL for a specific stream.

Formats:
  hls  - HLS streaming URL (default)
  rtsp - RTSP streaming URL

Examples:
  aipc-cli stream url main
  aipc-cli stream url main --format rtsp
`,
	Args: cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		streamID := args[0]
		url := streamAPIBase + "/api/v1/streams/" + streamID

		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		req, err := http.NewRequestWithContext(ctx, "GET", url, nil)
		if err != nil {
			return fmt.Errorf("failed to create request: %w", err)
		}

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			return fmt.Errorf("failed to get stream info: %w", err)
		}
		defer resp.Body.Close()

		if resp.StatusCode == http.StatusNotFound {
			return fmt.Errorf("stream not found: %s", streamID)
		}
		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("API error: %s", resp.Status)
		}

		var stream struct {
			HLSURL  string `json:"hls_url"`
			RTSPURL string `json:"rtsp_url"`
		}

		if err := json.NewDecoder(resp.Body).Decode(&stream); err != nil {
			return fmt.Errorf("failed to decode response: %w", err)
		}

		switch streamURLFormat {
		case "hls":
			fmt.Println(streamAPIBase + stream.HLSURL)
		case "rtsp":
			fmt.Println(stream.RTSPURL)
		default:
			return fmt.Errorf("invalid format: use 'hls' or 'rtsp'")
		}
		return nil
	},
}

func init() {
	// Global stream flags
	streamCmd.PersistentFlags().StringVar(&streamAPIBase, "api", "http://localhost:8080", "Platform API base URL")

	// stream url flags
	streamURLCmd.Flags().StringVar(&streamURLFormat, "format", "hls", "URL format: hls, rtsp")

	// Register subcommands
	streamCmd.AddCommand(streamListCmd)
	streamCmd.AddCommand(streamInfoCmd)
	streamCmd.AddCommand(streamURLCmd)
}
