package handlers

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"google.golang.org/protobuf/types/known/emptypb"
	"gopkg.in/yaml.v3"

	inferencepb "aipc/platform/ai-runtime/proto"
	apppb "aipc/platform/app-manager/proto"
	"aipc/platform/common/constants"
	"aipc/platform/common/events"
	"aipc/platform/common/logger"
	"aipc/platform/modelload"
	platformdb "aipc/platform/platform-api/db"
	"aipc/platform/platform-api/model"
	"aipc/platform/platform-api/storage"
)

// AI Runtime proxy handlers

var (
	// stagedBlobHashRE matches the hex sha256 the parse pipeline stages blobs
	// under — anything else is not a hash this server handed out.
	stagedBlobHashRE = regexp.MustCompile(`^[a-f0-9]{64}$`)

	// modelIDRE bounds device-level model ids to a runtime-safe charset: the
	// id travels into gRPC registrations and filesystem paths, so spaces,
	// slashes and non-ASCII must never reach the store. Mirrored in the web
	// wizard (modelImportFlow.ts).
	modelIDRE = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$`)
)

func (h *APIHandlers) GetCapabilities(c *gin.Context) {
	Resp(c).OK(gin.H{
		"formats":           model.SupportedFormats,
		"model_types":       model.SupportedModelTypes,
		"package_extension": model.PackageExtension,
	})
}

func (h *APIHandlers) ParseModel(c *gin.Context) {
	file, err := c.FormFile("model")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Model file is required: "+err.Error())
		return
	}

	ext := strings.ToLower(filepath.Ext(file.Filename))
	isPackage := ext == model.PackageExtension
	validExt := isPackage
	for _, f := range model.SupportedFormats {
		if f.Extension == ext {
			validExt = true
			break
		}
	}
	if !validExt {
		Resp(c).FailMsg(CodeInvalidRequest, "Unsupported file format. Supported: "+func() string {
			exts := make([]string, 0, len(model.SupportedFormats)+1)
			for _, f := range model.SupportedFormats {
				exts = append(exts, f.Extension)
			}
			exts = append(exts, model.PackageExtension)
			return strings.Join(exts, ", ")
		}())
		return
	}

	var modelPath string
	var fileHash string
	var fileSize int64
	var pkgMeta *storage.PackageMeta
	// Extension of the staged blob — packages always unpack to a .hef blob.
	stagedExt := ext
	// Whether the staged blob predates this request (dedup hit) — a failed
	// parse must not reclaim a blob another model may reference.
	var blobExisted bool

	if h.modelStore != nil {
		src, err := file.Open()
		if err != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to open uploaded file: "+err.Error())
			return
		}
		defer src.Close()

		if isPackage {
			// .bin uploads are AMPK containers: verify the package digest,
			// unpack and stage the inner HEF (blob name = the HEF's own
			// sha256, deduping with plain .hef imports), and hand the wizard
			// a prefill object from the metadata.
			meta, result, err := h.importModelPackage(src, file.Size)
			if err != nil {
				Resp(c).FailMsg(CodeInvalidRequest, "Invalid model package: "+err.Error())
				return
			}
			pkgMeta = meta
			modelPath = result.Path
			fileHash = result.Hash
			fileSize = result.Size
			stagedExt = ".hef"
			blobExisted = result.Existed
		} else {
			result, err := h.modelStore.SaveWithHash(src, ext, file.Size)
			if err != nil {
				Resp(c).FailMsg(CodeServiceError, "Failed to save model: "+err.Error())
				return
			}
			modelPath = result.Path
			fileHash = result.Hash
			fileSize = result.Size
			blobExisted = result.Existed
		}
	} else {
		Resp(c).FailMsg(CodeServiceError, "Model storage not available")
		return
	}

	var vstreamInfoJSON string
	var networkName string
	var inputWidth, inputHeight int
	if h.modelStore != nil {
		jsonStr, hefInfo, err := h.modelStore.ValidateHEFToJSON(modelPath)
		if err != nil {
			// Reference-count-aware cleanup: a deduped upload must not take
			// a live model's blob with it.
			h.cleanupStagedBlob(fileHash, stagedExt, blobExisted)
			Resp(c).FailMsg(CodeInvalidRequest, "Model validation failed: "+err.Error())
			return
		}
		vstreamInfoJSON = jsonStr
		if hefInfo != nil {
			networkName = hefInfo.NetworkName
			inputWidth = hefInfo.InputWidth
			inputHeight = hefInfo.InputHeight
		}
	}

	suggestedType := model.GuessModelType(networkName)

	resp := gin.H{
		"file_hash":      fileHash,
		"file_path":      modelPath,
		"file_size":      fileSize,
		"filename":       file.Filename,
		"network_name":   networkName,
		"input_width":    inputWidth,
		"input_height":   inputHeight,
		"vstream_info":   vstreamInfoJSON,
		"suggested_type": suggestedType,
		"format":         ext,
		// What the HEF actually emits — independent of the semantic model
		// type. Decides whether the platform postprocess can decode it at all.
		"output_format": model.ClassifyOutputFormat(vstreamInfoJSON),
	}
	if pkgMeta != nil {
		resp["package"] = pkgMeta
	}
	Resp(c).OK(resp)
}

// AbandonStagedModel drops a blob staged by ParseModel when the import
// wizard is cancelled. It deliberately is NOT the generic
// files/batch-delete: staging is content-addressed, so an identical upload
// may have deduped onto a blob an existing model already references.
// UnregisterModel's reference-count rule decides here too, and the only
// acceptable path is exactly BlobPath(hash, ext) — never an arbitrary
// filesystem location.
func (h *APIHandlers) AbandonStagedModel(c *gin.Context) {
	var req struct {
		FileHash string `json:"file_hash"`
		FilePath string `json:"file_path"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}
	if !stagedBlobHashRE.MatchString(req.FileHash) {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid file_hash: expected the hex sha256 returned by parse")
		return
	}
	if h.modelStore == nil {
		Resp(c).FailMsg(CodeServiceError, "Model storage not available")
		return
	}
	ext := strings.ToLower(filepath.Ext(req.FilePath))
	if ext == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "file_path must carry the staged blob extension")
		return
	}
	// The equality check doubles as the jail: nothing outside the blob
	// directory — and no path for a different hash — can be removed through
	// this endpoint.
	if filepath.Clean(req.FilePath) != h.modelStore.BlobPath(req.FileHash, ext) {
		Resp(c).FailMsg(CodeInvalidRequest, "file_path does not match the staged blob for file_hash")
		return
	}
	// Same admission-vs-deletion atomicity as the orphan sweep (blobRefMu):
	// without it, a concurrent RegisterModel committing a row for this
	// deduplicated hash between the count and the delete would leave a live
	// model pointing at a removed blob.
	h.blobRefMu.Lock()
	defer h.blobRefMu.Unlock()
	// Fail closed on reference-count errors: a DB hiccup must never turn
	// into deleting a live model's blob.
	if h.aiModelRepo != nil {
		count, err := h.aiModelRepo.CountByFileHash(req.FileHash)
		if err != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to check model references: "+err.Error())
			return
		}
		if count > 0 {
			Resp(c).OK(gin.H{"file_hash": req.FileHash, "deleted": false, "referenced_by": count})
			return
		}
	}
	if err := h.modelStore.Delete(req.FileHash, ext); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "Failed to remove staged blob: "+err.Error())
		return
	}
	Resp(c).OK(gin.H{"file_hash": req.FileHash, "deleted": true})
}

func (h *APIHandlers) ScanModels(c *gin.Context) {
	if h.db == nil {
		Resp(c).FailMsg(CodeUnknownError, "database not available")
		return
	}
	result := platformdb.SeedDiskModels(h.db)
	// Newly seeded rows have no hash either — backfill asynchronously so the
	// scan response stays fast.
	go h.BackfillMissingHashes()
	Resp(c).OK(result)
}

