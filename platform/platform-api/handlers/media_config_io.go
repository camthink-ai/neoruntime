package handlers

// Unified media-config import/export (Option B aggregation layer).
//
// The device media config is fragmented across one base YAML
// (camera-daemon.yaml) and seven runtime-override JSON files under
// /data/aipc/etc (osd / privacy_mask / transform / isp / profile /
// media_config_fields / lens). No single component holds the full picture,
// which is the real obstacle for "clone this device's config onto another
// device".
//
// Export aggregates all seven sources into one versioned envelope (pure read,
// zero side effects). Import validates the envelope, snapshots the current
// files for rollback, atomically writes them back, then restarts
// camera-daemon and device-control whose boot replays are the apply engine.
// camera-daemon's loaders are fault-tolerant (miss/corrupt = WARN+skip, not
// abort), so even a bad import degrades gracefully instead of bricking boot.
//
// This layer intentionally does NOT touch typed RPCs, existing handlers, the
// proto, or C++. It only reads/writes the same files the rest of the system
// already uses.

import (
	"context"
	"encoding/json"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"time"

	"github.com/gin-gonic/gin"
	"gopkg.in/yaml.v3"

	eventLoggerPkg "aipc/platform/common/events"
	"aipc/platform/common/logger"
)

// schema identifiers for the unified media-config envelope.
const (
	mediaConfigSchema  = "aipc.media_config"
	mediaConfigVersion = 1
)

// Runtime-override JSON files written by camera-daemon. Paths mirror the
// constants in platform/camera-daemon/src/camera_daemon.cpp.
const (
	mediaEtcDir         = "/data/aipc/etc"
	osdConfigPath       = mediaEtcDir + "/osd_config.json"
	privacyMaskCfgPath  = mediaEtcDir + "/privacy_mask.json"
	transformCfgPath    = mediaEtcDir + "/transform_config.json"
	ispCfgPath          = mediaEtcDir + "/isp_config.json"
	profileCfgPath      = mediaEtcDir + "/profile_config.json"
	scalarFieldsCfgPath = mediaEtcDir + "/media_config_fields.json"
	lensCfgPath         = mediaEtcDir + "/lens_config.json"
	mediaBackupRoot     = mediaEtcDir + "/backup"
)

// mediaConfigEnvelope is the versioned, self-describing config snapshot.
type mediaConfigEnvelope struct {
	Schema     string             `json:"schema"`      // "aipc.media_config"
	Version    int                `json:"version"`     // 1
	ExportedAt string             `json:"exported_at"` // RFC3339 UTC
	Device     map[string]string  `json:"device,omitempty"`
	Config     mediaConfigPayload `json:"config"`
}

// mediaConfigPayload holds the eight config sources. The seven JSON dimensions
// use json.RawMessage so they round-trip verbatim (no float/int coercion);
// base_yaml is a map to match GetConfig's shape and to let import reuse the
// YAML marshal/validate helpers. Omitempty means "missing file on export"
// and "leave that file untouched on import".
type mediaConfigPayload struct {
	BaseYAML     map[string]interface{} `json:"base_yaml,omitempty"`
	Osd          json.RawMessage        `json:"osd,omitempty"`
	PrivacyMask  json.RawMessage        `json:"privacy_mask,omitempty"`
	Transform    json.RawMessage        `json:"transform,omitempty"`
	Isp          json.RawMessage        `json:"isp,omitempty"`
	Profile      json.RawMessage        `json:"profile,omitempty"`
	ScalarFields json.RawMessage        `json:"scalar_fields,omitempty"`
	Lens         json.RawMessage        `json:"lens,omitempty"`
}

// mediaConfigDim ties a payload field to its on-disk path for the import loop.
type mediaConfigDim struct {
	name string
	path string
	raw  json.RawMessage
}

