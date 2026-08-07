// Package media is the Config Controller adapter for the media domain. It owns
// the camera-daemon.yaml config file — the media pipeline config (encoders,
// streams, rtsp, image/transform sections) that platform-api reads and writes.
//
// The adapter is SINGLE-KEY ("config"): the desired value IS the full YAML the
// handler already produces via yaml.Marshal of its config map. Returning that
// YAML verbatim in Render (rather than round-tripping through JSON) is
// deliberate: a YAML→JSON→YAML cycle would drift integer formatting
// (bitrate 8000000 → 8e+06), changing the file's bytes even though the value
// is semantically equal. By treating the handler's marshaled YAML as the
// desired state, the adapter preserves byte-for-byte output and still gives
// the Manager atomic write + read-back Verify + auto-Restore + revision/audit.
//
// All seven os.WriteFile(h.configPath, ...) sites in handlers/media.go
// (SetConfig, writeStreamHotReload, writeAppliedStreamsToConfig,
// writeStreamToConfig, addStreamToConfig, removeStreamFromConfig,
// setStreamEnabledInConfig) write the SAME file and already produce the full
// config YAML before writing, so they collapse onto this one key.
//
// The adapter only projects the file. The live pipeline reconfiguration — gRPC
// UpdateEncoderConfig / ReloadStreams / ReconfigurePipeline — stays in the
// handler post-Apply, exactly as the time and network adapters keep their
// exec/restart in the handler. camera-daemon.yaml is platform-api-owned
// (camera-daemon reads it, never writes it at runtime), so a byte-compare
// Verify is safe: no concurrent writer can make a just-written file mismatch.
//
// Per the Phase 0 decision, OSD and privacy-mask are self-projected by
// camera-daemon to their own JSON files; this adapter does NOT touch those —
// routing their gRPC calls here would double-write.
package media

import (
	"context"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"gopkg.in/yaml.v3"

	"aipc/platform/common/constants"
	"aipc/platform/platform-api/internal/atomicfile"
)

const (
	// keyConfig is the only key: desired = full camera-daemon.yaml content.
	keyConfig = "config"
)

var (
	// ErrUnknownKey is returned when key is not "config".
	ErrUnknownKey = errors.New("media: unknown key")
	// ErrInvalidYAML is returned when the desired value is not parseable YAML.
	ErrInvalidYAML = errors.New("media: desired is not valid YAML")
	// ErrBadRenderedType is returned by Apply when rendered is not YAML bytes.
	ErrBadRenderedType = errors.New("media: rendered value is not YAML bytes")
	// ErrBadBackupType is returned by Restore when backup is not file bytes.
	ErrBadBackupType = errors.New("media: backup value is not file bytes")
)

// Adapter owns the camera-daemon.yaml file for the media domain.
type Adapter struct {
	configPath string
}

// New returns a media Adapter. configPath defaults to
// constants.ConfigPath()+"/camera-daemon.yaml" when empty so production uses
// the canonical install root while tests inject a t.TempDir() path.
func New(configPath string) *Adapter {
	if configPath == "" {
		configPath = constants.ConfigPath() + "/camera-daemon.yaml"
	}
	return &Adapter{configPath: configPath}
}

// Normalize migrates release-owned paths inside camera-daemon.yaml before the
// Config Manager persists or re-projects desired state. This prevents stale DB
// rows from older install roots from overwriting the staged release config on
// platform-api startup.
func (a *Adapter) Normalize(_ context.Context, key, desiredJSON string) (string, bool, error) {
	if key != keyConfig {
		return "", false, ErrUnknownKey
	}
	var cfg map[string]interface{}
	if err := yaml.Unmarshal([]byte(desiredJSON), &cfg); err != nil {
		return "", false, ErrInvalidYAML
	}
	if cfg == nil {
		return desiredJSON, false, nil
	}

	changed := false
	if ch, err := normalizeHALPaths(cfg); err != nil {
		return "", false, err
	} else if ch {
		changed = true
	}
	if ch, err := normalizeMediaPaths(cfg); err != nil {
		return "", false, err
	} else if ch {
		changed = true
	}
	if normalizeEncoderDims(cfg) {
		changed = true
	}
	if !changed {
		return desiredJSON, false, nil
	}
	out, err := yaml.Marshal(cfg)
	if err != nil {
		return "", false, err
	}
	return string(out), true, nil
}

