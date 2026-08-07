package model

import "time"

// AIModel represents a registered AI model in the system.
type AIModel struct {
	ID        uint      `gorm:"primaryKey" json:"id"`
	ModelID   string    `gorm:"uniqueIndex;not null" json:"model_id"`
	Name      string    `json:"name"`
	FilePath  string    `json:"file_path"`
	Version   string    `json:"version"`
	Status    string    `gorm:"default:uploaded" json:"status"` // uploaded, loaded
	FileSize  int64     `json:"file_size"`
	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`

	// CAS (Content Addressable Storage)
	FileHash string `gorm:"index" json:"file_hash"` // SHA256 of model binary

	// AI Task metadata
	ModelType     string  `json:"model_type"`                       // detection, classification, landmarks, segmentation
	Variant       string  `json:"variant"`                          // yolov8n, yolov8s, yolov5, mediapipe_face, ...
	Threshold     float32 `gorm:"default:0.25" json:"threshold"`    // confidence threshold
	MaxDetections int     `gorm:"default:64" json:"max_detections"` // max results per frame

	// Auto-extracted HEF metadata (JSON)
	VStreamInfo string `gorm:"type:text" json:"vstream_info"` // JSON: input/output tensor specs from hailortcli parse-hef
	NetworkName string `json:"network_name"`                  // extracted from HEF
	InputWidth  int    `json:"input_width"`                   // extracted from HEF input vstream
	InputHeight int    `json:"input_height"`                  // extracted from HEF input vstream

	// Schema-driven config (JSON map of key→value, from model type field definitions)
	Config string `gorm:"type:text" json:"config,omitempty"`

	// Model provenance and lifecycle
	Source       string `gorm:"default:disk" json:"source"`          // "disk" (seed) or "dynamic" (gRPC registration)
	OwnerAppID   string `json:"owner_app_id"`                        // App ID that registered this model
	DesiredState string `gorm:"default:loaded" json:"desired_state"` // "loaded" or "unloaded"
}

// TableName overrides the default table name.
func (AIModel) TableName() string {
	return "ai_models"
}
