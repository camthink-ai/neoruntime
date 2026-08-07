package auth

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

// sampleYAML mirrors the shape of platform-api.yaml: a top-level auth section
// plus sibling sections the password-change flow must preserve untouched.
const sampleYAML = `auth:
  username: admin
  password: changeme
  token_key: tk-12345
  enabled: true
stream:
  camera_config: /data/aipc/etc/camera-daemon.yaml
storage:
  model_path: /data/models
`

func newAdapter(t *testing.T, seed string) (*Adapter, string) {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "platform-api.yaml")
	a := New(path)
	if seed != "" {
		if err := os.WriteFile(path, []byte(seed), 0644); err != nil {
			t.Fatalf("seed config: %v", err)
		}
	}
	return a, path
}

func TestValidate(t *testing.T) {
	cases := []struct {
		name    string
		key     string
		desired string
		wantErr bool
	}{
		{"ok", keyConfig, sampleYAML, false},
		{"empty-doc-ok", keyConfig, "", false}, // empty YAML parses to nil map
		{"bad-yaml", keyConfig, "auth:\n  password: : bad\n", true},
		{"unknown-key", "streams", sampleYAML, true},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			a, _ := newAdapter(t, "")
			err := a.Validate(context.Background(), tc.key, tc.desired)
			if (err != nil) != tc.wantErr {
				t.Fatalf("Validate err=%v wantErr=%v", err, tc.wantErr)
			}
		})
	}
}

func TestBackup_ExistingFile(t *testing.T) {
	a, _ := newAdapter(t, sampleYAML)
	got, err := a.Backup(context.Background(), keyConfig)
	if err != nil {
		t.Fatalf("Backup: %v", err)
	}
	bs, ok := got.(backupState)
	if !ok {
		t.Fatalf("Backup returned %T, want backupState", got)
	}
	if string(bs.bytes) != sampleYAML {
		t.Errorf("Backup bytes mismatch")
	}
}

func TestBackup_MissingFileIsNil(t *testing.T) {
	a, _ := newAdapter(t, "")
	got, err := a.Backup(context.Background(), keyConfig)
	if err != nil {
		t.Fatalf("Backup missing file: %v", err)
	}
	bs, ok := got.(backupState)
	if !ok {
		t.Fatalf("Backup returned %T, want backupState", got)
	}
	if bs.bytes != nil {
		t.Errorf("Backup of missing file = %v, want nil", bs.bytes)
	}
}

func TestBackup_UnknownKey(t *testing.T) {
	a, _ := newAdapter(t, sampleYAML)
	if _, err := a.Backup(context.Background(), "nope"); err != ErrUnknownKey {
		t.Fatalf("Backup unknown key err=%v want ErrUnknownKey", err)
	}
}

func TestRender_Passthrough(t *testing.T) {
	a, _ := newAdapter(t, "")
	got, err := a.Render(context.Background(), keyConfig, sampleYAML)
	if err != nil {
		t.Fatalf("Render: %v", err)
	}
	rd, ok := got.(rendered)
	if !ok {
		t.Fatalf("Render returned %T, want rendered", got)
	}
	if string(rd) != sampleYAML {
		t.Errorf("Render did not pass through verbatim")
	}
}

func TestRender_UnknownKey(t *testing.T) {
	a, _ := newAdapter(t, "")
	if _, err := a.Render(context.Background(), "nope", sampleYAML); err != ErrUnknownKey {
		t.Fatalf("Render unknown key err=%v want ErrUnknownKey", err)
	}
}

