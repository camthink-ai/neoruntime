package handlers

// Device-scope config clone: export an entire device's configuration (the /data/
// aipc/etc tree + the config Controller's desired-state DB) as one tar.gz, and
// import it onto another same-model device. This is the device-scoped variant
// of the media bundle (media_config_bundle.go): same transport (self-describing
// tar.gz + sha256 manifest via config_io.go), broader scope.
//
// Scope (per locked decision): config + state DB ONLY. Apps/models/containerd
// stay on the target. Identity policy: regenerate secrets, keep target identity
// — the source's platform-api.yaml (token_key/password), device.conf (name),
// ssl/ (TLS), rsa/ (keypair) are NEVER packed, and the auth + device_info rows
// in the desired-state DB are excluded so the next-boot reconcile does not
// re-project the source's secrets/name onto the target.
//
// Why the DB rides along: the Config Controller reconciles /data/aipc/etc/*.yaml
// from the config_items desired-state table on every boot. Cloning the YAMLs
// without the DB means the first boot overwrites them with the target's old
// desired state. Cloning the DB wholesale would also drag over the app/model
// registries (AppInstall, AIModel, …) which point at apps/models the target does
// not have — so only the four config tables are cloned, and only their
// non-identity rows.

import (
	"archive/tar"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"time"

	"github.com/gin-gonic/gin"
	"gorm.io/gorm"

	"aipc/platform/common/constants"
	eventLoggerPkg "aipc/platform/common/events"
	"aipc/platform/platform-api/internal/atomicfile"
	"aipc/platform/platform-api/model"
)

const (
	deviceCloneSchema  = "aipc.device_clone"
	deviceCloneVersion = 1
)

// cloneEtcDir is the live config tree shipped in the bundle (under etc/). It is
// /data/aipc/etc. A var (not const) because it derives from constants.ConfigPath().
var (
	cloneEtcDir   = constants.ConfigPath()
	cloneBackupRt = constants.RootPath() + "/backups/clone-restore"
)

// cloneIdentityDomains are config-item domains that carry device identity or
// secrets (auth = token_key + password; device_info = device name). Rows in
// these domains are EXCLUDED from the bundle's state payload; on import the
// target's own rows in these domains are preserved. Without this, the next-boot
// reconcile would re-project the SOURCE secrets/name into the target's files
// from these very rows — defeating the file-level identity skip.
var cloneIdentityDomains = map[string]bool{
	"auth":        true,
	"device_info": true,
}

// cloneIdentityDomainsList returns the identity domains as a slice for SQL IN.
func cloneIdentityDomainsList() []string {
	return []string{"auth", "device_info"}
}

// cloneIdentitySkipPaths are etc-tree relative paths NOT applied from the clone.
// Trailing-slash entries skip whole subtrees. They are also NOT packed at export
// (defense-in-depth: the bundle never carries source secrets even in transit).
var cloneIdentitySkipPaths = map[string]bool{
	"platform-api.yaml": true, // auth.token_key + password + listen config: target keeps its own
	"device.conf":       true, // device name
	"ssl/":              true, // TLS cert/key: target self-signs on boot
	"rsa/":              true, // RSA keypair: target's own (password crypto)
}

// cloneIdentitySkipList returns the skipped identity paths for response reporting.
func cloneIdentitySkipList() []string {
	return []string{"platform-api.yaml", "device.conf", "ssl/", "rsa/"}
}

// cloneStatePayload is the JSON carried at state/tables.json. The four config
// tables round-trip as their gorm model types (clean json tags). Identity-domain
// rows are absent before marshal (filtered at read).
type cloneStatePayload struct {
	ConfigItems     []model.ConfigItem     `json:"config_items"`
	ConfigRevisions []model.ConfigRevision `json:"config_revisions"`
	ConfigApplyJobs []model.ConfigApplyJob `json:"config_apply_jobs"`
	Settings        []model.Setting        `json:"settings"`
}

func (s cloneStatePayload) counts() map[string]int {
	return map[string]int{
		"config_items":      len(s.ConfigItems),
		"config_revisions":  len(s.ConfigRevisions),
		"config_apply_jobs": len(s.ConfigApplyJobs),
		"settings":          len(s.Settings),
	}
}

