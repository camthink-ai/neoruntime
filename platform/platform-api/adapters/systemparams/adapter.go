// Package systemparams is the DB-only Config Controller adapter for the
// "system" domain (system settings KV). It wraps repo.SettingRepo so that
// setting writes flow through the Manager state machine: validate -> backup ->
// render -> apply -> verify (with auto-restore on failure) -> revision + audit.
//
// Unlike the file-projecting adapters (network/time/device_info), there is no
// file side-effect: the live store (settings table) IS the desired state. The
// Manager still records a desired-state row in config_items and a revision in
// config_revisions, giving settings the same history/audit/job tracking as
// every other domain.
package systemparams

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"

	"aipc/platform/platform-api/repo"
)

// desiredEnvelope is the JSON shape stored as the desired state for a setting.
// Wrapping the free-form string value in an object keeps the desired-state
// record self-describing and leaves room for future metadata without changing
// the on-the-wire setting value.
type desiredEnvelope struct {
	Value string `json:"value"`
}

// ErrInvalidJSON is returned by Validate when the desired JSON is malformed.
var ErrInvalidJSON = errors.New("systemparams: invalid desired json")

// Adapter is the system-params Config Controller adapter. It is safe for
// concurrent use because the underlying SettingRepo serializes via GORM.
type Adapter struct {
	store settingStore
}

// settingStore is the subset of *repo.SettingRepo the adapter needs, kept as an
// interface so tests can inject a fake (and so the adapter does not import a
// concrete gorm-backed type at test time).
type settingStore interface {
	Get(key string) (string, error)
	Set(key, value string) error
}

// New returns a system-params adapter backed by the given SettingRepo.
func New(store *repo.SettingRepo) *Adapter {
	return &Adapter{store: store}
}

// Validate parses the desired JSON envelope.
func (a *Adapter) Validate(ctx context.Context, key, desiredJSON string) error {
	var env desiredEnvelope
	if err := json.Unmarshal([]byte(desiredJSON), &env); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalidJSON, err)
	}
	return nil
}

// Backup reads the current live value so the Manager can Restore on failure.
// The backup is the raw string value (or "" if the key is new).
func (a *Adapter) Backup(ctx context.Context, key string) (any, error) {
	v, err := a.store.Get(key)
	if err != nil {
		return nil, fmt.Errorf("backup get: %w", err)
	}
	return v, nil
}

// Render is a passthrough: the desired JSON is already the rendered form for a
// DB-only domain (no file projection step).
func (a *Adapter) Render(ctx context.Context, key, desiredJSON string) (any, error) {
	return desiredJSON, nil
}

// Apply writes the desired value to the live settings store.
func (a *Adapter) Apply(ctx context.Context, key string, rendered any) error {
	desiredJSON, ok := rendered.(string)
	if !ok {
		return fmt.Errorf("apply: unexpected rendered type %T", rendered)
	}
	var env desiredEnvelope
	if err := json.Unmarshal([]byte(desiredJSON), &env); err != nil {
		return fmt.Errorf("apply: %w: %v", ErrInvalidJSON, err)
	}
	if err := a.store.Set(key, env.Value); err != nil {
		return fmt.Errorf("apply set: %w", err)
	}
	return nil
}

// Verify confirms the live value matches the desired value.
func (a *Adapter) Verify(ctx context.Context, key, desiredJSON string) error {
	var env desiredEnvelope
	if err := json.Unmarshal([]byte(desiredJSON), &env); err != nil {
		return fmt.Errorf("verify: %w: %v", ErrInvalidJSON, err)
	}
	got, err := a.store.Get(key)
	if err != nil {
		return fmt.Errorf("verify get: %w", err)
	}
	if got != env.Value {
		return fmt.Errorf("verify: live %q != desired %q", got, env.Value)
	}
	return nil
}

// Restore writes the backed-up value back to the live store.
func (a *Adapter) Restore(ctx context.Context, key string, backup any) error {
	prev, _ := backup.(string)
	if err := a.store.Set(key, prev); err != nil {
		return fmt.Errorf("restore set: %w", err)
	}
	return nil
}
