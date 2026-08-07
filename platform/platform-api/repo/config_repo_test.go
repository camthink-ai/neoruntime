package repo

import (
	"errors"
	"path/filepath"
	"testing"
	"time"

	"github.com/glebarez/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"

	"aipc/platform/platform-api/model"
)

// newTestRepo opens a fresh SQLite DB and migrates only the Config Controller
// models. It deliberately does NOT call db.Init: db (via seed.go) imports the
// repo package, so importing db from a repo test would form an import cycle.
// Migrating the three config tables inline is enough to exercise ConfigRepo.
func newTestRepo(t *testing.T) *ConfigRepo {
	t.Helper()
	dbPath := filepath.Join(t.TempDir(), "test.db")
	gdb, err := gorm.Open(sqlite.Open(dbPath), &gorm.Config{
		Logger: logger.Default.LogMode(logger.Warn),
	})
	if err != nil {
		t.Fatalf("gorm.Open: %v", err)
	}
	t.Cleanup(func() {
		if sqlDB, err := gdb.DB(); err == nil {
			_ = sqlDB.Close()
		}
	})
	if err := gdb.AutoMigrate(
		&model.ConfigItem{}, &model.ConfigRevision{}, &model.ConfigApplyJob{},
	); err != nil {
		t.Fatalf("AutoMigrate: %v", err)
	}
	return NewConfigRepo(gdb)
}

func TestGet_NotFound(t *testing.T) {
	r := newTestRepo(t)
	item, found, err := r.Get("media", "osd")
	if err != nil {
		t.Fatalf("Get: %v", err)
	}
	if found || item != nil {
		t.Fatalf("found=%v item=%v, want not found", found, item)
	}
}

func TestUpsertThenGet(t *testing.T) {
	r := newTestRepo(t)
	now := time.Now().UTC().Truncate(time.Second)
	if err := r.Upsert(&model.ConfigItem{
		Domain: "media", Key: "osd", ValueJSON: `{"v":1}`,
		SchemaVersion: 1, Revision: 1, UpdatedAt: now, UpdatedBy: "alice",
	}); err != nil {
		t.Fatalf("Upsert: %v", err)
	}
	item, found, err := r.Get("media", "osd")
	if err != nil || !found {
		t.Fatalf("Get after Upsert: found=%v err=%v", found, err)
	}
	if item.ValueJSON != `{"v":1}` || item.Revision != 1 || item.UpdatedBy != "alice" {
		t.Fatalf("unexpected item: %+v", item)
	}
}

func TestUpsert_OverwritesExisting(t *testing.T) {
	r := newTestRepo(t)
	mk := func(rev int) *model.ConfigItem {
		return &model.ConfigItem{Domain: "media", Key: "osd", ValueJSON: `{"v":` + itoa(rev) + `}`,
			Revision: rev, UpdatedAt: time.Now().UTC(), UpdatedBy: "u"}
	}
	if err := r.Upsert(mk(1)); err != nil {
		t.Fatalf("Upsert 1: %v", err)
	}
	if err := r.Upsert(mk(2)); err != nil {
		t.Fatalf("Upsert 2: %v", err)
	}
	item, found, _ := r.Get("media", "osd")
	if !found || item.Revision != 2 || item.ValueJSON != `{"v":2}` {
		t.Fatalf("after 2 upserts: found=%v item=%+v", found, item)
	}
	all, err := r.GetAll("media")
	if err != nil {
		t.Fatalf("GetAll: %v", err)
	}
	if len(all) != 1 {
		t.Fatalf("GetAll len = %d, want 1", len(all))
	}
}