// modelConfigJSON exposes the JSON-text config column as a parsed object.
// Empty or invalid payloads return nil so a corrupt row can never break
// response marshalling (json.RawMessage with invalid JSON would).
func modelConfigJSON(s string) json.RawMessage {
	if s == "" || !json.Valid([]byte(s)) {
		return nil
	}
	return json.RawMessage(s)
}

// ListModels returns all models: DB records (uploaded + loaded) enriched with ai-runtime info.
func (h *APIHandlers) ListModels(c *gin.Context) {
	type EnrichedModel struct {
		ModelId         string                    `json:"model_id"`
		Name            string                    `json:"name"`
		ModelPath       string                    `json:"model_path"`
		Version         string                    `json:"version"`
		Status          string                    `json:"status"`
		Source          string                    `json:"source,omitempty"`
		OwnerAppID      string                    `json:"owner_app_id,omitempty"`
		DesiredState    string                    `json:"desired_state,omitempty"`
		LoadTimestamp   uint64                    `json:"load_timestamp,omitempty"`
		Inputs          []*inferencepb.TensorSpec `json:"inputs,omitempty"`
		Outputs         []*inferencepb.TensorSpec `json:"outputs,omitempty"`
		EstimatedTops   float32                   `json:"estimated_tops,omitempty"`
		EstimatedMemory uint32                    `json:"estimated_memory,omitempty"`
		ModelType       string                    `json:"model_type,omitempty"`
		OutputMode      string                    `json:"output_mode,omitempty"`
		Variant         string                    `json:"variant,omitempty"`
		Threshold       float32                   `json:"threshold,omitempty"`
		MaxDetections   int                       `json:"max_detections,omitempty"`
		Config          json.RawMessage           `json:"config,omitempty"`
		FileSize        int64                     `json:"file_size,omitempty"`
		FileHash        string                    `json:"file_hash,omitempty"`
		NetworkName     string                    `json:"network_name,omitempty"`
		InputWidth      int                       `json:"input_width,omitempty"`
		InputHeight     int                       `json:"input_height,omitempty"`
		UsedByApps      []string                  `json:"used_by_apps,omitempty"`
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	// Collect ai-runtime models (loaded on NPU)
	runtimeMap, runtimeOK := h.listRuntimeModels(ctx)

	// Upsert runtime models into DB so platform-api is the single source of truth
	h.syncRuntimeModelsToDB(ctx, runtimeMap, runtimeOK)

	enrichedModels := make([]EnrichedModel, 0)

	// DB is the single source of truth for model metadata
	if h.aiModelRepo != nil {
		dbModels, err := h.aiModelRepo.List()
		if err == nil {
			for _, db := range dbModels {
				// App-origin rows (dynamic registration / legacy owner
				// backfill) are invisible on the model page — it lists only
				// device-level models. The sync pass heals or GCs them.
				if db.OwnerAppID != "" {
					continue
				}
				em := EnrichedModel{
					ModelId:       db.ModelID,
					Name:          db.Name,
					ModelPath:     db.FilePath,
					Status:        db.Status,
					Source:        db.Source,
					OwnerAppID:    db.OwnerAppID,
					DesiredState:  db.DesiredState,
					ModelType:     db.ModelType,
					OutputMode:    db.OutputMode,
					Variant:       db.Variant,
					Threshold:     db.Threshold,
					MaxDetections: db.MaxDetections,
					Config:        modelConfigJSON(db.Config),
					FileSize:      db.FileSize,
					FileHash:      db.FileHash,
					NetworkName:   db.NetworkName,
					InputWidth:    db.InputWidth,
					InputHeight:   db.InputHeight,
				}
				// Enrich with runtime info if loaded on NPU
				if rt, ok := runtimeMap[db.ModelID]; ok {
					em.Status = "loaded"
					em.LoadTimestamp = rt.LoadTimestamp
					em.Inputs = rt.Inputs
					em.Outputs = rt.Outputs
					em.EstimatedTops = rt.EstimatedTops
					em.EstimatedMemory = rt.EstimatedMemory
					if em.Name == "" {
						em.Name = rt.Name
					}
				}
				// Find apps using this model
				if h.grpcClients.AppManager != nil {
					apps, _ := h.getAppsUsingModel(ctx, db.ModelID)
					em.UsedByApps = apps
				}
				enrichedModels = append(enrichedModels, em)
			}
		}
	}

	Resp(c).OK(gin.H{"models": enrichedModels})
}

// syncRuntimeModelsToDB ensures the DB reflects the actual runtime state.
// This makes platform-api the single source of truth for model metadata.
// App-origin registrations (bundled transient models, owner-tagged dynamic
// registrations and legacy preloads) never get DB rows: the model page lists
// only device-level models.
func (h *APIHandlers) syncRuntimeModelsToDB(ctx context.Context, runtimeMap map[string]*inferencepb.ModelInfo, runtimeOK bool) {
	if h.aiModelRepo == nil {
		return
	}

	// 1. Upsert: runtime models → DB
	for modelID, rt := range runtimeMap {
		// The runtime normalizes an omitted owner to "<system>" before it
		// stores and returns it, so ownerless device-level registrations
		// arrive here as systemOwnerID, never "". Classify through
		// isSystemOwned or runtime-only device models vanish from the
		// model page as pseudo app-origin rows.
		if rt.Transient || !isSystemOwned(rt.OwnerId) {
			// App-bundled and app-registered models serve only the app that
			// shipped them: never persisted, never listed on the model page.
			continue
		}
		existing, _ := h.aiModelRepo.GetByModelID(modelID)
		if existing == nil {
			newModel := &model.AIModel{
				ModelID:      modelID,
				Name:         rt.Name,
				FilePath:     rt.ModelPath,
				Version:      rt.Version,
				Status:       "loaded",
				Source:       "dynamic",
				DesiredState: "loaded",
			}
			if err := h.aiModelRepo.Create(newModel); err != nil {
				logger.Warn("syncRuntimeModelsToDB: failed to create %s: %v", modelID, err)
			}
		} else if existing.Status != "loaded" && existing.DesiredState != "unloaded" {
			existing.Status = "loaded"
			if existing.Source == "" {
				existing.Source = "disk"
			}
			if existing.DesiredState == "" {
				existing.DesiredState = "loaded"
			}
			if err := h.aiModelRepo.Update(existing); err != nil {
				logger.Warn("syncRuntimeModelsToDB: failed to update %s: %v", modelID, err)
			}
		}
	}

	// 2. Subtract: only when runtime was reachable, mark DB models as uploaded
	//    if they disappeared from runtime. Skip when runtime is unreachable
	//    to avoid wiping all loaded states on transient gRPC failures.
	if !runtimeOK {
		return
	}
	dbModels, err := h.aiModelRepo.List()
	if err != nil {
		return
	}

	// 2a. Heal: owner stamps on non-dynamic rows came from the legacy
	// backfill of preloaded disk models — clear them so system models read
	// as device-level again.
	for i := range dbModels {
		db := &dbModels[i]
		if db.OwnerAppID == "" || db.Source == "dynamic" {
			continue
		}
		owner := db.OwnerAppID
		db.OwnerAppID = ""
		if err := h.aiModelRepo.Update(db); err != nil {
			logger.Warn("syncRuntimeModelsToDB: failed to heal owner on %s: %v", db.ModelID, err)
		} else {
			logger.Info("syncRuntimeModelsToDB: healed legacy owner %q on %s", owner, db.ModelID)
		}
	}

	for i := range dbModels {
		db := &dbModels[i]
		// 2b. GC app-origin rows: they are page-invisible by design, so once
		// the app's model is gone from runtime the row is garbage (stopped/
		// uninstalled apps, probe leftovers).
		if db.OwnerAppID != "" {
			if rt, inRuntime := runtimeMap[db.ModelID]; inRuntime && !rt.Transient {
				continue
			}
			if err := h.aiModelRepo.DeleteByModelID(db.ModelID); err != nil {
				logger.Warn("syncRuntimeModelsToDB: failed to GC %s (owner %s): %v", db.ModelID, db.OwnerAppID, err)
			} else {
				logger.Info("syncRuntimeModelsToDB: GC'd app-origin row %s (owner %s)", db.ModelID, db.OwnerAppID)
			}
			continue
		}
		if db.Status != "loaded" {
			continue
		}
		if rt, inRuntime := runtimeMap[db.ModelID]; inRuntime && !rt.Transient {
			continue
		}
		// Preserve DesiredState: if user intentionally loaded this model,
		// keep desired_state=loaded so the system can re-register on recovery.
		db.Status = "uploaded"
		if db.DesiredState == "loaded" {
			// Model was intentionally loaded but runtime lost it —
			// keep desired_state so auto-reload can attempt recovery.
		} else {
			db.DesiredState = ""
		}
		if updateErr := h.aiModelRepo.Update(db); updateErr != nil {
			logger.Warn("syncRuntimeModelsToDB: failed to mark %s as uploaded: %v", db.ModelID, updateErr)
		} else {
			logger.Info("syncRuntimeModelsToDB: %s no longer in runtime, status → uploaded", db.ModelID)
		}
	}
}

func (h *APIHandlers) GetModelInfo(c *gin.Context) {
	if h.grpcClients.AIRuntime == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "AI Runtime not available")
		return
	}

	modelID := c.Param("model_id")
	if modelID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Model ID is required")
		return
	}

	// Try DB first
	if h.aiModelRepo != nil {
		dbModel, err := h.aiModelRepo.GetByModelID(modelID)
		if err == nil && dbModel != nil {
			result := gin.H{
				"model_id":       dbModel.ModelID,
				"name":           dbModel.Name,
				"model_path":     dbModel.FilePath,
				"status":         dbModel.Status,
				"model_type":     dbModel.ModelType,
				"output_mode":    dbModel.OutputMode,
				"output_format":  model.ClassifyOutputFormat(dbModel.VStreamInfo),
				"variant":        dbModel.Variant,
				"threshold":      dbModel.Threshold,
				"max_detections": dbModel.MaxDetections,
				"file_size":      dbModel.FileSize,
				"file_hash":      dbModel.FileHash,
				"network_name":   dbModel.NetworkName,
				"input_width":    dbModel.InputWidth,
				"input_height":   dbModel.InputHeight,
			}
			// Expose registration-time config (labels, nms_threshold, ...)
			// only when the stored JSON is parseable; corrupt rows stay hidden.
			if cfg := modelConfigJSON(dbModel.Config); cfg != nil {
				result["config"] = cfg
			}
			// Supplement with runtime info if loaded
			client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
			ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
			defer cancel()
			resp, err := client.ListModels(ctx, &inferencepb.Empty{})
			if err == nil {
				for _, m := range resp.Models {
					if m.ModelId == modelID {
						result["load_timestamp"] = m.LoadTimestamp
						result["inputs"] = m.Inputs
						result["outputs"] = m.Outputs
						result["estimated_tops"] = m.EstimatedTops
						result["estimated_memory"] = m.EstimatedMemory
						break
					}
				}
			}
			Resp(c).OK(result)
			return
		}
	}

	// Fallback: check ai-runtime for system preloaded models
	client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.ListModels(ctx, &inferencepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	for _, m := range resp.Models {
		if m.ModelId == modelID {
			result := gin.H{
				"model_id":         m.ModelId,
				"name":             m.Name,
				"model_path":       m.ModelPath,
				"version":          m.Version,
				"status":           "loaded",
				"load_timestamp":   m.LoadTimestamp,
				"inputs":           m.Inputs,
				"outputs":          m.Outputs,
				"estimated_tops":   m.EstimatedTops,
				"estimated_memory": m.EstimatedMemory,
			}
			Resp(c).OK(result)
			return
		}
	}

	Resp(c).FailMsg(CodeNotFound, "Model not found")
}

