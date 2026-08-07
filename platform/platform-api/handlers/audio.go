package handlers

import (
	"aipc/platform/common/constants"
	"context"
	"net/http"
	"os"
	"sync"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"
	"gopkg.in/yaml.v3"

	camerapb "aipc/platform/camera-daemon/proto"
	"aipc/platform/common/logger"
	"aipc/platform/platform-api/config"
	"google.golang.org/grpc"
)

// AudioHandlers proxies audio REST endpoints to camera-daemon gRPC
// and streams live audio via WebSocket.
type AudioHandlers struct {
	cameraClient *grpc.ClientConn
	audioStreams *AudioStreamManager

	// configPath / configMu / configMgr mirror MediaHandlers so audio config
	// changes can be persisted to camera-daemon.yaml (the media/config domain)
	// and survive a daemon restart. In production configMgr is non-nil and the
	// Config Controller serializes applies internally; configMu only guards
	// the read-modify-write of the local fallback path.
	configPath string
	configMu   sync.Mutex
	configMgr  *config.Manager

	// talkClientOverride, when set, replaces the real camera-daemon client for
	// the /audio/talk handler. Used only by tests; nil in production.
	talkClientOverride talkPlaybackClient
}

// NewAudioHandlers creates audio handlers with a camera-daemon gRPC connection
// and the shared media config path/manager for persistence.
func NewAudioHandlers(cameraClient *grpc.ClientConn, configPath string, configMgr *config.Manager) *AudioHandlers {
	if configPath == "" {
		configPath = constants.ConfigPath() + "/camera-daemon.yaml"
	}
	return &AudioHandlers{
		cameraClient: cameraClient,
		audioStreams: NewAudioStreamManager(),
		configPath:   configPath,
		configMgr:    configMgr,
	}
}

// newAudioHandlersWithTalk builds an AudioHandlers whose /audio/talk handler
// uses the provided playback client instead of a real gRPC connection. For
// tests only.
func newAudioHandlersWithTalk(talk talkPlaybackClient) *AudioHandlers {
	return &AudioHandlers{
		cameraClient:       nil,
		audioStreams:       NewAudioStreamManager(),
		talkClientOverride: talk,
	}
}

const audioTimeout = 5 * time.Second

// ListCaptureDevices returns available audio capture devices.
// GET /api/v1/audio/capture-devices
func (h *AudioHandlers) ListCaptureDevices(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Camera Control service not available")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), audioTimeout)
	defer cancel()

	resp, err := client.ListAudioCaptureDevices(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "ListAudioCaptureDevices: "+err.Error())
		return
	}

	devices := make([]gin.H, 0, len(resp.GetDevices()))
	for _, d := range resp.GetDevices() {
		devices = append(devices, gin.H{
			"name":        d.GetName(),
			"description": d.GetDescription(),
		})
	}
	Resp(c).OK(gin.H{"devices": devices})
}

// ListPlaybackDevices returns available audio playback devices.
// GET /api/v1/audio/playback-devices
func (h *AudioHandlers) ListPlaybackDevices(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Camera Control service not available")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), audioTimeout)
	defer cancel()

	resp, err := client.ListAudioPlaybackDevices(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "ListAudioPlaybackDevices: "+err.Error())
		return
	}

	devices := make([]gin.H, 0, len(resp.GetDevices()))
	for _, d := range resp.GetDevices() {
		devices = append(devices, gin.H{
			"name":        d.GetName(),
			"description": d.GetDescription(),
		})
	}
	Resp(c).OK(gin.H{"devices": devices})
}

// GetStatus returns current audio capture/playback status.
// GET /api/v1/audio/status
func (h *AudioHandlers) GetStatus(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Camera Control service not available")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), audioTimeout)
	defer cancel()

	resp, err := client.GetAudioStatus(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "GetAudioStatus: "+err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"capturing":        resp.GetCapturing(),
		"playing":          resp.GetPlaying(),
		"device":           resp.GetDevice(),
		"sample_rate":      resp.GetSampleRate(),
		"channels":         resp.GetChannels(),
		"codec":            resp.GetCodec(),
		"volume":           resp.GetVolume(),
		"mute":             resp.GetMute(),
		"playback_enabled": h.loadPlaybackEnabled(),
	})
}

