package config

import (
	"context"
	"errors"
	"fmt"
	"os"
	"strings"
	"sync"
	"time"

	"github.com/google/uuid"

	"aipc/platform/common/events"
	"aipc/platform/platform-api/model"
)

// ErrUnknownDomain is returned by Apply when no Adapter is registered for the
// requested domain. The job is not recorded (there is nothing to apply).
var ErrUnknownDomain = errors.New("config: unknown domain")

// configStore is the subset of repo.ConfigRepo the Manager needs. Depending
// on this interface (rather than the concrete repo) enables injecting a
// failing store in tests for the persist-failure branches. repo.ConfigRepo
// satisfies it, so production wiring is unchanged.
type configStore interface {
	Get(domain, key string) (item *model.ConfigItem, found bool, err error)
	Upsert(item *model.ConfigItem) error
	Delete(domain, key string) error
	AppendRevision(domain, key, valueJSON, reason, createdBy string) (int, error)
	CreateJob(job *model.ConfigApplyJob) error
	FinishJob(id, status, errMsg string, toRevision int) error
}

// Manager is the single entry point for mutating system configuration. It
// drives each apply through a uniform state machine, records a ConfigApplyJob
// for every attempt, and on success appends a ConfigRevision and updates the
// desired-state ConfigItem.
//
// Concurrency: applies within the same domain are serialized via a per-domain
// mutex; different domains run concurrently. The live-system side effects
// (file writes, gRPC calls, exec) happen under this lock, so two concurrent
// requests targeting the same domain cannot interleave.
type Manager struct {
	db       configStore
	adapters map[string]Adapter
	mu       map[string]*sync.Mutex
	muGuard  sync.Mutex // guards the mu map when lazily creating a domain lock

	// logger is optional; nil skips the config.changed event.
	logger *events.Logger
	// clock and newID are injectable for deterministic tests.
	clock func() time.Time
	newID func() string
}

// NewManager creates a Manager with no registered adapters. Adapters register
// via Register before any Apply targets their domain.
func NewManager(db configStore, logger *events.Logger) *Manager {
	if db == nil {
		// Defensive: a nil repo would panic on the first Apply. Callers that
		// truly have no DB should not construct a Manager.
		panic("config: NewManager requires a non-nil ConfigRepo")
	}
	return &Manager{
		db:       db,
		adapters: make(map[string]Adapter),
		mu:       make(map[string]*sync.Mutex),
		logger:   logger,
		clock:    time.Now,
		newID:    uuid.NewString,
	}
}

// Register associates adapter with domain. Register all adapters at startup
// (in the handler constructor) before serving requests; it is not safe to
// register a domain while an Apply for that domain is in flight.
func (m *Manager) Register(domain string, adapter Adapter) {
	m.adapters[domain] = adapter
}

// domainMu returns the per-domain lock, creating it on first use.
func (m *Manager) domainMu(domain string) *sync.Mutex {
	m.muGuard.Lock()
	defer m.muGuard.Unlock()
	l, ok := m.mu[domain]
	if !ok {
		l = &sync.Mutex{}
		m.mu[domain] = l
	}
	return l
}

func normalizeDesired(ctx context.Context, adapter Adapter, key, desiredJSON string) (string, bool, error) {
	n, ok := adapter.(Normalizer)
	if !ok {
		return desiredJSON, false, nil
	}
	return n.Normalize(ctx, key, desiredJSON)
}