func (h *APIHandlers) GetAIStats(c *gin.Context) {
	if h.grpcClients.AIRuntime == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "AI Runtime not available")
		return
	}

	client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	stats, err := client.GetStats(ctx, &inferencepb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(stats)
}

// RegisterModel saves model metadata to DB without loading to NPU.
func (h *APIHandlers) RegisterModel(c *gin.Context) {
	var req struct {
		FileHash    string                 `json:"file_hash"`
		ModelPath   string                 `json:"model_path"`
		ModelID     string                 `json:"model_id"`
		ModelType   string                 `json:"model_type"`
		OutputMode  string                 `json:"output_mode"`
		Variant     string                 `json:"model_variant"`
		Config      map[string]interface{} `json:"config"`
		FileSize    int64                  `json:"file_size"`
		NetworkName string                 `json:"network_name"`
		VStreamInfo string                 `json:"vstream_info"`
		InputWidth  int                    `json:"input_width"`
		InputHeight int                    `json:"input_height"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	var modelPath string
	var fileHash string

	if req.FileHash != "" && h.modelStore != nil {
		ext := ".hef"
		if !h.modelStore.Exists(req.FileHash, ext) {
			Resp(c).FailMsg(CodeInvalidRequest, "Model file not found. Please re-parse the model first.")
			return
		}
		modelPath = h.modelStore.BlobPath(req.FileHash, ext)
		fileHash = req.FileHash
	} else if req.ModelPath != "" {
		modelPath = req.ModelPath
	} else {
		Resp(c).FailMsg(CodeInvalidRequest, "file_hash or model_path is required")
		return
	}

	if req.ModelID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "model_id is required")
		return
	}
	// The id flows into gRPC registrations and materialized runtime paths;
	// reject unsafe charsets here rather than letting load time fail opaquely.
	if !modelIDRE.MatchString(req.ModelID) {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid model_id: must start with a letter or digit and contain only letters, digits, '.', '_' or '-' (max 64 chars)")
		return
	}

	if req.ModelType != "" {
		resolved := model.ResolveModelType(req.ModelType)
		if resolved == "" {
			Resp(c).FailMsg(CodeInvalidRequest, "Unsupported model_type: "+req.ModelType)
			return
		}
		req.ModelType = resolved
	}

	// Output delivery mode is orthogonal to the semantic type; reject unknown
	// values outright instead of silently coercing (empty → platform).
	outputMode, ok := model.ResolveOutputMode(req.OutputMode)
	if !ok {
		Resp(c).FailMsg(CodeInvalidRequest, "Unsupported output_mode: "+req.OutputMode+" (supported: platform, raw)")
		return
	}
	if err := validateOutputModeForModel(outputMode, req.ModelType, req.VStreamInfo); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	// Same custom-variant guardrail as UpdateModel: a stored blob that the
	// plugin will reject makes the model unloadable from the moment it is
	// registered, so validate at entry.
	if model.ResolveModelType(req.ModelType) == "detection" {
		if err := validateDetectionVariant(req.Variant); err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, "Invalid model_variant: "+err.Error())
			return
		}
	}

	if h.aiModelRepo != nil {
		if existing, _ := h.aiModelRepo.GetByModelID(req.ModelID); existing != nil {
			Resp(c).FailMsg(CodeInvalidRequest, "Model ID already exists: "+req.ModelID+" (use update instead)")
			return
		}
	}

	// Save to DB as "uploaded" — not loaded to NPU yet
	if h.aiModelRepo != nil {
		defaults := model.GetFieldDefaults(req.ModelType)
		merged := make(map[string]interface{})
		for k, v := range defaults {
			merged[k] = v
		}
		for k, v := range req.Config {
			merged[k] = v
		}
		if err := validatePostprocessProfile(req.ModelType, outputMode, merged); err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, err.Error())
			return
		}
		configJSON, _ := json.Marshal(merged)

		var threshold float32
		if v, ok := merged["threshold"].(float64); ok {
			threshold = float32(v)
		}
		var maxDet int
		if v, ok := merged["max_detections"].(float64); ok {
			maxDet = int(v)
		}

		dbModel := &model.AIModel{
			ModelID:       req.ModelID,
			Name:          req.ModelID,
			FilePath:      modelPath,
			FileHash:      fileHash,
			FileSize:      req.FileSize,
			ModelType:     req.ModelType,
			OutputMode:    outputMode,
			Variant:       strings.TrimSpace(req.Variant),
			Threshold:     threshold,
			MaxDetections: maxDet,
			NetworkName:   req.NetworkName,
			VStreamInfo:   req.VStreamInfo,
			InputWidth:    req.InputWidth,
			InputHeight:   req.InputHeight,
			Config:        string(configJSON),
			Status:        "uploaded",
			// Explicit "unloaded": registration is not a load promise, and
			// desired_state=loaded would have the self-heal loop auto-load
			// this model within a minute.
			DesiredState: "unloaded",
		}
		// Admission commit: re-assert the staged blob and create the row
		// atomically with the orphan sweep (blobRefMu). The Exists check at
		// the top ran before validations; an aged staged blob could have
		// been collected in between, which would leave this row pointing
		// at a deleted file.
		h.blobRefMu.Lock()
		if fileHash != "" && h.modelStore != nil && !h.modelStore.Exists(fileHash, ".hef") {
			h.blobRefMu.Unlock()
			Resp(c).FailMsg(CodeInvalidRequest, "Model file not found. Please re-parse the model first.")
			return
		}
		var createErr error
		if model.ResolveModelType(dbModel.ModelType) == "detection" && dbModel.Threshold == 0 {
			createErr = h.aiModelRepo.CreatePreservingZeroThreshold(dbModel)
		} else {
			createErr = h.aiModelRepo.Create(dbModel)
		}
		h.blobRefMu.Unlock()
		if createErr != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to persist model to DB: "+createErr.Error())
			return
		}
	}

	Resp(c).OK(gin.H{
		"model_id":    req.ModelID,
		"model_path":  modelPath,
		"status":      "uploaded",
		"output_mode": outputMode,
	})

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"ai.model.uploaded",
			events.MessageParams{
				"model_id": req.ModelID,
				"size":     req.FileSize,
			},
			getUsernameFromContext(c),
		)
	}
}

// UpdateModel updates an existing device-level model: metadata always, the
// underlying file optionally (file_hash from a prior parse). A loaded model
// whose file or inference-relevant settings changed is reloaded on the NPU.
func (h *APIHandlers) UpdateModel(c *gin.Context) {
	modelID := c.Param("model_id")
	if modelID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Model ID is required")
		return
	}
	if h.aiModelRepo == nil {
		Resp(c).FailMsg(CodeServiceError, "Model repository not available")
		return
	}

	// Pointer fields distinguish "absent" (keep current) from zero values —
	// the update applies exactly what the body carries.
	var req struct {
		FileHash    string                 `json:"file_hash"`
		ModelType   string                 `json:"model_type"`
		OutputMode  *string                `json:"output_mode"`
		Variant     *string                `json:"model_variant"`
		Config      map[string]interface{} `json:"config"`
		FileSize    *int64                 `json:"file_size"`
		NetworkName *string                `json:"network_name"`
		VStreamInfo *string                `json:"vstream_info"`
		InputWidth  *int                   `json:"input_width"`
		InputHeight *int                   `json:"input_height"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	dbModel, err := h.aiModelRepo.GetByModelID(modelID)
	if err != nil || dbModel == nil || dbModel.OwnerAppID != "" {
		// App-origin rows are not device-level models — same as not found.
		Resp(c).FailMsg(CodeNotFound, "Model not found")
		return
	}
	// Keep an immutable pre-update value for runtime compensation. dbModel is
	// later overwritten with staged fields, so retaining only its pointer would
	// restore the new path/variant after a failed Save.
	original := *dbModel

	newModelType := dbModel.ModelType
	if req.ModelType != "" {
		resolved := model.ResolveModelType(req.ModelType)
		if resolved == "" {
			Resp(c).FailMsg(CodeInvalidRequest, "Unsupported model_type: "+req.ModelType)
			return
		}
		newModelType = resolved
	}

	newOutputMode := dbModel.OutputMode
	if req.OutputMode != nil {
		mode, ok := model.ResolveOutputMode(*req.OutputMode)
		if !ok {
			Resp(c).FailMsg(CodeInvalidRequest, "Unsupported output_mode: "+*req.OutputMode+" (supported: platform, raw)")
			return
		}
		newOutputMode = mode
	}
	// Cross-check against the post-update vstream info when the swap carries
	// new metadata; otherwise the stored row's view decides.
	checkVStream := dbModel.VStreamInfo
	if req.VStreamInfo != nil {
		checkVStream = *req.VStreamInfo
	}
	if err := validateOutputModeForModel(newOutputMode, newModelType, checkVStream); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	// Custom `{…}` variant JSON is the advanced escape hatch into the vendor
	// plugin's schema — reject incomplete or unsupported blobs here rather
	// than at load time, where the plugin answers with a bare "required".
	if req.Variant != nil && model.ResolveModelType(newModelType) == "detection" {
		if err := validateDetectionVariant(*req.Variant); err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, "Invalid model_variant: "+err.Error())
			return
		}
	}

	// Optional file swap: an empty file_hash is a metadata-only update.
	newPath, newHash := dbModel.FilePath, dbModel.FileHash
	fileChanged := false
	if req.FileHash != "" {
		if h.modelStore == nil {
			Resp(c).FailMsg(CodeServiceError, "Model storage not available")
			return
		}
		ext := ".hef"
		if !h.modelStore.Exists(req.FileHash, ext) {
			Resp(c).FailMsg(CodeInvalidRequest, "Model file not found. Please re-parse the model first.")
			return
		}
		newPath = h.modelStore.BlobPath(req.FileHash, ext)
		fileChanged = req.FileHash != dbModel.FileHash
		newHash = req.FileHash
	}
	// Stage the post-update row before deciding on a reload. Detection
	// tuning (threshold / max_detections / nms_threshold /
	// postprocess_profile) lives in the merged config and feeds the
	// composed runtime variant, so config-only edits on a loaded model must
	// reload too — a bare row update used to leave the NPU serving the old
	// threshold until someone unloaded by hand.
	staged := *dbModel
	staged.FilePath = newPath
	staged.FileHash = newHash
	staged.ModelType = newModelType
	staged.OutputMode = newOutputMode
	if req.Variant != nil {
		// Store what validation saw: it trims before parsing, and the
		// runtime's variant parsers require the leading '{' — an accepted
		// but untrimmed blob would fail (or misparse) at load time.
		staged.Variant = strings.TrimSpace(*req.Variant)
	}
	if req.FileSize != nil {
		staged.FileSize = *req.FileSize
	}
	if req.NetworkName != nil {
		staged.NetworkName = *req.NetworkName
	}
	if req.VStreamInfo != nil {
		staged.VStreamInfo = *req.VStreamInfo
	}
	if req.InputWidth != nil {
		staged.InputWidth = *req.InputWidth
	}
	if req.InputHeight != nil {
		staged.InputHeight = *req.InputHeight
	}
	if req.Config != nil {
		merged := make(map[string]interface{})
		for k, v := range model.GetFieldDefaults(newModelType) {
			merged[k] = v
		}
		for k, v := range req.Config {
			merged[k] = v
		}
		if err := validatePostprocessProfile(newModelType, newOutputMode, merged); err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, err.Error())
			return
		}
		configJSON, _ := json.Marshal(merged)
		staged.Config = string(configJSON)

		var threshold float32
		if v, ok := merged["threshold"].(float64); ok {
			threshold = float32(v)
		}
		var maxDet int
		if v, ok := merged["max_detections"].(float64); ok {
			maxDet = int(v)
		}
		staged.Threshold = threshold
		staged.MaxDetections = maxDet
	}

	// Reload when anything the runtime consumes changes: the file, the type,
	// the output mode (platform ⇄ raw rewrites the whole gRPC payload), or
	// the variant the runtime would receive. detectionVariantJSON is
	// side-effect-free (the materialized runtime path is only touched on an
	// actual reload), so comparing it here cannot leave stray runtime/
	// copies behind for models that stay put. Raw mode sends neither type
	// nor variant, so variant edits under raw mode don't reach the runtime
	// and don't warrant a reload. Advanced `{…}` variants pass through
	// verbatim, so for those the config-vs-JSON split is explicit: no
	// variant change, no reload.
	oldVariant, newVariant := dbModel.Variant, staged.Variant
	needsReload := fileChanged || newModelType != dbModel.ModelType ||
		newOutputMode != dbModel.OutputMode
	if newOutputMode == model.OutputModePlatform && model.ResolveModelType(newModelType) == "detection" {
		oldVariant, oldErr := modelload.DetectionVariantJSON(dbModel)
		newVariant, newErr := modelload.DetectionVariantJSON(&staged)
		// A variant that can no longer be composed (e.g. the row's
		// postprocess_profile names an unknown profile) forces a reload
		// instead of being treated as "unchanged": the reload fails fast with
		// the audit-visible error, or succeeds when this very update repairs
		// the config. Without this a broken row would keep the NPU serving
		// the stale registration while the DB diverges from it.
		needsReload = needsReload || oldErr != nil || newErr != nil || newVariant != oldVariant
	} else {
		needsReload = needsReload || newVariant != oldVariant
	}
	wasLoaded := dbModel.Status == "loaded"
	if needsReload && h.grpcClients.AIRuntime != nil {
		client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		// Trust the runtime over the row for "is it serving": a stale "loaded"
		// (runtime restarted solo) makes the pre-update unload fail and veto
		// the whole update; a stale "uploaded" skips the reload and leaves the
		// NPU serving the old registration. An unreachable runtime keeps the
		// row's word.
		if rt, checked := runtimeHasModel(ctx, client, modelID); checked {
			wasLoaded = rt != nil
		}
	}

	// A loaded model whose runtime view changes must be unloaded first so
	// the NPU never serves stale weights under the new row. Record actual
	// successful teardown rather than inferring it later from wasLoaded: only
	// that state requires compensation if the staged DB Save fails.
	didUnload := false
	if wasLoaded && needsReload && h.grpcClients.AIRuntime != nil {
		client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		// Same occupancy guard as UnloadModel: the swap tears the runtime
		// registration down while apps may be mid-inference, and a failed
		// reload would leave their model missing entirely.
		if h.grpcClients.AppManager != nil {
			if apps, _ := h.getAppsUsingModel(ctx, modelID, true); len(apps) > 0 {
				Resp(c).FailMsg(CodeOperationFailed, "Model is in use by apps, please stop them first: "+strings.Join(apps, ", "))
				return
			}
		}
		// The runtime reports logical failures (a racing session kept the
		// model in use, a co-owner kept the registration) as OK transport
		// status with success=false — proceeding anyway would commit the
		// new row while the NPU keeps serving the old weights under the
		// same ID, so the response status gates the update too.
		unloadStatus, err := client.UnregisterModel(ctx, &inferencepb.ModelInfo{ModelId: modelID})
		if err != nil {
			Resp(c).FailMsg(CodeOperationFailed, "Failed to unload model before update: "+err.Error())
			return
		}
		if unloadStatus != nil && !unloadStatus.Success {
			Resp(c).FailMsg(CodeOperationFailed,
				"Failed to unload model before update: "+unloadStatus.Message)
			return
		}
		didUnload = true
	}

	// Commit the staged row — fields were staged above so the reload
	// decision could compare against the post-update view.
	*dbModel = staged

	if wasLoaded && needsReload {
		dbModel.Status = "uploaded"
		dbModel.DesiredState = "loaded"
	}
	// Admission commit under blobRefMu: re-assert the referenced blob and
	// persist the row atomically with the orphan sweep — the Exists check
	// near the top ran before the unload gate, and an aged deduped blob
	// could otherwise be collected in between. Scoped to file_hash updates:
	// a row keeping its old reference (metadata-only, model_path, disk
	// models without a CAS blob) has nothing new to re-assert.
	h.blobRefMu.Lock()
	if req.FileHash != "" && h.modelStore != nil && !h.modelStore.Exists(req.FileHash, ".hef") {
		h.blobRefMu.Unlock()
		Resp(c).FailMsg(CodeInvalidRequest, "Model file not found. Please re-parse the model first.")
		return
	}
	updateErr := h.aiModelRepo.Update(dbModel)
	h.blobRefMu.Unlock()
	if updateErr != nil {
		persistMsg := "Failed to persist model update: " + updateErr.Error()
		if didUnload {
			// Never perform gRPC while holding blobRefMu. The DB row is still the
			// immutable original because Save failed, so restore exactly that
			// runtime registration under an independent deadline.
			client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
			restoreCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
			restoreErr := loadModelRuntime(restoreCtx, client, &original)
			cancel()
			if restoreErr != nil {
				Resp(c).FailMsg(CodeServiceError, persistMsg+"; failed to restore original model on NPU: "+humanizeLoadError(restoreErr))
				return
			}
		}
		// Compensation does not turn a failed persistence operation into success.
		Resp(c).FailMsg(CodeServiceError, persistMsg)
		return
	}

	// Reload the swapped model so a previously loaded one stays in service.
	if wasLoaded && needsReload && h.grpcClients.AIRuntime != nil {
		client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
		ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
		defer cancel()
		loadErr := loadModelRuntime(ctx, client, dbModel)
		if loadErr != nil {
			// The row already reflects the new file as uploaded — report the
			// reload failure explicitly instead of claiming success.
			Resp(c).FailMsg(CodeModelLoadFailed, "Model updated but failed to reload on NPU: "+humanizeLoadError(loadErr))
			return
		}
		dbModel.Status = "loaded"
		if err := h.aiModelRepo.Update(dbModel); err != nil {
			logger.Warn("Failed to update model status to loaded: %v", err)
		}
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"ai.model.updated",
			events.MessageParams{
				"model_id": modelID,
				"size":     dbModel.FileSize,
			},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{
		"model_id": dbModel.ModelID,
		"status":   dbModel.Status,
	})
}

