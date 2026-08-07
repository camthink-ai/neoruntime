package config

import (
	"context"
	"errors"
	"os"
	"sync"
	"testing"
	"time"

	"aipc/platform/platform-api/model"
)

// fakeStore is an in-memory configStore for exercising the Manager state
// machine without a real DB. Its fail* hooks let a test force a failure at a
// specific persist stage to cover the otherwise-unreachable error branches.
type fakeStore struct {
	mu        sync.Mutex
	items     map[string]*model.ConfigItem // keyed domain|key
	jobs      map[string]*model.ConfigApplyJob
	revisions map[string]int // keyed domain|key -> last revision

	failGet            error
	failCreateJob      error
	failAppendRevision error
	failUpsert         error
	failDelete         error
}

func newFakeStore() *fakeStore {
	return &fakeStore{
		items:     make(map[string]*model.ConfigItem),
		jobs:      make(map[string]*model.ConfigApplyJob),
		revisions: make(map[string]int),
	}
}

func keyOf(domain, key string) string { return domain + "|" + key }

func (s *fakeStore) Get(domain, key string) (*model.ConfigItem, bool, error) {
	if s.failGet != nil {
		return nil, false, s.failGet
	}
	it, ok := s.items[keyOf(domain, key)]
	return it, ok, nil
}
func (s *fakeStore) Upsert(item *model.ConfigItem) error {
	if s.failUpsert != nil {
		return s.failUpsert
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	s.items[keyOf(item.Domain, item.Key)] = item
	return nil
}
func (s *fakeStore) Delete(domain, key string) error {
	if s.failDelete != nil {
		return s.failDelete
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	delete(s.items, keyOf(domain, key))
	return nil
}
func (s *fakeStore) AppendRevision(domain, key, valueJSON, reason, createdBy string) (int, error) {
	if s.failAppendRevision != nil {
		return 0, s.failAppendRevision
	}
	s.mu.Lock()
	defer s.mu.Unlock()
	k := keyOf(domain, key)
	s.revisions[k]++
	return s.revisions[k], nil
}
func (s *fakeStore) CreateJob(job *model.ConfigApplyJob) error {
	if s.failCreateJob != nil {
		return s.failCreateJob
	}
	s.jobs[job.ID] = job
	return nil
}
func (s *fakeStore) FinishJob(id, status, errMsg string, toRevision int) error {
	j, ok := s.jobs[id]
	if !ok {
		return nil
	}
	j.Status = status
	j.Error = errMsg
	j.ToRevision = toRevision
	now := time.Now()
	j.FinishedAt = &now
	return nil
}

// fakeAdapter is a configurable Adapter. Each hook can be set to a sentinel
// error to force a branch; the Apply/Verify/Restore hooks also record calls.
type fakeAdapter struct {
	validateErr error
	backupErr   error
	renderErr   error
	applyErr    error
	verifyErr   error
	restoreErr  error

	backupVal any
	renderVal any

	applyCalled   bool
	verifyCalled  bool
	restoreCalled bool

	// verifyFn, when non-nil, overrides verifyErr and is called with the 1-based
	// call index so a test can make Verify fail on the first call and pass on
	// the second (the Reconcile drift → re-project → re-verify flow).
	verifyFn    func(call int) error
	verifyCalls int

	// Snapshot (config.Snapshotter) support.
	snapshotVal    string
	snapshotErr    error
	snapshotCalled bool
}

func (a *fakeAdapter) Validate(ctx context.Context, key, desiredJSON string) error {
	return a.validateErr
}
func (a *fakeAdapter) Backup(ctx context.Context, key string) (any, error) {
	if a.backupErr != nil {
		return nil, a.backupErr
	}
	return a.backupVal, nil
}
func (a *fakeAdapter) Render(ctx context.Context, key, desiredJSON string) (any, error) {
	if a.renderErr != nil {
		return nil, a.renderErr
	}
	if a.renderVal != nil {
		return a.renderVal, nil
	}
	return desiredJSON, nil
}
func (a *fakeAdapter) Apply(ctx context.Context, key string, rendered any) error {
	a.applyCalled = true
	return a.applyErr
}
func (a *fakeAdapter) Verify(ctx context.Context, key, desiredJSON string) error {
	a.verifyCalled = true
	a.verifyCalls++
	if a.verifyFn != nil {
		return a.verifyFn(a.verifyCalls)
	}
	return a.verifyErr
}
func (a *fakeAdapter) Restore(ctx context.Context, key string, backup any) error {
	a.restoreCalled = true
	return a.restoreErr
}

// Snapshot implements config.Snapshotter so Reconcile's R-migration path can be
// exercised with the fake adapter.
func (a *fakeAdapter) Snapshot(ctx context.Context, key string) (string, error) {
	a.snapshotCalled = true
	if a.snapshotErr != nil {
		return "", a.snapshotErr
	}
	return a.snapshotVal, nil
}

type normalizingAdapter struct {
	fakeAdapter
	normalizeVal     string
	normalizeChanged bool
	normalizeErr     error
	normalizeCalled  bool
}

func (a *normalizingAdapter) Normalize(ctx context.Context, key, desiredJSON string) (string, bool, error) {
	a.normalizeCalled = true
	if a.normalizeErr != nil {
		return "", false, a.normalizeErr
	}
	if a.normalizeChanged {
		return a.normalizeVal, true, nil
	}
	return desiredJSON, false, nil
}

// bareAdapter implements Adapter but NOT Snapshotter — used to cover the
// Reconcile "not snapshot-capable, skip" path. All hooks are inert.
type bareAdapter struct{}

func (a *bareAdapter) Validate(context.Context, string, string) error            { return nil }
func (a *bareAdapter) Backup(context.Context, string) (any, error)               { return nil, nil }
func (a *bareAdapter) Render(_ context.Context, _ string, d string) (any, error) { return d, nil }
func (a *bareAdapter) Apply(context.Context, string, any) error                  { return nil }
func (a *bareAdapter) Verify(context.Context, string, string) error              { return nil }
func (a *bareAdapter) Restore(context.Context, string, any) error                { return nil }

// seedDesired writes a desired-state row + its revision counter directly into
// the fakeStore, bypassing Apply so the adapter's call-recorders start clean
// for a Reconcile assertion.
func seedDesired(store *fakeStore, domain, key, valueJSON string, rev int, actor string) {
	store.items[keyOf(domain, key)] = &model.ConfigItem{
		Domain: domain, Key: key, ValueJSON: valueJSON,
		SchemaVersion: 1, Revision: rev, UpdatedBy: actor,
	}
	store.revisions[keyOf(domain, key)] = rev
}

// newTestManager wires a fresh Manager over a fakeStore with deterministic
// clock/ID, registering domain "media" with the given adapter.
func newTestManager(store *fakeStore, adapter Adapter) *Manager {
	m := NewManager(store, nil) // nil logger: no event bus in tests
	m.clock = func() time.Time { return time.Date(2026, 7, 14, 12, 0, 0, 0, time.UTC) }
	m.newID = func() string { return "job-test" }
	m.Register("media", adapter)
	return m
}

func TestApply_UnknownDomain(t *testing.T) {
	m := NewManager(newFakeStore(), nil)
	m.Register("media", &fakeAdapter{})

	_, _, err := m.Apply(context.Background(), "nope", "k", `{}`, "u")
	if !errors.Is(err, ErrUnknownDomain) {
		t.Fatalf("err = %v, want ErrUnknownDomain", err)
	}
}

func TestApply_Success(t *testing.T) {
	store := newFakeStore()
	m := newTestManager(store, &fakeAdapter{})

	jobID, rev, err := m.Apply(context.Background(), "media", "osd", `{"v":1}`, "alice")
	if err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if jobID != "job-test" {
		t.Fatalf("jobID = %q, want job-test", jobID)
	}
	if rev != 1 {
		t.Fatalf("rev = %d, want 1", rev)
	}
	// Desired-state row upserted with the assigned revision.
	item, found, _ := store.Get("media", "osd")
	if !found || item.Revision != 1 || item.ValueJSON != `{"v":1}` || item.UpdatedBy != "alice" {
		t.Fatalf("item not persisted: %+v", item)
	}
	// Job recorded as success.
	j := store.jobs[jobID]
	if j.Status != "success" || j.ToRevision != 1 || j.FinishedAt == nil {
		t.Fatalf("job = %+v", j)
	}
	// Second apply bumps the revision and the from->to bookkeeping.
	_, rev2, err := m.Apply(context.Background(), "media", "osd", `{"v":2}`, "bob")
	if err != nil {
		t.Fatalf("Apply 2: %v", err)
	}
	if rev2 != 2 {
		t.Fatalf("rev2 = %d, want 2", rev2)
	}
}

func TestApply_NormalizesBeforePersist(t *testing.T) {
	store := newFakeStore()
	adapter := &normalizingAdapter{
		normalizeVal:     `{"path":"canonical"}`,
		normalizeChanged: true,
	}
	m := newTestManager(store, adapter)

	_, rev, err := m.Apply(context.Background(), "media", "config", `{"path":"legacy"}`, "system")
	if err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if rev != 1 {
		t.Fatalf("rev = %d, want 1", rev)
	}
	if !adapter.normalizeCalled {
		t.Fatal("Normalize was not called")
	}
	item := store.items[keyOf("media", "config")]
	if item == nil || item.ValueJSON != `{"path":"canonical"}` {
		t.Fatalf("stored item = %+v, want normalized desired", item)
	}
}

func TestApply_ValidateFail(t *testing.T) {
	store := newFakeStore()
	ad := &fakeAdapter{validateErr: errors.New("bad json")}
	m := newTestManager(store, ad)

	_, _, err := m.Apply(context.Background(), "media", "osd", `{bad`, "u")
	if err == nil {
		t.Fatal("Apply succeeded; want validate failure")
	}
	j := store.jobs["job-test"]
	if j.Status != "failed" || j.Error == "" {
		t.Fatalf("job = %+v", j)
	}
	if !hasPrefix(j.Error, "validate") {
		t.Fatalf("job.Error = %q, want stage 'validate'", j.Error)
	}
	// No side-effecting adapter call should have run.
	if ad.applyCalled {
		t.Fatal("Apply ran despite Validate failure")
	}
}

func TestApply_BackupFail(t *testing.T) {
	store := newFakeStore()
	m := newTestManager(store, &fakeAdapter{backupErr: errors.New("disk full")})

	_, _, err := m.Apply(context.Background(), "media", "osd", `{}`, "u")
	if err == nil {
		t.Fatal("Apply succeeded; want backup failure")
	}
	j := store.jobs["job-test"]
	if j.Status != "failed" || !hasPrefix(j.Error, "backup") {
		t.Fatalf("job = %+v", j)
	}
}

func TestApply_RenderFail(t *testing.T) {
	store := newFakeStore()
	m := newTestManager(store, &fakeAdapter{renderErr: errors.New("no template")})

	_, _, err := m.Apply(context.Background(), "media", "osd", `{}`, "u")
	if err == nil {
		t.Fatal("Apply succeeded; want render failure")
	}
	j := store.jobs["job-test"]
	if j.Status != "failed" || !hasPrefix(j.Error, "render") {
		t.Fatalf("job = %+v", j)
	}
}

func TestApply_ApplyFail(t *testing.T) {
	store := newFakeStore()
	m := newTestManager(store, &fakeAdapter{applyErr: errors.New("grpc unavailable")})

	_, _, err := m.Apply(context.Background(), "media", "osd", `{}`, "u")
	if err == nil {
		t.Fatal("Apply succeeded; want apply failure")
	}
	j := store.jobs["job-test"]
	if j.Status != "failed" || !hasPrefix(j.Error, "apply") {
		t.Fatalf("job = %+v", j)
	}
}

func TestApply_VerifyFail_AutoRestores(t *testing.T) {
	store := newFakeStore()
	ad := &fakeAdapter{
		verifyErr:  errors.New("readback mismatch"),
		restoreErr: nil, // restore succeeds
	}
	m := newTestManager(store, ad)

	_, _, err := m.Apply(context.Background(), "media", "osd", `{"v":1}`, "u")
	if err == nil {
		t.Fatal("Apply succeeded; want verify failure")
	}
	if !ad.restoreCalled {
		t.Fatal("Restore not called after Verify failure")
	}
	j := store.jobs["job-test"]
	if j.Status != "failed" {
		t.Fatalf("status = %q, want failed", j.Status)
	}
	if j.Error == "" || !contains(j.Error, "auto-restored") {
		t.Fatalf("job.Error = %q, want mention of auto-restored", j.Error)
	}
	// Verify failure must NOT persist the desired-state row or a revision.
	if _, found, _ := store.Get("media", "osd"); found {
		t.Fatal("item persisted despite verify failure")
	}
}

func TestApply_VerifyFail_RestoreAlsoFails(t *testing.T) {
	store := newFakeStore()
	ad := &fakeAdapter{
		verifyErr:  errors.New("readback mismatch"),
		restoreErr: errors.New("backup corrupt"),
	}
	m := newTestManager(store, ad)

	_, _, err := m.Apply(context.Background(), "media", "osd", `{}`, "u")
	if err == nil {
		t.Fatal("Apply succeeded; want verify failure")
	}
	j := store.jobs["job-test"]
	if !contains(j.Error, "restore failed") {
		t.Fatalf("job.Error = %q, want mention of restore failure", j.Error)
	}
}

func TestApply_CreateJobFail(t *testing.T) {
	store := newFakeStore()
	store.failCreateJob = errors.New("db locked")
	m := newTestManager(store, &fakeAdapter{})

	_, _, err := m.Apply(context.Background(), "media", "osd", `{}`, "u")
	if err == nil {
		t.Fatal("Apply succeeded; want create-job failure")
	}
}

func TestApply_ReadCurrentFail(t *testing.T) {
	store := newFakeStore()
	store.failGet = errors.New("db locked")
	m := newTestManager(store, &fakeAdapter{})

	_, _, err := m.Apply(context.Background(), "media", "osd", `{}`, "u")
	if err == nil {
		t.Fatal("Apply succeeded; want read-current failure")
	}
}

func TestApply_AppendRevisionFail(t *testing.T) {
	store := newFakeStore()
	store.failAppendRevision = errors.New("history full")
	m := newTestManager(store, &fakeAdapter{})

	_, _, err := m.Apply(context.Background(), "media", "osd", `{}`, "u")
	if err == nil {
		t.Fatal("Apply succeeded; want append-revision failure")
	}
	j := store.jobs["job-test"]
	if j.Status != "failed" || !contains(j.Error, "append-revision") {
		t.Fatalf("job = %+v", j)
	}
	// The live change verified but the desired-state row must not be upserted
	// (we failed before Upsert).
	if _, found, _ := store.Get("media", "osd"); found {
		t.Fatal("item upserted despite append-revision failure")
	}
}

func TestApply_UpsertFail(t *testing.T) {
	store := newFakeStore()
	store.failUpsert = errors.New("upsert blew up")
	m := newTestManager(store, &fakeAdapter{})

	_, _, err := m.Apply(context.Background(), "media", "osd", `{}`, "u")
	if err == nil {
		t.Fatal("Apply succeeded; want upsert failure")
	}
	j := store.jobs["job-test"]
	if j.Status != "failed" || !contains(j.Error, "upsert-item") {
		t.Fatalf("job = %+v", j)
	}
}

// TestApply_SameDomainSerializes asserts the per-domain mutex serializes two
// concurrent applies to the same domain (no interleaving of the live state).
func TestApply_SameDomainSerializes(t *testing.T) {
	store := newFakeStore()
	ad := &fakeAdapter{}
	m := newTestManager(store, ad)

	// Distinct IDs per call so both jobs are recorded.
	var idc uint64
	var idMu sync.Mutex
	m.newID = func() string {
		idMu.Lock()
		defer idMu.Unlock()
		idc++
		return "job-" + itoa(int(idc))
	}

	var wg sync.WaitGroup
	for i := 0; i < 5; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			_, _, _ = m.Apply(context.Background(), "media", "osd", `{"v":1}`, "u")
		}()
	}
	wg.Wait()

	// All 5 applies targeted the same (domain,key); the final revision count
	// must be exactly 5 (each Apply appended once, serialized, no lost
	// increments from the AppendRevision max+1 race).
	if got := store.revisions[keyOf("media", "osd")]; got != 5 {
		t.Fatalf("revisions = %d, want 5 (serialization lost increments)", got)
	}
}