func normalizeHALPaths(cfg map[string]interface{}) (bool, error) {
	halRaw, ok := cfg["hal"]
	if !ok {
		return false, nil
	}
	hal, ok := asStringMap(halRaw)
	if !ok {
		return false, errors.New("media: hal section must be a mapping")
	}
	cfg["hal"] = hal

	libaipc := filepath.Join(constants.LibPath(), "hal", "libaipc_hal.so")
	lens := filepath.Join(constants.LibPath(), "hal", "libhal-lens-bridge.so")
	for _, path := range []string{libaipc, lens} {
		if !fileExists(path) {
			return false, fmt.Errorf("media: canonical HAL library missing: %s", path)
		}
	}

	changed := false
	changed = setStringIfDifferent(hal, "video_library", libaipc) || changed
	changed = setStringIfDifferent(hal, "codec_library", libaipc) || changed
	changed = setStringIfDifferent(hal, "lens_library", lens) || changed
	return changed, nil
}

func normalizeMediaPaths(cfg map[string]interface{}) (bool, error) {
	mediaRaw, ok := cfg["media"]
	if !ok {
		return false, nil
	}
	media, ok := asStringMap(mediaRaw)
	if !ok {
		return false, errors.New("media: media section must be a mapping")
	}
	cfg["media"] = media

	changed := false
	if _, ok := media["backup_path"]; ok {
		changed = setStringIfDifferent(media, "backup_path", filepath.Join(constants.DataPath(), "media-backup")) || changed
	}
	// config_path is no longer platform-maintained: camera-daemon falls back to the
	// HAL compiled-in default medialib config when it is absent. Drop any config_path
	// a prior install or DB blob still carries so devices converge on the default
	// rather than persisting a stale module-specific path. Normalize is the single
	// chokepoint covering both the save path (Manager Apply) and the boot DB→YAML
	// re-project, so a stale config_path in the DB is self-healed on the first boot
	// after this code ships.
	if _, ok := media["config_path"]; ok {
		delete(media, "config_path")
		changed = true
	}
	return changed, nil
}

// normalizeEncoderDims enforces the canonical landscape geometry for every
// encoder: a portrait-transposed pair (height>width) is swapped back to
// landscape. The platform sensor is natively landscape (e.g. 3840×2160); portrait
// output is a runtime rotation transform the HAL applies AFTER pipeline init by
// swapping input_stream W↔H, so portrait dims must never be baked into the
// encoder dimensions stored in camera-daemon.yaml.
//
// A stale portrait dim set in the DB (left by a save while rotation was 90/270)
// gets replayed to the YAML on every platform-api boot and makes the pipeline
// come up landscape while HAL patches encoders portrait → add_buffer fails for
// every video sink → /media black screen (audio is a separate path, unaffected).
//
// Running this in Normalize covers BOTH the save path (Manager Apply) and the
// boot re-project (DB → YAML), so a portrait DB blob is self-healed to landscape
// on the first boot after this code ships, and the healed value persists back to
// the DB. Square pairs (width==height) and already-landscape pairs are untouched.
func normalizeEncoderDims(cfg map[string]interface{}) bool {
	encodersRaw, ok := cfg["encoders"]
	if !ok {
		return false
	}
	encoders, ok := encodersRaw.([]interface{})
	if !ok {
		return false
	}
	changed := false
	for _, item := range encoders {
		m, ok := item.(map[string]interface{})
		if !ok {
			continue
		}
		w, wOk := toInt(m["width"])
		h, hOk := toInt(m["height"])
		if !wOk || !hOk || h <= w {
			continue
		}
		// height > width: portrait-transposed residual — swap to landscape.
		m["width"] = h
		m["height"] = w
		changed = true
	}
	return changed
}