// Apply runs the full state machine for (domain, key): Validate → Backup →
// Render → Apply → Verify, with auto-Restore on Verify failure. It returns
// the job ID (whenever the job was recorded), the assigned revision (on
// success only), and an error (non-nil when the job ended in failure).
//
// On every terminal path the ConfigApplyJob row is updated to "success" or
// "failed" with a human-readable error, so the job history is always
// reconcilable.
func (m *Manager) Apply(ctx context.Context, domain, key, desiredJSON, actor string) (jobID string, rev int, err error) {
	adapter, ok := m.adapters[domain]
	if !ok {
		return "", 0, fmt.Errorf("%w: %s", ErrUnknownDomain, domain)
	}

	m.domainMu(domain).Lock()
	defer m.domainMu(domain).Unlock()

	// Capture the revision we are applying from (0 if first-time).
	fromRev := 0
	if cur, found, qerr := m.db.Get(domain, key); qerr != nil {
		return "", 0, fmt.Errorf("config: read current: %w", qerr)
	} else if found {
		fromRev = cur.Revision
	}

	// Record the attempt as a running job up front so a crash mid-apply
	// leaves an auditable "running" row rather than no record at all.
	jobID = m.newID()
	started := m.clock()
	if jerr := m.db.CreateJob(&model.ConfigApplyJob{
		ID: jobID, Domain: domain, Key: key, Action: "apply",
		Status: "running", StartedAt: started, FromRevision: fromRev, ToRevision: 0,
	}); jerr != nil {
		return "", 0, fmt.Errorf("config: create job: %w", jerr)
	}

	// fail marks the job failed with the given reason and returns. It is the
	// single exit path for every non-success branch so the job row is always
	// finalized.
	fail := func(stage string, e error) (string, int, error) {
		msg := fmt.Sprintf("%s: %v", stage, e)
		_ = m.db.FinishJob(jobID, "failed", msg, 0)
		return jobID, 0, fmt.Errorf("config: %s", msg)
	}

	var changed bool
	var e error
	desiredJSON, changed, e = normalizeDesired(ctx, adapter, key, desiredJSON)
	if e != nil {
		return fail("normalize", e)
	}

	// 1. Validate — fail fast, no side effects.
	if e := adapter.Validate(ctx, key, desiredJSON); e != nil {
		return fail("validate", e)
	}

	// 2. Backup — capture current live state for potential Restore.
	backup, e := adapter.Backup(ctx, key)
	if e != nil {
		return fail("backup", e)
	}

	// 3. Render — turn desired into the concrete projection.
	rendered, e := adapter.Render(ctx, key, desiredJSON)
	if e != nil {
		return fail("render", e)
	}

	// 4. Apply — push to the live system (file/gRPC/exec).
	if e := adapter.Apply(ctx, key, rendered); e != nil {
		return fail("apply", e)
	}

	// 5. Verify — read back and confirm. On failure, auto-Restore.
	if e := adapter.Verify(ctx, key, desiredJSON); e != nil {
		// Best-effort restore; surface its outcome in the job message alongside
		// the verify error so operators can see both.
		if restErr := adapter.Restore(ctx, key, backup); restErr != nil {
			_ = m.db.FinishJob(jobID, "failed", fmt.Sprintf("verify: %v (restore failed: %v)", e, restErr), 0)
		} else {
			_ = m.db.FinishJob(jobID, "failed", fmt.Sprintf("verify: %v (auto-restored)", e), 0)
		}
		return jobID, 0, fmt.Errorf("config: verify: %w", e)
	}

	// 6. Success — append revision, upsert desired-state row, finalize job.
	rev, e = m.db.AppendRevision(domain, key, desiredJSON, "apply", actor)
	if e != nil {
		// The live change took effect and verified, but we could not record
		// the revision. Mark failed; Phase 2 reconcile will catch the drift
		// between the live state and the lagged DB row.
		return fail("append-revision", e)
	}
	now := m.clock()
	if e := m.db.Upsert(&model.ConfigItem{
		Domain: domain, Key: key, ValueJSON: desiredJSON,
		SchemaVersion: 1, Revision: rev, UpdatedAt: now, UpdatedBy: actor,
	}); e != nil {
		return fail("upsert-item", e)
	}

	_ = m.db.FinishJob(jobID, "success", "", rev)

	// Fire-and-forget audit event, mirroring handlers/settings.go's existing
	// "config.changed" usage.
	if m.logger != nil {
		changeText := fmt.Sprintf("%s: rev %d → rev %d", key, fromRev, rev)
		if changed {
			changeText += " (normalized)"
		}
		m.logger.LogWithCodeAsync("config.changed", events.MessageParams{
			"section": domain,
			"changes": changeText,
		}, actor)
	}

	return jobID, rev, nil
}

