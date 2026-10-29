package db

import (
	"fmt"
	"path/filepath"
	"sync"
	"testing"

	"aipc/platform/platform-api/repo"
)

// TestInit_PinsSingleConnection asserts the production DB setup serializes
// in-process access: SQLite allows one writer at a time, and the WAL
// BUSY_SNAPSHOT failure (deferred read-then-write transactions like
// ConfigRepo.AppendRevision failing to upgrade their lock) is exempt from
// busy_timeout. A single pooled connection is the only guard against it.
func TestInit_PinsSingleConnection(t *testing.T) {
	gdb, err := Init(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatalf("Init: %v", err)
	}
	t.Cleanup(func() {
		if sqlDB, err := gdb.DB(); err == nil {
			_ = sqlDB.Close()
		}
	})

	sqlDB, err := gdb.DB()
	if err != nil {
		t.Fatalf("db.DB(): %v", err)
	}
	if got := sqlDB.Stats().MaxOpenConnections; got != 1 {
		t.Fatalf("MaxOpenConnections = %d, want 1", got)
	}
}

// TestInit_DurablePragmas pins the DSN's durability contract: WAL journaling
// plus synchronous=NORMAL. NORMAL is the documented WAL pairing — no
// per-commit fsync, consistent across abrupt power loss, and only the last
// few seconds of commits are at risk. A DSN edit that silently reverts to
// FULL (per-commit fsync stalls every read queued on the single connection)
// or to OFF (corruption on power loss) must fail this test.
func TestInit_DurablePragmas(t *testing.T) {
	gdb, err := Init(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatalf("Init: %v", err)
	}
	t.Cleanup(func() {
		if sqlDB, err := gdb.DB(); err == nil {
			_ = sqlDB.Close()
		}
	})

	var syncMode int
	if err := gdb.Raw("PRAGMA synchronous").Scan(&syncMode).Error; err != nil {
		t.Fatalf("PRAGMA synchronous: %v", err)
	}
	// 0=OFF, 1=NORMAL, 2=FULL
	if syncMode != 1 {
		t.Fatalf("synchronous = %d, want 1 (NORMAL)", syncMode)
	}

	var journalMode string
	if err := gdb.Raw("PRAGMA journal_mode").Scan(&journalMode).Error; err != nil {
		t.Fatalf("PRAGMA journal_mode: %v", err)
	}
	if journalMode != "wal" {
		t.Fatalf("journal_mode = %q, want %q", journalMode, "wal")
	}
}

// TestInit_ConcurrentWritesNoLockFailures hammers the production DB setup
// with concurrent writers across the setting and config tables. Every write
// must succeed (no "database is locked"/SQLITE_BUSY errors), and the
// revision history must show a gapless 1..N sequence — MAX+1 assignments
// only stay collision-free if appends truly serialize.
func TestInit_ConcurrentWritesNoLockFailures(t *testing.T) {
	gdb, err := Init(filepath.Join(t.TempDir(), "test.db"))
	if err != nil {
		t.Fatalf("Init: %v", err)
	}
	t.Cleanup(func() {
		if sqlDB, err := gdb.DB(); err == nil {
			_ = sqlDB.Close()
		}
	})

	const writers = 8
	const perWriter = 25
	settingRepo := repo.NewSettingRepo(gdb)
	configRepo := repo.NewConfigRepo(gdb)

	var wg sync.WaitGroup
	errCh := make(chan error, writers*perWriter*2)
	for w := 0; w < writers; w++ {
		wg.Add(1)
		go func(w int) {
			defer wg.Done()
			for i := 0; i < perWriter; i++ {
				key := fmt.Sprintf("writer-%d-key-%d", w, i)
				if err := settingRepo.Set(key, "v"); err != nil {
					errCh <- fmt.Errorf("Set(%s): %w", key, err)
				}
				if _, err := configRepo.AppendRevision("media", "osd", "{}", "test", "hammer"); err != nil {
					errCh <- fmt.Errorf("AppendRevision: %w", err)
				}
			}
		}(w)
	}
	wg.Wait()
	close(errCh)
	for err := range errCh {
		t.Error(err)
	}

	revs, err := configRepo.ListRevisions("media", "osd")
	if err != nil {
		t.Fatalf("ListRevisions: %v", err)
	}
	want := writers * perWriter
	if len(revs) != want {
		t.Fatalf("revision count = %d, want %d (lost appends)", len(revs), want)
	}
	seen := make(map[int]bool, want)
	for _, rev := range revs {
		if rev.Revision < 1 || rev.Revision > want || seen[rev.Revision] {
			t.Fatalf("revision sequence broken: got %d (len=%d)", rev.Revision, len(revs))
		}
		seen[rev.Revision] = true
	}
}
