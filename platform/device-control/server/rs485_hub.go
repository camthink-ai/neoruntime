package main

// rs485_hub.go — push-based fan-out for RS-485 RX frames.
//
// One upstream server-streaming RPC to camera-daemon (SubscribeRs485Rx) is
// shared by every SDK subscriber: the pump goroutine (started lazily on the
// first subscriber, alive for the process lifetime) drains it and fans out
// to per-subscriber channels honoring each subscriber's queue_size/drop_old
// policy. When the last subscriber leaves the upstream stream is cancelled
// and the pump parks until a new subscriber arrives.
//
// While no upstream is open, bus frames are dropped before they reach any
// subscriber policy. A lost upstream (e.g. a camera-daemon restart) is
// therefore reopened on a short fixed cadence, and a subscriber arriving
// during the wait interrupts it: never a backoff the subscriber has to sit
// out. camera-daemon is a local UDS peer, so a failed reopen costs one dial.

import (
	"context"
	"sync"
	"time"

	camerapb "aipc/platform/camera-daemon/proto"
	"aipc/platform/common/logger"
	pb "aipc/platform/device-control/proto"

	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
)

const (
	rs485DefaultQueueSize = uint32(16)
	rs485MaxQueueSize     = uint32(128)

	// rs485RetryInterval is the reopen cadence after a transient upstream
	// loss (stream broken, daemon unreachable).
	rs485RetryInterval = time.Second

	// rs485SlowRetryInterval backs the reopen off when the upstream rejects
	// the stream outright: no MCU link on this unit, or a camera-daemon
	// without the RPC. Retrying still picks up a later upgrade or HAL load,
	// but not at a warning per second.
	rs485SlowRetryInterval = 10 * time.Second
)

// rs485Sub is one SDK-facing subscription.
type rs485Sub struct {
	ch      chan *pb.Rs485RxEvent
	dropOld bool

	mu      sync.Mutex
	dropped uint64
}

func (s *rs485Sub) recordDrop() {
	s.mu.Lock()
	s.dropped++
	s.mu.Unlock()
}

func (s *rs485Sub) droppedCount() uint64 {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.dropped
}

type rs485Hub struct {
	client camerapb.CameraControlClient

	// Reopen cadences (fields so tests can change them).
	retryInterval     time.Duration
	slowRetryInterval time.Duration

	mu           sync.Mutex
	subs         map[*rs485Sub]struct{}
	started      bool               // Start() called with a lifecycle context
	running      bool               // pump goroutine exists
	parent       context.Context    // lifecycle context (shutdown cancels it)
	cancelStream context.CancelFunc // cancels the current upstream stream; nil while parked or retrying
	lastLoss     string             // last upstream error already reported at WARN

	wake chan struct{} // buffered 1: the subscriber set changed
}

func newRs485Hub(client camerapb.CameraControlClient) *rs485Hub {
	return &rs485Hub{
		client:            client,
		retryInterval:     rs485RetryInterval,
		slowRetryInterval: rs485SlowRetryInterval,
		subs:              make(map[*rs485Sub]struct{}),
		wake:              make(chan struct{}, 1),
	}
}

// Start records the lifecycle context (mirrors StartEventPoller). The pump
// itself starts lazily on the first subscriber.
func (h *rs485Hub) Start(ctx context.Context) {
	h.mu.Lock()
	h.parent = ctx
	h.started = true
	h.mu.Unlock()
}

