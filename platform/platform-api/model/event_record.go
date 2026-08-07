package model

import "time"

// EventRecord represents a persisted event/alert record (P1 — table created now, write logic later).
type EventRecord struct {
	ID        uint      `gorm:"primaryKey" json:"id"`
	Topic     string    `gorm:"index" json:"topic"`
	Payload   string    `gorm:"type:text" json:"payload"`
	Source    string    `json:"source"`
	Severity  string    `json:"severity"` // info, warning, critical
	CreatedAt time.Time `gorm:"index" json:"created_at"`
}

// TableName overrides the default table name.
func (EventRecord) TableName() string {
	return "event_records"
}