// UploadModel saves a model file to storage without loading to NPU (legacy endpoint).
func (h *APIHandlers) UploadModel(c *gin.Context) {
	file, err := c.FormFile("model")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Model file is required: "+err.Error())
		return
	}

	ext := strings.ToLower(filepath.Ext(file.Filename))
	if ext != ".hef" {
		Resp(c).FailMsg(CodeInvalidRequest, "Unsupported file type. Only .hef format is supported")
		return
	}

	modelID := c.PostForm("model_id")
	if modelID == "" {
		modelID = strings.TrimSuffix(file.Filename, ext)
	}
	// Same charset contract as RegisterModel: the id flows into gRPC
	// registrations, materialized runtime paths and (in the no-store
	// fallback below) a file name.
	if !modelIDRE.MatchString(modelID) {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid model_id: must start with a letter or digit and contain only letters, digits, '.', '_' or '-' (max 64 chars)")
		return
	}

	modelType := c.PostForm("model_type")
	if modelType != "" {
		resolved := model.ResolveModelType(modelType)
		if resolved == "" {
			Resp(c).FailMsg(CodeInvalidRequest, "Unsupported model_type: "+modelType)
			return
		}
		modelType = resolved
	}
	variant := c.PostForm("variant")
	if modelType == "detection" {
		if err := validateDetectionVariant(variant); err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, "Invalid variant: "+err.Error())
			return
		}
	}
	thresholdStr := c.PostForm("threshold")
	threshold := float32(0.25)
	if thresholdStr != "" {
		v, err := strconv.ParseFloat(thresholdStr, 32)
		if err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, "Invalid threshold: must be a number, got "+thresholdStr)
			return
		}
		threshold = float32(v)
	}
	maxDetStr := c.PostForm("max_detections")
	maxDetections := 64
	if maxDetStr != "" {
		v, err := strconv.Atoi(maxDetStr)
		if err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, "Invalid max_detections: must be an integer, got "+maxDetStr)
			return
		}
		maxDetections = v
	}

	// Cheap duplicate rejection before any bytes are staged — same contract
	// as RegisterModel.
	if h.aiModelRepo != nil {
		if existing, _ := h.aiModelRepo.GetByModelID(modelID); existing != nil {
			Resp(c).FailMsg(CodeInvalidRequest, "Model ID already exists: "+modelID+" (use update instead)")
			return
		}
	}

	var modelPath string
	var fileHash string
	var fileSize int64
	var blobExisted bool

	if h.modelStore != nil {
		src, err := file.Open()
		if err != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to open uploaded file: "+err.Error())
			return
		}
		defer src.Close()

		result, err := h.modelStore.SaveWithHash(src, ext, file.Size)
		if err != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to save model: "+err.Error())
			return
		}
		modelPath = result.Path
		fileHash = result.Hash
		fileSize = result.Size
		blobExisted = result.Existed
		if result.Existed {
			logger.Info("Model blob already exists (dedup): %s", fileHash)
		}
	} else {
		storagePath := h.modelStorage
		if storagePath == "" {
			storagePath = constants.ModelsPath()
		}
		if err := os.MkdirAll(storagePath, 0755); err != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to create model directory: "+err.Error())
			return
		}
		filename := modelID + ext
		modelPath = filepath.Join(storagePath, filename)
		if err := c.SaveUploadedFile(file, modelPath); err != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to save model file: "+err.Error())
			return
		}
		fileSize = file.Size
	}

	var vstreamInfoJSON string
	var networkName string
	var inputWidth, inputHeight int
	if h.modelStore != nil {
		jsonStr, hefInfo, err := h.modelStore.ValidateHEFToJSON(modelPath)
		if err != nil {
			// Reference-count-aware cleanup: a deduped upload must not take
			// a live model's blob with it.
			h.cleanupStagedBlob(fileHash, ext, blobExisted)
			Resp(c).FailMsg(CodeInvalidRequest, "HEF validation failed: "+err.Error())
			return
		}
		vstreamInfoJSON = jsonStr
		if hefInfo != nil {
			networkName = hefInfo.NetworkName
			inputWidth = hefInfo.InputWidth
			inputHeight = hefInfo.InputHeight
		}
	}

	// Detection uploads persist a schema-default config like RegisterModel
	// does. The config's threshold key is what lets the composed runtime
	// variant tell an explicit threshold: 0 (retain every detection — a
	// legal schema value) apart from a legacy row's never-set column.
	var configJSON string
	if model.ResolveModelType(modelType) == "detection" {
		merged := model.GetFieldDefaults("detection")
		if merged != nil {
			merged["threshold"] = threshold
			merged["max_detections"] = maxDetections
			if blob, err := json.Marshal(merged); err == nil {

				configJSON = string(blob)
			}
		}
	}

	// Save to DB as "uploaded" — not loaded to NPU yet
	if h.aiModelRepo != nil {
		dbModel := &model.AIModel{
			ModelID:       modelID,
			Name:          file.Filename,
			FilePath:      modelPath,
			FileSize:      fileSize,
			FileHash:      fileHash,
			ModelType:     modelType,
			Variant:       strings.TrimSpace(variant),
			Threshold:     threshold,
			MaxDetections: maxDetections,
			Config:        configJSON,
			VStreamInfo:   vstreamInfoJSON,
			NetworkName:   networkName,
			InputWidth:    inputWidth,
			InputHeight:   inputHeight,
			Status:        "uploaded",
			// Same as RegisterModel: upload is not a load promise — an
			// implicit desired_state=loaded would auto-load via self-heal.
			DesiredState: "unloaded",
		}
		// Save to DB as "uploaded" — not loaded to NPU yet. A row that
		// fails to persist is an explicit error, never a silent success
		// that leaves an unreachable model behind. The CAS case commits
		// under blobRefMu with the blob re-asserted: a dedup hit landed on
		// an aged blob the orphan sweep may have collected during HEF
		// validation, and committing then would reference a deleted file.
		h.blobRefMu.Lock()
		if h.modelStore != nil && fileHash != "" && !h.modelStore.Exists(fileHash, ext) {
			h.blobRefMu.Unlock()
			h.cleanupStagedBlob(fileHash, ext, blobExisted)
			Resp(c).FailMsg(CodeInvalidRequest, "Model file vanished during upload (reclaimed by storage cleanup); please retry")
			return
		}
		var createErr error
		if model.ResolveModelType(dbModel.ModelType) == "detection" && dbModel.Threshold == 0 {
			createErr = h.aiModelRepo.CreatePreservingZeroThreshold(dbModel)
		} else {
			createErr = h.aiModelRepo.Create(dbModel)
		}
		h.blobRefMu.Unlock()
		if createErr != nil {
			h.cleanupStagedBlob(fileHash, ext, blobExisted)
			if h.modelStore == nil && modelPath != "" {
				os.Remove(modelPath)
			}
			Resp(c).FailMsg(CodeServiceError, "Failed to save model record: "+createErr.Error())
			return
		}
	}

	Resp(c).OK(gin.H{
		"model_id":     modelID,
		"model_path":   modelPath,
		"filename":     file.Filename,
		"size":         fileSize,
		"file_hash":    fileHash,
		"network_name": networkName,
		"vstream_info": vstreamInfoJSON,
		"status":       "uploaded",
	})

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"ai.model.uploaded",
			events.MessageParams{
				"model_id":     modelID,
				"filename":     file.Filename,
				"size":         fileSize,
				"network_name": networkName,
			},
			getUsernameFromContext(c),
		)
	}
}

