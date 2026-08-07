package handlers

import (
	"context"
	"errors"
	"io"
	"strconv"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"
	"google.golang.org/grpc"

	camerapb "aipc/platform/camera-daemon/proto"
	"aipc/platform/common/logger"
)

// ============================================================
// Two-way talk (web microphone → device speaker)
//
// The browser captures the mic (see web audioTalker.ts + mic-processor.js),
// resamples to S16LE 48 kHz mono, and streams PCM as binary WebSocket frames
// to GET /api/v1/audio/talk. This handler forwards each frame into the
// camera-daemon StreamAudioPcm gRPC client stream, which writes it to the
// ALSA playback device via AudioService::write_pcm().
//
// Lifecycle per connection:
//  1. acquire single-session slot (device has one playback device)
//  2. StartAudioPlayback → open / configure the ALSA playback device
//  3. upgrade to WebSocket
//  4. StreamAudioPcm → forward binary PCM frames
//  5. on disconnect: CloseAndRecv + StopAudioPlayback, release slot
// ============================================================

// talkStreamClient is the narrow subset of the generated gRPC client-streaming
// surface that the talk handler needs (send PCM, close). The generated
// grpc.ClientStreamingClient[AudioPcmChunk, Status] satisfies it, and so does
// a test mock.
type talkStreamClient interface {
	Send(*camerapb.AudioPcmChunk) error
	CloseAndRecv() (*camerapb.Status, error)
}

// talkPlaybackClient is the narrow camera-daemon client surface the talk
// handler needs. grpcTalkClient adapts the generated client to it so tests
// can inject a mock without standing up a real gRPC server.
type talkPlaybackClient interface {
	StartAudioPlayback(ctx context.Context, in *camerapb.AudioConfigRequest, opts ...grpc.CallOption) (*camerapb.Status, error)
	StopAudioPlayback(ctx context.Context, in *camerapb.Empty, opts ...grpc.CallOption) (*camerapb.Status, error)
	StreamAudioPcm(ctx context.Context, opts ...grpc.CallOption) (talkStreamClient, error)
}

// grpcTalkClient adapts camerapb.CameraControlClient to talkPlaybackClient.
// The only real translation is StreamAudioPcm's return type: the generated
// client returns grpc.ClientStreamingClient[...], which we narrow to our
// talkStreamClient.
type grpcTalkClient struct{ inner camerapb.CameraControlClient }

func (g grpcTalkClient) StartAudioPlayback(ctx context.Context, in *camerapb.AudioConfigRequest, opts ...grpc.CallOption) (*camerapb.Status, error) {
	return g.inner.StartAudioPlayback(ctx, in, opts...)
}

func (g grpcTalkClient) StopAudioPlayback(ctx context.Context, in *camerapb.Empty, opts ...grpc.CallOption) (*camerapb.Status, error) {
	return g.inner.StopAudioPlayback(ctx, in, opts...)
}

func (g grpcTalkClient) StreamAudioPcm(ctx context.Context, opts ...grpc.CallOption) (talkStreamClient, error) {
	s, err := g.inner.StreamAudioPcm(ctx, opts...)
	if err != nil {
		return nil, err
	}
	return talkStreamClient(s), nil
}

// talkSem is a 1-slot semaphore enforcing a single active talk session:
// the device has one playback device, so concurrent talkers would conflict.
var talkSem = make(chan struct{}, 1)

// talkReadIdleTimeout bounds sessions whose WS stays open but stops sending
// mic PCM. Browsers normally send a 1024-sample frame about every 21 ms while
// push-to-talk is held; an idle socket means the UI, browser, or network has
// lost the capture path and should not keep the device playback slot open.
var talkReadIdleTimeout = 5 * time.Second

// acquireTalk reserves the single talk slot; returns false if one is active.
func acquireTalk() bool {
	select {
	case talkSem <- struct{}{}:
		return true
	default:
		return false
	}
}

// releaseTalk frees the talk slot.
func releaseTalk() {
	select {
	case <-talkSem:
	default:
	}
}

// talkConfig holds the audio format the browser will stream.
type talkConfig struct {
	Device     string
	SampleRate uint32
	Channels   uint32
}

// talkFormat is the PCM wire format expected from the browser mic worklet.
const talkFormat = "S16LE"

// parseTalkConfig reads optional format overrides from query params, falling
// back to the values used by the web mic worklet (48 kHz, mono).
func parseTalkConfig(c *gin.Context) talkConfig {
	cfg := talkConfig{SampleRate: 48000, Channels: 1}

	if v := c.Query("device"); v != "" {
		cfg.Device = v
	}
	if v := c.Query("sample_rate"); v != "" {
		if n, err := strconv.ParseUint(v, 10, 32); err == nil && n > 0 {
			cfg.SampleRate = uint32(n)
		}
	}
	if v := c.Query("channels"); v != "" {
		if n, err := strconv.ParseUint(v, 10, 32); err == nil && (n == 1 || n == 2) {
			cfg.Channels = uint32(n)
		}
	}
	return cfg
}

