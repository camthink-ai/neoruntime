package model

import "time"

// BaseImage base image (provides SDK and build environment)
type BaseImage struct {
	ID          uint   `gorm:"primaryKey" json:"id"`
	Name        string `gorm:"uniqueIndex;not null" json:"name"` // e.g. "aipc-sdk-python"
	Tag         string `gorm:"not null" json:"tag"`              // e.g. "1.0.0"
	Image       string `gorm:"not null" json:"image"`            // Full image path
	Language    string `json:"language"`                         // python, cpp, go
	Description string `json:"description"`
	DescZh      string `json:"desc_zh"`

	// SDK information
	SdkVersion string `json:"sdk_version"`
	Features   string `json:"features"` // JSON: inference, video, events, device

	// Build information
	BuildCmd   string `json:"build_cmd"`   // Default build command
	EntryPoint string `json:"entry_point"` // Default entry point

	Status    string    `gorm:"default:active" json:"status"` // active, deprecated
	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

func (BaseImage) TableName() string {
	return "base_images"
}

// AppProject user application project
type AppProject struct {
	ID          uint   `gorm:"primaryKey" json:"id"`
	Name        string `gorm:"not null" json:"name"`
	Key         string `gorm:"uniqueIndex;not null" json:"key"` // Application identifier
	Description string `json:"description"`

	// Development configuration
	BaseImageID uint   `gorm:"index" json:"base_image_id"`
	Language    string `json:"language"`

	// Source code
	SourcePath string `json:"source_path"` // Source directory path
	EntryFile  string `json:"entry_file"`  // Entry file

	// Build configuration
	BuildConfig string `gorm:"type:text" json:"build_config"` // JSON

	// Application configuration
	AppConfig string `gorm:"type:text" json:"app_config"` // app.yaml content

	// Persistent storage configuration
	Volumes string `gorm:"type:text" json:"volumes"` // JSON: [{"name":"data","host_path":"/data/aipc/apps/{key}/data","container_path":"/app/data","read_only":false}]

	// Status
	Status     string `gorm:"default:draft" json:"status"` // draft, building, built, published, error
	BuildImage string `json:"build_image"`                 // Built image
	Message    string `json:"message"`                     // Status message

	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

func (AppProject) TableName() string {
	return "app_projects"
}

// AppBuild build record
type AppBuild struct {
	ID        uint   `gorm:"primaryKey" json:"id"`
	ProjectID uint   `gorm:"index;not null" json:"project_id"`
	Version   string `json:"version"`

	Status    string `gorm:"default:pending" json:"status"` // pending, building, success, failed
	Log       string `gorm:"type:text" json:"log"`
	Image     string `json:"image"` // Build output image
	ImageSize int64  `json:"image_size"`

	StartedAt  *time.Time `json:"started_at"`
	FinishedAt *time.Time `json:"finished_at"`
	CreatedAt  time.Time  `json:"created_at"`
}

func (AppBuild) TableName() string {
	return "app_builds"
}
