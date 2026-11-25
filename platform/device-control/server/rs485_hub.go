package main

// rs485_hub.go — push-based fan-out for RS-485 RX frames.
//
// One upstream server-streaming RPC to camera-daemon (SubscribeRs485Rx) is
// shared by every SDK subscriber: the pump goroutine (started lazily on the
// first subscriber, alive for the process lifetime) drains it and fans out
// to per-subscriber channels honoring each subscriber's queue_size/drop_old
// policy. When the last subscriber leaves, the upstream stream is cancelled
// and the pump parks until a new subscriber arrives. Upstream errors (e.g.
// camera-daemon restart) reconnect with exponential backoff.

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

	// Tunable reconnect timing (fields so tests can shorten them).
	backoffBase time.Duration
	backoffMax  time.Duration
	stableReset time.Duration

	mu           sync.Mutex
	subs         map[*rs485Sub]struct{}
	started      bool               // Start() called with a lifecycle context
	running      bool               // pump goroutine exists
	parent       context.Context    // lifecycle context (shutdown cancels it)
	cancelStream context.CancelFunc // cancels the current upstream stream; nil while parked
	restart      chan struct{}      // buffered 1: wakes a parked pump
}

func newRs485Hub(client camerapb.CameraControlClient) *rs485Hub {
	return &rs485Hub{
		client:      client,
		backoffBase: 500 * time.Millisecond,
		backoffMax:  30 * time.Second,
		stableReset: 10 * time.Second,
		restart:     make(chan struct{}, 1),
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
	if queueSize == 0 {
		queueSize = rs485DefaultQueueSize
	}
	if queueSize > rs485MaxQueueSize {
		queueSize = rs485MaxQueueSize
	}

	sub := &rs485Sub{ch: make(chan *pb.Rs485RxEvent, queueSize), dropOld: dropOld}
	if h.subs == nil {
		h.subs = make(map[*rs485Sub]struct{})
	}
	h.subs[sub] = struct{}{}

	parent := h.parent
	if !h.running {
		h.running = true
		go h.pump(parent)
	} else {
		h.signalRestart()
	}
	return sub, nil
}

func (h *rs485Hub) unsubscribe(sub *rs485Sub) {
	var cancel context.CancelFunc
	h.mu.Lock()
	delete(h.subs, sub)
	if len(h.subs) == 0 {
		cancel = h.cancelStream // tear the upstream down; pump parks
	}
	h.mu.Unlock()
	if cancel != nil {
		cancel()
	}
	h.signalRestart()
}

func (h *rs485Hub) signalRestart() {
	select {
	case h.restart <- struct{}{}:
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
	backoff := h.backoffBase
	for {
		if parent.Err() != nil {
			h.teardown()
			return
		}
		if h.subscriberCount() == 0 {
			h.park(parent)
			continue
		}

		sctx, cancel := context.WithCancel(parent)
		h.setStreamCancel(cancel)
		stream, err := h.client.SubscribeRs485Rx(sctx, &camerapb.Empty{})
		if err != nil {
			cancel()
			h.setStreamCancel(nil)
			logger.Warn("rs485: upstream subscribe failed: %v (retry in %v)", err, backoff)
			if !rs485Sleep(parent, backoff) {
				h.teardown()
				return
			}
			backoff = rs485GrowBackoff(backoff, h.backoffBase, h.backoffMax)
			continue
		}
		logger.Info("rs485: upstream stream opened")

		var lastHealthy time.Time
		for {
			ev, err := stream.Recv()
			if err != nil {
				logger.Debug("rs485: upstream stream ended: %v", err)
				break
			}
			lastHealthy = time.Now()
			// camera.proto and device.proto declare independent
			// Rs485RxEvent messages (same shape) — convert across the hop.
			h.fanout(&pb.Rs485RxEvent{Data: ev.Data, TimestampNs: ev.TimestampNs})
		}
		cancel()
		h.setStreamCancel(nil)

		if parent.Err() != nil {
			h.teardown()
			return
		}
		if time.Since(lastHealthy) >= h.stableReset {
			backoff = h.backoffBase
		}
		if !rs485Sleep(parent, backoff) {
			h.teardown()
			return
		}
		backoff = rs485GrowBackoff(backoff, h.backoffBase, h.backoffMax)
	}
}

// park blocks (running stays true — exactly one pump exists for life) until
// a new subscriber signals restart or the lifecycle context ends.
func (h *rs485Hub) park(parent context.Context) {
	h.mu.Lock()
	h.cancelStream = nil
	h.mu.Unlock()
	select {
	case <-h.restart:
	case <-parent.Done():
	}
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

func rs485GrowBackoff(cur, base, max time.Duration) time.Duration {
	next := cur * 2
	if next > max {
		next = max
	}
	if next < base {
		next = base
	}
	return next
}

func rs485Sleep(ctx context.Context, d time.Duration) bool {
	t := time.NewTimer(d)
	defer t.Stop()
	select {
	case <-ctx.Done():
		return false
	case <-t.C:
		return true
	}
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