func (h *rs485Hub) subscribe(queueSize uint32, dropOld bool) (*rs485Sub, error) {
	h.mu.Lock()
	defer h.mu.Unlock()

	if !h.started {
		return nil, status.Error(codes.FailedPrecondition, "rs485 hub not started")
	}
	if h.client == nil {
		return nil, status.Error(codes.FailedPrecondition, "camera-daemon not connected")
	}
	if h.parent == nil || h.parent.Err() != nil {
		return nil, status.Error(codes.Unavailable, "rs485 hub is shutting down")
	}
	if queueSize == 0 {
		queueSize = rs485DefaultQueueSize
	}
	if queueSize > rs485MaxQueueSize {
		queueSize = rs485MaxQueueSize
	}

	sub := &rs485Sub{ch: make(chan *pb.Rs485RxEvent, queueSize), dropOld: dropOld}
	h.subs[sub] = struct{}{}

	if !h.running {
		h.running = true
		go h.pump(h.parent)
	} else {
		// A pump parked or mid-retry must not wait out its timer for us.
		h.signalWake()
	}
	return sub, nil
}

func (h *rs485Hub) unsubscribe(sub *rs485Sub) {
	var cancel context.CancelFunc
	h.mu.Lock()
	delete(h.subs, sub)
	// Tear the upstream down only when this really was the last subscriber.
	// Decided under the lock subscribe() takes, so a subscriber arriving
	// concurrently either precedes the delete (the stream stays up) or
	// follows it (its wake reopens the stream at once).
	last := len(h.subs) == 0
	if last && h.cancelStream != nil {
		cancel, h.cancelStream = h.cancelStream, nil
	}
	h.mu.Unlock()

	if cancel != nil {
		cancel()
	}
	if last {
		// Wakes a pump that is mid-retry so it notices nobody is left.
		h.signalWake()
	}
}

func (h *rs485Hub) signalWake() {
	select {
	case h.wake <- struct{}{}:
	default:
	}
}

func (h *rs485Hub) subscriberCount() int {
	h.mu.Lock()
	defer h.mu.Unlock()
	return len(h.subs)
}

// fanout never blocks: per-subscriber bounded channel, drop per policy.
func (h *rs485Hub) fanout(ev *pb.Rs485RxEvent) {
	h.mu.Lock()
	subs := make([]*rs485Sub, 0, len(h.subs))
	for s := range h.subs {
		subs = append(subs, s)
	}
	h.mu.Unlock()

	for _, s := range subs {
		select {
		case s.ch <- ev:
		default:
			if s.dropOld {
				// Make room by discarding the oldest queued frame, then retry.
				select {
				case <-s.ch:
				default:
				}
				select {
				case s.ch <- ev:
				default:
				}
			}
			// drop_old=false: keep the backlog as-is, drop this frame.
			s.recordDrop()
		}
	}
}

// pump is the single upstream consumer; runs until parent is cancelled.
func (h *rs485Hub) pump(parent context.Context) {
	for {
		if parent.Err() != nil {
			h.teardown()
			return
		}
		if h.subscriberCount() == 0 {
			if !h.park(parent) {
				h.teardown()
				return
			}
			continue
		}

		sctx, cancel := context.WithCancel(parent)
		h.setStreamCancel(cancel)
		err := h.openAndDrain(sctx)
		// Sample why sctx is done BEFORE our own cancel() below makes every
		// stream look deliberately closed.
		closedByUnsubscribe := sctx.Err() != nil
		cancel()
		h.setStreamCancel(nil)

		if parent.Err() != nil {
			h.teardown() // shutting down: the closed stream is expected
			return
		}
		// sctx cancelled by unsubscribe() is a deliberate close, not a loss:
		// either park (nobody left) or reopen at once for a subscriber that
		// joined in the meantime — never a warn, never a retry wait.
		if closedByUnsubscribe || h.subscriberCount() == 0 {
			continue
		}

		// The stream ended on its own while subscribers remain: a real loss.
		cadence := h.retryInterval
		if rs485RejectedOutright(err) {
			cadence = h.slowRetryInterval
		}
		h.reportUpstreamLoss(err, cadence)
		if !h.waitRetry(parent, cadence) {
			h.teardown()
			return
		}
	}
}

