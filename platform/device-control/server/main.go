package main

import (
	"context"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"net"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/connectivity"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"

	camerapb "aipc/platform/camera-daemon/proto"
	"aipc/platform/common/config"
	"aipc/platform/common/constants"
	"aipc/platform/common/logger"
	"aipc/platform/common/socket"
	"aipc/platform/common/utils"
	"aipc/platform/device-control/hal"
	"aipc/platform/device-control/lens"
	pb "aipc/platform/device-control/proto"
	eventpb "aipc/platform/event-bus/proto"
)

var (
	configPath = flag.String("config", "/data/aipc/etc/device-control.yaml", "Path to configuration file")
)

type Config struct {
	Service struct {
		Name     string `yaml:"name"`
		Listen   string `yaml:"listen"`
		LogLevel string `yaml:"log_level"`
		LogFile  string `yaml:"log_file"`
	} `yaml:"service"`

	CameraDaemon struct {
		LensEndpoint          string `yaml:"lens_endpoint"`
		CameraControlEndpoint string `yaml:"camera_control_endpoint"`
	} `yaml:"camera_daemon"`

	MCU struct {
		Protocol   string `yaml:"protocol"`
		Device     string `yaml:"device"`
		Baudrate   int    `yaml:"baudrate"`
		TimeoutMs  int    `yaml:"timeout_ms"`
		MaxRetries int    `yaml:"max_retries"`
	} `yaml:"mcu"`

	Capabilities struct {
		Light struct {
			WhiteLight bool `yaml:"white_light"`
			IrLed      bool `yaml:"ir_led"`
			IrCut      bool `yaml:"ir_cut"`
		} `yaml:"light"`
		PTZ struct {
			Enabled bool `yaml:"enabled"`
			Presets int  `yaml:"presets"`
		} `yaml:"ptz"`
		Lens struct {
			Zoom              bool       `yaml:"zoom"`
			Focus             bool       `yaml:"focus"`
			Autofocus         bool       `yaml:"autofocus"`
			Iris              bool       `yaml:"iris"`
			ZoomRange         [2]float32 `yaml:"zoom_range"`
			FocusRange        [2]float32 `yaml:"focus_range"`
			DefaultZoomLimit  [2]int32   `yaml:"default_zoom_limit"`
			DefaultFocusLimit [2]int32   `yaml:"default_focus_limit"`
		} `yaml:"lens"`
		GPIO struct {
			AvailablePins []uint32 `yaml:"available_pins"`
			InputPins     []uint32 `yaml:"input_pins"`
			OutputPins    []uint32 `yaml:"output_pins"`
		} `yaml:"gpio"`
	} `yaml:"capabilities"`

	EventBus struct {
		Enabled       bool     `yaml:"enabled"`
		Endpoint      string   `yaml:"endpoint"`
		PublishEvents []string `yaml:"publish_events"`
	} `yaml:"event_bus"`
}

func (c *Config) Validate() error {
	if c.Service.Listen == "" {
		return fmt.Errorf("service.listen is required")
	}
	return nil
}

// DeviceControlServer implements the DeviceControl service
type DeviceControlServer struct {
	pb.UnimplementedDeviceControlServer
	config           *Config
	halLens          hal.LensHAL
	autofocusEnabled bool
	lensStatusMu     sync.RWMutex
	lastLensStatus   lensStatusCache
	hasLensStatus    bool

	// Factory-fitted lens model and its optical ratio ceiling, learned via
	// ProfileGet during lens init. Guarded by lensStatusMu. Empty model (or
	// an old camera-daemon without the RPC) means "assume AF0832".
	lensModel            string
	lensMaxZoomRatio     float32
	lensZoomTravelSteps  int32
	lensFocusTravelSteps int32

	// Camera-daemon gRPC client for IR-Cut control
	cameraDaemonClient camerapb.CameraControlClient
	cameraDaemonConn   *grpc.ClientConn

	// Event Bus client
	eventBusClient eventpb.EventBusClient
	eventBusConn   *grpc.ClientConn
	eventBusMutex  sync.RWMutex
}

type lensStatusCache struct {
	ZoomState     uint32
	FocusState    uint32
	ZoomRzDone    bool
	FocusRzDone   bool
	ZoomPos       int32
	FocusPos      int32
	IrisAdc       uint32
	ZoomLimitMin  int32
	ZoomLimitMax  int32
	FocusLimitMin int32
	FocusLimitMax int32
	ZoomRatio     float32
	HasZoomRatio  bool
}

func NewDeviceControlServer(cfg *Config, lensHal hal.LensHAL, cameraDaemonClient camerapb.CameraControlClient, cameraDaemonConn *grpc.ClientConn) *DeviceControlServer {
	server := &DeviceControlServer{
		config:             cfg,
		halLens:            lensHal,
		cameraDaemonClient: cameraDaemonClient,
		cameraDaemonConn:   cameraDaemonConn,
	}

	// Connect to Event Bus if enabled
	if cfg.EventBus.Enabled && cfg.EventBus.Endpoint != "" {
		if err := server.connectToEventBus(cfg.EventBus.Endpoint); err != nil {
			logger.Warn("Failed to connect to Event Bus: %v (will continue without event publishing)", err)
		} else {
			logger.Info("Connected to Event Bus: %s", cfg.EventBus.Endpoint)
		}
	}

	return server
}

// connectToEventBus connects to the Event Bus service
func (s *DeviceControlServer) connectToEventBus(endpoint string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	parsedAddr, err := utils.ParseListenAddress(endpoint)
	if err != nil {
		return fmt.Errorf("failed to parse event bus address: %w", err)
	}

	conn, err := grpc.DialContext(ctx, "unix://"+parsedAddr,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithBlock())
	if err != nil {
		return fmt.Errorf("failed to dial event bus: %w", err)
	}

	s.eventBusMutex.Lock()
	s.eventBusConn = conn
	s.eventBusClient = eventpb.NewEventBusClient(conn)
	s.eventBusMutex.Unlock()

	return nil
}

// publishEvent publishes an event to Event Bus if enabled
func (s *DeviceControlServer) publishEvent(eventType string, data map[string]interface{}) {
	if !s.config.EventBus.Enabled {
		return
	}

	// Check if this event type should be published
	shouldPublish := false
	for _, event := range s.config.EventBus.PublishEvents {
		if event == eventType {
			shouldPublish = true
			break
		}
	}
	if !shouldPublish {
		return
	}

	s.eventBusMutex.RLock()
	client := s.eventBusClient
	s.eventBusMutex.RUnlock()

	if client == nil {
		return
	}

	// Serialize data to JSON
	payload, err := json.Marshal(data)
	if err != nil {
		logger.Warn("Failed to serialize event data for device event '%s': %v", eventType, err)
		return
	}

	// Publish event
	ctx, cancel := context.WithTimeout(context.Background(), constants.EventPublishTimeout)
	defer cancel()

	topic := fmt.Sprintf("device/%s", eventType)
	_, err = client.Publish(ctx, &eventpb.PublishRequest{
		Event: &eventpb.Event{
			Topic:       topic,
			TimestampNs: uint64(time.Now().UnixNano()),
			Source:      "device-control",
			EventId:     fmt.Sprintf("dev-%d", time.Now().UnixNano()),
			Payload:     payload,
			PayloadType: "json",
			Metadata: map[string]string{
				"event_type": eventType,
			},
		},
	})

	if err != nil {
		// Event publishing failure is non-critical, but should be logged as warning
		logger.Warn("Failed to publish device event '%s' to event bus: %v", eventType, err)
	}
}

func (s *DeviceControlServer) ensureFocusReady() (*hal.LensState, error) {
	if s.halLens == nil {
		return nil, fmt.Errorf("lens HAL not initialized")
	}
	if err := s.ensureLensBootstrapped(); err != nil {
		return nil, err
	}
	state, err := s.halLens.StateGet()
	if err != nil {
		return nil, fmt.Errorf("state_get failed: %w", err)
	}
	if !state.FocusRzDone {
		logger.Warn("focus_rz_done=false, auto running focus reset-zero")
		if err := s.halLens.StopAndWaitAll(2 * time.Second); err != nil {
			return nil, fmt.Errorf("stop motors before focus reset-zero failed: %w", err)
		}
		// Plain reset-zero, best-effort with one recover+retry. A failure here is
		// NOT fatal: the -247 false-homing most often surfaces as a reset-zero or
		// rz-wait timeout, and the Run-to-zero recovery below is the proven fix.
		if err := s.halLens.FocusResetZero(); err != nil {
			if isRecoverable(err) {
				logger.Warn("focus reset-zero transient error, recovering and retrying once: %v", err)
				if re := s.recoverLensLink(); re != nil {
					logger.Warn("focus reset-zero recover failed (will try Run-to-zero): %v", re)
				} else if err2 := s.halLens.FocusResetZero(); err2 != nil {
					logger.Warn("auto focus reset-zero retry failed (will try Run-to-zero): %v", err2)
				}
			} else {
				logger.Warn("auto focus reset-zero failed (will try Run-to-zero): %v", err)
			}
		}
		// Best-effort wait for the easy case; ignore timeout — the latch is
		// re-checked via StateGet below, and Run-to-zero handles the not-latched case.
		if err := s.halLens.WaitFocusRzDone(15 * time.Second); err != nil {
			logger.Warn("wait auto focus reset-zero timed out (will try Run-to-zero): %v", err)
		}
		state, err = s.halLens.StateGet()
		if err != nil {
			return nil, fmt.Errorf("state_get after auto focus reset-zero failed: %w", err)
		}
		if !state.FocusRzDone {
			logger.Warn("focus rz not latched after rz; trying Run-to-zero recovery")
			if err := s.runToZeroAndHome("focus"); err != nil {
				return nil, fmt.Errorf("focus run-to-zero recovery: %w", err)
			}
			// runToZeroAndHome issues rz but does not wait for it to latch; wait
			// here and re-verify before declaring the axis ready.
			if err := s.halLens.WaitFocusRzDone(15 * time.Second); err != nil {
				return nil, fmt.Errorf("focus rz wait after run-to-zero failed: %w", err)
			}
			state, err = s.halLens.StateGet()
			if err != nil {
				return nil, fmt.Errorf("state_get after focus run-to-zero failed: %w", err)
			}
			if !state.FocusRzDone {
				return nil, fmt.Errorf("focus rz not latched even after Run-to-zero recovery")
			}
		}
	}
	return &state, nil
}

