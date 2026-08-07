package cmd

import (
	"context"
	"fmt"
	"strconv"
	"strings"

	"github.com/spf13/cobra"

	devicepb "aipc/platform/device-control/proto"
	"aipc/tools/aipc-cli/pkg/output"
)

var deviceCmd = &cobra.Command{
	Use:   "device",
	Short: "Device control commands",
	Long:  `Control device peripherals: light, IR, PTZ, zoom, focus, GPIO.`,
}

// connectDeviceControl ensures we have a connection to device-control
func connectDeviceControl() error {
	return grpcCli.ConnectDeviceControl()
}

// ============ device status ============

var deviceStatusCmd = &cobra.Command{
	Use:   "status",
	Short: "Show device status",
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectDeviceControl(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		status, err := grpcCli.DeviceControl.GetDeviceStatus(ctx, &devicepb.Empty{})
		if err != nil {
			return fmt.Errorf("failed to get device status: %w", err)
		}

		if outputFmt == "json" || outputFmt == "yaml" {
			return printer.Print(protoDeviceStatusToMap(status))
		}

		printer.Printf("Device Status\n")
		printer.Printf("─────────────────────────────\n")
		printer.Printf("Temperature:\n")
		printer.Printf("  SoC:          %.1f°C\n", status.SocTempC)
		printer.Printf("  MCU:          %.1f°C\n", status.McuTempC)
		printer.Printf("\nSensors:\n")
		printer.Printf("  Light:        %d\n", status.LightSensor)
		printer.Printf("\nLighting:\n")
		printer.Printf("  White Light:  %d%%\n", status.WhiteLightLevel)
		printer.Printf("  IR LED:       %d%%\n", status.IrLedLevel)
		printer.Printf("  IR-Cut:       %s\n", formatIrCutMode(status.IrcutMode))
		printer.Printf("\nPTZ Position:\n")
		printer.Printf("  Pan:          %d\n", status.PtzPanPos)
		printer.Printf("  Tilt:         %d\n", status.PtzTiltPos)
		printer.Printf("\nLens:\n")
		printer.Printf("  Zoom:         %d\n", status.ZoomPos)
		printer.Printf("  Focus:        %d\n", status.FocusPos)
		printer.Printf("  Autofocus:    %s\n", formatBool(status.AutofocusEnabled))
		printer.Printf("\nMCU:\n")
		printer.Printf("  Version:      %s\n", status.McuVersion)
		printer.Printf("  Uptime:       %s\n", output.FormatDuration(int64(status.McuUptimeMs/1000)))
		return nil
	},
}

// ============ device light ============

var (
	lightLevel uint32
)

var deviceLightCmd = &cobra.Command{
	Use:   "light <level>",
	Short: "Set white light level (0-100)",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		var level uint32
		if _, err := fmt.Sscanf(args[0], "%d", &level); err != nil {
			return fmt.Errorf("invalid level: %s", args[0])
		}
		if level > 100 {
			return fmt.Errorf("level must be 0-100")
		}

		if err := connectDeviceControl(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		result, err := grpcCli.DeviceControl.SetWhiteLight(ctx, &devicepb.LightLevelRequest{
			Level: level,
		})
		if err != nil {
			return fmt.Errorf("failed to set light: %w", err)
		}

		if !result.Success {
			printer.Error("Set light failed: %s", result.Message)
			return fmt.Errorf("set light failed")
		}

		printer.Success("White light set to %d%%", level)
		return nil
	},
}

// ============ device ir ============

var deviceIrCmd = &cobra.Command{
	Use:   "ir <0-100>",
	Short: "Control IR LED brightness (0=off, 100=max)",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		level, err := strconv.ParseUint(args[0], 10, 32)
		if err != nil || level > 100 {
			return fmt.Errorf("invalid level: use 0-100")
		}

		if err := connectDeviceControl(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		result, err := grpcCli.DeviceControl.SetIrLed(ctx, &devicepb.LightLevelRequest{
			Level: uint32(level),
		})
		if err != nil {
			return fmt.Errorf("failed to set IR LED: %w", err)
		}

		if !result.Success {
			printer.Error("Set IR LED failed: %s", result.Message)
			return fmt.Errorf("set IR LED failed")
		}

		if level == 0 {
			printer.Success("IR LED turned off")
		} else {
			printer.Success("IR LED set to %d%%", level)
		}
		return nil
	},
}

