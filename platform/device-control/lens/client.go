package lens

import (
	"context"
	"fmt"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	"aipc/platform/device-control/hal"
	pb "aipc/platform/device-control/lens/lenspb"
)

type LensClient struct {
	conn   *grpc.ClientConn
	client pb.LensHALClient
	cfg    hal.LensConfig

	// irisTarget / hasIrisTarget cache the last successfully applied iris
	// target so it can be persisted (there is no HAL getter for it). They
	// mirror the persisted side-file so a reconnect replay can re-push them.
	irisTarget    uint16
	hasIrisTarget bool
}

func NewLensClient(endpoint string, cfg hal.LensConfig) (*LensClient, error) {
	// NewClient deliberately does not wait for the Unix socket.  ClientConn owns
	// the reconnect/backoff state, so a slow or restarted camera-daemon does not
	// permanently disable lens support in device-control.
	conn, err := grpc.NewClient("unix://"+endpoint,
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return nil, fmt.Errorf("connect to lens HAL service: %w", err)
	}
	return &LensClient{
		conn:   conn,
		client: pb.NewLensHALClient(conn),
		cfg:    cfg,
	}, nil
}

// Conn exposes the long-lived connection so the owner can observe connectivity
// transitions and re-run the remote HAL initialization after daemon restarts.
func (c *LensClient) Conn() *grpc.ClientConn { return c.conn }

func (c *LensClient) Init() error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.Init(ctx, &pb.Empty{})
	})
}

func (c *LensClient) ReInit() error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.ReInit(ctx, &pb.Empty{})
	})
}

func (c *LensClient) Close() {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	c.client.Shutdown(ctx, &pb.Empty{})
	c.conn.Close()
}

func (c *LensClient) StateGet() (hal.LensState, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	resp, err := c.client.StateGet(ctx, &pb.Empty{})
	if err != nil {
		return hal.LensState{}, err
	}
	return protoToState(resp), nil
}

func (c *LensClient) StateGetTry() (hal.LensState, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	resp, err := c.client.StateGet(ctx, &pb.Empty{})
	if err != nil {
		return hal.LensState{}, err
	}
	return protoToState(resp), nil
}

func (c *LensClient) IrisAdcGet() (uint16, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	resp, err := c.client.IrisAdcGet(ctx, &pb.Empty{})
	if err != nil {
		return 0, err
	}
	return uint16(resp.Adc), nil
}

func (c *LensClient) IrisAdcGetTry() (uint16, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 200*time.Millisecond)
	defer cancel()
	resp, err := c.client.IrisAdcGet(ctx, &pb.Empty{})
	if err != nil {
		return 0, err
	}
	return uint16(resp.Adc), nil
}

func (c *LensClient) ZoomRun(pps uint16, steps int32) error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.ZoomRun(ctx, &pb.MotorRunRequest{Pps: uint32(pps), Steps: steps})
	})
}

func (c *LensClient) ZoomAbs(pps uint16, position int32) error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.ZoomAbs(ctx, &pb.MotorAbsRequest{Pps: uint32(pps), Position: position})
	})
}

func (c *LensClient) ZoomStop() error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.ZoomStop(ctx, &pb.Empty{})
	})
}

func (c *LensClient) ZoomResetZero() error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.ZoomResetZero(ctx, &pb.Empty{})
	})
}

func (c *LensClient) ZoomLimitSet(min, max int32) error {
	err := c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.ZoomLimitSet(ctx, &pb.LimitSetRequest{MinPos: min, MaxPos: max})
	})
	if err == nil {
		// Live in-memory cache only — ZoomLimits() returns c.cfg. NOT persisted:
		// limits are AF0832 calibration constants sourced from
		// device-control.yaml (default_zoom_limit). Persisting them would let a
		// one-off set (e.g. a test PUT of placeholder 0/8000) poison every later
		// boot's ReplayPersistedConfig into overwriting the yaml-calibrated
		// range, breaking GetLensStatus readback and the UI (pos-min)/range.
		c.cfg.ZoomLimit = hal.LensLimit{MinPos: min, MaxPos: max}
	}
	return err
}

func (c *LensClient) ZoomLimits() hal.LensLimit { return c.cfg.ZoomLimit }

