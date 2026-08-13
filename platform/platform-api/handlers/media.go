package handlers

import (
	"aipc/platform/common/constants"
	"context"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"reflect"
	"strconv"
	"strings"
	"sync"
	"time"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"
	"gopkg.in/yaml.v3"

	camerapb "aipc/platform/camera-daemon/proto"
	eventLoggerPkg "aipc/platform/common/events"
	"aipc/platform/common/logger"
	"aipc/platform/platform-api/config"
)

// StreamReloader is implemented by StreamHandlers to support hot-reload after gRPC reconfigure.
type StreamReloader interface {
	ReloadStreams(configPath string)
	UpdateStreamFromApplied(streamID string, width, height uint32, codec string, bitrate, fps, gop uint32)
	RestartH264Stream(streamID string)
	ForceKeyframe(streamID string)
}

// MediaHandlers handles media configurations
type MediaHandlers struct {
	configPath     string
	cameraClient   *grpc.ClientConn
	eventLogger    *eventLoggerPkg.Logger
	streamReloader StreamReloader
	configMu       sync.Mutex
	configMgr      *config.Manager
}

func NewMediaHandlers(configPath string, cameraClient *grpc.ClientConn, eventLogger *eventLoggerPkg.Logger, configMgr *config.Manager) *MediaHandlers {
	if configPath == "" {
		configPath = constants.ConfigPath() + "/camera-daemon.yaml"
	}
	return &MediaHandlers{
		configPath:   configPath,
		cameraClient: cameraClient,
		eventLogger:  eventLogger,
		configMgr:    configMgr,
	}
}

// SetStreamReloader sets the stream reloader for hot-reload after gRPC reconfigure.
func (h *MediaHandlers) SetStreamReloader(r StreamReloader) {
	h.streamReloader = r
}

// SetEventLogger sets the event logger (for dependency injection)
func (h *MediaHandlers) SetEventLogger(logger *eventLoggerPkg.Logger) {
	h.eventLogger = logger
}

// projectMediaConfig persists the marshaled camera-daemon.yaml. It is the single
// write chokepoint for every per-field media edit (SetConfig and the stream
// hot-reload / RTSP / AI-overlay / add / remove / toggle helpers), all of which
// hold h.configMu while calling it.
//
// It first takes configApplyMu — the shared apply lock that also guards the
// multi-file media/bundle import (applyImportedMediaConfig) and the device-clone
// file apply (applyTree). That serialization is what stops a clone restore from
// overwriting /data/aipc/etc mid-edit while a media write is in flight (or vice
// versa) — the P0 race. Lock order is configMu → configApplyMu (media edits) and
// configApplyMu alone (clone); the two never form a cycle.
//
// Callers that ALREADY hold configApplyMu (only applyImportedMediaConfig) must
// call projectMediaConfigLocked instead to avoid reentrant self-deadlock —
// sync.Mutex is not reentrant.
//
// Persist itself routes through the Config Controller when available (atomic
// write + read-back verify + auto-restore + revision/audit), falling back to a
// direct os.WriteFile when the Manager is nil or the apply fails. yamlStr is the
// full YAML the handler already produced via yaml.Marshal of its config map —
// passing it verbatim avoids a YAML→JSON→YAML round-trip that would drift
// integer formatting. The actor is the HTTP user for entry-point callers and ""
// for the internal fire-and-forget helpers (which are invoked from goroutines
// where the gin context is unsafe to capture).
func (h *MediaHandlers) projectMediaConfig(ctx context.Context, actor, yamlStr string) error {
	configApplyMu.Lock()
	defer configApplyMu.Unlock()
	return h.projectMediaConfigLocked(ctx, actor, yamlStr)
}

// projectMediaConfigLocked is projectMediaConfig without the configApplyMu lock.
// Precondition: the caller already holds configApplyMu. See projectMediaConfig
// for the full contract and why this split exists.
func (h *MediaHandlers) projectMediaConfigLocked(ctx context.Context, actor, yamlStr string) error {
	if h.configMgr != nil {
		if _, _, err := h.configMgr.Apply(ctx, "media", "config", yamlStr, actor); err != nil {
			logger.Warn("media manager apply failed, falling back to direct write: %v", err)
		} else {
			return nil
		}
	}
	return os.WriteFile(h.configPath, []byte(yamlStr), 0644)
}

// cleanMaps converts map[interface{}]interface{} to map[string]interface{} recursively.
func cleanMaps(in interface{}) interface{} {
	switch v := in.(type) {
	case []interface{}:
		res := make([]interface{}, len(v))
		for i, val := range v {
			res[i] = cleanMaps(val)
		}
		return res
	case map[interface{}]interface{}:
		res := make(map[string]interface{})
		for key, val := range v {
			res[fmt.Sprintf("%v", key)] = cleanMaps(val)
		}
		return res
	case map[string]interface{}:
		res := make(map[string]interface{})
		for key, val := range v {
			res[key] = cleanMaps(val)
		}
		return res
	default:
		return in
	}
}

func clampOsdUnit(v float32) float32 {
	if v != v || v < 0 {
		return 0
	}
	if v > 1 {
		return 1
	}
	return v
}

func validOsdExtent(v float32) bool {
	return v == v && v > 0 && v <= 1
}

func validOsdImagePath(path string) (string, bool) {
	cleaned := filepath.Clean(strings.TrimSpace(path))
	if cleaned == "." || cleaned == string(filepath.Separator) {
		return "", false
	}
	ext := strings.ToLower(filepath.Ext(cleaned))
	if ext != ".png" && ext != ".bmp" {
		return "", false
	}
	osdDir := filepath.Clean(constants.RootPath() + "/etc/osd")
	if cleaned == osdDir || !strings.HasPrefix(cleaned, osdDir+string(filepath.Separator)) {
		return "", false
	}
	info, err := os.Stat(cleaned)
	if err != nil || info.IsDir() {
		return "", false
	}
	return cleaned, true
}

// deepMerge recursively merges src into dst.
func deepMerge(dst, src map[string]interface{}) {
	for k, v := range src {
		if srcMap, ok := v.(map[string]interface{}); ok {
			if dstMap, ok := dst[k].(map[string]interface{}); ok {
				deepMerge(dstMap, srcMap)
				continue
			}
		}
		dst[k] = v
	}
}

func (h *MediaHandlers) GetConfig(c *gin.Context) {
	data, err := os.ReadFile(h.configPath)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to read config: "+err.Error())
		return
	}

	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to parse config: "+err.Error())
		return
	}

	// Inject RTSP URL into the rtsp section.
	// Use the Host header from the request — that's the IP the browser used
	// to reach the device, which is the same IP RTSP clients should use.
	if rtsp, ok := config["rtsp"].(map[string]interface{}); ok {
		rtsp["url"] = fmt.Sprintf("rtsp://%s:8554", getHostFromRequest(c))
	}

	cleanConfig := cleanMaps(config)
	Resp(c).OK(cleanConfig)
}

func (h *MediaHandlers) SetConfig(c *gin.Context) {
	if !requireJSONContentType(c) {
		return
	}

	var req map[string]interface{}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body")
		return
	}

	h.configMu.Lock()
	defer h.configMu.Unlock()

	// Read existing config
	data, err := os.ReadFile(h.configPath)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to read config: "+err.Error())
		return
	}

	var oldConfig map[string]interface{}
	if err := yaml.Unmarshal(data, &oldConfig); err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to parse config: "+err.Error())
		return
	}
	if err := normalizeMediaConfigNumbers(oldConfig); err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", err.Error())
		return
	}

	// Snapshot old encoders for diff
	oldEncoders := extractEncoders(oldConfig)

	// Deep merge updates
	deepMerge(oldConfig, req)
	if err := normalizeMediaConfigNumbers(oldConfig); err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", err.Error())
		return
	}
	if err := validateMediaConfigEncoders(oldConfig); err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", err.Error())
		return
	}

	newEncoders := extractEncoders(oldConfig)

	// Apply encoder changes via gRPC first, only write YAML on success
	if h.cameraClient != nil {
		changed := diffEncoders(oldEncoders, newEncoders)
		if len(changed) > 0 {
			if err := h.applyEncoderChanges(changed); err != nil {
				Resp(c).FailMsg(CodeCameraError, "Failed to apply config: "+err.Error())
				return
			}
		}
	} else {
		// cameraClient unavailable: do NOT silently bounce camera-daemon from a
		// web request — each restart drops the in-memory lens bootstrap flag and
		// causes a needless lens re-home ("咔嚓") on the next lens op. Surface
		// the error. YAML write is intentionally skipped, matching the file's
		// own "write back only after gRPC success" convention above.
		Resp(c).FailMsg(CodeCameraError, "camera service unavailable; cannot apply encoder changes")
		return
	}

	// Write back to YAML only after gRPC success
	outData, err := marshalMediaConfig(oldConfig)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to encode config: "+err.Error())
		return
	}

	if err := h.projectMediaConfig(c.Request.Context(), getUsernameFromContext(c), string(outData)); err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to save config: "+err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.changed", eventLoggerPkg.MessageParams{"stream": "all", "changes": "video config updated"}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"message": "Configuration saved and applied"})
}

func marshalMediaConfig(config map[string]interface{}) ([]byte, error) {
	if err := normalizeMediaConfigNumbers(config); err != nil {
		return nil, err
	}
	return yaml.Marshal(config)
}

var encoderUintConfigFields = map[string]struct{}{
	"width": {}, "height": {}, "fps": {}, "bitrate": {}, "gop": {},
	"max_bitrate": {}, "qp_min": {}, "qp_max": {},
	"output_pool_max_buffers": {}, "max_queue_size": {},
}

var streamUintConfigFields = map[string]struct{}{
	"width": {}, "height": {}, "fps": {},
	"pool_max_buffers": {}, "max_queue_size": {},
}

var audioUintConfigFields = map[string]struct{}{
	"sample_rate": {}, "channels": {}, "bitrate": {},
}

var watchdogUintConfigFields = map[string]struct{}{
	"scan_interval_ms": {}, "frame_timeout_ms": {}, "warn_threshold_ms": {},
}

// normalizeMediaConfigNumbers converts integer-valued config fields decoded
// from JSON/YAML float values back to integers before yaml.Marshal. Without
// this, values such as 4032000 can be emitted as 4.032e+06 and older parsers
// may truncate them to 4.
func normalizeMediaConfigNumbers(config map[string]interface{}) error {
	if config == nil {
		return nil
	}
	if err := normalizeListUintFields(config, "encoders", encoderUintConfigFields); err != nil {
		return err
	}
	if err := normalizeListUintFields(config, "streams", streamUintConfigFields); err != nil {
		return err
	}
	if err := normalizeSectionUintFields(config, "audio", audioUintConfigFields); err != nil {
		return err
	}
	if err := normalizeSectionUintFields(config, "watchdog", watchdogUintConfigFields); err != nil {
		return err
	}
	if rtsp, ok := stringMap(config["rtsp"]); ok {
		if err := normalizeUintField(rtsp, "rtsp.port", "port", 65535); err != nil {
			return err
		}
	}
	if overlay, ok := stringMap(config["ai_overlay"]); ok {
		if err := normalizeUintField(overlay, "ai_overlay.box_thickness", "box_thickness", maxUint32ConfigValue); err != nil {
			return err
		}
	}
	return nil
}

func normalizeListUintFields(config map[string]interface{}, key string, fields map[string]struct{}) error {
	raw, ok := config[key]
	if !ok || raw == nil {
		return nil
	}
	list, ok := raw.([]interface{})
	if !ok {
		return nil
	}
	for i, item := range list {
		m, ok := stringMap(item)
		if !ok {
			continue
		}
		for field := range fields {
			path := fmt.Sprintf("%s[%d].%s", key, i, field)
			if err := normalizeUintField(m, path, field, maxUint32ConfigValue); err != nil {
				return err
			}
		}
	}
	return nil
}

