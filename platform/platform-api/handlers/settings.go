package handlers

import (
	"encoding/json"

	"github.com/gin-gonic/gin"

	"aipc/platform/common/events"
)

// Settings handlers

func (h *APIHandlers) GetAllSettings(c *gin.Context) {
	if h.settingRepo == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Database not available")
		return
	}

	settings, err := h.settingRepo.GetAll()
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{"settings": settings})
}

func (h *APIHandlers) SetSetting(c *gin.Context) {
	if h.settingRepo == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Database not available")
		return
	}

	var req struct {
		Key   string `json:"key" binding:"required"`
		Value string `json:"value"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	// Route through the Config Controller Manager so the write gets revision
	// history, an apply-job record, and the config.changed audit event (the
	// Manager logs it; we no longer log here to avoid double-logging). The
	// adapter does the actual settingRepo.Set + readback Verify. Falls back to
	// the direct repo path only if the Manager was not constructed (no DB).
	if h.configMgr != nil {
		desired, err := json.Marshal(struct {
			Value string `json:"value"`
		}{Value: req.Value})
		if err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, "Failed to encode value: "+err.Error())
			return
		}
		if _, _, err := h.configMgr.Apply(c.Request.Context(), "system", req.Key, string(desired), getUsernameFromContext(c)); err != nil {
			Resp(c).FailMsg(CodeOperationFailed, err.Error())
			return
		}
		Resp(c).OK(gin.H{"key": req.Key, "value": req.Value})
		return
	}

	// No DB (configMgr is nil): preserve the original direct-set behavior.
	if h.settingRepo == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Database not available")
		return
	}

	// Get old value for logging
	var oldValue string
	if existing, err := h.settingRepo.Get(req.Key); err == nil {
		oldValue = existing
	}

	if err := h.settingRepo.Set(req.Key, req.Value); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	// Log configuration change
	if h.eventLogger != nil && oldValue != req.Value {
		h.eventLogger.LogWithCodeAsync("config.changed", events.MessageParams{
			"section": "settings",
			"changes": req.Key + ": " + oldValue + " → " + req.Value,
		}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"key": req.Key, "value": req.Value})
}

func (h *APIHandlers) DeleteSetting(c *gin.Context) {
	if h.settingRepo == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Database not available")
		return
	}

	key := c.Param("key")
	if key == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Setting key is required")
		return
	}

	// Route through the Config Controller Manager so the desired-state mirror
	// stays consistent with the live KV. Manager.Delete clears the config_items
	// row — without it, the live delete below would leave an orphan desired row
	// that Phase 2 reconcile would resurrect as if the setting had never been
	// deleted — and records a "delete" revision + apply-job for audit. It does
	// NOT touch the live setting; settingRepo.Delete below does that.
	//
	// Order matters: clear desired BEFORE the live delete so a failure between
	// the two leaves a desired-less live setting (Phase 2 reconcile won't
	// resurrect it) rather than a live-less orphan desired row (which would be
	// resurrected). Fail closed: if the bookkeeping fails, the live setting is
	// left intact so live and desired stay consistent. Falls back to the direct
	// repo path only if the Manager was not constructed (no DB).
	if h.configMgr != nil {
		var oldValue string
		if existing, err := h.settingRepo.Get(key); err == nil {
			oldValue = existing
		}
		if _, _, err := h.configMgr.Delete(c.Request.Context(), "system", key, getUsernameFromContext(c)); err != nil {
			Resp(c).FailMsg(CodeOperationFailed, err.Error())
			return
		}
		if err := h.settingRepo.Delete(key); err != nil {
			Resp(c).FailMsg(CodeServiceError, err.Error())
			return
		}
		// Manager.Delete records the apply-job; the config.changed event is still
		// logged here (the Manager only emits it for Apply, not Delete).
		if h.eventLogger != nil && oldValue != "" {
			h.eventLogger.LogWithCodeAsync("config.changed", events.MessageParams{
				"section": "settings",
				"changes": key + " deleted (was: " + oldValue + ")",
			}, getUsernameFromContext(c))
		}
		Resp(c).OK(gin.H{"deleted": key})
		return
	}

	// No DB (configMgr is nil): preserve the original direct-delete behavior.
	var oldValue string
	if existing, err := h.settingRepo.Get(key); err == nil {
		oldValue = existing
	}

	if err := h.settingRepo.Delete(key); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	// Log configuration deletion
	if h.eventLogger != nil && oldValue != "" {
		h.eventLogger.LogWithCodeAsync("config.changed", events.MessageParams{
			"section": "settings",
			"changes": key + " deleted (was: " + oldValue + ")",
		}, getUsernameFromContext(c))
	}

	Resp(c).OK(gin.H{"deleted": key})
}
