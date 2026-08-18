package media

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"gopkg.in/yaml.v3"

	"aipc/platform/common/constants"
)

// newAdapter builds an Adapter whose configPath lives under a temp dir,
// optionally seeding an existing camera-daemon.yaml. Returns the adapter and
// the config path so tests can read/inspect the file.
func newAdapter(t *testing.T, seed string) (*Adapter, string) {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "camera-daemon.yaml")
	if seed != "" {
		if err := os.WriteFile(path, []byte(seed), 0644); err != nil {
			t.Fatalf("seed config: %v", err)
		}
	}
	return New(path), path
}

const sampleYAML = "encoders:\n- stream_name: main\n  width: 1920\n  height: 1080\n  codec: h264\n  bitrate: 4000000\n  fps: 30\n  gop: 60\nrtsp:\n  enabled: true\n"

func TestValidate(t *testing.T) {
	a, _ := newAdapter(t, "")
	ctx := context.Background()
	cases := []struct {
		name    string
		key     string
		desired string
		wantErr error
	}{
		{"ok", keyConfig, sampleYAML, nil},
		{"empty yaml ok", keyConfig, "", nil}, // empty string unmarshals to nil map, valid
		{"bad yaml", keyConfig, "encoders:\n  - [unterminated", ErrInvalidYAML},
		{"unknown key", "nope", sampleYAML, ErrUnknownKey},
	}
	for _, tc := range cases {
		err := a.Validate(ctx, tc.key, tc.desired)
		if tc.wantErr == nil {
			if err != nil {
				t.Errorf("%s: want nil, got %v", tc.name, err)
			}
			continue
		}
		if !errors.Is(err, tc.wantErr) {
			t.Errorf("%s: want %v, got %v", tc.name, tc.wantErr, err)
		}
	}
}

func TestBackup_ExistingFile(t *testing.T) {
	a, path := newAdapter(t, sampleYAML)
	bs, err := a.Backup(context.Background(), keyConfig)
	if err != nil {
		t.Fatalf("backup: %v", err)
	}
	if string(bs.(backupState).bytes) != sampleYAML {
		t.Fatalf("backup bytes = %q, want seeded YAML", bs.(backupState).bytes)
	}
	// Backup must not mutate the file.
	got, _ := os.ReadFile(path)
	if string(got) != sampleYAML {
		t.Fatalf("file changed after backup: %q", got)
	}
}

func TestBackup_MissingFileIsNil(t *testing.T) {
	a, _ := newAdapter(t, "")
	bs, err := a.Backup(context.Background(), keyConfig)
	if err != nil {
		t.Fatalf("backup missing: %v", err)
	}
	if bs.(backupState).bytes != nil {
		t.Fatalf("want nil bytes for missing file, got %q", bs.(backupState).bytes)
	}
}

func TestBackup_UnknownKey(t *testing.T) {
	a, _ := newAdapter(t, "")
	if _, err := a.Backup(context.Background(), "nope"); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("want ErrUnknownKey, got %v", err)
	}
}

func TestRender_Passthrough(t *testing.T) {
	a, _ := newAdapter(t, "")
	r, err := a.Render(context.Background(), keyConfig, sampleYAML)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	if string(r.(rendered)) != sampleYAML {
		t.Fatalf("render changed the bytes: %q", r.(rendered))
	}
}

func TestRender_UnknownKey(t *testing.T) {
	a, _ := newAdapter(t, "")
	if _, err := a.Render(context.Background(), "nope", sampleYAML); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("want ErrUnknownKey, got %v", err)
	}
}

