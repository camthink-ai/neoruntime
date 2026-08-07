package handlers

import (
	"aipc/platform/common/events"
	"aipc/platform/platform-api/model"
	"encoding/json"
	"time"

	"github.com/gin-gonic/gin"
	"gorm.io/gorm"
)

// EventLogHandler handles user event logs
type EventLogHandler struct {
	db     *gorm.DB
	logger *events.Logger
}

// NewEventLogHandler creates a new event log handler
func NewEventLogHandler(db *gorm.DB, logger *events.Logger) *EventLogHandler {
	return &EventLogHandler{
		db:     db,
		logger: logger,
	}
}

// ListEventLogsRequest represents the query parameters for listing event logs
type ListEventLogsRequest struct {
	Category  string `form:"category"`
	Level     string `form:"level"`
	StartTime string `form:"start_time"`
	EndTime   string `form:"end_time"`
	Search    string `form:"search"`
	Limit     int    `form:"limit" binding:"gte=0,lte=1000"`
	Offset    int    `form:"offset" binding:"gte=0"`
}

// List returns user event logs with optional filters
func (h *EventLogHandler) List(c *gin.Context) {
	var req ListEventLogsRequest
	if err := c.ShouldBindQuery(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	// Set default limit
	if req.Limit == 0 {
		req.Limit = 100
	}

	query := h.db.Table("event_logs")

	// Filter by category
	if req.Category != "" && req.Category != "all" {
		query = query.Where("category = ?", req.Category)
	}

	// Filter by level
	if req.Level != "" {
		query = query.Where("level = ?", req.Level)
	}

	// Filter by time range
	if req.StartTime != "" {
		if startTime, err := time.Parse(time.RFC3339, req.StartTime); err == nil {
			query = query.Where("timestamp >= ?", startTime)
		}
	}
	if req.EndTime != "" {
		if endTime, err := time.Parse(time.RFC3339, req.EndTime); err == nil {
			query = query.Where("timestamp <= ?", endTime)
		}
	}

	// Search by keyword (matches English message, Chinese message, and event type code)
	if req.Search != "" {
		searchPattern := "%" + req.Search + "%"
		query = query.Where("message LIKE ? OR message_zh LIKE ? OR event_type LIKE ?", searchPattern, searchPattern, searchPattern)
	}

	// Get total count
	var total int64
	if err := query.Count(&total).Error; err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to count event logs")
		return
	}

	// Paginated query
	var entries []model.EventLog
	if err := query.Order("timestamp DESC, created_at DESC").
		Limit(req.Limit).
		Offset(req.Offset).
		Find(&entries).Error; err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to fetch event logs")
		return
	}

	// Convert Data from JSON string to parsed object for frontend
	type eventLogResponse struct {
		ID        uint                   `json:"id"`
		EventID   string                 `json:"event_id"`
		Timestamp time.Time              `json:"timestamp"`
		EventType string                 `json:"event_type"`
		Level     string                 `json:"level"`
		Category  string                 `json:"category"`
		Source    string                 `json:"source"`
		User      string                 `json:"user,omitempty"`
		Message   string                 `json:"message"`
		MessageZh string                 `json:"message_zh"`
		Data      map[string]interface{} `json:"data,omitempty"`
		CreatedAt time.Time              `json:"created_at"`
	}

	respEntries := make([]eventLogResponse, 0, len(entries))
	for _, e := range entries {
		resp := eventLogResponse{
			ID: e.ID, EventID: e.EventID, Timestamp: e.Timestamp,
			EventType: e.EventType, Level: e.Level, Category: e.Category,
			Source: e.Source, User: e.User, Message: e.Message, MessageZh: e.MessageZh,
			CreatedAt: e.CreatedAt,
		}
		if e.Data != "" {
			_ = json.Unmarshal([]byte(e.Data), &resp.Data)
		}
		respEntries = append(respEntries, resp)
	}

	Resp(c).OK(gin.H{
		"total":   total,
		"limit":   req.Limit,
		"offset":  req.Offset,
		"entries": respEntries,
	})
}

