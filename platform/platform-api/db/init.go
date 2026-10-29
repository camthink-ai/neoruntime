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

	// Open database with WAL mode for better concurrency.
	// synchronous=NORMAL (the documented pairing with WAL) drops the
	// per-commit fsync: process crash and clean reboot lose nothing, abrupt
	// power loss rolls back the last few seconds of commits without database
	// corruption. Everything stored here (settings, model registry, event
	// logs, config revisions) is redoable, and the fsync it removes used to
	// stall all reads queued on the single pooled connection below.
	dsn := dbPath + "?_pragma=journal_mode(WAL)&_pragma=busy_timeout(5000)&_pragma=synchronous(NORMAL)"
	db, err := gorm.Open(sqlite.Open(dsn), &gorm.Config{
		Logger: logger.Default.LogMode(logger.Warn),
	})
	if err != nil {
		return nil, fmt.Errorf("failed to open database: %w", err)
	}

	// SQLite allows a single writer at a time. Pinning the pool to one
	// connection serializes all in-process DB access, which eliminates
	// writer-writer contention — including the WAL BUSY_SNAPSHOT failure on
	// deferred-transaction lock upgrades (read-then-write transactions),
	// which busy_timeout does NOT cover. busy_timeout stays in the DSN only
	// to cover cross-process lock waits. All queries here are short, so
	// queueing behind the single connection is negligible.
	// INVARIANT: with one pooled connection, any Transaction callback that
	// queries the outer *gorm.DB (instead of tx) deadlocks permanently.
	sqlDB, err := db.DB()
	if err != nil {
		return nil, fmt.Errorf("failed to access database handle: %w", err)
	}
	sqlDB.SetMaxOpenConns(1)
	sqlDB.SetMaxIdleConns(1)

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