func (s *DeviceControlServer) ensureZoomReady() (*hal.LensState, error) {
	if s.halLens == nil {
		return nil, fmt.Errorf("lens HAL not initialized")
	}
	if err := s.ensureLensBootstrapped(); err != nil {
		return nil, err
	}
	state, err := s.halLens.StateGet()
	if err != nil {
		return nil, fmt.Errorf("state_get failed: %w", err)
	}
	if !state.ZoomRzDone {
		if state.ZoomState == hal.MotorStateStopped {
			logger.Warn("zoom_rz_done=false but motor stopped, proceeding without reset-zero")
		} else {
			logger.Warn("zoom_rz_done=false and motor not stopped, auto running zoom reset-zero")
			if err := s.halLens.StopAndWaitAll(2 * time.Second); err != nil {
				return nil, fmt.Errorf("stop motors before zoom reset-zero failed: %w", err)
			}
			if err := s.halLens.ZoomResetZero(); err != nil {
				if isRecoverable(err) {
					logger.Warn("zoom reset-zero transient error, recovering and retrying once: %v", err)
					if re := s.recoverLensLink(); re != nil {
						logger.Warn("zoom reset-zero recover failed (will try Run-to-zero): %v", re)
					} else if err2 := s.halLens.ZoomResetZero(); err2 != nil {
						logger.Warn("auto zoom reset-zero retry failed (will try Run-to-zero): %v", err2)
					}
				} else {
					logger.Warn("auto zoom reset-zero failed (will try Run-to-zero): %v", err)
				}
			}
			// Best-effort wait for the easy case; ignore timeout — the latch is
			// re-checked via StateGet below, and Run-to-zero handles the not-latched case.
			if err := s.halLens.WaitZoomRzDone(15 * time.Second); err != nil {
				logger.Warn("wait auto zoom reset-zero timed out (will try Run-to-zero): %v", err)
			}
			state, err = s.halLens.StateGet()
			if err != nil {
				return nil, fmt.Errorf("state_get after auto zoom reset-zero failed: %w", err)
			}
			if !state.ZoomRzDone {
				logger.Warn("zoom rz not latched after rz; trying Run-to-zero recovery")
				if err := s.runToZeroAndHome("zoom"); err != nil {
					return nil, fmt.Errorf("zoom run-to-zero recovery: %w", err)
				}
				// runToZeroAndHome issues rz but does not wait for it to latch; wait
				// here and re-verify before declaring the axis ready.
				if err := s.halLens.WaitZoomRzDone(15 * time.Second); err != nil {
					return nil, fmt.Errorf("zoom rz wait after run-to-zero failed: %w", err)
				}
				state, err = s.halLens.StateGet()
				if err != nil {
					return nil, fmt.Errorf("state_get after zoom run-to-zero failed: %w", err)
				}
				if !state.ZoomRzDone {
					return nil, fmt.Errorf("zoom rz not latched even after Run-to-zero recovery")
				}
			}
		}
	}
	return &state, nil
}

func (s *DeviceControlServer) ensureLensBootstrapped() error {
	if s.halLens == nil {
		return fmt.Errorf("lens HAL not initialized")
	}
	if s.lensIsFg2009() {
		// Open-loop lens: StateGet reports the dead-reckoning anchor state
		// as rz-done. An unanchored model is only repaired by the full
		// profile-push + anchor + park sequence inside ReInit.
		state, err := s.halLens.StateGet()
		if err != nil {
			return fmt.Errorf("state_get failed: %w", err)
		}
		if state.ZoomRzDone && state.FocusRzDone {
			return nil
		}
		logger.Warn("fg2009 lens not anchored, running full reinit bootstrap")
		if err := s.halLens.ReInit(); err != nil {
			return fmt.Errorf("fg2009 reinit failed: %w", err)
		}
		return nil
	}
	if !s.halLens.IsAF0832Bootstrapped() {
		// Flag is absent (post-restart, or a transient IsAF0832Bootstrapped
		// transport error), but the motors may already be homed. Skip the
		// audible mechanical re-bootstrap when both axes report rz-done; just
		// re-mark and proceed. Avoids the "咔嚓" of a needless re-home.
		if zd, fd, err := s.lensHomed(); err == nil && zd && fd {
			logger.Info("Lens already homed (zoom+focus rz-done); re-marking bootstrapped without re-bootstrap")
			_ = s.halLens.AF0832MarkBootstrapped()
			return nil
		}
		logger.Warn("Lens not bootstrapped, running full bootstrap sequence")
		if err := s.halLens.ReInit(); err != nil {
			return fmt.Errorf("auto reinit failed: %w", err)
		}
		time.Sleep(300 * time.Millisecond)
		if err := s.halLens.AF0832Bootstrap(); err != nil {
			logger.Warn("AF0832 bootstrap failed (%v), trying Run-to-zero recovery before mark", err)
			if rerr := s.runToZeroAndHome("both"); rerr != nil {
				logger.Warn("Run-to-zero recovery failed: %v", rerr)
			}
			zd, fd, _ := s.lensHomed()
			if zd && fd {
				logger.Warn("axes homed after Run-to-zero recovery, marking bootstrapped")
				_ = s.halLens.AF0832MarkBootstrapped()
			} else {
				return fmt.Errorf("lens bootstrap failed and not homed after Run-to-zero recovery (zoom=%v focus=%v): %w", zd, fd, err)
			}
		} else {
			_ = s.halLens.AF0832MarkBootstrapped()
		}
	}
	return nil
}

func (s *DeviceControlServer) recoverLensLink() error {
	if s.halLens == nil {
		return fmt.Errorf("lens HAL not initialized")
	}
	_ = s.halLens.StopAndWaitAll(2 * time.Second)
	if s.lensIsFg2009() {
		// ReInit re-pushes the MCU profile and re-anchors the open-loop
		// model; there is no AF0832 zero/bootstrap step to redo.
		if err := s.halLens.ReInit(); err != nil {
			return fmt.Errorf("fg2009 reinit failed: %w", err)
		}
		return nil
	}
	if err := s.halLens.ReInit(); err != nil {
		return fmt.Errorf("lens reinit failed: %w", err)
	}
	time.Sleep(300 * time.Millisecond)
	if err := s.halLens.AF0832ForceResetZero(); err != nil {
		logger.Warn("recoverLensLink: force reset-zero failed (non-fatal): %v", err)
	}
	time.Sleep(200 * time.Millisecond)
	if err := s.halLens.AF0832Bootstrap(); err != nil {
		logger.Warn("recoverLensLink: bootstrap failed (%v), falling back to mark-only", err)
		if err2 := s.halLens.AF0832MarkBootstrapped(); err2 != nil {
			return fmt.Errorf("mark bootstrapped failed: %w", err2)
		}
	}
	time.Sleep(120 * time.Millisecond)
	return nil
}

// lensHomed reports live per-axis reset-zero (home) status from the MCU.
func (s *DeviceControlServer) lensHomed() (zoomDone, focusDone bool, err error) {
	st, err := s.halLens.StateGet()
	if err != nil {
		return false, false, err
	}
	return st.ZoomRzDone, st.FocusRzDone, nil
}

// lensIsFg2009 reports whether the factory-fitted lens is the FG2009
// open-loop model. False until ProfileGet succeeds during lens init (or on
// an old camera-daemon without the RPC), which by default means AF0832.
func (s *DeviceControlServer) lensIsFg2009() bool {
	s.lensStatusMu.RLock()
	defer s.lensStatusMu.RUnlock()
	return s.lensModel == "fg2009"
}

// runToZeroAndHome recovers an axis stuck in the "reset-done event fired but home
// not latched" state (AF event result=-247), where plain rz / bootstrap spin
// forever. It explicitly Runs the motor to position 0 (steps = -current_pos),
// then re-rz — the only sequence proven (field log 2026-06-16) to clear -247.
// axis: "zoom" | "focus" | "both".
func (s *DeviceControlServer) runToZeroAndHome(axis string) error {
	const (
		ppsZoom  = uint16(3000)
		ppsFocus = uint16(1000)
	)
	st, err := s.halLens.StateGet()
	if err != nil {
		return fmt.Errorf("run-to-zero state_get: %w", err)
	}

	// recoverAxis: stop all, Run one motor to 0, then rz so the home sensor can latch.
	recoverAxis := func(name string, rzDone bool, pos int32, pps uint16,
		run func(uint16, int32) error, wait func(time.Duration) error, rz func() error,
	) error {
		if rzDone {
			return nil
		}
		logger.Warn("%s rz not latched (-247); Run-to-zero then re-rz: pos=%d", name, pos)
		if err := s.halLens.StopAndWaitAll(2 * time.Second); err != nil {
			return fmt.Errorf("%s stop before run: %w", name, err)
		}
		if pos != 0 {
			if err := run(pps, -pos); err != nil {
				return fmt.Errorf("%s run-to-zero: %w", name, err)
			}
			if err := wait(5 * time.Second); err != nil {
				return fmt.Errorf("%s wait run-to-zero: %w", name, err)
			}
		}
		if err := rz(); err != nil {
			return fmt.Errorf("%s rz after run: %w", name, err)
		}
		return nil
	}

	if axis == "zoom" || axis == "both" {
		if err := recoverAxis("zoom", st.ZoomRzDone, st.ZoomPos, ppsZoom,
			s.halLens.ZoomRun, s.halLens.WaitZoomStopped, s.halLens.ZoomResetZero); err != nil {
			return err
		}
	}
	if axis == "focus" || axis == "both" {
		if err := recoverAxis("focus", st.FocusRzDone, st.FocusPos, ppsFocus,
			s.halLens.FocusRun, s.halLens.WaitFocusStopped, s.halLens.FocusResetZero); err != nil {
			return err
		}
	}
	return nil
}

// isRecoverable returns true for errors that can be resolved by
// reinitializing the MCU-to-lens link and retrying the operation.
//
// It classifies these as recoverable:
//   - All *lens.HalError values (errors from the HAL bridge / MCU)
//   - gRPC transport errors that indicate a broken or timed-out connection
//     (Unavailable, DeadlineExceeded, Internal, Aborted)
//   - String heuristics for OS-level transport failures
//   - Known MCU transient error codes (-2810, -2815)
//
// The errors.As check for *lens.HalError MUST come first: status.FromError
// returns ok=true for any error wrapped through the gRPC framework, so
// ordering prevents HAL errors from being double-classified.
func isRecoverable(err error) bool {
	if err == nil {
		return false
	}

	// All HAL errors from the lens bridge are potentially recoverable.
	// Must be checked before status.FromError to prevent gRPC-wrapped
	// HAL errors from reaching the code-based switch below.
	var halErr *lens.HalError
	if errors.As(err, &halErr) {
		return true
	}

	// gRPC transport-layer errors: status.FromError returns ok=true only
	// for errors produced by the gRPC status package (i.e., actual RPC
	// failures). Non-gRPC errors reach the string heuristics below.
	if st, ok := status.FromError(err); ok {
		switch st.Code() {
		case codes.Unavailable, codes.DeadlineExceeded,
			codes.Internal, codes.Aborted:
			return true
			// codes.Unknown is intentionally excluded: it's the default
			// gRPC code and could mask permanent failures.
		}
	}

	// String-based heuristics for OS-level transport errors that may
	// not be wrapped as gRPC status (e.g., from grpc.DialContext).
	msg := err.Error()
	if strings.Contains(msg, "timeout") ||
		strings.Contains(msg, "broken pipe") ||
		strings.Contains(msg, "connection refused") ||
		strings.Contains(msg, "connection reset") ||
		strings.Contains(msg, "transport is closing") {
		return true
	}

	// Known MCU transient error codes — preserved for errors that
	// reach this point without being wrapped in *lens.HalError
	// (e.g., embedded in gRPC status messages).
	if strings.Contains(msg, "-2810") || strings.Contains(msg, "-2815") {
		return true
	}

	return false
}

// retryWithRecover executes fn once, and if it fails with a recoverable
// error, calls recoverLensLink() and retries fn exactly once more.
//
// Note: when used with Wait* operations (e.g., WaitZoomStopped), the
// first attempt may block for the full timeout (typically 15s). After
// recoverLensLink() resets the motors, the retry returns quickly since
// motors are already stopped. The total worst-case latency on the error
// path is ~20s — ensure the caller's gRPC deadline accommodates this.
func (s *DeviceControlServer) retryWithRecover(opName string, fn func() error) error {
	err := fn()
	if err == nil {
		return nil
	}
	if !isRecoverable(err) {
		return err
	}
	logger.Warn("%s failed with recoverable error, recovering lens link and retrying: %v", opName, err)
	if re := s.recoverLensLink(); re != nil {
		return fmt.Errorf("%s recover failed: %w", opName, re)
	}
	if err2 := fn(); err2 != nil {
		return fmt.Errorf("%s retry failed: %w", opName, err2)
	}
	return nil
}

func (s *DeviceControlServer) lensZoomRatioBounds() (float32, float32) {
	minRatio := s.config.Capabilities.Lens.ZoomRange[0]
	maxRatio := s.config.Capabilities.Lens.ZoomRange[1]
	// AF0832 optical zoom range fallback.
	if minRatio <= 0 {
		minRatio = 1.0
	}
	if maxRatio <= minRatio {
		maxRatio = 2.88
	}
	return minRatio, maxRatio
}

