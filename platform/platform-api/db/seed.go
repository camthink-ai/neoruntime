package db

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"gorm.io/gorm"

	"aipc/platform/common/constants"
	"aipc/platform/common/logger"
	"aipc/platform/platform-api/model"
	"aipc/platform/platform-api/repo"
	"aipc/platform/platform-api/storage"
)

// Seed 初始化种子数据
func Seed(db *gorm.DB) error {
	if err := seedCategories(db); err != nil {
		return err
	}
	if err := seedTags(db); err != nil {
		return err
	}
	if err := seedApps(db); err != nil {
		return err
	}
	if err := seedBaseImages(db); err != nil {
		return err
	}
	return nil
}

type ScanResult struct {
	Scanned int      `json:"scanned"`
	Added   int      `json:"added"`
	Skipped int      `json:"skipped"`
	Errors  []string `json:"errors,omitempty"`
}

// SeedDiskModels scans /data/aipc/models/*/*.hef and auto-registers
// any HEF files found on disk but missing from the database.
func SeedDiskModels(db *gorm.DB) *ScanResult {
	result := &ScanResult{}

	modelDir := constants.ModelsPath()
	if _, err := os.Stat(modelDir); os.IsNotExist(err) {
		return result
	}

	modelRepo := repo.NewAIModelRepo(db)

	// Collect existing file paths from DB to skip already-registered models
	existingModels, err := modelRepo.List()
	if err != nil {
		logger.Warn("Failed to list existing models for disk scan: %v", err)
		return result
	}
	existingPaths := make(map[string]bool)
	for _, m := range existingModels {
		existingPaths[m.FilePath] = true
	}

	// Scan category directories: detection/, classification/, etc.
	entries, err := os.ReadDir(modelDir)
	if err != nil {
		logger.Warn("Failed to read model directory %s: %v", modelDir, err)
		return result
	}

	for _, entry := range entries {
		if !entry.IsDir() {
			continue
		}
		catName := entry.Name()
		// Skip internal CAS directory
		if catName == "blobs" {
			continue
		}
		catDir := filepath.Join(modelDir, catName)

		hefs, err := filepath.Glob(filepath.Join(catDir, "*.hef"))
		if err != nil {
			continue
		}

		for _, hefPath := range hefs {
			result.Scanned++
			if existingPaths[hefPath] {
				result.Skipped++
				continue
			}

			if err := registerDiskModel(modelRepo, hefPath, catName); err != nil {
				logger.Warn("Failed to auto-register model %s: %v", hefPath, err)
				result.Errors = append(result.Errors, hefPath+": "+err.Error())
			} else {
				result.Added++
			}
		}
	}

	if result.Added > 0 {
		logger.Info("Auto-registered disk models: %d", result.Added)
	}
	return result
}

func registerDiskModel(modelRepo *repo.AIModelRepo, hefPath, category string) error {
	fileName := filepath.Base(hefPath)
	modelID := strings.TrimSuffix(fileName, ".hef")

	fi, err := os.Stat(hefPath)
	if err != nil {
		return fmt.Errorf("stat %s: %w", hefPath, err)
	}

	// Extract metadata via hailortcli parse-hef
	var networkName string
	var inputWidth, inputHeight int
	if store, storeErr := storage.NewModelStorage(constants.ModelsPath()+"/blobs", 0); storeErr == nil {
		if info, parseErr := store.ValidateHEF(hefPath); parseErr == nil && info != nil {
			networkName = info.NetworkName
			inputWidth = info.InputWidth
			inputHeight = info.InputHeight
		}
	}

	// Guess model type from network name or directory category
	modelType := model.GuessModelType(networkName)
	if modelType == "" || modelType == "detection" {
		if resolved := model.ResolveModelType(category); resolved != "" {
			modelType = resolved
		}
	}

	// Map directory category to model type for better accuracy
	categoryTypeMap := map[string]string{
		"detection":      "detection",
		"classification": "classification",
		"segmentation":   "segmentation",
		"keypoint":       "keypoint",
		"landmarks":      "keypoint",
		"clip":           "clip",
		"depth":          "depth",
		"ocr":            "ocr_recognition",
		"genai":          "genai",
	}
	if mapped, ok := categoryTypeMap[category]; ok {
		modelType = mapped
	}

	// Apply field defaults for the model type
	defaults := model.GetFieldDefaults(modelType)
	var threshold float32 = 0.25
	var maxDet int = 64
	if defaults != nil {
		if v, ok := defaults["threshold"].(float64); ok {
			threshold = float32(v)
		}
		if v, ok := defaults["max_detections"].(float64); ok {
			maxDet = int(v)
		}
	}

	dbModel := &model.AIModel{
		ModelID:       modelID,
		Name:          modelID,
		FilePath:      hefPath,
		FileSize:      fi.Size(),
		ModelType:     modelType,
		Threshold:     threshold,
		MaxDetections: maxDet,
		NetworkName:   networkName,
		InputWidth:    inputWidth,
		InputHeight:   inputHeight,
		Status:        "uploaded",
	}

	return modelRepo.Create(dbModel)
}

