package handlers

import (
	"strconv"

	"github.com/gin-gonic/gin"
	"gorm.io/gorm"

	"aipc/platform/platform-api/model"
	"aipc/platform/platform-api/repo"
)

// StoreHandlers store API handlers
type StoreHandlers struct {
	storeRepo *repo.StoreRepo
}

// NewStoreHandlers creates store handlers
func NewStoreHandlers(db *gorm.DB) *StoreHandlers {
	return &StoreHandlers{
		storeRepo: repo.NewStoreRepo(db),
	}
}

// ListApps gets application list
// GET /api/v1/store/apps?category=&search=&featured=&page=&page_size=
func (h *StoreHandlers) ListApps(c *gin.Context) {
	category := c.Query("category")
	search := c.Query("search")
	featured := c.Query("featured") == "true"

	page, _ := strconv.Atoi(c.DefaultQuery("page", "1"))
	pageSize, _ := strconv.Atoi(c.DefaultQuery("page_size", "20"))
	if page < 1 {
		page = 1
	}
	if pageSize < 1 || pageSize > 100 {
		pageSize = 20
	}

	apps, total, err := h.storeRepo.ListApps(category, search, featured, page, pageSize)
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	// Get tags for each app
	type AppWithTags struct {
		model.StoreApp
		Tags []model.StoreTag `json:"tags"`
	}
	result := make([]AppWithTags, len(apps))
	for i, app := range apps {
		tags, _ := h.storeRepo.GetTagsByAppID(app.ID)
		result[i] = AppWithTags{StoreApp: app, Tags: tags}
	}

	Resp(c).OK(gin.H{
		"apps":      result,
		"total":     total,
		"page":      page,
		"page_size": pageSize,
	})
}

// GetApp gets application details
// GET /api/v1/store/apps/:key
func (h *StoreHandlers) GetApp(c *gin.Context) {
	key := c.Param("key")
	if key == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "app key is required")
		return
	}

	app, err := h.storeRepo.GetAppByKey(key)
	if err != nil {
		if err == gorm.ErrRecordNotFound {
			Resp(c).FailMsg(CodeNotFound, "app not found")
		} else {
			Resp(c).FailMsg(CodeServiceError, err.Error())
		}
		return
	}

	// Get version list
	details, _ := h.storeRepo.ListAppDetails(app.ID)

	// Get tags
	tags, _ := h.storeRepo.GetTagsByAppID(app.ID)

	Resp(c).OK(gin.H{
		"app":      app,
		"versions": details,
		"tags":     tags,
	})
}

// ListCategories gets category list
// GET /api/v1/store/categories
func (h *StoreHandlers) ListCategories(c *gin.Context) {
	categories, err := h.storeRepo.ListCategories()
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	Resp(c).OK(gin.H{"categories": categories})
}

// ListTags gets tag list
// GET /api/v1/store/tags
func (h *StoreHandlers) ListTags(c *gin.Context) {
	tags, err := h.storeRepo.ListTags()
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	Resp(c).OK(gin.H{"tags": tags})
}

// InstallFromStore installs an app from store (gets install configuration)
// POST /api/v1/store/apps/:key/install
func (h *StoreHandlers) InstallFromStore(c *gin.Context) {
	key := c.Param("key")
	if key == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "app key is required")
		return
	}

	var req struct {
		Version string `json:"version"` // Optional, defaults to latest version
		AppID   string `json:"app_id"`  // Optional, custom application instance ID
	}
	c.ShouldBindJSON(&req)

	// Get application
	app, err := h.storeRepo.GetAppByKey(key)
	if err != nil {
		if err == gorm.ErrRecordNotFound {
			Resp(c).FailMsg(CodeNotFound, "app not found")
		} else {
			Resp(c).FailMsg(CodeServiceError, err.Error())
		}
		return
	}

	// Get version details
	var detail *model.StoreAppDetail
	if req.Version != "" {
		detail, err = h.storeRepo.GetDetailByVersion(app.ID, req.Version)
	} else {
		detail, err = h.storeRepo.GetLatestDetail(app.ID)
	}
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "version not found")
		return
	}

	// Return install configuration, frontend uses wizard to complete installation
	Resp(c).OK(gin.H{
		"app":            app,
		"version":        detail,
		"default_config": detail.DefaultConfig,
	})
}

// ========== Installed Application Management ==========

