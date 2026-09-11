package handlers

// Periodic desired-state self-heal: the recovery consumer for the
// desired_state=loaded promise. desired_state used to be write-only — loads
// stamped "reload me on recovery" but nothing ever read it — so a runtime
// wipe (solo ai-runtime restart, force_unregister_all, crash mid-operation)
// silently stranded every loaded model until someone clicked Load again.
// Instead of teaching ai-runtime about events it cannot act on, this loop
// runs in platform-api: each pass performs the same one-way runtime→DB sync
// the models page performs, then re-registers platform-managed models whose
// desired_state=loaded the runtime no longer holds, through loadModelCore —
// the exact code path the REST load endpoint uses. App-owned rows are
// app-manager's preload responsibility and are skipped; a model whose reload
// keeps failing backs off exponentially so a broken model cannot hammer the
// NPU every tick.

import (
	"context"
	"fmt"
	"sync"
	"time"

	inferencepb "aipc/platform/ai-runtime/proto"
	"aipc/platform/common/events"
	"aipc/platform/common/logger"
	"aipc/platform/modelload"
	"aipc/platform/platform-api/model"
)

const (
	// modelSelfHealInterval is the default tick of the self-heal loop.
	modelSelfHealInterval = time.Minute

	// modelHealTimeout bounds one self-heal reload (composition +
	// registration + probe) so a single slow model cannot stall the pass.
	modelHealTimeout = 30 * time.Second
)

// loadModelRuntime performs only the runtime half of a model load: compose the
// registration, register it, capture tensor dimensions and smoke-test
// postprocess. It never writes the database, which makes it safe for restoring
// an immutable pre-update registration after a staged Save fails.
func loadModelRuntime(ctx context.Context, client inferencepb.InferenceServiceClient, dbModel *model.AIModel) error {
	// Wizard-imported detection models live under sha256 blob names the
	// postprocess plugin cannot match; modelload.RuntimeRegistration materializes
	// them under a recognized basename and composes a schema-valid variant.
	runtimePath, runtimeVariant, grpcModelType, pathErr := modelload.RuntimeRegistration(dbModel)
	if pathErr != nil {
		return fmt.Errorf("Failed to prepare model for runtime: %w", pathErr)
	}

	resp, err := client.RegisterModel(ctx, &inferencepb.ModelRegisterRequest{
		ModelPath:    runtimePath,
		ModelId:      dbModel.ModelID,
		ModelType:    grpcModelType,
		ModelVariant: runtimeVariant,
	})
	if err != nil {
		return fmt.Errorf("Failed to load model on NPU: %w", err)
	}
	if resp.Status != nil && !resp.Status.Success {
		return fmt.Errorf("%s", resp.Status.Message)
	}

	// Tensor info is best effort: runtimes without GetModelInfo leave the
	// supplied model's dimensions unchanged.
	infoModelID := resp.ModelId
	if infoModelID == "" {
		infoModelID = dbModel.ModelID
	}
	modelInfo, infoErr := client.GetModelInfo(ctx, &inferencepb.ModelInfo{ModelId: infoModelID})
	if infoErr != nil {
		modelInfo = nil
	}
	if modelInfo != nil && len(modelInfo.Inputs) > 0 {
		input := modelInfo.Inputs[0]
		switch input.GetLayout() {
		case "NHWC":
			if len(input.Shape) >= 4 {
				dbModel.InputHeight = int(input.Shape[1])
				dbModel.InputWidth = int(input.Shape[2])
			}
		case "NCHW":
			if len(input.Shape) >= 4 {
				dbModel.InputHeight = int(input.Shape[2])
				dbModel.InputWidth = int(input.Shape[3])
			}
		default:
			if len(input.Shape) >= 3 {
				dbModel.InputHeight = int(input.Shape[0])
				dbModel.InputWidth = int(input.Shape[1])
			}
		}
	}

	if model.ResolveModelType(dbModel.ModelType) == "detection" {
		if smokeErr := modelload.RunLoadSmokeTest(ctx, client, dbModel.ModelID, modelInfo); smokeErr != nil {
			rollbackRegistration(client, dbModel.ModelID)
			return fmt.Errorf("postprocess smoke test failed: %w", smokeErr)
		}
	}
	return nil
}