func TestUpsert_DistinctKeysCoexist(t *testing.T) {
	r := newTestRepo(t)
	upsert := func(key string) {
		if err := r.Upsert(&model.ConfigItem{Domain: "media", Key: key, ValueJSON: "{}",
			Revision: 1, UpdatedAt: time.Now().UTC(), UpdatedBy: "u"}); err != nil {
			t.Fatalf("Upsert %s: %v", key, err)
		}
	}
	upsert("osd")
	upsert("privacy-mask")
	upsert("encoder")
	all, err := r.GetAll("media")
	if err != nil {
		t.Fatalf("GetAll: %v", err)
	}
	if len(all) != 3 {
		t.Fatalf("GetAll len = %d, want 3", len(all))
	}
}

func TestDelete(t *testing.T) {
	r := newTestRepo(t)
	if err := r.Upsert(&model.ConfigItem{Domain: "media", Key: "osd", ValueJSON: "{}",
		Revision: 1, UpdatedAt: time.Now().UTC(), UpdatedBy: "u"}); err != nil {
		t.Fatalf("Upsert: %v", err)
	}
	if err := r.Delete("media", "osd"); err != nil {
		t.Fatalf("Delete: %v", err)
	}
	if _, found, _ := r.Get("media", "osd"); found {
		t.Fatal("item still present after Delete")
	}
}

func TestAppendRevision_Monotonic(t *testing.T) {
	r := newTestRepo(t)
	rev1, err := r.AppendRevision("media", "osd", `{"v":1}`, "apply", "alice")
	if err != nil {
		t.Fatalf("AppendRevision 1: %v", err)
	}
	rev2, err := r.AppendRevision("media", "osd", `{"v":2}`, "apply", "bob")
	if err != nil {
		t.Fatalf("AppendRevision 2: %v", err)
	}
	if rev1 != 1 || rev2 != 2 {
		t.Fatalf("revisions = %d,%d, want 1,2", rev1, rev2)
	}
	revs, err := r.ListRevisions("media", "osd")
	if err != nil {
		t.Fatalf("ListRevisions: %v", err)
	}
	if len(revs) != 2 || revs[0].Revision != 1 || revs[1].Revision != 2 {
		t.Fatalf("revisions = %+v", revs)
	}
	if revs[0].CreatedBy != "alice" || revs[1].CreatedBy != "bob" {
		t.Fatalf("createdBy not preserved: %+v", revs)
	}
}

func TestAppendRevision_IndependentKeys(t *testing.T) {
	r := newTestRepo(t)
	a, _ := r.AppendRevision("media", "osd", `{}`, "apply", "u")
	b, _ := r.AppendRevision("media", "encoder", `{}`, "apply", "u")
	if a != 1 || b != 1 {
		t.Fatalf("independent keys should each start at rev 1; got %d,%d", a, b)
	}
}

func TestJobLifecycle(t *testing.T) {
	r := newTestRepo(t)
	start := time.Now().UTC()
	job := &model.ConfigApplyJob{
		ID: "job-1", Domain: "media", Key: "osd", Action: "apply",
		Status: "running", StartedAt: start, FromRevision: 0, ToRevision: 0,
	}
	if err := r.CreateJob(job); err != nil {
		t.Fatalf("CreateJob: %v", err)
	}
	if err := r.FinishJob("job-1", "success", "", 1); err != nil {
		t.Fatalf("FinishJob: %v", err)
	}
	var got model.ConfigApplyJob
	if err := r.db.First(&got, "id = ?", "job-1").Error; err != nil {
		t.Fatalf("reload job: %v", err)
	}
	if got.Status != "success" || got.ToRevision != 1 || got.FinishedAt == nil {
		t.Fatalf("job after FinishJob = %+v", got)
	}
}

func TestFinishJob_FailedWithError(t *testing.T) {
	r := newTestRepo(t)
	job := &model.ConfigApplyJob{ID: "job-2", Domain: "media", Key: "osd",
		Action: "apply", Status: "running", StartedAt: time.Now().UTC()}
	if err := r.CreateJob(job); err != nil {
		t.Fatalf("CreateJob: %v", err)
	}
	if err := r.FinishJob("job-2", "failed", "verify: bad field", 0); err != nil {
		t.Fatalf("FinishJob: %v", err)
	}
	var got model.ConfigApplyJob
	r.db.First(&got, "id = ?", "job-2")
	if got.Status != "failed" || got.Error != "verify: bad field" {
		t.Fatalf("failed job = %+v", got)
	}
}