// payloadDims returns the seven JSON dimensions present in the payload.
func (p mediaConfigPayload) payloadDims() []mediaConfigDim {
	return []mediaConfigDim{
		{"osd", osdConfigPath, p.Osd},
		{"privacy_mask", privacyMaskCfgPath, p.PrivacyMask},
		{"transform", transformCfgPath, p.Transform},
		{"isp", ispCfgPath, p.Isp},
		{"profile", profileCfgPath, p.Profile},
		{"scalar_fields", scalarFieldsCfgPath, p.ScalarFields},
		{"lens", lensCfgPath, p.Lens},
	}
}

// isEmpty reports whether the payload carries no applyable config at all.
func (p mediaConfigPayload) isEmpty() bool {
	if len(p.BaseYAML) != 0 {
		return false
	}
	for _, d := range p.payloadDims() {
		if len(d.raw) != 0 {
			return false
		}
	}
	return true
}

// assignRaw sets the payload field that corresponds to path. It centralizes
// the path→field mapping so export/import resolve every dimension through one
// switch instead of six handwritten assignments each.
func (p *mediaConfigPayload) assignRaw(path string, raw json.RawMessage) {
	switch path {
	case osdConfigPath:
		p.Osd = raw
	case privacyMaskCfgPath:
		p.PrivacyMask = raw
	case transformCfgPath:
		p.Transform = raw
	case ispCfgPath:
		p.Isp = raw
	case profileCfgPath:
		p.Profile = raw
	case scalarFieldsCfgPath:
		p.ScalarFields = raw
	case lensCfgPath:
		p.Lens = raw
	}
}

// readJSONRaw loads a JSON file as opaque bytes. Missing file or invalid JSON
// yields nil (best-effort, mirroring camera-daemon's tolerant loaders — one
// corrupt dimension must not abort the whole export).
func readJSONRaw(path string) json.RawMessage {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil
	}
	if !json.Valid(data) {
		logger.Warn("media config export: skipping non-JSON file %s", path)
		return nil
	}
	return json.RawMessage(data)
}

// atomicWriteJSON validates then writes a JSON file via tmp+rename (0644),
// mirroring camera-daemon's persist_* pattern so a crash mid-write cannot
// leave a truncated config behind.
func atomicWriteJSON(path string, raw json.RawMessage) error {
	if !json.Valid(raw) {
		return fmt.Errorf("refusing to write invalid JSON to %s", path)
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, raw, 0644); err != nil {
		return fmt.Errorf("write %s: %w", tmp, err)
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return fmt.Errorf("rename %s: %w", path, err)
	}
	return nil
}

// snapshotMediaConfig copies the current base YAML and every existing runtime
// JSON into targetDir so a botched import can be reverted by hand. Missing
// files are skipped; the base YAML path comes from the receiver.
func (h *MediaHandlers) snapshotMediaConfig(targetDir string) error {
	if err := os.MkdirAll(targetDir, 0755); err != nil {
		return fmt.Errorf("create backup dir: %w", err)
	}
	sources := append([]string{h.configPath},
		osdConfigPath, privacyMaskCfgPath, transformCfgPath,
		ispCfgPath, profileCfgPath, scalarFieldsCfgPath, lensCfgPath)
	for _, src := range sources {
		if src == "" {
			continue
		}
		data, err := os.ReadFile(src)
		if err != nil {
			continue // file not present yet — nothing to back up
		}
		dst := filepath.Join(targetDir, filepath.Base(src))
		if err := os.WriteFile(dst, data, 0644); err != nil {
			return fmt.Errorf("backup %s: %w", src, err)
		}
	}
	return nil
}

// ExportMediaConfig (GET /api/v1/media/config/export) streams the versioned
// envelope as a downloadable .json. The file IS the body /config/import expects
// (the bare envelope) — it is NOT wrapped in APIResponse's {code,data}: that
// wrapper would be saved verbatim by the client and arrive at import without a
// top-level schema, failing validation as `unsupported schema ""`. Mirrors the
// tar.gz streaming shape used by ExportMediaBundle / ExportDeviceConfig so all
// three exports hand the client a bare, self-describing file. Pure read.
func (h *MediaHandlers) ExportMediaConfig(c *gin.Context) {
	env, err := h.buildMediaEnvelope()
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to build media config envelope: "+err.Error())
		return
	}
	envBytes, mErr := json.MarshalIndent(env, "", "  ")
	if mErr != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to encode media config envelope: "+mErr.Error())
		return
	}
	c.Header("Content-Disposition", `attachment; filename="media-config.json"`)
	c.Data(http.StatusOK, "application/json; charset=utf-8", envBytes)
}

