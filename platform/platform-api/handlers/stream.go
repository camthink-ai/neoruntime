package handlers

import (
	"fmt"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"

	"aipc/platform/common/logger"

	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"
	"gopkg.in/yaml.v3"
)

// StreamConfig represents a stream parsed from camera-daemon.yaml encoders section
type StreamConfig struct {
	ID         string `json:"id" yaml:"stream_name"`
	Name       string `json:"name"`
	Codec      string `json:"codec" yaml:"codec"`
	Width      int    `json:"width" yaml:"width"`
	Height     int    `json:"height" yaml:"height"`
	FPS        int    `json:"fps" yaml:"fps"`
	Bitrate    int    `json:"bitrate" yaml:"bitrate"`
	GOP        int    `json:"gop" yaml:"gop"`
	Enabled    bool   `json:"enabled" yaml:"enabled"`
	SocketPath string `json:"-"` // UDS path, internal only
	Status     string `json:"status"`
}

// cameraConfig represents the relevant parts of camera-daemon.yaml
type cameraConfig struct {
	Encoders []struct {
		StreamName string `yaml:"stream_name"`
		Codec      string `yaml:"codec"`
		Width      int    `yaml:"width"`
		Height     int    `yaml:"height"`
		FPS        int    `yaml:"fps"`
		Bitrate    int    `yaml:"bitrate"`
		GOP        int    `yaml:"gop"`
		Enabled    *bool  `yaml:"enabled"` // pointer so we can detect absent (default: true)
	} `yaml:"encoders"`
}

// streamNameMap maps stream IDs to display names
var streamNameMap = map[string]string{
	"main":  "Main Stream",
	"sub":   "Sub Stream",
	"third": "Third Stream",
}

// LoadStreamsFromCameraConfig reads camera-daemon.yaml and extracts encoder configs as StreamConfigs
func LoadStreamsFromCameraConfig(configPath string, rtspBaseURL string, encodedPubDir string) []StreamConfig {
	if configPath == "" {
		logger.Warn("No camera config path specified, using default main stream")
		return defaultStreams(rtspBaseURL, encodedPubDir)
	}

	data, err := os.ReadFile(configPath)
	if err != nil {
		logger.Warn("Failed to read camera config %s: %v, using defaults", configPath, err)
		return defaultStreams(rtspBaseURL, encodedPubDir)
	}

	var cfg cameraConfig
	if err := yaml.Unmarshal(data, &cfg); err != nil {
		logger.Warn("Failed to parse camera config %s: %v, using defaults", configPath, err)
		return defaultStreams(rtspBaseURL, encodedPubDir)
	}

	if len(cfg.Encoders) == 0 {
		logger.Warn("No encoders found in camera config %s, using defaults", configPath)
		return defaultStreams(rtspBaseURL, encodedPubDir)
	}

	var streams []StreamConfig
	for _, enc := range cfg.Encoders {
		name := streamNameMap[enc.StreamName]
		if name == "" {
			name = enc.StreamName
		}
		resolution := fmt.Sprintf("%dp", enc.Height)
		// Default enabled=true when field is absent (backward compat)
		enabled := true
		if enc.Enabled != nil {
			enabled = *enc.Enabled
		}
		status := "active"
		if !enabled {
			status = "stopped"
		}
		streams = append(streams, StreamConfig{
			ID:         enc.StreamName,
			Name:       fmt.Sprintf("%s (%s)", name, resolution),
			Codec:      enc.Codec,
			Width:      enc.Width,
			Height:     enc.Height,
			FPS:        enc.FPS,
			Bitrate:    enc.Bitrate,
			GOP:        enc.GOP,
			Enabled:    enabled,
			SocketPath: encodedPubDir + "/" + enc.StreamName + ".sock",
			Status:     status,
		})
	}

	logger.Info("Loaded %d streams from camera config %s", len(streams), configPath)
	return streams
}

func defaultStreams(rtspBaseURL string, encodedPubDir string) []StreamConfig {
	return []StreamConfig{
		{
			ID: "main", Name: "Main Stream (1080p)", Codec: "h264",
			Width: 1920, Height: 1080, FPS: 30, Bitrate: 4000000, GOP: 60,
			SocketPath: encodedPubDir + "/main.sock", Status: "active",
		},
	}
}

