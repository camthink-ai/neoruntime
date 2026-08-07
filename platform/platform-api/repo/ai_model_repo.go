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
