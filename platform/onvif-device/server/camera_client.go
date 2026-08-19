package main

// Camera-daemon gRPC integration for live (runtime) stream metadata.
//
// onvif.yaml statically declares each stream's codec/resolution/bitrate/fps/gop,
// but those parameters are runtime-mutable on the camera-daemon side (web UI,
// ReconfigurePipeline, SwitchProfile, …). The static config therefore drifts
// from reality. This thin client queries camera-daemon's GetStreamStatus over
// its Unix socket and feeds the actual running parameters to enrichProfiles so
// every ONVIF configuration response (GetProfiles/GetProfile/Get*Configuration/
// Options) reflects what the encoder is really emitting.
//
// Semantics mirror platform-api's getAllRunningStreamParams
// (platform-api/handlers/media.go): on any gRPC failure or when no stream is
// active, RunningParams returns nil and callers silently fall back to the
// static (library) values — ONVIF never breaks because camera-daemon is down.

import (
	"context"
	"fmt"
	"log"
	"os"
	"strings"
	"sync"
	"time"

	"gopkg.in/yaml.v3"

	camerapb "aipc/platform/camera-daemon/proto"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

// streamParams is the subset of a running stream's encoder parameters that we
// project onto ONVIF MediaProfile fields. Bitrate is normalized to kbps
// (ONVIF tt:BitrateLimit is kbps; camera-daemon reports bps).
type streamParams struct {
	Codec       string
	Width       uint32
	Height      uint32
	Fps         uint32
	BitrateKbps uint32
	Gop         uint32
}

// streamStatusProvider returns the live encoder parameters keyed by stream id
// ("main", "sub", "third"). A nil return means "no live data — use static".
// The interface keeps this package testable without a real Unix socket.
type streamStatusProvider interface {
	RunningParams() map[string]*streamParams
}

// liveStreams is the package-wide provider, wired once at startup by
// initLiveStreams. nil (or a nil map from RunningParams) disables the overlay
// and onvif-device falls back to static onvif.yaml values.
var liveStreams streamStatusProvider

// streamReconfigurer is the write counterpart to streamStatusProvider: it pushes
// new encoder parameters to a camera-daemon stream. The ONVIF
// SetVideoEncoderConfiguration handler calls it. nil (camera-daemon down) makes
// that handler return a SOAP fault rather than silently no-oping, so an NVR
// learns the change was not applied.
type streamReconfigurer interface {
	ReconfigureEncoder(stream string, p streamParams) error
}

// reconfigurer is wired once at startup by initLiveStreams alongside
// liveStreams (same client implements both).
var reconfigurer streamReconfigurer

// cameraStatusClient is the production streamStatusProvider: a long-lived gRPC
// connection to camera-daemon queried once per ONVIF request.
type cameraStatusClient struct {
	conn       *grpc.ClientConn
	timeout    time.Duration
	configPath string // camera-daemon.yaml; "" disables ONVIF-side persistence
}

// RunningParams implements streamStatusProvider. It never returns an error:
// any failure (conn down, RPC error, no active streams) yields nil so the
// caller falls back to static config.
func (c *cameraStatusClient) RunningParams() map[string]*streamParams {
	if c == nil || c.conn == nil {
		return nil
	}
	ctx, cancel := context.WithTimeout(context.Background(), c.timeout)
	defer cancel()

	resp, err := camerapb.NewCameraControlClient(c.conn).GetStreamStatus(ctx, &camerapb.GetStreamStatusRequest{})
	if err != nil {
		return nil
	}
	out := make(map[string]*streamParams, len(resp.GetStreams()))
	for _, s := range resp.GetStreams() {
		// Only streams that are actually running carry hardware-backed params.
		if s == nil || !s.GetHasEncoder() || s.GetStatus() != "active" {
			continue
		}
		out[s.GetStreamId()] = &streamParams{
			Codec:       s.GetCodec(),
			Width:       s.GetWidth(),
			Height:      s.GetHeight(),
			Fps:         s.GetFps(),
			BitrateKbps: s.GetBitrateBps() / 1000,
			Gop:         s.GetGop(),
		}
	}
	if len(out) == 0 {
		return nil
	}
	return out
}

// reconfigureTimeout is the gRPC deadline for ReconfigureEncoder. Unlike the
// 2s read timeout (c.timeout, for the cheap GetStreamStatus), a resolution or
// codec change restarts the encode pipeline and blocks until it re-aligns on a
// keyframe — easily several seconds. 30s gives that headroom so the NVR gets a
// real success/failure instead of a DeadlineExceeded fault while camera-daemon
// is still (successfully) applying the change.
const reconfigureTimeout = 30 * time.Second

// ReconfigureEncoder pushes new encoder parameters to a camera-daemon stream via
// the ReconfigureEncoder RPC. It is the write counterpart to RunningParams: the
// ONVIF SetVideoEncoderConfiguration handler maps the NVR-supplied config onto a
// streamParams and calls this. Empty/zero fields mean "no change"
// (EncoderReconfigRequest's 0/empty convention), so a partial ONVIF update only
// touches the fields the client actually sent. Codec is lower-cased because
// camera-daemon expects "h264"/"h265" while ONVIF carries "H264"/"H265", and
// BitrateKbps is converted to bps. Any gRPC failure or success==false yields an
// error so the SOAP layer returns a fault rather than a silent success.
func (c *cameraStatusClient) ReconfigureEncoder(stream string, p streamParams) error {
	if c == nil || c.conn == nil {
		return fmt.Errorf("camera-daemon not available")
	}
	ctx, cancel := context.WithTimeout(context.Background(), reconfigureTimeout)
	defer cancel()

	req := &camerapb.EncoderReconfigRequest{StreamName: stream}
	if p.Codec != "" {
		req.Codec = strings.ToLower(p.Codec)
	}
	if p.Width != 0 {
		req.Width = p.Width
	}
	if p.Height != 0 {
		req.Height = p.Height
	}
	if p.Fps != 0 {
		req.Fps = p.Fps
	}
	if p.Gop != 0 {
		req.Gop = p.Gop
	}
	if p.BitrateKbps != 0 {
		req.BitrateBps = p.BitrateKbps * 1000
	}

	resp, err := camerapb.NewCameraControlClient(c.conn).ReconfigureEncoder(ctx, req)
	if err != nil {
		return err
	}
	if !resp.GetSuccess() {
		msg := resp.GetMessage()
		if msg == "" {
			msg = "camera-daemon rejected encoder reconfiguration"
		}
		return fmt.Errorf("%s", msg)
	}
	// Runtime apply succeeded — persist to camera-daemon.yaml so the change
	// survives reboot AND the web settings page (GET /media/config, which reads
	// the yaml) reflects it. Best-effort: the live encoder is already updated, so
	// a persistence failure is logged, not returned (mirrors platform-api's
	// writeStreamToConfig, which also swallows persistence errors).
	if err := persistStreamConfig(c.configPath, stream, p); err != nil {
		log.Printf("[onvif] ReconfigureEncoder(%s) applied but NOT persisted: %v", stream, err)
	} else if c.configPath != "" {
		log.Printf("[onvif] ReconfigureEncoder(%s) persisted to %s", stream, c.configPath)
	}
	return nil
}

// initLiveStreams dials camera-daemon's CameraControl gRPC service and arms the
// package-wide liveStreams provider. The dial is non-blocking, so a missing or
// down camera-daemon never stalls startup — the first request simply fails and
// onvif-device serves static metadata until camera-daemon recovers.
// An empty socket leaves liveStreams nil (feature disabled). configPath is the
// camera-daemon.yaml path used to persist ONVIF encoder changes (empty disables
// persistence; runtime-only changes then revert on reboot).
func initLiveStreams(socket, configPath string) {
	if socket == "" {
		return
	}
	conn, err := grpc.DialContext(context.Background(), socket,
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		log.Printf("[onvif] camera-daemon stream-status disabled: dial %s: %v", socket, err)
		return
	}
	client := &cameraStatusClient{conn: conn, timeout: 2 * time.Second, configPath: configPath}
	liveStreams = client
	reconfigurer = client
	log.Printf("[onvif] camera-daemon stream-status enabled (%s); ONVIF metadata tracks live encoder params", socket)
}

// persistMu serializes onvif-device's read-modify-write of camera-daemon.yaml so
// concurrent ONVIF Sets don't clobber each other. platform-api has its own
// configMu; cross-process races are last-writer-wins on a full-file rewrite,
// acceptable for these low-frequency human/NVR-driven changes (the same risk the
// web UI already has with concurrent browser tabs).
var persistMu sync.Mutex

// persistStreamConfig writes the encoder params into camera-daemon.yaml's
// encoders[] entry for streamName, mirroring platform-api's writeStreamToConfig.
// This makes an ONVIF SetVideoEncoderConfiguration survive reboot AND lets the
// web settings page (GET /media/config, which reads the yaml) reflect the change
// immediately — closing the gap where ONVIF changed only the runtime encoder
// while the persisted config (and thus the web UI) still showed the old value.
//
// An empty configPath is a no-op (persistence disabled). 0-valued fields are left
// unchanged (writeStreamToConfig/"0 = no change" semantics), so a partial ONVIF
// update touches only the fields the client sent. BitrateKbps is converted to
// bps (the yaml bitrate unit); width/height are canonicalized to landscape (swap
// when height>width) to match platform-api's canonicalEncoderDims. The write is
// atomic (temp + rename) so a crash mid-write cannot brick camera-daemon's boot.
func persistStreamConfig(configPath, streamName string, p streamParams) error {
	if configPath == "" {
		return nil
	}
	persistMu.Lock()
	defer persistMu.Unlock()

	data, err := os.ReadFile(configPath)
	if err != nil {
		return fmt.Errorf("read camera-daemon.yaml: %w", err)
	}
	var config map[string]interface{}
	if err := yaml.Unmarshal(data, &config); err != nil {
		return fmt.Errorf("parse camera-daemon.yaml: %w", err)
	}
	encoders, ok := config["encoders"].([]interface{})
	if !ok {
		return fmt.Errorf("camera-daemon.yaml has no encoders[] list")
	}
	found := false
	for _, item := range encoders {
		m, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		if fmt.Sprint(m["stream_name"]) != streamName {
			continue
		}
		found = true
		w, h := p.Width, p.Height
		if w > 0 && h > 0 && h > w {
			// Canonical landscape: undo a portrait transpose (a rotation-residual
			// signature) so a stale portrait dim never survives into the next boot.
			w, h = h, w
		}
		if w > 0 {
			m["width"] = int(w)
		}
		if h > 0 {
			m["height"] = int(h)
		}
		if p.Codec != "" {
			m["codec"] = strings.ToLower(p.Codec)
		}
		if p.BitrateKbps > 0 {
			m["bitrate"] = int(p.BitrateKbps) * 1000 // yaml bitrate is bps
		}
		if p.Fps > 0 {
			m["fps"] = int(p.Fps)
		}
		if p.Gop > 0 {
			m["gop"] = int(p.Gop)
		}
		break
	}
	if !found {
		return fmt.Errorf("stream %q not found in camera-daemon.yaml encoders[]", streamName)
	}

	out, err := yaml.Marshal(config)
	if err != nil {
		return fmt.Errorf("marshal camera-daemon.yaml: %w", err)
	}
	tmp := configPath + ".onvif-tmp"
	if err := os.WriteFile(tmp, out, 0o644); err != nil {
		return fmt.Errorf("write temp config: %w", err)
	}
	if err := os.Rename(tmp, configPath); err != nil {
		return fmt.Errorf("replace camera-daemon.yaml: %w", err)
	}
	return nil
}