func normalizeSectionUintFields(config map[string]interface{}, key string, fields map[string]struct{}) error {
	section, ok := stringMap(config[key])
	if !ok {
		return nil
	}
	for field := range fields {
		path := key + "." + field
		if err := normalizeUintField(section, path, field, maxUint32ConfigValue); err != nil {
			return err
		}
	}
	return nil
}

func normalizeUintField(m map[string]interface{}, path, key string, maxValue uint32) error {
	v, ok := m[key]
	if !ok || v == nil {
		return nil
	}
	n, err := configNumberToUint32(v, maxValue)
	if err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	m[key] = int(n)
	return nil
}

const maxUint32ConfigValue = uint32(1<<32 - 1)
const maxUint32ConfigFloat = float64(1<<32 - 1)

func configNumberToUint32(v interface{}, maxValue uint32) (uint32, error) {
	switch n := v.(type) {
	case int:
		return signedConfigNumberToUint32(int64(n), maxValue)
	case int8:
		return signedConfigNumberToUint32(int64(n), maxValue)
	case int16:
		return signedConfigNumberToUint32(int64(n), maxValue)
	case int32:
		return signedConfigNumberToUint32(int64(n), maxValue)
	case int64:
		return signedConfigNumberToUint32(n, maxValue)
	case uint:
		return unsignedConfigNumberToUint32(uint64(n), maxValue)
	case uint8:
		return unsignedConfigNumberToUint32(uint64(n), maxValue)
	case uint16:
		return unsignedConfigNumberToUint32(uint64(n), maxValue)
	case uint32:
		return unsignedConfigNumberToUint32(uint64(n), maxValue)
	case uint64:
		return unsignedConfigNumberToUint32(n, maxValue)
	case float32:
		return floatConfigNumberToUint32(float64(n), maxValue)
	case float64:
		return floatConfigNumberToUint32(n, maxValue)
	case string:
		s := strings.TrimSpace(n)
		if s == "" {
			return 0, fmt.Errorf("empty numeric value")
		}
		f, err := strconv.ParseFloat(s, 64)
		if err != nil {
			return 0, fmt.Errorf("invalid numeric value %q", n)
		}
		return floatConfigNumberToUint32(f, maxValue)
	default:
		return 0, fmt.Errorf("expected unsigned integer, got %T", v)
	}
}

func signedConfigNumberToUint32(n int64, maxValue uint32) (uint32, error) {
	if n < 0 {
		return 0, fmt.Errorf("must be >= 0")
	}
	return unsignedConfigNumberToUint32(uint64(n), maxValue)
}

func unsignedConfigNumberToUint32(n uint64, maxValue uint32) (uint32, error) {
	if n > uint64(maxValue) {
		return 0, fmt.Errorf("exceeds maximum %d", maxValue)
	}
	return uint32(n), nil
}

func floatConfigNumberToUint32(n float64, maxValue uint32) (uint32, error) {
	if math.IsNaN(n) || math.IsInf(n, 0) {
		return 0, fmt.Errorf("must be finite")
	}
	if n < 0 {
		return 0, fmt.Errorf("must be >= 0")
	}
	if n > maxUint32ConfigFloat || n > float64(maxValue) {
		return 0, fmt.Errorf("exceeds maximum %d", maxValue)
	}
	if math.Trunc(n) != n {
		return 0, fmt.Errorf("must be an integer")
	}
	return uint32(n), nil
}

func stringMap(v interface{}) (map[string]interface{}, bool) {
	m, ok := v.(map[string]interface{})
	return m, ok
}

func validateMediaConfigEncoders(config map[string]interface{}) error {
	for _, enc := range extractEncoders(config) {
		if err := ValidateEncoderReconfig(enc.StreamName, enc.Width, enc.Height, enc.Codec, enc.Bitrate, enc.Fps, enc.Gop); err != nil {
			return fmt.Errorf("encoder %s: %w", enc.StreamName, err)
		}
	}
	return nil
}

// diffEncoders returns only encoders whose parameters have actually changed.
func diffEncoders(old, new_ []encoderParams) []encoderParams {
	oldMap := make(map[string]encoderParams, len(old))
	for _, e := range old {
		oldMap[e.StreamName] = e
	}

	var changed []encoderParams
	for _, ne := range new_ {
		oe, exists := oldMap[ne.StreamName]
		if !exists || oe.Width != ne.Width || oe.Height != ne.Height ||
			oe.Codec != ne.Codec || oe.Bitrate != ne.Bitrate ||
			oe.Fps != ne.Fps || oe.Gop != ne.Gop {
			changed = append(changed, ne)
		}
	}
	return changed
}

// applyEncoderChanges applies only changed encoder configs via gRPC.
func (h *MediaHandlers) applyEncoderChanges(encoders []encoderParams) error {
	client := camerapb.NewCameraControlClient(h.cameraClient)

	for _, enc := range encoders {
		ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)

		resp, err := client.ReconfigureEncoder(ctx, &camerapb.EncoderReconfigRequest{
			StreamName: enc.StreamName,
			Width:      enc.Width,
			Height:     enc.Height,
			Codec:      enc.Codec,
			BitrateBps: enc.Bitrate,
			Fps:        enc.Fps,
			Gop:        enc.Gop,
		})
		cancel()

		if err != nil {
			return fmt.Errorf("stream %s: gRPC error: %w", enc.StreamName, err)
		}
		if !resp.Success {
			return fmt.Errorf("stream %s: camera-daemon rejected: %s", enc.StreamName, resp.Message)
		}

		if h.streamReloader != nil {
			h.streamReloader.UpdateStreamFromApplied(enc.StreamName, enc.Width, enc.Height, enc.Codec, enc.Bitrate, enc.Fps, enc.Gop)
		}
	}
	return nil
}

// encoderParams is a parsed encoder config from the YAML.
type encoderParams struct {
	StreamName string
	Width      uint32
	Height     uint32
	Fps        uint32
	Codec      string
	Bitrate    uint32
	Gop        uint32
}

// extractEncoders parses encoder entries from the merged YAML config.
func extractEncoders(config map[string]interface{}) []encoderParams {
	raw, ok := config["encoders"]
	if !ok {
		return nil
	}
	list, ok := raw.([]interface{})
	if !ok {
		return nil
	}
	var result []encoderParams
	for _, item := range list {
		m, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		ep := encoderParams{
			StreamName: getFieldString(m, "stream_name"),
			Codec:      getFieldString(m, "codec"),
			Width:      getFieldUint32(m, "width"),
			Height:     getFieldUint32(m, "height"),
			Fps:        getFieldUint32(m, "fps"),
			Bitrate:    getFieldUint32(m, "bitrate"),
			Gop:        getFieldUint32(m, "gop"),
		}
		if ep.StreamName == "" {
			continue
		}
		result = append(result, ep)
	}
	return result
}

func getFieldString(m map[string]interface{}, key string) string {
	v, ok := m[key]
	if !ok {
		return ""
	}
	s, ok := v.(string)
	if ok {
		return s
	}
	return fmt.Sprintf("%v", v)
}

func getFieldUint32(m map[string]interface{}, key string) uint32 {
	v, ok := m[key]
	if !ok {
		return 0
	}
	n, err := configNumberToUint32(v, maxUint32ConfigValue)
	if err != nil {
		return 0
	}
	return n
}

// UpdateImageConfig applies real-time ISP updates via gRPC
func (h *MediaHandlers) UpdateImageConfig(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		ManualMode     *bool  `json:"manual_mode"`
		Brightness     *int32 `json:"brightness"`
		Contrast       *int32 `json:"contrast"`
		Saturation     *int32 `json:"saturation"`
		Sharpness      *int32 `json:"sharpness"`
		AutoExposure   *bool  `json:"auto_exposure"`
		Backlight      *int32 `json:"backlight"`
		ExposureTimeUs *int32 `json:"exposure_time_us"`
		Gain           *int32 `json:"gain"`
		NoiseReduction *int32 `json:"noise_reduction"`
		WdrValue       *int32 `json:"wdr_value"`
		PowerlineFreq  *int32 `json:"powerline_freq"`
		AwbIndex       *int32 `json:"awb_index"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body")
		return
	}

	if err := ValidateISPUpdate(
		req.Brightness, req.Contrast, req.Saturation, req.Sharpness,
		req.NoiseReduction, req.WdrValue, req.Backlight,
		req.ExposureTimeUs, req.Gain,
		req.PowerlineFreq,
		req.AwbIndex,
	); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	protoReq := &camerapb.ISPUpdateRequest{}
	if req.ManualMode != nil {
		protoReq.ManualMode = req.ManualMode
	}
	if req.Brightness != nil {
		protoReq.Brightness = req.Brightness
	}
	if req.Contrast != nil {
		protoReq.Contrast = req.Contrast
	}
	if req.Saturation != nil {
		protoReq.Saturation = req.Saturation
	}
	if req.Sharpness != nil {
		protoReq.Sharpness = req.Sharpness
	}
	if req.AutoExposure != nil {
		protoReq.AutoExposure = req.AutoExposure
	}
	if req.Backlight != nil {
		protoReq.Backlight = req.Backlight
	}
	if req.ExposureTimeUs != nil {
		protoReq.ExposureTimeUs = req.ExposureTimeUs
	}
	if req.Gain != nil {
		protoReq.Gain = req.Gain
	}
	if req.NoiseReduction != nil {
		protoReq.NoiseReduction = req.NoiseReduction
	}
	if req.WdrValue != nil {
		protoReq.WdrValue = req.WdrValue
	}
	if req.PowerlineFreq != nil {
		protoReq.PowerlineFreq = req.PowerlineFreq
	}
	if req.AwbIndex != nil {
		protoReq.AwbIndex = req.AwbIndex
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.UpdateISPSettings(ctx, protoReq)

	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to update ISP settings: "+err.Error())
		return
	}

	if resp.Status != nil && !resp.Status.Success {
		Resp(c).FailMsg(CodeCameraError, "Camera daemon rejected ISP update: "+resp.Status.Message)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.changed", eventLoggerPkg.MessageParams{"stream": "main", "changes": fmt.Sprintf("ISP: %+v", req)}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"message": "ISP settings applied successfully"})
}

// GetImageConfig retrieves the current ISP configuration from camera daemon
func (h *MediaHandlers) GetImageConfig(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.GetISPConfig(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to get ISP config: "+err.Error())
		return
	}

	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, "Camera daemon error: "+resp.Message)
		return
	}

	cur := resp.Current
	Resp(c).OK(gin.H{
		"manual_mode":      cur.ManualMode,
		"brightness":       cur.Brightness,
		"contrast":         cur.Contrast,
		"saturation":       cur.Saturation,
		"sharpness":        cur.Sharpness,
		"auto_exposure":    cur.AutoExposure,
		"backlight":        cur.Backlight,
		"exposure_time_us": cur.ExposureTimeUs,
		"gain":             cur.Gain,
		"noise_reduction":  cur.NoiseReduction,
		"wdr_value":        cur.WdrValue,
		"powerline_freq":   cur.PowerlineFreq,
		"awb_index":        cur.AwbIndex,
	})
}

// GetTransformConfig retrieves the current transform configuration (rotation, flip, dewarp, grayscale)
func (h *MediaHandlers) GetTransformConfig(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.GetTransformConfig(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to get transform config: "+err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"rotation":  resp.Rotation,
		"flip":      resp.Flip,
		"dewarp":    resp.Dewarp,
		"grayscale": resp.Grayscale,
		"dis":       resp.Dis,
		"eis":       resp.Eis,
	})
}

// UpdateTransformConfig applies transform changes (rotation, flip, dewarp, grayscale)
func (h *MediaHandlers) UpdateTransformConfig(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		Rotation  *uint32 `json:"rotation"`
		Flip      *uint32 `json:"flip"`
		Dewarp    *bool   `json:"dewarp"`
		Grayscale *bool   `json:"grayscale"`
		Dis       *bool   `json:"dis"`
		Eis       *bool   `json:"eis"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body")
		return
	}

	if err := ValidateTransformUpdate(req.Rotation, req.Flip); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	// SetTransformConfig may trigger a ~12-14s medialib full reinit when the
	// rotation changes (rotation_full_reinit rebuilds all ISP profiles). The
	// normal 10s budget trips DeadlineExceeded mid-reinit, surfacing a false
	// "save failed" to the UI even though the daemon completes successfully.
	// 30s gives comfortable headroom over the worst-case reinit. Keep
	// context.Background() (not c.Request.Context()) so a client disconnect
	// never cancels the in-flight reinit — the daemon cannot be interrupted
	// mid-rebuild and cancelling would orphan the consumer re-attach.
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// proto3 scalars cannot represent "unchanged": a single-field partial request would
	// make the daemon treat the omitted fields as 0/false and overwrite them to defaults,
	// silently resetting the other transform params. Read the current values as a merge
	// baseline, overlay only the explicitly provided fields, then send the full config.
	cur, err := client.GetTransformConfig(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to read current transform config: "+err.Error())
		return
	}
	protoReq := cur
	if req.Rotation != nil {
		protoReq.Rotation = *req.Rotation
	}
	if req.Flip != nil {
		protoReq.Flip = *req.Flip
	}
	if req.Dewarp != nil {
		protoReq.Dewarp = *req.Dewarp
	}
	if req.Grayscale != nil {
		protoReq.Grayscale = *req.Grayscale
	}
	if req.Dis != nil {
		protoReq.Dis = *req.Dis
	}
	if req.Eis != nil {
		protoReq.Eis = *req.Eis
	}

	resp, err := client.SetTransformConfig(ctx, protoReq)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to update transform config: "+err.Error())
		return
	}

	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, "Camera daemon rejected transform update: "+resp.Message)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.changed", eventLoggerPkg.MessageParams{"stream": "main", "changes": fmt.Sprintf("Transform: %+v", req)}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"message": "Transform config applied successfully"})
}