// contains reports whether sub appears in s.
func contains(s, sub string) bool {
	return len(s) >= len(sub) && indexOf(s, sub) >= 0
}

// hasPrefix reports whether s begins with prefix.
func hasPrefix(s, prefix string) bool {
	if len(s) < len(prefix) {
		return false
	}
	return s[:len(prefix)] == prefix
}

func indexOf(s, sub string) int {
	for i := 0; i+len(sub) <= len(s); i++ {
		if s[i:i+len(sub)] == sub {
			return i
		}
	}
	return -1
}

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

// --- Delete ---

// TestDelete_Found proves DELETE clears the desired-state ConfigItem and
// appends a "delete" revision + success job. This is the core of the orphan-row
// fix: without it, POST (Apply writes the row) + DELETE leaves an orphan that
// Phase 2 reconcile would resurrect.
func TestDelete_Found(t *testing.T) {
	store := newFakeStore()
	m := newTestManager(store, &fakeAdapter{})

	// Seed a desired row via Apply so there is something to delete (rev 1).
	if _, _, err := m.Apply(context.Background(), "media", "osd", `{"v":1}`, "alice"); err != nil {
		t.Fatalf("seed Apply: %v", err)
	}

	jobID, rev, err := m.Delete(context.Background(), "media", "osd", "bob")
	if err != nil {
		t.Fatalf("Delete: %v", err)
	}
	if jobID != "job-test" {
		t.Fatalf("jobID = %q, want job-test", jobID)
	}
	if rev != 2 { // Apply took rev 1; the delete marker is rev 2.
		t.Fatalf("rev = %d, want 2", rev)
	}
	// Desired-state row gone — the orphan is cleared.
	if _, found, _ := store.Get("media", "osd"); found {
		t.Fatal("desired row still present after Delete")
	}
	// Job recorded as success pointing at the delete revision.
	j := store.jobs[jobID]
	if j == nil || j.Status != "success" || j.ToRevision != 2 || j.Action != "delete" {
		t.Fatalf("job = %+v", j)
	}
}