// runtimeHasModel reports whether the runtime currently serves modelID.
// checked is false when the runtime was unreachable — callers must keep
// their unreachable-path behavior instead of guessing from a missing entry.
// runtimeHasModel returns the runtime's current entry for modelID, or nil
// when it is not registered. checked is false when the runtime itself was
// unreachable, so callers can distinguish "absent" from "unknown".
// systemOwnerID is the literal ai-runtime substitutes for an empty owner at
// registration time (grpc_service.cpp: "if (owner_id.empty()) owner_id =
// \"<system>\""), so ownerless entries surface in ListModels with this string
// rather than "".
const systemOwnerID = "<system>"

// isSystemOwned reports whether a runtime entry's owner marks it as
// device-level rather than app-owned. Both spellings occur: "" from local
// tooling, systemOwnerID from the runtime's own normalization.
func isSystemOwned(owner string) bool {
	return owner == "" || owner == systemOwnerID
}

func runtimeHasModel(ctx context.Context, client inferencepb.InferenceServiceClient, modelID string) (rt *inferencepb.ModelInfo, checked bool) {
	resp, err := client.ListModels(ctx, &inferencepb.Empty{})
	if err != nil {
		return nil, false
	}
	for _, m := range resp.Models {
		if m.ModelId == modelID {
			return m, true
		}
	}
	return nil, true
}

