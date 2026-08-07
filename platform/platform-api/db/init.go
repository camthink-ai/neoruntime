package db

import (
	"fmt"
	"os"
	"path/filepath"

	"github.com/glebarez/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"

	"aipc/platform/platform-api/model"
)

// Init initializes the SQLite database connection and performs auto-migration.
// dbPath is the path to the SQLite database file (e.g., /data/aipc/data/platform.db).
func Init(dbPath string) (*gorm.DB, error) {
	// Ensure parent directory exists
	dir := filepath.Dir(dbPath)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return nil, fmt.Errorf("failed to create database directory %s: %w", dir, err)
	}

	// Open database with WAL mode for better concurrency
	dsn := dbPath + "?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)"
	db, err := gorm.Open(sqlite.Open(dsn), &gorm.Config{
		Logger: logger.Default.LogMode(logger.Warn),
	})
	if err != nil {
		return nil, fmt.Errorf("failed to open database: %w", err)
	}

	// Auto-migrate all models
	if err := db.AutoMigrate(
		&model.AIModel{},
		&model.Setting{},
		&model.EventRecord{},
		&model.EventLog{},
		// Store models
		&model.StoreApp{},
		&model.StoreAppDetail{},
		&model.StoreCategory{},
		&model.StoreTag{},
		&model.StoreAppTag{},
		&model.AppInstall{},
		// Dev models
		&model.BaseImage{},
		&model.AppProject{},
		&model.AppBuild{},
		// Config Controller (desired-state DB + history + apply jobs)
		&model.ConfigItem{},
		&model.ConfigRevision{},
		&model.ConfigApplyJob{},
	); err != nil {
		return nil, fmt.Errorf("failed to auto-migrate database: %w", err)
	}

	return db, nil
}