func (c *LensClient) WaitZoomStopped(timeout time.Duration) error {
	return c.wait(func(ctx context.Context, ms uint32) (*pb.HalStatus, error) {
		return c.client.WaitZoomStopped(ctx, &pb.WaitRequest{TimeoutMs: ms})
	}, timeout)
}

func (c *LensClient) WaitZoomRzDone(timeout time.Duration) error {
	return c.wait(func(ctx context.Context, ms uint32) (*pb.HalStatus, error) {
		return c.client.WaitZoomRzDone(ctx, &pb.WaitRequest{TimeoutMs: ms})
	}, timeout)
}

func (c *LensClient) FocusRun(pps uint16, steps int32) error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.FocusRun(ctx, &pb.MotorRunRequest{Pps: uint32(pps), Steps: steps})
	})
}

func (c *LensClient) FocusAbs(pps uint16, position int32) error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.FocusAbs(ctx, &pb.MotorAbsRequest{Pps: uint32(pps), Position: position})
	})
}

func (c *LensClient) FocusStop() error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.FocusStop(ctx, &pb.Empty{})
	})
}

func (c *LensClient) FocusResetZero() error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.FocusResetZero(ctx, &pb.Empty{})
	})
}

func (c *LensClient) FocusLimitSet(min, max int32) error {
	err := c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.FocusLimitSet(ctx, &pb.LimitSetRequest{MinPos: min, MaxPos: max})
	})
	if err == nil {
		// Live in-memory cache only — see ZoomLimitSet for why focus limits are
		// never persisted (yaml-sourced calibration constants; a stale
		// side-file would overwrite them on replay and break readback).
		c.cfg.FocusLimit = hal.LensLimit{MinPos: min, MaxPos: max}
	}
	return err
}

func (c *LensClient) FocusLimits() hal.LensLimit { return c.cfg.FocusLimit }

func (c *LensClient) WaitFocusStopped(timeout time.Duration) error {
	return c.wait(func(ctx context.Context, ms uint32) (*pb.HalStatus, error) {
		return c.client.WaitFocusStopped(ctx, &pb.WaitRequest{TimeoutMs: ms})
	}, timeout)
}

func (c *LensClient) WaitFocusRzDone(timeout time.Duration) error {
	return c.wait(func(ctx context.Context, ms uint32) (*pb.HalStatus, error) {
		return c.client.WaitFocusRzDone(ctx, &pb.WaitRequest{TimeoutMs: ms})
	}, timeout)
}

func (c *LensClient) IrisRun(pps uint16, steps int32) error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.IrisRun(ctx, &pb.MotorRunRequest{Pps: uint32(pps), Steps: steps})
	})
}

func (c *LensClient) IrisStop() error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.IrisStop(ctx, &pb.Empty{})
	})
}

func (c *LensClient) IrisTargetSet(target uint16) error {
	err := c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.IrisTargetSet(ctx, &pb.IrisTargetRequest{Target: uint32(target)})
	})
	if err == nil {
		c.irisTarget = target
		c.hasIrisTarget = true
		c.persist()
	}
	return err
}

func (c *LensClient) StopAndWaitAll(timeout time.Duration) error {
	return c.wait(func(ctx context.Context, ms uint32) (*pb.HalStatus, error) {
		return c.client.StopAndWaitAll(ctx, &pb.WaitRequest{TimeoutMs: ms})
	}, timeout)
}

func (c *LensClient) AF0832Bootstrap() error {
	return c.simpleWithTimeout(30*time.Second, func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.AF0832Bootstrap(ctx, &pb.Empty{})
	})
}

func (c *LensClient) AF0832MarkBootstrapped() error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.AF0832MarkBootstrapped(ctx, &pb.Empty{})
	})
}

func (c *LensClient) AF0832ForceResetZero() error {
	return c.simpleWithTimeout(30*time.Second, func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.AF0832ForceResetZero(ctx, &pb.Empty{})
	})
}

func (c *LensClient) AF0832GotoRatioDistance(zoomRatio, focusDistanceM float32) error {
	return c.simpleWithTimeout(60*time.Second, func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.AF0832GotoRatioDistance(ctx, &pb.AF0832GotoRequest{
			ZoomRatio:      zoomRatio,
			FocusDistanceM: focusDistanceM,
		})
	})
}

func (c *LensClient) AF0832PosToRatio(halZoomPos int32) float32 {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	resp, err := c.client.AF0832PosToRatio(ctx, &pb.AF0832PosToRatioRequest{HalZoomPos: halZoomPos})
	if err != nil {
		return 1.0
	}
	return resp.Ratio
}