// listRuntimeModels snapshots the runtime's model table. ok is false when
// the runtime is unreachable, so reconciliation passes can distinguish
// "empty" from "unknown".
func (h *APIHandlers) listRuntimeModels(ctx context.Context) (map[string]*inferencepb.ModelInfo, bool) {
	if h.grpcClients.AIRuntime == nil {
		return nil, false
	}
	client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
	resp, err := client.ListModels(ctx, &inferencepb.Empty{})
	if err != nil {
		return nil, false
	}
	models := make(map[string]*inferencepb.ModelInfo, len(resp.Models))
	for _, m := range resp.Models {
		models[m.ModelId] = m
	}
	return models, true
}

// ReconcileRuntimeModels syncs DB model rows with the live runtime state.
// Exported for server bootstrap: one pass at startup (retrying briefly
// while ai-runtime may still be coming up) heals rows left stale by a solo
// ai-runtime restart, without waiting for someone to open the models page.
func (h *APIHandlers) ReconcileRuntimeModels() {
	if h.grpcClients.AIRuntime == nil || h.aiModelRepo == nil {
		return
	}
	for attempt := 0; attempt < 5; attempt++ {
		if attempt > 0 {
			time.Sleep(2 * time.Second)
		}
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		runtimeMap, ok := h.listRuntimeModels(ctx)
		cancel()
		if !ok {
			continue
		}
		h.syncRuntimeModelsToDB(context.Background(), runtimeMap, true)
		return
	}
	logger.Warn("ReconcileRuntimeModels: ai-runtime unreachable, skipping startup reconciliation")
}