// loadModelCore adds persistence to the shared runtime-only load. Persistence
// remains transactional from the caller's perspective: a failed Save rolls the
// new runtime registration back.
func (h *APIHandlers) loadModelCore(ctx context.Context, client inferencepb.InferenceServiceClient, dbModel *model.AIModel) error {
	if err := loadModelRuntime(ctx, client, dbModel); err != nil {
		return err
	}
	dbModel.Status = "loaded"
	dbModel.DesiredState = "loaded"
	if err := h.aiModelRepo.Update(dbModel); err != nil {
		rollbackRegistration(client, dbModel.ModelID)
		return fmt.Errorf("failed to persist loaded state: %w", err)
	}
	return nil
}

// rollbackRegistration tears down a registration whose load-time smoke test
// failed. It runs on its own deadline: by the time a smoke test fails the
// caller's context may already be exhausted, and a rollback that dies on the
// parent's ctx.Err() would leave the broken registration live on the NPU.
func rollbackRegistration(client inferencepb.InferenceServiceClient, modelID string) {
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if _, err := client.UnregisterModel(ctx, &inferencepb.ModelInfo{ModelId: modelID}); err != nil {
		logger.Warn("Failed to unregister model %s after smoke test failure: %v", modelID, err)
	}
}

// modelHealBackoff tracks consecutive reload failures per model and delays
// the next attempt exponentially (1×, 2×, 4× … capped at 16× the tick
// interval); a success clears the counter. Without it, a model that can
// never load again (deleted blob, incompatible HEF) would retry every tick
// forever.
type modelHealBackoff struct {
	mu      sync.Mutex
	fails   map[string]int
	nextTry map[string]time.Time
}

func newModelHealBackoff() *modelHealBackoff {
	return &modelHealBackoff{fails: map[string]int{}, nextTry: map[string]time.Time{}}
}

// ready reports whether the model's next retry time has passed.
func (b *modelHealBackoff) ready(modelID string, now time.Time) bool {
	b.mu.Lock()
	defer b.mu.Unlock()
	next, ok := b.nextTry[modelID]
	return !ok || !now.Before(next)
}

// fail records a failure and schedules the next attempt after 2^(n-1)× the
// tick interval, capped at 16×.
func (b *modelHealBackoff) fail(modelID string, interval time.Duration, now time.Time) {
	b.mu.Lock()
	defer b.mu.Unlock()
	b.fails[modelID]++
	delay := interval
	for i := 1; i < b.fails[modelID]; i++ {
		delay *= 2
		if delay >= 16*interval {
			delay = 16 * interval
			break
		}
	}
	b.nextTry[modelID] = now.Add(delay)
}

// reset clears the failure state after a successful reload.
func (b *modelHealBackoff) reset(modelID string) {
	b.mu.Lock()
	defer b.mu.Unlock()
	delete(b.fails, modelID)
	delete(b.nextTry, modelID)
}

// DemoteNeverLoadedImports corrects desired_state on rows created before the
// registration paths stopped relying on the column default: import, upload
// and disk-seed used to leave desired_state at its old default "loaded", so
// this loop's restore pass auto-loaded models the user never loaded —
// flipping the models page to "loaded" on its own and pushing every scanned
// disk model onto the NPU. Idempotent; runs once per startup before the heal
// loop's first tick, which is the pass that would otherwise act on the stale
// promise. The rare crash-mid-load row (uploaded + desired loaded) is
// indistinguishable from a never-loaded import and gets demoted too: it then
// takes one manual load, an acceptable trade for not auto-loading everything.
func (h *APIHandlers) DemoteNeverLoadedImports() {
	if h.aiModelRepo == nil {
		return
	}
	n, err := h.aiModelRepo.DemoteNeverLoadedToUnloaded()
	if err != nil {
		logger.Warn("ModelSelfHeal: demoting never-loaded imports failed: %v", err)
		return
	}
	if n > 0 {
		logger.Info("ModelSelfHeal: demoted %d imported-but-never-loaded model(s) to desired_state=unloaded", n)
	}
}

