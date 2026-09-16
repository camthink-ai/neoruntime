package handlers

import (
	"context"
	"path/filepath"
	"strings"
	"testing"
	"time"

	"aipc/platform/common/constants"
	"aipc/platform/platform-api/model"

	inferencepb "aipc/platform/ai-runtime/proto"
)

// withTempConstantsRoot points the platform root at a temp dir for the duration
// of a test so detection materialization (models/runtime/<id>/<profile>.hef)
// lands inside the test sandbox.
func withTempConstantsRoot(t *testing.T) {
	t.Helper()
	oldRoot := constants.RootPath()
	constants.SetRootPath(t.TempDir())
	t.Cleanup(func() { constants.SetRootPath(oldRoot) })
}

// selfHealClient builds the inference client over the test env's bufconn.
func selfHealClient(h *APIHandlers) inferencepb.InferenceServiceClient {
	return inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
}

func loadCalls(t *testing.T, fake *fakeAIRuntime) []string {
	t.Helper()
	calls, _ := fake.snapshot()
	return calls
}

// The core promise of #1: a model the user loaded (desired_state=loaded) that
// the runtime no longer holds — solo ai-runtime restart, force_unregister_all
// wipe, crash mid-operation — is re-registered by the heal pass through the
// same core path a REST load uses, and the row reflects it.
func TestRestoreDesiredLoadsReloadsMissingModel(t *testing.T) {
	withTempConstantsRoot(t)
	h, fake, store := newAIUpdateTestEnv(t)
	blob := seedBlob(t, store, "h1")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "heal_det", Name: "heal_det", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1", DesiredState: "loaded",
	})

	h.restoreDesiredLoads(context.Background(), selfHealClient(h),
		map[string]*inferencepb.ModelInfo{}, newModelHealBackoff(), time.Minute)

	calls := loadCalls(t, fake)
	if len(calls) != 1 || calls[0] != "load:heal_det" {
		t.Fatalf("expected exactly one register call, got %v", calls)
	}
	row, err := h.aiModelRepo.GetByModelID("heal_det")
	if err != nil {
		t.Fatalf("GetByModelID: %v", err)
	}
	if row.Status != "loaded" || row.DesiredState != "loaded" {
		t.Errorf("row after heal = status:%q desired:%q, want loaded/loaded", row.Status, row.DesiredState)
	}
	// Registered through loadModelCore, so the composition contract holds:
	// materialized profile path and a non-empty composed variant.
	if want := filepath.Join(constants.ModelsPath(), "runtime", "heal_det", "hailo_yolov8n_384_640.hef"); fake.registeredPath("heal_det") != want {
		t.Errorf("registered path = %q, want %q", fake.registeredPath("heal_det"), want)
	}
	if fake.registeredVariant("heal_det") == "" {
		t.Error("detection heal must register the composed variant, got empty")
	}
}

// Rows that are not restore candidates — not desired-loaded, app-owned
// (app-manager's preload responsibility), or still live in the runtime —
// produce no runtime traffic.
func TestRestoreDesiredLoadsSkipsNonCandidates(t *testing.T) {
	withTempConstantsRoot(t)
	h, fake, store := newAIUpdateTestEnv(t)
	blob := seedBlob(t, store, "h1")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "not_desired", Name: "not_desired", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1", DesiredState: "unloaded",
		// (explicit beats relying on the column default)
	})
	seedAIModel(t, h, &model.AIModel{
		ModelID: "app_owned", Name: "app_owned", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1",
		DesiredState: "loaded", OwnerAppID: "app-x",
	})
	seedAIModel(t, h, &model.AIModel{
		ModelID: "still_live", Name: "still_live", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1", DesiredState: "loaded",
	})
	fake.markLiveEntry(&inferencepb.ModelInfo{
		ModelId: "still_live", ModelPath: blob, OwnerId: systemOwnerID,
	})
	runtimeMap, ok := h.listRuntimeModels(context.Background())
	if !ok {
		t.Fatal("listRuntimeModels failed against bufconn fake")
	}

	h.restoreDesiredLoads(context.Background(), selfHealClient(h),
		runtimeMap, newModelHealBackoff(), time.Minute)

	if calls := loadCalls(t, fake); len(calls) != 0 {
		t.Errorf("no register calls expected, got %v", calls)
	}
	if row, _ := h.aiModelRepo.GetByModelID("not_desired"); row.Status != "uploaded" {
		t.Errorf("not_desired status = %q, want untouched uploaded", row.Status)
	}
}