// LoadModel loads an uploaded model to NPU via ai-runtime.
func (h *APIHandlers) LoadModel(c *gin.Context) {
	if h.grpcClients.AIRuntime == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "AI Runtime not available")
		return
	}

	modelID := c.Param("model_id")
	if modelID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Model ID is required")
		return
	}

	if h.aiModelRepo == nil {
		Resp(c).FailMsg(CodeServiceError, "Model repository not available")
		return
	}

	dbModel, err := h.aiModelRepo.GetByModelID(modelID)
	if err != nil || dbModel == nil {
		Resp(c).FailMsg(CodeNotFound, "Model not found")
		return
	}

	client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	// Trust the runtime, not the DB row: after a solo ai-runtime restart
	// the row can still read "loaded" while the NPU no longer holds the
	// model. Rejecting on the stale row would deadlock it — UnloadModel
	// refuses the same row because it checks the runtime first.
	rt, runtimeChecked := runtimeHasModel(ctx, client, modelID)
	if rt != nil {
		// Legacy preload registrations handed the runtime the raw CAS blob
		// path with an empty variant, silently degrading detection models to
		// raw tensors; ModelInfo carries no variant, so the path is the only
		// signal available. When a system-owned entry's path differs from
		// what composition would produce, unregister it and reload instead of
		// refusing — otherwise the degradation could never self-heal.
		// (System-owned must cover the runtime's "<system>" normalization:
		// ownerless registrations never list as "".) Composition errors
		// leave the entry alone (nothing better to offer). App-owned and
		// transient entries are somebody's live registration, untouched.
		stale := false
		if !rt.GetTransient() && isSystemOwned(rt.GetOwnerId()) && rt.GetModelPath() != "" {
			if expectedPath, _, _, err := modelload.RuntimeRegistration(dbModel); err == nil && expectedPath != rt.GetModelPath() {
				stale = true
				logger.Info("Healing stale runtime registration for %s (path %q differs from composed %q), reloading", modelID, rt.GetModelPath(), expectedPath)
				// The runtime reports a refused unload (a racing session
				// kept the model in use) as OK transport status with
				// success=false. Proceeding would re-register the same id
				// and merely co-own the stale session — reporting a heal
				// that never happened — so the response status gates the
				// reload exactly like UpdateModel's swap path.
				unregStatus, unregErr := client.UnregisterModel(ctx, &inferencepb.ModelInfo{ModelId: modelID})
				if unregErr != nil {
					Resp(c).FailMsg(CodeOperationFailed, "Failed to unload stale model registration: "+unregErr.Error())
					return
				}
				if unregStatus != nil && !unregStatus.Success {
					Resp(c).FailMsg(CodeOperationFailed, "Failed to unload stale model registration: "+unregStatus.Message)
					return
				}
			}
		}
		if !stale {
			if dbModel.Status != "loaded" {
				dbModel.Status = "loaded"
				dbModel.DesiredState = "loaded"
				if err := h.aiModelRepo.Update(dbModel); err != nil {
					logger.Warn("Failed to heal status of runtime-loaded model %s: %v", modelID, err)
				}
			}
			Resp(c).FailMsg(CodeInvalidRequest, "Model is already loaded")
			return
		}
	}
	if dbModel.Status == "loaded" {
		if !runtimeChecked {
			// Runtime unreachable — keep the old DB-only rejection rather
			// than healing against a guess.
			Resp(c).FailMsg(CodeInvalidRequest, "Model is already loaded")
			return
		}
		// Stale "loaded": the runtime lost the model. Heal the row so the
		// registration below can proceed instead of deadlocking.
		dbModel.Status = "uploaded"
		if err := h.aiModelRepo.Update(dbModel); err != nil {
			logger.Warn("Failed to heal stale loaded status for %s: %v", modelID, err)
		} else {
			logger.Info("Healed stale loaded status for %s (model missing from runtime), reloading", modelID)
		}
	}

	// Registration, input-dimension capture, postprocess probe with rollback
	// and status persistence live in loadModelCore, shared with the periodic
	// self-heal loop so a REST load and a heal reload behave identically.
	if err := h.loadModelCore(ctx, client, dbModel); err != nil {
		Resp(c).FailMsg(CodeModelLoadFailed, humanizeLoadError(err))
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			string(events.EventAIModelLoaded),
			events.MessageParams{
				"model_id":   dbModel.ModelID,
				"model_path": dbModel.FilePath,
			},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{
		"model_id": dbModel.ModelID,
		"status":   "loaded",
	})
}