// Delete removes the desired-state ConfigItem for (domain, key) and records a
// "delete" revision + job row for audit. It does NOT touch the live system —
// the handler owns the live-side removal (e.g. settingRepo.Delete for the
// system domain) — and only appends to (never edits) the revision history.
//
// Call this from a handler's DELETE route so the desired-state mirror stays
// consistent with the live state. Without it, a POST (which writes the desired
// row via Apply) followed by a DELETE leaves an orphan desired row that Phase 2
// reconcile would resurrect as if the setting had never been deleted.
//
// Deleting a (domain, key) with no existing desired row is an audited no-op:
// it records a success job (so the attempt is traceable) but appends no
// revision (there is no state transition to record). Delete does not consult
// the Adapter — it is purely desired-state bookkeeping — so it works for any
// registered domain without driving an adapter state machine.
func (m *Manager) Delete(ctx context.Context, domain, key, actor string) (jobID string, rev int, err error) {
	m.domainMu(domain).Lock()
	defer m.domainMu(domain).Unlock()

	fromRev := 0
	found := false
	if cur, ok, qerr := m.db.Get(domain, key); qerr != nil {
		return "", 0, fmt.Errorf("config: read current: %w", qerr)
	} else if ok {
		found = true
		fromRev = cur.Revision
	}

	jobID = m.newID()
	started := m.clock()
	if jerr := m.db.CreateJob(&model.ConfigApplyJob{
		ID: jobID, Domain: domain, Key: key, Action: "delete",
		Status: "running", StartedAt: started, FromRevision: fromRev, ToRevision: 0,
	}); jerr != nil {
		return "", 0, fmt.Errorf("config: create job: %w", jerr)
	}

	fail := func(stage string, e error) (string, int, error) {
		msg := fmt.Sprintf("%s: %v", stage, e)
		_ = m.db.FinishJob(jobID, "failed", msg, 0)
		return jobID, 0, fmt.Errorf("config: %s", msg)
	}

	// No desired-state row exists — audited no-op, no revision appended.
	if !found {
		_ = m.db.FinishJob(jobID, "success", "nothing to delete", 0)
		return jobID, 0, nil
	}

	// Remove the desired-state row. Revision history is append-only and is
	// left intact (the delete marker below records the removal).
	if e := m.db.Delete(domain, key); e != nil {
		return fail("delete-item", e)
	}

	// Append a delete marker so the history shows the removal at this revision.
	rev, e := m.db.AppendRevision(domain, key, "", "delete", actor)
	if e != nil {
		return fail("append-revision", e)
	}

	_ = m.db.FinishJob(jobID, "success", "", rev)
	return jobID, rev, nil
}

// ReconcileTarget names a (domain, key) pair for Reconcile to converge.
type ReconcileTarget struct {
	Domain string
	Key    string
}

// Reconcile converges live state toward the desired-state store for each
// target. It is the Phase 2 startup hook: after platform-api boots, it walks a
// fixed target list (currently just media/config) and, per target, either
//  1. normalizes stale/version-dependent desired values,
//  2. confirms live matches desired (in-sync no-op),
//  3. re-projects the desired state to disk on drift (re-project), or
//  4. imports pre-existing live state into an empty store (R-migration).
//
// A normalized R-migration is re-projected after import so old release-owned
// paths cannot remain on disk.
//
// Reconcile never aborts the boot. Per-target failures are recorded as failed
// jobs and reflected in the returned summary error, which the caller treats as
// non-fatal (logs and continues serving).
//
// It does NOT bump revisions for the in-sync and re-project paths (desired is
// unchanged, merely enforced); normalization and import append revisions.
// Restore is never invoked — desired is the source of truth, not a candidate
// to roll back. The live reconfiguration gRPC calls stay in the handler
// post-Apply (Phase 1); Reconcile only re-projects the file.
func (m *Manager) Reconcile(ctx context.Context, targets []ReconcileTarget, actor string) error {
	var errs []string
	for _, t := range targets {
		if e := m.reconcileOne(ctx, t, actor); e != nil {
			errs = append(errs, fmt.Sprintf("%s/%s: %v", t.Domain, t.Key, e))
		}
	}
	if len(errs) > 0 {
		return fmt.Errorf("config: reconcile completed with %d error(s): %s", len(errs), strings.Join(errs, "; "))
	}
	return nil
}

