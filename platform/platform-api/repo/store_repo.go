package repo

import (
	"aipc/platform/platform-api/model"

	"gorm.io/gorm"
)

// StoreRepo store data access
type StoreRepo struct {
	db *gorm.DB
}

// NewStoreRepo creates a store data access instance
func NewStoreRepo(db *gorm.DB) *StoreRepo {
	return &StoreRepo{db: db}
}

// ========== StoreApp ==========

// ListApps gets application list
func (r *StoreRepo) ListApps(category, search string, featured bool, page, pageSize int) ([]model.StoreApp, int64, error) {
	var apps []model.StoreApp
	var total int64

	query := r.db.Model(&model.StoreApp{}).Where("status = ?", "active")

	if category != "" {
		query = query.Where("category = ?", category)
	}
	if search != "" {
		query = query.Where("name LIKE ? OR name_zh LIKE ? OR short_desc LIKE ?",
			"%"+search+"%", "%"+search+"%", "%"+search+"%")
	}
	if featured {
		query = query.Where("featured = ?", true)
	}

	if err := query.Count(&total).Error; err != nil {
		return nil, 0, err
	}

	offset := (page - 1) * pageSize
	if err := query.Order("sort_order DESC, downloads DESC").
		Offset(offset).Limit(pageSize).Find(&apps).Error; err != nil {
		return nil, 0, err
	}

	return apps, total, nil
}

// GetAppByKey gets application by Key
func (r *StoreRepo) GetAppByKey(key string) (*model.StoreApp, error) {
	var app model.StoreApp
	if err := r.db.Where("key = ?", key).First(&app).Error; err != nil {
		return nil, err
	}
	return &app, nil
}

// GetAppByID gets application by ID
func (r *StoreRepo) GetAppByID(id uint) (*model.StoreApp, error) {
	var app model.StoreApp
	if err := r.db.First(&app, id).Error; err != nil {
		return nil, err
	}
	return &app, nil
}

// CreateApp creates an application
func (r *StoreRepo) CreateApp(app *model.StoreApp) error {
	return r.db.Create(app).Error
}

// UpdateApp updates an application
func (r *StoreRepo) UpdateApp(app *model.StoreApp) error {
	return r.db.Save(app).Error
}

// IncrementDownloads increments download count
func (r *StoreRepo) IncrementDownloads(appID uint) error {
	return r.db.Model(&model.StoreApp{}).Where("id = ?", appID).
		UpdateColumn("downloads", gorm.Expr("downloads + 1")).Error
}

// ========== StoreAppDetail ==========

// ListAppDetails gets application version list
func (r *StoreRepo) ListAppDetails(appID uint) ([]model.StoreAppDetail, error) {
	var details []model.StoreAppDetail
	if err := r.db.Where("app_id = ?", appID).
		Order("release_date DESC").Find(&details).Error; err != nil {
		return nil, err
	}
	return details, nil
}

// GetLatestDetail gets the latest version
func (r *StoreRepo) GetLatestDetail(appID uint) (*model.StoreAppDetail, error) {
	var detail model.StoreAppDetail
	if err := r.db.Where("app_id = ? AND status = ?", appID, "stable").
		Order("release_date DESC").First(&detail).Error; err != nil {
		return nil, err
	}
	return &detail, nil
}

// GetDetailByVersion gets details by version
func (r *StoreRepo) GetDetailByVersion(appID uint, version string) (*model.StoreAppDetail, error) {
	var detail model.StoreAppDetail
	if err := r.db.Where("app_id = ? AND version = ?", appID, version).
		First(&detail).Error; err != nil {
		return nil, err
	}
	return &detail, nil
}

// CreateAppDetail creates a version
func (r *StoreRepo) CreateAppDetail(detail *model.StoreAppDetail) error {
	return r.db.Create(detail).Error
}

// ========== StoreCategory ==========

// ListCategories gets category list
func (r *StoreRepo) ListCategories() ([]model.StoreCategory, error) {
	var categories []model.StoreCategory
	if err := r.db.Order("sort ASC").Find(&categories).Error; err != nil {
		return nil, err
	}
	return categories, nil
}

// GetCategoryByKey gets category by Key
func (r *StoreRepo) GetCategoryByKey(key string) (*model.StoreCategory, error) {
	var category model.StoreCategory
	if err := r.db.Where("key = ?", key).First(&category).Error; err != nil {
		return nil, err
	}
	return &category, nil
}

// CreateCategory creates a category
func (r *StoreRepo) CreateCategory(category *model.StoreCategory) error {
	return r.db.Create(category).Error
}

// ========== StoreTag ==========

// ListTags gets tag list
func (r *StoreRepo) ListTags() ([]model.StoreTag, error) {
	var tags []model.StoreTag
	if err := r.db.Order("sort ASC").Find(&tags).Error; err != nil {
		return nil, err
	}
	return tags, nil
}

// GetTagsByAppID gets tags for an application
func (r *StoreRepo) GetTagsByAppID(appID uint) ([]model.StoreTag, error) {
	var tags []model.StoreTag
	if err := r.db.Table("store_tags").
		Joins("JOIN store_app_tags ON store_tags.id = store_app_tags.tag_id").
		Where("store_app_tags.app_id = ?", appID).
		Find(&tags).Error; err != nil {
		return nil, err
	}
	return tags, nil
}

// CreateTag creates a tag
func (r *StoreRepo) CreateTag(tag *model.StoreTag) error {
	return r.db.Create(tag).Error
}

// AddAppTag adds application-tag association
func (r *StoreRepo) AddAppTag(appID, tagID uint) error {
	return r.db.Create(&model.StoreAppTag{AppID: appID, TagID: tagID}).Error
}

// ========== AppInstall ==========

// ListInstalls gets installed application list
func (r *StoreRepo) ListInstalls() ([]model.AppInstall, error) {
	var installs []model.AppInstall
	if err := r.db.Order("created_at DESC").Find(&installs).Error; err != nil {
		return nil, err
	}
	return installs, nil
}

// GetInstallByAppID gets install record by application ID
func (r *StoreRepo) GetInstallByAppID(appID string) (*model.AppInstall, error) {
	var install model.AppInstall
	if err := r.db.Where("app_id = ?", appID).First(&install).Error; err != nil {
		return nil, err
	}
	return &install, nil
}

// CreateInstall creates an install record
func (r *StoreRepo) CreateInstall(install *model.AppInstall) error {
	return r.db.Create(install).Error
}

// UpdateInstall updates an install record
func (r *StoreRepo) UpdateInstall(install *model.AppInstall) error {
	return r.db.Save(install).Error
}

// DeleteInstall deletes an install record
func (r *StoreRepo) DeleteInstall(appID string) error {
	return r.db.Where("app_id = ?", appID).Delete(&model.AppInstall{}).Error
}
