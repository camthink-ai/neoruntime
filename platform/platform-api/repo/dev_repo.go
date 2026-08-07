package repo

import (
	"aipc/platform/platform-api/model"

	"gorm.io/gorm"
)

// DevRepo development workbench data access
type DevRepo struct {
	db *gorm.DB
}

// NewDevRepo creates a development data access instance
func NewDevRepo(db *gorm.DB) *DevRepo {
	return &DevRepo{db: db}
}

// ========== BaseImage ==========

// ListBaseImages gets base image list
func (r *DevRepo) ListBaseImages() ([]model.BaseImage, error) {
	var images []model.BaseImage
	if err := r.db.Where("status = ?", "active").Order("language, name").Find(&images).Error; err != nil {
		return nil, err
	}
	return images, nil
}

// GetBaseImage gets a base image
func (r *DevRepo) GetBaseImage(id uint) (*model.BaseImage, error) {
	var image model.BaseImage
	if err := r.db.First(&image, id).Error; err != nil {
		return nil, err
	}
	return &image, nil
}

// CreateBaseImage creates a base image
func (r *DevRepo) CreateBaseImage(image *model.BaseImage) error {
	return r.db.Create(image).Error
}

// ========== AppProject ==========

// ListProjects gets project list
func (r *DevRepo) ListProjects() ([]model.AppProject, error) {
	var projects []model.AppProject
	if err := r.db.Order("updated_at DESC").Find(&projects).Error; err != nil {
		return nil, err
	}
	return projects, nil
}

// GetProject gets a project
func (r *DevRepo) GetProject(id uint) (*model.AppProject, error) {
	var project model.AppProject
	if err := r.db.First(&project, id).Error; err != nil {
		return nil, err
	}
	return &project, nil
}

// GetProjectByKey gets a project by Key
func (r *DevRepo) GetProjectByKey(key string) (*model.AppProject, error) {
	var project model.AppProject
	if err := r.db.Where("key = ?", key).First(&project).Error; err != nil {
		return nil, err
	}
	return &project, nil
}

// CreateProject creates a project
func (r *DevRepo) CreateProject(project *model.AppProject) error {
	return r.db.Create(project).Error
}

// UpdateProject updates a project
func (r *DevRepo) UpdateProject(project *model.AppProject) error {
	return r.db.Save(project).Error
}

// DeleteProject deletes a project
func (r *DevRepo) DeleteProject(id uint) error {
	return r.db.Delete(&model.AppProject{}, id).Error
}

// ========== AppBuild ==========

// ListBuilds gets build records
func (r *DevRepo) ListBuilds(projectID uint) ([]model.AppBuild, error) {
	var builds []model.AppBuild
	if err := r.db.Where("project_id = ?", projectID).Order("created_at DESC").Find(&builds).Error; err != nil {
		return nil, err
	}
	return builds, nil
}

// GetBuild gets a build record
func (r *DevRepo) GetBuild(id uint) (*model.AppBuild, error) {
	var build model.AppBuild
	if err := r.db.First(&build, id).Error; err != nil {
		return nil, err
	}
	return &build, nil
}

// CreateBuild creates a build record
func (r *DevRepo) CreateBuild(build *model.AppBuild) error {
	return r.db.Create(build).Error
}

// UpdateBuild updates a build record
func (r *DevRepo) UpdateBuild(build *model.AppBuild) error {
	return r.db.Save(build).Error
}