// reconcileOne runs the reconcile algorithm for a single target under the
// per-domain lock. Every terminal path finalizes the job row so the attempt
// is always auditable; the returned error is collected by Reconcile.
func (m *Manager) reconcileOne(ctx context.Context, t ReconcileTarget, actor string) error {
	adapter, ok := m.adapters[t.Domain]
	if !ok {
		// No adapter → nothing to reconcile. Record a failed job so the skip
		// is auditable, then let Reconcile continue to the next target.
		jobID := m.newID()
		_ = m.db.CreateJob(&model.ConfigApplyJob{
			ID: jobID, Domain: t.Domain, Key: t.Key, Action: "reconcile",
			Status: "running", StartedAt: m.clock(), FromRevision: 0, ToRevision: 0,
		})
		_ = m.db.FinishJob(jobID, "failed", "reconcile: unknown domain", 0)
		return fmt.Errorf("%w: %s", ErrUnknownDomain, t.Domain)
	}

	m.domainMu(t.Domain).Lock()
	defer m.domainMu(t.Domain).Unlock()

	// Read desired first so CreateJob can record the from-revision accurately.
	desired, found, qerr := m.db.Get(t.Domain, t.Key)
	if qerr != nil {
		jobID := m.newID()
		_ = m.db.CreateJob(&model.ConfigApplyJob{
			ID: jobID, Domain: t.Domain, Key: t.Key, Action: "reconcile",
			Status: "running", StartedAt: m.clock(), FromRevision: 0, ToRevision: 0,
		})
		_ = m.db.FinishJob(jobID, "failed", "reconcile: get: "+qerr.Error(), 0)
		return fmt.Errorf("get: %w", qerr)
	}

	fromRev := 0
	if found {
		fromRev = desired.Revision
	}
	jobID := m.newID()
	if jerr := m.db.CreateJob(&model.ConfigApplyJob{
		ID: jobID, Domain: t.Domain, Key: t.Key, Action: "reconcile",
		Status: "running", StartedAt: m.clock(), FromRevision: fromRev, ToRevision: 0,
	}); jerr != nil {
		return fmt.Errorf("create job: %w", jerr)
	}

	fail := func(stage string, e error) error {
		_ = m.db.FinishJob(jobID, "failed", "reconcile: "+stage+": "+e.Error(), 0)
		return fmt.Errorf("%s: %w", stage, e)
	}
	done := func(msg string, toRev int) error {
		_ = m.db.FinishJob(jobID, "success", msg, toRev)
		return nil
	}

	if found {
		desiredValue := desired.ValueJSON
		desiredRev := fromRev
		normalized, changed, e := normalizeDesired(ctx, adapter, t.Key, desiredValue)
		if e != nil {
			return fail("normalize", e)
		}
		if changed {
			rev, e := m.db.AppendRevision(t.Domain, t.Key, normalized, "normalize", actor)
			if e != nil {
				return fail("append-normalized-revision", e)
			}
			if e := m.db.Upsert(&model.ConfigItem{
				Domain: t.Domain, Key: t.Key, ValueJSON: normalized,
				SchemaVersion: 1, Revision: rev, UpdatedAt: m.clock(), UpdatedBy: actor,
			}); e != nil {
				return fail("upsert-normalized-item", e)
			}
			desiredValue = normalized
			desiredRev = rev
		}

		// Desired exists: verify live matches. nil → in-sync no-op.
		if e := adapter.Verify(ctx, t.Key, desiredValue); e == nil {
			if changed {
				return done("normalized", desiredRev)
			}
			return done("in-sync", desiredRev)
		} else {
			// Drift (incl. missing live file). Re-project desired, then re-verify.
			// Desired is the source of truth: do NOT Restore on verify failure.
			rendered, e := adapter.Render(ctx, t.Key, desiredValue)
			if e != nil {
				return fail("render", e)
			}
			if e := adapter.Apply(ctx, t.Key, rendered); e != nil {
				return fail("apply", e)
			}
			if e := adapter.Verify(ctx, t.Key, desiredValue); e != nil {
				return fail("verify after re-project", e)
			}
			if changed {
				return done("normalized and re-projected", desiredRev)
			}
			return done("re-projected", desiredRev)
		}
	}

	// !found: R-migration. Import live state into the empty store. If the
	// snapshot normalizes, re-project the normalized state once.
	snap, isSnap := adapter.(Snapshotter)
	if !isSnap {
		return done("not snapshot-capable, skip", 0)
	}
	live, e := snap.Snapshot(ctx, t.Key)
	if e != nil {
		if errors.Is(e, os.ErrNotExist) {
			return done("no live file, skip", 0)
		}
		return fail("snapshot", e)
	}
	liveValue := live
	normalized, changed, e := normalizeDesired(ctx, adapter, t.Key, live)
	if e != nil {
		return fail("normalize snapshot", e)
	}
	if changed {
		liveValue = normalized
	}
	rev, e := m.db.AppendRevision(t.Domain, t.Key, liveValue, "import", actor)
	if e != nil {
		return fail("append-revision", e)
	}
	if e := m.db.Upsert(&model.ConfigItem{
		Domain: t.Domain, Key: t.Key, ValueJSON: liveValue,
		SchemaVersion: 1, Revision: rev, UpdatedAt: m.clock(), UpdatedBy: actor,
	}); e != nil {
		return fail("upsert-item", e)
	}
	if changed {
		rendered, e := adapter.Render(ctx, t.Key, liveValue)
		if e != nil {
			return fail("render imported normalized", e)
		}
		if e := adapter.Apply(ctx, t.Key, rendered); e != nil {
			return fail("apply imported normalized", e)
		}
		if e := adapter.Verify(ctx, t.Key, liveValue); e != nil {
			return fail("verify imported normalized", e)
		}
		return done("imported normalized and re-projected", rev)
	}
	return done("imported", rev)
}