// TestDelete_NotFound is the audited no-op: a missing desired row still records
// a success job (so the attempt is traceable) but appends no revision.
func TestDelete_NotFound(t *testing.T) {
	store := newFakeStore()
	m := newTestManager(store, &fakeAdapter{})

	jobID, rev, err := m.Delete(context.Background(), "media", "ghost", "u")
	if err != nil {
		t.Fatalf("Delete: %v", err)
	}
	if rev != 0 {
		t.Fatalf("rev = %d, want 0 (no revision appended)", rev)
	}
	j := store.jobs[jobID]
	if j == nil || j.Status != "success" {
		t.Fatalf("job = %+v, want success", j)
	}
	if !contains(j.Error, "nothing to delete") && j.Error != "nothing to delete" {
		// fakeStore stores errMsg verbatim; Manager passes "nothing to delete".
		if j.Error != "nothing to delete" {
			t.Fatalf("job.Error = %q, want 'nothing to delete'", j.Error)
		}
	}
	// No revision recorded for a key that had no state.
	if got := store.revisions[keyOf("media", "ghost")]; got != 0 {
		t.Fatalf("revisions[ghost] = %d, want 0", got)
	}
}

// TestDelete_GetFail: a store read error aborts before any job is recorded.
func TestDelete_GetFail(t *testing.T) {
	store := newFakeStore()
	store.failGet = errors.New("db offline")
	m := newTestManager(store, &fakeAdapter{})

	_, _, err := m.Delete(context.Background(), "media", "osd", "u")
	if err == nil {
		t.Fatal("Delete succeeded; want read-current failure")
	}
	if !hasPrefix(err.Error(), "config: read current") {
		t.Fatalf("err = %v, want 'config: read current'", err)
	}
	if len(store.jobs) != 0 {
		t.Fatalf("jobs = %d, want 0 (no job recorded on read fail)", len(store.jobs))
	}
}