// buildMediaEnvelope reads the base YAML and every runtime-override JSON,
// folding them into one versioned envelope. Shared by the pure-JSON export
// (ExportMediaConfig) and the OSD-image bundle (ExportMediaBundle) so the two
// always agree on what "the media config" is. Pure read.
func (h *MediaHandlers) buildMediaEnvelope() (mediaConfigEnvelope, error) {
	data, err := os.ReadFile(h.configPath)
	if err != nil {
		return mediaConfigEnvelope{}, fmt.Errorf("read base config: %w", err)
	}
	var baseRaw map[string]interface{}
	if err := yaml.Unmarshal(data, &baseRaw); err != nil {
		return mediaConfigEnvelope{}, fmt.Errorf("parse base config: %w", err)
	}
	// cleanMaps normalizes any map[interface{}]interface{} from yaml.v3 into
	// JSON-friendly map[string]interface{}. Export deliberately does NOT inject
	// the RTSP url (unlike GetConfig) so the snapshot reflects the on-disk
	// truth, not the requester's address.
	var baseYAML map[string]interface{}
	if cleaned, ok := cleanMaps(baseRaw).(map[string]interface{}); ok {
		baseYAML = cleaned
	} else {
		baseYAML = baseRaw
	}

	payload := mediaConfigPayload{BaseYAML: baseYAML}
	for _, d := range payload.payloadDims() {
		if raw := readJSONRaw(d.path); len(raw) > 0 {
			payload.assignRaw(d.path, raw)
		}
	}

	env := mediaConfigEnvelope{
		Schema:     mediaConfigSchema,
		Version:    mediaConfigVersion,
		ExportedAt: time.Now().UTC().Format(time.RFC3339),
		Config:     payload,
	}
	if hn, err := os.Hostname(); err == nil && hn != "" {
		env.Device = map[string]string{"hostname": hn}
	}
	return env, nil
}

// validateMediaEnvelope checks schema/version/emptiness and base_yaml shape
// before any write. Shared by ImportMediaConfig and ImportMediaBundle so both
// reject bad input identically and at the same point. On success the base_yaml
// map is normalized in place (ints restored), ready to marshal.
func validateMediaEnvelope(env *mediaConfigEnvelope) error {
	if env.Schema != mediaConfigSchema {
		return fmt.Errorf("unsupported schema %q (expected %q)", env.Schema, mediaConfigSchema)
	}
	if env.Version != mediaConfigVersion {
		return fmt.Errorf("unsupported envelope version %d (expected %d)", env.Version, mediaConfigVersion)
	}
	if env.Config.isEmpty() {
		return fmt.Errorf("envelope carries no config to import")
	}
	if len(env.Config.BaseYAML) != 0 {
		// Normalize JSON-decoded floats (e.g. width 1920.0) back to ints before
		// validation, exactly as SetConfig does, to avoid the 4.032e+06 emit bug.
		if err := normalizeMediaConfigNumbers(env.Config.BaseYAML); err != nil {
			return fmt.Errorf("base_yaml: %w", err)
		}
		if err := validateMediaConfigEncoders(env.Config.BaseYAML); err != nil {
			return fmt.Errorf("base_yaml: %w", err)
		}
	}
	return nil
}

