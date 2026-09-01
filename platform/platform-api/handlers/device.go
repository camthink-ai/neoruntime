package handlers

import (
	"context"
	"encoding/base64"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"

	"aipc/platform/common/events"
	devicepb "aipc/platform/device-control/proto"
)

// Device Control handlers

func (h *APIHandlers) GetDeviceStatus(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	status, err := client.GetDeviceStatus(ctx, &devicepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	// Explicit field mapping to preserve proto3 zero-value fields
	// (white_light_level=0, ir_led_on=false, soc_temp_c=0, etc.)
	Resp(c).OK(gin.H{
		"soc_temp_c":        status.GetSocTempC(),
		"mcu_temp_c":        status.GetMcuTempC(),
		"light_sensor":      status.GetLightSensor(),
		"ptz_pan_pos":       status.GetPtzPanPos(),
		"ptz_tilt_pos":      status.GetPtzTiltPos(),
		"zoom_pos":          status.GetZoomPos(),
		"focus_pos":         status.GetFocusPos(),
		"autofocus_enabled": status.GetAutofocusEnabled(),
		"ircut_mode":        status.GetIrcutMode(),
		"white_light_level": status.GetWhiteLightLevel(),
		"ir_led_level":      status.GetIrLedLevel(),
		"mcu_version":       status.GetMcuVersion(),
		"mcu_uptime_ms":     status.GetMcuUptimeMs(),
	})
}

func (h *APIHandlers) SetLight(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Level *uint32 `json:"level" binding:"required,min=0,max=100"` // 0-100
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.SetWhiteLight(ctx, &devicepb.LightLevelRequest{
		Level: *req.Level,
	})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.control", events.MessageParams{"device": "light", "action": "set_level", "level": *req.Level}, getUsernameFromContext(c))
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) SetIrLed(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Level *uint32 `json:"level" binding:"required,min=0,max=100"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.SetIrLed(ctx, &devicepb.LightLevelRequest{
		Level: *req.Level,
	})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.control", events.MessageParams{"device": "ir_led", "action": "set_level", "level": *req.Level}, getUsernameFromContext(c))
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) SetIrCut(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Mode string `json:"mode" binding:"required"` // "auto", "day", "night"
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	var mode devicepb.IrCutMode
	switch strings.ToLower(req.Mode) {
	case "auto":
		mode = devicepb.IrCutMode_IRCUT_AUTO
	case "day":
		mode = devicepb.IrCutMode_IRCUT_DAY
	case "night":
		mode = devicepb.IrCutMode_IRCUT_NIGHT
	default:
		Resp(c).FailMsg(CodeInvalidRequest, "Mode must be 'auto', 'day', or 'night'")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.SetIrCut(ctx, &devicepb.IrCutRequest{
		Mode: mode,
	})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.control", events.MessageParams{"device": "ircut", "action": "set_mode", "mode": req.Mode}, getUsernameFromContext(c))
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) ControlPTZ(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Action string `json:"action"` // "pan", "tilt", "stop", "preset", "zoom", "focus"
		// Pan/Tilt
		Direction string `json:"direction,omitempty"` // "left", "right", "up", "down", "stop"
		Speed     uint32 `json:"speed,omitempty"`     // 0-100
		// Preset
		PresetID uint32 `json:"preset_id,omitempty"` // 1-255
		// Zoom/Focus
		ZoomSpeed  int32 `json:"zoom_speed,omitempty"`  // -100 ~ 100
		FocusSpeed int32 `json:"focus_speed,omitempty"` // -100 ~ 100
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	var resp *devicepb.Status
	var err error

	switch req.Action {
	case "pan":
		var dir devicepb.PanDirection
		switch strings.ToLower(req.Direction) {
		case "left":
			dir = devicepb.PanDirection_PAN_LEFT
		case "right":
			dir = devicepb.PanDirection_PAN_RIGHT
		default:
			dir = devicepb.PanDirection_PAN_STOP
		}
		resp, err = client.Pan(ctx, &devicepb.PanRequest{
			Direction: dir,
			Speed:     req.Speed,
		})
	case "tilt":
		var dir devicepb.TiltDirection
		switch strings.ToLower(req.Direction) {
		case "up":
			dir = devicepb.TiltDirection_TILT_UP
		case "down":
			dir = devicepb.TiltDirection_TILT_DOWN
		default:
			dir = devicepb.TiltDirection_TILT_STOP
		}
		resp, err = client.Tilt(ctx, &devicepb.TiltRequest{
			Direction: dir,
			Speed:     req.Speed,
		})
	case "stop":
		resp, err = client.PTZStop(ctx, &devicepb.PTZStopRequest{})
	case "preset":
		if req.PresetID == 0 || req.PresetID > 255 {
			Resp(c).FailMsg(CodeInvalidRequest, "Preset ID must be between 1 and 255")
			return
		}
		resp, err = client.CallPreset(ctx, &devicepb.PresetRequest{
			PresetId: req.PresetID,
		})
	case "zoom":
		resp, err = client.Zoom(ctx, &devicepb.ZoomRequest{
			Speed: req.ZoomSpeed,
		})
	case "focus":
		resp, err = client.Focus(ctx, &devicepb.FocusRequest{
			Speed: req.FocusSpeed,
		})
	default:
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid action. Must be 'pan', 'tilt', 'stop', 'preset', 'zoom', or 'focus'")
		return
	}

	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.ptz.control", events.MessageParams{"action": req.Action, "direction": req.Direction, "speed": req.Speed}, getUsernameFromContext(c))
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) ControlZoom(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Speed int32 `json:"speed" binding:"required,min=-100,max=100"` // -100 ~ 100 (negative: zoom out, positive: zoom in)
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.Zoom(ctx, &devicepb.ZoomRequest{
		Speed: req.Speed,
	})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.zoom.changed", events.MessageParams{"speed": req.Speed}, getUsernameFromContext(c))
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) ControlFocus(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Speed int32 `json:"speed" binding:"required,min=-100,max=100"` // -100 ~ 100 (negative: near, positive: far)
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.Focus(ctx, &devicepb.FocusRequest{
		Speed: req.Speed,
	})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.focus.changed", events.MessageParams{"speed": req.Speed}, getUsernameFromContext(c))
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) SetAutofocus(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Enable *bool `json:"enable" binding:"required"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()

	resp, err := client.SetAutofocus(ctx, &devicepb.AutofocusRequest{
		Enable: *req.Enable,
	})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.autofocus.changed", events.MessageParams{"enable": *req.Enable}, getUsernameFromContext(c))
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) OneshotAutofocus(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	resp, err := client.StartOneShotAf(ctx, &devicepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetAccepted() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.lens.oneshot_af", nil, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{
		"accepted": true,
		"job_id":   resp.GetJobId(),
		"message":  resp.GetMessage(),
	})
}