// TestDelete_CreateJobFail: a job-record failure surfaces and aborts.
func TestDelete_CreateJobFail(t *testing.T) {
	store := newFakeStore()
	store.failCreateJob = errors.New("disk full")
	m := newTestManager(store, &fakeAdapter{})

	_, _, err := m.Delete(context.Background(), "media", "osd", "u")
	if err == nil {
		t.Fatal("Delete succeeded; want create-job failure")
	}
	if !hasPrefix(err.Error(), "config: create job") {
		t.Fatalf("err = %v, want 'config: create job'", err)
	}
}

// TestDelete_DeleteItemFail: a desired-row removal failure marks the job failed.
func TestDelete_DeleteItemFail(t *testing.T) {
	store := newFakeStore()
	m := newTestManager(store, &fakeAdapter{})

	// Seed a row so the delete-item branch is reached.
	if _, _, err := m.Apply(context.Background(), "media", "osd", `{"v":1}`, "alice"); err != nil {
		t.Fatalf("seed Apply: %v", err)
	}
	store.failDelete = errors.New("locked")

	_, _, err := m.Delete(context.Background(), "media", "osd", "u")
	if err == nil {
		t.Fatal("Delete succeeded; want delete-item failure")
	}
	j := store.jobs["job-test"]
	if j == nil || j.Status != "failed" || !hasPrefix(j.Error, "delete-item") {
		t.Fatalf("job = %+v, want failed at delete-item", j)
	}
}

