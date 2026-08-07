package model

import "time"

// StoreApp store application
type StoreApp struct {
	ID          uint   `gorm:"primaryKey" json:"id"`
	Key         string `gorm:"uniqueIndex;not null" json:"key"` // Unique identifier e.g. "people-counting"
	Name        string `gorm:"not null" json:"name"`
	NameZh      string `json:"name_zh"`
	Icon        string `json:"icon"` // Base64 or URL
	ShortDesc   string `json:"short_desc"`
	ShortDescZh string `json:"short_desc_zh"`
	Description string `gorm:"type:text" json:"description"` // Detailed description
	Category    string `gorm:"index" json:"category"`        // ai-vision, ai-audio, utility, custom
	Author      string `json:"author"`
	Website     string `json:"website"`
	Document    string `json:"document"`

	// Technical attributes
	Architectures string `json:"architectures"` // arm64,amd64
	MinMemory     int    `json:"min_memory"`    // MB
	GpuRequired   bool   `json:"gpu_required"`
	SocSupport    string `json:"soc_support"` // hailo15,rk3588,jetson

	// Store attributes
	Source    string  `gorm:"default:official" json:"source"` // official, community, custom
	Status    string  `gorm:"default:active" json:"status"`   // active, deprecated, hidden
	Featured  bool    `gorm:"default:false" json:"featured"`
	SortOrder int     `gorm:"default:0" json:"sort_order"`
	Downloads int     `gorm:"default:0" json:"downloads"`
	Rating    float32 `gorm:"default:0" json:"rating"`

	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

func (StoreApp) TableName() string {
	return "store_apps"
}

// StoreAppDetail application version details
type StoreAppDetail struct {
	ID      uint   `gorm:"primaryKey" json:"id"`
	AppID   uint   `gorm:"index;not null" json:"app_id"`
	Version string `gorm:"not null" json:"version"` // e.g. "1.0.0"
	Image   string `gorm:"not null" json:"image"`   // Image registry path

	// Configuration
	DefaultConfig string `gorm:"type:text" json:"default_config"` // Default app.yaml
	Changelog     string `gorm:"type:text" json:"changelog"`

	// Status
	Status      string    `gorm:"default:stable" json:"status"` // stable, beta, deprecated
	ReleaseDate time.Time `json:"release_date"`

	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

func (StoreAppDetail) TableName() string {
	return "store_app_details"
}

// StoreCategory application category
type StoreCategory struct {
	ID     uint   `gorm:"primaryKey" json:"id"`
	Key    string `gorm:"uniqueIndex;not null" json:"key"`
	Name   string `gorm:"not null" json:"name"`
	NameZh string `json:"name_zh"`
	Icon   string `json:"icon"`
	Sort   int    `gorm:"default:0" json:"sort"`

	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

func (StoreCategory) TableName() string {
	return "store_categories"
}

// StoreTag application tag
type StoreTag struct {
	ID     uint   `gorm:"primaryKey" json:"id"`
	Key    string `gorm:"uniqueIndex;not null" json:"key"`
	Name   string `gorm:"not null" json:"name"`
	NameZh string `json:"name_zh"`
	Sort   int    `gorm:"default:0" json:"sort"`

	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

func (StoreTag) TableName() string {
	return "store_tags"
}

// StoreAppTag 应用-标签关联
type StoreAppTag struct {
	ID    uint `gorm:"primaryKey" json:"id"`
	AppID uint `gorm:"index;not null" json:"app_id"`
	TagID uint `gorm:"index;not null" json:"tag_id"`
}

func (StoreAppTag) TableName() string {
	return "store_app_tags"
}

// AppInstall installed application
type AppInstall struct {
	ID          uint  `gorm:"primaryKey" json:"id"`
	StoreAppID  *uint `gorm:"index" json:"store_app_id"` // Associated store app (can be nil)
	AppDetailID *uint `json:"app_detail_id"`             // Associated version

	// Instance information
	AppID       string `gorm:"uniqueIndex;not null" json:"app_id"` // Application instance ID
	Name        string `gorm:"not null" json:"name"`
	Version     string `json:"version"`
	Description string `json:"description"`
	Image       string `json:"image"`

	// Running status
	Status      string `gorm:"default:installed" json:"status"` // installed, running, stopped, error
	ContainerID string `json:"container_id"`
	PID         int    `json:"pid"`
	Message     string `json:"message"`

	// Configuration
	ManifestPath string `json:"manifest_path"`
	Config       string `gorm:"type:text" json:"config"` // JSON

	// Timestamps
	InstalledAt time.Time `json:"installed_at"`
	StartedAt   time.Time `json:"started_at"`
	StoppedAt   time.Time `json:"stopped_at"`

	CreatedAt time.Time `json:"created_at"`
	UpdatedAt time.Time `json:"updated_at"`
}

func (AppInstall) TableName() string {
	return "app_installs"
}
