package repo

import (
	"aipc/platform/platform-api/model"
	"gorm.io/gorm"
	"gorm.io/gorm/clause"
)

// SettingRepo provides CRUD operations for system settings.
type SettingRepo struct {
	db *gorm.DB
}

// NewSettingRepo creates a new SettingRepo.
func NewSettingRepo(db *gorm.DB) *SettingRepo {
	return &SettingRepo{db: db}
}

// Get retrieves a setting value by key. Returns empty string if not found.
func (r *SettingRepo) Get(key string) (string, error) {
	var s model.Setting
	err := r.db.Where("`key` = ?", key).First(&s).Error
	if err != nil {
		if err == gorm.ErrRecordNotFound {
			return "", nil
		}
		return "", err
	}
	return s.Value, nil
}

// Set creates or updates a setting. Uses UPSERT for atomic operation.
func (r *SettingRepo) Set(key, value string) error {
	s := model.Setting{Key: key, Value: value}
	return r.db.Clauses(clause.OnConflict{
		Columns:   []clause.Column{{Name: "key"}},
		DoUpdates: clause.AssignmentColumns([]string{"value"}),
	}).Create(&s).Error
}

// GetAll returns all settings as a map.
func (r *SettingRepo) GetAll() (map[string]string, error) {
	var settings []model.Setting
	err := r.db.Find(&settings).Error
	if err != nil {
		return nil, err
	}
	result := make(map[string]string, len(settings))
	for _, s := range settings {
		result[s.Key] = s.Value
	}
	return result, nil
}

// Delete removes a setting by key.
func (r *SettingRepo) Delete(key string) error {
	return r.db.Where("`key` = ?", key).Delete(&model.Setting{}).Error
}