// restoreDesiredLoads re-registers platform-managed models whose
// desired_state=loaded but which the runtime no longer holds. Any runtime
// entry for the id counts as served — an app-owned registration keeping the
// model live is still live, and the platform must not shadow it.
func (h *APIHandlers) restoreDesiredLoads(ctx context.Context, client inferencepb.InferenceServiceClient, runtimeMap map[string]*inferencepb.ModelInfo, backoff *modelHealBackoff, interval time.Duration) {
	rows, err := h.aiModelRepo.List()
	if err != nil {
		logger.Warn("ModelSelfHeal: listing models failed: %v", err)
		return
	}
	now := time.Now()
	for i := range rows {
		dbRow := &rows[i]
		if dbRow.OwnerAppID != "" || dbRow.DesiredState != "loaded" {
			// App-owned rows are app-manager's preload responsibility;
			// anything not desired-loaded is not a restore candidate.
			continue
		}
		if _, stillLoaded := runtimeMap[dbRow.ModelID]; stillLoaded {
			continue
		}
		if dbRow.FilePath == "" {
			// A row without a file can never be loaded by anyone; skip it
			// quietly rather than failing (and backing off) every tick.
			continue
		}
		if !backoff.ready(dbRow.ModelID, now) {
			continue
		}
		loadCtx, cancel := context.WithTimeout(ctx, modelHealTimeout)
		coreErr := h.loadModelCore(loadCtx, client, dbRow)
		cancel()
		if coreErr != nil {
			logger.Warn("ModelSelfHeal: reloading %s failed: %v", dbRow.ModelID, coreErr)
			backoff.fail(dbRow.ModelID, interval, time.Now())
			continue
		}
		backoff.reset(dbRow.ModelID)
		logger.Info("ModelSelfHeal: restored model %s (desired_state=loaded, was missing from runtime)", dbRow.ModelID)
		if h.eventLogger != nil {
			h.eventLogger.LogWithCodeAsync(
				string(events.EventAIModelLoaded),
				events.MessageParams{
					"model_id": dbRow.ModelID,
					"trigger":  "self_heal",
				},
				"", // background action, no requesting user
			)
		}
	}
}

// selfHealPass runs one sync+restore pass. A runtime that cannot be reached
// means an empty pass, not a wipe: rows and backoff stay untouched rather
// than guessing against a dead service.
func (h *APIHandlers) selfHealPass(ctx context.Context, backoff *modelHealBackoff, interval time.Duration) {
	if h.grpcClients.AIRuntime == nil || h.aiModelRepo == nil {
		return
	}
	// The orphan blob sweep rides the heal tick: crashed imports and
	// superseded file swaps leave unreferenced blobs no request path
	// cleans up. It needs no runtime, only store + repo.
	h.sweepOrphanBlobs(modelBlobGCGrace)
	snapCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	runtimeMap, ok := h.listRuntimeModels(snapCtx)
	cancel()
	if !ok {
		return
	}
	client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
	syncCtx, cancel := context.WithTimeout(ctx, 10*time.Second)
	h.syncRuntimeModelsToDB(syncCtx, runtimeMap, true)
	cancel()
	h.restoreDesiredLoads(ctx, client, runtimeMap, backoff, interval)
}

// StartModelSelfHeal runs the periodic self-heal loop until ctx is
// cancelled. A failing pass logs and returns; only cancellation stops the
// loop.
func (h *APIHandlers) StartModelSelfHeal(ctx context.Context, interval time.Duration) {
	if interval <= 0 {
		interval = modelSelfHealInterval
	}
	backoff := newModelHealBackoff()
	ticker := time.NewTicker(interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-ticker.C:
			h.selfHealPass(ctx, backoff, interval)
		}
	}
}