func (s *DeviceControlServer) lensAutofocusDistanceM() float32 {
	minD := s.config.Capabilities.Lens.FocusRange[0]
	maxD := s.config.Capabilities.Lens.FocusRange[1]
	if minD <= 0 {
		minD = 0.5
	}
	if maxD <= minD {
		maxD = 10.0
	}
	return (minD + maxD) / 2.0
}

func (s *DeviceControlServer) zoomLevelFromRatio(ratio float32) float32 {
	minRatio, maxRatio := s.lensZoomRatioBounds()
	if ratio <= minRatio {
		return 0
	}
	if ratio >= maxRatio {
		return 1
	}
	return (ratio - minRatio) / (maxRatio - minRatio)
}

func (s *DeviceControlServer) focusLevelFromDistance(distanceM float32) float32 {
	minD := s.config.Capabilities.Lens.FocusRange[0]
	maxD := s.config.Capabilities.Lens.FocusRange[1]
	if minD <= 0 {
		minD = 0.5
	}
	if maxD <= minD {
		maxD = 10.0
	}
	if distanceM <= minD {
		return 0
	}
	if distanceM >= maxD {
		return 1
	}
	return (distanceM - minD) / (maxD - minD)
}

func (s *DeviceControlServer) fallbackGotoByAbs(zoomRatio, focusDistanceM float32) error {
	tryOnce := func() error {
		if err := s.halLens.StopAndWaitAll(2 * time.Second); err != nil {
			return fmt.Errorf("fallback goto stop motors failed: %w", err)
		}

		zlim := s.halLens.ZoomLimits()
		flim := s.halLens.FocusLimits()
		zoomPos := hal.LevelToPosition(s.zoomLevelFromRatio(zoomRatio), zlim)
		focusPos := hal.LevelToPosition(s.focusLevelFromDistance(focusDistanceM), flim)

		if err := s.halLens.ZoomAbs(2000, zoomPos); err != nil {
			return fmt.Errorf("fallback zoom abs failed: %w", err)
		}
		if err := s.halLens.WaitZoomStopped(15 * time.Second); err != nil {
			return fmt.Errorf("fallback wait zoom stop failed: %w", err)
		}
		if err := s.halLens.FocusAbs(2000, focusPos); err != nil {
			return fmt.Errorf("fallback focus abs failed: %w", err)
		}
		if err := s.halLens.WaitFocusStopped(15 * time.Second); err != nil {
			return fmt.Errorf("fallback wait focus stop failed: %w", err)
		}
		return nil
	}

	if err := tryOnce(); err != nil {
		if isRecoverable(err) {
			logger.Warn("fallback abs transient error, recovering and retrying once: %v", err)
			if re := s.recoverLensLink(); re != nil {
				return fmt.Errorf("fallback abs recover failed: %w", re)
			}
			return tryOnce()
		}
		return err
	}
	return nil
}

// Light Control