// TestGetJob_FoundAndMissing verifies GetJob returns the row by ID and yields
// gorm.ErrRecordNotFound for a missing job (the handler maps this to 404).
func TestGetJob_FoundAndMissing(t *testing.T) {
	r := newTestRepo(t)
	start := time.Now().UTC()
	if err := r.CreateJob(&model.ConfigApplyJob{
		ID: "job-x", Domain: "media", Key: "osd", Action: "reconcile",
		Status: "success", StartedAt: start, Error: "in-sync", ToRevision: 1,
	}); err != nil {
		t.Fatalf("CreateJob: %v", err)
	}
	got, err := r.GetJob("job-x")
	if err != nil {
		t.Fatalf("GetJob: %v", err)
	}
	if got.ID != "job-x" || got.Domain != "media" || got.Error != "in-sync" || got.ToRevision != 1 {
		t.Fatalf("GetJob returned %+v", got)
	}
	if _, err := r.GetJob("missing"); !errors.Is(err, gorm.ErrRecordNotFound) {
		t.Fatalf("GetJob(missing) err = %v, want ErrRecordNotFound", err)
	}
}

// TestListJobs_DomainFilterLimitOrder seeds jobs across two domains with
// staggered start times and asserts newest-first ordering, domain filtering,
// and the limit clamp.
func TestListJobs_DomainFilterLimitOrder(t *testing.T) {
	r := newTestRepo(t)
	base := time.Date(2026, 7, 14, 12, 0, 0, 0, time.UTC)
	mk := func(id, domain string, off int) *model.ConfigApplyJob {
		return &model.ConfigApplyJob{
			ID: id, Domain: domain, Key: "osd", Action: "apply",
			Status: "success", StartedAt: base.Add(time.Duration(off) * time.Second),
		}
	}
	for _, j := range []*model.ConfigApplyJob{
		mk("a", "media", 0),
		mk("b", "media", 10),
		mk("c", "network", 5),
		mk("d", "media", 20),
	} {
		if err := r.CreateJob(j); err != nil {
			t.Fatalf("CreateJob %s: %v", j.ID, err)
		}
	}

	// No filter: newest-first across all domains (d@20s, b@10s, c@5s, a@0s).
	all, err := r.ListJobs("", 0)
	if err != nil {
		t.Fatalf("ListJobs: %v", err)
	}
	if len(all) != 4 || all[0].ID != "d" || all[1].ID != "b" || all[2].ID != "c" || all[3].ID != "a" {
		t.Fatalf("ListJobs order = %+v", all)
	}

	// Domain filter returns only media, newest-first.
	media, err := r.ListJobs("media", 0)
	if err != nil {
		t.Fatalf("ListJobs media: %v", err)
	}
	if len(media) != 3 || media[0].ID != "d" || media[1].ID != "b" || media[2].ID != "a" {
		t.Fatalf("ListJobs(media) = %+v", media)
	}

	// Limit caps the page size.
	limited, err := r.ListJobs("media", 2)
	if err != nil {
		t.Fatalf("ListJobs media limit: %v", err)
	}
	if len(limited) != 2 || limited[0].ID != "d" || limited[1].ID != "b" {
		t.Fatalf("ListJobs(media,limit=2) = %+v", limited)
	}
}

// itoa avoids importing strconv only for a tiny test helper.
func itoa(n int) string {
	if n == 0 {
		return "0"
	}
	var b []byte
	neg := n < 0
	if neg {
		n = -n
	}
	for n > 0 {
		b = append([]byte{byte('0' + n%10)}, b...)
		n /= 10
	}
	if neg {
		b = append([]byte{'-'}, b...)
	}
	return string(b)
}
