package handlers

import (
	"bytes"
	"context"
	"encoding/json"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"

	"github.com/gin-gonic/gin"
	"github.com/glebarez/sqlite"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"

	"aipc/platform/common/constants"
	"aipc/platform/platform-api/model"
	"aipc/platform/platform-api/repo"
	"aipc/platform/platform-api/storage"

	inferencepb "aipc/platform/ai-runtime/proto"
)

// fakeAIRuntime records RegisterModel/UnregisterModel calls so update-flow
// tests can assert the unload→reload sequence.
type fakeAIRuntime struct {
	inferencepb.UnimplementedInferenceServiceServer
	mu          sync.Mutex
	calls       []string
	regPaths    map[string]string
	regVariants map[string]string
	live        map[string]bool
	liveInfos   map[string]*inferencepb.ModelInfo
	loadFail    bool
	// unloadFail makes UnregisterModel answer OK transport status with
	// success=false — the runtime's real "logically refused" shape (a racing
	// session kept the model in use, a co-owner kept it live).
	unloadFail bool
	// smokeSpec, when set, is attached as the single input of entries created
	// by RegisterModel, so a load-time smoke test gets a real tensor spec to
	// build a zero input from instead of skipping on empty Inputs.
	smokeSpec *inferencepb.TensorSpec
	// smokeHang makes Infer block until its ctx dies — the deterministic way
	// to exhaust a load deadline inside the smoke test stage.
	smokeHang bool
}

func (f *fakeAIRuntime) UnregisterModel(_ context.Context, in *inferencepb.ModelInfo) (*inferencepb.Status, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls = append(f.calls, "unload:"+in.ModelId)
	if f.unloadFail {
		// Keep the entry live: a refused unload means the runtime is still
		// serving this model.
		return &inferencepb.Status{Success: false, Message: "model in use"}, nil
	}
	delete(f.live, in.ModelId)
	delete(f.liveInfos, in.ModelId)
	return &inferencepb.Status{Success: true}, nil
}

func (f *fakeAIRuntime) RegisterModel(_ context.Context, in *inferencepb.ModelRegisterRequest) (*inferencepb.ModelRegisterResponse, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.calls = append(f.calls, "load:"+in.ModelId)
	if f.regPaths == nil {
		f.regPaths = map[string]string{}
	}
	f.regPaths[in.ModelId] = in.ModelPath
	if f.regVariants == nil {
		f.regVariants = map[string]string{}
	}
	f.regVariants[in.ModelId] = in.ModelVariant
	if !f.loadFail {
		if f.live == nil {
			f.live = map[string]bool{}
		}
		f.live[in.ModelId] = true
		if f.liveInfos == nil {
			f.liveInfos = map[string]*inferencepb.ModelInfo{}
		}
		owner := in.OwnerId
		if owner == "" {
			// grpc_service.cpp normalizes ownerless registrations to
			// "<system>" before storing; mirror that so ListModels reports
			// what a real runtime reports, not the "" the wire request had.
			owner = systemOwnerID
		}
		entry := &inferencepb.ModelInfo{
			ModelId: in.ModelId, ModelPath: in.ModelPath, OwnerId: owner,
		}
		if f.smokeSpec != nil {
			entry.Inputs = []*inferencepb.TensorSpec{f.smokeSpec}
		}
		f.liveInfos[in.ModelId] = entry
	}
	if f.loadFail {
		return &inferencepb.ModelRegisterResponse{
			Status: &inferencepb.Status{Success: false, Message: "npu rejected"},
		}, nil
	}
	return &inferencepb.ModelRegisterResponse{ModelId: in.ModelId}, nil
}

func (f *fakeAIRuntime) registeredVariant(modelID string) string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.regVariants[modelID]
}

func (f *fakeAIRuntime) registeredPath(modelID string) string {
	f.mu.Lock()
	defer f.mu.Unlock()
	return f.regPaths[modelID]
}

// ListModels reports the live set so handlers reconcile against a runtime
// view that evolves with Register/Unregister calls. Entries seeded via
// markLiveEntry keep their path/owner/transient fields — the heal logic in
// LoadModel reads exactly those.
func (f *fakeAIRuntime) ListModels(_ context.Context, _ *inferencepb.Empty) (*inferencepb.ModelListResponse, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	resp := &inferencepb.ModelListResponse{}
	for id := range f.live {
		if info := f.liveInfos[id]; info != nil {
			resp.Models = append(resp.Models, info)
			continue
		}
		resp.Models = append(resp.Models, &inferencepb.ModelInfo{ModelId: id})
	}
	return resp, nil
}