// Rows registered before the creation paths wrote desired_state explicitly
// carry the old column default "loaded" despite never being loaded. The
// startup demotion must flip exactly those (device-level, status uploaded)
// to "unloaded" — leaving user-loaded rows and app-owned rows alone — so the
// heal pass stops auto-loading models nobody asked to load.
func TestDemoteNeverLoadedImports(t *testing.T) {
	withTempConstantsRoot(t)
	h, fake, store := newAIUpdateTestEnv(t)
	blob := seedBlob(t, store, "h1")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "stale_import", Name: "stale_import", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1", DesiredState: "loaded",
	})
	seedAIModel(t, h, &model.AIModel{
		ModelID: "user_loaded", Name: "user_loaded", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1", DesiredState: "loaded",
	})
	seedAIModel(t, h, &model.AIModel{
		ModelID: "app_row", Name: "app_row", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1",
		DesiredState: "loaded", OwnerAppID: "app-x",
	})
	fake.markLiveEntry(&inferencepb.ModelInfo{
		ModelId: "user_loaded", ModelPath: blob, OwnerId: systemOwnerID,
	})

	h.DemoteNeverLoadedImports()

	for _, tc := range []struct{ id, want string }{
		{"stale_import", "unloaded"}, // the regression: never-loaded import demoted
		{"user_loaded", "loaded"},    // genuinely loaded promise preserved
		{"app_row", "loaded"},        // app-owned rows are app-manager's business
	} {
		row, err := h.aiModelRepo.GetByModelID(tc.id)
		if err != nil || row == nil {
			t.Fatalf("%s: %v", tc.id, err)
		}
		if row.DesiredState != tc.want {
			t.Errorf("%s desired_state = %q, want %q", tc.id, row.DesiredState, tc.want)
		}
	}

	// The demoted row is no longer a heal candidate: one pass, zero loads.
	runtimeMap, ok := h.listRuntimeModels(context.Background())
	if !ok {
		t.Fatal("listRuntimeModels failed against bufconn fake")
	}
	h.restoreDesiredLoads(context.Background(), selfHealClient(h),
		runtimeMap, newModelHealBackoff(), time.Minute)
	if calls := loadCalls(t, fake); len(calls) != 0 {
		t.Errorf("demoted import must not be auto-loaded by the heal pass, got %v", calls)
	}
}