// SetConfigField writes one allow-listed scalar media-profile field via the
// camera-daemon config_field RPC (e.g. frontend.hailort.use-hailort-service).
// The daemon enforces an allow-list (preventing two-writer races with the typed
// transform/encoder/ISP RPCs), applies the override to the HAL, and mirrors it
// to /data/aipc/etc so it survives restart (replay-on-boot). `type` is the
// ConfigFieldType enum value (0=BOOL 1=INT32 2=UINT32 3=FLOAT64 4=STRING);
// `value` is always a string, parsed by the HAL per `type`.
func (h *MediaHandlers) SetConfigField(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		FieldPath string                   `json:"field_path" binding:"required"`
		Type      camerapb.ConfigFieldType `json:"type"`
		Value     string                   `json:"value"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}
	if req.FieldPath == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "field_path is required")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	// set_config_field may stop/reconfigure a stream to apply the override; allow
	// headroom over the default 10s. Use context.Background() (not the request
	// context) so a client disconnect never cancels an in-flight HAL reconfigure.
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	resp, err := client.SetConfigField(ctx, &camerapb.SetConfigFieldRequest{
		FieldPath: req.FieldPath,
		Type:      req.Type,
		Value:     req.Value,
	})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to set config field: "+err.Error())
		return
	}
	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, "Camera daemon rejected config field: "+resp.Message)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.changed",
			eventLoggerPkg.MessageParams{"field": req.FieldPath, "value": req.Value},
			getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"message": "Config field applied successfully"})
}

// GetConfigField reads one scalar media-profile field straight from the HAL via
// the camera-daemon config_field RPC. Returns the current runtime value (which
// reflects any override applied via SetConfigField / transform / encoder RPCs).
func (h *MediaHandlers) GetConfigField(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	fieldPath := c.Query("field_path")
	if fieldPath == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "field_path query parameter is required")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.GetConfigField(ctx, &camerapb.GetConfigFieldRequest{FieldPath: fieldPath})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to get config field: "+err.Error())
		return
	}
	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, "Camera daemon could not read config field: "+resp.Message)
		return
	}

	Resp(c).OK(gin.H{
		"field_path": fieldPath,
		"type":       int32(resp.Type),
		"type_name":  resp.Type.String(),
		"value":      resp.Value,
	})
}

// UpdateEncoderConfig applies real-time encoder updates (bitrate, gop) via hot-reload.
// FPS changes require pipeline restart and are routed to fpsPipelineReconfigure automatically.
func (h *MediaHandlers) UpdateEncoderConfig(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		StreamName string  `json:"stream_name" binding:"required"`
		BitrateBps *uint32 `json:"bitrate_bps"`
		Framerate  *uint32 `json:"framerate"`
		Gop        *uint32 `json:"gop"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	// At least one encoder parameter must be set
	if req.BitrateBps == nil && req.Framerate == nil && req.Gop == nil {
		Resp(c).FailMsg(CodeInvalidRequest, "At least one encoder parameter must be specified")
		return
	}

	// Validate parameters against stream rules
	var bitrate, framerate, gop uint32
	if req.BitrateBps != nil {
		bitrate = *req.BitrateBps
	}
	if req.Framerate != nil {
		framerate = *req.Framerate
	}
	if req.Gop != nil {
		gop = *req.Gop
	}
	if err := ValidateEncoderHotReload(req.StreamName, bitrate, framerate, gop); err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", err.Error())
		return
	}

	// Route FPS changes to pipeline reconfigure (requires restart for ISP buffer pool resize).
	// Bitrate and GOP-only changes use the faster hot-reload path below.
	if req.Framerate != nil {
		h.fpsPipelineReconfigure(c, req.StreamName, req.BitrateBps, req.Framerate, req.Gop)
		return
	}

	// Build gRPC request — hot-reload path (bitrate/gop only)
	grpcReq := &camerapb.EncoderConfigRequest{
		StreamName: req.StreamName,
	}
	if req.BitrateBps != nil {
		grpcReq.BitrateBps = *req.BitrateBps
	}
	if req.Gop != nil {
		grpcReq.Gop = *req.Gop
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.UpdateEncoderConfig(ctx, grpcReq)

	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to update encoder config: "+err.Error())
		return
	}

	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, "Camera daemon rejected encoder update: "+resp.Message)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.changed", eventLoggerPkg.MessageParams{"stream": req.StreamName, "changes": "encoder config updated"}, getUsernameFromContext(c))
	}

	// Persist hot-reload changes to YAML so they survive restart
	h.writeStreamHotReload(req.StreamName, req.BitrateBps, nil, req.Gop)
	// Update in-memory config and clear SPS/PPS cache so frontend
	// gets fresh init segments after bitrate/GOP changes.
	if h.streamReloader != nil {
		h.streamReloader.UpdateStreamFromApplied(req.StreamName, 0, 0, "", safeU32(req.BitrateBps), 0, safeU32(req.Gop))
		// Some encoders reset their reference-frame state when bitrate changes.
		// Request a clean IDR immediately so existing clients do not decode
		// transitional P-frames against the old reference chain.
		h.streamReloader.ForceKeyframe(req.StreamName)
	}

	Resp(c).OK(gin.H{"message": "Encoder config updated successfully", "stream": req.StreamName})
}

// fpsPipelineReconfigure handles FPS changes via full pipeline reconfigure.
// This rebuilds ISP buffer pools sized for the new framerate.
func (h *MediaHandlers) fpsPipelineReconfigure(c *gin.Context, streamName string, bitrateBps, framerate, gop *uint32) {
	fpsVal := safeU32(framerate)
	bitrateVal := safeU32(bitrateBps)
	gopVal := safeU32(gop)

	running := h.getRunningStreamParams(streamName)

	var baseWidth, baseHeight, baseBitrate, baseFps uint32
	var baseCodec string
	var oldFps uint32

	if running != nil {
		baseWidth = running.Width
		baseHeight = running.Height
		baseBitrate = running.Bitrate
		baseFps = running.Fps
		baseCodec = running.Codec
		oldFps = running.Fps
	} else {
		// Fallback to YAML config when gRPC is unavailable
		for _, ye := range extractEncodersFromFile(h.configPath) {
			if ye.StreamName == streamName {
				baseWidth = ye.Width
				baseHeight = ye.Height
				baseBitrate = ye.Bitrate
				baseFps = ye.Fps
				baseCodec = ye.Codec
				oldFps = ye.Fps
				break
			}
		}
		if baseWidth == 0 {
			Resp(c).FailMsg(CodeCameraError, "Cannot determine current params for "+streamName)
			return
		}
	}

	br := baseBitrate
	if bitrateVal > 0 {
		br = bitrateVal
	}
	fps := baseFps
	if fpsVal > 0 {
		fps = fpsVal
	}

	streams := h.buildPipelineStreamConfigs(streamName, baseWidth, baseHeight, baseCodec, br, fps, gopVal)

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	resp, err := client.ReconfigurePipeline(ctx, &camerapb.ReconfigurePipelineRequest{Streams: streams})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Pipeline reconfigure failed: "+err.Error())
		return
	}
	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, "Pipeline reconfigure failed: "+resp.Message)
		return
	}

	// Persist FPS change to YAML
	h.writeStreamHotReload(streamName, bitrateBps, framerate, gop)
	if h.streamReloader != nil {
		h.streamReloader.UpdateStreamFromApplied(streamName, 0, 0, "", safeU32(bitrateBps), safeU32(framerate), safeU32(gop))
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.changed", eventLoggerPkg.MessageParams{"stream": streamName, "changes": fmt.Sprintf("fps pipeline reconfigure: %d->%d", oldFps, fps)}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"message": fmt.Sprintf("FPS updated via pipeline reconfigure (%d->%d, took %dms)", oldFps, fps, resp.InterruptMs), "stream": streamName})
}

func safeU32(p *uint32) uint32 {
	if p != nil {
		return *p
	}
	return 0
}

// writeStreamHotReload persists hot-reload parameters to YAML without touching other fields.
func (h *MediaHandlers) writeStreamHotReload(streamName string, bitrateBps, framerate, gop *uint32) {
	h.configMu.Lock()
	defer h.configMu.Unlock()

	data, err := os.ReadFile(h.configPath)
	if err != nil {
		return
	}
	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		return
	}

	raw, ok := config["encoders"]
	if !ok {
		return
	}
	list, ok := raw.([]interface{})
	if !ok {
		return
	}

	changed := false
	for _, item := range list {
		m, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		if getFieldString(m, "stream_name") != streamName {
			continue
		}
		if bitrateBps != nil {
			m["bitrate"] = int(*bitrateBps)
			changed = true
		}
		if framerate != nil {
			m["fps"] = int(*framerate)
			changed = true
		}
		if gop != nil {
			m["gop"] = int(*gop)
			changed = true
		}
		break
	}

	if !changed {
		return
	}

	outData, err := marshalMediaConfig(config)
	if err != nil {
		return
	}
	_ = h.projectMediaConfig(context.Background(), "", string(outData))
}

// writeRtspConfig persists the RTSP enabled flag to camera-daemon.yaml so the
// toggle survives a daemon restart. Mirrors writeStreamHotReload: read-modify-
// write the YAML map, then route through the Config Controller (media/config
// domain) with a direct-write fallback. Called only after the gRPC hot-reload
// succeeds, so the file never lags the live device state.
func (h *MediaHandlers) writeRtspConfig(ctx context.Context, actor string, enabled bool) {
	h.configMu.Lock()
	defer h.configMu.Unlock()

	data, err := os.ReadFile(h.configPath)
	if err != nil {
		return
	}
	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		return
	}

	rtsp, ok := config["rtsp"].(map[string]interface{})
	if !ok {
		rtsp = make(map[string]interface{})
		config["rtsp"] = rtsp
	}
	rtsp["enabled"] = enabled

	outData, err := marshalMediaConfig(config)
	if err != nil {
		return
	}
	_ = h.projectMediaConfig(ctx, actor, string(outData))
}

