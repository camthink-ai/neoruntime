package main

// rs485_hub_test.go — covers the device-control RS-485 RX hub: subscribe
// validation/clamping, fan-out drop policies, the single-shared-upstream
// lifecycle (lazy start, park on last unsubscribe, prompt reopen on
// resubscribe and after an upstream loss, slow cadence when the upstream
// rejects the stream), and the SubscribeRs485Rx handler glue, against an
// in-memory fake camera-daemon.

import (
	"context"
	"net"
	"sync"
	"testing"
	"time"

	camerapb "aipc/platform/camera-daemon/proto"
	pb "aipc/platform/device-control/proto"

	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"
)

// ---- validation ------------------------------------------------------------

func TestRs485HubSubscribeValidation(t *testing.T) {
	h := newRs485Hub(nil)
	ctx := context.Background()

	if _, err := h.subscribe(4, true); status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("want FailedPrecondition before Start, got %v", err)
	}
	h.Start(ctx)
	if _, err := h.subscribe(4, true); status.Code(err) != codes.FailedPrecondition {
		t.Fatalf("want FailedPrecondition with nil client, got %v", err)
	}
}

func TestRs485HubClampsQueueSize(t *testing.T) {
	h, _ := newTestHub(t, 0)

	def, err := h.subscribe(0, true)
	if err != nil {
		t.Fatalf("subscribe(0): %v", err)
	}
	big, err := h.subscribe(200, false)
	if err != nil {
		t.Fatalf("subscribe(200): %v", err)
	}
	small, err := h.subscribe(5, true)
	if err != nil {
		t.Fatalf("subscribe(5): %v", err)
	}
	if cap(def.ch) != 16 || cap(big.ch) != 128 || cap(small.ch) != 5 {
		t.Fatalf("queue clamps: want 16/128/5, got %d/%d/%d",
			cap(def.ch), cap(big.ch), cap(small.ch))
	}
	if !def.dropOld || big.dropOld {
		t.Fatalf("drop_old not carried through: %v/%v", def.dropOld, big.dropOld)
	}
}

// ---- fake camera-daemon ----------------------------------------------------

// fakeCameraDaemon stands in for camera-daemon's SubscribeRs485Rx: it pushes
// `burst` frames when a stream opens, then holds the stream until the client
// goes away or the test kills it.
type fakeCameraDaemon struct {
	camerapb.UnimplementedCameraControlServer

	mu     sync.Mutex
	opens  int   // SubscribeRs485Rx calls seen
	closes int   // streams that ended
	burst  int   // frames pushed when a stream opens
	reject error // when set, every stream fails with this error
	kill   chan struct{}
}

func newFakeCameraDaemon(burst int) *fakeCameraDaemon {
	return &fakeCameraDaemon{burst: burst, kill: make(chan struct{})}
}

func (f *fakeCameraDaemon) SubscribeRs485Rx(_ *camerapb.Empty,
	stream camerapb.CameraControl_SubscribeRs485RxServer) error {
	f.mu.Lock()
	f.opens++
	kill, reject, burst := f.kill, f.reject, f.burst
	f.mu.Unlock()
	defer func() {
		f.mu.Lock()
		f.closes++
		f.mu.Unlock()
	}()

	if reject != nil {
		return reject
	}
	for i := 0; i < burst; i++ {
		if err := stream.Send(&camerapb.Rs485RxEvent{
			Data:        []byte{byte('a' + i)},
			TimestampNs: uint64(i + 1),
		}); err != nil {
			return err
		}
	}
	select {
	case <-kill:
	case <-stream.Context().Done():
	}
	return stream.Context().Err()
}

// killStreams ends every open stream the way a camera-daemon restart would.
func (f *fakeCameraDaemon) killStreams() {
	f.mu.Lock()
	close(f.kill)
	f.kill = make(chan struct{})
	f.mu.Unlock()
}

func (f *fakeCameraDaemon) setReject(err error) {
	f.mu.Lock()
	f.reject = err
	f.mu.Unlock()
}

func (f *fakeCameraDaemon) counts() (opens, closes int) {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.opens, f.closes
}