// StreamHandlers handles streaming-related endpoints
type StreamHandlers struct {
	mu          sync.RWMutex
	streams     []StreamConfig
	streamMap   map[string]*StreamConfig
	rtspBaseURL string
	h264Streams *H264StreamManager
	encodedDir  string

	// Per-IP WebSocket connection limiting. Guards against leaked/zombie WS
	// clients from a single browser (multi-tab, re-render storms) saturating
	// the relay. Each live H264 WS connection is tracked by client IP; when a
	// client opens a sixth preview, the oldest connection from that IP is closed
	// so the newly opened /media tab can render instead of black-screening.
	wsCountMu     sync.Mutex
	wsClientsByIP map[string][]*wsClientRef
}

// maxWSPerIP caps simultaneous H264 WebSocket connections from one client IP.
// 5 tolerates a couple of tabs + a stuck reconnect without enabling a storm.
const maxWSPerIP = 5

type wsClientRef struct {
	conn        *websocket.Conn
	streamID    string
	connectedAt time.Time
}

// NewStreamHandlers creates a new stream handlers instance
func NewStreamHandlers(streams []StreamConfig, rtspBaseURL string, encodedDir string) *StreamHandlers {
	h := &StreamHandlers{
		streams:       streams,
		streamMap:     make(map[string]*StreamConfig),
		rtspBaseURL:   rtspBaseURL,
		h264Streams:   NewH264StreamManager(),
		encodedDir:    encodedDir,
		wsClientsByIP: make(map[string][]*wsClientRef),
	}
	for i := range h.streams {
		h.streamMap[h.streams[i].ID] = &h.streams[i]
	}
	return h
}

func (h *StreamHandlers) reserveWSClient(clientIP, streamID string, conn *websocket.Conn) *wsClientRef {
	h.wsCountMu.Lock()
	defer h.wsCountMu.Unlock()

	clients := h.wsClientsByIP[clientIP]
	var victim *wsClientRef
	if len(clients) >= maxWSPerIP {
		victim = clients[0]
		clients = clients[1:]
	}

	ref := &wsClientRef{
		conn:        conn,
		streamID:    streamID,
		connectedAt: time.Now(),
	}
	h.wsClientsByIP[clientIP] = append(clients, ref)
	return victim
}

func (h *StreamHandlers) releaseWSClient(clientIP string, conn *websocket.Conn) {
	h.wsCountMu.Lock()
	defer h.wsCountMu.Unlock()

	clients := h.wsClientsByIP[clientIP]
	if len(clients) == 0 {
		return
	}

	for i, ref := range clients {
		if ref.conn == conn {
			clients = append(clients[:i], clients[i+1:]...)
			break
		}
	}
	if len(clients) == 0 {
		delete(h.wsClientsByIP, clientIP)
		return
	}
	h.wsClientsByIP[clientIP] = clients
}

func (h *StreamHandlers) closeReplacedWSClient(clientIP string, ref *wsClientRef) {
	if ref == nil || ref.conn == nil {
		return
	}
	logger.Warn(
		"H264 WS: replacing oldest connection from %s (stream=%s age=%s) because per-IP cap=%d was reached",
		clientIP,
		ref.streamID,
		time.Since(ref.connectedAt).Round(time.Second),
		maxWSPerIP,
	)
	_ = ref.conn.WriteControl(
		websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.ClosePolicyViolation, "Connection replaced"),
		time.Now().Add(time.Second),
	)
	_ = ref.conn.Close()
}

// ReloadStreams replaces the in-memory stream list from YAML config.
func (h *StreamHandlers) ReloadStreams(configPath string) {
	newStreams := LoadStreamsFromCameraConfig(configPath, h.rtspBaseURL, h.encodedDir)
	if len(newStreams) == 0 {
		return
	}
	h.mu.Lock()
	defer h.mu.Unlock()
	h.streams = newStreams
	h.streamMap = make(map[string]*StreamConfig, len(newStreams))
	for i := range h.streams {
		h.streamMap[h.streams[i].ID] = &h.streams[i]
	}
	logger.Info("Reloaded %d streams from config", len(newStreams))
}