// TestDelete_AppendRevisionFail: the row was removed but the revision marker
// could not be appended — the job is marked failed so the drift is auditable.
func TestDelete_AppendRevisionFail(t *testing.T) {
	store := newFakeStore()
	m := newTestManager(store, &fakeAdapter{})

	if _, _, err := m.Apply(context.Background(), "media", "osd", `{"v":1}`, "alice"); err != nil {
		t.Fatalf("seed Apply: %v", err)
	}
	store.failAppendRevision = errors.New("wal full")

	_, _, err := m.Delete(context.Background(), "media", "osd", "u")
	if err == nil {
		t.Fatal("Delete succeeded; want append-revision failure")
	}
	j := store.jobs["job-test"]
	if j == nil || j.Status != "failed" || !hasPrefix(j.Error, "append-revision") {
		t.Fatalf("job = %+v, want failed at append-revision", j)
	}
	// The desired row was removed before the revision append failed; the orphan
	// is gone, which is the user-facing invariant this whole fix protects.
	if _, found, _ := store.Get("media", "osd"); found {
		t.Fatal("desired row still present after partial Delete")
	}
}

// reconcileJob returns the single job recorded by newTestManager's deterministic
// newID ("job-test"). Every Reconcile target creates exactly one job row keyed
// "job-test", so the final state is what the test asserts against.
func reconcileJob(t *testing.T, store *fakeStore) *model.ConfigApplyJob {
	t.Helper()
	j := store.jobs["job-test"]
	if j == nil {
		t.Fatal("reconcile job not recorded")
	}
	return j
}

