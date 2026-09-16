package handlers

import (
	"context"
	"fmt"
	"path/filepath"
	"testing"

	"github.com/glebarez/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"

	"aipc/platform/platform-api/model"
	"aipc/platform/platform-api/repo"

	inferencepb "aipc/platform/ai-runtime/proto"
)

func newAISyncTestHandler(t *testing.T) *APIHandlers {
	t.Helper()
	gdb, err := gorm.Open(sqlite.Open(filepath.Join(t.TempDir(), "ai_sync.db")), &gorm.Config{
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
	return &APIHandlers{aiModelRepo: repo.NewAIModelRepo(gdb)}
}

func TestSyncRuntimeModelsToDBSkipsTransient(t *testing.T) {
	h := newAISyncTestHandler(t)
	runtimeMap := map[string]*inferencepb.ModelInfo{
		"sys_det":     {ModelId: "sys_det", Name: "System Detector", ModelPath: "/data/aipc/models/sys.hef"},
		"bundled_det": {ModelId: "bundled_det", Name: "Bundled Detector", ModelPath: "/data/aipc/app-models/app-x/detector/det.hef", OwnerId: "app-x", Transient: true},
	}

	h.syncRuntimeModelsToDB(context.Background(), runtimeMap, true)

	sysRow, err := h.aiModelRepo.GetByModelID("sys_det")
	if err != nil || sysRow == nil {
		t.Fatalf("sys_det missing from DB after sync (err=%v)", err)
	}
	if sysRow.Status != "loaded" {
		t.Errorf("sys_det status = %q, want loaded", sysRow.Status)
	}

	if row, err := h.aiModelRepo.GetByModelID("bundled_det"); err == nil && row != nil {
		t.Errorf("bundled_det must never get a DB row (model page visibility), got %+v", row)
	}
}

func TestSyncRuntimeModelsToDBTransientDoesNotBlockSubtract(t *testing.T) {
	h := newAISyncTestHandler(t)

	// A DB row that claims to be loaded, while the runtime only holds a
	// transient copy under the same id: the transient entry must not count
	// as "still loaded" membership, so the row flips to uploaded.
	seed := &model.AIModel{ModelID: "shared_id", Name: "Shared", Status: "loaded", Source: "dynamic"}
	if err := h.aiModelRepo.Create(seed); err != nil {
		t.Fatalf("seed: %v", err)
	}
	control := &model.AIModel{ModelID: "control_id", Name: "Control", Status: "loaded", Source: "dynamic"}
	if err := h.aiModelRepo.Create(control); err != nil {
		t.Fatalf("seed control: %v", err)
	}

	runtimeMap := map[string]*inferencepb.ModelInfo{
		"shared_id":  {ModelId: "shared_id", Transient: true},
		"control_id": {ModelId: "control_id"},
	}
	h.syncRuntimeModelsToDB(context.Background(), runtimeMap, true)

	shared, err := h.aiModelRepo.GetByModelID("shared_id")
	if err != nil || shared == nil {
		t.Fatalf("shared_id row missing: %v", err)
	}
	if shared.Status != "uploaded" {
		t.Errorf("shared_id status = %q, want uploaded (transient must not pin loaded state)", shared.Status)
	}

	ctrl, err := h.aiModelRepo.GetByModelID("control_id")
	if err != nil || ctrl == nil {
		t.Fatalf("control_id row missing: %v", err)
	}
	if ctrl.Status != "loaded" {
		t.Errorf("control_id status = %q, want loaded", ctrl.Status)
	}
}

func TestSyncRuntimeModelsToDBMarksMissingAsUploaded(t *testing.T) {
	h := newAISyncTestHandler(t)
	seed := &model.AIModel{ModelID: "gone_id", Name: "Gone", Status: "loaded", Source: "dynamic"}
	if err := h.aiModelRepo.Create(seed); err != nil {
		t.Fatalf("seed: %v", err)
	}

	// Runtime reachable, model absent entirely → uploaded.
	h.syncRuntimeModelsToDB(context.Background(), map[string]*inferencepb.ModelInfo{}, true)

	row, err := h.aiModelRepo.GetByModelID("gone_id")
	if err != nil || row == nil {
		t.Fatalf("gone_id row missing: %v", err)
	}
	if row.Status != "uploaded" {
		t.Errorf("gone_id status = %q, want uploaded", row.Status)
	}

	// Runtime unreachable → loaded state must be preserved.
	row.Status = "loaded"
	if err := h.aiModelRepo.Update(row); err != nil {
		t.Fatalf("re-set loaded: %v", err)
	}
	h.syncRuntimeModelsToDB(context.Background(), nil, false)
	row2, err := h.aiModelRepo.GetByModelID("gone_id")
	if err != nil || row2 == nil {
		t.Fatalf("gone_id row missing: %v", err)
	}
	if row2.Status != "loaded" {
		t.Errorf("gone_id status = %q after unreachable sync, want loaded preserved", row2.Status)
	}
}

func TestSyncRuntimeModelsToDBSkipsOwnerTaggedRegistrations(t *testing.T) {
	h := newAISyncTestHandler(t)
	runtimeMap := map[string]*inferencepb.ModelInfo{
		// Legacy preload registration: owner-tagged, not transient.
		"preloaded": {ModelId: "preloaded", Name: "Preloaded", OwnerId: "app-x"},
		// Dynamic registration: owner-tagged, not transient.
		"dyn_reg": {ModelId: "dyn_reg", Name: "Dynamic", OwnerId: "app-y"},
	}

	h.syncRuntimeModelsToDB(context.Background(), runtimeMap, true)

	for _, id := range []string{"preloaded", "dyn_reg"} {
		if row, err := h.aiModelRepo.GetByModelID(id); err == nil && row != nil {
			t.Errorf("%s must not get a DB row (app-origin registration), got %+v", id, row)
		}
	}
}

func TestSyncRuntimeModelsToDBTreatsSystemOwnerAsDeviceLevel(t *testing.T) {
	h := newAISyncTestHandler(t)
	// The runtime normalizes an omitted owner to "<system>" before storing
	// (grpc_service/model_manager), so this is how ownerless device-level
	// registrations actually arrive. Testing "" alone would miss the bug.
	runtimeMap := map[string]*inferencepb.ModelInfo{
		"runtime_det": {ModelId: "runtime_det", Name: "Runtime Detector", OwnerId: "<system>"},
		"tool_det":    {ModelId: "tool_det", Name: "Tool Detector", OwnerId: ""},
	}

	h.syncRuntimeModelsToDB(context.Background(), runtimeMap, true)

	for _, id := range []string{"runtime_det", "tool_det"} {
		row, err := h.aiModelRepo.GetByModelID(id)
		if err != nil || row == nil {
			t.Fatalf("%s missing from DB after sync — system-owned registrations are device-level (err=%v)", id, err)
		}
		if row.Status != "loaded" {
			t.Errorf("%s status = %q, want loaded", id, row.Status)
		}
	}
}

func TestSyncRuntimeModelsToDBHealsLegacyOwnerStamps(t *testing.T) {
	h := newAISyncTestHandler(t)
	// Disk model mislabeled as app-owned by the legacy backfill.
	seed := &model.AIModel{ModelID: "visdrone", Name: "Visdrone", Status: "loaded", Source: "disk", OwnerAppID: "<system>"}
	if err := h.aiModelRepo.Create(seed); err != nil {
		t.Fatalf("seed: %v", err)
	}

	h.syncRuntimeModelsToDB(context.Background(), map[string]*inferencepb.ModelInfo{}, true)

	row, err := h.aiModelRepo.GetByModelID("visdrone")
	if err != nil || row == nil {
		t.Fatalf("visdrone row missing: %v", err)
	}
	if row.OwnerAppID != "" {
		t.Errorf("visdrone owner = %q, want healed to empty", row.OwnerAppID)
	}
}

func TestSyncRuntimeModelsToDBGCOwnerRowsMissingFromRuntime(t *testing.T) {
	h := newAISyncTestHandler(t)
	// Probe garbage: dynamic rows owned by an app that no longer runs.
	for i := 0; i < 3; i++ {
		id := fmt.Sprintf("probe_%d", i)
		if err := h.aiModelRepo.Create(&model.AIModel{ModelID: id, Name: id, Status: "loaded", Source: "dynamic", OwnerAppID: "probe-app"}); err != nil {
			t.Fatalf("seed %s: %v", id, err)
		}
	}
	// Control rows: device-level, and an owner row still live in runtime.
	if err := h.aiModelRepo.Create(&model.AIModel{ModelID: "device_model", Name: "Device", Status: "loaded", Source: "dynamic"}); err != nil {
		t.Fatalf("seed device_model: %v", err)
	}
	if err := h.aiModelRepo.Create(&model.AIModel{ModelID: "live_app_model", Name: "Live", Status: "loaded", Source: "dynamic", OwnerAppID: "app-x"}); err != nil {
		t.Fatalf("seed live_app_model: %v", err)
	}

	runtimeMap := map[string]*inferencepb.ModelInfo{
		"live_app_model": {ModelId: "live_app_model", OwnerId: "app-x"},
	}
	h.syncRuntimeModelsToDB(context.Background(), runtimeMap, true)

	for i := 0; i < 3; i++ {
		id := fmt.Sprintf("probe_%d", i)
		if row, err := h.aiModelRepo.GetByModelID(id); err == nil && row != nil {
			t.Errorf("probe row %s must be GC'd, still present", id)
		}
	}
	for _, id := range []string{"device_model", "live_app_model"} {
		if row, err := h.aiModelRepo.GetByModelID(id); err != nil || row == nil {
			t.Errorf("%s must survive sync: %v", id, err)
		}
	}
}

func TestSyncRuntimeModelsToDBNoOwnerBackfill(t *testing.T) {
	h := newAISyncTestHandler(t)
	seed := &model.AIModel{ModelID: "sys_det", Name: "Sys", Status: "loaded", Source: "disk"}
	if err := h.aiModelRepo.Create(seed); err != nil {
		t.Fatalf("seed: %v", err)
	}

	// Legacy preload: runtime holds the model with an owner tag — the DB row
	// must NOT be stamped with it.
	runtimeMap := map[string]*inferencepb.ModelInfo{
		"sys_det": {ModelId: "sys_det", OwnerId: "app-x"},
	}
	h.syncRuntimeModelsToDB(context.Background(), runtimeMap, true)

	row, err := h.aiModelRepo.GetByModelID("sys_det")
	if err != nil || row == nil {
		t.Fatalf("sys_det row missing: %v", err)
	}
	if row.OwnerAppID != "" {
		t.Errorf("sys_det owner = %q, want empty (no backfill)", row.OwnerAppID)
	}
}
