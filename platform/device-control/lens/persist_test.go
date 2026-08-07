package lens

import (
	"context"
	"net"
	"os"
	"path/filepath"
	"sync"
	"testing"

	"aipc/platform/common/constants"
	"aipc/platform/device-control/hal"
	pb "aipc/platform/device-control/lens/lenspb"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"
)

// withTempRoot redirects constants.RootPath to a temp dir for one test and
// restores it on cleanup. lensConfigFile() resolves under constants.ConfigPath
// (= root + "/etc"), so persist/load hit the temp file. Tests in this package
// are non-parallel, so the global swap is safe.
func withTempRoot(t *testing.T) {
	t.Helper()
	orig := constants.RootPath()
	tmp := t.TempDir()
	constants.SetRootPath(tmp)
	t.Cleanup(func() { constants.SetRootPath(orig) })
}

// recordingLensServer is a fake LensHALServer that always succeeds and records
// the last ZoomLimitSet / FocusLimitSet / IrisTargetSet arguments. The zoom/focus
// recorders are retained so tests can assert replay and the limit setters do
// NOT push zoom/focus (the 93.72 regression was exactly that side-effect).
type recordingLensServer struct {
	pb.UnimplementedLensHALServer
	mu sync.Mutex

	zoomMin, zoomMax   int32
	zoomSet            bool
	focusMin, focusMax int32
	focusSet           bool
	irisTarget         uint32
	irisSet            bool
}

func (r *recordingLensServer) ZoomLimitSet(_ context.Context, req *pb.LimitSetRequest) (*pb.HalStatus, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.zoomMin, r.zoomMax, r.zoomSet = req.MinPos, req.MaxPos, true
	return &pb.HalStatus{Ok: true}, nil
}

func (r *recordingLensServer) FocusLimitSet(_ context.Context, req *pb.LimitSetRequest) (*pb.HalStatus, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.focusMin, r.focusMax, r.focusSet = req.MinPos, req.MaxPos, true
	return &pb.HalStatus{Ok: true}, nil
}

func (r *recordingLensServer) IrisTargetSet(_ context.Context, req *pb.IrisTargetRequest) (*pb.HalStatus, error) {
	r.mu.Lock()
	defer r.mu.Unlock()
	r.irisTarget, r.irisSet = req.Target, true
	return &pb.HalStatus{Ok: true}, nil
}

// newReplayClient builds a LensClient wired to a bufconn fake HAL so that
// ReplayPersistedConfig's HAL round-trips hit the recording server.
func newReplayClient(t *testing.T, cfg hal.LensConfig) (*LensClient, *recordingLensServer) {
	t.Helper()
	lis := bufconn.Listen(1024 * 1024)
	fake := &recordingLensServer{}
	srv := grpc.NewServer()
	pb.RegisterLensHALServer(srv, fake)
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)

	conn, err := grpc.DialContext(context.Background(), "bufnet",
		grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) { return lis.Dial() }),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		t.Fatalf("dial bufnet: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	return &LensClient{conn: conn, client: pb.NewLensHALClient(conn), cfg: cfg}, fake
}

// sideFileExists returns whether the lens side-file is present under the
// redirected config root.
func sideFileExists(t *testing.T) bool {
	t.Helper()
	_, err := os.Stat(lensConfigFile())
	return err == nil
}

// TestPersistLoad_IrisTargetSurvives persists an iris target of 0 with
// has_iris_target=true and asserts the gate stays true (0 is a valid target).
func TestPersistLoad_IrisTargetSurvives(t *testing.T) {
	withTempRoot(t)

	if err := persistLensConfig(0, true); err != nil {
		t.Fatalf("persist: %v", err)
	}
	if !sideFileExists(t) {
		t.Fatal("side-file not created")
	}
	gIris, gHas, ok := loadLensConfig()
	if !ok || !gHas {
		t.Fatalf("load ok=%v has=%v, want true/true", ok, gHas)
	}
	if gIris != 0 {
		t.Errorf("iris_target = %d, want 0 (valid target must survive)", gIris)
	}
}

// TestPersistLoad_HasIrisTargetFalseSurvives persists has_iris_target=false and
// asserts the gate stays false so an unset config is distinguishable from a
// config that set the target to 0.
func TestPersistLoad_HasIrisTargetFalseSurvives(t *testing.T) {
	withTempRoot(t)

	if err := persistLensConfig(0, false); err != nil {
		t.Fatalf("persist: %v", err)
	}
	_, gHas, ok := loadLensConfig()
	if !ok || gHas {
		t.Fatalf("load ok=%v has=%v, want true/false", ok, gHas)
	}
}

// TestLoadLensConfig_MissingFileIsNormal asserts a missing side-file (fresh
// device, never persisted) returns ok=false without error — replay must no-op.
func TestLoadLensConfig_MissingFileIsNormal(t *testing.T) {
	withTempRoot(t)

	if _, _, ok := loadLensConfig(); ok {
		t.Fatal("loadLensConfig ok=true on missing file, want false (fresh boot)")
	}
}