func (h *APIHandlers) StartZoomFollow(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}
	var req struct {
		Ratio float32 `json:"ratio" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil || req.Ratio <= 0 {
		Resp(c).FailMsg(CodeInvalidRequest, "ratio must be greater than zero")
		return
	}
	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	resp, err := client.StartZoomFollow(ctx, &devicepb.ZoomFollowRequest{Ratio: req.Ratio})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetAccepted() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}
	Resp(c).OK(gin.H{"accepted": true, "job_id": resp.GetJobId(), "message": resp.GetMessage()})
}

func (h *APIHandlers) GetAutofocusStatus(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}
	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	resp, err := client.GetAutofocusStatus(ctx, &devicepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	Resp(c).OK(gin.H{
		"job_id": resp.GetJobId(), "operation": resp.GetOperation(), "state": resp.GetState(),
		"progress": resp.GetProgress(), "busy": resp.GetBusy(), "anchor_valid": resp.GetAnchorValid(),
		"requested_ratio": resp.GetRequestedRatio(), "effective_ratio": resp.GetEffectiveRatio(),
		"zoom_pos": resp.GetZoomPos(), "focus_pos": resp.GetFocusPos(), "best_focus": resp.GetBestFocus(),
		"metric": resp.GetMetric(), "confidence": resp.GetConfidence(),
		"reproducibility": resp.GetReproducibility(), "estimated_distance_m": resp.GetEstimatedDistanceM(),
		"elapsed_ms": resp.GetElapsedMs(), "error_code": resp.GetErrorCode(), "message": resp.GetMessage(),
	})
}

func (h *APIHandlers) CancelAutofocus(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}
	var req struct {
		JobID uint64 `json:"job_id"`
	}
	_ = c.ShouldBindJSON(&req)
	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	resp, err := client.CancelAutofocus(ctx, &devicepb.AfJobRequest{JobId: req.JobID})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}
	Resp(c).OK(resp)
}

func (h *APIHandlers) GPIOWrite(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Pin   *uint32 `json:"pin" binding:"required"`
		Value *bool   `json:"value" binding:"required"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GPIOWrite(ctx, &devicepb.GPIOWriteRequest{
		Pin:   *req.Pin,
		Value: *req.Value,
	})
	if err != nil {
		// Mirror GPIORead: an out-of-catalog pin is a 404, not a device error.
		if status.Code(err) == codes.NotFound {
			Resp(c).FailMsg(CodeNotFound, status.Convert(err).Message())
			return
		}
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.gpio.write", events.MessageParams{"pin": *req.Pin, "value": *req.Value}, getUsernameFromContext(c))
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) GPIORead(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	pinStr := c.Param("pin")
	if pinStr == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Pin number is required")
		return
	}

	pin, err := strconv.ParseUint(pinStr, 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid pin number")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GPIORead(ctx, &devicepb.GPIOReadRequest{
		Pin: uint32(pin),
	})
	if err != nil {
		if status.Code(err) == codes.NotFound {
			Resp(c).FailMsg(CodeNotFound, status.Convert(err).Message())
			return
		}
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	Resp(c).OK(resp)
}