// SetRtspEnabled enables or disables RTSP server via gRPC - hot reload
func (h *MediaHandlers) SetRtspEnabled(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		Enabled bool `json:"enabled"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.SetRtspEnabled(ctx, &camerapb.RtspEnabledRequest{
		Enabled: req.Enabled,
	})

	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to set RTSP state: "+err.Error())
		return
	}

	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, "Camera daemon rejected RTSP state change: "+resp.Message)
		return
	}

	// Persist the toggle so it survives a daemon restart. Fire-and-forget:
	// failure here only means the next restart reverts to the old value.
	h.writeRtspConfig(context.Background(), getUsernameFromContext(c), req.Enabled)

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.changed", eventLoggerPkg.MessageParams{"stream": "rtsp", "changes": fmt.Sprintf("enabled=%v", req.Enabled)}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"message": "RTSP state updated successfully", "enabled": req.Enabled})
}

// writeAiOverlayConfig persists AI overlay settings to camera-daemon.yaml so
// they survive a daemon restart. The proto field names differ from the yaml
// keys (show_label→draw_labels, show_confidence→draw_confidence,
// line_thickness→box_thickness); the remaining yaml-only keys
// (event_bus_endpoint, topic_prefix, draw_landmarks, enable_face_blur,
// stream_map) are preserved by the read-modify-write of the whole map. Called
// only after the gRPC hot-reload succeeds.
func (h *MediaHandlers) writeAiOverlayConfig(ctx context.Context, actor string, enabled bool, showLabel, showConfidence bool, lineThickness uint32) {
	h.configMu.Lock()
	defer h.configMu.Unlock()

	data, err := os.ReadFile(h.configPath)
	if err != nil {
		return
	}
	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		return
	}

	overlay, ok := config["ai_overlay"].(map[string]interface{})
	if !ok {
		overlay = make(map[string]interface{})
		config["ai_overlay"] = overlay
	}
	overlay["enabled"] = enabled
	overlay["draw_labels"] = showLabel
	overlay["draw_confidence"] = showConfidence
	overlay["box_thickness"] = int(lineThickness)

	outData, err := marshalMediaConfig(config)
	if err != nil {
		return
	}
	_ = h.projectMediaConfig(ctx, actor, string(outData))
}

// UpdateAiOverlay updates AI overlay configuration via gRPC - hot reload
func (h *MediaHandlers) UpdateAiOverlay(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		Enabled        bool   `json:"enabled"`
		ShowLabel      bool   `json:"show_label"`
		ShowConfidence bool   `json:"show_confidence"`
		LineThickness  uint32 `json:"line_thickness"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.UpdateAiOverlay(ctx, &camerapb.AiOverlayConfig{
		Enabled:        req.Enabled,
		ShowLabel:      req.ShowLabel,
		ShowConfidence: req.ShowConfidence,
		LineThickness:  req.LineThickness,
	})

	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to update AI overlay config: "+err.Error())
		return
	}

	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, "Camera daemon rejected AI overlay update: "+resp.Message)
		return
	}

	// Persist so overlay settings survive a daemon restart.
	h.writeAiOverlayConfig(context.Background(), getUsernameFromContext(c),
		req.Enabled, req.ShowLabel, req.ShowConfidence, req.LineThickness)

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.ai_overlay.changed", eventLoggerPkg.MessageParams{"enabled": req.Enabled,
			"show_label":      req.ShowLabel,
			"show_confidence": req.ShowConfidence}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"message": "AI overlay config updated successfully"})
}

// UpdateOsdConfig updates OSD configuration via gRPC - hot reload
func (h *MediaHandlers) UpdateOsdConfig(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		Streams []struct {
			StreamName   string `json:"stream_name"`
			TextOverlays []struct {
				ID        string  `json:"id"`
				Text      string  `json:"text"`
				X         float32 `json:"x"`
				Y         float32 `json:"y"`
				FontSize  float32 `json:"font_size"`
				TextColor uint32  `json:"text_color"`
				HAlign    int32   `json:"h_align"`
				VAlign    int32   `json:"v_align"`
				Enabled   bool    `json:"enabled"`
			} `json:"text_overlays"`
			DatetimeOverlays []struct {
				ID        string  `json:"id"`
				X         float32 `json:"x"`
				Y         float32 `json:"y"`
				Format    string  `json:"format"`
				FontSize  float32 `json:"font_size"`
				TextColor uint32  `json:"text_color"`
				HAlign    int32   `json:"h_align"`
				VAlign    int32   `json:"v_align"`
				Enabled   bool    `json:"enabled"`
			} `json:"datetime_overlays"`
			ImageOverlays []struct {
				ID        string  `json:"id"`
				ImagePath string  `json:"image_path"`
				X         float32 `json:"x"`
				Y         float32 `json:"y"`
				Width     float32 `json:"width"`
				Height    float32 `json:"height"`
				HAlign    int32   `json:"h_align"`
				VAlign    int32   `json:"v_align"`
				Enabled   bool    `json:"enabled"`
			} `json:"image_overlays"`
		} `json:"streams"`
		// Editor edit-mode: when true the daemon bakes no text overlays (clean
		// stream for the HTML proxy layer); re-bakes on editor exit (false).
		SuppressBake bool `json:"suppress_bake"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body")
		return
	}

	// Convert to gRPC request
	grpcReq := &camerapb.OsdConfigRequest{}
	for _, s := range req.Streams {
		streamName := strings.TrimSpace(s.StreamName)
		if streamName == "" {
			logger.Warn("media OSD: dropping stream with empty stream_name")
			continue
		}
		streamCfg := &camerapb.StreamOsdConfig{
			StreamName: streamName,
		}

		for _, t := range s.TextOverlays {
			id := strings.TrimSpace(t.ID)
			if id == "" {
				logger.Warn("media OSD: dropping text overlay with empty id on stream %s", streamName)
				continue
			}
			streamCfg.TextOverlays = append(streamCfg.TextOverlays, &camerapb.OsdTextOverlayConfig{
				Id:        id,
				Text:      t.Text,
				X:         clampOsdUnit(t.X),
				Y:         clampOsdUnit(t.Y),
				FontSize:  t.FontSize,
				TextColor: t.TextColor,
				HAlign:    t.HAlign,
				VAlign:    t.VAlign,
				Enabled:   t.Enabled,
			})
		}

		for _, d := range s.DatetimeOverlays {
			id := strings.TrimSpace(d.ID)
			if id == "" {
				logger.Warn("media OSD: dropping datetime overlay with empty id on stream %s", streamName)
				continue
			}
			streamCfg.DatetimeOverlays = append(streamCfg.DatetimeOverlays, &camerapb.OsdDateTimeOverlayConfig{
				Id:        id,
				X:         clampOsdUnit(d.X),
				Y:         clampOsdUnit(d.Y),
				Format:    d.Format,
				FontSize:  d.FontSize,
				TextColor: d.TextColor,
				HAlign:    d.HAlign,
				VAlign:    d.VAlign,
				Enabled:   d.Enabled,
			})
		}

		for _, img := range s.ImageOverlays {
			id := strings.TrimSpace(img.ID)
			if id == "" {
				logger.Warn("media OSD: dropping image overlay with empty id on stream %s", streamName)
				continue
			}
			imagePath, ok := validOsdImagePath(img.ImagePath)
			if !ok {
				logger.Warn("media OSD: dropping image overlay %s on stream %s: invalid image_path=%q",
					id, streamName, img.ImagePath)
				continue
			}
			if !validOsdExtent(img.Width) || !validOsdExtent(img.Height) {
				logger.Warn("media OSD: dropping image overlay %s on stream %s: invalid size %.3fx%.3f",
					id, streamName, img.Width, img.Height)
				continue
			}
			streamCfg.ImageOverlays = append(streamCfg.ImageOverlays, &camerapb.OsdImageOverlayConfig{
				Id:        id,
				ImagePath: imagePath,
				X:         clampOsdUnit(img.X),
				Y:         clampOsdUnit(img.Y),
				Width:     img.Width,
				Height:    img.Height,
				HAlign:    img.HAlign,
				VAlign:    img.VAlign,
				Enabled:   img.Enabled,
			})
		}

		grpcReq.Streams = append(grpcReq.Streams, streamCfg)
	}
	grpcReq.SuppressBake = req.SuppressBake

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.UpdateOsdConfig(ctx, grpcReq)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to update OSD config: "+err.Error())
		return
	}

	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, "Camera daemon rejected OSD update: "+resp.Message)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"media.osd.changed",
			eventLoggerPkg.MessageParams{"streams": "all"},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{"message": "OSD config updated successfully"})
}

// GetOsdConfig returns current OSD configuration via gRPC
func (h *MediaHandlers) GetOsdConfig(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.GetOsdConfig(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to get OSD config: "+err.Error())
		return
	}

	// Convert gRPC response to JSON
	streams := make([]gin.H, 0, len(resp.Streams))
	for _, s := range resp.Streams {
		stream := gin.H{
			"stream_name": s.StreamName,
		}

		textOverlays := make([]gin.H, 0, len(s.TextOverlays))
		for _, t := range s.TextOverlays {
			textOverlays = append(textOverlays, gin.H{
				"id":         t.Id,
				"text":       t.Text,
				"x":          t.X,
				"y":          t.Y,
				"font_size":  t.FontSize,
				"text_color": t.TextColor,
				"h_align":    t.HAlign,
				"v_align":    t.VAlign,
				"enabled":    t.Enabled,
			})
		}
		stream["text_overlays"] = textOverlays

		datetimeOverlays := make([]gin.H, 0, len(s.DatetimeOverlays))
		for _, d := range s.DatetimeOverlays {
			datetimeOverlays = append(datetimeOverlays, gin.H{
				"id":         d.Id,
				"x":          d.X,
				"y":          d.Y,
				"format":     d.Format,
				"font_size":  d.FontSize,
				"text_color": d.TextColor,
				"h_align":    d.HAlign,
				"v_align":    d.VAlign,
				"enabled":    d.Enabled,
			})
		}
		stream["datetime_overlays"] = datetimeOverlays

		imageOverlays := make([]gin.H, 0, len(s.ImageOverlays))
		for _, img := range s.ImageOverlays {
			var imageSize int64
			if info, err := os.Stat(img.ImagePath); err == nil && !info.IsDir() {
				imageSize = info.Size()
			}
			imageOverlays = append(imageOverlays, gin.H{
				"id":         img.Id,
				"image_path": img.ImagePath,
				"image_size": imageSize,
				"x":          img.X,
				"y":          img.Y,
				"width":      img.Width,
				"height":     img.Height,
				"h_align":    img.HAlign,
				"v_align":    img.VAlign,
				"enabled":    img.Enabled,
			})
		}
		stream["image_overlays"] = imageOverlays

		streams = append(streams, stream)
	}

	Resp(c).OK(gin.H{"streams": streams})
}

// UploadOsdImage handles PNG/BMP upload for OSD image overlays.
// Files are saved to a fixed directory (/data/aipc/etc/osd/) and the
// absolute path is returned for use as image_path in overlay config.
func (h *MediaHandlers) UploadOsdImage(c *gin.Context) {
	file, err := c.FormFile("file")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "file is required: "+err.Error())
		return
	}

	// Validate file extension — HAL supports PNG and BMP
	ext := strings.ToLower(filepath.Ext(file.Filename))
	if ext != ".png" && ext != ".bmp" {
		Resp(c).FailMsg(CodeInvalidRequest, "only PNG and BMP images are supported")
		return
	}

	// Sanitize: use only the base filename to prevent path traversal
	filename := filepath.Base(file.Filename)
	destDir := constants.RootPath() + "/etc/osd"
	if err := os.MkdirAll(destDir, 0755); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "failed to create OSD image directory: "+err.Error())
		return
	}

	destPath := filepath.Join(destDir, filename)
	if err := c.SaveUploadedFile(file, destPath); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "failed to save image: "+err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"path": destPath,
		"size": file.Size,
	})
}

// ServeOsdFont streams the exact TTF the camera-daemon bakes text/datetime OSD
// with (/usr/share/fonts/ttf/LiberationMono-Regular.ttf) so the web HTML proxy
// can render text in the identical font — eliminating the proxy-vs-baked font
// mismatch that made configured text feel disjointed.
func (h *MediaHandlers) ServeOsdFont(c *gin.Context) {
	const fontPath = "/usr/share/fonts/ttf/LiberationMono-Regular.ttf"
	c.Header("Cache-Control", "public, max-age=86400")
	c.File(fontPath)
}

// ServeOsdImage streams an uploaded OSD overlay image (PNG/BMP) back to the
// browser so the HTML image proxy can render the picture and it follows the
// drag frame. Addressed by base filename only and joined under the OSD image
// directory, so path traversal can't escape it (image_path on the overlay is
// the absolute device path; the proxy derives the basename for this route).
func (h *MediaHandlers) ServeOsdImage(c *gin.Context) {
	name := filepath.Base(c.Param("name"))
	if name == "" || name == "." || name == string(filepath.Separator) {
		Resp(c).FailMsg(CodeInvalidRequest, "invalid image name")
		return
	}
	ext := strings.ToLower(filepath.Ext(name))
	if ext != ".png" && ext != ".bmp" {
		Resp(c).FailMsg(CodeInvalidRequest, "only PNG and BMP images are supported")
		return
	}
	dest := filepath.Join(constants.RootPath()+"/etc/osd", name)
	c.Header("Cache-Control", "public, max-age=3600")
	c.File(dest)
}

// ReconfigureEncoder performs full encoder reconfiguration with brief restart
// This is for resolution/codec changes that require encoder restart
func (h *MediaHandlers) ReconfigureEncoder(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		StreamName string `json:"stream_name"`
		Width      uint32 `json:"width"`
		Height     uint32 `json:"height"`
		Codec      string `json:"codec"`
		BitrateBps uint32 `json:"bitrate_bps"`
		Fps        uint32 `json:"fps"`
		Gop        uint32 `json:"gop"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body")
		return
	}

	if req.StreamName == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "stream_name is required")
		return
	}

	// Validate all parameters against stream rules
	if err := ValidateEncoderReconfig(req.StreamName, req.Width, req.Height, req.Codec, req.BitrateBps, req.Fps, req.Gop); err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", err.Error())
		return
	}

	// The web UI sends all encoder params (width, height, fps, codec, bitrate, gop) on every save.
	// Check if resolution or fps is actually CHANGING vs. running config.
	// Only real resolution/fps changes need full pipeline reinit.
	// Compare against RUNNING pipeline (gRPC), not YAML — YAML can desync.
	running := h.getRunningStreamParams(req.StreamName)
	resolutionChanging := false
	fpsChanging := false
	if running != nil {
		if req.Width > 0 && req.Width != running.Width {
			resolutionChanging = true
		}
		if req.Height > 0 && req.Height != running.Height {
			resolutionChanging = true
		}
		if req.Fps > 0 && req.Fps != running.Fps {
			fpsChanging = true
		}
	}

	if resolutionChanging || fpsChanging {
		h.encoderReconfigViaPipeline(c, req.StreamName, req.Width, req.Height, req.Codec, req.BitrateBps, req.Fps, req.Gop)
		return
	}

	// Codec/bitrate/GOP changes use the ReconfigureEncoder gRPC path
	grpcReq := &camerapb.EncoderReconfigRequest{
		StreamName: req.StreamName,
		Codec:      req.Codec,
		BitrateBps: req.BitrateBps,
		Gop:        req.Gop,
		Width:      req.Width,
		Height:     req.Height,
		Fps:        req.Fps,
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	resp, err := client.ReconfigureEncoder(ctx, grpcReq)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to reconfigure encoder: "+err.Error())
		return
	}

	if !resp.Success {
		detail := resp.Message
		if resp.HalError != nil {
			detail = fmt.Sprintf("HAL error %s (%d): %s — %s", resp.HalError.Name, resp.HalError.Code, resp.HalError.Description, resp.Message)
		}
		Resp(c).FailTyped(CodeCameraError, "hal", detail)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.changed", eventLoggerPkg.MessageParams{"stream": req.StreamName, "changes": "encoder config updated"}, getUsernameFromContext(c))
	}

	h.writeStreamToConfig(req.StreamName, req.Width, req.Height, req.Codec, req.BitrateBps, req.Fps, req.Gop)
	if h.streamReloader != nil {
		h.streamReloader.UpdateStreamFromApplied(req.StreamName, req.Width, req.Height, req.Codec, req.BitrateBps, req.Fps, req.Gop)
		// The encoder briefly restarted. Force an IDR so connected clients resume
		// from a clean keyframe instead of decoding transitional P-frames that
		// reference the pre-reconfig reference (causes 花屏/黑屏).
		h.streamReloader.ForceKeyframe(req.StreamName)
	}

	Resp(c).OK(gin.H{
		"message":      "Encoder reconfigured successfully",
		"interrupt_ms": resp.InterruptMs,
	})
}