// ============ device ircut ============

var deviceIrCutCmd = &cobra.Command{
	Use:   "ircut <auto|day|night>",
	Short: "Set IR-Cut filter mode",
	Args:  cobra.ExactArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		var mode devicepb.IrCutMode
		switch strings.ToLower(args[0]) {
		case "auto":
			mode = devicepb.IrCutMode_IRCUT_AUTO
		case "day":
			mode = devicepb.IrCutMode_IRCUT_DAY
		case "night":
			mode = devicepb.IrCutMode_IRCUT_NIGHT
		default:
			return fmt.Errorf("invalid mode: use 'auto', 'day', or 'night'")
		}

		if err := connectDeviceControl(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		result, err := grpcCli.DeviceControl.SetIrCut(ctx, &devicepb.IrCutRequest{
			Mode: mode,
		})
		if err != nil {
			return fmt.Errorf("failed to set IR-Cut: %w", err)
		}

		if !result.Success {
			printer.Error("Set IR-Cut failed: %s", result.Message)
			return fmt.Errorf("set IR-Cut failed")
		}

		printer.Success("IR-Cut mode set to %s", args[0])
		return nil
	},
}

// ============ device ptz ============

var (
	ptzSpeed uint32
)

var devicePtzCmd = &cobra.Command{
	Use:   "ptz <action>",
	Short: "PTZ control",
	Long: `PTZ control commands.

Actions:
  left [speed]    - Pan left (0-100)
  right [speed]   - Pan right (0-100)
  up [speed]      - Tilt up (0-100)
  down [speed]    - Tilt down (0-100)
  stop            - Stop all PTZ movement
  preset <id>     - Go to preset position (1-255)
  save <id>       - Save current position as preset

Examples:
  aipc-cli device ptz left 50
  aipc-cli device ptz stop
  aipc-cli device ptz preset 1
`,
	Args: cobra.MinimumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectDeviceControl(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		action := strings.ToLower(args[0])
		var result *devicepb.Status
		var err error

		switch action {
		case "left":
			speed := ptzSpeed
			if len(args) > 1 {
				fmt.Sscanf(args[1], "%d", &speed)
			}
			result, err = grpcCli.DeviceControl.Pan(ctx, &devicepb.PanRequest{
				Direction: devicepb.PanDirection_PAN_LEFT,
				Speed:     speed,
			})
		case "right":
			speed := ptzSpeed
			if len(args) > 1 {
				fmt.Sscanf(args[1], "%d", &speed)
			}
			result, err = grpcCli.DeviceControl.Pan(ctx, &devicepb.PanRequest{
				Direction: devicepb.PanDirection_PAN_RIGHT,
				Speed:     speed,
			})
		case "up":
			speed := ptzSpeed
			if len(args) > 1 {
				fmt.Sscanf(args[1], "%d", &speed)
			}
			result, err = grpcCli.DeviceControl.Tilt(ctx, &devicepb.TiltRequest{
				Direction: devicepb.TiltDirection_TILT_UP,
				Speed:     speed,
			})
		case "down":
			speed := ptzSpeed
			if len(args) > 1 {
				fmt.Sscanf(args[1], "%d", &speed)
			}
			result, err = grpcCli.DeviceControl.Tilt(ctx, &devicepb.TiltRequest{
				Direction: devicepb.TiltDirection_TILT_DOWN,
				Speed:     speed,
			})
		case "stop":
			result, err = grpcCli.DeviceControl.PTZStop(ctx, &devicepb.PTZStopRequest{})
		case "preset":
			if len(args) < 2 {
				return fmt.Errorf("preset ID required")
			}
			var presetID uint32
			fmt.Sscanf(args[1], "%d", &presetID)
			if presetID == 0 || presetID > 255 {
				return fmt.Errorf("preset ID must be 1-255")
			}
			result, err = grpcCli.DeviceControl.CallPreset(ctx, &devicepb.PresetRequest{
				PresetId: presetID,
			})
		case "save":
			if len(args) < 2 {
				return fmt.Errorf("preset ID required")
			}
			var presetID uint32
			fmt.Sscanf(args[1], "%d", &presetID)
			if presetID == 0 || presetID > 255 {
				return fmt.Errorf("preset ID must be 1-255")
			}
			result, err = grpcCli.DeviceControl.SavePreset(ctx, &devicepb.PresetRequest{
				PresetId: presetID,
			})
		default:
			return fmt.Errorf("unknown action: %s", action)
		}

		if err != nil {
			return fmt.Errorf("PTZ command failed: %w", err)
		}

		if !result.Success {
			printer.Error("PTZ command failed: %s", result.Message)
			return fmt.Errorf("PTZ command failed")
		}

		printer.Success("PTZ %s executed", action)
		return nil
	},
}