// openAndDrain opens the upstream stream and fans out until it ends, then
// returns the reason it ended.
func (h *rs485Hub) openAndDrain(sctx context.Context) error {
	stream, err := h.client.SubscribeRs485Rx(sctx, &camerapb.Empty{})
	if err != nil {
		return err
	}
	// A stream that opened clears the loss state: the next failure of the
	// same kind is news again and deserves its warning.
	h.mu.Lock()
	h.lastLoss = ""
	h.mu.Unlock()
	logger.Info("rs485: upstream stream opened")
	for {
		ev, err := stream.Recv()
		if err != nil {
			return err
		}
		// camera.proto and device.proto declare independent Rs485RxEvent
		// messages (same shape) — convert across the hop.
		h.fanout(&pb.Rs485RxEvent{Data: ev.Data, TimestampNs: ev.TimestampNs})
	}
}

// rs485RejectedOutright reports whether the upstream refused the stream for a
// reason a fast retry cannot fix.
func rs485RejectedOutright(err error) bool {
	st, ok := status.FromError(err)
	if !ok {
		return false
	}
	switch st.Code() {
	case codes.FailedPrecondition, codes.Unimplemented, codes.PermissionDenied:
		return true
	default:
		return false
	}
}

// reportUpstreamLoss warns once per distinct error and then stays at debug,
// so a unit that cannot serve RS-485 does not flood the journal.
func (h *rs485Hub) reportUpstreamLoss(err error, cadence time.Duration) {
	text := err.Error()
	h.mu.Lock()
	repeat := text == h.lastLoss
	h.lastLoss = text
	h.mu.Unlock()

	if repeat {
		logger.Debug("rs485: upstream still unavailable: %v (reopen in %v)", err, cadence)
		return
	}
	logger.Warn("rs485: upstream stream lost: %v (reopen in %v)", err, cadence)
}

// park blocks until the subscriber set changes or the lifecycle context ends.
func (h *rs485Hub) park(parent context.Context) bool {
	for {
		// Discard a token left over from an earlier change, then re-check:
		// a subscriber that signalled between the caller's count check and
		// here must not be lost with it.
		select {
		case <-h.wake:
		default:
		}
		if h.subscriberCount() > 0 {
			return true
		}
		select {
		case <-h.wake:
			return true
		case <-parent.Done():
			return false
		}
	}
}

// waitRetry pauses for d before the next upstream attempt, returning early
// when the subscriber set changes. It returns false once the lifecycle
// context ends.
func (h *rs485Hub) waitRetry(parent context.Context, d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-parent.Done():
		return false
	case <-h.wake:
	case <-t.C:
	}
	return parent.Err() == nil
}

func (h *rs485Hub) setStreamCancel(cancel context.CancelFunc) {
	h.mu.Lock()
	h.cancelStream = cancel
	h.mu.Unlock()
}

func (h *rs485Hub) teardown() {
	h.mu.Lock()
	h.cancelStream = nil
	h.running = false
	h.mu.Unlock()
}

// SubscribeRs485Rx is the DeviceControl RPC: one stream per SDK client,
// backed by the shared upstream.
func (s *DeviceControlServer) SubscribeRs485Rx(req *pb.Rs485SubscribeRequest,
	stream pb.DeviceControl_SubscribeRs485RxServer) error {

	sub, err := s.rs485.subscribe(req.GetQueueSize(), req.GetDropOld())
	if err != nil {
		return err
	}
	defer s.rs485.unsubscribe(sub)
	defer func() {
		// The policy is only as good as its visibility: say when it bit.
		if dropped := sub.droppedCount(); dropped > 0 {
			logger.Warn("SubscribeRs485Rx: client gone after dropping %d frames (queue=%d drop_old=%v)",
				dropped, cap(sub.ch), sub.dropOld)
		}
	}()

	logger.Info("SubscribeRs485Rx: client subscribed (queue=%d drop_old=%v)",
		cap(sub.ch), sub.dropOld)

	ctx := stream.Context()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case ev := <-sub.ch:
			if err := stream.Send(ev); err != nil {
				return err
			}
		}
	}
}