// GPIOBatchRead returns the GPIO pin catalog (from device-control config) plus
// the live value of each pin. Optional ?pins=12,21 restricts to a subset; omit
// to read every configured available pin.
func (h *APIHandlers) GPIOBatchRead(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var pins []uint32
	if pinsStr := strings.TrimSpace(c.Query("pins")); pinsStr != "" {
		for _, p := range strings.Split(pinsStr, ",") {
			p = strings.TrimSpace(p)
			if p == "" {
				continue
			}
			v, err := strconv.ParseUint(p, 10, 32)
			if err != nil {
				Resp(c).FailMsg(CodeInvalidRequest, "Invalid pin number: "+p)
				return
			}
			pins = append(pins, uint32(v))
		}
		if len(pins) > 64 {
			Resp(c).FailMsg(CodeInvalidRequest, "Too many pins (max 64)")
			return
		}
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GPIOBatchRead(ctx, &devicepb.GPIOBatchReadRequest{
		Pins: pins,
	})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	Resp(c).OK(resp)
}

// ── Lens extended APIs ────────────────────────────────────

func (h *APIHandlers) GetLensStatus(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GetLensStatus(ctx, &devicepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	// Return explicit fields (including zero values like NO_CFG=0) to avoid
	// proto3 omitempty hiding critical status after reset-zero failures.
	Resp(c).OK(gin.H{
		"zoom_state":        resp.GetZoomState(),
		"focus_state":       resp.GetFocusState(),
		"zoom_rz_done":      resp.GetZoomRzDone(),
		"focus_rz_done":     resp.GetFocusRzDone(),
		"zoom_pos":          resp.GetZoomPos(),
		"focus_pos":         resp.GetFocusPos(),
		"iris_adc":          resp.GetIrisAdc(),
		"autofocus_enabled": resp.GetAutofocusEnabled(),
		"zoom_limit": gin.H{
			"min_pos": resp.GetZoomLimit().GetMinPos(),
			"max_pos": resp.GetZoomLimit().GetMaxPos(),
		},
		"focus_limit": gin.H{
			"min_pos": resp.GetFocusLimit().GetMinPos(),
			"max_pos": resp.GetFocusLimit().GetMaxPos(),
		},
	})
}

func (h *APIHandlers) SetZoomLevel(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Level float32 `json:"level"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()

	resp, err := client.SetZoomLevel(ctx, &devicepb.ZoomLevelRequest{Level: req.Level})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}

	Resp(c).OK(resp)
}
func (h *APIHandlers) SetFocusLevel(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Level float32 `json:"level"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()

	resp, err := client.SetFocusLevel(ctx, &devicepb.FocusLevelRequest{Level: req.Level})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) LensResetZero(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Zoom  bool `json:"zoom"`
		Focus bool `json:"focus"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()

	resp, err := client.LensResetZero(ctx, &devicepb.LensResetRequest{Zoom: req.Zoom, Focus: req.Focus})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) ControlIris(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Speed int32 `json:"speed"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.ControlIris(ctx, &devicepb.IrisRequest{Speed: req.Speed})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) SetIrisTarget(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Target uint32 `json:"target"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.SetIrisTarget(ctx, &devicepb.IrisTargetRequest{Target: req.Target})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) SetLensLimits(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		ZoomLimit  *devicepb.LensLimit `json:"zoom_limit"`
		FocusLimit *devicepb.LensLimit `json:"focus_limit"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.SetLensLimits(ctx, &devicepb.LensLimitsRequest{
		ZoomLimit:  req.ZoomLimit,
		FocusLimit: req.FocusLimit,
	})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) LensInit(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()

	resp, err := client.LensInit(ctx, &devicepb.LensInitRequest{})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.lens.init", events.MessageParams{"action": "lens_init"}, getUsernameFromContext(c))
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) LensGotoRatioDistance(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		ZoomRatio      float32 `json:"zoom_ratio"`
		FocusDistanceM float32 `json:"focus_distance_m"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 120*time.Second)
	defer cancel()

	resp, err := client.LensGotoRatioDistance(ctx, &devicepb.GotoRatioDistanceRequest{
		ZoomRatio:      req.ZoomRatio,
		FocusDistanceM: req.FocusDistanceM,
	})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeDeviceError, resp.GetMessage())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.lens.goto", events.MessageParams{"zoom_ratio": req.ZoomRatio, "focus_distance_m": req.FocusDistanceM}, getUsernameFromContext(c))
	}

	Resp(c).OK(resp)
}

// ── Peripheral control APIs (fan, heat, radar, alarm, wiegand, RS485) ─────

func (h *APIHandlers) SetFan(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Enable *bool `json:"enable" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.SetFan(ctx, &devicepb.EnvCtrlRequest{Enable: *req.Enable})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.peripheral", events.MessageParams{"device": "fan", "action": "set", "enable": *req.Enable}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"success": resp.Success, "enabled": resp.Enabled, "message": resp.Message})
}

func (h *APIHandlers) GetFan(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GetFan(ctx, &devicepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"success": resp.Success, "enabled": resp.Enabled, "message": resp.Message})
}

func (h *APIHandlers) SetHeat(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Enable *bool `json:"enable" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.SetHeat(ctx, &devicepb.EnvCtrlRequest{Enable: *req.Enable})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.peripheral", events.MessageParams{"device": "heat", "action": "set", "enable": *req.Enable}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"success": resp.Success, "enabled": resp.Enabled, "message": resp.Message})
}

func (h *APIHandlers) GetHeat(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GetHeat(ctx, &devicepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"success": resp.Success, "enabled": resp.Enabled, "message": resp.Message})
}

func (h *APIHandlers) SetRadar(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Enable *bool `json:"enable" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.SetRadar(ctx, &devicepb.EnvCtrlRequest{Enable: *req.Enable})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.peripheral", events.MessageParams{"device": "radar", "action": "set", "enable": *req.Enable}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"success": resp.Success, "enabled": resp.Enabled, "message": resp.Message})
}

func (h *APIHandlers) GetRadar(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GetRadar(ctx, &devicepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"success": resp.Success, "enabled": resp.Enabled, "message": resp.Message})
}