// StartCapture starts audio capture with optional configuration.
// POST /api/v1/audio/capture/start
func (h *AudioHandlers) StartCapture(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Camera Control service not available")
		return
	}

	var req struct {
		Device     string   `json:"device"`
		SampleRate uint32   `json:"sample_rate"`
		Channels   uint32   `json:"channels"`
		Codec      string   `json:"codec"`
		Bitrate    uint32   `json:"bitrate"`
		Volume     *float32 `json:"volume"` // nil = omitted by client = no change
		Mute       *bool    `json:"mute"`   // nil = omitted by client = no change
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), audioTimeout)
	defer cancel()

	protoReq := &camerapb.AudioConfigRequest{
		Device:     req.Device,
		SampleRate: req.SampleRate,
		Channels:   req.Channels,
		Codec:      req.Codec,
		Bitrate:    req.Bitrate,
	}
	if req.Volume != nil {
		protoReq.Volume = req.Volume
	}
	if req.Mute != nil {
		protoReq.Mute = req.Mute
	}

	resp, err := client.StartAudioCapture(ctx, protoReq)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "StartAudioCapture: "+err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeCameraError, resp.GetMessage())
		return
	}
	Resp(c).OK(gin.H{"message": "Audio capture started"})
}

// StopCapture stops audio capture.
// POST /api/v1/audio/capture/stop
func (h *AudioHandlers) StopCapture(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Camera Control service not available")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), audioTimeout)
	defer cancel()

	resp, err := client.StopAudioCapture(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "StopAudioCapture: "+err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeCameraError, resp.GetMessage())
		return
	}
	Resp(c).OK(gin.H{"message": "Audio capture stopped"})
}

// audioConfigReq is the shared shape of the StartCapture/SetConfig request
// bodies. Volume/Mute/PlaybackEnabled are pointers: nil means the client
// omitted the field so the device (and persistence) must not touch it.
type audioConfigReq struct {
	Device          string   `json:"device"`
	SampleRate      uint32   `json:"sample_rate"`
	Channels        uint32   `json:"channels"`
	Codec           string   `json:"codec"`
	Bitrate         uint32   `json:"bitrate"`
	Volume          *float32 `json:"volume"`
	Mute            *bool    `json:"mute"`
	PlaybackEnabled *bool    `json:"playback_enabled"`
}

// projectAudioConfig persists the marshaled camera-daemon.yaml through the
// Config Controller (media/config domain) when available, falling back to a
// direct os.WriteFile. Mirrors MediaHandlers.projectMediaConfig.
func (h *AudioHandlers) projectAudioConfig(ctx context.Context, actor, yamlStr string) error {
	if h.configMgr != nil {
		if _, _, err := h.configMgr.Apply(ctx, "media", "config", yamlStr, actor); err != nil {
			logger.Warn("audio manager apply failed, falling back to direct write: %v", err)
		} else {
			return nil
		}
	}
	return os.WriteFile(h.configPath, []byte(yamlStr), 0644)
}

// writeAudioConfig persists audio settings to camera-daemon.yaml so they
// survive a daemon restart. Device/SampleRate/Channels/Codec/Bitrate are proto3
// value types, so a partial request (e.g. only volume) would zero them — gate
// each on non-zero/non-empty to avoid corrupting fields the caller didn't
// touch. Volume/Mute are pointers: nil means no change. audio.enabled is
// preserved by the read-modify-write of the whole map. Called only after the
// gRPC SetAudioConfig succeeds.
func (h *AudioHandlers) writeAudioConfig(ctx context.Context, actor string, req audioConfigReq) {
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

	audio, ok := config["audio"].(map[string]interface{})
	if !ok {
		audio = make(map[string]interface{})
		config["audio"] = audio
	}
	if req.Device != "" {
		audio["capture_device"] = req.Device
	}
	if req.SampleRate != 0 {
		audio["sample_rate"] = int(req.SampleRate)
	}
	if req.Channels != 0 {
		audio["channels"] = int(req.Channels)
	}
	if req.Codec != "" {
		audio["codec"] = req.Codec
	}
	if req.Bitrate != 0 {
		audio["bitrate"] = int(req.Bitrate)
	}
	if req.Volume != nil {
		audio["volume"] = *req.Volume
	}
	if req.Mute != nil {
		audio["mute"] = *req.Mute
	}
	if req.PlaybackEnabled != nil {
		audio["playback_enabled"] = *req.PlaybackEnabled
	}

	outData, err := marshalMediaConfig(config)
	if err != nil {
		return
	}
	_ = h.projectAudioConfig(ctx, actor, string(outData))
}