func seedCategories(db *gorm.DB) error {
	categories := []model.StoreCategory{
		{Key: "ai-vision", Name: "AI Vision", NameZh: "视觉AI", Icon: "eye", Sort: 1},
		{Key: "ai-audio", Name: "AI Audio", NameZh: "音频AI", Icon: "mic", Sort: 2},
		{Key: "analytics", Name: "Analytics", NameZh: "数据分析", Icon: "chart", Sort: 3},
		{Key: "utility", Name: "Utility", NameZh: "工具", Icon: "tool", Sort: 4},
		{Key: "custom", Name: "Custom", NameZh: "自定义", Icon: "code", Sort: 5},
	}
	for _, cat := range categories {
		var existing model.StoreCategory
		if db.Where("key = ?", cat.Key).First(&existing).Error == gorm.ErrRecordNotFound {
			db.Create(&cat)
		}
	}
	return nil
}

func seedTags(db *gorm.DB) error {
	tags := []model.StoreTag{
		{Key: "face", Name: "Face Recognition", NameZh: "人脸识别", Sort: 1},
		{Key: "person", Name: "Person Detection", NameZh: "人员检测", Sort: 2},
		{Key: "vehicle", Name: "Vehicle Detection", NameZh: "车辆检测", Sort: 3},
		{Key: "behavior", Name: "Behavior Analysis", NameZh: "行为分析", Sort: 4},
		{Key: "counting", Name: "Counting", NameZh: "计数统计", Sort: 5},
		{Key: "intrusion", Name: "Intrusion Detection", NameZh: "入侵检测", Sort: 6},
		{Key: "plate", Name: "License Plate", NameZh: "车牌识别", Sort: 7},
		{Key: "safety", Name: "Safety Detection", NameZh: "安全检测", Sort: 8},
	}
	for _, tag := range tags {
		var existing model.StoreTag
		if db.Where("key = ?", tag.Key).First(&existing).Error == gorm.ErrRecordNotFound {
			db.Create(&tag)
		}
	}
	return nil
}