// ============ device zoom ============

var deviceZoomCmd = &cobra.Command{
	Use:   "zoom <in|out|stop> [speed]",
	Short: "Control zoom",
	Long: `Control camera zoom.

Arguments:
  in [speed]   - Zoom in (speed: 1-100)
  out [speed]  - Zoom out (speed: 1-100)
  stop         - Stop zooming

Examples:
  aipc-cli device zoom in 50
  aipc-cli device zoom out
  aipc-cli device zoom stop
`,
	Args: cobra.MinimumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		action := strings.ToLower(args[0])
		var speed int32

		switch action {
		case "in":
			speed = 50
			if len(args) > 1 {
				fmt.Sscanf(args[1], "%d", &speed)
			}
		case "out":
			speed = -50
			if len(args) > 1 {
				var s int32
				fmt.Sscanf(args[1], "%d", &s)
				speed = -s
			}
		case "stop":
			speed = 0
		default:
			return fmt.Errorf("invalid action: use 'in', 'out', or 'stop'")
		}

		if err := connectDeviceControl(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		result, err := grpcCli.DeviceControl.Zoom(ctx, &devicepb.ZoomRequest{
			Speed: speed,
		})
		if err != nil {
			return fmt.Errorf("zoom failed: %w", err)
		}

		if !result.Success {
			printer.Error("Zoom failed: %s", result.Message)
			return fmt.Errorf("zoom failed")
		}

		printer.Success("Zoom %s", action)
		return nil
	},
}

// ============ device focus ============

var deviceFocusCmd = &cobra.Command{
	Use:   "focus <near|far|auto|stop> [speed]",
	Short: "Control focus",
	Long: `Control camera focus.

Arguments:
  near [speed]  - Focus near (speed: 1-100)
  far [speed]   - Focus far (speed: 1-100)
  auto          - Enable autofocus
  manual        - Disable autofocus
  stop          - Stop manual focus

Examples:
  aipc-cli device focus near 30
  aipc-cli device focus auto
`,
	Args: cobra.MinimumNArgs(1),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectDeviceControl(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		action := strings.ToLower(args[0])
		var result *devicepb.Status
		var err error

		switch action {
		case "near":
			speed := int32(-50)
			if len(args) > 1 {
				var s int32
				fmt.Sscanf(args[1], "%d", &s)
				speed = -s
			}
			result, err = grpcCli.DeviceControl.Focus(ctx, &devicepb.FocusRequest{Speed: speed})
		case "far":
			speed := int32(50)
			if len(args) > 1 {
				fmt.Sscanf(args[1], "%d", &speed)
			}
			result, err = grpcCli.DeviceControl.Focus(ctx, &devicepb.FocusRequest{Speed: speed})
		case "stop":
			result, err = grpcCli.DeviceControl.Focus(ctx, &devicepb.FocusRequest{Speed: 0})
		case "auto":
			result, err = grpcCli.DeviceControl.SetAutofocus(ctx, &devicepb.AutofocusRequest{Enable: true})
		case "manual":
			result, err = grpcCli.DeviceControl.SetAutofocus(ctx, &devicepb.AutofocusRequest{Enable: false})
		default:
			return fmt.Errorf("invalid action: use 'near', 'far', 'auto', 'manual', or 'stop'")
		}

		if err != nil {
			return fmt.Errorf("focus command failed: %w", err)
		}

		if !result.Success {
			printer.Error("Focus command failed: %s", result.Message)
			return fmt.Errorf("focus command failed")
		}

		printer.Success("Focus %s", action)
		return nil
	},
}

// ============ device gpio ============

