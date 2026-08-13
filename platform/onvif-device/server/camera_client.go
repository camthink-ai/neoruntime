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
	"log"
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
	liveStreams = &cameraStatusClient{conn: conn, timeout: 2 * time.Second}
	log.Printf("[onvif] camera-daemon stream-status enabled (%s); ONVIF metadata tracks live encoder params", socket)
}