// GetModelInfo serves the stored live entry when present; anything else is
// genuinely unknown to the fake, so the embedded Unimplemented error stands
// (production callers treat that as "no info" and skip optional steps).
func (f *fakeAIRuntime) GetModelInfo(ctx context.Context, in *inferencepb.ModelInfo) (*inferencepb.ModelInfo, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if info := f.liveInfos[in.ModelId]; info != nil {
		return info, nil
	}
	return f.UnimplementedInferenceServiceServer.GetModelInfo(ctx, in)
}

// Infer is the smoke-test endpoint: by default it succeeds instantly; with
// smokeHang set it blocks until its ctx is cancelled, which is how tests
// exhaust a load deadline exactly at the smoke stage.
func (f *fakeAIRuntime) Infer(ctx context.Context, _ *inferencepb.InferRequest) (*inferencepb.InferResponse, error) {
	if f.smokeHang {
		<-ctx.Done()
		return nil, ctx.Err()
	}
	return &inferencepb.InferResponse{}, nil
}

// markLive seeds the runtime with a model no RegisterModel call created
// (e.g. simulating a model registered by another component).
func (f *fakeAIRuntime) markLive(id string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.live == nil {
		f.live = map[string]bool{}
	}
	f.live[id] = true
}

// markLiveEntry seeds a runtime entry with its full registration info —
// e.g. a legacy preload registration carrying the raw CAS blob path.
func (f *fakeAIRuntime) markLiveEntry(info *inferencepb.ModelInfo) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.live == nil {
		f.live = map[string]bool{}
	}
	if f.liveInfos == nil {
		f.liveInfos = map[string]*inferencepb.ModelInfo{}
	}
	f.live[info.ModelId] = true
	f.liveInfos[info.ModelId] = info
}

func (f *fakeAIRuntime) snapshot() ([]string, map[string]string) {
	f.mu.Lock()
	defer f.mu.Unlock()
	out := make([]string, len(f.calls))
	copy(out, f.calls)
	return out, f.regPaths
}

// newAIUpdateTestEnv returns a handler backed by a sqlite DB, a bufconn
// ai-runtime and a real temp-dir model store, plus the fake for assertions.
func newAIUpdateTestEnv(t *testing.T) (*APIHandlers, *fakeAIRuntime, *storage.ModelStorage) {
	t.Helper()
	h, fake, store, _ := newAIUpdateTestEnvWithDB(t)
	return h, fake, store
}

func newAIUpdateTestEnvWithDB(t *testing.T) (*APIHandlers, *fakeAIRuntime, *storage.ModelStorage, *gorm.DB) {
	t.Helper()
	gdb, err := gorm.Open(sqlite.Open(filepath.Join(t.TempDir(), "ai_update.db")), &gorm.Config{
		Logger: logger.Default.LogMode(logger.Warn),
	})
	if err != nil {
		t.Fatalf("gorm.Open: %v", err)
	}
	t.Cleanup(func() {
		if sqlDB, err := gdb.DB(); err == nil {
			_ = sqlDB.Close()
		}
	})
	if err := gdb.AutoMigrate(&model.AIModel{}); err != nil {
		t.Fatalf("AutoMigrate: %v", err)
	}

	store, err := storage.NewModelStorage(filepath.Join(t.TempDir(), "blobs"), 0, 0)
	if err != nil {
		t.Fatalf("NewModelStorage: %v", err)
	}

	lis := bufconn.Listen(1024 * 1024)
	fake := &fakeAIRuntime{}
	srv := grpc.NewServer()
	inferencepb.RegisterInferenceServiceServer(srv, fake)
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

	return &APIHandlers{
		aiModelRepo: repo.NewAIModelRepo(gdb),
		grpcClients: &GRPCClients{AIRuntime: conn},
		modelStore:  store,
	}, fake, store, gdb
}

// seedBlob drops a dummy .hef blob into the store so Exists/BlobPath resolve.
func seedBlob(t *testing.T, store *storage.ModelStorage, hash string) string {
	t.Helper()
	path := store.BlobPath(hash, ".hef")
	if err := os.WriteFile(path, []byte("blob-"+hash), 0644); err != nil {
		t.Fatalf("seed blob %s: %v", hash, err)
	}
	return path
}