// encoderReconfigViaPipeline handles resolution/codec/fps changes through full pipeline
// deinit/reinit instead of override_stream_params, which is more reliable for codec transitions.
func (h *MediaHandlers) encoderReconfigViaPipeline(c *gin.Context, streamName string, width, height uint32, codec string, bitrate, fps, gop uint32) {
	grpcStreams := h.buildPipelineStreamConfigs(streamName, width, height, codec, bitrate, fps, gop)
	if len(grpcStreams) == 0 {
		Resp(c).FailMsg(CodeCameraError, "Cannot read current stream config for pipeline reconfigure")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	resp, err := client.ReconfigurePipeline(ctx, &camerapb.ReconfigurePipelineRequest{
		Streams: grpcStreams,
	})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to reconfigure pipeline: "+err.Error())
		return
	}

	if !resp.Success {
		detail := resp.Message
		if resp.HalError != nil {
			detail = fmt.Sprintf("HAL error %s (%d): %s — %s", resp.HalError.Name, resp.HalError.Code, resp.HalError.Description, resp.Message)
		}
		Resp(c).FailTyped(CodeCameraError, "hal", detail)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.changed", eventLoggerPkg.MessageParams{"stream": streamName, "changes": fmt.Sprintf("resolution=%dx%d, codec=%s via pipeline reconfigure", width, height, codec)}, getUsernameFromContext(c))
	}

	if resp.AppliedStreams != nil {
		for _, s := range resp.AppliedStreams {
			go h.writeStreamToConfig(s.StreamId, s.EncoderWidth, s.EncoderHeight, s.Codec, s.EncoderBitrate, s.EncoderFramerate, s.EncoderGop)
			if h.streamReloader != nil {
				h.streamReloader.UpdateStreamFromApplied(s.StreamId, s.EncoderWidth, s.EncoderHeight, s.Codec, s.EncoderBitrate, s.EncoderFramerate, s.EncoderGop)
				// Full pipeline restart drops the EncodedPublisher UDS. Remove the
				// stale H264Stream so the next WebSocket client creates a fresh one
				// with a zero-backoff readLoop; AddClient on reconnect forces the IDR.
				// Without this the old readLoop sleeps up to ~10s on backoff → 黑屏.
				h.streamReloader.RestartH264Stream(s.StreamId)
			}
		}
	} else {
		h.writeStreamToConfig(streamName, width, height, codec, bitrate, fps, gop)
		if h.streamReloader != nil {
			h.streamReloader.UpdateStreamFromApplied(streamName, width, height, codec, bitrate, fps, gop)
			h.streamReloader.RestartH264Stream(streamName)
		}
	}

	Resp(c).OK(gin.H{
		"message":      "Encoder reconfigured via pipeline successfully",
		"interrupt_ms": resp.InterruptMs,
	})
}

// GetProfile returns the current media pipeline profile name
func (h *MediaHandlers) GetProfile(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.GetProfile(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to get current profile: "+err.Error())
		return
	}

	Resp(c).OK(gin.H{"profile_name": resp.ProfileName})
}

// ListProfiles returns all available media pipeline profiles
func (h *MediaHandlers) ListProfiles(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.ListProfiles(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to list profiles: "+err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"profiles":        resp.Profiles,
		"current_profile": resp.CurrentProfile,
	})
}

// SwitchProfile switches the active medialib pipeline profile.
// POST /api/v1/media/profile/switch  body: {"profile_name":"AI_ISP_Gen1_Basic"}
// The camera-daemon enforces an AI_ISP_Gen{1,2,3}_Basic allowlist and rolls back
// on failure; we surface success/message/interrupt_ms to the caller.
func (h *MediaHandlers) SwitchProfile(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		ProfileName string `json:"profile_name"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}
	if req.ProfileName == "" {
		Resp(c).FailMsg(CodeInvalidParameter, "profile_name is required")
		return
	}

	// Profile switching is synchronous and restarts the GStreamer pipeline; give it
	// headroom beyond the normal RPC timeout.
	ctx, cancel := context.WithTimeout(c.Request.Context(), 30*time.Second)
	defer cancel()

	client := camerapb.NewCameraControlClient(h.cameraClient)
	resp, err := client.SwitchProfile(ctx, &camerapb.SwitchProfileRequest{
		ProfileName: req.ProfileName,
	})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Switch profile failed: "+err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"success":      resp.Success,
		"message":      resp.Message,
		"interrupt_ms": resp.InterruptMs,
	})
}

// BackupProfile persists current medialib profiles to disk
// POST /api/v1/media/profile/backup
func (h *MediaHandlers) BackupProfile(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		Path string `json:"path"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		// Path is optional — empty body is fine
	}

	ctx, cancel := context.WithTimeout(c.Request.Context(), 10*time.Second)
	defer cancel()

	client := camerapb.NewCameraControlClient(h.cameraClient)
	resp, err := client.BackupProfile(ctx, &camerapb.BackupProfileRequest{
		Path: req.Path,
	})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Backup failed: "+err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"success": resp.Success,
		"message": resp.Message,
	})
}

