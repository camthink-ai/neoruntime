package handlers

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"google.golang.org/protobuf/types/known/emptypb"
	"gopkg.in/yaml.v3"

	inferencepb "aipc/platform/ai-runtime/proto"
	apppb "aipc/platform/app-manager/proto"
	"aipc/platform/common/events"
	"aipc/platform/common/logger"
	platformdb "aipc/platform/platform-api/db"
	"aipc/platform/platform-api/model"
)

// AI Runtime proxy handlers

func (h *APIHandlers) GetCapabilities(c *gin.Context) {
	Resp(c).OK(gin.H{
		"formats":     model.SupportedFormats,
		"model_types": model.SupportedModelTypes,
	})
}

func (h *APIHandlers) ParseModel(c *gin.Context) {
	file, err := c.FormFile("model")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Model file is required: "+err.Error())
		return
	}

	ext := strings.ToLower(filepath.Ext(file.Filename))
	validExt := false
	for _, f := range model.SupportedFormats {
		if f.Extension == ext {
			validExt = true
			break
		}
	}
	if !validExt {
		Resp(c).FailMsg(CodeInvalidRequest, "Unsupported file format. Supported: "+func() string {
			var exts []string
			for _, f := range model.SupportedFormats {
				exts = append(exts, f.Extension)
			}
			return strings.Join(exts, ", ")
		}())
		return
	}

	var modelPath string
	var fileHash string
	var fileSize int64

	if h.modelStore != nil {
		src, err := file.Open()
		if err != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to open uploaded file: "+err.Error())
			return
		}
		defer src.Close()

		result, err := h.modelStore.SaveWithHash(src, ext)
		if err != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to save model: "+err.Error())
			return
		}
		modelPath = result.Path
		fileHash = result.Hash
		fileSize = result.Size
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
			if fileHash != "" {
				h.modelStore.Delete(fileHash, ext)
			}
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

	Resp(c).OK(gin.H{
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
	})
}