// TestLoadLensConfig_CorruptFileIsIgnored asserts a corrupt side-file is
// ignored (ok=false) rather than applying garbage to the HAL.
func TestLoadLensConfig_CorruptFileIsIgnored(t *testing.T) {
	withTempRoot(t)
	if err := os.MkdirAll(filepath.Dir(lensConfigFile()), 0755); err != nil {
		t.Fatalf("mkdir: %v", err)
	}
	if err := os.WriteFile(lensConfigFile(), []byte("{not json"), 0644); err != nil {
		t.Fatalf("seed corrupt: %v", err)
	}
	if _, _, ok := loadLensConfig(); ok {
		t.Fatal("loadLensConfig ok=true on corrupt file, want false")
	}
}

// TestIrisTargetSet_PersistsOnSuccess wires a recording HAL and asserts
// IrisTargetSet writes its value to the side-file after a successful round-trip.
func TestIrisTargetSet_PersistsOnSuccess(t *testing.T) {
	withTempRoot(t)
	client, fake := newReplayClient(t, hal.DefaultLensConfig())

	if err := client.IrisTargetSet(300); err != nil {
		t.Fatalf("IrisTargetSet: %v", err)
	}
	if !fake.irisSet || fake.irisTarget != 300 {
		t.Fatalf("HAL IrisTargetSet not invoked: set=%v target=%d", fake.irisSet, fake.irisTarget)
	}
	gIris, gHas, ok := loadLensConfig()
	if !ok || !gHas || gIris != 300 {
		t.Fatalf("iris not persisted: target=%d has=%v ok=%v", gIris, gHas, ok)
	}
}

// TestZoomFocusLimitSet_DoNotPersist asserts the limit setters update the live
// HAL and in-memory cfg but do NOT write the side-file. Limits are yaml-sourced
// calibration constants; persisting them would let a one-off set poison every
// later boot's replay (the 93.72 regression root cause).
func TestZoomFocusLimitSet_DoNotPersist(t *testing.T) {
	withTempRoot(t)
	client, fake := newReplayClient(t, hal.DefaultLensConfig())

	if err := client.ZoomLimitSet(-10, 20); err != nil {
		t.Fatalf("ZoomLimitSet: %v", err)
	}
	if !fake.zoomSet {
		t.Fatal("HAL ZoomLimitSet not invoked")
	}
	if sideFileExists(t) {
		t.Fatal("ZoomLimitSet persisted a side-file; limits must never be persisted")
	}
	if got := client.ZoomLimits(); got != (hal.LensLimit{MinPos: -10, MaxPos: 20}) {
		t.Errorf("live c.cfg.ZoomLimit = %+v, want {-10 20}", got)
	}

	if err := client.FocusLimitSet(-30, 40); err != nil {
		t.Fatalf("FocusLimitSet: %v", err)
	}
	if !fake.focusSet {
		t.Fatal("HAL FocusLimitSet not invoked")
	}
	if sideFileExists(t) {
		t.Fatal("FocusLimitSet persisted a side-file; limits must never be persisted")
	}
	if got := client.FocusLimits(); got != (hal.LensLimit{MinPos: -30, MaxPos: 40}) {
		t.Errorf("live c.cfg.FocusLimit = %+v, want {-30 40}", got)
	}
}

// TestReplayPersistedConfig_RestoresIrisOnly asserts replay re-pushes a
// persisted iris target to the HAL and does NOT touch zoom/focus limits
// (which must stay at the yaml-calibrated cfg values).
func TestReplayPersistedConfig_RestoresIrisOnly(t *testing.T) {
	withTempRoot(t)

	if err := persistLensConfig(300, true); err != nil {
		t.Fatalf("seed: %v", err)
	}

	client, fake := newReplayClient(t, hal.DefaultLensConfig())
	client.ReplayPersistedConfig()

	if !fake.irisSet || fake.irisTarget != 300 {
		t.Errorf("replay did not push iris override: set=%v target=%d", fake.irisSet, fake.irisTarget)
	}
	if fake.zoomSet {
		t.Error("replay pushed zoom limit; limits must never be replayed")
	}
	if fake.focusSet {
		t.Error("replay pushed focus limit; limits must never be replayed")
	}
}

// TestReplayPersistedConfig_NoOpWhenNoIris asserts replay does NOT push iris
// when has_iris_target was false, and never pushes zoom/focus.
func TestReplayPersistedConfig_NoOpWhenNoIris(t *testing.T) {
	withTempRoot(t)

	if err := persistLensConfig(0, false); err != nil {
		t.Fatalf("seed: %v", err)
	}

	client, fake := newReplayClient(t, hal.DefaultLensConfig())
	client.ReplayPersistedConfig()

	if fake.irisSet {
		t.Error("replay pushed iris target even though has_iris_target was false")
	}
	if fake.zoomSet {
		t.Error("replay pushed zoom limit; limits must never be replayed")
	}
	if fake.focusSet {
		t.Error("replay pushed focus limit; limits must never be replayed")
	}
}

// TestReplayPersistedConfig_NoOpOnMissingFile asserts a fresh boot (no
// side-file) issues no HAL traffic at all.
func TestReplayPersistedConfig_NoOpOnMissingFile(t *testing.T) {
	withTempRoot(t)

	client, fake := newReplayClient(t, hal.DefaultLensConfig())
	client.ReplayPersistedConfig()

	if fake.irisSet || fake.zoomSet || fake.focusSet {
		t.Errorf("replay pushed HAL traffic on missing side-file: iris=%v zoom=%v focus=%v",
			fake.irisSet, fake.zoomSet, fake.focusSet)
	}
}