// ReconfigurePipeline reconfigures the video pipeline with a new stream layout
// POST /api/v1/media/pipeline/reconfigure
func (h *MediaHandlers) ReconfigurePipeline(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		Streams []struct {
			StreamID         string `json:"stream_id"`
			InputWidth       uint32 `json:"input_width"`
			InputHeight      uint32 `json:"input_height"`
			InputFramerate   uint32 `json:"input_framerate"`
			Codec            string `json:"codec"`
			EncoderWidth     uint32 `json:"encoder_width"`
			EncoderHeight    uint32 `json:"encoder_height"`
			EncoderFramerate uint32 `json:"encoder_framerate"`
			EncoderBitrate   uint32 `json:"encoder_bitrate"`
			EncoderGop       uint32 `json:"encoder_gop"`
		} `json:"streams" binding:"required"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}
	if len(req.Streams) == 0 || len(req.Streams) > 4 {
		Resp(c).FailMsg(CodeInvalidRequest, "Stream count must be 1-4")
		return
	}

	// Validate all stream parameters
	validateInputs := make([]PipelineStreamInput, len(req.Streams))
	for i, s := range req.Streams {
		streamID := s.StreamID
		if streamID == "" {
			streamID = fmt.Sprintf("sink%d", i)
		}
		validateInputs[i] = PipelineStreamInput{
			StreamID:         streamID,
			InputWidth:       s.InputWidth,
			InputHeight:      s.InputHeight,
			InputFramerate:   s.InputFramerate,
			Codec:            s.Codec,
			EncoderWidth:     s.EncoderWidth,
			EncoderHeight:    s.EncoderHeight,
			EncoderFramerate: s.EncoderFramerate,
			EncoderBitrate:   s.EncoderBitrate,
			EncoderGOP:       s.EncoderGop,
		}
	}
	if err := ValidatePipelineReconfig(validateInputs); err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", err.Error())
		return
	}

	grpcStreams := make([]*camerapb.PipelineStreamConfig, len(req.Streams))
	for i, s := range req.Streams {
		streamID := s.StreamID
		if streamID == "" {
			streamID = fmt.Sprintf("sink%d", i)
		}
		grpcStreams[i] = &camerapb.PipelineStreamConfig{
			StreamId:         streamID,
			InputWidth:       s.InputWidth,
			InputHeight:      s.InputHeight,
			InputFramerate:   s.InputFramerate,
			Codec:            s.Codec,
			EncoderWidth:     s.EncoderWidth,
			EncoderHeight:    s.EncoderHeight,
			EncoderFramerate: s.EncoderFramerate,
			EncoderBitrate:   s.EncoderBitrate,
			EncoderGop:       s.EncoderGop,
		}
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	// Pipeline reconfiguration may take ~2-5s
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	resp, err := client.ReconfigurePipeline(ctx, &camerapb.ReconfigurePipelineRequest{
		Streams: grpcStreams,
	})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to reconfigure pipeline: "+err.Error())
		return
	}

	if !resp.Success {
		detail := resp.Message
		if resp.HalError != nil {
			detail = fmt.Sprintf("HAL error %s (%d): %s — %s", resp.HalError.Name, resp.HalError.Code, resp.HalError.Description, resp.Message)
		}
		Resp(c).FailTyped(CodeCameraError, "hal", detail)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.pipeline.reconfigured",
			eventLoggerPkg.MessageParams{"stream_count": fmt.Sprintf("%d", len(req.Streams))},
			getUsernameFromContext(c))
	}

	// Write applied streams back to YAML and reload in-memory stream list
	if resp.AppliedStreams != nil {
		go h.writeAppliedStreamsToConfig(resp.AppliedStreams)
		for _, s := range resp.AppliedStreams {
			if h.streamReloader != nil {
				h.streamReloader.UpdateStreamFromApplied(s.StreamId, s.EncoderWidth, s.EncoderHeight, s.Codec, s.EncoderBitrate, s.EncoderFramerate, s.EncoderGop)
			}
		}
	} else {
		// Fallback: full reload from YAML
		if h.streamReloader != nil {
			h.streamReloader.ReloadStreams(h.configPath)
		}
	}

	Resp(c).OK(gin.H{
		"message":      "Pipeline reconfigured successfully",
		"interrupt_ms": resp.InterruptMs,
	})
}

// GetStreamStatus returns real-time encoder status for all streams.
// Falls back to config-derived status if the daemon has not implemented the RPC yet.
// GET /api/v1/media/status
func (h *MediaHandlers) GetStreamStatus(c *gin.Context) {
	// Always read YAML config first to get full stream list (including disabled)
	allParams, _ := h.readAllEncoderParams()
	yamlMap := make(map[string]streamEncoderParams)
	for _, e := range allParams {
		yamlMap[e.StreamName] = e
	}

	if h.cameraClient != nil {
		client := camerapb.NewCameraControlClient(h.cameraClient)
		ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
		defer cancel()

		resp, err := client.GetStreamStatus(ctx, &camerapb.GetStreamStatusRequest{})
		if err == nil {
			streams := make([]gin.H, 0, len(resp.Streams)+len(allParams))
			seen := make(map[string]bool)
			for _, s := range resp.Streams {
				seen[s.StreamId] = true
				// camera-daemon uses UINT64_MAX as the "never / unknown" sentinel
				// for ms_since_last_frame; render that as JSON null instead of
				// the raw 18446744073709551615.
				var msSinceLastFrame interface{} = s.MsSinceLastFrame
				if s.MsSinceLastFrame == math.MaxUint64 {
					msSinceLastFrame = nil
				}
				streams = append(streams, gin.H{
					"stream_id":           s.StreamId,
					"status":              s.Status,
					"has_encoder":         s.HasEncoder,
					"codec":               s.Codec,
					"width":               s.Width,
					"height":              s.Height,
					"fps":                 s.Fps,
					"bitrate_bps":         s.BitrateBps,
					"gop":                 s.Gop,
					"ms_since_last_frame": msSinceLastFrame,
					"measured_fps":        s.MeasuredFps,
					"status_detail":       s.StatusDetail,
				})
			}
			// Add disabled streams from YAML that aren't in gRPC response
			for name, enc := range yamlMap {
				if !seen[name] {
					streams = append(streams, gin.H{
						"stream_id":           name,
						"status":              "inactive",
						"has_encoder":         false,
						"codec":               enc.Codec,
						"width":               enc.Width,
						"height":              enc.Height,
						"fps":                 enc.FPS,
						"bitrate_bps":         enc.Bitrate,
						"gop":                 enc.GOP,
						"ms_since_last_frame": nil,
						"measured_fps":        0,
						"status_detail":       "stream disabled in config",
					})
				}
			}
			Resp(c).OK(gin.H{"streams": streams})
			return
		}
		// RPC not implemented by daemon yet — fall through to config-derived status
	}

	// Fallback: derive status from YAML config
	h.getStreamStatusFromConfig(c)
}

// getStreamStatusFromConfig builds stream status from the YAML config file.
//
// This is the FALLBACK path, used only when camera-daemon is unreachable or has
// not implemented GetStreamStatus. It MUST NOT claim streams are "active": the
// YAML config only describes intent, not reality, and a stale "active" here is
// exactly what hid the post-profile-switch black screen (encoder present in
// config but producing zero frames). Report "unknown" so callers know the live
// state was not measured.
func (h *MediaHandlers) getStreamStatusFromConfig(c *gin.Context) {
	encoders := extractEncodersFromFile(h.configPath)
	streams := make([]gin.H, 0, len(encoders))
	for _, enc := range encoders {
		streams = append(streams, gin.H{
			"stream_id":           enc.StreamName,
			"status":              "unknown",
			"has_encoder":         false,
			"codec":               enc.Codec,
			"width":               enc.Width,
			"height":              enc.Height,
			"fps":                 enc.Fps,
			"bitrate_bps":         enc.Bitrate,
			"gop":                 enc.Gop,
			"ms_since_last_frame": nil,
			"measured_fps":        0,
			"status_detail":       "camera-daemon unreachable; status derived from config only",
		})
	}
	Resp(c).OK(gin.H{"streams": streams})
}

// extractEncodersFromFile reads encoder entries from a YAML config file.
func extractEncodersFromFile(path string) []encoderParams {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		return nil
	}
	return extractEncoders(config)
}

// writeAppliedStreamsToConfig writes all applied encoder params back to the YAML config file
// in a single read-modify-write cycle to avoid concurrent goroutine races.
func (h *MediaHandlers) writeAppliedStreamsToConfig(streams []*camerapb.PipelineStreamConfig) {
	data, err := os.ReadFile(h.configPath)
	if err != nil {
		return
	}

	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		return
	}

	encoders, ok := config["encoders"].([]interface{})
	if !ok {
		return
	}

	for _, s := range streams {
		for _, item := range encoders {
			m, ok := item.(map[string]interface{})
			if !ok {
				continue
			}
			if getFieldString(m, "stream_name") != s.StreamId {
				continue
			}
			if s.EncoderWidth > 0 {
				m["width"] = s.EncoderWidth
			}
			if s.EncoderHeight > 0 {
				m["height"] = s.EncoderHeight
			}
			if s.Codec != "" {
				m["codec"] = s.Codec
			}
			if s.EncoderBitrate > 0 {
				m["bitrate"] = s.EncoderBitrate
			}
			if s.EncoderFramerate > 0 {
				m["fps"] = s.EncoderFramerate
			}
			if s.EncoderGop > 0 {
				m["gop"] = s.EncoderGop
			}
			break
		}
	}

	outData, err := marshalMediaConfig(config)
	if err != nil {
		return
	}
	_ = h.projectMediaConfig(context.Background(), "", string(outData))
}

// canonicalEncoderDims returns the landscape (sensor-native) width/height for an
// encoder, swapping a portrait-transposed pair back to landscape.
//
// Why: this platform's sensor is natively landscape (e.g. 3840×2160). Portrait
// output is produced by a runtime rotation transform (rotation 90/270) that the
// HAL applies by swapping input_stream W↔H in the medialib config *after*
// pipeline init — it is never meant to be baked into the encoder dimensions
// stored in camera-daemon.yaml. While portrait, the running pipeline reports
// rotated dims, and a naive persistence of those dims (writeStreamToConfig)
// left portrait (H>W) values in the YAML. On the next boot the pipeline comes up
// landscape but HAL patches encoders to the stale portrait dims → portrait
// encoders fed landscape frames → add_buffer fails for every video sink → /media
// black screen (audio unaffected, separate path).
//
// Guard: the YAML encoder dims are the canonical landscape geometry. A stored
// pair with height>width is the rotation-residual signature, so swap it back to
// width>=height before persisting. Square pairs (width==height) are left alone.
func canonicalEncoderDims(width, height uint32) (uint32, uint32) {
	if height > width {
		return height, width
	}
	return width, height
}

// writeStreamToConfig writes a single encoder's updated parameters back to the YAML config file.
func (h *MediaHandlers) writeStreamToConfig(streamName string, width, height uint32, codec string, bitrate, fps, gop uint32) {
	h.configMu.Lock()
	defer h.configMu.Unlock()

	data, err := os.ReadFile(h.configPath)
	if err != nil {
		return
	}

	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		return
	}

	encoders, ok := config["encoders"].([]interface{})
	if !ok {
		return
	}

	for _, item := range encoders {
		m, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		if getFieldString(m, "stream_name") != streamName {
			continue
		}
		// Persist canonical landscape dims: swap a portrait-transposed pair
		// (height>width, the rotation-residual signature) back to landscape so a
		// stale portrait dim set never survives into the next boot. Only
		// normalize when both dims are present; 0 here means "leave unchanged".
		if width > 0 && height > 0 {
			width, height = canonicalEncoderDims(width, height)
		}
		if width > 0 {
			m["width"] = int(width)
		}
		if height > 0 {
			m["height"] = int(height)
		}
		if codec != "" {
			m["codec"] = codec
		}
		if bitrate > 0 {
			m["bitrate"] = int(bitrate)
		}
		if fps > 0 {
			m["fps"] = int(fps)
		}
		if gop > 0 {
			m["gop"] = int(gop)
		}
		break
	}

	outData, err := marshalMediaConfig(config)
	if err != nil {
		return
	}
	_ = h.projectMediaConfig(context.Background(), "", string(outData))
}

// AddStream dynamically adds a new stream via gRPC
// POST /api/v1/media/streams
func (h *MediaHandlers) AddStream(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	var req struct {
		StreamID string `json:"stream_id" binding:"required"`
		Width    uint32 `json:"width" binding:"required"`
		Height   uint32 `json:"height" binding:"required"`
		FPS      uint32 `json:"fps" binding:"required"`
		Codec    string `json:"codec"`
		Bitrate  uint32 `json:"bitrate"`
		GOP      uint32 `json:"gop"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	if err := ValidateAddStream(req.StreamID, req.Width, req.Height, req.FPS, req.Bitrate, req.GOP, req.Codec); err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", err.Error())
		return
	}

	codec := req.Codec
	if codec == "" {
		codec = "h264"
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.AddStream(ctx, &camerapb.AddStreamRequest{
		StreamId: req.StreamID,
		Width:    req.Width,
		Height:   req.Height,
		Fps:      req.FPS,
		Codec:    codec,
		Bitrate:  req.Bitrate,
		Gop:      req.GOP,
	})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to add stream: "+err.Error())
		return
	}

	if !resp.Success {
		detail := resp.Message
		if resp.HalError != nil {
			detail = fmt.Sprintf("HAL error %s (%d): %s - %s", resp.HalError.Name, resp.HalError.Code, resp.HalError.Description, resp.Message)
		}
		Resp(c).FailTyped(CodeCameraError, "hal", detail)
		return
	}

	// Write new stream to YAML config and reload (sync, after gRPC success)
	h.addStreamToConfig(req.StreamID, req.Width, req.Height, req.FPS, codec, req.Bitrate, req.GOP)
	RegisterStreamRule(req.StreamID, StreamValidationRule{
		MinWidth: 64, MaxWidth: req.Width,
		MinHeight: 64, MaxHeight: req.Height,
		WidthAlign: 2, HeightAlign: 2,
		MinBitrate: 32000, MaxBitrate: 10000000,
		MinFps: 1, MaxFps: req.FPS,
		MinGop: 1, MaxGop: 300,
		AllowedCodecs: []string{codec},
	})
	if h.streamReloader != nil {
		h.streamReloader.ReloadStreams(h.configPath)
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.stream.added",
			eventLoggerPkg.MessageParams{"stream": req.StreamID},
			getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"message": "Stream added successfully", "stream_id": req.StreamID})
}

