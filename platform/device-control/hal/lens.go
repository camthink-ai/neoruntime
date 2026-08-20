package hal

import (
	"errors"
	"time"
)

// LensHAL is the interface for lens hardware control.
// Implemented by *lens.LensClient (gRPC).
type LensHAL interface {
	Init() error
	ReInit() error
	Close()

	StateGet() (LensState, error)
	StateGetTry() (LensState, error)
	IrisAdcGet() (uint16, error)
	IrisAdcGetTry() (uint16, error)

	ZoomRun(pps uint16, steps int32) error
	ZoomAbs(pps uint16, position int32) error
	ZoomStop() error
	ZoomResetZero() error
	ZoomLimitSet(min, max int32) error
	ZoomLimits() LensLimit
	WaitZoomStopped(timeout time.Duration) error
	WaitZoomRzDone(timeout time.Duration) error

	FocusRun(pps uint16, steps int32) error
	FocusAbs(pps uint16, position int32) error
	FocusStop() error
	FocusResetZero() error
	FocusLimitSet(min, max int32) error
	FocusLimits() LensLimit
	WaitFocusStopped(timeout time.Duration) error
	WaitFocusRzDone(timeout time.Duration) error

	IrisRun(pps uint16, steps int32) error
	IrisStop() error
	IrisTargetSet(target uint16) error

	StopAndWaitAll(timeout time.Duration) error

	AF0832Bootstrap() error
	AF0832MarkBootstrapped() error
	AF0832ForceResetZero() error
	AF0832GotoRatioDistance(zoomRatio, focusDistanceM float32) error
	AF0832PosToRatio(halZoomPos int32) float32
	IsAF0832Bootstrapped() bool

	// Lens profile & FG2009 open-loop helpers
	ProfileGet() (LensProfile, error)
	ZoomGotoRatio(zoomRatio float32, pps uint16) error
	FocusGotoLevel(level float32, pps uint16) error
	ZoomMoveRel(pps uint16, steps int32) error
	FocusMoveRel(pps uint16, steps int32) error

	// AF window / measurement (ISP-level)
	SetAfWindows(config AfWindowsConfig) error
	GetAfMeasurement() (*AfMeasurement, error)
}

// AfWindow represents one AF measurement window.
type AfWindow struct {
	X, Y, W, H int32
}

// AfWindowsConfig maps to HalIspAfWindowsConfig.
type AfWindowsConfig struct {
	Enabled     bool
	WindowCount int
	Windows     [3]AfWindow
}

// AfMeasurement maps to HalIspAfMeasurement.
type AfMeasurement struct {
	WindowCount int
	FrameID     uint64
	TimestampNS uint64
	Sum         [3]uint32
	Luma        [3]uint32
}

// LensState mirrors HalIOLensState from hal_io.h.
type LensState struct {
	IrisState   uint8
	ZoomState   uint8
	FocusState  uint8
	ZoomRzDone  bool
	FocusRzDone bool
	ZoomPos     int32
	FocusPos    int32
}

// LensLimit mirrors HalIOLensLimit from hal_io.h.
type LensLimit struct {
	MinPos int32
	MaxPos int32
}

// LensProfile describes the factory-fitted lens (from the lens HAL service).
// Model is "af0832" or "fg2009".
type LensProfile struct {
	Model            string
	Relative         bool
	Ircut            bool
	ZoomTravelSteps  int32
	FocusTravelSteps int32
	MaxZoomRatio     float32
}

// LensConfig holds HAL lens configuration.
type LensConfig struct {
	SerialDevice string
	BaudRate     uint32
	TimeoutMs    uint32

	ZoomLimit  LensLimit
	FocusLimit LensLimit
}

// DefaultLensConfig returns sensible defaults for AF0832-D09 lens.
func DefaultLensConfig() LensConfig {
	return LensConfig{
		SerialDevice: "/dev/ttyS0",
		BaudRate:     921600,
		TimeoutMs:    1000,
		ZoomLimit:    LensLimit{MinPos: -3236, MaxPos: 760},
		FocusLimit:   LensLimit{MinPos: -844, MaxPos: 592},
	}
}

// lensStateC mirrors the C struct layout for HalIOLensState (24 bytes).
// uint8 (1) + uint8 (1) + uint8 (1) + padding(1) + bool(1) + bool(1) + padding(2) + int32(4) + int32(4)
// Total: 16 bytes on most ABIs but may vary. We use a larger buffer and interpret.
type lensStateC struct {
	irisState   uint8
	zoomState   uint8
	focusState  uint8
	_           uint8  // padding
	zoomRzDone  uint8  // bool in C = 1 byte
	focusRzDone uint8  // bool in C = 1 byte
	_           uint16 // padding
	zoomPos     int32
	focusPos    int32
}

// Motor state constants matching ms41908m_state_t.
const (
	MotorStateNoCfg     = 0
	MotorStateStopped   = 1
	MotorStateRunning   = 2
	MotorStateResetZero = 3
	MotorStateError     = 4
)

var ErrLensBusy = errors.New("lens operation in progress")

func SpeedToPPS(speed int32) uint16 {
	if speed == 0 {
		return 0
	}
	abs := speed
	if abs < 0 {
		abs = -abs
	}
	pps := 100 + abs*19
	if pps > 2000 {
		pps = 2000
	}
	return uint16(pps)
}

func SpeedToSteps(speed int32) int32 {
	if speed > 0 {
		return 200
	}
	if speed < 0 {
		return -200
	}
	return 0
}

func LevelToPosition(level float32, lim LensLimit) int32 {
	if level <= 0 {
		return lim.MinPos
	}
	if level >= 1 {
		return lim.MaxPos
	}
	return lim.MinPos + int32(float32(lim.MaxPos-lim.MinPos)*level)
}