func TestNormalizeMigratesReleasePathsAndDropsConfigPath(t *testing.T) {
	root := t.TempDir()
	oldRoot := constants.RootPath()
	constants.SetRootPath(root)
	t.Cleanup(func() { constants.SetRootPath(oldRoot) })

	halDir := filepath.Join(root, "lib", "hal")
	if err := os.MkdirAll(halDir, 0755); err != nil {
		t.Fatalf("mkdir hal dir: %v", err)
	}
	for _, name := range []string{"libaipc_hal.so", "libhal-lens-bridge.so"} {
		if err := os.WriteFile(filepath.Join(halDir, name), []byte("test"), 0644); err != nil {
			t.Fatalf("write hal %s: %v", name, err)
		}
	}

	desired := `hal:
  video_library: /mnt/old-release/lib/hal/libaipc_hal.so
  codec_library: /opt/vendor/libhal.so
  lens_library: /legacy/libhal-lens-bridge.so
media:
  config_path: /vendor/imaging/ai_example_medialib_config.json
  backup_path: /mnt/old-data/media-backup
`
	got, changed, err := New("").Normalize(context.Background(), keyConfig, desired)
	if err != nil {
		t.Fatalf("normalize: %v", err)
	}
	if !changed {
		t.Fatal("Normalize changed=false, want true")
	}
	for _, want := range []string{
		filepath.Join(root, "lib", "hal", "libaipc_hal.so"),
		filepath.Join(root, "lib", "hal", "libhal-lens-bridge.so"),
		filepath.Join(root, "data", "media-backup"),
	} {
		if !strings.Contains(got, want) {
			t.Fatalf("normalized YAML missing %q:\n%s", want, got)
		}
	}
	// config_path is dropped entirely (no longer platform-maintained); the HAL and
	// backup release paths are still migrated to the canonical install root.
	for _, forbidden := range []string{"/mnt/old-release", "/opt/vendor", "/legacy", "ai_example_medialib_config.json", "/mnt/old-data", "config_path"} {
		if strings.Contains(got, forbidden) {
			t.Fatalf("normalized YAML still contains %q:\n%s", forbidden, got)
		}
	}
}

func TestNormalizeLeavesCanonicalPathsUnchanged(t *testing.T) {
	root := t.TempDir()
	oldRoot := constants.RootPath()
	constants.SetRootPath(root)
	t.Cleanup(func() { constants.SetRootPath(oldRoot) })

	halDir := filepath.Join(root, "lib", "hal")
	if err := os.MkdirAll(halDir, 0755); err != nil {
		t.Fatalf("mkdir hal dir: %v", err)
	}
	for _, name := range []string{"libaipc_hal.so", "libhal-lens-bridge.so"} {
		if err := os.WriteFile(filepath.Join(halDir, name), []byte("test"), 0644); err != nil {
			t.Fatalf("write hal %s: %v", name, err)
		}
	}

	desired := "hal:\n" +
		"  video_library: " + filepath.Join(root, "lib", "hal", "libaipc_hal.so") + "\n" +
		"  codec_library: " + filepath.Join(root, "lib", "hal", "libaipc_hal.so") + "\n" +
		"  lens_library: " + filepath.Join(root, "lib", "hal", "libhal-lens-bridge.so") + "\n" +
		"media:\n" +
		"  backup_path: " + filepath.Join(root, "data", "media-backup") + "\n"

	got, changed, err := New("").Normalize(context.Background(), keyConfig, desired)
	if err != nil {
		t.Fatalf("normalize: %v", err)
	}
	if changed {
		t.Fatalf("Normalize changed=true, want false; got:\n%s", got)
	}
	if got != desired {
		t.Fatalf("Normalize changed bytes without changed=true:\n%s", got)
	}
}

func TestApply_WritesFile(t *testing.T) {
	a, path := newAdapter(t, "")
	if err := a.Apply(context.Background(), keyConfig, rendered([]byte(sampleYAML))); err != nil {
		t.Fatalf("apply: %v", err)
	}
	got, _ := os.ReadFile(path)
	if string(got) != sampleYAML {
		t.Fatalf("file = %q, want %q", got, sampleYAML)
	}
}

func TestApply_CreatesParentDir(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "nested", "deep", "camera-daemon.yaml")
	a := New(path)
	if err := a.Apply(context.Background(), keyConfig, rendered([]byte(sampleYAML))); err != nil {
		t.Fatalf("apply with missing parent: %v", err)
	}
	got, _ := os.ReadFile(path)
	if string(got) != sampleYAML {
		t.Fatalf("file = %q", got)
	}
}