// RemoveStream dynamically removes a stream via gRPC
// DELETE /api/v1/media/streams/:name
func (h *MediaHandlers) RemoveStream(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	streamName := c.Param("name")
	if streamName == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Stream name is required")
		return
	}
	if streamName == "main" {
		Resp(c).FailMsg(CodeInvalidRequest, "Cannot remove main stream")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.RemoveStream(ctx, &camerapb.RemoveStreamRequest{
		StreamName: streamName,
	})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to remove stream: "+err.Error())
		return
	}

	if !resp.Success {
		detail := resp.Message
		if resp.HalError != nil {
			detail = fmt.Sprintf("HAL error %s (%d): %s - %s", resp.HalError.Name, resp.HalError.Code, resp.HalError.Description, resp.Message)
		}
		Resp(c).FailTyped(CodeCameraError, "hal", detail)
		return
	}

	// Remove stream from YAML config and reload
	h.removeStreamFromConfig(streamName)
	RemoveStreamRule(streamName)
	if h.streamReloader != nil {
		h.streamReloader.ReloadStreams(h.configPath)
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.stream.removed",
			eventLoggerPkg.MessageParams{"stream": streamName},
			getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"message": "Stream removed successfully", "stream": streamName})
}

// addStreamToConfig appends a new encoder entry to the YAML config file.
func (h *MediaHandlers) addStreamToConfig(streamID string, width, height, fps uint32, codec string, bitrate, gop uint32) {
	h.configMu.Lock()
	defer h.configMu.Unlock()

	data, err := os.ReadFile(h.configPath)
	if err != nil {
		return
	}

	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		return
	}

	encoders, ok := config["encoders"].([]interface{})
	if !ok {
		encoders = []interface{}{}
	}

	newEnc := map[string]interface{}{
		"stream_name": streamID,
		"codec":       codec,
		"width":       width,
		"height":      height,
		"fps":         fps,
		"bitrate":     bitrate,
		"gop":         gop,
	}
	config["encoders"] = append(encoders, newEnc)

	outData, err := marshalMediaConfig(config)
	if err != nil {
		return
	}
	_ = h.projectMediaConfig(context.Background(), "", string(outData))
}

// removeStreamFromConfig removes an encoder entry from the YAML config file.
func (h *MediaHandlers) removeStreamFromConfig(streamName string) {
	h.configMu.Lock()
	defer h.configMu.Unlock()

	data, err := os.ReadFile(h.configPath)
	if err != nil {
		return
	}

	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		return
	}

	encoders, ok := config["encoders"].([]interface{})
	if !ok {
		return
	}

	var filtered []interface{}
	for _, item := range encoders {
		m, ok := item.(map[string]interface{})
		if !ok {
			filtered = append(filtered, item)
			continue
		}
		if getFieldString(m, "stream_name") == streamName {
			continue
		}
		filtered = append(filtered, item)
	}

	if len(filtered) == len(encoders) {
		return
	}

	config["encoders"] = filtered
	outData, err := marshalMediaConfig(config)
	if err != nil {
		return
	}
	_ = h.projectMediaConfig(context.Background(), "", string(outData))
}

// ==========================================
// Stream Enable / Disable via ReconfigurePipeline
// ==========================================

// streamEncoderParams holds the params needed to build a PipelineStreamConfig from YAML.
type streamEncoderParams struct {
	StreamName string
	Codec      string
	Width      uint32
	Height     uint32
	FPS        uint32
	Bitrate    uint32
	GOP        uint32
	Enabled    bool
}

// readAllEncoderParams loads all encoder entries (with enabled flag) from the YAML config.
func (h *MediaHandlers) readAllEncoderParams() ([]streamEncoderParams, error) {
	data, err := os.ReadFile(h.configPath)
	if err != nil {
		return nil, err
	}
	var raw struct {
		Encoders []struct {
			StreamName string `yaml:"stream_name"`
			Codec      string `yaml:"codec"`
			Width      uint32 `yaml:"width"`
			Height     uint32 `yaml:"height"`
			FPS        uint32 `yaml:"fps"`
			Bitrate    uint32 `yaml:"bitrate"`
			GOP        uint32 `yaml:"gop"`
			Enabled    *bool  `yaml:"enabled"`
		} `yaml:"encoders"`
	}
	if err := yaml.Unmarshal(data, &raw); err != nil {
		return nil, err
	}
	result := make([]streamEncoderParams, 0, len(raw.Encoders))
	for _, e := range raw.Encoders {
		enabled := true
		if e.Enabled != nil {
			enabled = *e.Enabled
		}
		result = append(result, streamEncoderParams{
			StreamName: e.StreamName,
			Codec:      e.Codec,
			Width:      e.Width,
			Height:     e.Height,
			FPS:        e.FPS,
			Bitrate:    e.Bitrate,
			GOP:        e.GOP,
			Enabled:    enabled,
		})
	}
	return result, nil
}

// setStreamEnabledInConfig sets the enabled flag on a specific encoder entry in the YAML
// without deleting the entry or changing any other parameters.
func (h *MediaHandlers) setStreamEnabledInConfig(streamName string, enabled bool) {
	h.configMu.Lock()
	defer h.configMu.Unlock()

	data, err := os.ReadFile(h.configPath)
	if err != nil {
		return
	}
	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		return
	}
	encoders, ok := config["encoders"].([]interface{})
	if !ok {
		return
	}
	for _, item := range encoders {
		m, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		if getFieldString(m, "stream_name") == streamName {
			m["enabled"] = enabled
			break
		}
	}
	outData, err := marshalMediaConfig(config)
	if err != nil {
		return
	}
	_ = h.projectMediaConfig(context.Background(), "", string(outData))
}

// EnableStream enables a stopped stream by calling AddStream gRPC.
// The camera-daemon will use HAL add_codec_stream (incremental, no interruption to other streams).
// Parameters are read from YAML where they were preserved by the last DisableStream call.
// POST /api/v1/media/streams/:name/enable
func (h *MediaHandlers) EnableStream(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	streamName := c.Param("name")
	if streamName == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Stream name is required")
		return
	}
	if streamName == "main" {
		Resp(c).FailMsg(CodeInvalidRequest, "Main stream is always enabled")
		return
	}

	// Load all encoder params from YAML
	encoders, err := h.readAllEncoderParams()
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to read stream config: "+err.Error())
		return
	}

	// Verify stream exists and find its saved parameters
	var target *streamEncoderParams
	for i := range encoders {
		if encoders[i].StreamName == streamName {
			target = &encoders[i]
			break
		}
	}
	if target == nil {
		Resp(c).FailMsg(CodeNotFound, "Stream not found in config: "+streamName)
		return
	}

	// Check if the stream is actually running via gRPC, not just the YAML flag.
	// The YAML enabled field defaults to true when absent, so a stream that was
	// never explicitly disabled will have enabled=true in YAML but may not be
	// running (e.g. hardware profile didn't start it).
	running := h.getRunningStreamParams(streamName)
	if running != nil {
		Resp(c).OK(gin.H{"message": "Stream is already enabled", "stream": streamName})
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	// Incremental add: no pipeline restart; typically completes in <500ms
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	resp, err := client.AddStream(ctx, &camerapb.AddStreamRequest{
		StreamId: streamName,
		Codec:    target.Codec,
		Width:    target.Width,
		Height:   target.Height,
		Fps:      target.FPS,
		Bitrate:  target.Bitrate,
		Gop:      target.GOP,
	})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to enable stream: "+err.Error())
		return
	}
	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, resp.Message)
		return
	}

	// Persist enabled state and reload in-memory stream list.
	// Restart the H264 readLoop so it reconnects immediately instead of
	// sitting in exponential backoff from when the socket was missing.
	h.setStreamEnabledInConfig(streamName, true)
	if h.streamReloader != nil {
		h.streamReloader.RestartH264Stream(streamName)
		h.streamReloader.ReloadStreams(h.configPath)
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.stream.enabled",
			eventLoggerPkg.MessageParams{"stream": streamName},
			getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{
		"message": "Stream enabled successfully (incremental, no interruption to other streams)",
		"stream":  streamName,
	})
}

// DisableStream stops a stream using RemoveStream gRPC.
// The camera-daemon will use HAL remove_codec_stream (incremental, no interruption to other streams).
// Parameters are preserved in YAML (enabled: false) so they survive restarts and can be restored.
// DELETE /api/v1/media/streams/:name/disable
func (h *MediaHandlers) DisableStream(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	streamName := c.Param("name")
	if streamName == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Stream name is required")
		return
	}
	if streamName == "main" {
		Resp(c).FailMsg(CodeInvalidRequest, "Main stream cannot be disabled")
		return
	}

	// Load all encoder params from YAML
	encoders, err := h.readAllEncoderParams()
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to read stream config: "+err.Error())
		return
	}

	// Verify stream exists in config
	found := false
	for _, e := range encoders {
		if e.StreamName == streamName {
			found = true
			break
		}
	}
	if !found {
		Resp(c).FailMsg(CodeNotFound, "Stream not found in config: "+streamName)
		return
	}

	// Check if the stream is actually running via gRPC.
	running := h.getRunningStreamParams(streamName)
	if running == nil {
		Resp(c).OK(gin.H{"message": "Stream is already disabled", "stream": streamName})
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	// Incremental remove: no pipeline restart; completes quickly
	ctx, cancel := context.WithTimeout(context.Background(), 15*time.Second)
	defer cancel()

	resp, err := client.RemoveStream(ctx, &camerapb.RemoveStreamRequest{
		StreamName: streamName,
	})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to disable stream: "+err.Error())
		return
	}
	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, resp.Message)
		return
	}

	// Persist disabled state (keep all params!) and reload in-memory stream list
	h.setStreamEnabledInConfig(streamName, false)
	if h.streamReloader != nil {
		h.streamReloader.RestartH264Stream(streamName)
		h.streamReloader.ReloadStreams(h.configPath)
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.stream.disabled",
			eventLoggerPkg.MessageParams{"stream": streamName},
			getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{
		"message": "Stream disabled successfully — parameters preserved in config",
		"stream":  streamName,
	})
}

// runningStreamParams holds the current running parameters of a stream from gRPC.
type runningStreamParams struct {
	Width    uint32
	Height   uint32
	Fps      uint32
	Bitrate  uint32
	Codec    string
	StreamId string
	Gop      uint32
}

// getRunningStreamParams queries the running pipeline via gRPC GetStreamStatus
// and returns the current parameters for the named stream. Returns nil if the
// stream is not found or gRPC fails — callers should fall through to YAML comparison.
func (h *MediaHandlers) getRunningStreamParams(streamName string) *runningStreamParams {
	all := h.getAllRunningStreamParams()
	return all[streamName]
}

// getAllRunningStreamParams returns a map of all running stream params from gRPC GetStreamStatus.
// Returns nil if gRPC fails.
func (h *MediaHandlers) getAllRunningStreamParams() map[string]*runningStreamParams {
	result := make(map[string]*runningStreamParams)
	if h.cameraClient == nil {
		return nil
	}
	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.GetStreamStatus(ctx, &camerapb.GetStreamStatusRequest{})
	if err != nil {
		return nil
	}
	for _, s := range resp.Streams {
		// Only include streams that are actually running (have an active encoder).
		// Stopped streams exist in config but are not backed by hardware.
		if !s.HasEncoder || s.Status != "active" {
			continue
		}
		result[s.StreamId] = &runningStreamParams{
			StreamId: s.StreamId,
			Width:    s.Width,
			Height:   s.Height,
			Fps:      s.Fps,
			Bitrate:  s.BitrateBps,
			Codec:    s.Codec,
			Gop:      s.Gop,
		}
	}
	if len(result) == 0 {
		return nil
	}
	return result
}

