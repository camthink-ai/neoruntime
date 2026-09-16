package repo

import (
	"aipc/platform/platform-api/model"
	"gorm.io/gorm"
)

// AIModelRepo provides CRUD operations for AI model records.
type AIModelRepo struct {
	db *gorm.DB
}

// NewAIModelRepo creates a new AIModelRepo.
func NewAIModelRepo(db *gorm.DB) *AIModelRepo {
	return &AIModelRepo{db: db}
}

// Create inserts a new AI model record.
func (r *AIModelRepo) Create(m *model.AIModel) error {
	return r.db.Create(m).Error
}

// CreatePreservingZeroThreshold inserts a model while preserving an explicit
// threshold of zero. AIModel's GORM default tag intentionally maps an omitted
// zero value to 0.25 for legacy/runtime-discovered rows, but the detection
// schema also defines 0 as a valid user value (retain every detection). The
// handlers call this only after schema-default merging has made that intent
// explicit in Config; the corrective update stays in the same transaction so
// readers can never observe the substituted default.
func (r *AIModelRepo) CreatePreservingZeroThreshold(m *model.AIModel) error {
	intended := m.Threshold
	return r.db.Transaction(func(tx *gorm.DB) error {
		if err := tx.Create(m).Error; err != nil {
			return err
		}
		if intended != 0 {
			return nil
		}
		if err := tx.Model(&model.AIModel{}).
			Where("id = ?", m.ID).
			UpdateColumn("threshold", 0).Error; err != nil {
			return err
		}
		m.Threshold = 0
		return nil
	})
}

// GetByModelID retrieves a model by its business ID.
func (r *AIModelRepo) GetByModelID(modelID string) (*model.AIModel, error) {
	var m model.AIModel
	err := r.db.Where("model_id = ?", modelID).First(&m).Error
	if err != nil {
		return nil, err
	}
	return &m, nil
}

// List returns all registered models.
func (r *AIModelRepo) List() ([]model.AIModel, error) {
	var models []model.AIModel
	err := r.db.Order("created_at DESC").Find(&models).Error
	return models, err
}

// Update saves changes to an existing model record.
func (r *AIModelRepo) Update(m *model.AIModel) error {
	return r.db.Save(m).Error
}

// UpdateFileHash sets only the file hash, leaving concurrent row changes
// (status, config) untouched — used by the background hash backfill, which
// may hold a row for seconds while hashing a multi-GB file.
func (r *AIModelRepo) UpdateFileHash(modelID string, hash string) error {
	return r.db.Model(&model.AIModel{}).
		Where("model_id = ?", modelID).
		Update("file_hash", hash).Error
}

// DemoteNeverLoadedToUnloaded flips desired_state on device-level rows that
// were registered but never loaded. Rows created before the import/disk-seed
// paths started writing an explicit "unloaded" inherited the column default
// "loaded", which the self-heal loop honors by auto-loading the model — so
// the correction must run at startup, before the heal loop's first tick.
// Rows the user actually loaded (status "loaded") and app-owned rows are
// untouched: the latter are app-manager's preload responsibility.
func (r *AIModelRepo) DemoteNeverLoadedToUnloaded() (int64, error) {
	result := r.db.Model(&model.AIModel{}).
		Where("owner_app_id = ? AND status = ? AND desired_state = ?", "", "uploaded", "loaded").
		Update("desired_state", "unloaded")
	return result.RowsAffected, result.Error
}

// GetByFilePath retrieves a model by its file path.
func (r *AIModelRepo) GetByFilePath(filePath string) (*model.AIModel, error) {
	var m model.AIModel
	err := r.db.Where("file_path = ?", filePath).First(&m).Error
	if err != nil {
		return nil, err
	}
	return &m, nil
}

// DeleteByModelID removes a model record by its business ID.
func (r *AIModelRepo) DeleteByModelID(modelID string) error {
	return r.db.Where("model_id = ?", modelID).Delete(&model.AIModel{}).Error
}

// DeleteByOwnerAppID removes all model records owned by a specific app.
func (r *AIModelRepo) DeleteByOwnerAppID(appID string) (int64, error) {
	result := r.db.Where("owner_app_id = ?", appID).Delete(&model.AIModel{})
	return result.RowsAffected, result.Error
}

// CountByFileHash returns the number of model records sharing the same file hash.
// Used to guard blob file deletion — only delete when this is the last reference.
func (r *AIModelRepo) CountByFileHash(hash string) (int64, error) {
	var count int64
	err := r.db.Model(&model.AIModel{}).Where("file_hash = ?", hash).Count(&count).Error
	return count, err
}
