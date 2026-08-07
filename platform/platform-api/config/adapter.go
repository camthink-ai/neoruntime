// Package config implements the Config Controller: a desired-state store that
// drives live system configuration through per-domain adapters.
//
// The Manager is the single entry point for mutating config. It owns the
// apply state machine (Validate → Backup → Render → Apply → Verify, with
// auto-Restore on Verify failure), records every attempt as a ConfigApplyJob,
// and appends a ConfigRevision on success. Adapters encapsulate the
// domain-specific details of what "applying" means (writing a file + reloading
// a service, issuing a gRPC hot-reload, exec'ing hostname, etc.).
package config

import "context"

// Adapter translates a desired JSON value for one configuration domain into a
// live-system change and verifies it took effect. Each domain (media, network,
// time, device_info, auth, system_params) registers one Adapter with the
// Manager.
//
// The Backup/Rendered values are opaque to the Manager: an adapter may use a
// []byte (file bytes), a proto message, or any struct it round-trips itself.
// The Manager only threads Backup → Restore and Render → Apply.
type Adapter interface {
	// Validate checks that desiredJSON is well-formed and acceptable for
	// (domain, key) before any live change is attempted. Returning an error
	// here fails the job fast with no side effects.
	Validate(ctx context.Context, key, desiredJSON string) error

	// Backup captures the current live state for (domain, key) so Restore can
	// revert to it. Return nil backup if there is nothing to revert from
	// (first-time config). The returned value is passed unchanged to Restore.
	Backup(ctx context.Context, key string) (backup any, err error)

	// Render converts desiredJSON into the concrete projection the adapter
	// will apply — e.g. file bytes for a file adapter, or a prepared request
	// for a gRPC adapter. For adapters that act directly on desiredJSON in
	// Apply, Render may return the input unchanged (or nil).
	Render(ctx context.Context, key, desiredJSON string) (rendered any, err error)

	// Apply pushes the rendered state to the live system. It must be
	// idempotent: applying the same rendered value twice is a no-op beyond the
	// first call.
	Apply(ctx context.Context, key string, rendered any) error

	// Verify reads back the live state and confirms it matches desiredJSON.
	// A non-nil error triggers an automatic Restore in the Manager.
	Verify(ctx context.Context, key, desiredJSON string) error

	// Restore reverts the live system to the state captured by Backup. Called
	// by the Manager only after a failed Verify. A best-effort restore that
	// cannot fully revert must still return nil if the system is left in a
	// safe (if older) state, and surface the residual via the job error.
	Restore(ctx context.Context, key string, backup any) error
}

// Snapshotter is an optional capability an Adapter may implement to expose the
// current live state for a key as a desiredJSON string — the inverse of Apply.
// The Manager's Reconcile uses it to import pre-existing live state into an
// empty desired-state store (the R-migration case). If the adapter also
// implements Normalizer and the snapshot contains stale release-owned values,
// Reconcile records the normalized state and re-projects it once.
//
// An adapter that does not implement Snapshotter is skipped by Reconcile when
// no desired row exists (Reconcile cannot discover live state for it). This is
// non-breaking: the six Phase 1 adapters do not implement it and are unaffected.
//
// Snapshot should return the desiredJSON that, if passed to Apply, would leave
// the live state unchanged before optional normalization. A missing live state
// — typically os.ErrNotExist for a file-backed adapter — is not an error:
// Reconcile treats it as "nothing to import" and records a no-op job.
type Snapshotter interface {
	Snapshot(ctx context.Context, key string) (desiredJSON string, err error)
}

// Normalizer is an optional capability for adapters that own desired-state
// fields whose safe value is version/platform dependent. The Manager calls it
// before Validate/Render/Apply and during Reconcile so stale desired rows from
// older releases are migrated before they can be re-projected to disk.
//
// Return changed=false when desiredJSON is already canonical. A normalized
// value returned with changed=true becomes the value stored in config_items and
// config_revisions.
type Normalizer interface {
	Normalize(ctx context.Context, key, desiredJSON string) (normalizedJSON string, changed bool, err error)
}
