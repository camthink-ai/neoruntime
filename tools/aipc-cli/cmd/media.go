package cmd

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"time"

	"github.com/spf13/cobra"
	"github.com/spf13/viper"

	"aipc/tools/aipc-cli/pkg/output"
)

var mediaCmd = &cobra.Command{
	Use:   "media",
	Short: "Media configuration",
	Long:  `Manage media settings: ISP image, encoder, RTSP, AI overlay, OSD.`,
}

var (
	mediaAPIBase string
)

func doAPIPut(url string, body interface{}) (*apiResponse, error) {
	payload, err := json.Marshal(body)
	if err != nil {
		return nil, fmt.Errorf("failed to encode request body: %w", err)
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	req, err := http.NewRequestWithContext(ctx, "PUT", url, bytes.NewReader(payload))
	if err != nil {
		return nil, fmt.Errorf("failed to create request: %w", err)
	}
	req.Header.Set("Content-Type", "application/json")

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

// ============ media config ============

var mediaConfigCmd = &cobra.Command{
	Use:   "config",
	Short: "Show full media configuration",
	RunE: func(cmd *cobra.Command, args []string) error {
		resp, err := doAPIGet(mediaAPIBase + "/api/v1/media/config")
		if err != nil {
			return err
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(resp.Data)
		}

		var config struct {
			Camera struct {
				Streams []struct {
					Name       string `json:"name"`
					Width      int    `json:"width"`
					Height     int    `json:"height"`
					Codec      string `json:"codec"`
					FPS        int    `json:"fps"`
					BitrateBps int    `json:"bitrate_bps"`
					Gop        int    `json:"gop"`
				} `json:"streams"`
				ISP struct {
					Brightness  int `json:"brightness"`
					Contrast    int `json:"contrast"`
					Saturation  int `json:"saturation"`
					Sharpness   int `json:"sharpness"`
				} `json:"isp"`
				RTSP struct {
					Enabled bool `json:"enabled"`
					Port    int  `json:"port"`
				} `json:"rtsp"`
				AIOverlay struct {
					Enabled        bool `json:"enabled"`
					ShowLabel      bool `json:"show_label"`
					ShowConfidence bool `json:"show_confidence"`
					LineThickness  int  `json:"line_thickness"`
				} `json:"ai_overlay"`
			} `json:"camera"`
		}

		if err := json.Unmarshal(resp.Data, &config); err != nil {
			return fmt.Errorf("failed to parse config: %w", err)
		}

		printer.Println("Media Configuration")
		printer.Println("───────────────────")

		printer.Println("\nStreams:")
		table := output.NewTable("NAME", "RESOLUTION", "CODEC", "FPS", "BITRATE", "GOP")
		for _, s := range config.Camera.Streams {
			table.AddRow(
				s.Name,
				fmt.Sprintf("%dx%d", s.Width, s.Height),
				s.Codec,
				fmt.Sprintf("%d", s.FPS),
				output.FormatBytes(int64(s.BitrateBps)),
				fmt.Sprintf("%d", s.Gop),
			)
		}
		table.RenderTo(printer)

		printer.Println("\nISP:")
		printer.Printf("  Brightness: %d  Contrast: %d  Saturation: %d  Sharpness: %d\n",
			config.Camera.ISP.Brightness, config.Camera.ISP.Contrast,
			config.Camera.ISP.Saturation, config.Camera.ISP.Sharpness)

		printer.Println("\nRTSP:")
		rtspStatus := "disabled"
		if config.Camera.RTSP.Enabled {
			rtspStatus = fmt.Sprintf("enabled (port %d)", config.Camera.RTSP.Port)
		}
		printer.Printf("  %s\n", rtspStatus)

		printer.Println("\nAI Overlay:")
		overlayStatus := "disabled"
		if config.Camera.AIOverlay.Enabled {
			overlayStatus = fmt.Sprintf("enabled (label=%v, confidence=%v, thickness=%d)",
				config.Camera.AIOverlay.ShowLabel, config.Camera.AIOverlay.ShowConfidence,
				config.Camera.AIOverlay.LineThickness)
		}
		printer.Printf("  %s\n", overlayStatus)
		return nil
	},
}

// ============ media image ============

var (
	mediaBrightness  int
	mediaContrast    int
	mediaSaturation  int
	mediaSharpness   int
)

var mediaImageCmd = &cobra.Command{
	Use:   "image",
	Short: "Update ISP image parameters",
	Long: `Update ISP image parameters (brightness, contrast, saturation, sharpness).

All values are 0-100.

Examples:
  aipc-cli media image --brightness 60
  aipc-cli media image --contrast 55 --saturation 45`,
	RunE: func(cmd *cobra.Command, args []string) error {
		body := map[string]interface{}{}
		changed := false

		if cmd.Flags().Changed("brightness") {
			body["brightness"] = mediaBrightness
			changed = true
		}
		if cmd.Flags().Changed("contrast") {
			body["contrast"] = mediaContrast
			changed = true
		}
		if cmd.Flags().Changed("saturation") {
			body["saturation"] = mediaSaturation
			changed = true
		}
		if cmd.Flags().Changed("sharpness") {
			body["sharpness"] = mediaSharpness
			changed = true
		}

		if !changed {
			return fmt.Errorf("at least one parameter must be specified (use --brightness, --contrast, --saturation, --sharpness)")
		}

		_, err := doAPIPut(mediaAPIBase+"/api/v1/media/image", body)
		if err != nil {
			return err
		}

		printer.Success("Image parameters updated")
		return nil
	},
}

// ============ media encoder ============

var (
	mediaStream   string
	mediaBitrate  int
	mediaFPS      int
	mediaGop      int
)

var mediaEncoderCmd = &cobra.Command{
	Use:   "encoder",
	Short: "Update encoder parameters",
	Long: `Update video encoder parameters for a stream.

Examples:
  aipc-cli encoder --stream main --bitrate 5000000
  aipc-cli encoder --stream sub --fps 15 --gop 30`,
	RunE: func(cmd *cobra.Command, args []string) error {
		body := map[string]interface{}{}

		if mediaStream != "" {
			body["stream_name"] = mediaStream
		}
		if cmd.Flags().Changed("bitrate") {
			body["bitrate_bps"] = mediaBitrate
		}
		if cmd.Flags().Changed("fps") {
			body["framerate"] = mediaFPS
		}
		if cmd.Flags().Changed("gop") {
			body["gop"] = mediaGop
		}

		_, err := doAPIPut(mediaAPIBase+"/api/v1/media/encoder", body)
		if err != nil {
			return err
		}

		printer.Success("Encoder parameters updated")
		return nil
	},
}

// ============ media rtsp ============

var (
	mediaRTSPEnable bool
)

var mediaRTSPCmd = &cobra.Command{
	Use:   "rtsp",
	Short: "Enable or disable RTSP server",
	Long: `Enable or disable the RTSP streaming server.

Examples:
  aipc-cli media rtsp --enable
  aipc-cli media rtsp --disable`,
	RunE: func(cmd *cobra.Command, args []string) error {
		if !cmd.Flags().Changed("enable") {
			return fmt.Errorf("use --enable or --disable")
		}
		enabled := mediaRTSPEnable
		body := map[string]interface{}{"enabled": enabled}

		_, err := doAPIPut(mediaAPIBase+"/api/v1/media/rtsp", body)
		if err != nil {
			return err
		}

		if enabled {
			printer.Success("RTSP enabled")
		} else {
			printer.Success("RTSP disabled")
		}
		return nil
	},
}

// ============ media ai-overlay ============

var (
	mediaAIOverlayEnable        bool
	mediaAIOverlayShowLabel     bool
	mediaAIOverlayShowConf      bool
	mediaAIOverlayThickness     int
)

var mediaAIOverlayCmd = &cobra.Command{
	Use:   "ai-overlay",
	Short: "Update AI overlay settings",
	Long: `Update AI detection overlay settings.

Examples:
  aipc-cli media ai-overlay --enable --show-label
  aipc-cli media ai-overlay --disable`,
	RunE: func(cmd *cobra.Command, args []string) error {
		body := map[string]interface{}{}
		changed := false

		if cmd.Flags().Changed("enable") {
			body["enabled"] = mediaAIOverlayEnable
			changed = true
		}
		if cmd.Flags().Changed("show-label") {
			body["show_label"] = mediaAIOverlayShowLabel
			changed = true
		}
		if cmd.Flags().Changed("show-confidence") {
			body["show_confidence"] = mediaAIOverlayShowConf
			changed = true
		}
		if cmd.Flags().Changed("thickness") {
			body["line_thickness"] = mediaAIOverlayThickness
			changed = true
		}

		if !changed {
			return fmt.Errorf("at least one parameter must be specified")
		}

		_, err := doAPIPut(mediaAPIBase+"/api/v1/media/ai-overlay", body)
		if err != nil {
			return err
		}

		printer.Success("AI overlay settings updated")
		return nil
	},
}

// ============ media osd ============

var mediaOSDCmd = &cobra.Command{
	Use:   "osd",
	Short: "Update OSD configuration",
	Long: `Update On-Screen Display configuration.

OSD configuration is complex. Use JSON format for full control:
  aipc-cli media osd -o json  # View current config via media config`,
	RunE: func(cmd *cobra.Command, args []string) error {
		// OSD config is passed via stdin as JSON
		if len(args) == 0 {
			return fmt.Errorf("provide OSD config as JSON argument, e.g.: '{\"streams\":[{\"stream_name\":\"main\",\"text_overlays\":[]}]}'")
		}

		var body interface{}
		if err := json.Unmarshal([]byte(args[0]), &body); err != nil {
			return fmt.Errorf("invalid JSON: %w", err)
		}

		_, err := doAPIPut(mediaAPIBase+"/api/v1/media/osd", body)
		if err != nil {
			return err
		}

		printer.Success("OSD configuration updated")
		return nil
	},
}

func init() {
	mediaCmd.PersistentFlags().StringVar(&mediaAPIBase, "api", "http://localhost:8080", "Platform API base URL")

	// image flags
	mediaImageCmd.Flags().IntVar(&mediaBrightness, "brightness", 50, "Brightness (0-100)")
	mediaImageCmd.Flags().IntVar(&mediaContrast, "contrast", 50, "Contrast (0-100)")
	mediaImageCmd.Flags().IntVar(&mediaSaturation, "saturation", 50, "Saturation (0-100)")
	mediaImageCmd.Flags().IntVar(&mediaSharpness, "sharpness", 50, "Sharpness (0-100)")

	// encoder flags
	mediaEncoderCmd.Flags().StringVar(&mediaStream, "stream", "", "Stream name (main, sub, third)")
	mediaEncoderCmd.Flags().IntVar(&mediaBitrate, "bitrate", 0, "Bitrate in bps")
	mediaEncoderCmd.Flags().IntVar(&mediaFPS, "fps", 0, "Frame rate")
	mediaEncoderCmd.Flags().IntVar(&mediaGop, "gop", 0, "Group of pictures size")

	// rtsp flags
	mediaRTSPCmd.Flags().BoolVar(&mediaRTSPEnable, "enable", false, "Enable RTSP")
	// --disable is the inverse of --enable
	mediaRTSPCmd.Flags().Bool("disable", false, "Disable RTSP")

	// ai-overlay flags
	mediaAIOverlayCmd.Flags().BoolVar(&mediaAIOverlayEnable, "enable", false, "Enable AI overlay")
	mediaAIOverlayCmd.Flags().BoolVar(&mediaAIOverlayShowLabel, "show-label", true, "Show detection labels")
	mediaAIOverlayCmd.Flags().BoolVar(&mediaAIOverlayShowConf, "show-confidence", true, "Show confidence scores")
	mediaAIOverlayCmd.Flags().IntVar(&mediaAIOverlayThickness, "thickness", 2, "Bounding box line thickness")

	mediaCmd.AddCommand(mediaConfigCmd)
	mediaCmd.AddCommand(mediaImageCmd)
	mediaCmd.AddCommand(mediaEncoderCmd)
	mediaCmd.AddCommand(mediaRTSPCmd)
	mediaCmd.AddCommand(mediaAIOverlayCmd)
	mediaCmd.AddCommand(mediaOSDCmd)
}