// ImportMediaConfig (POST /api/v1/media/config/import) applies a previously
// exported envelope: validate → snapshot current files → atomically write the
// YAML (via projectMediaConfig) and any present JSONs → restart camera-daemon
// + device-control so their boot replays pick everything up. The original
// files are preserved in a timestamped backup dir returned in the response for
// manual rollback.
func (h *MediaHandlers) ImportMediaConfig(c *gin.Context) {
	if !requireJSONContentType(c) {
		return
	}

	var env mediaConfigEnvelope
	if err := c.ShouldBindJSON(&env); err != nil {
		Resp(c).FailMsg(CodeInvalidJSON, "Invalid request body: "+err.Error())
		return
	}
	if verr := validateMediaEnvelope(&env); verr != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", verr.Error())
		return
	}

	actor := getUsernameFromContext(c)
	ctx, cancel := context.WithTimeout(c.Request.Context(), 30*time.Second)
	defer cancel()
	applied, backupDir, err := h.applyImportedMediaConfig(ctx, actor, env)
	if err != nil {
		Resp(c).FailTyped(CodeCameraError, "import", err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.imported",
			eventLoggerPkg.MessageParams{"backup_dir": backupDir}, actor)
	}

	Resp(c).OK(gin.H{
		"applied":    true,
		"backup_dir": backupDir,
		"restart":    "camera-daemon,device-control",
		"items":      applied,
	})
}

// applyImportedMediaConfig is the write path shared by ImportMediaConfig and
// ImportMediaBundle. It snapshots the current base YAML + seven JSONs for
// rollback, writes the envelope (base_yaml via projectMediaConfigLocked so the
// config Controller sees it; JSONs via atomic tmp+rename), then restarts
// camera-daemon + device-control whose boot replays are the apply engine.
// Caller validates the envelope first. Holds configMu + the shared configApplyMu
// (so it cannot overlap a device-clone restore that rewrites the same etc tree).
// The applied map is returned non-empty even when the restart step fails (files
// are already on disk and recoverable from backupDir); err is then non-nil too.
func (h *MediaHandlers) applyImportedMediaConfig(ctx context.Context, actor string, env mediaConfigEnvelope) (gin.H, string, error) {
	// Hold the same lock SetConfig uses so a concurrent web edit can't interleave.
	h.configMu.Lock()
	defer h.configMu.Unlock()
	// Also take the shared apply lock so this multi-file write is serialized
	// against the device-clone file apply (applyTree) — both rewrite the same
	// /data/aipc/etc tree. Lock order configMu → configApplyMu matches the
	// per-field media edits. We call projectMediaConfigLocked (not the locking
	// wrapper) for base_yaml because we already hold this lock.
	configApplyMu.Lock()
	defer configApplyMu.Unlock()

	backupDir := filepath.Join(mediaBackupRoot,
		"media-config-"+time.Now().UTC().Format("20060102-150405"))
	if err := h.snapshotMediaConfig(backupDir); err != nil {
		return nil, backupDir, fmt.Errorf("snapshot current config: %w", err)
	}

	applied := gin.H{}
	// Partial-failure note: if a later step fails after some files are written,
	// the services have NOT been restarted, so the live state is unchanged; the
	// on-disk edits are recoverable from backupDir (returned in the response).
	if len(env.Config.BaseYAML) != 0 {
		out, err := marshalMediaConfig(env.Config.BaseYAML) // re-normalizes (idempotent) + marshals
		if err != nil {
			return nil, backupDir, fmt.Errorf("encode base_yaml: %w", err)
		}
		if err := h.projectMediaConfigLocked(ctx, actor, string(out)); err != nil {
			return nil, backupDir, fmt.Errorf("write base_yaml (backup at %s): %w", backupDir, err)
		}
		applied["base_yaml"] = true
	}

	for _, d := range env.Config.payloadDims() {
		if len(d.raw) == 0 {
			continue
		}
		if err := atomicWriteJSON(d.path, d.raw); err != nil {
			return nil, backupDir, fmt.Errorf("write %s (backup at %s): %w", d.name, backupDir, err)
		}
		applied[d.name] = true
	}

	// Apply = restart camera-daemon (boot replay reloads the six media JSONs +
	// base YAML) and device-control (replays lens_config.json → iris_target).
	if err := exec.CommandContext(ctx, "systemctl", "restart", "camera-daemon", "device-control").Run(); err != nil {
		return applied, backupDir, fmt.Errorf("service restart failed (files written, backup at %s): %w", backupDir, err)
	}
	return applied, backupDir, nil
}