// UpdateStreamFromApplied updates a single stream's parameters in memory after gRPC reconfigure.
func (h *StreamHandlers) UpdateStreamFromApplied(streamID string, width, height uint32, codec string, bitrate, fps, gop uint32) {
	h.mu.Lock()
	sc, ok := h.streamMap[streamID]
	if ok {
		if width > 0 {
			sc.Width = int(width)
		}
		if height > 0 {
			sc.Height = int(height)
		}
		if codec != "" {
			sc.Codec = codec
		}
		if bitrate > 0 {
			sc.Bitrate = int(bitrate)
		}
		if fps > 0 {
			sc.FPS = int(fps)
		}
		if gop > 0 {
			sc.GOP = int(gop)
		}
	}
	h.mu.Unlock()

	// Clear cached SPS/PPS for the affected stream so new WS clients
	// don't receive stale init segments from the previous configuration.
	if h.h264Streams != nil {
		h.h264Streams.ClearSpsPpsCacheForStream(streamID)
		if gop > 0 {
			h.h264Streams.SetKeyframeInterval(streamID, gop)
		}
	}
}

// RestartH264Stream stops and removes the old H264Stream entry so the next
// WebSocket connection creates a fresh one. This resets the readLoop's
// exponential backoff delay, which otherwise would keep the UDS reader
// sleeping for up to 10 seconds after a stream is re-enabled.
func (h *StreamHandlers) RestartH264Stream(streamID string) {
	if h.h264Streams != nil {
		h.h264Streams.RemoveStream(streamID)
	}
}

// ForceKeyframe requests an immediate IDR for the stream's encoder via the UDS
// control channel. Called after a non-destructive encoder reconfigure (codec /
// bitrate / GOP path) so connected clients receive a clean keyframe instead of
// transitional P-frames that reference the pre-reconfig reference (causes 花屏).
func (h *StreamHandlers) ForceKeyframe(streamID string) {
	if h.h264Streams != nil {
		h.h264Streams.ForceKeyframe(streamID)
	}
}

// ==========================================
// H264 over WebSocket (MSE)
// ==========================================

var h264Upgrader = websocket.Upgrader{
	// Large main-stream IDR NALs need headroom; tiny buffers risk write failures.
	ReadBufferSize:  64 * 1024,
	WriteBufferSize: 512 * 1024,
	CheckOrigin: func(r *http.Request) bool {
		return true
	},
}

// HandleH264WebSocket handles H264 WebSocket connections for MSE playback
func (h *StreamHandlers) HandleH264WebSocket(c *gin.Context) {
	streamID := c.Param("stream_id")

	if strings.Contains(streamID, "..") {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid stream ID")
		return
	}

	// Validate stream exists in config
	stream, ok := h.streamMap[streamID]
	if !ok {
		Resp(c).FailMsg(CodeNotFound, "Stream not found: "+streamID)
		return
	}

	conn, err := h264Upgrader.Upgrade(c.Writer, c.Request, nil)
	if err != nil {
		logger.Error("Failed to upgrade H264 WebSocket: %v", err)
		return
	}
	defer conn.Close()

	// Per-IP connection cap: replace the oldest connection from the same IP
	// instead of rejecting the new /media preview. This recovers from leaked
	// tabs/reconnect loops while letting the user's latest tab render video.
	clientIP := c.ClientIP()
	victim := h.reserveWSClient(clientIP, streamID, conn)
	h.closeReplacedWSClient(clientIP, victim)
	defer h.releaseWSClient(clientIP, conn)

	// Get or create H264 stream using UDS socket path
	h264Stream, err := h.h264Streams.GetOrCreateStream(streamID, stream.SocketPath)
	if err != nil {
		logger.Error("Failed to create H264 stream: %v", err)
		conn.WriteMessage(websocket.TextMessage, []byte(`{"error":"Failed to start stream"}`))
		return
	}

	h264Stream.AddClient(conn)
	defer h264Stream.RemoveClient(conn)

	logger.Info("H264 WebSocket client connected to stream %s (%s)", streamID, stream.Name)

	for {
		_, _, err := conn.ReadMessage()
		if err != nil {
			break
		}
	}
}