// A model whose reload keeps failing must not hammer the NPU every tick: the
// backoff suppresses the immediate retry, and once the runtime accepts the
// model again (and the backoff window elapses) the next pass recovers it.
func TestRestoreDesiredLoadsBackoffAndRecovery(t *testing.T) {
	withTempConstantsRoot(t)
	h, fake, store := newAIUpdateTestEnv(t)
	blob := seedBlob(t, store, "h1")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "flaky_det", Name: "flaky_det", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1", DesiredState: "loaded",
	})
	interval := 50 * time.Millisecond
	backoff := newModelHealBackoff()
	ctx := context.Background()
	empty := map[string]*inferencepb.ModelInfo{}

	// Pass 1: the runtime rejects the registration.
	fake.loadFail = true
	h.restoreDesiredLoads(ctx, selfHealClient(h), empty, backoff, interval)
	if calls := loadCalls(t, fake); len(calls) != 1 {
		t.Fatalf("pass 1: expected one register attempt, got %v", calls)
	}
	if row, _ := h.aiModelRepo.GetByModelID("flaky_det"); row.Status != "uploaded" {
		t.Errorf("failed heal must not mark row loaded, got %q", row.Status)
	}

	// Pass 2 (immediately): suppressed by backoff — no new NPU attempt.
	h.restoreDesiredLoads(ctx, selfHealClient(h), empty, backoff, interval)
	if calls := loadCalls(t, fake); len(calls) != 1 {
		t.Fatalf("pass 2: expected backoff to suppress retry, got %v", calls)
	}

	// Runtime recovers; after the backoff window the next pass succeeds and
	// clears the failure state.
	fake.loadFail = false
	time.Sleep(interval + 20*time.Millisecond)
	h.restoreDesiredLoads(ctx, selfHealClient(h), empty, backoff, interval)
	if calls := loadCalls(t, fake); len(calls) != 2 {
		t.Fatalf("pass 3: expected recovery attempt, got %v", calls)
	}
	row, err := h.aiModelRepo.GetByModelID("flaky_det")
	if err != nil {
		t.Fatalf("GetByModelID: %v", err)
	}
	if row.Status != "loaded" {
		t.Errorf("row after recovery = %q, want loaded", row.Status)
	}
	if !backoff.ready("flaky_det", time.Now()) {
		t.Error("successful reload must reset the backoff window")
	}
}

// One full pass composes sync + restore: an empty runtime with a
// desired-loaded row ends with the model registered and the row healed.
func TestSelfHealPassRestoresModel(t *testing.T) {
	withTempConstantsRoot(t)
	h, fake, store := newAIUpdateTestEnv(t)
	blob := seedBlob(t, store, "h1")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "pass_det", Name: "pass_det", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1", DesiredState: "loaded",
	})

	h.selfHealPass(context.Background(), newModelHealBackoff(), time.Minute)

	calls := loadCalls(t, fake)
	if len(calls) != 1 || calls[0] != "load:pass_det" {
		t.Fatalf("expected one register call from pass, got %v", calls)
	}
	row, _ := h.aiModelRepo.GetByModelID("pass_det")
	if row.Status != "loaded" {
		t.Errorf("row after pass = %q, want loaded", row.Status)
	}
}

// An unreachable runtime is an empty pass, not a wipe: nothing panics and no
// state is touched.
func TestSelfHealPassWithoutRuntimeIsNoop(t *testing.T) {
	withTempConstantsRoot(t)
	h, fake, _ := newAIUpdateTestEnv(t)
	h.grpcClients.AIRuntime = nil

	h.selfHealPass(context.Background(), newModelHealBackoff(), time.Minute)

	if calls := loadCalls(t, fake); len(calls) != 0 {
		t.Errorf("no runtime calls expected, got %v", calls)
	}
}