// UnloadModel unloads a model from NPU, keeping the file in storage.
func (h *APIHandlers) UnloadModel(c *gin.Context) {
	if h.grpcClients.AIRuntime == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "AI Runtime not available")
		return
	}

	modelID := c.Param("model_id")
	if modelID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Model ID is required")
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	// Dynamically registered models (e.g. from container apps) may not be in DB.
	// If found in DB, check status; otherwise proceed directly to gRPC unload.
	var dbModel *model.AIModel
	if h.aiModelRepo != nil {
		dbModel, _ = h.aiModelRepo.GetByModelID(modelID)
	}

	// Check if model is actually loaded on NPU — use runtime state, not DB.
	// DB status may be stale (e.g. model re-registered by app after eviction).
	client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
	rt, runtimeChecked := runtimeHasModel(ctx, client, modelID)
	if rt == nil {
		// Heal a stale row on the way out: DB "loaded" + runtime missing is
		// the deadlock twin of LoadModel's stale rejection. The response is
		// unchanged (idempotent unload semantics), and healing only runs
		// when the runtime was actually reachable so a transient gRPC
		// outage cannot demote a model that is still serving.
		if runtimeChecked && dbModel != nil && dbModel.Status == "loaded" {
			dbModel.Status = "uploaded"
			dbModel.DesiredState = "unloaded"
			if err := h.aiModelRepo.Update(dbModel); err != nil {
				logger.Warn("Failed to heal stale loaded status for %s: %v", modelID, err)
			} else {
				logger.Info("Healed stale loaded status for %s (model missing from runtime)", modelID)
			}
		}
		Resp(c).FailMsg(CodeInvalidRequest, "Model is not loaded")
		return
	}

	// Check if any app is using this model (strict: only explicit model declarations and owner)
	if h.grpcClients.AppManager != nil {
		apps, _ := h.getAppsUsingModel(ctx, modelID, true)
		if len(apps) > 0 {
			Resp(c).FailMsg(CodeOperationFailed, "Model is in use by apps, please stop them first: "+strings.Join(apps, ", "))
			return
		}
	}

	resp, err := client.UnregisterModel(ctx, &inferencepb.ModelInfo{
		ModelId: modelID,
	})

	// If gRPC call failed or returned failure, check if model is actually
	// still in runtime memory — it may have been lost after a restart.
	if err != nil || (resp != nil && !resp.Success) {
		if rtStill, _ := runtimeHasModel(ctx, client, modelID); rtStill != nil {
			msg := "Failed to unload model from runtime"
			if resp != nil {
				msg = resp.Message
			} else {
				msg = err.Error()
			}
			Resp(c).FailMsg(CodeOperationFailed, msg)
			return
		}
		// Model not in runtime — treat as already unloaded, fall through to DB update
	}

	if dbModel != nil {
		dbModel.Status = "uploaded"
		dbModel.DesiredState = "unloaded"
		if err := h.aiModelRepo.Update(dbModel); err != nil {
			logger.Warn("Failed to update model status to uploaded: %v", err)
		}
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"ai.model.unloaded",
			events.MessageParams{"model_id": modelID},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{
		"model_id": modelID,
		"status":   "uploaded",
	})
}

// UnregisterModel deletes a model: unload from NPU if loaded, then remove file and DB record.
func (h *APIHandlers) UnregisterModel(c *gin.Context) {
	modelID := c.Param("model_id")
	if modelID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Model ID is required")
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	// Deleting a model an installed app relies on — an explicit models[]
	// declaration (durable: it breaks the app's next start even when the
	// app is stopped) or a running owner — must be refused, same as
	// UnloadModel refuses to evict an in-use model.
	if h.grpcClients.AppManager != nil {
		if apps, _ := h.getAppsUsingModel(ctx, modelID, true); len(apps) > 0 {
			Resp(c).FailMsg(CodeOperationFailed, "Model is in use by apps, please stop them first: "+strings.Join(apps, ", "))
			return
		}
	}

	// If model exists in DB, unload from NPU and delete file
	if h.aiModelRepo != nil {
		dbModel, err := h.aiModelRepo.GetByModelID(modelID)
		if err == nil && dbModel != nil {
			// Unload from NPU. Trust the runtime over the row's Status: a row
			// stuck at "uploaded" while the runtime still serves the id would
			// leave a zombie serving a model whose blob is about to be
			// deleted; a stale "loaded" on an absent registration makes the
			// unregister a harmless no-op. An unreachable runtime keeps the
			// row's word.
			if h.grpcClients.AIRuntime != nil {
				client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
				unload := dbModel.Status == "loaded"
				if rt, checked := runtimeHasModel(ctx, client, modelID); checked {
					unload = rt != nil
				}
				if unload {
					// A failed unload must veto the deletion, same gate as
					// UpdateModel: the runtime refuses (OK transport status,
					// success=false) while an inference is in flight, and
					// proceeding would delete the row, the runtime copy and
					// possibly the last CAS blob out from under a model the
					// NPU is still serving. Same contract as the no-DB
					// fallback below: transport error or success=false
					// aborts.
					unregStatus, unregErr := client.UnregisterModel(ctx, &inferencepb.ModelInfo{ModelId: modelID})
					if unregErr != nil {
						Resp(c).FailMsg(CodeServiceError, "Failed to unload model before delete: "+unregErr.Error())
						return
					}
					if unregStatus != nil && !unregStatus.Success {
						Resp(c).FailMsg(CodeOperationFailed, "Failed to unload model before delete: "+unregStatus.Message)
						return
					}
				}
			}
			// Delete DB record first so ref-count excludes this entry
			h.aiModelRepo.DeleteByModelID(modelID)
			// Drop the materialized runtime copy, if this model had one
			// (blob ref-count below is unaffected — hardlinks share the inode).
			modelload.RemoveRuntimeCopy(modelID)

			// Reclaim what the platform owns: the CAS blob under the
			// reference-count rule, and raw files only inside the platform
			// model root — an app's bundled HEF or a user-registered
			// model_path is not ours to delete.
			h.removeModelFile(dbModel)
		}
	} else if h.grpcClients.AIRuntime != nil {
		// No DB — fallback to direct ai-runtime unregister (system models)
		client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
		resp, err := client.UnregisterModel(ctx, &inferencepb.ModelInfo{ModelId: modelID})
		if err != nil {
			Resp(c).FailMsg(CodeServiceError, err.Error())
			return
		}
		if !resp.Success {
			Resp(c).FailMsg(CodeOperationFailed, resp.Message)
			return
		}
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"ai.model.deleted",
			events.MessageParams{"model_id": modelID},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{"model_id": modelID})
}

func (h *APIHandlers) GetModelApps(c *gin.Context) {
	modelID := c.Param("model_id")
	if modelID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Model ID is required")
		return
	}

	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	apps, err := h.getAppsUsingModel(ctx, modelID)
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"model_id": modelID, "apps": apps})
}

func (h *APIHandlers) getAppsUsingModel(ctx context.Context, modelID string, forUnload ...bool) ([]string, error) {
	seen := map[string]bool{}
	runningApps := map[string]bool{}

	// Ask app-manager for every installed app, running or not: an explicit
	// models[] reference is durable, so a stopped app must still count.
	if h.grpcClients.AppManager != nil {
		client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
		resp, err := client.ListApps(ctx, &emptypb.Empty{})
		if err == nil {
			isUnload := len(forUnload) > 0 && forUnload[0]
			for _, app := range resp.Apps {
				if app.State == "running" {
					runningApps[app.Id] = true
				}
				if app.ManifestPath == "" {
					continue
				}
				manifest, err := h.readAppManifest(app.ManifestPath)
				if err != nil || manifest == nil {
					continue
				}
				for _, m := range manifest.Spec.Permissions.Inference.Models {
					if m == modelID {
						seen[app.Id] = true
						break
					}
				}
				// The broad "may register models" grant is a live capability —
				// it only shields the model while the app is actually running.
				if runningApps[app.Id] && manifest.Spec.Permissions.Inference.AllowRegisterModel && !isUnload {
					seen[app.Id] = true
				}
			}
		}
	}

	// Check OwnerAppID from DB, but only if the owning app is running
	if h.aiModelRepo != nil {
		if dbModel, _ := h.aiModelRepo.GetByModelID(modelID); dbModel != nil && dbModel.OwnerAppID != "" {
			if runningApps[dbModel.OwnerAppID] {
				seen[dbModel.OwnerAppID] = true
			}
		}
	}

	var appsUsingModel []string
	for appID := range seen {
		appsUsingModel = append(appsUsingModel, appID)
	}
	return appsUsingModel, nil
}

type AppManifestForCheck struct {
	Spec struct {
		Permissions struct {
			Inference struct {
				Models             []string `yaml:"models"`
				AllowRegisterModel bool     `yaml:"allow_register_model"`
			} `yaml:"inference"`
		} `yaml:"permissions"`
	} `yaml:"spec"`
}

func (h *APIHandlers) readAppManifest(manifestPath string) (*AppManifestForCheck, error) {
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		return nil, err
	}

	var manifest AppManifestForCheck
	if err := yaml.Unmarshal(data, &manifest); err != nil {
		return nil, err
	}

	return &manifest, nil
}