// GetStatistics returns event statistics
func (h *EventLogHandler) GetStatistics(c *gin.Context) {
	now := time.Now()
	today := time.Date(now.Year(), now.Month(), now.Day(), 0, 0, 0, 0, now.Location())

	var stats struct {
		TodayErrors   int64 `json:"today_errors"`
		TodayWarnings int64 `json:"today_warnings"`
		TodayCritical int64 `json:"today_critical"`
		Operations    int64 `json:"operations"`
		SystemEvents  int64 `json:"system_events"`
		TotalEntries  int64 `json:"total_entries"`
	}

	// Count today's errors and warnings
	h.db.Table("event_logs").
		Where("timestamp >= ?", today).
		Where("level IN ?", []string{"error", "critical"}).
		Count(&stats.TodayErrors)

	h.db.Table("event_logs").
		Where("timestamp >= ?", today).
		Where("level = ?", "warning").
		Count(&stats.TodayWarnings)

	h.db.Table("event_logs").
		Where("timestamp >= ?", today).
		Where("level = ?", "critical").
		Count(&stats.TodayCritical)

	// Count by category
	h.db.Table("event_logs").
		Where("category = ?", "operation").
		Count(&stats.Operations)

	h.db.Table("event_logs").
		Where("category = ?", "system").
		Count(&stats.SystemEvents)

	// Total count
	h.db.Table("event_logs").Count(&stats.TotalEntries)

	Resp(c).OK(stats)
}

// CleanupRequest represents the cleanup request body
type CleanupRequest struct {
	Days int `json:"days" binding:"required,gte=1,lte=365"`
}

// Cleanup removes old event logs
func (h *EventLogHandler) Cleanup(c *gin.Context) {
	var req CleanupRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	if err := h.logger.CleanupOldEvents(req.Days); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to cleanup old events")
		return
	}

	Resp(c).OK(gin.H{
		"message": "Old event logs cleaned up successfully",
		"days":    req.Days,
	})
}

// GetTemplates returns all event message templates with localized messages
func (h *EventLogHandler) GetTemplates(c *gin.Context) {
	allTemplates := events.GetAllTemplates()

	type templateEntry struct {
		En     string   `json:"en"`
		Zh     string   `json:"zh"`
		Module string   `json:"module"`
		Level  string   `json:"level"`
		Params []string `json:"params"`
	}

	result := make(map[string]templateEntry, len(allTemplates))
	for code, tmpl := range allTemplates {
		result[code] = templateEntry{
			En:     tmpl.Default,
			Zh:     events.GetZhMessage(code),
			Module: tmpl.Module,
			Level:  string(tmpl.Level),
			Params: tmpl.Params,
		}
	}

	Resp(c).OK(gin.H{"templates": result})
}

// LogRecord represents the request body for creating an event log
type LogRecord struct {
	EventType string                 `json:"event_type" binding:"required"`
	Level     string                 `json:"level"`
	Category  string                 `json:"category"`
	Source    string                 `json:"source" binding:"required"`
	User      string                 `json:"user"`
	Message   string                 `json:"message" binding:"required"`
	Data      map[string]interface{} `json:"data"`
}

// Create creates a new event log entry (for testing/manual logging)
func (h *EventLogHandler) Create(c *gin.Context) {
	var req LogRecord
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	event := &events.Event{
		Type:      events.EventType(req.EventType),
		Level:     events.EventLevel(req.Level),
		Category:  events.EventCategory(req.Category),
		Source:    req.Source,
		User:      req.User,
		Message:   req.Message,
		Data:      req.Data,
		Timestamp: time.Now().Format(time.RFC3339),
	}

	if err := h.logger.Log(event); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create event log")
		return
	}

	Resp(c).OK(gin.H{
		"message": "Event log created successfully",
	})
}