// ExportDeviceConfig (GET /api/v1/system/clone/export) streams a clone tar.gz:
// state/tables.json (4 config DB tables, identity rows excluded) + the etc tree
// (identity files/dirs excluded) + manifest.json. Pure read on live state;
// writes only a temp file under mediaBundleStaging.
func (h *APIHandlers) ExportDeviceConfig(c *gin.Context) {
	if err := os.MkdirAll(mediaBundleStaging, 0755); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "staging dir: "+err.Error())
		return
	}

	state, err := h.exportCloneState()
	if err != nil {
		Resp(c).FailMsg(CodeDatabaseError, "read state: "+err.Error())
		return
	}

	devName := cloneDeviceName()
	tempFile := filepath.Join(mediaBundleStaging,
		fmt.Sprintf("clone_%s_%s.tar.gz", devName, time.Now().UTC().Format("20060102-150405")))
	manifest := &configManifest{
		Schema:    deviceCloneSchema,
		Version:   deviceCloneVersion,
		CreatedAt: time.Now().UTC().Format(time.RFC3339),
		Source:    map[string]string{"hostname": devName},
		Extra:     map[string]any{"tables": state.counts(), "identity_excluded": cloneIdentitySkipList()},
	}
	if err := writeCloneBundle(tempFile, state, manifest); err != nil {
		_ = os.Remove(tempFile)
		Resp(c).FailMsg(CodeOperationFailed, "build bundle: "+err.Error())
		return
	}

	c.Header("Content-Disposition", `attachment; filename="`+filepath.Base(tempFile)+`"`)
	c.Header("Content-Type", "application/gzip")
	c.File(tempFile)
	go os.Remove(tempFile)
}

// cloneDeviceName returns a filename-safe device label for the bundle name.
func cloneDeviceName() string {
	if hn, err := os.Hostname(); err == nil && hn != "" {
		return hn
	}
	return "device"
}

// writeCloneBundle assembles the clone tar.gz at path: state/tables.json, then
// the etc tree (identity paths skipped so source secrets never enter the bundle),
// then manifest.json. manifest.Files is filled as a side effect. On error the
// partial file is removed.
func writeCloneBundle(path string, state cloneStatePayload, manifest *configManifest) (retErr error) {
	f, err := os.Create(path)
	if err != nil {
		return fmt.Errorf("create bundle: %w", err)
	}
	defer func() {
		f.Close()
		if retErr != nil {
			_ = os.Remove(path)
		}
	}()

	return writeTarGz(f, func(tw *tar.Writer) error {
		// state/tables.json first.
		stateBytes, err := json.MarshalIndent(state, "", "  ")
		if err != nil {
			return fmt.Errorf("marshal state: %w", err)
		}
		if err := tarWriteBytes(tw, "state/tables.json", stateBytes, 0644); err != nil {
			return err
		}
		manifest.Files = append(manifest.Files, manifestEntry{
			Path: "state/tables.json", SHA256: sha256Sum(stateBytes), Size: int64(len(stateBytes)), Mode: 0644,
		})

		// etc tree, skipping identity files/subtrees so the bundle never carries
		// source secrets (token_key/password/certs/device name).
		rels, err := walkFiles(cloneEtcDir)
		if err != nil {
			return fmt.Errorf("walk etc: %w", err)
		}
		for _, rel := range rels {
			if pathSkipped(rel, cloneIdentitySkipPaths) {
				continue
			}
			me, err := tarWriteFile(tw, filepath.Join(cloneEtcDir, rel), "etc/"+rel)
			if err != nil {
				return err
			}
			manifest.Files = append(manifest.Files, me)
		}

		mb, err := json.MarshalIndent(manifest, "", "  ")
		if err != nil {
			return fmt.Errorf("marshal manifest: %w", err)
		}
		return tarWriteBytes(tw, "manifest.json", mb, 0644)
	})
}

// exportCloneState reads the four config tables, filtering identity-domain rows
// out of the three config_* tables. Returns an empty payload when no DB is
// attached (config Controller disabled) — the clone then carries files only.
func (h *APIHandlers) exportCloneState() (cloneStatePayload, error) {
	var st cloneStatePayload
	if h.db == nil {
		return st, nil
	}
	idDomains := cloneIdentityDomainsList()
	if err := h.db.Where("domain NOT IN ?", idDomains).Find(&st.ConfigItems).Error; err != nil {
		return st, fmt.Errorf("config_items: %w", err)
	}
	if err := h.db.Where("domain NOT IN ?", idDomains).Find(&st.ConfigRevisions).Error; err != nil {
		return st, fmt.Errorf("config_revisions: %w", err)
	}
	if err := h.db.Where("domain NOT IN ?", idDomains).Find(&st.ConfigApplyJobs).Error; err != nil {
		return st, fmt.Errorf("config_apply_jobs: %w", err)
	}
	if err := h.db.Find(&st.Settings).Error; err != nil {
		return st, fmt.Errorf("settings: %w", err)
	}
	return st, nil
}