func seedApps(db *gorm.DB) error {
	// People Counting
	var app1 model.StoreApp
	if db.Where("key = ?", "people-counting").First(&app1).Error == gorm.ErrRecordNotFound {
		app1 = model.StoreApp{
			Key: "people-counting", Name: "People Counting", NameZh: "人员计数",
			ShortDesc: "Count people in the scene", ShortDescZh: "统计场景中的人数",
			Category: "ai-vision", Author: "AIPC", Architectures: "arm64",
			MinMemory: 256, SocSupport: "hailo15,rk3588", Source: "official",
			Featured: true, SortOrder: 100,
		}
		db.Create(&app1)
		db.Create(&model.StoreAppDetail{
			AppID: app1.ID, Version: "1.0.0", Image: "aipc/people-counting:1.0.0",
			Status: "stable", ReleaseDate: time.Now(),
			DefaultConfig: defaultConfigPeopleCounting,
		})
		addTagsToApp(db, app1.ID, []string{"person", "counting"})
	}

	// Face Detection
	var app2 model.StoreApp
	if db.Where("key = ?", "face-detection").First(&app2).Error == gorm.ErrRecordNotFound {
		app2 = model.StoreApp{
			Key: "face-detection", Name: "Face Detection", NameZh: "人脸检测",
			ShortDesc: "Detect faces in video", ShortDescZh: "检测视频中的人脸",
			Category: "ai-vision", Author: "AIPC", Architectures: "arm64",
			MinMemory: 512, SocSupport: "hailo15,rk3588", Source: "official",
			Featured: true, SortOrder: 90,
		}
		db.Create(&app2)
		db.Create(&model.StoreAppDetail{
			AppID: app2.ID, Version: "1.0.0", Image: "aipc/face-detection:1.0.0",
			Status: "stable", ReleaseDate: time.Now(),
			DefaultConfig: defaultConfigFaceDetection,
		})
		addTagsToApp(db, app2.ID, []string{"face"})
	}

	// Vehicle Detection
	var app3 model.StoreApp
	if db.Where("key = ?", "vehicle-detection").First(&app3).Error == gorm.ErrRecordNotFound {
		app3 = model.StoreApp{
			Key: "vehicle-detection", Name: "Vehicle Detection", NameZh: "车辆检测",
			ShortDesc: "Detect vehicles", ShortDescZh: "检测车辆",
			Category: "ai-vision", Author: "AIPC", Architectures: "arm64",
			MinMemory: 512, SocSupport: "hailo15,rk3588", Source: "official",
			Featured: false, SortOrder: 80,
		}
		db.Create(&app3)
		db.Create(&model.StoreAppDetail{
			AppID: app3.ID, Version: "1.0.0", Image: "aipc/vehicle-detection:1.0.0",
			Status: "stable", ReleaseDate: time.Now(),
			DefaultConfig: defaultConfigVehicle,
		})
		addTagsToApp(db, app3.ID, []string{"vehicle", "plate"})
	}

	// Intrusion Detection
	var app4 model.StoreApp
	if db.Where("key = ?", "intrusion-detection").First(&app4).Error == gorm.ErrRecordNotFound {
		app4 = model.StoreApp{
			Key: "intrusion-detection", Name: "Intrusion Detection", NameZh: "入侵检测",
			ShortDesc: "Detect intrusions in zones", ShortDescZh: "检测区域入侵",
			Category: "ai-vision", Author: "AIPC", Architectures: "arm64",
			MinMemory: 256, SocSupport: "hailo15,rk3588", Source: "official",
			Featured: false, SortOrder: 70,
		}
		db.Create(&app4)
		db.Create(&model.StoreAppDetail{
			AppID: app4.ID, Version: "1.0.0", Image: "aipc/intrusion-detection:1.0.0",
			Status: "stable", ReleaseDate: time.Now(),
			DefaultConfig: defaultConfigIntrusion,
		})
		addTagsToApp(db, app4.ID, []string{"intrusion", "person"})
	}

	return nil
}

func addTagsToApp(db *gorm.DB, appID uint, tagKeys []string) {
	for _, key := range tagKeys {
		var tag model.StoreTag
		if db.Where("key = ?", key).First(&tag).Error == nil {
			db.Create(&model.StoreAppTag{AppID: appID, TagID: tag.ID})
		}
	}
}

const defaultConfigPeopleCounting = `apiVersion: v1
kind: Application
metadata:
  id: people_counting
  name: People Counting
  version: 1.0.0
spec:
  image: aipc/people-counting:1.0.0
  resources:
    cpu: "50%"
    memory: "256Mi"
  permissions:
    inference:
      models: [person_v1]
      max_qps: 10
    events:
      publish: [app/people_counting/*]
  restart_policy: on-failure`

const defaultConfigFaceDetection = `apiVersion: v1
kind: Application
metadata:
  id: face_detection
  name: Face Detection
  version: 1.0.0
spec:
  image: aipc/face-detection:1.0.0
  resources:
    cpu: "60%"
    memory: "512Mi"
  permissions:
    inference:
      models: [face_v1]
      max_qps: 15
    events:
      publish: [app/face_detection/*]
  restart_policy: on-failure`

