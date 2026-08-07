package handlers

// Unified media-config import/export (Option B aggregation layer).
//
// The device media config is fragmented across one base YAML
// (camera-daemon.yaml) and six runtime-override JSON files under
// /data/aipc/etc. No single component holds the full picture, which is the
// real obstacle for "clone this device's config onto another device".
//
// Export aggregates all seven sources into one versioned envelope (pure read,
// zero side effects). Import validates the envelope, snapshots the current
// files for rollback, atomically writes them back, then restarts camera-daemon
// whose boot replay (camera_daemon.cpp load_* sequence) is the apply engine.
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

// mediaConfigPayload holds the seven config sources. The six JSON dimensions
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
}

// mediaConfigDim ties a payload field to its on-disk path for the import loop.
type mediaConfigDim struct {
	name string
	path string
	raw  json.RawMessage
}

// payloadDims returns the six JSON dimensions present in the payload.
func (p mediaConfigPayload) payloadDims() []mediaConfigDim {
	return []mediaConfigDim{
		{"osd", osdConfigPath, p.Osd},
		{"privacy_mask", privacyMaskCfgPath, p.PrivacyMask},
		{"transform", transformCfgPath, p.Transform},
		{"isp", ispCfgPath, p.Isp},
		{"profile", profileCfgPath, p.Profile},
		{"scalar_fields", scalarFieldsCfgPath, p.ScalarFields},
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
		ispCfgPath, profileCfgPath, scalarFieldsCfgPath)
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

// ExportMediaConfig (GET /api/v1/media/config/export) returns a versioned
// envelope aggregating the base YAML plus all six runtime-override JSONs.
// Pure read: no gRPC, no writes, no restart.
func (h *MediaHandlers) ExportMediaConfig(c *gin.Context) {
	data, err := os.ReadFile(h.configPath)
	if err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to read base config: "+err.Error())
		return
	}
	var baseRaw map[string]interface{}
	if err := yaml.Unmarshal(data, &baseRaw); err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to parse base config: "+err.Error())
		return
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

	Resp(c).OK(env)
}

// ImportMediaConfig (POST /api/v1/media/config/import) applies a previously
// exported envelope: validate → snapshot current files → atomically write the
// YAML (via projectMediaConfig) and any present JSONs → restart camera-daemon
// so its boot replay picks everything up. The original files are preserved in
// a timestamped backup dir returned in the response for manual rollback.
func (h *MediaHandlers) ImportMediaConfig(c *gin.Context) {
	if !requireJSONContentType(c) {
		return
	}

	var env mediaConfigEnvelope
	if err := c.ShouldBindJSON(&env); err != nil {
		Resp(c).FailMsg(CodeInvalidJSON, "Invalid request body: "+err.Error())
		return
	}
	if env.Schema != mediaConfigSchema {
		Resp(c).FailTyped(CodeInvalidParameter, "validation",
			fmt.Sprintf("unsupported schema %q (expected %q)", env.Schema, mediaConfigSchema))
		return
	}
	if env.Version != mediaConfigVersion {
		Resp(c).FailTyped(CodeInvalidParameter, "validation",
			fmt.Sprintf("unsupported envelope version %d (expected %d)", env.Version, mediaConfigVersion))
		return
	}
	if env.Config.isEmpty() {
		Resp(c).FailMsg(CodeInvalidParameter, "envelope carries no config to import")
		return
	}

	// Hold the same lock SetConfig uses so a concurrent web edit can't interleave.
	h.configMu.Lock()
	defer h.configMu.Unlock()

	actor := getUsernameFromContext(c)
	backupDir := filepath.Join(mediaBackupRoot,
		"media-config-"+time.Now().UTC().Format("20060102-150405"))
	if err := h.snapshotMediaConfig(backupDir); err != nil {
		Resp(c).FailMsg(CodeCameraError, "Failed to snapshot current config: "+err.Error())
		return
	}

	applied := gin.H{}
	// Partial-failure note: if a later step fails after some files are written,
	// camera-daemon has NOT been restarted, so the live state is unchanged;
	// the on-disk edits are recoverable from backupDir (returned in errors and
	// in the success response).
	if len(env.Config.BaseYAML) != 0 {
		cfg := env.Config.BaseYAML
		// Normalize JSON-decoded floats (e.g. width 1920.0) back to ints before
		// validation, exactly as SetConfig does, to avoid the 4.032e+06 emit bug.
		if err := normalizeMediaConfigNumbers(cfg); err != nil {
			Resp(c).FailTyped(CodeInvalidParameter, "validation", "base_yaml: "+err.Error())
			return
		}
		if err := validateMediaConfigEncoders(cfg); err != nil {
			Resp(c).FailTyped(CodeInvalidParameter, "validation", "base_yaml: "+err.Error())
			return
		}
		out, err := marshalMediaConfig(cfg) // re-normalizes (idempotent) + marshals
		if err != nil {
			Resp(c).FailMsg(CodeCameraError, "Failed to encode base_yaml: "+err.Error())
			return
		}
		if err := h.projectMediaConfig(c.Request.Context(), actor, string(out)); err != nil {
			Resp(c).FailMsg(CodeCameraError, "Failed to write base_yaml (backup at "+backupDir+"): "+err.Error())
			return
		}
		applied["base_yaml"] = true
	}

	for _, d := range env.Config.payloadDims() {
		if len(d.raw) == 0 {
			continue
		}
		if err := atomicWriteJSON(d.path, d.raw); err != nil {
			Resp(c).FailMsg(CodeCameraError,
				fmt.Sprintf("Failed to write %s (backup at %s): %s", d.name, backupDir, err.Error()))
			return
		}
		applied[d.name] = true
	}

	// Apply = restart camera-daemon so its boot replay reloads all six JSONs +
	// the base YAML. platform-api itself is unaffected; its gRPC connection
	// reconnects on its own. Blocking (~few seconds); a generous timeout keeps
	// a slow startup from surfacing as a client disconnect.
	ctx, cancel := context.WithTimeout(c.Request.Context(), 30*time.Second)
	defer cancel()
	if err := exec.CommandContext(ctx, "systemctl", "restart", "camera-daemon").Run(); err != nil {
		// Files are written and backed up; only the restart failed. Report it
		// distinctly so the operator can restart manually rather than re-import.
		Resp(c).FailTyped(CodeCameraError, "restart",
			"files written (backup at "+backupDir+") but camera-daemon restart failed: "+err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("media.config.imported",
			eventLoggerPkg.MessageParams{"backup_dir": backupDir}, actor)
	}

	Resp(c).OK(gin.H{
		"applied":    true,
		"backup_dir": backupDir,
		"restart":    "camera-daemon",
		"items":      applied,
	})
}