func (s *DeviceControlServer) SetWhiteLight(ctx context.Context, req *pb.LightLevelRequest) (*pb.Status, error) {
	logger.Debug("SetWhiteLight: level=%d", req.Level)

	if req.Level > 100 {
		return &pb.Status{
			Success: false,
			Message: "Level must be 0-100",
		}, nil
	}

	if s.cameraDaemonClient != nil {
		resp, err := s.cameraDaemonClient.SetLedDuty(ctx, &camerapb.SetLedDutyRequest{
			LedId:       0, // white light
			DutyPercent: uint32(req.Level),
		})
		if err != nil {
			logger.Error("SetWhiteLight: camera-daemon call failed: %v", err)
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		if !resp.Success {
			return &pb.Status{Success: false, Message: resp.Message}, nil
		}
	}

	s.publishEvent("white_light_change", map[string]interface{}{
		"level": req.Level,
	})

	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) SetIrLed(ctx context.Context, req *pb.LightLevelRequest) (*pb.Status, error) {
	logger.Debug("SetIrLed: level=%d", req.Level)

	if req.Level > 100 {
		return &pb.Status{
			Success: false,
			Message: "Level must be 0-100",
		}, nil
	}

	if s.cameraDaemonClient != nil {
		resp, err := s.cameraDaemonClient.SetLedDuty(ctx, &camerapb.SetLedDutyRequest{
			LedId:       1, // IR LED
			DutyPercent: req.Level,
		})
		if err != nil {
			logger.Error("SetIrLed: camera-daemon call failed: %v", err)
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		if !resp.Success {
			return &pb.Status{Success: false, Message: resp.Message}, nil
		}
	}

	s.publishEvent("ir_led_change", map[string]interface{}{
		"level": req.Level,
	})

	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) SetIrCut(ctx context.Context, req *pb.IrCutRequest) (*pb.Status, error) {
	logger.Debug("SetIrCut: mode=%v", req.Mode)

	if s.cameraDaemonClient == nil {
		return &pb.Status{Success: false, Message: "Camera daemon not connected"}, nil
	}

	mode := uint32(0) // day
	switch req.Mode {
	case pb.IrCutMode_IRCUT_NIGHT:
		mode = 1
	case pb.IrCutMode_IRCUT_DAY:
		mode = 0
	default: // AUTO → default to day
		mode = 0
	}

	resp, err := s.cameraDaemonClient.SetIrCut(ctx, &camerapb.SetIrCutRequest{Mode: mode})
	if err != nil {
		logger.Error("SetIrCut: camera-daemon call failed: %v", err)
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	return &pb.Status{Success: resp.Success, Message: resp.Message}, nil
}

func infraredStatusFromCamera(resp *camerapb.InfraredStatusResponse) *pb.InfraredStatusResponse {
	if resp == nil {
		return &pb.InfraredStatusResponse{Success: false, Message: "empty camera response"}
	}
	return &pb.InfraredStatusResponse{
		Success: resp.Success, Message: resp.Message, Mode: resp.Mode,
		Transition: resp.Transition, OutputSource: resp.OutputSource,
		AutoFollow: resp.AutoFollow, FollowActive: resp.FollowActive,
		ManualOverride: resp.ManualOverride, Degraded: resp.Degraded,
		RequestedNearPwm: resp.RequestedNearPwm, RequestedFarPwm: resp.RequestedFarPwm,
		AppliedNearPwm: resp.AppliedNearPwm, AppliedFarPwm: resp.AppliedFarPwm,
		ZoomRatio: resp.ZoomRatio, ActiveProfile: resp.ActiveProfile,
		SelectedMode: resp.SelectedMode, LightPercent: resp.LightPercent,
		LightMv: resp.LightMv, LightMilli: resp.LightMilli,
		LightValid: resp.LightValid, NightEnter: resp.NightEnter, DayEnter: resp.DayEnter,
	}
}

func (s *DeviceControlServer) SetImagingMode(ctx context.Context, req *pb.ImagingModeRequest) (*pb.InfraredStatusResponse, error) {
	if s.cameraDaemonClient == nil {
		return &pb.InfraredStatusResponse{Success: false, Message: "Camera daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.SetImagingMode(ctx, &camerapb.ImagingModeRequest{Mode: req.Mode})
	if err != nil {
		return &pb.InfraredStatusResponse{Success: false, Message: err.Error()}, nil
	}
	return infraredStatusFromCamera(resp), nil
}

func (s *DeviceControlServer) GetInfraredStatus(ctx context.Context, _ *pb.Empty) (*pb.InfraredStatusResponse, error) {
	if s.cameraDaemonClient == nil {
		return &pb.InfraredStatusResponse{Success: false, Message: "Camera daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.GetInfraredStatus(ctx, &camerapb.Empty{})
	if err != nil {
		return &pb.InfraredStatusResponse{Success: false, Message: err.Error()}, nil
	}
	return infraredStatusFromCamera(resp), nil
}

func (s *DeviceControlServer) SetInfraredSettings(ctx context.Context, req *pb.InfraredSettingsRequest) (*pb.InfraredStatusResponse, error) {
	if s.cameraDaemonClient == nil {
		return &pb.InfraredStatusResponse{Success: false, Message: "Camera daemon not connected"}, nil
	}
	cameraReq := &camerapb.InfraredSettingsRequest{}
	if req.AutoFollow != nil {
		cameraReq.AutoFollow = req.AutoFollow
	}
	if req.NearPwm != nil {
		cameraReq.NearPwm = req.NearPwm
	}
	if req.FarPwm != nil {
		cameraReq.FarPwm = req.FarPwm
	}
	if req.NightEnter != nil {
		cameraReq.NightEnter = req.NightEnter
	}
	if req.DayEnter != nil {
		cameraReq.DayEnter = req.DayEnter
	}
	resp, err := s.cameraDaemonClient.SetInfraredSettings(ctx, cameraReq)
	if err != nil {
		return &pb.InfraredStatusResponse{Success: false, Message: err.Error()}, nil
	}
	return infraredStatusFromCamera(resp), nil
}

func (s *DeviceControlServer) ClearInfraredManual(ctx context.Context, _ *pb.Empty) (*pb.InfraredStatusResponse, error) {
	if s.cameraDaemonClient == nil {
		return &pb.InfraredStatusResponse{Success: false, Message: "Camera daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.ClearInfraredManual(ctx, &camerapb.Empty{})
	if err != nil {
		return &pb.InfraredStatusResponse{Success: false, Message: err.Error()}, nil
	}
	return infraredStatusFromCamera(resp), nil
}

func irPresetListFromCamera(resp *camerapb.IrPresetListResponse) *pb.IrPresetListResponse {
	if resp == nil {
		return &pb.IrPresetListResponse{Success: false, Message: "empty camera response"}
	}
	out := &pb.IrPresetListResponse{Success: resp.Success, Message: resp.Message}
	for _, p := range resp.Presets {
		out.Presets = append(out.Presets, &pb.IrPreset{
			Name: p.Name, ZoomRatio: p.ZoomRatio, NearPwm: p.NearPwm, FarPwm: p.FarPwm,
		})
	}
	return out
}

func (s *DeviceControlServer) ListIrPresets(ctx context.Context, _ *pb.Empty) (*pb.IrPresetListResponse, error) {
	if s.cameraDaemonClient == nil {
		return &pb.IrPresetListResponse{Success: false, Message: "Camera daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.ListIrPresets(ctx, &camerapb.Empty{})
	if err != nil {
		return &pb.IrPresetListResponse{Success: false, Message: err.Error()}, nil
	}
	return irPresetListFromCamera(resp), nil
}

func (s *DeviceControlServer) SaveIrPreset(ctx context.Context, req *pb.IrPreset) (*pb.IrPresetListResponse, error) {
	if s.cameraDaemonClient == nil {
		return &pb.IrPresetListResponse{Success: false, Message: "Camera daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.SaveIrPreset(ctx, &camerapb.IrPreset{
		Name: req.Name, ZoomRatio: req.ZoomRatio, NearPwm: req.NearPwm, FarPwm: req.FarPwm,
	})
	if err != nil {
		return &pb.IrPresetListResponse{Success: false, Message: err.Error()}, nil
	}
	return irPresetListFromCamera(resp), nil
}

func (s *DeviceControlServer) DeleteIrPreset(ctx context.Context, req *pb.DeleteIrPresetRequest) (*pb.IrPresetListResponse, error) {
	if s.cameraDaemonClient == nil {
		return &pb.IrPresetListResponse{Success: false, Message: "Camera daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.DeleteIrPreset(ctx, &camerapb.DeleteIrPresetRequest{Name: req.Name})
	if err != nil {
		return &pb.IrPresetListResponse{Success: false, Message: err.Error()}, nil
	}
	return irPresetListFromCamera(resp), nil
}

// PTZ Control

func (s *DeviceControlServer) Pan(ctx context.Context, req *pb.PanRequest) (*pb.Status, error) {
	logger.Debug("Pan: direction=%v, speed=%d", req.Direction, req.Speed)

	if !s.config.Capabilities.PTZ.Enabled {
		return &pb.Status{
			Success: false,
			Message: "PTZ not enabled",
		}, nil
	}

	if s.cameraDaemonClient != nil {
		payload := []byte{byte(req.Direction), byte(req.Speed)}
		resp, err := s.cameraDaemonClient.McuRawRequest(ctx, &camerapb.McuRawRequestMessage{
			Cmd:     0x20, // HOST_LINK_CMD_PTZ_PAN
			Payload: payload,
		})
		if err != nil {
			logger.Error("Pan: MCU command failed: %v", err)
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		if !resp.Success {
			return &pb.Status{Success: false, Message: resp.Message}, nil
		}
	}

	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) Tilt(ctx context.Context, req *pb.TiltRequest) (*pb.Status, error) {
	logger.Debug("Tilt: direction=%v, speed=%d", req.Direction, req.Speed)

	if !s.config.Capabilities.PTZ.Enabled {
		return &pb.Status{
			Success: false,
			Message: "PTZ not enabled",
		}, nil
	}

	if s.cameraDaemonClient != nil {
		payload := []byte{byte(req.Direction), byte(req.Speed)}
		resp, err := s.cameraDaemonClient.McuRawRequest(ctx, &camerapb.McuRawRequestMessage{
			Cmd:     0x21, // HOST_LINK_CMD_PTZ_TILT
			Payload: payload,
		})
		if err != nil {
			logger.Error("Tilt: MCU command failed: %v", err)
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		if !resp.Success {
			return &pb.Status{Success: false, Message: resp.Message}, nil
		}
	}

	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) PTZStop(ctx context.Context, req *pb.PTZStopRequest) (*pb.Status, error) {
	logger.Debug("PTZStop")

	if s.cameraDaemonClient != nil {
		resp, err := s.cameraDaemonClient.McuRawRequest(ctx, &camerapb.McuRawRequestMessage{
			Cmd: 0x22, // HOST_LINK_CMD_PTZ_STOP
		})
		if err != nil {
			logger.Error("PTZStop: MCU command failed: %v", err)
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		if !resp.Success {
			return &pb.Status{Success: false, Message: resp.Message}, nil
		}
	}

	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) SavePreset(ctx context.Context, req *pb.PresetRequest) (*pb.Status, error) {
	logger.Info("SavePreset: id=%d", req.PresetId)

	if req.PresetId == 0 || req.PresetId > uint32(s.config.Capabilities.PTZ.Presets) {
		return &pb.Status{
			Success: false,
			Message: fmt.Sprintf("Preset ID must be 1-%d", s.config.Capabilities.PTZ.Presets),
		}, nil
	}

	if s.cameraDaemonClient != nil {
		payload := []byte{byte(req.PresetId)}
		resp, err := s.cameraDaemonClient.McuRawRequest(ctx, &camerapb.McuRawRequestMessage{
			Cmd:     0x23, // HOST_LINK_CMD_PTZ_SAVE_PRESET
			Payload: payload,
		})
		if err != nil {
			logger.Error("SavePreset: MCU command failed: %v", err)
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		if !resp.Success {
			return &pb.Status{Success: false, Message: resp.Message}, nil
		}
	}

	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) CallPreset(ctx context.Context, req *pb.PresetRequest) (*pb.Status, error) {
	logger.Info("CallPreset: id=%d", req.PresetId)

	if req.PresetId == 0 || req.PresetId > uint32(s.config.Capabilities.PTZ.Presets) {
		return &pb.Status{
			Success: false,
			Message: fmt.Sprintf("Preset ID must be 1-%d", s.config.Capabilities.PTZ.Presets),
		}, nil
	}

	if s.cameraDaemonClient != nil {
		payload := []byte{byte(req.PresetId)}
		resp, err := s.cameraDaemonClient.McuRawRequest(ctx, &camerapb.McuRawRequestMessage{
			Cmd:     0x24, // HOST_LINK_CMD_PTZ_CALL_PRESET
			Payload: payload,
		})
		if err != nil {
			logger.Error("CallPreset: MCU command failed: %v", err)
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		if !resp.Success {
			return &pb.Status{Success: false, Message: resp.Message}, nil
		}
	}

	s.publishEvent("ptz_preset_reached", map[string]interface{}{
		"preset_id": req.PresetId,
	})

	return &pb.Status{Success: true}, nil
}

// Lens Control

func (s *DeviceControlServer) Zoom(ctx context.Context, req *pb.ZoomRequest) (*pb.Status, error) {
	logger.Debug("Zoom: speed=%d", req.Speed)

	if !s.config.Capabilities.Lens.Zoom {
		return &pb.Status{Success: false, Message: "Zoom not supported"}, nil
	}
	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}
	if _, err := s.ensureZoomReady(); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	if req.Speed == 0 {
		if err := s.halLens.ZoomStop(); err != nil {
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		s.invalidateAutofocusAnchor("manual zoom stop")
		return &pb.Status{Success: true}, nil
	}

	// Stop any running motion first to avoid MCU BUSY rejection
	_ = s.halLens.ZoomStop()
	_ = s.halLens.WaitZoomStopped(2 * time.Second)

	pps := hal.SpeedToPPS(req.Speed)
	steps := hal.SpeedToSteps(req.Speed)
	if err := s.retryWithRecover("zoom_run", func() error {
		return s.halLens.ZoomRun(pps, steps)
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	if err := s.retryWithRecover("wait_zoom_stopped", func() error {
		return s.halLens.WaitZoomStopped(15 * time.Second)
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	s.publishEvent("lens_zoom", map[string]interface{}{"speed": req.Speed})
	s.invalidateAutofocusAnchor("manual zoom movement")
	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) Focus(ctx context.Context, req *pb.FocusRequest) (*pb.Status, error) {
	logger.Debug("Focus: speed=%d", req.Speed)

	if !s.config.Capabilities.Lens.Focus {
		return &pb.Status{Success: false, Message: "Focus not supported"}, nil
	}
	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}
	if s.autofocusEnabled && req.Speed != 0 {
		return &pb.Status{Success: false, Message: "manual focus disabled while autofocus is enabled"}, nil
	}
	if _, err := s.ensureFocusReady(); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	if req.Speed == 0 {
		if err := s.halLens.FocusStop(); err != nil {
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		s.invalidateAutofocusAnchor("manual focus stop")
		return &pb.Status{Success: true}, nil
	}

	// Stop any running motion first to avoid MCU BUSY rejection
	_ = s.halLens.FocusStop()
	_ = s.halLens.WaitFocusStopped(2 * time.Second)

	pps := hal.SpeedToPPS(req.Speed)
	steps := hal.SpeedToSteps(req.Speed)
	if err := s.retryWithRecover("focus_run", func() error {
		return s.halLens.FocusRun(pps, steps)
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	if err := s.retryWithRecover("wait_focus_stopped", func() error {
		return s.halLens.WaitFocusStopped(15 * time.Second)
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	s.publishEvent("lens_focus", map[string]interface{}{"speed": req.Speed})
	s.invalidateAutofocusAnchor("manual focus movement")
	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) SetAutofocus(ctx context.Context, req *pb.AutofocusRequest) (*pb.Status, error) {
	logger.Info("SetAutofocus: enable=%v", req.Enable)

	if !s.config.Capabilities.Lens.Autofocus {
		return &pb.Status{Success: false, Message: "Autofocus not supported"}, nil
	}

	if req.Enable {
		job, err := s.StartOneShotAf(ctx, &pb.Empty{})
		if err != nil || !job.GetAccepted() {
			if err != nil {
				return &pb.Status{Success: false, Message: err.Error()}, nil
			}
			return &pb.Status{Success: false, Message: job.GetMessage()}, nil
		}
		return &pb.Status{Success: true, Message: fmt.Sprintf("queued AF job %d", job.GetJobId())}, nil
	}
	return s.CancelAutofocus(ctx, &pb.AfJobRequest{})
}

func (s *DeviceControlServer) StartOneShotAf(ctx context.Context, _ *pb.Empty) (*pb.AfJobResponse, error) {
	if s.cameraDaemonClient == nil {
		return &pb.AfJobResponse{Accepted: false, Message: "camera-daemon unavailable"}, nil
	}
	resp, err := s.cameraDaemonClient.StartOneShotAutofocus(ctx, &camerapb.Empty{})
	if err != nil {
		return &pb.AfJobResponse{Accepted: false, Message: err.Error()}, nil
	}
	return &pb.AfJobResponse{Accepted: resp.GetAccepted(), JobId: resp.GetJobId(), Message: resp.GetMessage()}, nil
}

func (s *DeviceControlServer) StartZoomFollow(ctx context.Context, req *pb.ZoomFollowRequest) (*pb.AfJobResponse, error) {
	if s.cameraDaemonClient == nil {
		return &pb.AfJobResponse{Accepted: false, Message: "camera-daemon unavailable"}, nil
	}
	resp, err := s.cameraDaemonClient.StartZoomFollow(ctx, &camerapb.AutofocusZoomFollowRequest{Ratio: req.GetRatio()})
	if err != nil {
		return &pb.AfJobResponse{Accepted: false, Message: err.Error()}, nil
	}
	return &pb.AfJobResponse{Accepted: resp.GetAccepted(), JobId: resp.GetJobId(), Message: resp.GetMessage()}, nil
}

func (s *DeviceControlServer) GetAutofocusStatus(ctx context.Context, _ *pb.Empty) (*pb.AfStatusResponse, error) {
	if s.cameraDaemonClient == nil {
		return &pb.AfStatusResponse{State: "failed", ErrorCode: -1, Message: "camera-daemon unavailable"}, nil
	}
	resp, err := s.cameraDaemonClient.GetAutofocusStatus(ctx, &camerapb.Empty{})
	if err != nil {
		return &pb.AfStatusResponse{State: "failed", ErrorCode: -1, Message: err.Error()}, nil
	}
	return &pb.AfStatusResponse{
		JobId: resp.GetJobId(), Operation: resp.GetOperation(), State: resp.GetState(),
		Progress: resp.GetProgress(), Busy: resp.GetBusy(), AnchorValid: resp.GetAnchorValid(),
		RequestedRatio: resp.GetRequestedRatio(), EffectiveRatio: resp.GetEffectiveRatio(),
		ZoomPos: resp.GetZoomPos(), FocusPos: resp.GetFocusPos(), BestFocus: resp.GetBestFocus(),
		Metric: resp.GetMetric(), Confidence: resp.GetConfidence(), Reproducibility: resp.GetReproducibility(),
		EstimatedDistanceM: resp.GetEstimatedDistanceM(), ElapsedMs: resp.GetElapsedMs(),
		ErrorCode: resp.GetErrorCode(), Message: resp.GetMessage(),
	}, nil
}

func (s *DeviceControlServer) CancelAutofocus(ctx context.Context, req *pb.AfJobRequest) (*pb.Status, error) {
	if s.cameraDaemonClient == nil {
		return &pb.Status{Success: false, Message: "camera-daemon unavailable"}, nil
	}
	resp, err := s.cameraDaemonClient.CancelAutofocus(ctx, &camerapb.AutofocusJobRequest{JobId: req.GetJobId()})
	if err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	return &pb.Status{Success: resp.GetSuccess(), Message: resp.GetMessage()}, nil
}

func (s *DeviceControlServer) invalidateAutofocusAnchor(reason string) {
	if s.cameraDaemonClient == nil {
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()
	resp, err := s.cameraDaemonClient.InvalidateAutofocusAnchor(
		ctx, &camerapb.AutofocusInvalidateRequest{Reason: reason})
	if err != nil || !resp.GetSuccess() {
		logger.Warn("invalidate autofocus anchor (%s) failed: %v", reason, err)
	}
}

func (s *DeviceControlServer) SetZoomLevel(ctx context.Context, req *pb.ZoomLevelRequest) (*pb.Status, error) {
	logger.Debug("SetZoomLevel: level=%.2f", req.Level)

	if req.Level < 0 || req.Level > 1.0 {
		return &pb.Status{Success: false, Message: "Level must be 0.0-1.0"}, nil
	}
	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}
	if s.lensIsFg2009() {
		// Level is normalized full travel; map it onto the optical-ratio
		// model instead of the AF0832 step limits.
		s.lensStatusMu.RLock()
		maxRatio := s.lensMaxZoomRatio
		s.lensStatusMu.RUnlock()
		if maxRatio <= 1 {
			maxRatio = 2.88
		}
		ratio := 1.0 + req.Level*(maxRatio-1.0)
		return s.LensGotoZoomRatio(ctx, &pb.ZoomRatioRequest{ZoomRatio: ratio})
	}
	state, err := s.ensureZoomReady()
	if err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	// Stop all axes before absolute move to avoid MCU BUSY.
	if err := s.halLens.StopAndWaitAll(2 * time.Second); err != nil {
		return &pb.Status{Success: false, Message: "stop motors before zoom abs: " + err.Error()}, nil
	}

	lim := s.halLens.ZoomLimits()
	position := hal.LevelToPosition(req.Level, lim)
	if state.ZoomPos == position {
		return &pb.Status{Success: true}, nil
	}
	if err := s.retryWithRecover("zoom_abs", func() error {
		return s.halLens.ZoomAbs(2000, position)
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	if err := s.retryWithRecover("wait_zoom_stopped", func() error {
		return s.halLens.WaitZoomStopped(15 * time.Second)
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	s.invalidateAutofocusAnchor("absolute zoom movement")
	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) GetLensStatus(ctx context.Context, req *pb.Empty) (*pb.LensStatusResponse, error) {
	if s.halLens == nil {
		return &pb.LensStatusResponse{}, nil
	}

	zlim := s.halLens.ZoomLimits()
	flim := s.halLens.FocusLimits()
	// Lens identity is server-level state (learned at init), so it stays
	// available even when the live HAL poll below fails.
	s.lensStatusMu.RLock()
	lensModel := s.lensModel
	maxRatio := s.lensMaxZoomRatio
	zoomTravel := s.lensZoomTravelSteps
	focusTravel := s.lensFocusTravelSteps
	cached := s.lastLensStatus
	hasCached := s.hasLensStatus
	s.lensStatusMu.RUnlock()
	fillLensIdentity := func(resp *pb.LensStatusResponse, ratio float32, hasRatio bool) {
		if lensModel == "" {
			return
		}
		resp.LensModel = lensModel
		if maxRatio > 1 {
			resp.ZoomRatioRange = &pb.ZoomRatioRange{Min: 1.0, Max: maxRatio}
		}
		if hasRatio {
			resp.ZoomRatio = ratio
		}
		// The open-loop model reports positions in curve coordinates; pair
		// them with the profile's travel limits instead of the AF0832 step
		// limits this service was configured with.
		if lensModel == "fg2009" && zoomTravel > 0 && focusTravel > 0 {
			resp.ZoomLimit = &pb.LensLimit{MinPos: 0, MaxPos: zoomTravel}
			resp.FocusLimit = &pb.LensLimit{MinPos: 0, MaxPos: focusTravel}
		}
	}
	fallbackStatus := func() *pb.LensStatusResponse {
		if hasCached {
			// Clamp motor states to Stopped(1) — cached Running(2)/ResetZero(3)
			// would cause the frontend to show "initializing" indefinitely when
			// the lens HAL link is down.
			zoomState := cached.ZoomState
			focusState := cached.FocusState
			if zoomState != 1 && zoomState != 4 {
				zoomState = 1
			}
			if focusState != 1 && focusState != 4 {
				focusState = 1
			}
			resp := &pb.LensStatusResponse{
				ZoomState:        zoomState,
				FocusState:       focusState,
				ZoomRzDone:       cached.ZoomRzDone,
				FocusRzDone:      cached.FocusRzDone,
				ZoomPos:          cached.ZoomPos,
				FocusPos:         cached.FocusPos,
				IrisAdc:          cached.IrisAdc,
				AutofocusEnabled: s.autofocusEnabled,
				ZoomLimit:        &pb.LensLimit{MinPos: cached.ZoomLimitMin, MaxPos: cached.ZoomLimitMax},
				FocusLimit:       &pb.LensLimit{MinPos: cached.FocusLimitMin, MaxPos: cached.FocusLimitMax},
			}
			fillLensIdentity(resp, cached.ZoomRatio, cached.HasZoomRatio)
			return resp
		}
		resp := &pb.LensStatusResponse{
			AutofocusEnabled: s.autofocusEnabled,
			ZoomLimit:        &pb.LensLimit{MinPos: zlim.MinPos, MaxPos: zlim.MaxPos},
			FocusLimit:       &pb.LensLimit{MinPos: flim.MinPos, MaxPos: flim.MaxPos},
		}
		fillLensIdentity(resp, 0, false)
		return resp
	}

	state, err := s.halLens.StateGetTry()
	if err != nil {
		if err != hal.ErrLensBusy {
			logger.Warn("GetLensStatus failed: %v", err)
		}
		return fallbackStatus(), nil
	}

	irisADC := uint32(0)
	if adc, err := s.halLens.IrisAdcGetTry(); err == nil {
		irisADC = uint32(adc)
	}

	// Ratio conversion covers both lenses: the lens HAL maps the FG2009
	// curve coordinate through the position model and the AF0832 position
	// through its ratio table.
	zoomRatio := s.halLens.AF0832PosToRatio(state.ZoomPos)

	resp := &pb.LensStatusResponse{
		ZoomState:        uint32(state.ZoomState),
		FocusState:       uint32(state.FocusState),
		ZoomRzDone:       state.ZoomRzDone,
		FocusRzDone:      state.FocusRzDone,
		ZoomPos:          state.ZoomPos,
		FocusPos:         state.FocusPos,
		IrisAdc:          irisADC,
		AutofocusEnabled: s.autofocusEnabled,
		ZoomLimit:        &pb.LensLimit{MinPos: zlim.MinPos, MaxPos: zlim.MaxPos},
		FocusLimit:       &pb.LensLimit{MinPos: flim.MinPos, MaxPos: flim.MaxPos},
	}
	fillLensIdentity(resp, zoomRatio, true)
	s.lensStatusMu.Lock()
	s.lastLensStatus = lensStatusCache{
		ZoomState:     resp.ZoomState,
		FocusState:    resp.FocusState,
		ZoomRzDone:    resp.ZoomRzDone,
		FocusRzDone:   resp.FocusRzDone,
		ZoomPos:       resp.ZoomPos,
		FocusPos:      resp.FocusPos,
		IrisAdc:       resp.IrisAdc,
		ZoomLimitMin:  resp.ZoomLimit.GetMinPos(),
		ZoomLimitMax:  resp.ZoomLimit.GetMaxPos(),
		FocusLimitMin: resp.FocusLimit.GetMinPos(),
		FocusLimitMax: resp.FocusLimit.GetMaxPos(),
		ZoomRatio:     zoomRatio,
		HasZoomRatio:  true,
	}
	s.hasLensStatus = true
	s.lensStatusMu.Unlock()
	return resp, nil
}

func (s *DeviceControlServer) SetFocusLevel(ctx context.Context, req *pb.FocusLevelRequest) (*pb.Status, error) {
	logger.Debug("SetFocusLevel: level=%.2f", req.Level)

	if req.Level < 0 || req.Level > 1.0 {
		return &pb.Status{Success: false, Message: "Level must be 0.0-1.0"}, nil
	}
	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}
	if s.autofocusEnabled {
		return &pb.Status{Success: false, Message: "manual focus disabled while autofocus is enabled"}, nil
	}
	if s.lensIsFg2009() {
		// Level 0..1 maps to NEAR..FAR on the open-loop focus curve inside
		// the lens HAL; the AF0832 step limits do not apply.
		if err := s.halLens.StopAndWaitAll(2 * time.Second); err != nil {
			return &pb.Status{Success: false, Message: "stop motors before focus goto: " + err.Error()}, nil
		}
		if err := s.retryWithRecover("focus_goto_level", func() error {
			return s.halLens.FocusGotoLevel(req.Level, 0)
		}); err != nil {
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		if err := s.retryWithRecover("wait_focus_stopped", func() error {
			return s.halLens.WaitFocusStopped(15 * time.Second)
		}); err != nil {
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		s.publishEvent("lens_focus", map[string]interface{}{"level": req.Level})
		return &pb.Status{Success: true}, nil
	}
	state, err := s.ensureFocusReady()
	if err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	// Stop all axes before absolute move to avoid MCU BUSY.
	_ = s.halLens.StopAndWaitAll(2 * time.Second)

	lim := s.halLens.FocusLimits()
	position := hal.LevelToPosition(req.Level, lim)
	if state.FocusPos == position {
		return &pb.Status{Success: true}, nil
	}
	if err := s.retryWithRecover("focus_abs", func() error {
		return s.halLens.FocusAbs(2000, position)
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	if err := s.retryWithRecover("wait_focus_stopped", func() error {
		return s.halLens.WaitFocusStopped(15 * time.Second)
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	s.invalidateAutofocusAnchor("absolute focus movement")
	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) LensResetZero(ctx context.Context, req *pb.LensResetRequest) (*pb.Status, error) {
	logger.Info("LensResetZero: zoom=%v focus=%v", req.Zoom, req.Focus)

	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}

	if err := s.ensureLensBootstrapped(); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	if err := s.halLens.StopAndWaitAll(2 * time.Second); err != nil {
		return &pb.Status{Success: false, Message: "stop motors: " + err.Error()}, nil
	}

	validateRequestedAxes := func(state hal.LensState) error {
		if state.ZoomState == 0 && state.FocusState == 0 {
			return fmt.Errorf("post reset-zero MCU in NO_CFG state")
		}
		if req.Zoom && !state.ZoomRzDone {
			return fmt.Errorf("post reset-zero zoom_rz_done=false")
		}
		if req.Focus && !state.FocusRzDone {
			return fmt.Errorf("post reset-zero focus_rz_done=false")
		}
		return nil
	}

	ensureNotNoCfg := func(stage string) error {
		st, err := s.halLens.StateGet()
		if err != nil {
			return fmt.Errorf("%s state_get failed: %w", stage, err)
		}
		if st.ZoomState == 0 && st.FocusState == 0 {
			logger.Warn("%s MCU in NO_CFG, trying ReInit", stage)
			if err := s.halLens.ReInit(); err != nil {
				return fmt.Errorf("%s reinit failed: %w", stage, err)
			}
			st2, err2 := s.halLens.StateGet()
			if err2 != nil {
				return fmt.Errorf("%s state_get after reinit failed: %w", stage, err2)
			}
			if st2.ZoomState == 0 && st2.FocusState == 0 {
				return fmt.Errorf("%s MCU still NO_CFG after reinit", stage)
			}
		}
		return nil
	}

	runSerialReset := func() error {
		if err := ensureNotNoCfg("serial_reset_precheck"); err != nil {
			return err
		}
		if req.Zoom {
			if err := s.halLens.ZoomResetZero(); err != nil {
				return fmt.Errorf("zoom reset-zero: %w", err)
			}
			if err := s.halLens.WaitZoomRzDone(15 * time.Second); err != nil {
				return fmt.Errorf("wait zoom reset-zero: %w", err)
			}
		}
		if req.Focus {
			if err := s.halLens.FocusResetZero(); err != nil {
				return fmt.Errorf("focus reset-zero: %w", err)
			}
			if err := s.halLens.WaitFocusRzDone(15 * time.Second); err != nil {
				return fmt.Errorf("wait focus reset-zero: %w", err)
			}
		}
		return nil
	}

	if req.Zoom && req.Focus {
		// Pre-check before force RZ to avoid calling it when MCU already fell into NO_CFG.
		if preState, err := s.halLens.StateGet(); err != nil {
			logger.Warn("StateGet before AF0832ForceResetZero failed: %v", err)
		} else if err := validateRequestedAxes(preState); err != nil {
			logger.Warn("Pre-force reset validation not ready (%v), fallback to serial reset", err)
			if serr := runSerialReset(); serr != nil {
				return &pb.Status{Success: false, Message: serr.Error()}, nil
			}
			goto POST_CHECK
		}
		if err := s.halLens.AF0832ForceResetZero(); err != nil {
			logger.Warn("AF0832ForceResetZero failed, fallback to serial reset: %v", err)
			if serr := runSerialReset(); serr != nil {
				return &pb.Status{Success: false, Message: serr.Error()}, nil
			}
		}
	} else {
		if err := runSerialReset(); err != nil {
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
	}

POST_CHECK:
	state, err := s.halLens.StateGet()
	if err != nil {
		return &pb.Status{Success: false, Message: "post reset-zero state_get: " + err.Error()}, nil
	}
	if err := validateRequestedAxes(state); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	s.invalidateAutofocusAnchor("lens reset-zero")
	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) ControlIris(ctx context.Context, req *pb.IrisRequest) (*pb.Status, error) {
	logger.Debug("ControlIris: speed=%d", req.Speed)

	if !s.config.Capabilities.Lens.Iris {
		return &pb.Status{Success: false, Message: "Iris not supported"}, nil
	}
	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}
	if err := s.ensureLensBootstrapped(); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	if req.Speed == 0 {
		if err := s.halLens.IrisStop(); err != nil {
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		return &pb.Status{Success: true}, nil
	}

	pps := hal.SpeedToPPS(req.Speed)
	steps := hal.SpeedToSteps(req.Speed)
	if err := s.retryWithRecover("iris_run", func() error {
		return s.halLens.IrisRun(pps, steps)
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) SetIrisTarget(ctx context.Context, req *pb.IrisTargetRequest) (*pb.Status, error) {
	logger.Debug("SetIrisTarget: target=%d", req.Target)

	if !s.config.Capabilities.Lens.Iris {
		return &pb.Status{Success: false, Message: "Iris not supported"}, nil
	}
	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}
	if err := s.ensureLensBootstrapped(); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	if err := s.retryWithRecover("iris_target_set", func() error {
		return s.halLens.IrisTargetSet(uint16(req.Target))
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) SetLensLimits(ctx context.Context, req *pb.LensLimitsRequest) (*pb.Status, error) {
	logger.Info("SetLensLimits")

	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}

	if req.ZoomLimit != nil {
		if err := s.halLens.ZoomLimitSet(req.ZoomLimit.MinPos, req.ZoomLimit.MaxPos); err != nil {
			return &pb.Status{Success: false, Message: "zoom limit: " + err.Error()}, nil
		}
	}
	if req.FocusLimit != nil {
		if err := s.halLens.FocusLimitSet(req.FocusLimit.MinPos, req.FocusLimit.MaxPos); err != nil {
			return &pb.Status{Success: false, Message: "focus limit: " + err.Error()}, nil
		}
	}

	s.invalidateAutofocusAnchor("lens limits changed")
	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) LensInit(ctx context.Context, req *pb.LensInitRequest) (*pb.Status, error) {
	logger.Info("LensInit: AF0832 bootstrap")

	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}

	if s.lensIsFg2009() {
		// Full open-loop sequence: profile push, hard-stop anchor, park.
		// There is no AF0832 bootstrap for this lens.
		if err := s.halLens.Init(); err != nil {
			return &pb.Status{Success: false, Message: "lens init failed: " + err.Error()}, nil
		}
		logger.Info("LensInit: fg2009 profile + bootstrap completed")
		return &pb.Status{Success: true}, nil
	}

	// Full bootstrap: init + af0832 create + reset-zero via C event polling
	if err := s.halLens.AF0832Bootstrap(); err != nil {
		return &pb.Status{Success: false, Message: "lens init failed: " + err.Error()}, nil
	}

	logger.Info("LensInit: AF0832 bootstrap completed successfully")
	s.invalidateAutofocusAnchor("lens initialized")
	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) LensGotoRatioDistance(ctx context.Context, req *pb.GotoRatioDistanceRequest) (*pb.Status, error) {
	logger.Info("LensGotoRatioDistance: zoom_ratio=%.2f focus_distance_m=%.2f", req.ZoomRatio, req.FocusDistanceM)

	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}

	// Stop all motors first
	if err := s.halLens.StopAndWaitAll(2 * time.Second); err != nil {
		return &pb.Status{Success: false, Message: "stop motors before goto: " + err.Error()}, nil
	}
	if err := s.ensureLensBootstrapped(); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	if _, err := s.ensureZoomReady(); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	if _, err := s.ensureFocusReady(); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	if err := s.halLens.AF0832GotoRatioDistance(req.ZoomRatio, req.FocusDistanceM); err != nil {
		if isRecoverable(err) {
			logger.Warn("LensGotoRatioDistance transient error, recovering and retrying once: %v", err)
			if re := s.recoverLensLink(); re != nil {
				return &pb.Status{Success: false, Message: "goto recover failed: " + re.Error()}, nil
			}
			if err2 := s.halLens.AF0832GotoRatioDistance(req.ZoomRatio, req.FocusDistanceM); err2 != nil {
				logger.Warn("LensGotoRatioDistance retry still failed, fallback to abs move: %v", err2)
				if re2 := s.recoverLensLink(); re2 != nil {
					logger.Warn("LensGotoRatioDistance fallback recover failed (best-effort): %v", re2)
					return &pb.Status{Success: true, Message: "goto best-effort: fallback recover failed"}, nil
				}
				if fbErr := s.fallbackGotoByAbs(req.ZoomRatio, req.FocusDistanceM); fbErr != nil {
					logger.Warn("LensGotoRatioDistance fallback abs failed (best-effort): %v", fbErr)
					return &pb.Status{Success: true, Message: "goto best-effort: fallback abs failed"}, nil
				}
			}
		} else {
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
	}

	s.invalidateAutofocusAnchor("absolute zoom-focus goto")
	return &pb.Status{Success: true}, nil
}

// LensGotoZoomRatio moves the FG2009 open-loop zoom axis to an optical
// ratio target. The ratio→steps conversion and the virtual-absolute move
// (relative step delta against the dead-reckoned model) live in the lens
// HAL, so no AF0832 step limits are consulted here.
func (s *DeviceControlServer) LensGotoZoomRatio(ctx context.Context, req *pb.ZoomRatioRequest) (*pb.Status, error) {
	logger.Info("LensGotoZoomRatio: zoom_ratio=%.4f", req.ZoomRatio)

	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}
	if !s.lensIsFg2009() {
		return &pb.Status{Success: false, Message: "LensGotoZoomRatio requires lens fg2009"}, nil
	}
	if req.ZoomRatio < 1.0 {
		return &pb.Status{Success: false, Message: "zoom_ratio must be >= 1.0"}, nil
	}
	s.lensStatusMu.RLock()
	maxRatio := s.lensMaxZoomRatio
	s.lensStatusMu.RUnlock()
	if maxRatio > 1 && req.ZoomRatio > maxRatio {
		return &pb.Status{Success: false, Message: fmt.Sprintf("zoom_ratio must be <= %.4f", maxRatio)}, nil
	}

	// Stop all axes before the move to avoid MCU BUSY rejection.
	if err := s.halLens.StopAndWaitAll(2 * time.Second); err != nil {
		return &pb.Status{Success: false, Message: "stop motors before goto: " + err.Error()}, nil
	}
	if err := s.ensureLensBootstrapped(); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	if err := s.retryWithRecover("zoom_goto_ratio", func() error {
		return s.halLens.ZoomGotoRatio(req.ZoomRatio, 0)
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	if err := s.retryWithRecover("wait_zoom_stopped", func() error {
		return s.halLens.WaitZoomStopped(15 * time.Second)
	}); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}

	s.publishEvent("lens_zoom", map[string]interface{}{"zoom_ratio": req.ZoomRatio})
	s.invalidateAutofocusAnchor("absolute zoom movement")
	return &pb.Status{Success: true}, nil
}

// ── AF window / measurement ──────────────────────────────────────────────────

func (s *DeviceControlServer) SetAfWindows(ctx context.Context, req *pb.SetAfWindowsRequest) (*pb.Status, error) {
	logger.Info("SetAfWindows: enabled=%v windows=%d", req.Enabled, len(req.Windows))

	if s.halLens == nil {
		return &pb.Status{Success: false, Message: "Lens HAL not initialized"}, nil
	}

	config := hal.AfWindowsConfig{
		Enabled:     req.Enabled,
		WindowCount: len(req.Windows),
	}
	for i, w := range req.Windows {
		if i >= 3 {
			break
		}
		config.Windows[i] = hal.AfWindow{X: w.X, Y: w.Y, W: w.W, H: w.H}
	}

	if err := s.halLens.SetAfWindows(config); err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	return &pb.Status{Success: true}, nil
}

func (s *DeviceControlServer) GetAfMeasurement(ctx context.Context, req *pb.Empty) (*pb.AfMeasurementResponse, error) {
	if s.halLens == nil {
		return nil, fmt.Errorf("Lens HAL not initialized")
	}

	meas, err := s.halLens.GetAfMeasurement()
	if err != nil {
		return nil, err
	}

	resp := &pb.AfMeasurementResponse{
		FrameId: meas.FrameID,
	}
	for i := 0; i < meas.WindowCount && i < 3; i++ {
		resp.FocusEnergy = append(resp.FocusEnergy, meas.Sum[i])
		resp.MeanLuma = append(resp.MeanLuma, meas.Luma[i])
	}
	return resp, nil
}

// GPIO

func (s *DeviceControlServer) GPIOWrite(ctx context.Context, req *pb.GPIOWriteRequest) (*pb.Status, error) {
	logger.Debug("GPIOWrite: pin=%d, value=%v", req.Pin, req.Value)

	value := uint8(0)
	if req.Value {
		value = 1
	}
	payload := []byte{byte(req.Pin), value}

	if s.cameraDaemonClient != nil {
		resp, err := s.cameraDaemonClient.McuRawRequest(ctx, &camerapb.McuRawRequestMessage{
			Cmd:     0x30, // HOST_LINK_CMD_GPIO_WRITE
			Payload: payload,
		})
		if err != nil {
			logger.Error("GPIOWrite: MCU command failed: %v", err)
			return &pb.Status{Success: false, Message: err.Error()}, nil
		}
		if !resp.Success {
			return &pb.Status{Success: false, Message: resp.Message}, nil
		}
	}

	s.publishEvent("gpio_change", map[string]interface{}{
		"pin":   req.Pin,
		"value": req.Value,
	})

	return &pb.Status{Success: true}, nil
}

// readGPIOOnce performs a single MCU GPIO read (HOST_LINK_CMD_GPIO_READ=0x31).
// Failures are encoded in the returned Status (never returned as a Go error) so
// that one bad pin cannot abort a whole batch read.
func (s *DeviceControlServer) readGPIOOnce(ctx context.Context, pin uint32) *pb.GPIOReadResponse {
	if s.cameraDaemonClient == nil {
		return &pb.GPIOReadResponse{
			Pin:    pin,
			Value:  false,
			Status: &pb.Status{Success: false, Message: "GPIO read not available"},
		}
	}

	resp, err := s.cameraDaemonClient.McuRawRequest(ctx, &camerapb.McuRawRequestMessage{
		Cmd:     0x31, // HOST_LINK_CMD_GPIO_READ
		Payload: []byte{byte(pin)},
	})
	if err != nil {
		logger.Error("GPIO read: MCU command failed for pin %d: %v", pin, err)
		return &pb.GPIOReadResponse{
			Pin:    pin,
			Status: &pb.Status{Success: false, Message: err.Error()},
		}
	}

	if resp.Success && len(resp.Payload) > 0 {
		return &pb.GPIOReadResponse{
			Pin:    pin,
			Value:  resp.Payload[0] != 0,
			Status: &pb.Status{Success: true},
		}
	}

	return &pb.GPIOReadResponse{
		Pin:    pin,
		Value:  false,
		Status: &pb.Status{Success: false, Message: "GPIO read returned no data"},
	}
}

func (s *DeviceControlServer) GPIORead(ctx context.Context, req *pb.GPIOReadRequest) (*pb.GPIOReadResponse, error) {
	logger.Debug("GPIORead: pin=%d", req.Pin)
	return s.readGPIOOnce(ctx, req.Pin), nil
}

// GPIOBatchRead returns the GPIO pin catalog (from config, the single source of
// truth) plus the live value of each requested pin. MCU GPIO read is a single-pin
// command (0x31), so batch = server-side loop; each result carries its own Status.
// Empty req.Pins reads every configured available pin.
func (s *DeviceControlServer) GPIOBatchRead(ctx context.Context, req *pb.GPIOBatchReadRequest) (*pb.GPIOBatchReadResponse, error) {
	gpio := s.config.Capabilities.GPIO

	// Direction lookup from config.
	direction := make(map[uint32]string, len(gpio.InputPins)+len(gpio.OutputPins))
	for _, p := range gpio.InputPins {
		direction[p] = "input"
	}
	for _, p := range gpio.OutputPins {
		direction[p] = "output"
	}

	// Target pins: explicit subset, else all configured available pins.
	target := req.GetPins()
	if len(target) == 0 {
		target = gpio.AvailablePins
	}

	results := make([]*pb.GPIOReadResponse, 0, len(target))
	for _, pin := range target {
		state := s.readGPIOOnce(ctx, pin)
		state.Direction = direction[pin]
		results = append(results, state)
	}

	logger.Debug("GPIOBatchRead: %d pin(s)", len(results))

	return &pb.GPIOBatchReadResponse{
		AvailablePins: gpio.AvailablePins,
		InputPins:     gpio.InputPins,
		OutputPins:    gpio.OutputPins,
		Results:       results,
	}, nil
}

// Status

func (s *DeviceControlServer) GetDeviceStatus(ctx context.Context, req *pb.Empty) (*pb.DeviceStatus, error) {
	logger.Debug("GetDeviceStatus")

	status := &pb.DeviceStatus{
		IrcutMode: pb.IrCutMode_IRCUT_DAY,
	}

	// Fetch real hardware status from camera-daemon
	if s.cameraDaemonClient != nil {
		if hwResp, err := s.cameraDaemonClient.GetDeviceHardwareStatus(ctx, &camerapb.Empty{}); err == nil && hwResp.Success {
			status.LightSensor = hwResp.LightSensorMv
			status.McuTempC = float32(hwResp.McuTempMillic) / 1000.0
			status.WhiteLightLevel = uint32(hwResp.WhiteLightDuty)
			status.IrLedLevel = uint32(hwResp.IrLedDuty)
			status.McuVersion = hwResp.McuVersion
			if hwResp.IrcutMode == 1 {
				status.IrcutMode = pb.IrCutMode_IRCUT_NIGHT
			}
		} else if err != nil {
			logger.Warn("GetDeviceStatus: hardware status query failed: %v", err)
		}
	}

	if s.halLens != nil {
		if state, err := s.halLens.StateGet(); err == nil {
			status.ZoomPos = int32(state.ZoomPos)
			status.FocusPos = int32(state.FocusPos)
		}
	}
	status.AutofocusEnabled = s.autofocusEnabled
	status.SocTempC = readSoCTemp()

	return status, nil
}

// readSoCTemp reads SoC temperature from sysfs thermal zone (millidegree → °C)
func readSoCTemp() float32 {
	for _, path := range []string{"/sys/class/thermal/thermal_zone0/temp", "/sys/class/thermal/thermal_zone1/temp"} {
		data, err := os.ReadFile(path)
		if err != nil {
			continue
		}
		milli, err := strconv.Atoi(strings.TrimSpace(string(data)))
		if err != nil {
			continue
		}
		return float32(milli) / 1000.0
	}
	return 0
}

// Event Stream

func (s *DeviceControlServer) SubscribeEvents(req *pb.Empty, stream pb.DeviceControl_SubscribeEventsServer) error {
	logger.Info("Client subscribed to device events")

	// TODO: Implement event subscription
	// This would monitor MCU for events like GPIO changes, temperature alerts, etc.

	<-stream.Context().Done()
	return stream.Context().Err()
}

func main() {
	flag.Parse()

	// Load configuration
	var cfg Config
	if err := config.LoadYAML(*configPath, &cfg); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to load config: %v\n", err)
		os.Exit(1)
	}

	// Setup logger
	logger.SetLevelFromString(cfg.Service.LogLevel)

	// Configure log file output if specified
	if cfg.Service.LogFile != "" {
		if err := logger.SetOutputFile(cfg.Service.LogFile); err != nil {
			fmt.Fprintf(os.Stderr, "Failed to set log file: %v\n", err)
		} else {
			logger.Info("Logging to file: %s", cfg.Service.LogFile)
		}
	}

	logger.Info("Starting %s", cfg.Service.Name)
	logger.Info("Config file: %s", *configPath)
	logger.Info("Listen address: %s", cfg.Service.Listen)

	// Initialize HAL Lens
	var halLens hal.LensHAL
	if cfg.CameraDaemon.LensEndpoint != "" {
		// gRPC mode: connect to camera-daemon's LensHAL service
		lensCfg := hal.DefaultLensConfig()
		if cfg.Capabilities.Lens.DefaultZoomLimit[0] != 0 || cfg.Capabilities.Lens.DefaultZoomLimit[1] != 0 {
			lensCfg.ZoomLimit = hal.LensLimit{
				MinPos: cfg.Capabilities.Lens.DefaultZoomLimit[0],
				MaxPos: cfg.Capabilities.Lens.DefaultZoomLimit[1],
			}
		}
		if cfg.Capabilities.Lens.DefaultFocusLimit[0] != 0 || cfg.Capabilities.Lens.DefaultFocusLimit[1] != 0 {
			lensCfg.FocusLimit = hal.LensLimit{
				MinPos: cfg.Capabilities.Lens.DefaultFocusLimit[0],
				MaxPos: cfg.Capabilities.Lens.DefaultFocusLimit[1],
			}
		}
		client, err := lens.NewLensClient(cfg.CameraDaemon.LensEndpoint, lensCfg)
		if err != nil {
			logger.Warn("Failed to create lens gRPC client: %v (lens control disabled)", err)
		} else {
			// Connection and remote HAL initialization are reconciled below.  Keep
			// the client even when camera-daemon has not created its socket yet.
			halLens = client
		}
	}

	// Connect to camera-daemon's CameraControl service (for IR-Cut etc.)
	var cameraDaemonClient camerapb.CameraControlClient
	var cameraDaemonConn *grpc.ClientConn
	cameraControlEndpoint := cfg.CameraDaemon.CameraControlEndpoint
	if cameraControlEndpoint == "" && cfg.CameraDaemon.LensEndpoint != "" {
		cameraControlEndpoint = cfg.CameraDaemon.LensEndpoint
	}
	if cameraControlEndpoint != "" {
		camConn, camErr := grpc.NewClient("unix://"+cameraControlEndpoint,
			grpc.WithTransportCredentials(insecure.NewCredentials()))
		if camErr != nil {
			logger.Warn("Failed to create camera-daemon CameraControl client: %v (IR-cut disabled)", camErr)
		} else {
			cameraDaemonConn = camConn
			cameraDaemonClient = camerapb.NewCameraControlClient(camConn)
			logger.Info("CameraControl client created; connection will be maintained for %s", cameraControlEndpoint)
		}
	}

	// Parse listen address (handle unix:// URLs)
	listenAddr, err := utils.ParseListenAddress(cfg.Service.Listen)
	if err != nil {
		logger.Fatal("Failed to parse listen address: %v", err)
	}

	// Create gRPC server
	lis, err := net.Listen("unix", listenAddr)
	if err != nil {
		logger.Fatal("Failed to listen: %v", err)
	}
	defer os.Remove(listenAddr)

	// Set socket permissions for container access
	if err := socket.SetSocketGroupPermission(listenAddr); err != nil {
		logger.Warn("Failed to set socket permissions: %v (containers may not be able to connect)", err)
	} else {
		logger.Info("Socket permissions set for container access: %s", listenAddr)
	}

	grpcServer := grpc.NewServer()
	deviceServer := NewDeviceControlServer(&cfg, halLens, cameraDaemonClient, cameraDaemonConn)
	pb.RegisterDeviceControlServer(grpcServer, deviceServer)
	reconcileCtx, reconcileCancel := context.WithCancel(context.Background())

	// Handle shutdown gracefully
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)

	go func() {
		<-sigChan
		logger.Info("Shutting down...")
		reconcileCancel()
		// Close HAL Lens
		if deviceServer.halLens != nil {
			deviceServer.halLens.Close()
		}
		// Close Event Bus connection
		if deviceServer.eventBusConn != nil {
			deviceServer.eventBusConn.Close()
		}
		if deviceServer.cameraDaemonConn != nil {
			deviceServer.cameraDaemonConn.Close()
		}
		stopped := make(chan struct{})
		go func() {
			grpcServer.GracefulStop()
			close(stopped)
		}()
		select {
		case <-stopped:
			logger.Info("Graceful stop completed")
		case <-time.After(3 * time.Second):
			logger.Info("Graceful stop timed out, force stopping...")
			grpcServer.Stop()
		}
	}()

	logger.Info("Device Control started successfully")

	if client, ok := halLens.(*lens.LensClient); ok {
		go reconcileLens(reconcileCtx, deviceServer, client)
	}
	if cameraDaemonConn != nil {
		go monitorCameraControlConnection(reconcileCtx, cameraDaemonConn)
	}

	if err := grpcServer.Serve(lis); err != nil {
		logger.Fatal("Failed to serve: %v", err)
	}
}

func monitorCameraControlConnection(ctx context.Context, conn *grpc.ClientConn) {
	conn.Connect()
	lastState := connectivity.Idle
	for ctx.Err() == nil {
		state := conn.GetState()
		if state != lastState {
			if state == connectivity.Ready {
				logger.Info("CameraControl connection ready")
			} else {
				logger.Warn("CameraControl connection state: %s", state)
			}
			lastState = state
		}
		if !conn.WaitForStateChange(ctx, state) {
			return
		}
	}
}

// reconcileLens waits for each camera-daemon connection generation and applies
// the complete remote lens initialization exactly once.  A camera-daemon
// restart drives the ClientConn out of READY, which arms the sequence again.
func reconcileLens(ctx context.Context, s *DeviceControlServer, client *lens.LensClient) {
	conn := client.Conn()
	backoff := 500 * time.Millisecond
	initialized := false
	conn.Connect()

	for ctx.Err() == nil {
		state := conn.GetState()
		if state != connectivity.Ready {
			initialized = false
			if !conn.WaitForStateChange(ctx, state) {
				return
			}
			continue
		}

		if !initialized {
			if err := initializeRemoteLens(s, client); err != nil {
				logger.Warn("Lens initialization pending: %v (retry in %s)", err, backoff)
				select {
				case <-ctx.Done():
					return
				case <-time.After(backoff):
				}
				if backoff < 10*time.Second {
					backoff *= 2
					if backoff > 10*time.Second {
						backoff = 10 * time.Second
					}
				}
				continue
			}
			initialized = true
			backoff = 500 * time.Millisecond
		}

		// Block without polling until the transport leaves READY.  Per-RPC
		// failures remain visible to callers and the next state transition will
		// cause the full initialization sequence to be replayed.
		if !conn.WaitForStateChange(ctx, connectivity.Ready) {
			return
		}
	}
}

func initializeRemoteLens(s *DeviceControlServer, client *lens.LensClient) error {
	if err := client.Init(); err != nil {
		return fmt.Errorf("remote Init: %w", err)
	}

	// Learn the factory-fitted lens. ProfileGet is additive: an older
	// camera-daemon fails the RPC and we fall through to the AF0832 path.
	if profile, err := client.ProfileGet(); err == nil {
		s.lensStatusMu.Lock()
		s.lensModel = profile.Model
		s.lensMaxZoomRatio = profile.MaxZoomRatio
		s.lensZoomTravelSteps = profile.ZoomTravelSteps
		s.lensFocusTravelSteps = profile.FocusTravelSteps
		s.lensStatusMu.Unlock()
		if profile.Model == "fg2009" {
			// Init already pushed the MCU profile, anchored the open-loop
			// model against the hard stops and parked it. No AF0832
			// bootstrap and no startup goto — the lens stays at park.
			client.ReplayPersistedConfig()
			logger.Info("Lens HAL initialized via camera-daemon (fg2009 open-loop, max zoom ratio %.4f)",
				profile.MaxZoomRatio)
			return nil
		}
	} else {
		logger.Warn("Lens ProfileGet failed (%v); assuming af0832", err)
	}

	if err := client.AF0832Bootstrap(); err != nil {
		return fmt.Errorf("AF0832 bootstrap: %w", err)
	}
	if err := client.AF0832MarkBootstrapped(); err != nil {
		return fmt.Errorf("mark AF0832 bootstrapped: %w", err)
	}

	focusDistanceM := s.lensAutofocusDistanceM()
	logger.Info("Lens ready: moving to 1.0x zoom, %.2fm focus", focusDistanceM)
	if err := s.fallbackGotoByAbs(1.0, focusDistanceM); err != nil {
		logger.Warn("Startup goto 1.0x failed: %v (non-fatal)", err)
	}

	// Re-apply persisted runtime overrides (zoom/focus limits, iris target)
	// that were lost across a camera-daemon/device-control restart. Guarded:
	// only pushes when the side-file differs from the current HAL state, so a
	// clean boot with no overrides issues no extra HAL traffic.
	client.ReplayPersistedConfig()

	logger.Info("Lens HAL initialized via camera-daemon")
	return nil
}

// ==================== Environment Control ====================

func (s *DeviceControlServer) SetFan(ctx context.Context, req *pb.EnvCtrlRequest) (*pb.EnvCtrlStatus, error) {
	if s.cameraDaemonClient == nil {
		return &pb.EnvCtrlStatus{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.SetFan(ctx, &camerapb.EnvCtrlRequest{Enable: req.Enable})
	if err != nil {
		return &pb.EnvCtrlStatus{Success: false, Message: err.Error()}, nil
	}
	return &pb.EnvCtrlStatus{Success: resp.Success, Message: resp.Message, Enabled: resp.Enabled}, nil
}

func (s *DeviceControlServer) GetFan(ctx context.Context, _ *pb.Empty) (*pb.EnvCtrlStatus, error) {
	if s.cameraDaemonClient == nil {
		return &pb.EnvCtrlStatus{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.GetFan(ctx, &camerapb.Empty{})
	if err != nil {
		return &pb.EnvCtrlStatus{Success: false, Message: err.Error()}, nil
	}
	return &pb.EnvCtrlStatus{Success: resp.Success, Message: resp.Message, Enabled: resp.Enabled}, nil
}

func (s *DeviceControlServer) SetHeat(ctx context.Context, req *pb.EnvCtrlRequest) (*pb.EnvCtrlStatus, error) {
	if s.cameraDaemonClient == nil {
		return &pb.EnvCtrlStatus{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.SetHeat(ctx, &camerapb.EnvCtrlRequest{Enable: req.Enable})
	if err != nil {
		return &pb.EnvCtrlStatus{Success: false, Message: err.Error()}, nil
	}
	return &pb.EnvCtrlStatus{Success: resp.Success, Message: resp.Message, Enabled: resp.Enabled}, nil
}

func (s *DeviceControlServer) GetHeat(ctx context.Context, _ *pb.Empty) (*pb.EnvCtrlStatus, error) {
	if s.cameraDaemonClient == nil {
		return &pb.EnvCtrlStatus{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.GetHeat(ctx, &camerapb.Empty{})
	if err != nil {
		return &pb.EnvCtrlStatus{Success: false, Message: err.Error()}, nil
	}
	return &pb.EnvCtrlStatus{Success: resp.Success, Message: resp.Message, Enabled: resp.Enabled}, nil
}

func (s *DeviceControlServer) SetRadar(ctx context.Context, req *pb.EnvCtrlRequest) (*pb.EnvCtrlStatus, error) {
	if s.cameraDaemonClient == nil {
		return &pb.EnvCtrlStatus{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.SetRadar(ctx, &camerapb.EnvCtrlRequest{Enable: req.Enable})
	if err != nil {
		return &pb.EnvCtrlStatus{Success: false, Message: err.Error()}, nil
	}
	return &pb.EnvCtrlStatus{Success: resp.Success, Message: resp.Message, Enabled: resp.Enabled}, nil
}

func (s *DeviceControlServer) GetRadar(ctx context.Context, _ *pb.Empty) (*pb.EnvCtrlStatus, error) {
	if s.cameraDaemonClient == nil {
		return &pb.EnvCtrlStatus{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.GetRadar(ctx, &camerapb.Empty{})
	if err != nil {
		return &pb.EnvCtrlStatus{Success: false, Message: err.Error()}, nil
	}
	return &pb.EnvCtrlStatus{Success: resp.Success, Message: resp.Message, Enabled: resp.Enabled}, nil
}

// ==================== Alarm I/O ====================

func (s *DeviceControlServer) SetAlarmOut(ctx context.Context, req *pb.AlarmChannelRequest) (*pb.AlarmChannelStatus, error) {
	if s.cameraDaemonClient == nil {
		return &pb.AlarmChannelStatus{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.SetAlarmOut(ctx, &camerapb.AlarmOutRequest{Channel: req.Channel, Enable: req.Enable})
	if err != nil {
		return &pb.AlarmChannelStatus{Success: false, Message: err.Error()}, nil
	}
	return &pb.AlarmChannelStatus{Success: resp.Success, Message: resp.Message, Enabled: resp.Enabled}, nil
}

func (s *DeviceControlServer) GetAlarmOut(ctx context.Context, req *pb.AlarmChannelRequest) (*pb.AlarmChannelStatus, error) {
	if s.cameraDaemonClient == nil {
		return &pb.AlarmChannelStatus{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.GetAlarmOut(ctx, &camerapb.AlarmOutRequest{Channel: req.Channel})
	if err != nil {
		return &pb.AlarmChannelStatus{Success: false, Message: err.Error()}, nil
	}
	return &pb.AlarmChannelStatus{Success: resp.Success, Message: resp.Message, Enabled: resp.Enabled}, nil
}

func (s *DeviceControlServer) SetWiegandOut(ctx context.Context, req *pb.AlarmChannelRequest) (*pb.AlarmChannelStatus, error) {
	if s.cameraDaemonClient == nil {
		return &pb.AlarmChannelStatus{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.SetWiegandOut(ctx, &camerapb.WiegandOutRequest{Channel: req.Channel, Enable: req.Enable})
	if err != nil {
		return &pb.AlarmChannelStatus{Success: false, Message: err.Error()}, nil
	}
	return &pb.AlarmChannelStatus{Success: resp.Success, Message: resp.Message, Enabled: resp.Enabled}, nil
}

func (s *DeviceControlServer) GetWiegandOut(ctx context.Context, req *pb.AlarmChannelRequest) (*pb.AlarmChannelStatus, error) {
	if s.cameraDaemonClient == nil {
		return &pb.AlarmChannelStatus{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.GetWiegandOut(ctx, &camerapb.WiegandOutRequest{Channel: req.Channel})
	if err != nil {
		return &pb.AlarmChannelStatus{Success: false, Message: err.Error()}, nil
	}
	return &pb.AlarmChannelStatus{Success: resp.Success, Message: resp.Message, Enabled: resp.Enabled}, nil
}

func (s *DeviceControlServer) GetAlarmOutputs(ctx context.Context, _ *pb.Empty) (*pb.AlarmOutputsState, error) {
	if s.cameraDaemonClient == nil {
		return &pb.AlarmOutputsState{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.GetAlarmOutputs(ctx, &camerapb.Empty{})
	if err != nil {
		return &pb.AlarmOutputsState{Success: false, Message: err.Error()}, nil
	}
	return &pb.AlarmOutputsState{
		Success:   resp.Success,
		Message:   resp.Message,
		AlarmOut0: resp.AlarmOut0,
		AlarmOut1: resp.AlarmOut1,
		Wiegand0:  resp.Wiegand0,
		Wiegand1:  resp.Wiegand1,
	}, nil
}

// ==================== RS485 ====================

func (s *DeviceControlServer) Rs485Init(ctx context.Context, req *pb.Rs485InitRequest) (*pb.Status, error) {
	if s.cameraDaemonClient == nil {
		return &pb.Status{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.Rs485Init(ctx, &camerapb.Rs485InitRequest{Baudrate: req.Baudrate, Config: req.Config})
	if err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	return &pb.Status{Success: resp.Success, Message: resp.Message}, nil
}

func (s *DeviceControlServer) Rs485Deinit(ctx context.Context, _ *pb.Empty) (*pb.Status, error) {
	if s.cameraDaemonClient == nil {
		return &pb.Status{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.Rs485Deinit(ctx, &camerapb.Empty{})
	if err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	return &pb.Status{Success: resp.Success, Message: resp.Message}, nil
}

func (s *DeviceControlServer) Rs485Tx(ctx context.Context, req *pb.Rs485TxRequest) (*pb.Status, error) {
	if s.cameraDaemonClient == nil {
		return &pb.Status{Success: false, Message: "camera-daemon not connected"}, nil
	}
	resp, err := s.cameraDaemonClient.Rs485Tx(ctx, &camerapb.Rs485TxRequest{Data: req.Data})
	if err != nil {
		return &pb.Status{Success: false, Message: err.Error()}, nil
	}
	return &pb.Status{Success: resp.Success, Message: resp.Message}, nil
}

func (s *DeviceControlServer) GetCapabilities(ctx context.Context, req *pb.Empty) (*pb.CapabilitiesResponse, error) {
	if s.cameraDaemonClient == nil {
		return &pb.CapabilitiesResponse{HasVideo: true}, nil
	}
	resp, err := s.cameraDaemonClient.GetCapabilities(ctx, &camerapb.Empty{})
	if err != nil {
		return &pb.CapabilitiesResponse{
			HasVideo: true, HasCodec: true, HasLed: true,
			HasSensor: true, HasMcu: true, HasOsd: true,
		}, nil
	}
	return &pb.CapabilitiesResponse{
		HasVideo:   resp.HasVideo,
		HasCodec:   resp.HasCodec,
		HasLed:     resp.HasLed,
		HasSensor:  resp.HasSensor,
		HasMcu:     resp.HasMcu,
		HasEnvCtrl: resp.HasEnvCtrl,
		HasAlarm:   resp.HasAlarm,
		HasRs485:   resp.HasRs485,
		HasOsd:     resp.HasOsd,
		HasDraw:    resp.HasDraw,
		HasAudio:   resp.HasAudio,
	}, nil
}
