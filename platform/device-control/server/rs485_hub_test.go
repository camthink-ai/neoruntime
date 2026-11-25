package main

// rs485_hub_test.go — covers the device-control RS-485 RX hub: subscribe
// validation/clamping, fan-out drop policies, single-shared-upstream
// lifecycle (lazy start, park on last unsubscribe, reconnect), and the
// SubscribeRs485Rx handler glue, against an in-memory fake camera-daemon.

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
	if rs485DefaultQueueSize != 16 || rs485MaxQueueSize != 128 {
		t.Fatalf("unexpected clamp bounds: %d..%d", rs485DefaultQueueSize, rs485MaxQueueSize)
	}
}

// ---- fake camera-daemon ----------------------------------------------------

type fakeCameraDaemon struct {
	camerapb.UnimplementedCameraControlServer

	mu      sync.Mutex
	opens   int
	streams []chan struct{} // one closer chan per open stream
}

func (f *fakeCameraDaemon) SubscribeRs485Rx(_ *camerapb.Empty,
	stream camerapb.CameraControl_SubscribeRs485RxServer) error {
	f.mu.Lock()
	f.opens++
	closer := make(chan struct{})
	f.streams = append(f.streams, closer)
	f.mu.Unlock()

	// Deliver 3 frames, then hold the stream open until cancelled.
	for i := 0; i < 3; i++ {
		if err := stream.Send(&camerapb.Rs485RxEvent{
			Data:        []byte{byte('a' + i)},
			TimestampNs: uint64(i + 1),
		}); err != nil {
			return err
		}
	}
	<-closer
	return stream.Context().Err()
}

func (f *fakeCameraDaemon) openCount() int {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.opens
}

func newFakeCameraDaemonClient(t *testing.T) (camerapb.CameraControlClient, *fakeCameraDaemon) {
	t.Helper()
	fake := &fakeCameraDaemon{}
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

// ---- end-to-end through the pump ------------------------------------------

func TestRs485HubEndToEndSingleUpstream(t *testing.T) {
	client, fake := newFakeCameraDaemonClient(t)
	h := newRs485Hub(client)
	h.backoffBase = 10 * time.Millisecond
	h.backoffMax = 40 * time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	h.Start(ctx)

	// Two subscribers share ONE upstream stream.
	subA, err := h.subscribe(8, true)
	if err != nil {
		t.Fatalf("subscribe A: %v", err)
	}
	subB, err := h.subscribe(8, false)
	if err != nil {
		t.Fatalf("subscribe B: %v", err)
	}

	readN := func(s *rs485Sub, n int, timeout time.Duration) []*pb.Rs485RxEvent {
		var out []*pb.Rs485RxEvent
		deadline := time.After(timeout)
		for len(out) < n {
			select {
			case ev := <-s.ch:
				out = append(out, ev)
			case <-deadline:
				return out
			}
		}
		return out
	}

	got := readN(subA, 3, 3*time.Second)
	if len(got) != 3 {
		t.Fatalf("subscriber A: want 3 frames, got %d", len(got))
	}
	if got[0].Data[0] != 'a' || got[2].Data[0] != 'c' || got[2].TimestampNs != 3 {
		t.Fatalf("subscriber A: unexpected payloads: %+v", got)
	}
	if gotB := readN(subB, 3, time.Second); len(gotB) != 3 {
		t.Fatalf("subscriber B: want 3 frames, got %d", len(gotB))
	}

	// Allow the lazy pump + reconnect timing to settle, then require that
	// exactly one upstream stream was opened for both subscribers.
	time.Sleep(150 * time.Millisecond)
	if n := fake.openCount(); n != 1 {
		t.Fatalf("want exactly 1 upstream open for 2 subscribers, got %d", n)
	}

	// Last unsubscribe parks the pump (upstream cancelled, no new opens).
	h.unsubscribe(subA)
	h.unsubscribe(subB)
	time.Sleep(150 * time.Millisecond)
	if n := fake.openCount(); n != 1 {
		t.Fatalf("park: upstream reopened while idle (opens=%d)", n)
	}

	// A new subscriber restarts the upstream exactly once more.
	subC, err := h.subscribe(8, true)
	if err != nil {
		t.Fatalf("subscribe C: %v", err)
	}
	if got := readN(subC, 3, 3*time.Second); len(got) != 3 {
		t.Fatalf("subscriber C after park: want 3 frames, got %d", len(got))
	}
	if n := fake.openCount(); n != 2 {
		t.Fatalf("want 2 upstream opens total, got %d", n)
	}
	h.unsubscribe(subC)
}

// ---- drop policy via fanout -------------------------------------------------

func TestRs485HubFanoutDropPolicies(t *testing.T) {
	h := newRs485Hub(nil)
	h.Start(context.Background())

	oldSub := &rs485Sub{ch: make(chan *pb.Rs485RxEvent, 2), dropOld: true}
	newSub := &rs485Sub{ch: make(chan *pb.Rs485RxEvent, 2), dropOld: false}
	h.mu.Lock()
	if h.subs == nil {
		h.subs = make(map[*rs485Sub]struct{})
	}
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
	client, _ := newFakeCameraDaemonClient(t)
	srv := &DeviceControlServer{rs485: newRs485Hub(client)}
	srv.rs485.backoffBase = 10 * time.Millisecond
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	srv.rs485.Start(ctx)

	streamCtx, streamCancel := context.WithCancel(context.Background())
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