func TestApply_BadRenderedType(t *testing.T) {
	a, _ := newAdapter(t, "")
	if err := a.Apply(context.Background(), keyConfig, 123); !errors.Is(err, ErrBadRenderedType) {
		t.Fatalf("want ErrBadRenderedType, got %v", err)
	}
}

func TestApply_UnknownKey(t *testing.T) {
	a, _ := newAdapter(t, "")
	if err := a.Apply(context.Background(), "nope", rendered([]byte(sampleYAML))); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("want ErrUnknownKey, got %v", err)
	}
}

func TestVerify_Match(t *testing.T) {
	a, path := newAdapter(t, "")
	_ = os.WriteFile(path, []byte(sampleYAML), 0644)
	if err := a.Verify(context.Background(), keyConfig, sampleYAML); err != nil {
		t.Fatalf("verify match: %v", err)
	}
}

func TestVerify_Mismatch(t *testing.T) {
	a, path := newAdapter(t, "")
	_ = os.WriteFile(path, []byte("encoders: []\n"), 0644)
	if err := a.Verify(context.Background(), keyConfig, sampleYAML); err == nil {
		t.Fatal("verify mismatched content want error")
	}
}

func TestVerify_MissingFile(t *testing.T) {
	a, _ := newAdapter(t, "")
	if err := a.Verify(context.Background(), keyConfig, sampleYAML); err == nil {
		t.Fatal("verify missing file want error")
	}
}

func TestVerify_UnknownKey(t *testing.T) {
	a, _ := newAdapter(t, "")
	if err := a.Verify(context.Background(), "nope", sampleYAML); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("want ErrUnknownKey, got %v", err)
	}
}

func TestRestore_RevertsFile(t *testing.T) {
	a, path := newAdapter(t, "")
	// Simulate a failed Apply: write something new, then restore the backup.
	_ = os.WriteFile(path, []byte("encoders: NEW\n"), 0644)
	if err := a.Restore(context.Background(), keyConfig, backupState{bytes: []byte(sampleYAML)}); err != nil {
		t.Fatalf("restore: %v", err)
	}
	got, _ := os.ReadFile(path)
	if string(got) != sampleYAML {
		t.Fatalf("after restore = %q, want %q", got, sampleYAML)
	}
}

func TestRestore_RemovesCreatedFile(t *testing.T) {
	a, path := newAdapter(t, "")
	_ = os.WriteFile(path, []byte("encoders: NEW\n"), 0644)
	// nil-byte backup ⇒ file did not exist pre-Apply ⇒ remove.
	if err := a.Restore(context.Background(), keyConfig, backupState{nil}); err != nil {
		t.Fatalf("restore: %v", err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("want file removed, got %v", err)
	}
}

func TestRestore_RemovesAlreadyGone(t *testing.T) {
	a, path := newAdapter(t, "")
	_ = os.Remove(path)
	// Removing a file that's already gone must not error.
	if err := a.Restore(context.Background(), keyConfig, backupState{nil}); err != nil {
		t.Fatalf("restore already-gone: %v", err)
	}
}

func TestRestore_BadBackupType(t *testing.T) {
	a, _ := newAdapter(t, "")
	if err := a.Restore(context.Background(), keyConfig, "nope"); !errors.Is(err, ErrBadBackupType) {
		t.Fatalf("want ErrBadBackupType, got %v", err)
	}
}

func TestRestore_UnknownKey(t *testing.T) {
	a, _ := newAdapter(t, "")
	if err := a.Restore(context.Background(), "nope", backupState{nil}); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("want ErrUnknownKey, got %v", err)
	}
}

func TestNewReturnsAdapter(t *testing.T) {
	if a := New(""); a == nil {
		t.Fatal("New returned nil")
	}
}