// buildPipelineStreamConfigs builds PipelineStreamConfig entries for all streams,
// preferring RUNNING config from gRPC and falling back to YAML.
// Override params (non-zero values) are applied to the target stream.
func (h *MediaHandlers) buildPipelineStreamConfigs(targetStream string, width, height uint32, codec string, bitrate, fps, gop uint32) []*camerapb.PipelineStreamConfig {
	// Try running config first
	running := h.getAllRunningStreamParams()
	if running != nil && len(running) > 0 {
		// Use YAML order as the canonical stream order so that index i maps
		// to sink{i} correctly in camera-daemon.
		yamlEncoders := extractEncodersFromFile(h.configPath)
		streams := make([]*camerapb.PipelineStreamConfig, 0, len(running))
		for _, ye := range yamlEncoders {
			r, ok := running[ye.StreamName]
			if !ok {
				continue // stream not running, skip
			}
			s := &camerapb.PipelineStreamConfig{
				StreamId:         r.StreamId,
				InputWidth:       r.Width,
				InputHeight:      r.Height,
				InputFramerate:   r.Fps,
				Codec:            r.Codec,
				EncoderWidth:     r.Width,
				EncoderHeight:    r.Height,
				EncoderFramerate: r.Fps,
				EncoderBitrate:   r.Bitrate,
				EncoderGop:       r.Gop,
			}
			// Apply overrides to target stream
			if r.StreamId == targetStream {
				if width > 0 {
					s.InputWidth = width
					s.EncoderWidth = width
				}
				if height > 0 {
					s.InputHeight = height
					s.EncoderHeight = height
				}
				if codec != "" {
					s.Codec = codec
				}
				if bitrate > 0 {
					s.EncoderBitrate = bitrate
				}
				if fps > 0 {
					s.InputFramerate = fps
					s.EncoderFramerate = fps
				}
				if gop > 0 {
					s.EncoderGop = gop
				}
			}
			streams = append(streams, s)
		}
		return streams
	}

	// Fallback to YAML
	encoders := extractEncodersFromFile(h.configPath)
	if len(encoders) == 0 {
		return nil
	}
	streams := make([]*camerapb.PipelineStreamConfig, 0, len(encoders))
	for _, enc := range encoders {
		s := &camerapb.PipelineStreamConfig{
			StreamId:         enc.StreamName,
			InputWidth:       enc.Width,
			InputHeight:      enc.Height,
			InputFramerate:   enc.Fps,
			Codec:            enc.Codec,
			EncoderWidth:     enc.Width,
			EncoderHeight:    enc.Height,
			EncoderFramerate: enc.Fps,
			EncoderBitrate:   enc.Bitrate,
			EncoderGop:       enc.Gop,
		}
		if enc.StreamName == targetStream {
			if width > 0 {
				s.InputWidth = width
				s.EncoderWidth = width
			}
			if height > 0 {
				s.InputHeight = height
				s.EncoderHeight = height
			}
			if codec != "" {
				s.Codec = codec
			}
			if bitrate > 0 {
				s.EncoderBitrate = bitrate
			}
			if fps > 0 {
				s.InputFramerate = fps
				s.EncoderFramerate = fps
			}
			if gop > 0 {
				s.EncoderGop = gop
			}
		}
		streams = append(streams, s)
	}
	return streams
}

// ==================== Privacy Mask ====================

func (h *MediaHandlers) GetPrivacyMaskConfig(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.GetPrivacyMaskConfig(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to get privacy mask config: "+err.Error())
		return
	}

	regions := make([]gin.H, 0, len(resp.Regions))
	for _, r := range resp.Regions {
		px := make([]float32, len(r.PointsX))
		py := make([]float32, len(r.PointsY))
		for i, v := range r.PointsX {
			px[i] = v
		}
		for i, v := range r.PointsY {
			py[i] = v
		}
		regions = append(regions, gin.H{
			"id":       r.Id,
			"name":     r.Name,
			"enabled":  r.Enabled,
			"points_x": px,
			"points_y": py,
		})
	}

	Resp(c).OK(gin.H{
		"enabled":     resp.Enabled,
		"color":       resp.Color,
		"blur_radius": resp.BlurRadius,
		"regions":     regions,
		"dpm_enabled": resp.DpmEnabled,
		"dpm_labels":  resp.DpmLabels,
		"dpm_mode":    resp.DpmMode,
		"dpm_color":   resp.DpmColor,
	})
}

func (h *MediaHandlers) UpdatePrivacyMaskConfig(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeCameraError, "Camera control service is not available")
		return
	}
	if !requireJSONContentType(c) {
		return
	}

	type privacyMaskRegionUpdate struct {
		ID           string    `json:"id"`
		Name         string    `json:"name"`
		Enabled      *bool     `json:"enabled"`
		PointsX      []float32 `json:"points_x"`
		PointsXCamel []float32 `json:"pointsX"`
		PointsY      []float32 `json:"points_y"`
		PointsYCamel []float32 `json:"pointsY"`
	}
	var req struct {
		Enabled         *bool                     `json:"enabled"`
		Color           *uint32                   `json:"color"`
		BlurRadius      *int32                    `json:"blur_radius"`
		BlurRadiusCamel *int32                    `json:"blurRadius"`
		Regions         []privacyMaskRegionUpdate `json:"regions"`
		DpmEnabled      *bool                     `json:"dpm_enabled"`
		DpmEnabledCamel *bool                     `json:"dpmEnabled"`
		DpmLabels       *string                   `json:"dpm_labels"`
		DpmLabelsCamel  *string                   `json:"dpmLabels"`
		DpmMode         *string                   `json:"dpm_mode"`
		DpmModeCamel    *string                   `json:"dpmMode"`
		DpmColor        *uint32                   `json:"dpm_color"`
		DpmColorCamel   *uint32                   `json:"dpmColor"`
	}

	if err := decodeStrictJSONBody(c, &req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	blurRadius, err := mergeInt32JSONAlias("blur_radius", req.BlurRadius, req.BlurRadiusCamel)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}
	dpmEnabled, err := mergeBoolJSONAlias("dpm_enabled", req.DpmEnabled, req.DpmEnabledCamel)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}
	dpmLabels, err := mergeStringJSONAlias("dpm_labels", req.DpmLabels, req.DpmLabelsCamel)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}
	dpmMode, err := mergeStringJSONAlias("dpm_mode", req.DpmMode, req.DpmModeCamel)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}
	dpmColor, err := mergeUint32JSONAlias("dpm_color", req.DpmColor, req.DpmColorCamel)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	if req.Enabled == nil && req.Color == nil && blurRadius == nil && req.Regions == nil &&
		dpmEnabled == nil && dpmLabels == nil && dpmMode == nil && dpmColor == nil {
		Resp(c).FailMsg(CodeInvalidRequest, "No supported privacy mask fields were provided")
		return
	}

	if blurRadius != nil && (*blurRadius < 0 || *blurRadius > 64) {
		Resp(c).FailMsg(CodeInvalidRequest, "blur_radius must be in [0, 64]")
		return
	}
	if dpmMode != nil {
		switch *dpmMode {
		case "", "mosaic", "blur", "overlay":
		default:
			Resp(c).FailMsg(CodeInvalidRequest, "dpm_mode must be one of: mosaic, blur, overlay")
			return
		}
	}
	if dpmColor != nil && *dpmColor > 0xFFFFFF {
		Resp(c).FailMsg(CodeInvalidRequest, "dpm_color must be a 24-bit RGB value")
		return
	}

	for _, r := range req.Regions {
		pointsX, err := mergeFloat32SliceJSONAlias("points_x", r.PointsX, r.PointsXCamel)
		if err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, err.Error())
			return
		}
		pointsY, err := mergeFloat32SliceJSONAlias("points_y", r.PointsY, r.PointsYCamel)
		if err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, err.Error())
			return
		}
		if len(pointsX) > 8 || len(pointsY) > 8 {
			Resp(c).FailMsg(CodeInvalidRequest, "Each region must have at most 8 vertices")
			return
		}
		for _, v := range pointsX {
			if v < 0.0 || v > 1.0 {
				Resp(c).FailMsg(CodeInvalidRequest, "points_x must be in [0.0, 1.0]")
				return
			}
		}
		for _, v := range pointsY {
			if v < 0.0 || v > 1.0 {
				Resp(c).FailMsg(CodeInvalidRequest, "points_y must be in [0.0, 1.0]")
				return
			}
		}
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	cur, err := client.GetPrivacyMaskConfig(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to read current privacy mask config: "+err.Error())
		return
	}

	protoReq := cur
	if req.Enabled != nil {
		protoReq.Enabled = *req.Enabled
	}
	if req.Color != nil {
		protoReq.Color = *req.Color
	}
	if blurRadius != nil {
		protoReq.BlurRadius = *blurRadius
	}
	if dpmEnabled != nil {
		protoReq.DpmEnabled = *dpmEnabled
	}
	if dpmLabels != nil {
		protoReq.DpmLabels = *dpmLabels
	}
	if dpmMode != nil {
		protoReq.DpmMode = *dpmMode
	}
	if dpmColor != nil {
		protoReq.DpmColor = *dpmColor
	}
	if req.Regions != nil {
		protoReq.Regions = make([]*camerapb.PrivacyMaskRegion, len(req.Regions))
		for i, r := range req.Regions {
			pointsX, _ := mergeFloat32SliceJSONAlias("points_x", r.PointsX, r.PointsXCamel)
			pointsY, _ := mergeFloat32SliceJSONAlias("points_y", r.PointsY, r.PointsYCamel)
			region := &camerapb.PrivacyMaskRegion{
				Id:      r.ID,
				Name:    r.Name,
				PointsX: pointsX,
				PointsY: pointsY,
			}
			if r.Enabled != nil {
				region.Enabled = *r.Enabled
			}
			protoReq.Regions[i] = region
		}
	}

	resp, err := client.SetPrivacyMaskConfig(ctx, protoReq)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to update privacy mask config: "+err.Error())
		return
	}

	if !resp.Success {
		Resp(c).FailMsg(CodeCameraError, "Camera daemon rejected privacy mask update: "+resp.Message)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.changed", eventLoggerPkg.MessageParams{"stream": "main", "changes": "Privacy mask config updated"}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"message": "Privacy mask config applied successfully"})
}

func mergeBoolJSONAlias(name string, snake, camel *bool) (*bool, error) {
	if snake != nil && camel != nil && *snake != *camel {
		return nil, fmt.Errorf("%s and its camelCase alias disagree", name)
	}
	if snake != nil {
		return snake, nil
	}
	return camel, nil
}

func mergeStringJSONAlias(name string, snake, camel *string) (*string, error) {
	if snake != nil && camel != nil && *snake != *camel {
		return nil, fmt.Errorf("%s and its camelCase alias disagree", name)
	}
	if snake != nil {
		return snake, nil
	}
	return camel, nil
}

func mergeInt32JSONAlias(name string, snake, camel *int32) (*int32, error) {
	if snake != nil && camel != nil && *snake != *camel {
		return nil, fmt.Errorf("%s and its camelCase alias disagree", name)
	}
	if snake != nil {
		return snake, nil
	}
	return camel, nil
}

func mergeUint32JSONAlias(name string, snake, camel *uint32) (*uint32, error) {
	if snake != nil && camel != nil && *snake != *camel {
		return nil, fmt.Errorf("%s and its camelCase alias disagree", name)
	}
	if snake != nil {
		return snake, nil
	}
	return camel, nil
}

func mergeFloat32SliceJSONAlias(name string, snake, camel []float32) ([]float32, error) {
	if snake != nil && camel != nil && !reflect.DeepEqual(snake, camel) {
		return nil, fmt.Errorf("%s and its camelCase alias disagree", name)
	}
	if snake != nil {
		return snake, nil
	}
	return camel, nil
}