const defaultConfigVehicle = `apiVersion: v1
kind: Application
metadata:
  id: vehicle_detection
  name: Vehicle Detection
  version: 1.0.0
spec:
  image: aipc/vehicle-detection:1.0.0
  resources:
    cpu: "60%"
    memory: "512Mi"
  permissions:
    inference:
      models: [vehicle_v1]
      max_qps: 10
    events:
      publish: [app/vehicle_detection/*]
  restart_policy: on-failure`

const defaultConfigIntrusion = `apiVersion: v1
kind: Application
metadata:
  id: intrusion_detection
  name: Intrusion Detection
  version: 1.0.0
spec:
  image: aipc/intrusion-detection:1.0.0
  resources:
    cpu: "50%"
    memory: "256Mi"
  permissions:
    inference:
      models: [person_v1]
      max_qps: 10
    events:
      publish: [app/intrusion_detection/*]
    device:
      light: true
  restart_policy: on-failure`

func seedBaseImages(db *gorm.DB) error {
	images := []model.BaseImage{
		{
			Name:        "aipc-sdk-python",
			Tag:         "1.0.0",
			Image:       "aipc/sdk-python:1.0.0",
			Language:    "python",
			Description: "Python SDK with AI inference, video, events support",
			DescZh:      "Python SDK，支持 AI 推理、视频流、事件总线",
			SdkVersion:  "1.0.0",
			Features:    `["inference", "video", "events", "device"]`,
			BuildCmd:    "pip install -r requirements.txt",
			EntryPoint:  "python main.py",
			Status:      "active",
		},
		{
			Name:        "aipc-sdk-cpp",
			Tag:         "1.0.0",
			Image:       "aipc/sdk-cpp:1.0.0",
			Language:    "cpp",
			Description: "C++ SDK with high performance AI inference",
			DescZh:      "C++ SDK，高性能 AI 推理",
			SdkVersion:  "1.0.0",
			Features:    `["inference", "video", "events", "device"]`,
			BuildCmd:    "mkdir -p build && cd build && cmake .. && make",
			EntryPoint:  "./build/app",
			Status:      "active",
		},
		{
			Name:        "aipc-sdk-go",
			Tag:         "1.0.0",
			Image:       "aipc/sdk-go:1.0.0",
			Language:    "go",
			Description: "Go SDK for building lightweight apps",
			DescZh:      "Go SDK，构建轻量级应用",
			SdkVersion:  "1.0.0",
			Features:    `["inference", "events", "device"]`,
			BuildCmd:    "go build -o app .",
			EntryPoint:  "./app",
			Status:      "active",
		},
		{
			Name:        "aipc-sdk-nodejs",
			Tag:         "1.0.0",
			Image:       "aipc/sdk-nodejs:1.0.0",
			Language:    "nodejs",
			Description: "Node.js SDK for web services and visualization",
			DescZh:      "Node.js SDK，Web 服务和数据可视化",
			SdkVersion:  "1.0.0",
			Features:    `["events", "device", "http"]`,
			BuildCmd:    "npm install && npm run build",
			EntryPoint:  "node dist/index.js",
			Status:      "active",
		},
		{
			Name:        "aipc-sdk-rust",
			Tag:         "1.0.0",
			Image:       "aipc/sdk-rust:1.0.0",
			Language:    "rust",
			Description: "Rust SDK for high-performance edge computing",
			DescZh:      "Rust SDK，高性能边缘计算",
			SdkVersion:  "1.0.0",
			Features:    `["inference", "video", "events"]`,
			BuildCmd:    "cargo build --release",
			EntryPoint:  "./target/release/app",
			Status:      "active",
		},
	}

	for _, img := range images {
		var existing model.BaseImage
		if db.Where("name = ? AND tag = ?", img.Name, img.Tag).First(&existing).Error == gorm.ErrRecordNotFound {
			db.Create(&img)
		}
	}
	return nil
}
