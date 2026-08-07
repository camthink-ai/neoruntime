package model

import "time"

// ConfigItem is the desired-state row for a single configuration key within a
// domain (e.g. domain="media", key="osd"). It holds the latest desired JSON
// and the monotonically-increasing Revision that points into the
// ConfigRevision history. The (domain, key) pair is globally unique.
//
// This table is the source of truth that the Config Manager reconciles
// against on startup: if a projected file (/data/aipc/etc/*) drifts or is
// lost during an OS upgrade, the file is re-rendered from ValueJSON.
type ConfigItem struct {
	Domain        string    `gorm:"uniqueIndex:idx_config_items_domain_key;not null" json:"domain"`
	Key           string    `gorm:"uniqueIndex:idx_config_items_domain_key;not null" json:"key"`
	ValueJSON     string    `gorm:"type:text" json:"value_json"`
	SchemaVersion int       `gorm:"not null;default:1" json:"schema_version"`
	Revision      int       `gorm:"not null;default:0" json:"revision"`
	UpdatedAt     time.Time `json:"updated_at"`
	UpdatedBy     string    `json:"updated_by"`
}

// TableName overrides the default table name.
func (ConfigItem) TableName() string {
	return "config_items"
}

// ConfigRevision is an append-only history row recording every value a
// (domain, key) has held. The Config Manager appends one row per successful
// Apply (and one for an auto-Restore), so the full change history is queryable
// for audit. (domain, key, revision) is unique.
type ConfigRevision struct {
	ID        uint      `gorm:"primaryKey" json:"id"`
	Domain    string    `gorm:"uniqueIndex:idx_config_revisions_domain_key_rev;not null;index:idx_config_revisions_domain_key" json:"domain"`
	Key       string    `gorm:"uniqueIndex:idx_config_revisions_domain_key_rev;not null;index:idx_config_revisions_domain_key" json:"key"`
	Revision  int       `gorm:"uniqueIndex:idx_config_revisions_domain_key_rev;not null" json:"revision"`
	ValueJSON string    `gorm:"type:text" json:"value_json"`
	Reason    string    `json:"reason"`
	CreatedAt time.Time `json:"created_at"`
	CreatedBy string    `json:"created_by"`
}

// TableName overrides the default table name.
func (ConfigRevision) TableName() string {
	return "config_revisions"
}

// ConfigApplyJob records the lifecycle of a single Apply attempt: running →
// success | failed. A failed Verify triggers an auto-Restore, which is itself
// recorded as a separate job with Action="restore". The ID is a caller-supplied
// UUID so the handler can return it to the client before Apply completes.
type ConfigApplyJob struct {
	ID           string     `gorm:"primaryKey" json:"id"`
	Domain       string     `gorm:"not null;index" json:"domain"`
	Key          string     `gorm:"not null" json:"key"`
	Action       string     `gorm:"not null" json:"action"` // apply | restore
	Status       string     `gorm:"not null" json:"status"` // running | success | failed
	StartedAt    time.Time  `json:"started_at"`
	FinishedAt   *time.Time `json:"finished_at"`
	Error        string     `json:"error"`
	FromRevision int        `json:"from_revision"`
	ToRevision   int        `json:"to_revision"`
}

// TableName overrides the default table name.
func (ConfigApplyJob) TableName() string {
	return "config_apply_jobs"
}