// HandleAudioTalkWebSocket receives push-to-talk PCM audio from the browser and
// forwards it to the device speaker via camera-daemon's StreamAudioPcm gRPC.
// One active session at a time; a second concurrent connection is rejected
// with CodeResourceExhausted. GET /api/v1/audio/talk
func (h *AudioHandlers) HandleAudioTalkWebSocket(c *gin.Context) {
	// No camera-daemon connection and no test override → unavailable.
	if h.cameraClient == nil && h.talkClientOverride == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Camera Control service not available")
		return
	}

	// Speaker-output gate: the Peripheral page can disable the speaker to block
	// push-to-talk entirely. Enforced server-side (in addition to the UI
	// greying out the button) so a direct API call can't bypass the toggle.
	// Defaults to enabled when the persisted flag is absent.
	if !h.loadPlaybackEnabled() {
		Resp(c).FailMsg(CodeResourceExhausted, "Speaker output is disabled")
		return
	}

	if !acquireTalk() {
		Resp(c).FailMsg(CodeResourceExhausted, "Another talk session is already active")
		return
	}
	defer releaseTalk()

	client := h.talkClient()
	cfg := parseTalkConfig(c)

	// Open the ALSA playback device before the WS handshake so setup errors
	// reach the client as a normal HTTP status rather than a WS close frame.
	startCtx, startCancel := context.WithTimeout(c.Request.Context(), audioTimeout)
	startResp, err := client.StartAudioPlayback(startCtx, &camerapb.AudioConfigRequest{
		Device:     cfg.Device,
		SampleRate: cfg.SampleRate,
		Channels:   cfg.Channels,
	})
	startCancel()
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "StartAudioPlayback: "+err.Error())
		return
	}
	if !startResp.GetSuccess() {
		Resp(c).FailMsg(CodeCameraError, startResp.GetMessage())
		return
	}

	conn, err := audioUpgrader.Upgrade(c.Writer, c.Request, nil)
	if err != nil {
		// Playback device is open but the WS handshake failed — release it.
		stopPlayback(context.Background(), client)
		return
	}
	defer conn.Close()
	refreshTalkReadDeadline(conn)

	streamCtx, streamCancel := context.WithCancel(c.Request.Context())
	defer streamCancel()

	stream, err := client.StreamAudioPcm(streamCtx)
	if err != nil {
		logger.Error("audio talk: open StreamAudioPcm: %v", err)
		writeTalkError(conn, "StreamAudioPcm: "+err.Error())
		stopPlayback(context.Background(), client)
		return
	}

	logger.Info("audio talk session started (device=%q %dHz %dch)", cfg.Device, cfg.SampleRate, cfg.Channels)

	// Forward loop: binary WS frames → gRPC PCM chunks. Text frames ignored.
	for {
		msgType, data, err := conn.ReadMessage()
		if err != nil {
			if !websocket.IsCloseError(err, websocket.CloseNormalClosure, websocket.CloseGoingAway, websocket.CloseNoStatusReceived) {
				logger.Debug("audio talk: read ended: %v", err)
			}
			break
		}
		if msgType != websocket.BinaryMessage {
			continue
		}
		refreshTalkReadDeadline(conn)
		if err := stream.Send(&camerapb.AudioPcmChunk{
			Data:       data,
			SampleRate: cfg.SampleRate,
			Channels:   cfg.Channels,
			Format:     talkFormat,
		}); err != nil {
			// io.EOF means the server already finished the stream (e.g. the
			// device rejected the PCM); the real reason arrives in the
			// CloseAndRecv Status below. Anything else is a genuine send error.
			if !errors.Is(err, io.EOF) {
				logger.Error("audio talk: send PCM failed: %v", err)
			} else {
				logger.Debug("audio talk: server closed PCM stream")
			}
			break
		}
	}

	// Teardown: flush the client stream then stop the playback device.
	// The Status carries the device-side outcome (e.g. "PCM write failed").
	resp, err := stream.CloseAndRecv()
	if err != nil {
		logger.Error("audio talk: CloseAndRecv: %v", err)
	} else if resp != nil && !resp.GetSuccess() {
		logger.Error("audio talk: device rejected PCM: %s", resp.GetMessage())
	}
	stopPlayback(context.Background(), client)
	logger.Info("audio talk session ended")
}

func refreshTalkReadDeadline(conn *websocket.Conn) {
	if talkReadIdleTimeout <= 0 {
		return
	}
	_ = conn.SetReadDeadline(time.Now().Add(talkReadIdleTimeout))
}

// stopPlayback closes the device playback device with a bounded timeout.
func stopPlayback(ctx context.Context, client talkPlaybackClient) {
	stopCtx, cancel := context.WithTimeout(ctx, audioTimeout)
	defer cancel()
	if _, err := client.StopAudioPlayback(stopCtx, &camerapb.Empty{}); err != nil {
		logger.Error("audio talk: StopAudioPlayback: %v", err)
	}
}

// writeTalkError sends a JSON error + close frame to the WS client before we
// tear down. Used for failures that occur after the WS upgrade (where a normal
// HTTP response is no longer possible).
func writeTalkError(conn *websocket.Conn, detail string) {
	_ = conn.WriteJSON(gin.H{"error": detail})
	_ = conn.WriteMessage(
		websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.ClosePolicyViolation, detail),
	)
}

// talkClient returns the playback gRPC client, preferring a test override.
func (h *AudioHandlers) talkClient() talkPlaybackClient {
	if h.talkClientOverride != nil {
		return h.talkClientOverride
	}
	return grpcTalkClient{inner: camerapb.NewCameraControlClient(h.cameraClient)}
}
