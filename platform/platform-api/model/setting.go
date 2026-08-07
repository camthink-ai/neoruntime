package model

// Setting represents a key-value configuration entry stored in the database.
type Setting struct {
	ID    uint   `gorm:"primaryKey" json:"id"`
	Key   string `gorm:"uniqueIndex;not null" json:"key"`
	Value string `gorm:"type:text" json:"value"`
}

// TableName overrides the default table name.
func (Setting) TableName() string {
	return "settings"
}