// ListInstalls gets installed application list
// GET /api/v1/store/installs
func (h *StoreHandlers) ListInstalls(c *gin.Context) {
	installs, err := h.storeRepo.ListInstalls()
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	// Associate store application information
	type InstallWithApp struct {
		model.AppInstall
		StoreApp *model.StoreApp `json:"store_app,omitempty"`
	}
	result := make([]InstallWithApp, len(installs))
	for i, install := range installs {
		result[i] = InstallWithApp{AppInstall: install}
		if install.StoreAppID != nil {
			app, _ := h.storeRepo.GetAppByID(*install.StoreAppID)
			result[i].StoreApp = app
		}
	}

	Resp(c).OK(gin.H{
		"installs": result,
		"total":    len(result),
	})
}

// GetInstall gets installed application details
// GET /api/v1/store/installs/:app_id
func (h *StoreHandlers) GetInstall(c *gin.Context) {
	appID := c.Param("app_id")
	if appID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "app_id is required")
		return
	}

	install, err := h.storeRepo.GetInstallByAppID(appID)
	if err != nil {
		if err == gorm.ErrRecordNotFound {
			Resp(c).FailMsg(CodeNotFound, "install not found")
		} else {
			Resp(c).FailMsg(CodeServiceError, err.Error())
		}
		return
	}

	// Associate store application information
	var storeApp *model.StoreApp
	if install.StoreAppID != nil {
		storeApp, _ = h.storeRepo.GetAppByID(*install.StoreAppID)
	}

	Resp(c).OK(gin.H{
		"install":   install,
		"store_app": storeApp,
	})
}

// CreateInstall creates an install record
// POST /api/v1/store/installs
func (h *StoreHandlers) CreateInstall(c *gin.Context) {
	var req struct {
		StoreAppID  *uint  `json:"store_app_id"`
		AppDetailID *uint  `json:"app_detail_id"`
		AppID       string `json:"app_id" binding:"required"`
		Name        string `json:"name" binding:"required"`
		Version     string `json:"version"`
		Description string `json:"description"`
		Image       string `json:"image"`
		Config      string `json:"config"` // JSON configuration
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	// Check if already installed
	if _, err := h.storeRepo.GetInstallByAppID(req.AppID); err == nil {
		Resp(c).FailMsg(CodeInvalidRequest, "app already installed")
		return
	}

	install := &model.AppInstall{
		StoreAppID:  req.StoreAppID,
		AppDetailID: req.AppDetailID,
		AppID:       req.AppID,
		Name:        req.Name,
		Version:     req.Version,
		Description: req.Description,
		Image:       req.Image,
		Status:      "installed",
		Config:      req.Config,
	}

	if err := h.storeRepo.CreateInstall(install); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	// Increment download count
	if req.StoreAppID != nil {
		h.storeRepo.IncrementDownloads(*req.StoreAppID)
	}

	Resp(c).OK(gin.H{"install": install})
}

// UpdateInstall updates an install record
// PUT /api/v1/store/installs/:app_id
func (h *StoreHandlers) UpdateInstall(c *gin.Context) {
	appID := c.Param("app_id")
	if appID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "app_id is required")
		return
	}

	install, err := h.storeRepo.GetInstallByAppID(appID)
	if err != nil {
		if err == gorm.ErrRecordNotFound {
			Resp(c).FailMsg(CodeNotFound, "install not found")
		} else {
			Resp(c).FailMsg(CodeServiceError, err.Error())
		}
		return
	}

	var req struct {
		Status      *string `json:"status"`
		ContainerID *string `json:"container_id"`
		PID         *int    `json:"pid"`
		Message     *string `json:"message"`
		Config      *string `json:"config"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	if req.Status != nil {
		install.Status = *req.Status
	}
	if req.ContainerID != nil {
		install.ContainerID = *req.ContainerID
	}
	if req.PID != nil {
		install.PID = *req.PID
	}
	if req.Message != nil {
		install.Message = *req.Message
	}
	if req.Config != nil {
		install.Config = *req.Config
	}

	if err := h.storeRepo.UpdateInstall(install); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"install": install})
}

// DeleteInstall deletes an install record
// DELETE /api/v1/store/installs/:app_id
func (h *StoreHandlers) DeleteInstall(c *gin.Context) {
	appID := c.Param("app_id")
	if appID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "app_id is required")
		return
	}

	if err := h.storeRepo.DeleteInstall(appID); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"message": "install deleted"})
}
