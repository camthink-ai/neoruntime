package lens

import (
	"context"
	"net"
	"path/filepath"
	"testing"
	"time"

	"aipc/platform/device-control/hal"
	pb "aipc/platform/device-control/lens/lenspb"
	"google.golang.org/grpc"
	"google.golang.org/grpc/connectivity"
)

type testLensServer struct {
	pb.UnimplementedLensHALServer
}

func (testLensServer) Init(context.Context, *pb.Empty) (*pb.HalStatus, error) {
	return &pb.HalStatus{Ok: true}, nil
}

// A missing camera-daemon socket must not turn client construction into a
// one-shot startup deadline.  ClientConn keeps trying in the background.
func TestNewLensClientDoesNotBlockForMissingSocket(t *testing.T) {
	endpoint := filepath.Join(t.TempDir(), "camera-control.sock")
	started := time.Now()
	client, err := NewLensClient(endpoint, hal.DefaultLensConfig())
	if err != nil {
		t.Fatalf("NewLensClient() error = %v", err)
	}
	t.Cleanup(func() { _ = client.Conn().Close() })

	if elapsed := time.Since(started); elapsed > time.Second {
		t.Fatalf("NewLensClient blocked for %s with a missing socket", elapsed)
	}
}

func TestLensClientConnectsWhenSocketAppearsLater(t *testing.T) {
	endpoint := filepath.Join(t.TempDir(), "camera-control.sock")
	client, err := NewLensClient(endpoint, hal.DefaultLensConfig())
	if err != nil {
		t.Fatalf("NewLensClient() error = %v", err)
	}
	t.Cleanup(func() { _ = client.Conn().Close() })
	client.Conn().Connect()

	lis, err := net.Listen("unix", endpoint)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	server := grpc.NewServer()
	pb.RegisterLensHALServer(server, testLensServer{})
	t.Cleanup(server.Stop)
	go func() { _ = server.Serve(lis) }()

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	for client.Conn().GetState() != connectivity.Ready {
		state := client.Conn().GetState()
		if !client.Conn().WaitForStateChange(ctx, state) {
			t.Fatalf("connection did not become ready: %v", ctx.Err())
		}
	}
	if err := client.Init(); err != nil {
		t.Fatalf("Init() after socket appeared = %v", err)
	}
}