var deviceGpioCmd = &cobra.Command{
	Use:   "gpio <read|write> <pin> [value]",
	Short: "GPIO control",
	Long: `Read or write GPIO pins.

Arguments:
  read <pin>        - Read GPIO pin value
  write <pin> <0|1> - Write GPIO pin value

Examples:
  aipc-cli device gpio read 5
  aipc-cli device gpio write 5 1
`,
	Args: cobra.MinimumNArgs(2),
	RunE: func(cmd *cobra.Command, args []string) error {
		if err := connectDeviceControl(); err != nil {
			return err
		}

		ctx, cancel := context.WithTimeout(context.Background(), cfg.GRPC.Timeout)
		defer cancel()

		action := strings.ToLower(args[0])
		var pin uint32
		fmt.Sscanf(args[1], "%d", &pin)

		switch action {
		case "read":
			resp, err := grpcCli.DeviceControl.GPIORead(ctx, &devicepb.GPIOReadRequest{
				Pin: pin,
			})
			if err != nil {
				return fmt.Errorf("GPIO read failed: %w", err)
			}
			if resp.Status != nil && !resp.Status.Success {
				printer.Error("GPIO read failed: %s", resp.Status.Message)
				return fmt.Errorf("GPIO read failed")
			}

			if outputFmt == "json" || outputFmt == "yaml" {
				return printer.Print(map[string]interface{}{
					"pin":   resp.Pin,
					"value": resp.Value,
				})
			}

			value := "LOW"
			if resp.Value {
				value = "HIGH"
			}
			printer.Printf("GPIO %d: %s\n", pin, value)

		case "write":
			if len(args) < 3 {
				return fmt.Errorf("value required for write")
			}
			var val uint32
			fmt.Sscanf(args[2], "%d", &val)
			value := val != 0

			result, err := grpcCli.DeviceControl.GPIOWrite(ctx, &devicepb.GPIOWriteRequest{
				Pin:   pin,
				Value: value,
			})
			if err != nil {
				return fmt.Errorf("GPIO write failed: %w", err)
			}
			if !result.Success {
				printer.Error("GPIO write failed: %s", result.Message)
				return fmt.Errorf("GPIO write failed")
			}

			valStr := "LOW"
			if value {
				valStr = "HIGH"
			}
			printer.Success("GPIO %d set to %s", pin, valStr)

		default:
			return fmt.Errorf("invalid action: use 'read' or 'write'")
		}

		return nil
	},
}

// ============ Helper functions ============

func formatBool(b bool) string {
	if b {
		return "On"
	}
	return "Off"
}

func formatIrCutMode(mode devicepb.IrCutMode) string {
	switch mode {
	case devicepb.IrCutMode_IRCUT_AUTO:
		return "Auto"
	case devicepb.IrCutMode_IRCUT_DAY:
		return "Day"
	case devicepb.IrCutMode_IRCUT_NIGHT:
		return "Night"
	default:
		return "Unknown"
	}
}

func protoDeviceStatusToMap(s *devicepb.DeviceStatus) map[string]interface{} {
	return map[string]interface{}{
		"soc_temp_c":        s.SocTempC,
		"mcu_temp_c":        s.McuTempC,
		"light_sensor":      s.LightSensor,
		"ptz_pan_pos":       s.PtzPanPos,
		"ptz_tilt_pos":      s.PtzTiltPos,
		"zoom_pos":          s.ZoomPos,
		"focus_pos":         s.FocusPos,
		"autofocus_enabled": s.AutofocusEnabled,
		"ircut_mode":        formatIrCutMode(s.IrcutMode),
		"white_light_level": s.WhiteLightLevel,
		"ir_led_level":      s.IrLedLevel,
		"mcu_version":       s.McuVersion,
		"mcu_uptime_ms":     s.McuUptimeMs,
	}
}

func init() {
	// ptz flags
	devicePtzCmd.Flags().Uint32Var(&ptzSpeed, "speed", 50, "Default PTZ speed (0-100)")

	// Register subcommands
	deviceCmd.AddCommand(deviceStatusCmd)
	deviceCmd.AddCommand(deviceLightCmd)
	deviceCmd.AddCommand(deviceIrCmd)
	deviceCmd.AddCommand(deviceIrCutCmd)
	deviceCmd.AddCommand(devicePtzCmd)
	deviceCmd.AddCommand(deviceZoomCmd)
	deviceCmd.AddCommand(deviceFocusCmd)
	deviceCmd.AddCommand(deviceGpioCmd)
}
