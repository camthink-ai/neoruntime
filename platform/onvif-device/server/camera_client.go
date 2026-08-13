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
	"strings"
	"time"

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
	conn    *grpc.ClientConn
	timeout time.Duration
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
	return nil
}

// initLiveStreams dials camera-daemon's CameraControl gRPC service and arms the
// package-wide liveStreams provider. The dial is non-blocking, so a missing or
// down camera-daemon never stalls startup — the first request simply fails and
// onvif-device serves static metadata until camera-daemon recovers.
// An empty socket leaves liveStreams nil (feature disabled).
func initLiveStreams(socket string) {
	if socket == "" {
		return
	}
	conn, err := grpc.DialContext(context.Background(), socket,
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		log.Printf("[onvif] camera-daemon stream-status disabled: dial %s: %v", socket, err)
		return
	}
	client := &cameraStatusClient{conn: conn, timeout: 2 * time.Second}
	liveStreams = client
	reconfigurer = client
	log.Printf("[onvif] camera-daemon stream-status enabled (%s); ONVIF metadata tracks live encoder params", socket)
}