func newFakeCameraDaemonClient(t *testing.T, burst int) (camerapb.CameraControlClient, *fakeCameraDaemon) {
	t.Helper()
	fake := newFakeCameraDaemon(burst)
	lis := bufconn.Listen(1024 * 1024)
	srv := grpc.NewServer()
	camerapb.RegisterCameraControlServer(srv, fake)
	go func() { _ = srv.Serve(lis) }()
	t.Cleanup(srv.Stop)

	conn, err := grpc.Dial("bufnet",
		grpc.WithContextDialer(func(ctx context.Context, _ string) (net.Conn, error) {
			return lis.DialContext(ctx)
		}),
		grpc.WithInsecure(),
	)
	if err != nil {
		t.Fatalf("dial bufnet: %v", err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	return camerapb.NewCameraControlClient(conn), fake
}

// ---- harness ---------------------------------------------------------------

const rs485TestRetry = 10 * time.Millisecond

// newTestHub returns a started hub against a fake upstream, on a lifecycle
// context cancelled when the test ends.
func newTestHub(t *testing.T, burst int) (*rs485Hub, *fakeCameraDaemon) {
	t.Helper()
	client, fake := newFakeCameraDaemonClient(t, burst)
	h := newRs485Hub(client)
	h.retryInterval = rs485TestRetry
	h.slowRetryInterval = 10 * time.Second
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	h.Start(ctx)
	return h, fake
}

// waitFor polls cond until it holds or the (generous) deadline passes.
func waitFor(t *testing.T, what string, cond func() bool) {
	t.Helper()
	deadline := time.Now().Add(3 * time.Second)
	for !cond() {
		if time.Now().After(deadline) {
			t.Fatalf("timed out waiting for %s", what)
		}
		time.Sleep(2 * time.Millisecond)
	}
}

func readN(t *testing.T, s *rs485Sub, n int) []*pb.Rs485RxEvent {
	t.Helper()
	var out []*pb.Rs485RxEvent
	deadline := time.After(3 * time.Second)
	for len(out) < n {
		select {
		case ev := <-s.ch:
			out = append(out, ev)
		case <-deadline:
			t.Fatalf("want %d frames, got %d", n, len(out))
		}
	}
	return out
}

// ---- lifecycle -------------------------------------------------------------

func TestRs485HubSharesOneUpstreamAndParksWhenIdle(t *testing.T) {
	h, fake := newTestHub(t, 3)

	subA, err := h.subscribe(8, true)
	if err != nil {
		t.Fatalf("subscribe A: %v", err)
	}
	subB, err := h.subscribe(8, false)
	if err != nil {
		t.Fatalf("subscribe B: %v", err)
	}

	// Two subscribers share ONE upstream stream and both see its frames.
	got := readN(t, subA, 3)
	if got[0].Data[0] != 'a' || got[2].Data[0] != 'c' || got[2].TimestampNs != 3 {
		t.Fatalf("subscriber A: unexpected payloads: %+v", got)
	}
	if gotB := readN(t, subB, 3); len(gotB) != 3 {
		t.Fatalf("subscriber B: want 3 frames, got %d", len(gotB))
	}
	if opens, _ := fake.counts(); opens != 1 {
		t.Fatalf("want exactly 1 upstream open for 2 subscribers, got %d", opens)
	}

	// Last unsubscribe closes the upstream; an idle hub stays parked.
	h.unsubscribe(subA)
	h.unsubscribe(subB)
	waitFor(t, "upstream closed after the last unsubscribe", func() bool {
		_, closes := fake.counts()
		return closes == 1
	})
	time.Sleep(10 * rs485TestRetry)
	if opens, _ := fake.counts(); opens != 1 {
		t.Fatalf("parked hub reopened its upstream (opens=%d)", opens)
	}

	// A new subscriber reopens it exactly once more.
	subC, err := h.subscribe(8, true)
	if err != nil {
		t.Fatalf("subscribe C: %v", err)
	}
	readN(t, subC, 3)
	if opens, _ := fake.counts(); opens != 2 {
		t.Fatalf("want 2 upstream opens total, got %d", opens)
	}
	h.unsubscribe(subC)
}

func TestRs485HubResubscribeReopensWithoutWaitingOutRetry(t *testing.T) {
	// Regression: a subscriber arriving while the pump is parked or waiting
	// to retry must reopen the upstream at once. Frames arriving before the
	// reopen never reach a subscriber policy, so a missed wake — or a backoff
	// grown by earlier cycles — is a silent blind window. The retry timer is
	// far beyond the deadline, so only the wake can make this pass.
	h, fake := newTestHub(t, 3)
	h.retryInterval = 10 * time.Second

	for cycle := 1; cycle <= 3; cycle++ {
		subA, err := h.subscribe(8, true)
		if err != nil {
			t.Fatalf("cycle %d subscribe A: %v", cycle, err)
		}
		readN(t, subA, 3) // busy stream: frames were just delivered
		h.unsubscribe(subA)
		// Every earlier cycle closed two streams; this cycle's first one
		// must be closed too before the resubscribe.
		wantCloses := 2*(cycle-1) + 1
		waitFor(t, "upstream closed", func() bool {
			_, closes := fake.counts()
			return closes >= wantCloses
		})

		subB, err := h.subscribe(8, true)
		if err != nil {
			t.Fatalf("cycle %d subscribe B: %v", cycle, err)
		}
		// Must not wait out the 10 s retry timer: readN's own deadline is
		// the bound.
		readN(t, subB, 3)
		if opens, _ := fake.counts(); opens != 2*cycle {
			t.Fatalf("cycle %d: want %d upstream opens, got %d", cycle, 2*cycle, opens)
		}
		h.unsubscribe(subB)
	}
}

func TestRs485HubResubscribeOnIdleUpstreamReopensAtOnce(t *testing.T) {
	// Same property for a stream that never carried a frame.
	h, fake := newTestHub(t, 0)
	h.retryInterval = 10 * time.Second

	subA, err := h.subscribe(8, true)
	if err != nil {
		t.Fatalf("subscribe A: %v", err)
	}
	waitFor(t, "first upstream open", func() bool {
		opens, _ := fake.counts()
		return opens == 1
	})
	h.unsubscribe(subA)
	waitFor(t, "upstream closed", func() bool {
		_, closes := fake.counts()
		return closes == 1
	})

	start := time.Now()
	subB, err := h.subscribe(8, true)
	if err != nil {
		t.Fatalf("subscribe B: %v", err)
	}
	waitFor(t, "reopened upstream", func() bool {
		opens, _ := fake.counts()
		return opens == 2
	})
	if elapsed := time.Since(start); elapsed > 2*time.Second {
		t.Fatalf("resubscribe on an idle stream waited %v", elapsed)
	}
	h.unsubscribe(subB)
}

func TestRs485HubReopensAfterUpstreamLoss(t *testing.T) {
	h, fake := newTestHub(t, 3)

	sub, err := h.subscribe(8, true)
	if err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	readN(t, sub, 3)

	// The subscriber stays; only the upstream dies (camera-daemon restart).
	fake.killStreams()
	readN(t, sub, 3) // frames from the reopened stream
	waitFor(t, "upstream reopened", func() bool {
		opens, _ := fake.counts()
		return opens == 2
	})
	h.unsubscribe(sub)
}

func TestRs485HubNewSubscriberInterruptsRetryWait(t *testing.T) {
	// Regression for the non-interruptible backoff, on the path the tests
	// above do not reach: here the pump is INSIDE the retry wait (upstream
	// lost while a subscriber stayed) when the new subscriber arrives. It
	// must abandon the wait rather than let the newcomer sit out the timer.
	h, fake := newTestHub(t, 3)
	h.retryInterval = 10 * time.Second

	subA, err := h.subscribe(8, true)
	if err != nil {
		t.Fatalf("subscribe A: %v", err)
	}
	readN(t, subA, 3)

	fake.killStreams() // upstream lost; A stays subscribed
	waitFor(t, "lost upstream to close", func() bool {
		_, closes := fake.counts()
		return closes >= 1
	})
	time.Sleep(10 * rs485TestRetry) // let the pump settle into the retry wait

	subB, err := h.subscribe(8, true)
	if err != nil {
		t.Fatalf("subscribe B: %v", err)
	}
	readN(t, subB, 3) // frames only exist once the upstream has been reopened
	h.unsubscribe(subA)
	h.unsubscribe(subB)
}

func TestRs485HubBacksOffWhenUpstreamRejectsStream(t *testing.T) {
	// A unit that cannot serve RS-485 (no MCU link) or an old camera-daemon
	// rejects every attempt: keep retrying, but not at the fast cadence and
	// not at a warning per attempt.
	h, fake := newTestHub(t, 0)
	fake.setReject(status.Error(codes.FailedPrecondition, "RS485 HAL not loaded"))

	sub, err := h.subscribe(8, true)
	if err != nil {
		t.Fatalf("subscribe: %v", err)
	}
	waitFor(t, "first rejected attempt", func() bool {
		opens, _ := fake.counts()
		return opens == 1
	})
	time.Sleep(20 * rs485TestRetry) // many fast intervals
	if opens, _ := fake.counts(); opens != 1 {
		t.Fatalf("rejected upstream retried at the fast cadence (opens=%d)", opens)
	}
	h.unsubscribe(sub)
}

// ---- drop policy via fanout -------------------------------------------------

func TestRs485HubFanoutDropPolicies(t *testing.T) {
	h, _ := newTestHub(t, 0)

	oldSub := &rs485Sub{ch: make(chan *pb.Rs485RxEvent, 2), dropOld: true}
	newSub := &rs485Sub{ch: make(chan *pb.Rs485RxEvent, 2), dropOld: false}
	h.mu.Lock()
	h.subs[oldSub] = struct{}{}
	h.subs[newSub] = struct{}{}
	h.mu.Unlock()

	for i := 0; i < 4; i++ {
		h.fanout(&pb.Rs485RxEvent{Data: []byte{byte('0' + i)}})
	}

	// drop_old=true: keeps the two newest ("2","3"), dropped 2.
	if ev := <-oldSub.ch; ev.Data[0] != '2' {
		t.Fatalf("dropOld: want '2' first, got %q", ev.Data)
	}
	if ev := <-oldSub.ch; ev.Data[0] != '3' {
		t.Fatalf("dropOld: want '3' second, got %q", ev.Data)
	}
	if d := oldSub.droppedCount(); d != 2 {
		t.Fatalf("dropOld: want 2 drops, got %d", d)
	}

	// drop_old=false: keeps the two oldest ("0","1"), dropped 2.
	if ev := <-newSub.ch; ev.Data[0] != '0' {
		t.Fatalf("dropNew: want '0' first, got %q", ev.Data)
	}
	if ev := <-newSub.ch; ev.Data[0] != '1' {
		t.Fatalf("dropNew: want '1' second, got %q", ev.Data)
	}
	if d := newSub.droppedCount(); d != 2 {
		t.Fatalf("dropNew: want 2 drops, got %d", d)
	}
}

// ---- RPC handler glue --------------------------------------------------------

type fakeRs485Stream struct {
	ctx      context.Context
	received chan *pb.Rs485RxEvent
}

func (f *fakeRs485Stream) Send(ev *pb.Rs485RxEvent) error {
	f.received <- ev
	return nil
}
func (f *fakeRs485Stream) SetHeader(md metadata.MD) error  { return nil }
func (f *fakeRs485Stream) SendHeader(md metadata.MD) error { return nil }
func (f *fakeRs485Stream) SetTrailer(md metadata.MD)       {}
func (f *fakeRs485Stream) Context() context.Context        { return f.ctx }
func (f *fakeRs485Stream) SendMsg(m interface{}) error     { return nil }
func (f *fakeRs485Stream) RecvMsg(m interface{}) error     { return nil }

func TestSubscribeRs485RxHandler(t *testing.T) {
	h, _ := newTestHub(t, 3)
	srv := &DeviceControlServer{rs485: h}

	streamCtx, streamCancel := context.WithCancel(context.Background())
	defer streamCancel()
	fs := &fakeRs485Stream{ctx: streamCtx, received: make(chan *pb.Rs485RxEvent, 16)}

	errCh := make(chan error, 1)
	go func() {
		errCh <- srv.SubscribeRs485Rx(&pb.Rs485SubscribeRequest{QueueSize: 8, DropOld: true}, fs)
	}()

	// The handler must forward the 3 fake frames, then end on ctx cancel.
	for i := 0; i < 3; i++ {
		select {
		case ev := <-fs.received:
			if ev.Data[0] != byte('a'+i) {
				t.Fatalf("handler: frame %d mismatch: %q", i, ev.Data)
			}
		case <-time.After(3 * time.Second):
			t.Fatalf("handler: frame %d not delivered", i)
		}
	}
	streamCancel()
	select {
	case <-errCh:
	case <-time.After(3 * time.Second):
		t.Fatalf("handler did not return after stream cancel")
	}
}