func (h *APIHandlers) SetAlarmOut(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Channel uint32 `json:"channel"`
		Enable  *bool  `json:"enable" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.SetAlarmOut(ctx, &devicepb.AlarmChannelRequest{Channel: req.Channel, Enable: *req.Enable})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.peripheral", events.MessageParams{"device": "alarm_out", "action": "set", "channel": req.Channel, "enable": *req.Enable}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"success": resp.Success, "enabled": resp.Enabled, "message": resp.Message})
}

func (h *APIHandlers) GetAlarmOut(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	channel, err := strconv.ParseUint(c.Param("channel"), 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid channel number")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GetAlarmOut(ctx, &devicepb.AlarmChannelRequest{Channel: uint32(channel)})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"success": resp.Success, "enabled": resp.Enabled, "message": resp.Message})
}

func (h *APIHandlers) SetWiegand(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Channel uint32 `json:"channel"`
		Enable  *bool  `json:"enable" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.SetWiegandOut(ctx, &devicepb.AlarmChannelRequest{Channel: req.Channel, Enable: *req.Enable})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.peripheral", events.MessageParams{"device": "wiegand", "action": "set", "channel": req.Channel, "enable": *req.Enable}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"success": resp.Success, "enabled": resp.Enabled, "message": resp.Message})
}

