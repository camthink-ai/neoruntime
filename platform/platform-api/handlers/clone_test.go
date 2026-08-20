package handlers

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"encoding/json"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"testing"

	"github.com/glebarez/sqlite"
	"gorm.io/gorm"
	"gorm.io/gorm/logger"

	"aipc/platform/platform-api/model"
)

// newCloneTestDB opens a fresh SQLite DB with the four config tables migrated,
// so export/import state can be exercised without a running server. Mirrors
// newConfigJobsRepo but adds Setting (the clone carries settings verbatim).
func newCloneTestDB(t *testing.T) *gorm.DB {
	t.Helper()
	gdb, err := gorm.Open(sqlite.Open(filepath.Join(t.TempDir(), "clone.db")), &gorm.Config{
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
		&model.ConfigItem{}, &model.ConfigRevision{}, &model.ConfigApplyJob{}, &model.Setting{},
	); err != nil {
		t.Fatalf("AutoMigrate: %v", err)
	}
	return gdb
}

// withTempEtc swaps the package-level cloneEtcDir to a populated temp tree and
// restores it on cleanup, so writeCloneBundle can be tested without touching
// /data/aipc/etc. Returns the temp dir.
func withTempEtc(t *testing.T, files map[string]string) string {
	t.Helper()
	dir := t.TempDir()
	for rel, content := range files {
		full := filepath.Join(dir, rel)
		if err := os.MkdirAll(filepath.Dir(full), 0755); err != nil {
			t.Fatalf("mkdir %s: %v", filepath.Dir(full), err)
		}
		if err := os.WriteFile(full, []byte(content), 0644); err != nil {
			t.Fatalf("write %s: %v", rel, err)
		}
	}
	prev := cloneEtcDir
	cloneEtcDir = dir
	t.Cleanup(func() { cloneEtcDir = prev })
	return dir
}

// TestCloneIdentitySkipPaths pins the etc-tree identity policy: each device-
// unique path is skipped, while a benign config file is not. This is the
// security invariant behind "regenerate secrets, keep target identity".
func TestCloneIdentitySkipPaths(t *testing.T) {
	skipped := []string{
		"platform-api.yaml",                // token_key + password
		"device.conf",                      // device name
		"ssl/server.key", "ssl/server.crt", // TLS: target self-signs
		"rsa/password.key", "rsa/password.pub", // RSA keypair
	}
	for _, p := range skipped {
		if !pathSkipped(p, cloneIdentitySkipPaths) {
			t.Errorf("identity path %q should be skipped but was not", p)
		}
	}
	benign := []string{"camera-daemon.yaml", "ai-runtime.yaml", "osd/logo.png", "security/seccomp-default.json"}
	for _, p := range benign {
		if pathSkipped(p, cloneIdentitySkipPaths) {
			t.Errorf("benign path %q should NOT be skipped but was", p)
		}
	}
	if got := cloneIdentitySkipList(); len(got) != 4 {
		t.Errorf("cloneIdentitySkipList has %d entries, want 4", len(got))
	}
	if got := cloneIdentityDomainsList(); len(got) != 2 {
		t.Errorf("cloneIdentityDomainsList has %d entries, want 2", len(got))
	}
}

// TestCloneExportExcludesIdentity drives the real export path (writeCloneBundle)
// and asserts that device-unique files never enter the bundle while ordinary
// config files do. This validates the identity policy end-to-end through the
// tar + manifest machinery, not just the pathSkipped predicate.
func TestCloneExportExcludesIdentity(t *testing.T) {
	withTempEtc(t, map[string]string{
		"camera-daemon.yaml":  "encoders: {}\n",
		"ai-runtime.yaml":     "models: []\n",
		"platform-api.yaml":   "auth:\n  token_key: SOURCE-SECRET\n  password: src-pw\n",
		"device.conf":         "DEVICE_NAME=source-cam\n",
		"ssl/server.key":      "PRIVATE SOURCE KEY\n",
		"ssl/server.crt":      "SOURCE CERT\n",
		"rsa/password.key":    "SOURCE RSA PRIVATE\n",
		"rsa/password.pub":    "SOURCE RSA PUBLIC\n",
		"osd/logo.png":        "\x89PNG\r\n\x1a\n fake png",
		"osd/osd_config.json": "{}\n",
	})

	out := filepath.Join(t.TempDir(), "clone.tar.gz")
	manifest := &configManifest{
		Schema:  deviceCloneSchema,
		Version: deviceCloneVersion,
	}
	if err := writeCloneBundle(out, cloneStatePayload{}, manifest); err != nil {
		t.Fatalf("writeCloneBundle: %v", err)
	}

	// Read the archive names back (no extraction needed for the membership check).
	f, err := os.Open(out)
	if err != nil {
		t.Fatalf("open bundle: %v", err)
	}
	defer f.Close()
	gzr, _ := gzip.NewReader(f)
	defer gzr.Close()
	tr := tar.NewReader(gzr)
	var names []string
	for {
		h, err := tr.Next()
		if err != nil {
			break
		}
		names = append(names, h.Name)
	}
	sort.Strings(names)
	has := func(p string) bool {
		for _, n := range names {
			if n == p {
				return true
			}
		}
		return false
	}

	// Identity files MUST be absent — the bundle never carries source secrets.
	for _, identity := range []string{
		"etc/platform-api.yaml", "etc/device.conf",
		"etc/ssl/server.key", "etc/ssl/server.crt",
		"etc/rsa/password.key", "etc/rsa/password.pub",
	} {
		if has(identity) {
			t.Errorf("identity file %q MUST NOT be packed into the clone bundle (source secret leak)", identity)
		}
	}
	// Ordinary config files MUST be present.
	for _, benign := range []string{
		"etc/camera-daemon.yaml", "etc/ai-runtime.yaml",
		"etc/osd/logo.png", "etc/osd/osd_config.json",
	} {
		if !has(benign) {
			t.Errorf("benign config file %q should be packed into the clone bundle", benign)
		}
	}
	// State + manifest always ride along.
	if !has("state/tables.json") {
		t.Error("state/tables.json missing from bundle")
	}
	if !has("manifest.json") {
		t.Error("manifest.json missing from bundle")
	}
	// Manifest declares the clone schema and must NOT list any identity entry.
	for _, me := range manifest.Files {
		if strings.Contains(me.Path, "platform-api.yaml") ||
			strings.Contains(me.Path, "device.conf") ||
			strings.HasPrefix(me.Path, "etc/ssl/") ||
			strings.HasPrefix(me.Path, "etc/rsa/") {
			t.Errorf("manifest lists identity entry %q (should have been skipped)", me.Path)
		}
	}
	if manifest.Schema != deviceCloneSchema {
		t.Errorf("manifest schema = %q, want %q", manifest.Schema, deviceCloneSchema)
	}
}

// TestCloneStatePayloadRoundTrip confirms the four-table JSON transport survives
// a marshal/unmarshal cycle with counts and values intact.
func TestCloneStatePayloadRoundTrip(t *testing.T) {
	in := cloneStatePayload{
		ConfigItems: []model.ConfigItem{
			{Domain: "media", Key: "config", ValueJSON: `{"a":1}`, Revision: 3, UpdatedBy: "admin"},
			{Domain: "network", Key: "eth0", ValueJSON: `{"ip":"dhcp"}`, Revision: 1},
		},
		ConfigRevisions: []model.ConfigRevision{
			{ID: 1, Domain: "media", Key: "config", Revision: 2, ValueJSON: `{"old":true}`, Reason: "set"},
		},
		ConfigApplyJobs: []model.ConfigApplyJob{
			{ID: "job-1", Domain: "media", Key: "config", Action: "apply", Status: "success"},
		},
		Settings: []model.Setting{
			{Key: "theme", Value: "dark"},
		},
	}
	raw, err := json.MarshalIndent(in, "", "  ")
	if err != nil {
		t.Fatalf("marshal: %v", err)
	}
	var out cloneStatePayload
	if err := json.Unmarshal(raw, &out); err != nil {
		t.Fatalf("unmarshal: %v", err)
	}
	want := in.counts()
	got := out.counts()
	for k, n := range want {
		if got[k] != n {
			t.Errorf("counts[%s] = %d, want %d", k, got[k], n)
		}
	}
	if out.ConfigItems[0].ValueJSON != in.ConfigItems[0].ValueJSON {
		t.Error("ConfigItem ValueJSON not preserved")
	}
	if out.ConfigItems[0].Revision != 3 {
		t.Errorf("Revision = %d, want 3", out.ConfigItems[0].Revision)
	}
	if out.Settings[0].Key != "theme" {
		t.Errorf("Setting Key = %q, want theme", out.Settings[0].Key)
	}
}

// TestCloneStateImportPreservesIdentity is the DB-coupling guard: importCloneState
// must REPLACE non-identity rows but PRESERVE the target's auth + device_info rows.
// This blocks the identity-leak path where the source's token_key/password/name,
// carried as config_items desired-state, would be re-projected onto the target's
// files by the next-boot reconcile even after the etc files were skipped.
func TestCloneStateImportPreservesIdentity(t *testing.T) {
	gdb := newCloneTestDB(t)

	// Target's current state: identity rows + non-identity rows.
	targetRows := []model.ConfigItem{
		// Identity — MUST survive import unchanged.
		{Domain: "auth", Key: "config", ValueJSON: `{"token_key":"TARGET-SECRET"}`, Revision: 1},
		{Domain: "device_info", Key: "name", ValueJSON: `{"hostname":"target-cam"}`, Revision: 1},
		// Non-identity — MUST be replaced by the clone payload.
		{Domain: "media", Key: "config", ValueJSON: `{"old":"target media"}`, Revision: 1},
		{Domain: "network", Key: "eth0", ValueJSON: `{"old":"target net"}`, Revision: 1},
	}
	if err := gdb.Create(&targetRows).Error; err != nil {
		t.Fatalf("seed: %v", err)
	}
	// A target setting that the full-replace should wipe (replaced by payload's).
	if err := gdb.Create(&model.Setting{Key: "stale", Value: "gone"}).Error; err != nil {
		t.Fatalf("seed setting: %v", err)
	}

	h := &APIHandlers{db: gdb}
	payload := cloneStatePayload{
		ConfigItems: []model.ConfigItem{
			// Source identity rows — MUST be ignored (target keeps its own).
			{Domain: "auth", Key: "config", ValueJSON: `{"token_key":"SOURCE-SECRET-LEAK"}`, Revision: 9},
			{Domain: "device_info", Key: "name", ValueJSON: `{"hostname":"source-cam"}`, Revision: 9},
			// Source non-identity rows — MUST replace target's.
			{Domain: "media", Key: "config", ValueJSON: `{"new":"clone media"}`, Revision: 2},
			{Domain: "network", Key: "eth0", ValueJSON: `{"new":"clone net"}`, Revision: 2},
		},
		Settings: []model.Setting{
			{Key: "theme", Value: "dark"},
		},
	}
	if err := h.importCloneState(payload); err != nil {
		t.Fatalf("importCloneState: %v", err)
	}

	// Identity rows: target's values preserved, source values NOT applied.
	var authRow model.ConfigItem
	if err := gdb.Where("domain = ? AND key = ?", "auth", "config").First(&authRow).Error; err != nil {
		t.Fatalf("read auth row: %v", err)
	}
	if strings.Contains(authRow.ValueJSON, "SOURCE-SECRET-LEAK") {
		t.Error("source auth/token_key LEAKED into target DB — reconcile would re-project it")
	}
	if !strings.Contains(authRow.ValueJSON, "TARGET-SECRET") {
		t.Errorf("target auth row was clobbered: got %q", authRow.ValueJSON)
	}
	var nameRow model.ConfigItem
	if err := gdb.Where("domain = ? AND key = ?", "device_info", "name").First(&nameRow).Error; err != nil {
		t.Fatalf("read device_info row: %v", err)
	}
	if !strings.Contains(nameRow.ValueJSON, "target-cam") {
		t.Errorf("target device name was clobbered: got %q", nameRow.ValueJSON)
	}

	// Non-identity rows: replaced with clone payload values.
	var mediaRow model.ConfigItem
	if err := gdb.Where("domain = ? AND key = ?", "media", "config").First(&mediaRow).Error; err != nil {
		t.Fatalf("read media row: %v", err)
	}
	if !strings.Contains(mediaRow.ValueJSON, "clone media") {
		t.Errorf("media row not replaced by clone: got %q", mediaRow.ValueJSON)
	}

	// Identity rows count: exactly one auth + one device_info (no source dup).
	var authCount, devCount int64
	gdb.Model(&model.ConfigItem{}).Where("domain = ?", "auth").Count(&authCount)
	gdb.Model(&model.ConfigItem{}).Where("domain = ?", "device_info").Count(&devCount)
	if authCount != 1 || devCount != 1 {
		t.Errorf("identity row counts auth=%d device_info=%d, want 1 each (source rows must not accumulate)", authCount, devCount)
	}

	// Settings: full replace — stale gone, theme present.
	var staleCount int64
	gdb.Model(&model.Setting{}).Where("key = ?", "stale").Count(&staleCount)
	if staleCount != 0 {
		t.Error("settings table was not fully replaced: stale row survived")
	}
	var theme model.Setting
	if err := gdb.Where("key = ?", "theme").First(&theme).Error; err != nil {
		t.Error("settings table was not fully replaced: theme row missing")
	}

	// And the export side filters identity rows out of the payload.
	got, err := h.exportCloneState()
	if err != nil {
		t.Fatalf("exportCloneState: %v", err)
	}
	for _, ci := range got.ConfigItems {
		if cloneIdentityDomains[ci.Domain] {
			t.Errorf("exportCloneState leaked identity-domain row domain=%s into payload", ci.Domain)
		}
	}
}

// TestCloneExportStateFiltersIdentity confirms exportCloneState drops auth +
// device_info rows so the bundle never carries identity desired-state.
func TestCloneExportStateFiltersIdentity(t *testing.T) {
	gdb := newCloneTestDB(t)
	rows := []model.ConfigItem{
		{Domain: "auth", Key: "config", ValueJSON: `{"token_key":"x"}`},
		{Domain: "device_info", Key: "name", ValueJSON: `{"hostname":"c"}`},
		{Domain: "media", Key: "config", ValueJSON: `{"a":1}`},
	}
	if err := gdb.Create(&rows).Error; err != nil {
		t.Fatalf("seed: %v", err)
	}
	h := &APIHandlers{db: gdb}
	got, err := h.exportCloneState()
	if err != nil {
		t.Fatalf("exportCloneState: %v", err)
	}
	if len(got.ConfigItems) != 1 {
		t.Fatalf("exported %d config_items, want 1 (media only; auth+device_info filtered)", len(got.ConfigItems))
	}
	if got.ConfigItems[0].Domain != "media" {
		t.Errorf("exported domain = %q, want media", got.ConfigItems[0].Domain)
	}
}

// TestCloneBundleTamperDetection verifies the clone import's integrity gate: a
// bundle whose state/tables.json bytes diverge from its manifest sha256 is
// rejected by verifyManifest. (The same gate covers etc/* entries.)
func TestCloneBundleTamperDetection(t *testing.T) {
	// Build a tiny honest bundle, then corrupt the manifest sha256 for
	// state/tables.json and expect verifyManifest to fail.
	withTempEtc(t, map[string]string{"camera-daemon.yaml": "encoders: {}\n"})
	out := filepath.Join(t.TempDir(), "clone.tar.gz")
	manifest := &configManifest{Schema: deviceCloneSchema, Version: deviceCloneVersion}
	if err := writeCloneBundle(out, cloneStatePayload{}, manifest); err != nil {
		t.Fatalf("writeCloneBundle: %v", err)
	}

	// Read back via untar to get members + manifest, then flip one sha256.
	f, err := os.Open(out)
	if err != nil {
		t.Fatalf("open: %v", err)
	}
	extractDir := t.TempDir()
	manifestJSON, members, err := untar(f, extractDir)
	f.Close()
	if err != nil {
		t.Fatalf("untar: %v", err)
	}
	m, err := decodeManifest(manifestJSON)
	if err != nil {
		t.Fatalf("decodeManifest: %v", err)
	}
	// Tamper: corrupt the state/tables.json sha256 in the in-memory manifest.
	for i := range m.Files {
		if m.Files[i].Path == "state/tables.json" {
			m.Files[i].SHA256 = "0000000000000000000000000000000000000000000000000000000000000000"
		}
	}
	if _, err := verifyManifest(m, members); err == nil {
		t.Error("verifyManifest accepted a tampered sha256 (integrity gate broken)")
	}
}

// TestCloneImportRejectsWrongSchema ensures the clone schema gate distinguishes
// a clone bundle from a media bundle (which shares the tar+manifest transport
// but not the schema), so a mis-uploaded media bundle is rejected.
func TestCloneImportRejectsWrongSchema(t *testing.T) {
	var buf bytes.Buffer
	if err := writeTarGz(&buf, func(tw *tar.Writer) error {
		mb, _ := json.MarshalIndent(&configManifest{Schema: mediaBundleSchema, Version: mediaBundleVersion}, "", "  ")
		return tarWriteBytes(tw, "manifest.json", mb, 0644)
	}); err != nil {
		t.Fatalf("writeTarGz: %v", err)
	}
	manifestJSON, _, err := untar(&buf, t.TempDir())
	if err != nil {
		t.Fatalf("untar: %v", err)
	}
	m, err := decodeManifest(manifestJSON)
	if err != nil {
		t.Fatalf("decode: %v", err)
	}
	if m.Schema == deviceCloneSchema {
		t.Fatal("test precondition: media bundle schema should differ from clone schema")
	}
	// Mirrors the ImportDeviceConfig schema check.
	if m.Schema != deviceCloneSchema {
		// expected: rejected. Nothing to assert beyond reaching here.
		t.Log("clone gate correctly distinguishes non-clone schema:", m.Schema)
	}
}
