package handlers

import (
	"errors"
	"strconv"

	"github.com/gin-gonic/gin"
	"gorm.io/gorm"
)

// Config job audit handlers (read-only). These expose the Config Controller's
// apply-job history so operators can trace every apply/delete/reconcile
// attempt — including the Phase 2 startup reconcile that re-projects config on
// drift or imports a pre-existing live file into an empty desired-state store.
//
// They read configRepo directly (no Manager state machine): audit is a pure
// read over config_apply_jobs, mirroring GetAllSettings' direct-repo pattern.

// GetConfigJob returns a single ConfigApplyJob by ID.
//
// GET /api/v1/config/jobs/:id
func (h *APIHandlers) GetConfigJob(c *gin.Context) {
	if h.configRepo == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Database not available")
		return
	}
	id := c.Param("id")
	if id == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Job ID is required")
		return
	}
	job, err := h.configRepo.GetJob(id)
	if err != nil {
		if errors.Is(err, gorm.ErrRecordNotFound) {
			Resp(c).FailMsg(CodeNotFound, "Job not found: "+id)
			return
		}
		Resp(c).FailMsg(CodeDatabaseError, err.Error())
		return
	}
	Resp(c).OK(gin.H{"job": job})
}

// ListConfigJobs lists ConfigApplyJob rows newest-first. The optional domain
// query parameter filters by domain (e.g. "media"); limit caps the page size
// (clamped to 100 by the repo when unset or out of range).
//
// GET /api/v1/config/jobs?domain=media&limit=50
func (h *APIHandlers) ListConfigJobs(c *gin.Context) {
	if h.configRepo == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Database not available")
		return
	}
	domain := c.Query("domain")
	limit := 0
	if v := c.Query("limit"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			limit = n
		}
	}
	jobs, err := h.configRepo.ListJobs(domain, limit)
	if err != nil {
		Resp(c).FailMsg(CodeDatabaseError, err.Error())
		return
	}
	Resp(c).OK(gin.H{"jobs": jobs, "count": len(jobs)})
}