func (h *APIHandlers) GetWiegand(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	channel, err := strconv.ParseUint(c.Param("channel"), 10, 32)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid channel number")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GetWiegandOut(ctx, &devicepb.AlarmChannelRequest{Channel: uint32(channel)})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"success": resp.Success, "enabled": resp.Enabled, "message": resp.Message})
}

func (h *APIHandlers) GetAlarmOutputs(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GetAlarmOutputs(ctx, &devicepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"success":    resp.Success,
		"message":    resp.Message,
		"alarm_out0": resp.AlarmOut0,
		"alarm_out1": resp.AlarmOut1,
		"wiegand0":   resp.Wiegand0,
		"wiegand1":   resp.Wiegand1,
	})
}

func (h *APIHandlers) Rs485Init(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Baudrate uint32 `json:"baudrate"`
		Config   string `json:"config"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.Rs485Init(ctx, &devicepb.Rs485InitRequest{Baudrate: req.Baudrate, Config: req.Config})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.peripheral", events.MessageParams{"device": "rs485", "action": "init", "baudrate": req.Baudrate}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"success": resp.Success, "message": resp.Message})
}

func (h *APIHandlers) Rs485Deinit(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.Rs485Deinit(ctx, &devicepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.peripheral", events.MessageParams{"device": "rs485", "action": "deinit"}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"success": resp.Success, "message": resp.Message})
}

func (h *APIHandlers) Rs485Tx(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	var req struct {
		Data string `json:"data"` // base64-encoded
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	data, err := base64.StdEncoding.DecodeString(req.Data)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid base64 data: "+err.Error())
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.Rs485Tx(ctx, &devicepb.Rs485TxRequest{Data: data})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.peripheral", events.MessageParams{"device": "rs485", "action": "tx", "bytes": len(data)}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"success": resp.Success, "message": resp.Message})
}

func (h *APIHandlers) GetDeviceCapabilities(c *gin.Context) {
	if h.grpcClients.DeviceControl == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Device Control not available")
		return
	}

	client := devicepb.NewDeviceControlClient(h.grpcClients.DeviceControl)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GetCapabilities(ctx, &devicepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeDeviceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"has_video":    resp.HasVideo,
		"has_codec":    resp.HasCodec,
		"has_led":      resp.HasLed,
		"has_sensor":   resp.HasSensor,
		"has_mcu":      resp.HasMcu,
		"has_env_ctrl": resp.HasEnvCtrl,
		"has_alarm":    resp.HasAlarm,
		"has_rs485":    resp.HasRs485,
		"has_osd":      resp.HasOsd,
		"has_draw":     resp.HasDraw,
		"has_audio":    resp.HasAudio,
	})
}