// TestReconcile_InSync: desired rev1 exists and the live state already matches
// (Verify nil). Reconcile is a no-op: no Apply, no revision bump.
func TestReconcile_InSync(t *testing.T) {
	store := newFakeStore()
	ad := &fakeAdapter{}
	m := newTestManager(store, ad)
	seedDesired(store, "media", "osd", `{"v":1}`, 1, "alice")

	if err := m.Reconcile(context.Background(), []ReconcileTarget{{Domain: "media", Key: "osd"}}, "system"); err != nil {
		t.Fatalf("Reconcile: %v", err)
	}
	j := reconcileJob(t, store)
	if j.Status != "success" || j.Error != "in-sync" || j.ToRevision != 1 {
		t.Fatalf("job = %+v, want success/in-sync/rev1", j)
	}
	if store.revisions[keyOf("media", "osd")] != 1 {
		t.Fatalf("revision bumped to %d on in-sync no-op", store.revisions[keyOf("media", "osd")])
	}
	if ad.applyCalled {
		t.Fatal("Apply called on in-sync target; want no-op")
	}
}

// TestReconcile_Drift_Reproject: desired exists but Verify fails (drift, incl.
// missing file). Reconcile re-projects via Render+Apply then re-verifies; on
// success the job is "re-projected" with no revision bump.
func TestReconcile_Drift_Reproject(t *testing.T) {
	store := newFakeStore()
	ad := &fakeAdapter{verifyFn: func(call int) error {
		if call == 1 {
			return errors.New("drift")
		}
		return nil
	}}
	m := newTestManager(store, ad)
	seedDesired(store, "media", "osd", `{"v":1}`, 1, "alice")

	if err := m.Reconcile(context.Background(), []ReconcileTarget{{Domain: "media", Key: "osd"}}, "system"); err != nil {
		t.Fatalf("Reconcile: %v", err)
	}
	j := reconcileJob(t, store)
	if j.Status != "success" || j.Error != "re-projected" || j.ToRevision != 1 {
		t.Fatalf("job = %+v, want success/re-projected/rev1", j)
	}
	if store.revisions[keyOf("media", "osd")] != 1 {
		t.Fatalf("revision bumped to %d on re-project", store.revisions[keyOf("media", "osd")])
	}
	if !ad.applyCalled {
		t.Fatal("Apply not called during re-project")
	}
}