// loadPlaybackEnabled reads the persisted audio.playback_enabled flag from
// camera-daemon.yaml. Returns true when the field is absent (speaker enabled
// by default) so push-to-talk works out of the box until the Peripheral page
// explicitly disables it. Guarded by configMu to stay consistent with
// writeAudioConfig's read-modify-write.
func (h *AudioHandlers) loadPlaybackEnabled() bool {
	h.configMu.Lock()
	defer h.configMu.Unlock()

	data, err := os.ReadFile(h.configPath)
	if err != nil {
		return true
	}
	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		return true
	}
	audio, ok := config["audio"].(map[string]interface{})
	if !ok {
		return true
	}
	v, ok := audio["playback_enabled"]
	if !ok {
		return true
	}
	asBool, ok := v.(bool)
	if !ok {
		return true
	}
	return asBool
}

// SetConfig updates audio configuration (volume, mute, codec, etc.).
// PUT /api/v1/audio/config
func (h *AudioHandlers) SetConfig(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Camera Control service not available")
		return
	}

	var req audioConfigReq
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), audioTimeout)
	defer cancel()

	// volume/mute are proto3 optional: only populate when the client sent the
	// field, so the device can unmute (mute=false) and skip volume=0.
	protoReq := &camerapb.AudioConfigRequest{
		Device:     req.Device,
		SampleRate: req.SampleRate,
		Channels:   req.Channels,
		Codec:      req.Codec,
		Bitrate:    req.Bitrate,
	}
	if req.Volume != nil {
		protoReq.Volume = req.Volume
	}
	if req.Mute != nil {
		protoReq.Mute = req.Mute
	}

	resp, err := client.SetAudioConfig(ctx, protoReq)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "SetAudioConfig: "+err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeCameraError, resp.GetMessage())
		return
	}
	// Persist so audio settings survive a daemon restart. Fire-and-forget.
	h.writeAudioConfig(context.Background(), getUsernameFromContext(c), req)
	Resp(c).OK(gin.H{"message": "Audio config updated"})
}

// StartPlayback starts audio playback with optional configuration.
// POST /api/v1/audio/playback/start
func (h *AudioHandlers) StartPlayback(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Camera Control service not available")
		return
	}

	var req struct {
		Device     string `json:"device"`
		SampleRate uint32 `json:"sample_rate"`
		Channels   uint32 `json:"channels"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), audioTimeout)
	defer cancel()

	resp, err := client.StartAudioPlayback(ctx, &camerapb.AudioConfigRequest{
		Device:     req.Device,
		SampleRate: req.SampleRate,
		Channels:   req.Channels,
	})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "StartAudioPlayback: "+err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeCameraError, resp.GetMessage())
		return
	}
	Resp(c).OK(gin.H{"message": "Audio playback started"})
}

// StopPlayback stops audio playback.
// POST /api/v1/audio/playback/stop
func (h *AudioHandlers) StopPlayback(c *gin.Context) {
	if h.cameraClient == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Camera Control service not available")
		return
	}

	client := camerapb.NewCameraControlClient(h.cameraClient)
	ctx, cancel := context.WithTimeout(context.Background(), audioTimeout)
	defer cancel()

	resp, err := client.StopAudioPlayback(ctx, &camerapb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "StopAudioPlayback: "+err.Error())
		return
	}
	if !resp.GetSuccess() {
		Resp(c).FailMsg(CodeCameraError, resp.GetMessage())
		return
	}
	Resp(c).OK(gin.H{"message": "Audio playback stopped"})
}

// ==========================================
// Audio over WebSocket (live streaming)
// ==========================================

var audioUpgrader = websocket.Upgrader{
	ReadBufferSize:  4 * 1024,
	WriteBufferSize: 64 * 1024,
	CheckOrigin: func(r *http.Request) bool {
		return true
	},
}

// HandleAudioStreamWebSocket handles live audio WebSocket connections.
// GET /api/v1/audio/stream
func (h *AudioHandlers) HandleAudioStreamWebSocket(c *gin.Context) {
	audioStream, err := h.audioStreams.GetOrCreateStream()
	if err != nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Audio stream unavailable: "+err.Error())
		return
	}

	conn, err := audioUpgrader.Upgrade(c.Writer, c.Request, nil)
	if err != nil {
		return
	}
	defer conn.Close()

	audioStream.AddClient(conn)
	defer audioStream.RemoveClient(conn)

	// Read loop to detect client disconnect
	for {
		if _, _, err := conn.ReadMessage(); err != nil {
			break
		}
	}
}