// toInt coerces a YAML-decoded scalar (int or int64) to int. yaml.v3 decodes
// small integers into interface{} as int, but defends against int64 for values
// near int32 boundaries.
func toInt(v interface{}) (int, bool) {
	switch n := v.(type) {
	case int:
		return n, true
	case int64:
		return int(n), true
	default:
		return 0, false
	}
}

func asStringMap(v interface{}) (map[string]interface{}, bool) {
	switch m := v.(type) {
	case map[string]interface{}:
		return m, true
	case map[interface{}]interface{}:
		out := make(map[string]interface{}, len(m))
		for k, val := range m {
			ks, ok := k.(string)
			if !ok {
				return nil, false
			}
			out[ks] = val
		}
		return out, true
	default:
		return nil, false
	}
}

func setStringIfDifferent(m map[string]interface{}, key, want string) bool {
	if got, ok := m[key].(string); ok && cleanPath(got) == want {
		return false
	}
	m[key] = want
	return true
}

func cleanPath(path string) string {
	path = strings.TrimSpace(strings.Trim(path, `"'`))
	if path == "" {
		return ""
	}
	return filepath.Clean(path)
}

func fileExists(path string) bool {
	st, err := os.Stat(path)
	return err == nil && !st.IsDir()
}

// backupState holds the file's pre-Apply bytes. A nil slice means the file did
// not exist before Apply, in which case Restore removes whatever Apply created.
type backupState struct{ bytes []byte }

// rendered is the YAML byte slice to write. It is a distinct type so Apply can
// type-assert and reject a mis-typed rendered value.
type rendered []byte

// Validate parses desiredJSON as YAML. The media config is a generic
// map[string]interface{} (the handler unmarshals into exactly that), so any
// parseable YAML document is accepted; non-YAML input fails the job fast.
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
// avoids a YAML→JSON→YAML round-trip that would drift integer formatting.
func (a *Adapter) Render(ctx context.Context, key, desiredJSON string) (any, error) {
	if key != keyConfig {
		return nil, ErrUnknownKey
	}
	return rendered([]byte(desiredJSON)), nil
}

// Apply atomically writes the rendered YAML to the config file. The parent
// directory is created if missing (defensive — the install root normally
// exists; matches the network adapter).
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
// camera-daemon.yaml is platform-api-owned (no runtime concurrent writer), so
// a mismatch indicates external tampering or disk error and triggers Restore.
func (a *Adapter) Verify(ctx context.Context, key, desiredJSON string) error {
	if key != keyConfig {
		return ErrUnknownKey
	}
	got, err := os.ReadFile(a.configPath)
	if err != nil {
		return err
	}
	if string(got) != desiredJSON {
		return errors.New("media: file content does not match desired")
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

// Snapshot reads the current file bytes and returns them as desiredJSON. It
// implements config.Snapshotter so the Manager's Reconcile can import a
// pre-existing live camera-daemon.yaml into an empty desired-state store. If
// Normalize upgrades release-owned paths in that snapshot, Reconcile records
// the normalized desired state and writes it back once.
//
// The media desired value IS the full file content (Render returns
// rendered([]byte(desiredJSON)), Apply writes it, Verify byte-compares), so a
// Snapshot of an already-canonical live file is exactly the desiredJSON that
// Apply would re-project byte-identically.
//
// A missing file returns os.ErrNotExist, which Reconcile treats as "nothing to
// import" (non-error no-op). Any other read error is surfaced as a job failure.
func (a *Adapter) Snapshot(ctx context.Context, key string) (string, error) {
	if key != keyConfig {
		return "", ErrUnknownKey
	}
	b, err := os.ReadFile(a.configPath)
	if err != nil {
		return "", err
	}
	return string(b), nil
}