func (c *LensClient) IsAF0832Bootstrapped() bool {
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	resp, err := c.client.IsAF0832Bootstrapped(ctx, &pb.Empty{})
	if err != nil {
		return false
	}
	return resp.Bootstrapped
}

// ── Lens profile & FG2009 open-loop helpers ─────────────────────────────

func (c *LensClient) ProfileGet() (hal.LensProfile, error) {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	resp, err := c.client.ProfileGet(ctx, &pb.Empty{})
	if err != nil {
		return hal.LensProfile{}, err
	}
	return hal.LensProfile{
		Model:            resp.Model,
		Relative:         resp.Relative,
		Ircut:            resp.Ircut,
		ZoomTravelSteps:  resp.ZoomTravelSteps,
		FocusTravelSteps: resp.FocusTravelSteps,
		MaxZoomRatio:     resp.MaxZoomRatio,
	}, nil
}

func (c *LensClient) ZoomGotoRatio(zoomRatio float32, pps uint16) error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.ZoomGotoRatio(ctx, &pb.ZoomGotoRatioRequest{
			Ratio: zoomRatio,
			Pps:   uint32(pps),
		})
	})
}

func (c *LensClient) FocusGotoLevel(level float32, pps uint16) error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.FocusGotoLevel(ctx, &pb.FocusGotoLevelRequest{
			Level: level,
			Pps:   uint32(pps),
		})
	})
}

func (c *LensClient) ZoomMoveRel(pps uint16, steps int32) error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.ZoomMoveRel(ctx, &pb.MotorRunRequest{Pps: uint32(pps), Steps: steps})
	})
}

func (c *LensClient) FocusMoveRel(pps uint16, steps int32) error {
	return c.simple(func(ctx context.Context) (*pb.HalStatus, error) {
		return c.client.FocusMoveRel(ctx, &pb.MotorRunRequest{Pps: uint32(pps), Steps: steps})
	})
}

func (c *LensClient) SetAfWindows(config hal.AfWindowsConfig) error {
	return fmt.Errorf("SetAfWindows: not yet supported by lens HAL bridge")
}

func (c *LensClient) GetAfMeasurement() (*hal.AfMeasurement, error) {
	return nil, fmt.Errorf("GetAfMeasurement: not yet supported by lens HAL bridge")
}

// ── helpers ──────────────────────────────────────────────────────────────

func (c *LensClient) simple(fn func(context.Context) (*pb.HalStatus, error)) error {
	return c.simpleWithTimeout(15*time.Second, fn)
}

func (c *LensClient) simpleWithTimeout(timeout time.Duration, fn func(context.Context) (*pb.HalStatus, error)) error {
	ctx, cancel := context.WithTimeout(context.Background(), timeout)
	defer cancel()
	resp, err := fn(ctx)
	if err != nil {
		return err
	}
	if !resp.Ok {
		return &HalError{Code: resp.HalCode, Msg: resp.Message}
	}
	return nil
}

func (c *LensClient) wait(fn func(context.Context, uint32) (*pb.HalStatus, error), timeout time.Duration) error {
	ms := uint32(timeout.Milliseconds())
	ctx, cancel := context.WithTimeout(context.Background(), timeout+5*time.Second)
	defer cancel()
	resp, err := fn(ctx, ms)
	if err != nil {
		return err
	}
	if !resp.Ok {
		return &HalError{Code: resp.HalCode, Msg: resp.Message}
	}
	return nil
}

func protoToState(s *pb.LensState) hal.LensState {
	return hal.LensState{
		IrisState:   uint8(s.IrisState),
		ZoomState:   uint8(s.ZoomState),
		FocusState:  uint8(s.FocusState),
		ZoomRzDone:  s.ZoomRzDone,
		FocusRzDone: s.FocusRzDone,
		ZoomPos:     s.ZoomPos,
		FocusPos:    s.FocusPos,
	}
}

// HalError carries the raw HAL error code for pattern matching (e.g. -2810 MCU transient).
type HalError struct {
	Code int32
	Msg  string
}

func (e *HalError) Error() string {
	return fmt.Sprintf("hal error %d: %s", e.Code, e.Msg)
}

func (e *HalError) HalCode() int32 { return e.Code }