func TestReconcile_NormalizedDesiredReprojects(t *testing.T) {
	store := newFakeStore()
	ad := &normalizingAdapter{
		fakeAdapter: fakeAdapter{verifyFn: func(call int) error {
			if call == 1 {
				return errors.New("live still legacy")
			}
			return nil
		}},
		normalizeVal:     "canonical-yaml",
		normalizeChanged: true,
	}
	m := newTestManager(store, ad)
	seedDesired(store, "media", "config", "legacy-yaml", 2, "alice")

	if err := m.Reconcile(context.Background(), []ReconcileTarget{{Domain: "media", Key: "config"}}, "system"); err != nil {
		t.Fatalf("Reconcile: %v", err)
	}
	j := reconcileJob(t, store)
	if j.Status != "success" || j.Error != "normalized and re-projected" || j.ToRevision != 3 {
		t.Fatalf("job = %+v, want success/normalized-and-re-projected/rev3", j)
	}
	item, found, _ := store.Get("media", "config")
	if !found || item.Revision != 3 || item.ValueJSON != "canonical-yaml" {
		t.Fatalf("normalized item = %+v, want rev3 canonical-yaml", item)
	}
	if store.revisions[keyOf("media", "config")] != 3 {
		t.Fatalf("revision = %d, want 3", store.revisions[keyOf("media", "config")])
	}
	if !ad.applyCalled {
		t.Fatal("Apply not called for normalized drift")
	}
}

// TestReconcile_RMigration_Import: empty DB + a pre-existing live file. Reconcile
// imports the file as rev1 (reason "import") WITHOUT writing the file (Apply
// never called) — only DB rows are written.
func TestReconcile_RMigration_Import(t *testing.T) {
	store := newFakeStore()
	ad := &fakeAdapter{snapshotVal: "live-yaml"}
	m := newTestManager(store, ad)

	if err := m.Reconcile(context.Background(), []ReconcileTarget{{Domain: "media", Key: "osd"}}, "system"); err != nil {
		t.Fatalf("Reconcile: %v", err)
	}
	j := reconcileJob(t, store)
	if j.Status != "success" || j.Error != "imported" || j.ToRevision != 1 {
		t.Fatalf("job = %+v, want success/imported/rev1", j)
	}
	item, found, _ := store.Get("media", "osd")
	if !found || item.Revision != 1 || item.ValueJSON != "live-yaml" {
		t.Fatalf("imported item = %+v, want rev1 live-yaml", item)
	}
	if store.revisions[keyOf("media", "osd")] != 1 {
		t.Fatalf("revision = %d, want 1", store.revisions[keyOf("media", "osd")])
	}
	if ad.applyCalled {
		t.Fatal("Apply called during R-migration import; file must not be touched")
	}
	if !ad.snapshotCalled {
		t.Fatal("Snapshot not called during R-migration import")
	}
}

func TestReconcile_RMigration_NormalizesAndReprojects(t *testing.T) {
	store := newFakeStore()
	ad := &normalizingAdapter{
		fakeAdapter:      fakeAdapter{snapshotVal: "legacy-live-yaml"},
		normalizeVal:     "canonical-live-yaml",
		normalizeChanged: true,
	}
	m := newTestManager(store, ad)

	if err := m.Reconcile(context.Background(), []ReconcileTarget{{Domain: "media", Key: "config"}}, "system"); err != nil {
		t.Fatalf("Reconcile: %v", err)
	}
	j := reconcileJob(t, store)
	if j.Status != "success" || j.Error != "imported normalized and re-projected" || j.ToRevision != 1 {
		t.Fatalf("job = %+v, want success/imported-normalized-and-re-projected/rev1", j)
	}
	item, found, _ := store.Get("media", "config")
	if !found || item.Revision != 1 || item.ValueJSON != "canonical-live-yaml" {
		t.Fatalf("imported item = %+v, want rev1 canonical-live-yaml", item)
	}
	if !ad.applyCalled {
		t.Fatal("Apply not called for normalized import")
	}
}

// TestReconcile_NoDesired_NoFile: empty DB + no live file (Snapshot returns
// os.ErrNotExist). Reconcile is a non-error skip; nothing is imported.
func TestReconcile_NoDesired_NoFile(t *testing.T) {
	store := newFakeStore()
	ad := &fakeAdapter{snapshotErr: os.ErrNotExist}
	m := newTestManager(store, ad)

	if err := m.Reconcile(context.Background(), []ReconcileTarget{{Domain: "media", Key: "osd"}}, "system"); err != nil {
		t.Fatalf("Reconcile: %v", err)
	}
	j := reconcileJob(t, store)
	if j.Status != "success" || j.Error != "no live file, skip" {
		t.Fatalf("job = %+v, want success/no-live-file-skip", j)
	}
	if _, found, _ := store.Get("media", "osd"); found {
		t.Fatal("desired row created despite no live file")
	}
}