// ImportDeviceConfig (POST /api/v1/system/clone/import) applies a clone bundle from
// ExportDeviceConfig: receive tar.gz → extract → verify manifest sha256 →
// snapshot current etc + state for rollback → apply etc tree (identity skipped)
// → transactionally replace the 4 config tables (target identity rows preserved)
// → restart the config-consuming services → self-restart platform-api after the
// response ships so its in-memory config + DB view reload.
func (h *APIHandlers) ImportDeviceConfig(c *gin.Context) {
	if err := c.Request.ParseMultipartForm(2 << 30); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Failed to parse multipart upload: "+err.Error())
		return
	}
	file, header, err := c.Request.FormFile("file")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Missing 'file' in upload: "+err.Error())
		return
	}
	defer file.Close()
	if !validBundleSuffix(header.Filename) {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", "file must be .tar.gz or .tgz")
		return
	}

	if err := os.MkdirAll(mediaBundleStaging, 0755); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "staging dir: "+err.Error())
		return
	}
	extractDir := filepath.Join(mediaBundleStaging, "clone-"+time.Now().UTC().Format("20060102-150405"))
	defer os.RemoveAll(extractDir)

	manifestJSON, members, err := untar(file, extractDir)
	if err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "extract bundle: "+err.Error())
		return
	}
	if len(manifestJSON) == 0 {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", "bundle has no manifest.json")
		return
	}
	manifest, err := decodeManifest(manifestJSON)
	if err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "validation", err.Error())
		return
	}
	if manifest.Schema != deviceCloneSchema {
		Resp(c).FailTyped(CodeInvalidParameter, "validation",
			fmt.Sprintf("unsupported schema %q (expected %q)", manifest.Schema, deviceCloneSchema))
		return
	}
	if manifest.Version != deviceCloneVersion {
		Resp(c).FailTyped(CodeInvalidParameter, "validation",
			fmt.Sprintf("unsupported version %d (expected %d)", manifest.Version, deviceCloneVersion))
		return
	}
	if _, err := verifyManifest(manifest, members); err != nil {
		Resp(c).FailTyped(CodeInvalidParameter, "integrity", err.Error())
		return
	}

	// Snapshot current etc + state for manual rollback (best-effort).
	ts := time.Now().UTC().Format("20060102-150405")
	snapDir := filepath.Join(cloneBackupRt, "clone-restore-"+ts)
	_ = snapshotTree(cloneEtcDir, filepath.Join(snapDir, "etc"))
	if cur, e := h.exportCloneState(); e == nil {
		if b, mErr := json.MarshalIndent(cur, "", "  "); mErr == nil {
			_ = atomicfile.Write(filepath.Join(snapDir, "tables.json"), b, 0644)
		}
	}

	// Apply the etc tree. Identity paths are skipped here even though a
	// self-produced bundle never packs them — defense against foreign bundles.
	applied, err := applyTree(members, "etc", cloneEtcDir, cloneIdentitySkipPaths)
	if err != nil {
		Resp(c).FailTyped(CodeOperationFailed, "apply", "write config files (snapshot at "+snapDir+"): "+err.Error())
		return
	}

	// Apply the desired-state DB (4 tables, identity rows preserved on target).
	tablesApplied := map[string]int{}
	if statePath, ok := members["state/tables.json"]; ok {
		stateBytes, err := os.ReadFile(statePath)
		if err != nil {
			Resp(c).FailMsg(CodeOperationFailed, "read state/tables.json: "+err.Error())
			return
		}
		var state cloneStatePayload
		if err := json.Unmarshal(stateBytes, &state); err != nil {
			Resp(c).FailTyped(CodeInvalidParameter, "validation", "state/tables.json: "+err.Error())
			return
		}
		if err := h.importCloneState(state); err != nil {
			Resp(c).FailTyped(CodeDatabaseError, "import", "replace tables (snapshot at "+snapDir+"): "+err.Error())
			return
		}
		tablesApplied = state.counts()
	}

	// Restart config-consuming services so their boot replays reload the cloned
	// files + DB. camera-daemon (media/osd/privacy/transform/isp/profile/scalar),
	// device-control (lens), ai-runtime (ai-runtime.yaml), app-manager (its yaml).
	// Failures are tolerated per-service: a cloned config that one service rejects
	// must not block the rest (and the snapshot supports manual rollback).
	restarted := restartCloneServices()
	actor := getUsernameFromContext(c)
	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync("device.clone.imported",
			eventLoggerPkg.MessageParams{"snapshot_dir": snapDir, "restarted": restarted}, actor)
	}

	// platform-api is THIS process: restart it detached AFTER the response ships
	// so the client gets a clean reply and the service reloads its config + DB.
	go func() {
		time.Sleep(2 * time.Second)
		_ = exec.Command("systemctl", "restart", "platform-api").Run()
	}()

	Resp(c).OK(gin.H{
		"applied_files":    applied,
		"skipped_identity": cloneIdentitySkipList(),
		"tables":           tablesApplied,
		"identity_domains": cloneIdentityDomainsList(),
		"snapshot_dir":     snapDir,
		"restarted":        restarted,
		"self_restart":     "platform-api",
		"message":          "clone applied; platform-api will self-restart shortly to reload config",
	})
}

