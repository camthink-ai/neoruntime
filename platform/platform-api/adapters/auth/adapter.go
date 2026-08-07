// Package auth is the Config Controller adapter for the auth domain. It owns
// the platform-api.yaml config file — the file platform-api reads at startup
// for its auth section (username / password / token_key / enabled), loaded once
// into the process at boot (server/main.go reads cfg.Auth.* before serving).
//
// Like the media adapter, this is SINGLE-KEY ("config"): the desired value IS
// the full YAML the handler already produces via yaml.Marshal of its config
// map. Returning that YAML verbatim in Render (rather than round-tripping
// through JSON) is deliberate: a YAML→JSON→YAML cycle would drift scalar
// formatting and change the file's bytes even though the value is semantically
// equal. By treating the handler's marshaled YAML as the desired state, the
// adapter preserves byte-for-byte output and still gives the Manager atomic
// write + read-back Verify + auto-Restore + revision/audit.
//
// The only auth write site is handlers/system.go UpdatePassword
// (`os.WriteFile(h.configPath, outData, 0644)` at :150), which writes the full
// file after mutating the auth.password section. It collapses onto this one
// key.
//
// The adapter only projects the file. The live effect — `systemctl restart
// platform-api` so the new password takes effect — stays in the handler
// post-Apply, exactly as the time / network / media adapters keep their
// exec / restart / gRPC in the handler. platform-api.yaml is platform-api-owned
// (only platform-api reads it at startup; camera-daemon never touches it), so a
// byte-compare Verify is safe: no concurrent writer can make a just-written
// file mismatch.
//
// R-auth-rollback (the auth-specific risk): changing one's own auth config +
// restarting oneself is unrecoverable if the new config breaks startup — the
// API is unreachable to fix it. The adapter's byte-Verify cannot catch a
// semantically-broken-but-syntactically-valid config, so the handler schedules
// a DETACHED `systemd-run` probe (separate cgroup, survives platform-api's
// stop) that watches `systemctl is-active platform-api` after the restart and,
// on sustained failure, restores the pre-Apply backup file + restarts again.
// That detached probe + its pure command-builder live in rollback_probe.go;
// this adapter is the file-projection core.
package auth

import (
	"context"
	"errors"
	"os"
	"path/filepath"

	"gopkg.in/yaml.v3"

	"aipc/platform/common/constants"
	"aipc/platform/platform-api/internal/atomicfile"
)

const (
	// keyConfig is the only key: desired = full platform-api.yaml content.
	keyConfig = "config"
)

var (
	// ErrUnknownKey is returned when key is not "config".
	ErrUnknownKey = errors.New("auth: unknown key")
	// ErrInvalidYAML is returned when the desired value is not parseable YAML.
	ErrInvalidYAML = errors.New("auth: desired is not valid YAML")
	// ErrBadRenderedType is returned by Apply when rendered is not YAML bytes.
	ErrBadRenderedType = errors.New("auth: rendered value is not YAML bytes")
	// ErrBadBackupType is returned by Restore when backup is not file bytes.
	ErrBadBackupType = errors.New("auth: backup value is not file bytes")
)

// Adapter owns the platform-api.yaml file for the auth domain.
type Adapter struct {
	configPath string
}

// New returns an auth Adapter. configPath defaults to
// constants.ConfigPath()+"/platform-api.yaml" when empty so production uses the
// canonical install root while tests inject a t.TempDir() path.
func New(configPath string) *Adapter {
	if configPath == "" {
		configPath = constants.ConfigPath() + "/platform-api.yaml"
	}
	return &Adapter{configPath: configPath}
}

// backupState holds the file's pre-Apply bytes. A nil slice means the file did
// not exist before Apply, in which case Restore removes whatever Apply created.
type backupState struct{ bytes []byte }

// rendered is the YAML byte slice to write. It is a distinct type so Apply can
// type-assert and reject a mis-typed rendered value.
type rendered []byte

// Validate parses desiredJSON as YAML. The platform-api config is a generic
// map[string]interface{} (the handler unmarshals into exactly that), so any
// parseable YAML document is accepted; non-YAML input fails the job fast. This
// is the precheck gate: a desired value that is not even parseable YAML is
// rejected before any file is touched, so a malformed config can never be
// written and then crash platform-api on the post-Apply restart.
func (a *Adapter) Validate(ctx context.Context, key, desiredJSON string) error {
	if key != keyConfig {
		return ErrUnknownKey
	}
	var m map[string]interface{}
	if err := yaml.Unmarshal([]byte(desiredJSON), &m); err != nil {
		return ErrInvalidYAML
	}
	return nil
}

// Backup reads the current file bytes. A missing file yields a nil-byte
// backupState (not an error): Restore will then remove any file Apply created.
func (a *Adapter) Backup(ctx context.Context, key string) (any, error) {
	if key != keyConfig {
		return nil, ErrUnknownKey
	}
	b, err := os.ReadFile(a.configPath)
	if err != nil {
		if os.IsNotExist(err) {
			return backupState{nil}, nil
		}
		return nil, err
	}
	return backupState{b}, nil
}

// Render returns the desired YAML verbatim as the bytes to write. The handler
// produces this YAML via yaml.Marshal of its config map; returning it unchanged
// avoids a YAML→JSON→YAML round-trip that would drift scalar formatting.
func (a *Adapter) Render(ctx context.Context, key, desiredJSON string) (any, error) {
	if key != keyConfig {
		return nil, ErrUnknownKey
	}
	return rendered([]byte(desiredJSON)), nil
}

// Apply atomically writes the rendered YAML to the config file. The parent
// directory is created if missing (defensive — the install root normally
// exists; matches the media / network adapters).
func (a *Adapter) Apply(ctx context.Context, key string, r any) error {
	if key != keyConfig {
		return ErrUnknownKey
	}
	rd, ok := r.(rendered)
	if !ok {
		return ErrBadRenderedType
	}
	if err := os.MkdirAll(filepath.Dir(a.configPath), 0755); err != nil {
		return err
	}
	return atomicfile.Write(a.configPath, []byte(rd), 0644)
}

// Verify reads the file back and byte-compares against the desired YAML.
// platform-api.yaml is platform-api-owned (no runtime concurrent writer), so a
// mismatch indicates external tampering or disk error and triggers Restore.
func (a *Adapter) Verify(ctx context.Context, key, desiredJSON string) error {
	if key != keyConfig {
		return ErrUnknownKey
	}
	got, err := os.ReadFile(a.configPath)
	if err != nil {
		return err
	}
	if string(got) != desiredJSON {
		return errors.New("auth: file content does not match desired")
	}
	return nil
}

// Restore reverts the file to the backup. A nil-byte backup means the file did
// not exist pre-Apply; Restore removes the file Apply created (missing-file is
// not an error). A non-nil backup is written atomically.
func (a *Adapter) Restore(ctx context.Context, key string, backup any) error {
	if key != keyConfig {
		return ErrUnknownKey
	}
	bs, ok := backup.(backupState)
	if !ok {
		return ErrBadBackupType
	}
	if bs.bytes == nil {
		if err := os.Remove(a.configPath); err != nil && !os.IsNotExist(err) {
			return err
		}
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(a.configPath), 0755); err != nil {
		return err
	}
	return atomicfile.Write(a.configPath, bs.bytes, 0644)
}

// ConfigPath returns the file path this adapter projects. Handlers use it to
// stage the pre-Apply backup that the detached R-auth-rollback probe restores.
func (a *Adapter) ConfigPath() string { return a.configPath }
