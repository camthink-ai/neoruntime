package model

import (
	"time"
)

// EventLog represents a user-visible event log entry
type EventLog struct {
	ID        uint      `gorm:"primaryKey" json:"id"`
	EventID   string    `gorm:"uniqueIndex;size:64" json:"event_id"`
	Timestamp time.Time `gorm:"index" json:"timestamp"`
	EventType string    `gorm:"index;size:64" json:"event_type"`
	Level     string    `gorm:"index;size:16" json:"level"`
	Category  string    `gorm:"index;size:32" json:"category"`
	Source    string    `gorm:"index;size:64" json:"source"`
	User      string    `gorm:"index;size:64" json:"user,omitempty"`
	Message   string    `gorm:"type:text" json:"message"`
	MessageZh string    `gorm:"type:text" json:"message_zh"`
	Data      string    `gorm:"type:text" json:"data,omitempty"`
	CreatedAt time.Time `gorm:"index" json:"created_at"`
}

// TableName specifies the table name for EventLog
func (EventLog) TableName() string {
	return "event_logs"
}
