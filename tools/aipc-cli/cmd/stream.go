package cmd

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"strings"
	"time"

	"github.com/spf13/cobra"
	"github.com/spf13/viper"

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

// streamInfo mirrors one entry of GET /api/v1/media/streams (and the single
// object of GET /api/v1/media/streams/:name) inside the response envelope.
type streamInfo struct {
	ID         string `json:"id"`
	Name       string `json:"name"`
	Codec      string `json:"codec"`
	Width      int    `json:"width"`
	Height     int    `json:"height"`
	FPS        int    `json:"fps"`
	Bitrate    int    `json:"bitrate"`
	GOP        int    `json:"gop"`
	Enabled    bool   `json:"enabled"`
	Status     string `json:"status"`
	RtspURL    string `json:"rtsp_url"`
	H264WsPath string `json:"h264_ws_path"`
}

// fetchStreams lists the configured streams from GET /api/v1/media/streams.
func fetchStreams() ([]streamInfo, error) {
	resp, err := doAPIGet(streamAPIBase + "/api/v1/media/streams")
	if err != nil {
		return nil, err
	}

	var result struct {
		Streams []streamInfo `json:"streams"`
	}
	if err := json.Unmarshal(resp.Data, &result); err != nil {
		return nil, fmt.Errorf("failed to decode streams: %w", err)
	}
	return result.Streams, nil
}

// fetchStream gets a single stream from GET /api/v1/media/streams/:id. Unlike
// doAPIGet it maps HTTP 404 to a stream-not-found error instead of a generic
// API error.
func fetchStream(streamID string) (*streamInfo, error) {
	url := streamAPIBase + "/api/v1/media/streams/" + streamID

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, fmt.Errorf("failed to create request: %w", err)
	}
	if token := viper.GetString("auth.token"); token != "" {
		req.Header.Set("Authorization", "Bearer "+token)
	}

	resp, err := apiHTTPClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("failed to get stream info: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode == http.StatusNotFound {
		return nil, fmt.Errorf("stream not found: %s", streamID)
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("API error: %s", resp.Status)
	}

	var result apiResponse
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return nil, fmt.Errorf("failed to decode response: %w", err)
	}
	if result.Code != 0 {
		return nil, fmt.Errorf("API error %d: %s", result.Code, result.Message)
	}

	var stream streamInfo
	if err := json.Unmarshal(result.Data, &stream); err != nil {
		return nil, fmt.Errorf("failed to decode stream: %w", err)
	}
	return &stream, nil
}

// wsURL turns the API base plus the stream's h264_ws_path into a WebSocket
// URL (http→ws, https→wss), e.g. "http://host:8080" + "/api/v1/h264/main"
// → "ws://host:8080/api/v1/h264/main".
func wsURL(apiBase, path string) string {
	url := apiBase + path
	switch {
	case strings.HasPrefix(url, "https://"):
		return "wss://" + strings.TrimPrefix(url, "https://")
	case strings.HasPrefix(url, "http://"):
		return "ws://" + strings.TrimPrefix(url, "http://")
	default:
		return "ws://" + url
	}
}

// ============ stream list ============

var streamListCmd = &cobra.Command{
	Use:   "list",
	Short: "List available streams",
	RunE: func(cmd *cobra.Command, args []string) error {
		streams, err := fetchStreams()
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(map[string]any{"streams": streams})
		}

		if len(streams) == 0 {
			printer.Info("No streams available")
			return nil
		}

		table := output.NewTable("ID", "NAME", "RESOLUTION", "FPS", "STATUS")
		for _, s := range streams {
			table.AddRow(
				s.ID,
				s.Name,
				fmt.Sprintf("%dx%d", s.Width, s.Height),
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
		stream, err := fetchStream(args[0])
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(stream)
		}

		printer.Printf("Stream: %s\n", stream.ID)
		printer.Printf("  Name:       %s\n", stream.Name)
		printer.Printf("  Codec:      %s\n", stream.Codec)
		printer.Printf("  Resolution: %dx%d\n", stream.Width, stream.Height)
		printer.Printf("  FPS:        %d\n", stream.FPS)
		printer.Printf("  Bitrate:    %d bps\n", stream.Bitrate)
		printer.Printf("  GOP:        %d\n", stream.GOP)
		printer.Printf("  Enabled:    %t\n", stream.Enabled)
		printer.Printf("  Status:     %s\n", printer.FormatStatus(stream.Status))
		printer.Printf("\n  URLs:\n")
		printer.Printf("    WS:   %s\n", wsURL(streamAPIBase, stream.H264WsPath))
		printer.Printf("    RTSP: %s\n", stream.RtspURL)
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
  ws   - H264 over WebSocket URL for MSE playback (default)
  rtsp - RTSP streaming URL

Examples:
  aipc-cli stream url main
  aipc-cli stream url main --format rtsp
`,
	Args: cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		stream, err := fetchStream(args[0])
		if err != nil {
			return err
		}

		switch streamURLFormat {
		case "ws":
			fmt.Println(wsURL(streamAPIBase, stream.H264WsPath))
		case "rtsp":
			fmt.Println(stream.RtspURL)
		default:
			return fmt.Errorf("invalid format: use 'ws' or 'rtsp'")
		}
		return nil
	},
}

func init() {
	// Global stream flags
	streamCmd.PersistentFlags().StringVar(&streamAPIBase, "api", "http://localhost:8080", "Platform API base URL")

	// stream url flags
	streamURLCmd.Flags().StringVar(&streamURLFormat, "format", "ws", "URL format: ws, rtsp")

	// Register subcommands
	streamCmd.AddCommand(streamListCmd)
	streamCmd.AddCommand(streamInfoCmd)
	streamCmd.AddCommand(streamURLCmd)
}