func (h *APIHandlers) ScanModels(c *gin.Context) {
	if h.db == nil {
		Resp(c).FailMsg(CodeUnknownError, "database not available")
		return
	}
	result := platformdb.SeedDiskModels(h.db)
	Resp(c).OK(result)
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
		Variant         string                    `json:"variant,omitempty"`
		Threshold       float32                   `json:"threshold,omitempty"`
		MaxDetections   int                       `json:"max_detections,omitempty"`
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
	runtimeMap := map[string]*inferencepb.ModelInfo{}
	runtimeOK := false
	if h.grpcClients.AIRuntime != nil {
		client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
		resp, err := client.ListModels(ctx, &inferencepb.Empty{})
		if err == nil {
			runtimeOK = true
			for _, m := range resp.Models {
				runtimeMap[m.ModelId] = m
			}
		}
	}

	// Upsert runtime models into DB so platform-api is the single source of truth
	h.syncRuntimeModelsToDB(ctx, runtimeMap, runtimeOK)

	enrichedModels := make([]EnrichedModel, 0)

	// DB is the single source of truth for model metadata
	if h.aiModelRepo != nil {
		dbModels, err := h.aiModelRepo.List()
		if err == nil {
			for _, db := range dbModels {
				em := EnrichedModel{
					ModelId:       db.ModelID,
					Name:          db.Name,
					ModelPath:     db.FilePath,
					Status:        db.Status,
					Source:        db.Source,
					OwnerAppID:    db.OwnerAppID,
					DesiredState:  db.DesiredState,
					ModelType:     db.ModelType,
					Variant:       db.Variant,
					Threshold:     db.Threshold,
					MaxDetections: db.MaxDetections,
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
func (h *APIHandlers) syncRuntimeModelsToDB(ctx context.Context, runtimeMap map[string]*inferencepb.ModelInfo, runtimeOK bool) {
	if h.aiModelRepo == nil {
		return
	}

	// 1. Upsert: runtime models → DB
	for modelID, rt := range runtimeMap {
		existing, _ := h.aiModelRepo.GetByModelID(modelID)
		if existing == nil {
			newModel := &model.AIModel{
				ModelID:      modelID,
				Name:         rt.Name,
				FilePath:     rt.ModelPath,
				Version:      rt.Version,
				Status:       "loaded",
				Source:       "dynamic",
				OwnerAppID:   rt.OwnerId,
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
		} else if existing.Status == "loaded" && rt.OwnerId != "" && existing.OwnerAppID == "" {
			existing.OwnerAppID = rt.OwnerId
			if err := h.aiModelRepo.Update(existing); err != nil {
				logger.Warn("syncRuntimeModelsToDB: failed to update owner for %s: %v", modelID, err)
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
	for i := range dbModels {
		db := &dbModels[i]
		if db.Status != "loaded" {
			continue
		}
		if _, inRuntime := runtimeMap[db.ModelID]; inRuntime {
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
				"variant":        dbModel.Variant,
				"threshold":      dbModel.Threshold,
				"max_detections": dbModel.MaxDetections,
				"file_size":      dbModel.FileSize,
				"network_name":   dbModel.NetworkName,
				"input_width":    dbModel.InputWidth,
				"input_height":   dbModel.InputHeight,
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

	if req.ModelType != "" {
		resolved := model.ResolveModelType(req.ModelType)
		if resolved == "" {
			Resp(c).FailMsg(CodeInvalidRequest, "Unsupported model_type: "+req.ModelType)
			return
		}
		req.ModelType = resolved
	}

	if h.aiModelRepo != nil {
		if existing, _ := h.aiModelRepo.GetByModelID(req.ModelID); existing != nil {
			Resp(c).OK(gin.H{"model_id": existing.ModelID, "status": existing.Status})
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
			Variant:       req.Variant,
			Threshold:     threshold,
			MaxDetections: maxDet,
			NetworkName:   req.NetworkName,
			VStreamInfo:   req.VStreamInfo,
			InputWidth:    req.InputWidth,
			InputHeight:   req.InputHeight,
			Config:        string(configJSON),
			Status:        "uploaded",
		}
		if err := h.aiModelRepo.Create(dbModel); err != nil {
			logger.Warn("Failed to persist model to DB: %v", err)
		}
	}

	Resp(c).OK(gin.H{
		"model_id":   req.ModelID,
		"model_path": modelPath,
		"status":     "uploaded",
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

	modelType := c.PostForm("model_type")
	variant := c.PostForm("variant")
	thresholdStr := c.PostForm("threshold")
	threshold := float32(0.25)
	if thresholdStr != "" {
		if v, err := strconv.ParseFloat(thresholdStr, 32); err == nil {
			threshold = float32(v)
		}
	}
	maxDetStr := c.PostForm("max_detections")
	maxDetections := 64
	if maxDetStr != "" {
		if v, err := strconv.Atoi(maxDetStr); err == nil {
			maxDetections = v
		}
	}

	var modelPath string
	var fileHash string
	var fileSize int64

	if h.modelStore != nil {
		src, err := file.Open()
		if err != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to open uploaded file: "+err.Error())
			return
		}
		defer src.Close()

		result, err := h.modelStore.SaveWithHash(src, ext)
		if err != nil {
			Resp(c).FailMsg(CodeServiceError, "Failed to save model: "+err.Error())
			return
		}
		modelPath = result.Path
		fileHash = result.Hash
		fileSize = result.Size
		if result.Existed {
			logger.Info("Model blob already exists (dedup): %s", fileHash)
		}
	} else {
		storagePath := h.modelStorage
		if storagePath == "" {
			storagePath = h.modelStorage
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
			if fileHash != "" {
				h.modelStore.Delete(fileHash, ext)
			}
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

	// Save to DB as "uploaded" — not loaded to NPU yet
	if h.aiModelRepo != nil {
		dbModel := &model.AIModel{
			ModelID:       modelID,
			Name:          file.Filename,
			FilePath:      modelPath,
			FileSize:      fileSize,
			FileHash:      fileHash,
			ModelType:     modelType,
			Variant:       variant,
			Threshold:     threshold,
			MaxDetections: maxDetections,
			VStreamInfo:   vstreamInfoJSON,
			NetworkName:   networkName,
			InputWidth:    inputWidth,
			InputHeight:   inputHeight,
			Status:        "uploaded",
		}
		if err := h.aiModelRepo.Create(dbModel); err != nil {
			logger.Warn("Failed to persist uploaded model to DB: %v", err)
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

	if dbModel.Status == "loaded" {
		Resp(c).FailMsg(CodeInvalidRequest, "Model is already loaded")
		return
	}

	client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	resp, err := client.RegisterModel(ctx, &inferencepb.ModelRegisterRequest{
		ModelPath:    dbModel.FilePath,
		ModelId:      dbModel.ModelID,
		ModelType:    dbModel.ModelType,
		ModelVariant: dbModel.Variant,
	})
	if err != nil {
		Resp(c).FailMsg(CodeModelLoadFailed, "Failed to load model on NPU: "+err.Error())
		return
	}

	if resp.Status != nil && !resp.Status.Success {
		Resp(c).FailMsg(CodeModelLoadFailed, resp.Status.Message)
		return
	}

	// Update input dimensions from live model info
	if resp.ModelId != "" {
		modelInfo, infoErr := client.GetModelInfo(ctx, &inferencepb.ModelInfo{
			ModelId: resp.ModelId,
		})
		if infoErr == nil && modelInfo != nil && len(modelInfo.Inputs) > 0 {
			input := modelInfo.Inputs[0]
			layout := input.GetLayout()
			switch layout {
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
	}

	dbModel.Status = "loaded"
	dbModel.DesiredState = "loaded"
	if err := h.aiModelRepo.Update(dbModel); err != nil {
		logger.Warn("Failed to update model status to loaded: %v", err)
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
	isLoadedOnNPU := false
	listResp, listErr := client.ListModels(ctx, &inferencepb.Empty{})
	if listErr == nil {
		for _, m := range listResp.Models {
			if m.ModelId == modelID {
				isLoadedOnNPU = true
				break
			}
		}
	}
	if !isLoadedOnNPU {
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
		stillLoaded := false
		listResp, listErr := client.ListModels(ctx, &inferencepb.Empty{})
		if listErr == nil {
			for _, m := range listResp.Models {
				if m.ModelId == modelID {
					stillLoaded = true
					break
				}
			}
		}
		if stillLoaded {
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

	// If model exists in DB, unload from NPU and delete file
	if h.aiModelRepo != nil {
		dbModel, err := h.aiModelRepo.GetByModelID(modelID)
		if err == nil && dbModel != nil {
			// Unload from NPU if loaded
			if dbModel.Status == "loaded" && h.grpcClients.AIRuntime != nil {
				client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
				client.UnregisterModel(ctx, &inferencepb.ModelInfo{ModelId: modelID})
			}
			// Delete DB record first so ref-count excludes this entry
			h.aiModelRepo.DeleteByModelID(modelID)

			// Only delete the blob file when no other model references the same hash
			if dbModel.FileHash != "" && h.modelStore != nil {
				if count, err := h.aiModelRepo.CountByFileHash(dbModel.FileHash); err == nil && count == 0 {
					h.modelStore.Delete(dbModel.FileHash, ".hef")
				}
			} else if dbModel.FilePath != "" {
				os.Remove(dbModel.FilePath)
			}
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

	// Collect running apps
	if h.grpcClients.AppManager != nil {
		client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
		resp, err := client.ListApps(ctx, &emptypb.Empty{})
		if err == nil {
			for _, app := range resp.Apps {
				if app.State != "running" {
					continue
				}
				runningApps[app.Id] = true
				if app.ManifestPath != "" {
					manifest, err := h.readAppManifest(app.ManifestPath)
					if err == nil && manifest != nil {
						for _, m := range manifest.Spec.Permissions.Inference.Models {
							if m == modelID {
								seen[app.Id] = true
								break
							}
						}
						isUnload := len(forUnload) > 0 && forUnload[0]
						if manifest.Spec.Permissions.Inference.AllowRegisterModel && !isUnload {
							seen[app.Id] = true
						}
					}
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
