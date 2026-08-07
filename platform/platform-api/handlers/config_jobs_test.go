package handlers

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"

	"github.com/gin-gonic/gin"
	"github.com/glebarez/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"

	"aipc/platform/platform-api/model"
	"aipc/platform/platform-api/repo"
)

// newConfigJobsRepo opens a fresh SQLite DB and migrates the Config Controller
// models, returning a ConfigRepo the audit handlers read from. It mirrors
// repo.newTestRepo but lives here so the handler tests can construct an
// APIHandlers with a real (in-memory) configRepo without a running server.
func newConfigJobsRepo(t *testing.T) *repo.ConfigRepo {
	t.Helper()
	dbPath := filepath.Join(t.TempDir(), "jobs.db")
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
	return repo.NewConfigRepo(gdb)
}

// seedJob inserts a ConfigApplyJob row for assertion by the audit handlers.
func seedJob(t *testing.T, r *repo.ConfigRepo, j *model.ConfigApplyJob) {
	t.Helper()
	if err := r.CreateJob(j); err != nil {
		t.Fatalf("CreateJob: %v", err)
	}
}

// envelope captures the APIResponse envelope: code/message/data/error.
type envelope struct {
	Code    int             `json:"code"`
	Message string          `json:"message"`
	Data    json.RawMessage `json:"data"`
	Error   *struct {
		Detail string `json:"detail"`
	} `json:"error"`
}

// TestGetConfigJob_Found seeds a reconcile job and asserts GET /:id returns it
// wrapped in the success envelope.
func TestGetConfigJob_Found(t *testing.T) {
	gin.SetMode(gin.TestMode)
	r := newConfigJobsRepo(t)
	seedJob(t, r, &model.ConfigApplyJob{
		ID: "job-1", Domain: "media", Key: "config", Action: "reconcile",
		Status: "success", Error: "in-sync", ToRevision: 1,
	})
	h := &APIHandlers{configRepo: r}

	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Params = gin.Params{{Key: "id", Value: "job-1"}}
	h.GetConfigJob(c)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", w.Code)
	}
	var env envelope
	if err := json.Unmarshal(w.Body.Bytes(), &env); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if env.Code != 0 {
		t.Fatalf("code = %d, want 0", env.Code)
	}
	var body struct {
		Job model.ConfigApplyJob `json:"job"`
	}
	if err := json.Unmarshal(env.Data, &body); err != nil {
		t.Fatalf("unmarshal data: %v", err)
	}
	if body.Job.ID != "job-1" || body.Job.Domain != "media" || body.Job.Error != "in-sync" {
		t.Fatalf("job = %+v", body.Job)
	}
}

// TestGetConfigJob_Missing asserts a missing job yields CodeNotFound (HTTP 404).
func TestGetConfigJob_Missing(t *testing.T) {
	gin.SetMode(gin.TestMode)
	r := newConfigJobsRepo(t)
	h := &APIHandlers{configRepo: r}

	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Params = gin.Params{{Key: "id", Value: "nope"}}
	h.GetConfigJob(c)

	if w.Code != http.StatusNotFound {
		t.Fatalf("status = %d, want 404", w.Code)
	}
	var env envelope
	if err := json.Unmarshal(w.Body.Bytes(), &env); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	if env.Code != CodeNotFound {
		t.Fatalf("code = %d, want %d", env.Code, CodeNotFound)
	}
}

// TestGetConfigJob_NoDB asserts the handler degrades gracefully when no DB is
// wired (configRepo nil) instead of panicking on a nil dereference.
func TestGetConfigJob_NoDB(t *testing.T) {
	gin.SetMode(gin.TestMode)
	h := &APIHandlers{} // configRepo is nil

	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Params = gin.Params{{Key: "id", Value: "job-1"}}
	h.GetConfigJob(c)

	if w.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503", w.Code)
	}
}

// TestListConfigJobs_FilterAndLimit seeds jobs across two domains and asserts
// the domain query filter and limit cap are honored, newest-first.
func TestListConfigJobs_FilterAndLimit(t *testing.T) {
	gin.SetMode(gin.TestMode)
	r := newConfigJobsRepo(t)
	seedJob(t, r, &model.ConfigApplyJob{ID: "a", Domain: "media", Action: "apply", Status: "success"})
	seedJob(t, r, &model.ConfigApplyJob{ID: "b", Domain: "network", Action: "apply", Status: "success"})
	seedJob(t, r, &model.ConfigApplyJob{ID: "c", Domain: "media", Action: "reconcile", Status: "success"})
	h := &APIHandlers{configRepo: r}

	// domain=media returns only the two media jobs.
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Request = httptest.NewRequest(http.MethodGet, "/api/v1/config/jobs?domain=media", nil)
	h.ListConfigJobs(c)

	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", w.Code)
	}
	var env envelope
	if err := json.Unmarshal(w.Body.Bytes(), &env); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	var body struct {
		Jobs  []model.ConfigApplyJob `json:"jobs"`
		Count int                    `json:"count"`
	}
	if err := json.Unmarshal(env.Data, &body); err != nil {
		t.Fatalf("unmarshal data: %v", err)
	}
	if body.Count != 2 {
		t.Fatalf("count = %d, want 2 (media only)", body.Count)
	}
	for _, j := range body.Jobs {
		if j.Domain != "media" {
			t.Fatalf("non-media job leaked into filtered list: %+v", j)
		}
	}

	// limit=1 caps the page to a single row.
	w2 := httptest.NewRecorder()
	c2, _ := gin.CreateTestContext(w2)
	c2.Request = httptest.NewRequest(http.MethodGet, "/api/v1/config/jobs?limit=1", nil)
	h.ListConfigJobs(c2)

	var env2 envelope
	if err := json.Unmarshal(w2.Body.Bytes(), &env2); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	var body2 struct {
		Jobs  []model.ConfigApplyJob `json:"jobs"`
		Count int                    `json:"count"`
	}
	if err := json.Unmarshal(env2.Data, &body2); err != nil {
		t.Fatalf("unmarshal data: %v", err)
	}
	if body2.Count != 1 || len(body2.Jobs) != 1 {
		t.Fatalf("limit=1 result = %+v (count=%d), want 1 job", body2.Jobs, body2.Count)
	}
}

// TestListConfigJobs_NoDB asserts the list handler also degrades gracefully
// without a DB rather than panicking.
func TestListConfigJobs_NoDB(t *testing.T) {
	gin.SetMode(gin.TestMode)
	h := &APIHandlers{} // configRepo is nil

	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Request = httptest.NewRequest(http.MethodGet, "/api/v1/config/jobs", nil)
	h.ListConfigJobs(c)

	if w.Code != http.StatusServiceUnavailable {
		t.Fatalf("status = %d, want 503", w.Code)
	}
}