// importCloneState transactionally replaces the four config tables. For the
// three config_* tables it deletes only non-identity domains (the target's auth
// + device_info rows survive); settings is fully replaced (generic KV). The
// whole replace is atomic — any error rolls back and leaves the target unchanged.
//
// Payload identity rows are filtered before insert too: a hand-crafted or
// foreign bundle may carry auth/device_info rows that export would have
// dropped. Without this filter the insert would collide with the target rows
// the delete step just preserved (UNIQUE(domain,key)). Export filtering is the
// primary defense; this is the import-side mirror.
func (h *APIHandlers) importCloneState(st cloneStatePayload) error {
	if h.db == nil {
		return nil
	}
	idDomains := cloneIdentityDomainsList()
	return h.db.Transaction(func(tx *gorm.DB) error {
		// config_items: preserve target identity rows, replace the rest.
		if err := tx.Where("domain NOT IN ?", idDomains).Delete(&model.ConfigItem{}).Error; err != nil {
			return fmt.Errorf("purge config_items: %w", err)
		}
		if items := cloneDropIdentityItems(st.ConfigItems); len(items) > 0 {
			if err := tx.Create(&items).Error; err != nil {
				return fmt.Errorf("insert config_items: %w", err)
			}
		}
		// config_revisions: history, same identity split.
		if err := tx.Where("domain NOT IN ?", idDomains).Delete(&model.ConfigRevision{}).Error; err != nil {
			return fmt.Errorf("purge config_revisions: %w", err)
		}
		if revs := cloneDropIdentityRevisions(st.ConfigRevisions); len(revs) > 0 {
			if err := tx.Create(&revs).Error; err != nil {
				return fmt.Errorf("insert config_revisions: %w", err)
			}
		}
		// config_apply_jobs: audit log, same identity split.
		if err := tx.Where("domain NOT IN ?", idDomains).Delete(&model.ConfigApplyJob{}).Error; err != nil {
			return fmt.Errorf("purge config_apply_jobs: %w", err)
		}
		if jobs := cloneDropIdentityJobs(st.ConfigApplyJobs); len(jobs) > 0 {
			if err := tx.Create(&jobs).Error; err != nil {
				return fmt.Errorf("insert config_apply_jobs: %w", err)
			}
		}
		// settings: full replace (generic KV, no identity split).
		if err := tx.Where("1 = 1").Delete(&model.Setting{}).Error; err != nil {
			return fmt.Errorf("purge settings: %w", err)
		}
		if len(st.Settings) > 0 {
			if err := tx.Create(&st.Settings).Error; err != nil {
				return fmt.Errorf("insert settings: %w", err)
			}
		}
		return nil
	})
}

// cloneDropIdentityItems removes auth/device_info rows from a config_items slice.
// (config_items and config_revisions/config_apply_jobs share a Domain field but
// are distinct gorm types, so each gets its own filter rather than a generic.)
func cloneDropIdentityItems(in []model.ConfigItem) []model.ConfigItem {
	out := make([]model.ConfigItem, 0, len(in))
	for _, r := range in {
		if !cloneIdentityDomains[r.Domain] {
			out = append(out, r)
		}
	}
	return out
}

func cloneDropIdentityRevisions(in []model.ConfigRevision) []model.ConfigRevision {
	out := make([]model.ConfigRevision, 0, len(in))
	for _, r := range in {
		if !cloneIdentityDomains[r.Domain] {
			out = append(out, r)
		}
	}
	return out
}

func cloneDropIdentityJobs(in []model.ConfigApplyJob) []model.ConfigApplyJob {
	out := make([]model.ConfigApplyJob, 0, len(in))
	for _, r := range in {
		if !cloneIdentityDomains[r.Domain] {
			out = append(out, r)
		}
	}
	return out
}

// restartCloneServices restarts the config-consuming services, returning the
// units that restarted successfully. Sequential (not parallel) to avoid spikes
// and so a slow unit does not mask others' failures. Each unit is independent —
// one failing restart does not abort the rest.
func restartCloneServices() []string {
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	var restarted []string
	for _, unit := range []string{"camera-daemon", "device-control", "ai-runtime", "app-manager"} {
		if err := exec.CommandContext(ctx, "systemctl", "restart", unit).Run(); err == nil {
			restarted = append(restarted, unit)
		}
	}
	return restarted
}