// TestReconcile_RMigration_SnapshotFail: empty DB + Snapshot returns a non-
// NotExist error. The job fails at "snapshot"; Reconcile returns non-nil.
func TestReconcile_RMigration_SnapshotFail(t *testing.T) {
	store := newFakeStore()
	ad := &fakeAdapter{snapshotErr: errors.New("boom")}
	m := newTestManager(store, ad)

	err := m.Reconcile(context.Background(), []ReconcileTarget{{Domain: "media", Key: "osd"}}, "system")
	if err == nil {
		t.Fatal("Reconcile succeeded; want snapshot failure")
	}
	j := reconcileJob(t, store)
	if j.Status != "failed" || !hasPrefix(j.Error, "reconcile: snapshot:") {
		t.Fatalf("job = %+v, want failed snapshot", j)
	}
}

// TestReconcile_VerifyFailAfterReproject: desired exists, Verify fails on every
// call. After re-project the re-verify still fails → job failed "verify after
// re-project". Restore is NEVER invoked (desired is source of truth, not a
// rollback candidate).
func TestReconcile_VerifyFailAfterReproject(t *testing.T) {
	store := newFakeStore()
	ad := &fakeAdapter{verifyFn: func(call int) error { return errors.New("still drifted") }}
	m := newTestManager(store, ad)
	seedDesired(store, "media", "osd", `{"v":1}`, 1, "alice")

	err := m.Reconcile(context.Background(), []ReconcileTarget{{Domain: "media", Key: "osd"}}, "system")
	if err == nil {
		t.Fatal("Reconcile succeeded; want verify-after-reproject failure")
	}
	j := reconcileJob(t, store)
	if j.Status != "failed" || !hasPrefix(j.Error, "reconcile: verify after re-project:") {
		t.Fatalf("job = %+v, want failed verify-after-reproject", j)
	}
	if ad.restoreCalled {
		t.Fatal("Restore called during reconcile; desired is source of truth")
	}
}

// TestReconcile_GetFail: the store's Get fails (db down). The job fails at
// "get"; Reconcile returns non-nil so the caller can log the drift.
func TestReconcile_GetFail(t *testing.T) {
	store := newFakeStore()
	store.failGet = errors.New("db down")
	ad := &fakeAdapter{}
	m := newTestManager(store, ad)

	err := m.Reconcile(context.Background(), []ReconcileTarget{{Domain: "media", Key: "osd"}}, "system")
	if err == nil {
		t.Fatal("Reconcile succeeded; want get failure")
	}
	j := reconcileJob(t, store)
	if j.Status != "failed" || !hasPrefix(j.Error, "reconcile: get:") {
		t.Fatalf("job = %+v, want failed get", j)
	}
}

// TestReconcile_UnknownDomain: target names a domain with no registered adapter.
// Reconcile records a failed "unknown domain" job and returns non-nil; it does
// NOT abort the boot.
func TestReconcile_UnknownDomain(t *testing.T) {
	store := newFakeStore()
	m := newTestManager(store, &fakeAdapter{})

	err := m.Reconcile(context.Background(), []ReconcileTarget{{Domain: "nope", Key: "k"}}, "system")
	if err == nil || !hasPrefix(err.Error(), "config: reconcile completed with") {
		t.Fatalf("err = %v, want reconcile summary error", err)
	}
	j := reconcileJob(t, store)
	if j.Status != "failed" || j.Error != "reconcile: unknown domain" {
		t.Fatalf("job = %+v, want failed unknown-domain", j)
	}
}

// TestReconcile_NonSnapshotterDomain: empty DB + an adapter that does NOT
// implement Snapshotter. Reconcile skips with "not snapshot-capable, skip";
// nothing is imported.
func TestReconcile_NonSnapshotterDomain(t *testing.T) {
	store := newFakeStore()
	m := newTestManager(store, &bareAdapter{})

	if err := m.Reconcile(context.Background(), []ReconcileTarget{{Domain: "media", Key: "osd"}}, "system"); err != nil {
		t.Fatalf("Reconcile: %v", err)
	}
	j := reconcileJob(t, store)
	if j.Status != "success" || j.Error != "not snapshot-capable, skip" {
		t.Fatalf("job = %+v, want success/not-snapshot-capable-skip", j)
	}
	if _, found, _ := store.Get("media", "osd"); found {
		t.Fatal("desired row created by a non-snapshot-capable domain")
	}
}