func TestApply_WritesFile(t *testing.T) {
	a, path := newAdapter(t, "")
	if err := a.Apply(context.Background(), keyConfig, rendered([]byte(sampleYAML))); err != nil {
		t.Fatalf("Apply: %v", err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read back: %v", err)
	}
	if string(got) != sampleYAML {
		t.Errorf("Apply wrote wrong content")
	}
}

func TestApply_CreatesParentDir(t *testing.T) {
	dir := t.TempDir()
	nested := filepath.Join(dir, "nested", "deep", "platform-api.yaml")
	a := New(nested)
	if err := a.Apply(context.Background(), keyConfig, rendered([]byte(sampleYAML))); err != nil {
		t.Fatalf("Apply with missing parent: %v", err)
	}
	if _, err := os.Stat(nested); err != nil {
		t.Errorf("parent dir not created: %v", err)
	}
}

func TestApply_BadRenderedType(t *testing.T) {
	a, _ := newAdapter(t, "")
	if err := a.Apply(context.Background(), keyConfig, "not-bytes"); err != ErrBadRenderedType {
		t.Fatalf("Apply bad type err=%v want ErrBadRenderedType", err)
	}
}

func TestApply_UnknownKey(t *testing.T) {
	a, _ := newAdapter(t, "")
	if err := a.Apply(context.Background(), "nope", rendered(nil)); err != ErrUnknownKey {
		t.Fatalf("Apply unknown key err=%v want ErrUnknownKey", err)
	}
}

func TestVerify_Match(t *testing.T) {
	a, _ := newAdapter(t, sampleYAML)
	if err := a.Verify(context.Background(), keyConfig, sampleYAML); err != nil {
		t.Fatalf("Verify match: %v", err)
	}
}

func TestVerify_Mismatch(t *testing.T) {
	a, _ := newAdapter(t, sampleYAML)
	if err := a.Verify(context.Background(), keyConfig, "auth:\n  password: other\n"); err == nil {
		t.Errorf("Verify mismatch expected error, got nil")
	}
}

func TestVerify_MissingFile(t *testing.T) {
	a, _ := newAdapter(t, "")
	if err := a.Verify(context.Background(), keyConfig, sampleYAML); err == nil {
		t.Errorf("Verify missing file expected error, got nil")
	}
}

func TestVerify_UnknownKey(t *testing.T) {
	a, _ := newAdapter(t, sampleYAML)
	if err := a.Verify(context.Background(), "nope", sampleYAML); err != ErrUnknownKey {
		t.Fatalf("Verify unknown key err=%v want ErrUnknownKey", err)
	}
}

func TestRestore_RevertsFile(t *testing.T) {
	a, path := newAdapter(t, "old-bytes")
	if err := a.Apply(context.Background(), keyConfig, rendered([]byte("new-bytes"))); err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if err := a.Restore(context.Background(), keyConfig, backupState{[]byte("old-bytes")}); err != nil {
		t.Fatalf("Restore: %v", err)
	}
	got, _ := os.ReadFile(path)
	if string(got) != "old-bytes" {
		t.Errorf("Restore did not revert: %q", got)
	}
}

func TestRestore_RemovesCreatedFile(t *testing.T) {
	a, path := newAdapter(t, "")
	if err := a.Apply(context.Background(), keyConfig, rendered([]byte("created"))); err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if err := a.Restore(context.Background(), keyConfig, backupState{nil}); err != nil {
		t.Fatalf("Restore nil-backup: %v", err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Errorf("Restore nil-backup should remove file, stat err=%v", err)
	}
}

func TestRestore_RemovesAlreadyGone(t *testing.T) {
	a, _ := newAdapter(t, "")
	if err := a.Restore(context.Background(), keyConfig, backupState{nil}); err != nil {
		t.Fatalf("Restore already-gone: %v", err)
	}
}

func TestRestore_BadBackupType(t *testing.T) {
	a, _ := newAdapter(t, sampleYAML)
	if err := a.Restore(context.Background(), keyConfig, "not-backup"); err != ErrBadBackupType {
		t.Fatalf("Restore bad type err=%v want ErrBadBackupType", err)
	}
}

func TestRestore_UnknownKey(t *testing.T) {
	a, _ := newAdapter(t, sampleYAML)
	if err := a.Restore(context.Background(), "nope", backupState{nil}); err != ErrUnknownKey {
		t.Fatalf("Restore unknown key err=%v want ErrUnknownKey", err)
	}
}

func TestNewReturnsAdapter(t *testing.T) {
	a := New("")
	if a == nil || a.ConfigPath() == "" {
		t.Fatalf("New(\"\") returned bad adapter")
	}
	if filepath.Base(a.ConfigPath()) != "platform-api.yaml" {
		t.Errorf("ConfigPath base = %q, want platform-api.yaml", filepath.Base(a.ConfigPath()))
	}
}

func TestRoundTrip_BackupApplyVerifyRestore(t *testing.T) {
	a, path := newAdapter(t, sampleYAML)
	ctx := context.Background()

	backup, err := a.Backup(ctx, keyConfig)
	if err != nil {
		t.Fatalf("Backup: %v", err)
	}
	newYAML := "auth:\n  password: newpass\n  enabled: true\n"
	if err := a.Apply(ctx, keyConfig, rendered([]byte(newYAML))); err != nil {
		t.Fatalf("Apply: %v", err)
	}
	if err := a.Verify(ctx, keyConfig, newYAML); err != nil {
		t.Fatalf("Verify after Apply: %v", err)
	}
	if err := a.Restore(ctx, keyConfig, backup); err != nil {
		t.Fatalf("Restore: %v", err)
	}
	got, _ := os.ReadFile(path)
	if string(got) != sampleYAML {
		t.Errorf("round-trip did not restore original")
	}
}