func putUpdate(t *testing.T, h *APIHandlers, modelID, body string) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Params = gin.Params{{Key: "model_id", Value: modelID}}
	c.Request = httptest.NewRequest(http.MethodPut, "/api/v1/ai/models/"+modelID, strings.NewReader(body))
	c.Request.Header.Set("Content-Type", "application/json")
	h.UpdateModel(c)
	return w
}

func respCode(t *testing.T, w *httptest.ResponseRecorder) int {
	t.Helper()
	var resp struct {
		Code int `json:"code"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatalf("decode response %q: %v", w.Body.String(), err)
	}
	return resp.Code
}

func seedAIModel(t *testing.T, h *APIHandlers, m *model.AIModel) {
	t.Helper()
	if err := h.aiModelRepo.Create(m); err != nil {
		t.Fatalf("seed %s: %v", m.ModelID, err)
	}
}

func TestUpdateModelNotFound(t *testing.T) {
	h, fake, _ := newAIUpdateTestEnv(t)
	w := putUpdate(t, h, "ghost", `{"model_type":"detection"}`)
	if respCode(t, w) != CodeNotFound {
		t.Fatalf("code = %d body=%s, want %d", respCode(t, w), w.Body.String(), CodeNotFound)
	}
	if calls, _ := fake.snapshot(); len(calls) != 0 {
		t.Errorf("no runtime calls expected, got %v", calls)
	}
}

func TestUpdateModelAppOwnedRowIsHidden(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)
	seedAIModel(t, h, &model.AIModel{ModelID: "app_dyn", Name: "AppDyn", Status: "loaded", Source: "dynamic", OwnerAppID: "app-x"})
	w := putUpdate(t, h, "app_dyn", `{"model_type":"detection"}`)
	if respCode(t, w) != CodeNotFound {
		t.Fatalf("app-owned row must be treated as not found, code = %d body=%s", respCode(t, w), w.Body.String())
	}
}

func TestUpdateModelMetadataOnly(t *testing.T) {
	h, fake, _ := newAIUpdateTestEnv(t)
	seedAIModel(t, h, &model.AIModel{
		ModelID: "web_det", Name: "web_det", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: "/data/aipc/models/old.hef", FileHash: "h1",
	})
	w := putUpdate(t, h, "web_det", `{"model_type":"detection","model_variant":"yolo","config":{"threshold":0.5,"max_detections":32}}`)
	if respCode(t, w) != 0 {
		t.Fatalf("update failed: %s", w.Body.String())
	}
	row, err := h.aiModelRepo.GetByModelID("web_det")
	if err != nil || row == nil {
		t.Fatalf("row missing: %v", err)
	}
	if row.Variant != "yolo" || row.Threshold != 0.5 || row.MaxDetections != 32 {
		t.Errorf("metadata not applied: variant=%q threshold=%f maxDet=%d", row.Variant, row.Threshold, row.MaxDetections)
	}
	if row.FileHash != "h1" || row.FilePath != "/data/aipc/models/old.hef" {
		t.Errorf("file must be untouched on metadata-only update: %q %q", row.FileHash, row.FilePath)
	}
	if row.Status != "uploaded" {
		t.Errorf("status = %q, want uploaded", row.Status)
	}
	if calls, _ := fake.snapshot(); len(calls) != 0 {
		t.Errorf("no runtime calls expected on metadata-only update, got %v", calls)
	}
}

func TestUpdateModelLoadedFileSwapReloads(t *testing.T) {
	h, fake, store := newAIUpdateTestEnv(t)
	// Keep materialized runtime copies inside the test sandbox.
	oldRoot := constants.RootPath()
	constants.SetRootPath(t.TempDir())
	t.Cleanup(func() { constants.SetRootPath(oldRoot) })
	newBlob := seedBlob(t, store, "h2")
	// The row says loaded and the runtime actually serves it — under
	// trust-the-runtime the swap decision probes presence, not the row.
	fake.markLive("live_det")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "live_det", Name: "live_det", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
		DesiredState: "loaded",
	})
	w := putUpdate(t, h, "live_det", `{"file_hash":"h2","model_type":"detection"}`)
	if respCode(t, w) != 0 {
		t.Fatalf("update failed: %s", w.Body.String())
	}
	calls, regPaths := fake.snapshot()
	if len(calls) != 2 || calls[0] != "unload:live_det" || calls[1] != "load:live_det" {
		t.Fatalf("expected unload→load sequence, got %v", calls)
	}
	// Detection models reload via the materialized postprocess-profile path —
	// a basename the vendor plugin recognizes — hardlinked to the new blob.
	materialized := filepath.Join(constants.ModelsPath(), "runtime", "live_det", "hailo_yolov8n_384_640.hef")
	if regPaths["live_det"] != materialized {
		t.Errorf("reload used path %q, want materialized %q", regPaths["live_det"], materialized)
	}
	dstStat, err := os.Stat(materialized)
	if err != nil {
		t.Fatalf("materialized runtime copy missing: %v", err)
	}
	srcStat, err := os.Stat(newBlob)
	if err != nil {
		t.Fatal(err)
	}
	if !os.SameFile(dstStat, srcStat) {
		t.Error("materialized copy is not linked to the swapped blob")
	}
	row, err := h.aiModelRepo.GetByModelID("live_det")
	if err != nil || row == nil {
		t.Fatalf("row missing: %v", err)
	}
	if row.Status != "loaded" || row.FileHash != "h2" {
		t.Errorf("row after swap: status=%q hash=%q, want loaded/h2", row.Status, row.FileHash)
	}
	if row.FilePath != newBlob {
		t.Errorf("DB file path must track the swapped blob, got %q want %q", row.FilePath, newBlob)
	}
}

func TestUpdateModelUnloadRefusedAbortsUpdate(t *testing.T) {
	h, fake, store := newAIUpdateTestEnv(t)
	seedBlob(t, store, "h2")
	// Row loaded + runtime serves it, but the unload is logically refused —
	// a racing session kept the model in use. The runtime reports that as OK
	// transport status with success=false; proceeding would commit the new
	// row while the NPU keeps serving the old weights under the same id, so
	// the refusal must veto the update.
	fake.markLive("busy_det")
	fake.unloadFail = true
	seedAIModel(t, h, &model.AIModel{
		ModelID: "busy_det", Name: "busy_det", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
	})
	w := putUpdate(t, h, "busy_det", `{"file_hash":"h2","model_type":"detection"}`)
	if respCode(t, w) != CodeOperationFailed {
		t.Fatalf("code = %d body=%s, want %d (refused unload must abort)", respCode(t, w), w.Body.String(), CodeOperationFailed)
	}
	calls, _ := fake.snapshot()
	if len(calls) != 1 || calls[0] != "unload:busy_det" {
		t.Fatalf("refused unload must abort before any reload, got %v", calls)
	}
	row, err := h.aiModelRepo.GetByModelID("busy_det")
	if err != nil || row == nil {
		t.Fatalf("row missing: %v", err)
	}
	if row.FileHash != "h1" || row.Status != "loaded" {
		t.Errorf("row must keep its pre-update state, got hash=%q status=%q", row.FileHash, row.Status)
	}
}

func TestUpdateModelLoadedSameHashNoOpDoesNotReload(t *testing.T) {
	h, fake, store := newAIUpdateTestEnv(t)
	seedBlob(t, store, "h1")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "stable_det", Name: "stable_det", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
	})
	w := putUpdate(t, h, "stable_det", `{"file_hash":"h1"}`)
	if respCode(t, w) != 0 {
		t.Fatalf("update failed: %s", w.Body.String())
	}
	if calls, _ := fake.snapshot(); len(calls) != 0 {
		t.Errorf("no-op update must not reload, got %v", calls)
	}
	row, _ := h.aiModelRepo.GetByModelID("stable_det")
	if row.Status != "loaded" {
		t.Errorf("status = %q, want loaded", row.Status)
	}
}

func TestUpdateModelLoadedTuningChangeReloads(t *testing.T) {
	h, fake, store := newAIUpdateTestEnv(t)
	// Keep materialized runtime copies inside the test sandbox.
	oldRoot := constants.RootPath()
	constants.SetRootPath(t.TempDir())
	t.Cleanup(func() { constants.SetRootPath(oldRoot) })
	seedBlob(t, store, "h1")
	fake.markLive("tuned_det")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "tuned_det", Name: "tuned_det", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
	})
	w := putUpdate(t, h, "tuned_det", `{"file_hash":"h1","config":{"threshold":0.5,"max_detections":16}}`)
	if respCode(t, w) != 0 {
		t.Fatalf("update failed: %s", w.Body.String())
	}
	calls, _ := fake.snapshot()
	if len(calls) != 2 || calls[0] != "unload:tuned_det" || calls[1] != "load:tuned_det" {
		t.Fatalf("tuning change on a loaded model must unload→reload, got %v", calls)
	}
	// The reloaded registration must carry the new tuning to the runtime —
	// a bare row update would leave the NPU serving the old threshold.
	variant := fake.registeredVariant("tuned_det")
	if !strings.Contains(variant, `"detection_threshold":0.5`) || !strings.Contains(variant, `"max_boxes":16`) {
		t.Errorf("reloaded variant must carry new tuning, got %s", variant)
	}
	row, _ := h.aiModelRepo.GetByModelID("tuned_det")
	if row.Status != "loaded" || row.Threshold != 0.5 || row.MaxDetections != 16 {
		t.Errorf("row after tuning reload: status=%q threshold=%f maxDet=%d", row.Status, row.Threshold, row.MaxDetections)
	}
}

func TestUpdateModelPersistFailureRestoresOriginalRuntime(t *testing.T) {
	h, fake, store, gdb := newAIUpdateTestEnvWithDB(t)
	seedBlob(t, store, "h2")
	fake.markLive("restore_me")
	original := &model.AIModel{
		ModelID: "restore_me", Name: "restore_me", Status: "loaded", Source: "web",
		ModelType: "classification", Variant: "old-variant",
		FilePath: "/models/old.hef", FileHash: "h1", DesiredState: "loaded",
	}
	seedAIModel(t, h, original)
	// Fail only the staged file swap Save; reads and the existing seed remain
	// healthy, and a runtime-only compensation cannot touch this trigger.
	if err := gdb.Exec(`CREATE TRIGGER fail_restore_me_swap BEFORE UPDATE ON ai_models
		WHEN OLD.model_id = 'restore_me' AND NEW.file_hash = 'h2'
		BEGIN SELECT RAISE(FAIL, 'target staged save failed'); END`).Error; err != nil {
		t.Fatalf("create trigger: %v", err)
	}

	w := putUpdate(t, h, "restore_me", `{"file_hash":"h2","model_variant":"new-variant"}`)
	if respCode(t, w) != CodeServiceError || !strings.Contains(w.Body.String(), "target staged save failed") {
		t.Fatalf("want staged persist failure, code=%d body=%s", respCode(t, w), w.Body.String())
	}
	calls, paths := fake.snapshot()
	if len(calls) != 2 || calls[0] != "unload:restore_me" || calls[1] != "load:restore_me" {
		t.Fatalf("persist compensation order = %v, want unload old → load old", calls)
	}
	if paths["restore_me"] != original.FilePath {
		t.Errorf("restored path = %q, want original %q", paths["restore_me"], original.FilePath)
	}
	if got := fake.registeredVariant("restore_me"); got != original.Variant {
		t.Errorf("restored variant = %q, want original %q", got, original.Variant)
	}
	row, err := h.aiModelRepo.GetByModelID("restore_me")
	if err != nil {
		t.Fatal(err)
	}
	if row.FilePath != original.FilePath || row.FileHash != original.FileHash || row.Variant != original.Variant || row.Status != original.Status {
		t.Errorf("DB row changed despite failed Save: %+v", row)
	}
}

func TestUpdateModelPersistFailureReportsRestoreFailure(t *testing.T) {
	h, fake, store, gdb := newAIUpdateTestEnvWithDB(t)
	seedBlob(t, store, "h2")
	fake.markLive("restore_bad")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "restore_bad", Name: "restore_bad", Status: "loaded", Source: "web",
		ModelType: "classification", Variant: "old-variant",
		FilePath: "/models/old.hef", FileHash: "h1", DesiredState: "loaded",
	})
	if err := gdb.Exec(`CREATE TRIGGER fail_restore_bad_swap BEFORE UPDATE ON ai_models
		WHEN OLD.model_id = 'restore_bad' AND NEW.file_hash = 'h2'
		BEGIN SELECT RAISE(FAIL, 'target staged save failed'); END`).Error; err != nil {
		t.Fatalf("create trigger: %v", err)
	}
	fake.loadFail = true

	w := putUpdate(t, h, "restore_bad", `{"file_hash":"h2","model_variant":"new-variant"}`)
	if respCode(t, w) != CodeServiceError {
		t.Fatalf("code=%d body=%s, want service error", respCode(t, w), w.Body.String())
	}
	if !strings.Contains(w.Body.String(), "target staged save failed") || !strings.Contains(w.Body.String(), "failed to restore original model") || !strings.Contains(w.Body.String(), "npu rejected") {
		t.Fatalf("combined error missing persist or restore cause: %s", w.Body.String())
	}
	calls, _ := fake.snapshot()
	if len(calls) != 2 || calls[0] != "unload:restore_bad" || calls[1] != "load:restore_bad" {
		t.Fatalf("restore failure call order = %v", calls)
	}
	row, _ := h.aiModelRepo.GetByModelID("restore_bad")
	if row.FileHash != "h1" || row.FilePath != "/models/old.hef" || row.Variant != "old-variant" {
		t.Errorf("DB row changed despite failed Save: %+v", row)
	}
}

func TestUpdateModelReloadFailureReportsError(t *testing.T) {
	h, fake, store := newAIUpdateTestEnv(t)
	fake.loadFail = true
	seedBlob(t, store, "h2")
	// Row loaded + runtime serves it: the pre-update unload targets a real
	// registration, and the reload failure is the loadFail knob's doing.
	fake.markLive("brittle")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "brittle", Name: "brittle", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/old.hef", FileHash: "h1",
	})
	w := putUpdate(t, h, "brittle", `{"file_hash":"h2"}`)
	if respCode(t, w) != CodeModelLoadFailed {
		t.Fatalf("code = %d body=%s, want %d (reload failure)", respCode(t, w), w.Body.String(), CodeModelLoadFailed)
	}
	if !strings.Contains(w.Body.String(), "failed to reload") {
		t.Errorf("error must state reload failure explicitly: %s", w.Body.String())
	}
	// The row reflects the swap as uploaded — accurate, not silently loaded.
	row, _ := h.aiModelRepo.GetByModelID("brittle")
	if row.Status != "uploaded" || row.FileHash != "h2" {
		t.Errorf("row after failed reload: status=%q hash=%q, want uploaded/h2", row.Status, row.FileHash)
	}
}

// A row stuck at "loaded" after the runtime restarted solo must not veto the
// update with a phantom unload: the runtime is the authority on "is it
// serving", so a tuning change just updates the row and leaves the reload to
// the self-heal loop (desired_state stays loaded).
func TestUpdateModelStaleLoadedRowSkipsPhantomSwap(t *testing.T) {
	h, fake, _ := newAIUpdateTestEnv(t)
	// No markLive: the runtime holds nothing for this id.
	seedAIModel(t, h, &model.AIModel{
		ModelID: "phantom_det", Name: "phantom_det", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
		DesiredState: "loaded",
	})
	w := putUpdate(t, h, "phantom_det", `{"config":{"threshold":0.6}}`)
	if respCode(t, w) != 0 {
		t.Fatalf("update must succeed despite the stale row: %s", w.Body.String())
	}
	if calls, _ := fake.snapshot(); len(calls) != 0 {
		t.Errorf("phantom unload/reload must not happen, got %v", calls)
	}
	row, _ := h.aiModelRepo.GetByModelID("phantom_det")
	if row.Status != "loaded" || row.DesiredState != "loaded" || row.Threshold != 0.6 {
		t.Errorf("row after update: status=%q desired=%q threshold=%f, want loaded/loaded/0.6",
			row.Status, row.DesiredState, row.Threshold)
	}
}

// The mirror image: a row stuck at "uploaded" (app re-registration race)
// while the runtime still serves the id must swap through unload→reload —
// otherwise the NPU keeps serving the old registration forever.
func TestUpdateModelStaleUploadedRowStillSwaps(t *testing.T) {
	h, fake, store := newAIUpdateTestEnv(t)
	oldRoot := constants.RootPath()
	constants.SetRootPath(t.TempDir())
	t.Cleanup(func() { constants.SetRootPath(oldRoot) })
	// The config-only body keeps the row's FilePath, so seed the real CAS
	// blob path — the reload materializes from FilePath.
	blob := seedBlob(t, store, "h1")
	// Row says uploaded, runtime says serving — the runtime wins.
	fake.markLive("ghost_row")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "ghost_row", Name: "ghost_row", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: blob, FileHash: "h1",
	})
	w := putUpdate(t, h, "ghost_row", `{"config":{"threshold":0.5}}`)
	if respCode(t, w) != 0 {
		t.Fatalf("update failed: %s", w.Body.String())
	}
	calls, _ := fake.snapshot()
	if len(calls) != 2 || calls[0] != "unload:ghost_row" || calls[1] != "load:ghost_row" {
		t.Fatalf("stale-uploaded row must still swap, got %v", calls)
	}
	row, _ := h.aiModelRepo.GetByModelID("ghost_row")
	if row.Status != "loaded" {
		t.Errorf("row must end loaded after the swap, got %q", row.Status)
	}
}

func TestRegisterModelDuplicateRejected(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)
	seedAIModel(t, h, &model.AIModel{ModelID: "taken", Name: "taken", Status: "uploaded", Source: "web"})

	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	body := bytes.NewReader([]byte(`{"model_id":"taken","model_path":"/x.hef"}`))
	c.Request = httptest.NewRequest(http.MethodPost, "/api/v1/ai/models", body)
	c.Request.Header.Set("Content-Type", "application/json")
	h.RegisterModel(c)

	if respCode(t, w) != CodeInvalidRequest {
		t.Fatalf("duplicate register must fail with %d, got %d body=%s", CodeInvalidRequest, respCode(t, w), w.Body.String())
	}
	if !strings.Contains(w.Body.String(), "already exists") {
		t.Errorf("error must mention duplicate: %s", w.Body.String())
	}
}

// A partial custom variant must be rejected at entry with a message naming
// the missing keys — not stored to fail later at load time.
func TestUpdateModelRejectsPartialCustomVariant(t *testing.T) {
	h, fake, _ := newAIUpdateTestEnv(t)
	seedAIModel(t, h, &model.AIModel{
		ModelID: "partial_det", Name: "partial_det", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
	})
	w := putUpdate(t, h, "partial_det", `{"model_variant":"{\"backend_function\":\"hailo_yolov8n\",\"detection_threshold\":0.5}"}`)
	if respCode(t, w) != CodeInvalidRequest {
		t.Fatalf("partial variant must fail with %d, got %d body=%s", CodeInvalidRequest, respCode(t, w), w.Body.String())
	}
	if !strings.Contains(w.Body.String(), "missing required key") {
		t.Errorf("error must name the missing keys: %s", w.Body.String())
	}
	row, _ := h.aiModelRepo.GetByModelID("partial_det")
	if row.Variant != "" {
		t.Errorf("rejected variant must not be stored, got %q", row.Variant)
	}
	if calls, _ := fake.snapshot(); len(calls) != 0 {
		t.Errorf("no runtime calls expected, got %v", calls)
	}
}

// A schema-complete custom variant is stored verbatim — the escape hatch
// stays open, only broken blobs are fenced off.
func TestUpdateModelAcceptsCompleteCustomVariant(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)
	seedAIModel(t, h, &model.AIModel{
		ModelID: "custom_det", Name: "custom_det", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
	})
	custom := `{"backend_function":"hailo_yolov8s","iou_threshold":0.45,"detection_threshold":0.5,"output_activation":"none","label_offset":1,"max_boxes":32,"labels":["fire","smoke"]}`
	body := `{"model_variant":` + strconv.Quote(custom) + `}`
	w := putUpdate(t, h, "custom_det", body)
	if respCode(t, w) != 0 {
		t.Fatalf("complete variant must be accepted: %s", w.Body.String())
	}
	row, _ := h.aiModelRepo.GetByModelID("custom_det")
	if row.Variant != custom {
		t.Errorf("variant must be stored verbatim, got %q", row.Variant)
	}
}

// A typo'd postprocess_profile is rejected at the write boundary on update:
// config changes feed the composed runtime variant, and silently defaulting
// the profile would mismatch the model during the next reload.
func TestUpdateModelRejectsUnknownPostprocessProfile(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)
	seedAIModel(t, h, &model.AIModel{
		ModelID: "live_det", Name: "live_det", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
		DesiredState: "loaded",
	})
	w := putUpdate(t, h, "live_det", `{"config":{"postprocess_profile":"yolov8x_640_640"}}`)
	if respCode(t, w) != CodeInvalidRequest {
		t.Fatalf("unknown profile must fail with %d, got %d body=%s", CodeInvalidRequest, respCode(t, w), w.Body.String())
	}
	if !strings.Contains(w.Body.String(), "postprocess_profile") {
		t.Errorf("error must name the profile key: %s", w.Body.String())
	}
	row, _ := h.aiModelRepo.GetByModelID("live_det")
	if row == nil || row.Config != "" {
		t.Errorf("rejected update must leave row untouched, got Config=%q", row.Config)
	}
}

// RegisterModel applies the same guardrail: a blob pointing at a generic
// (hardcoded-threshold) function never reaches the DB.
func TestRegisterModelRejectsUnsupportedBackendFunction(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)

	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	body := bytes.NewReader([]byte(`{"model_id":"gen_fn","model_path":"/x.hef","model_type":"detection","model_variant":"{\"backend_function\":\"yolov8s\",\"iou_threshold\":0.45,\"detection_threshold\":0.5,\"output_activation\":\"none\",\"label_offset\":1,\"max_boxes\":32,\"labels\":[]}"}`))
	c.Request = httptest.NewRequest(http.MethodPost, "/api/v1/ai/models", body)
	c.Request.Header.Set("Content-Type", "application/json")
	h.RegisterModel(c)

	if respCode(t, w) != CodeInvalidRequest {
		t.Fatalf("generic function must fail with %d, got %d body=%s", CodeInvalidRequest, respCode(t, w), w.Body.String())
	}
	if !strings.Contains(w.Body.String(), "not supported") {
		t.Errorf("error must name the unsupported function: %s", w.Body.String())
	}
	if row, _ := h.aiModelRepo.GetByModelID("gen_fn"); row != nil {
		t.Errorf("rejected model must not be persisted, got %+v", row)
	}
}

// RegisterModel likewise rejects a typo'd postprocess_profile at entry: the
// config feeds the load-time materialization basename, and a silent default
// would mismatch the model to the wrong postprocess function later.
func TestRegisterModelRejectsUnknownPostprocessProfile(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)

	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	body := bytes.NewReader([]byte(`{"model_id":"typo_profile","model_path":"/x.hef","model_type":"detection","config":{"postprocess_profile":"yolov8x_640_640"}}`))
	c.Request = httptest.NewRequest(http.MethodPost, "/api/v1/ai/models", body)
	c.Request.Header.Set("Content-Type", "application/json")
	h.RegisterModel(c)

	if respCode(t, w) != CodeInvalidRequest {
		t.Fatalf("unknown profile must fail with %d, got %d body=%s", CodeInvalidRequest, respCode(t, w), w.Body.String())
	}
	if !strings.Contains(w.Body.String(), "postprocess_profile") {
		t.Errorf("error must name the profile key: %s", w.Body.String())
	}
	if row, _ := h.aiModelRepo.GetByModelID("typo_profile"); row != nil {
		t.Errorf("rejected model must not be persisted, got %+v", row)
	}
}

// RegisterModel enforces the runtime-safe model_id charset at entry: ids
// flow into gRPC registrations and materialized runtime paths.
func TestRegisterModelInvalidID(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)

	cases := []struct {
		name    string
		modelID string
	}{
		{"leading dash", "-det"},
		{"leading dot", ".det"},
		{"space", "my model"},
		{"slash", "../escape"},
		{"chinese", "检测模型"},
		{"too long", strings.Repeat("a", 65)},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			gin.SetMode(gin.TestMode)
			w := httptest.NewRecorder()
			c, _ := gin.CreateTestContext(w)
			body := bytes.NewReader([]byte(`{"model_id":` + strconv.Quote(tc.modelID) + `,"model_path":"/x.hef"}`))
			c.Request = httptest.NewRequest(http.MethodPost, "/api/v1/ai/models", body)
			c.Request.Header.Set("Content-Type", "application/json")
			h.RegisterModel(c)

			if respCode(t, w) != CodeInvalidRequest {
				t.Fatalf("invalid id must fail with %d, got %d body=%s",
					CodeInvalidRequest, respCode(t, w), w.Body.String())
			}
			if !strings.Contains(w.Body.String(), "Invalid model_id") {
				t.Errorf("error must name the charset rule: %s", w.Body.String())
			}
			if row, _ := h.aiModelRepo.GetByModelID(tc.modelID); row != nil {
				t.Errorf("rejected id must not be persisted, got %+v", row)
			}
		})
	}
}

// A well-formed id must clear the charset gate and fail later for its own
// reasons — proving the gate does not over-reject.
func TestRegisterModelValidIDPassesCharsetGate(t *testing.T) {
	h, _, _ := newAIUpdateTestEnv(t)

	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	body := bytes.NewReader([]byte(`{"model_id":"yolov8n.det-1","model_path":"/does/not/exist.hef"}`))
	c.Request = httptest.NewRequest(http.MethodPost, "/api/v1/ai/models", body)
	c.Request.Header.Set("Content-Type", "application/json")
	h.RegisterModel(c)

	if strings.Contains(w.Body.String(), "Invalid model_id") {
		t.Fatalf("valid id must not trip the charset gate: %s", w.Body.String())
	}
}