// A smoke-test failure surfaces exactly when the load ctx is exhausted —
// the rollback must still reach the runtime on its own deadline. Under the
// old inline rollback the UnregisterModel inherited the dead ctx and the
// broken registration stayed live on the NPU.
func TestLoadModelCoreSmokeRollbackSurvivesExhaustedDeadline(t *testing.T) {
	withTempConstantsRoot(t)
	h, fake, store := newAIUpdateTestEnv(t)
	blob := seedBlob(t, store, "h1")
	// Give the registration a real input spec so the smoke test runs (it
	// skips on empty Inputs), then hang Infer until the ctx dies.
	fake.smokeSpec = &inferencepb.TensorSpec{Shape: []int32{1, 8, 8}, Dtype: inferencepb.DataType_UINT8}
	fake.smokeHang = true
	row := &model.AIModel{
		ModelID: "smoke_bad", Name: "smoke_bad", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1",
	}
	seedAIModel(t, h, row)

	ctx, cancel := context.WithTimeout(context.Background(), 300*time.Millisecond)
	defer cancel()
	err := h.loadModelCore(ctx, selfHealClient(h), row)
	if err == nil || !strings.Contains(err.Error(), "smoke test") {
		t.Fatalf("want smoke-test failure, got %v", err)
	}
	calls := loadCalls(t, fake)
	if len(calls) != 2 || calls[0] != "load:smoke_bad" || calls[1] != "unload:smoke_bad" {
		t.Fatalf("rollback must reach the runtime despite the exhausted ctx, got %v", calls)
	}
	live, ok := h.listRuntimeModels(context.Background())
	if !ok {
		t.Fatal("listRuntimeModels failed against bufconn fake")
	}
	if _, still := live["smoke_bad"]; still {
		t.Error("broken registration must not stay live on the runtime")
	}
	if after, _ := h.aiModelRepo.GetByModelID("smoke_bad"); after.Status != "uploaded" {
		t.Errorf("row must stay uploaded after failed smoke, got %q", after.Status)
	}
}

// A DB persist failure after a successful runtime registration must surface
// as a load failure and roll the registration back: silently succeeding
// leaves status=uploaded with desired_state=unloaded, which the heal loop
// (correctly) refuses to restore — the model would vanish after any runtime
// restart despite the REST load reporting success.
func TestLoadModelCorePersistFailureRollsBack(t *testing.T) {
	withTempConstantsRoot(t)
	h, fake, store, gdb := newBlobGCTestEnv(t)
	blob := seedBlob(t, store, "h1")
	row := &model.AIModel{
		ModelID: "persist_fail", Name: "persist_fail", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1",
	}
	seedAIModel(t, h, row)
	// Detection models run a load-time smoke test; give the fake an input
	// spec so it passes and the failure stays focused on the DB write.
	fake.smokeSpec = &inferencepb.TensorSpec{Shape: []int32{1, 8, 8}, Dtype: inferencepb.DataType_UINT8}

	sqlDB, err := gdb.DB()
	if err != nil {
		t.Fatalf("raw db: %v", err)
	}
	if err := sqlDB.Close(); err != nil {
		t.Fatalf("close db: %v", err)
	}

	loadErr := h.loadModelCore(context.Background(), selfHealClient(h), row)
	if loadErr == nil || !strings.Contains(loadErr.Error(), "persist") {
		t.Fatalf("want persist failure surfaced, got %v", loadErr)
	}
	calls := loadCalls(t, fake)
	if len(calls) != 2 || calls[0] != "load:persist_fail" || calls[1] != "unload:persist_fail" {
		t.Fatalf("persist failure must roll the registration back, got %v", calls)
	}
}

// The backoff schedule itself: 1x, 2x, 4x ... capped at 16x the interval.
func TestModelHealBackoffSchedule(t *testing.T) {
	interval := time.Minute
	b := newModelHealBackoff()
	now := time.Now()

	b.fail("m", interval, now)
	if b.ready("m", now.Add(interval-time.Second)) {
		t.Error("first failure must not be ready before 1x interval")
	}
	if !b.ready("m", now.Add(interval)) {
		t.Error("first failure must be ready at 1x interval")
	}

	b.fail("m", interval, now)
	if b.ready("m", now.Add(2*interval-time.Second)) {
		t.Error("second failure must not be ready before 2x interval")
	}
	if !b.ready("m", now.Add(2*interval)) {
		t.Error("second failure must be ready at 2x interval")
	}

	for range 10 {
		b.fail("m", interval, now)
	}
	if !b.ready("m", now.Add(16*interval)) {
		t.Error("capped schedule must be ready at 16x interval")
	}
	if b.ready("m", now.Add(15*interval)) {
		t.Error("capped schedule must not be ready before 16x interval")
	}

	b.reset("m")
	if !b.ready("m", now) {
		t.Error("reset must clear the window")
	}
}