// TestRoundTrip_BackupApplyVerifyRestore exercises the full Manager-shaped
// sequence against one adapter instance to confirm the pieces compose.
func TestRoundTrip_BackupApplyVerifyRestore(t *testing.T) {
	a, path := newAdapter(t, "encoders: OLD\n")
	ctx := context.Background()

	backup, err := a.Backup(ctx, keyConfig)
	if err != nil {
		t.Fatalf("backup: %v", err)
	}
	r, err := a.Render(ctx, keyConfig, sampleYAML)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	if err := a.Apply(ctx, keyConfig, r); err != nil {
		t.Fatalf("apply: %v", err)
	}
	if err := a.Verify(ctx, keyConfig, sampleYAML); err != nil {
		t.Fatalf("verify after apply: %v", err)
	}
	// Simulate Verify failure elsewhere → restore the backup.
	if err := a.Restore(ctx, keyConfig, backup); err != nil {
		t.Fatalf("restore: %v", err)
	}
	got, _ := os.ReadFile(path)
	if string(got) != "encoders: OLD\n" {
		t.Fatalf("after restore = %q, want OLD", got)
	}
}

// TestNormalizeSwapsPortraitEncoderDimsToLandscape verifies the boot-residual
// black-screen guard: portrait-transposed encoder dims (left in the DB by a save
// while rotation was 90/270, replayed to the YAML on every boot) are swapped back
// to landscape in Normalize — the single chokepoint covering both the save path
// and the boot DB→YAML re-project. Output is decoded structurally so a landscape
// width (3840) is never confused with another encoder's portrait width (384).
func TestNormalizeSwapsPortraitEncoderDimsToLandscape(t *testing.T) {
	// Arrange: only encoders (no hal/media) so dim normalization runs in isolation.
	desired := `encoders:
  - stream_name: main
    width: 2160
    height: 3840
  - stream_name: sub
    width: 480
    height: 640
  - stream_name: third
    width: 384
    height: 640
`
	// Act
	got, changed, err := New("").Normalize(context.Background(), keyConfig, desired)
	// Assert
	if err != nil {
		t.Fatalf("normalize: %v", err)
	}
	if !changed {
		t.Fatal("Normalize changed=false, want true (portrait dims must be swapped)")
	}
	var parsed struct {
		Encoders []struct {
			StreamName string `yaml:"stream_name"`
			Width      int    `yaml:"width"`
			Height     int    `yaml:"height"`
		} `yaml:"encoders"`
	}
	if err := yaml.Unmarshal([]byte(got), &parsed); err != nil {
		t.Fatalf("re-decode normalized YAML: %v\n%s", err, got)
	}
	want := map[string][2]int{"main": {3840, 2160}, "sub": {640, 480}, "third": {640, 384}}
	if len(parsed.Encoders) != len(want) {
		t.Fatalf("got %d encoders, want %d:\n%s", len(parsed.Encoders), len(want), got)
	}
	for _, e := range parsed.Encoders {
		exp, ok := want[e.StreamName]
		if !ok {
			t.Fatalf("unexpected stream %q:\n%s", e.StreamName, got)
		}
		if e.Width != exp[0] || e.Height != exp[1] {
			t.Errorf("stream %q = %dx%d, want %dx%d", e.StreamName, e.Width, e.Height, exp[0], exp[1])
		}
		if e.Height > e.Width {
			t.Errorf("stream %q still portrait (%dx%d)", e.StreamName, e.Width, e.Height)
		}
	}
}

func TestNormalizeLeavesLandscapeEncoderDimsUnchanged(t *testing.T) {
	desired := `encoders:
  - stream_name: main
    width: 3840
    height: 2160
  - stream_name: sub
    width: 640
    height: 480
`
	got, changed, err := New("").Normalize(context.Background(), keyConfig, desired)
	if err != nil {
		t.Fatalf("normalize: %v", err)
	}
	if changed {
		t.Fatalf("Normalize changed=true, want false (landscape dims are canonical):\n%s", got)
	}
}
