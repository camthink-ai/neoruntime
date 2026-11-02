package cellular

import (
	"context"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

const sampleJSON = `{
  "enabled": true,
  "apn": "internet",
  "user": "",
  "password": "",
  "auth": "none",
  "ip_type": "ipv4v6",
  "pdp_cid": 1,
  "carrier_profile": "generic",
  "usbnet": "mbim",
  "network_mode": "auto",
  "roaming": false,
  "pin": ""
}
`

func newAdapter(t *testing.T, seed string) (*Adapter, string) {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "cellular.json")
	a := New(path)
	if seed != "" {
		if err := os.WriteFile(path, []byte(seed), 0644); err != nil {
			t.Fatalf("seed: %v", err)
		}
	}
	return a, path
}

func TestValidate(t *testing.T) {
	a, _ := newAdapter(t, "")
	if err := a.Validate(context.Background(), keyConfig, sampleJSON); err != nil {
		t.Fatal(err)
	}
	if err := a.Validate(context.Background(), keyConfig, "{"); err == nil {
		t.Fatal("want invalid json")
	}
	if err := a.Validate(context.Background(), "other", sampleJSON); err != ErrUnknownKey {
		t.Fatalf("err=%v", err)
	}
	badAuth := strings.Replace(sampleJSON, `"auth": "none"`, `"auth": "md5"`, 1)
	if err := a.Validate(context.Background(), keyConfig, badAuth); err == nil {
		t.Fatal("want auth rejected")
	}
	badUsb := strings.Replace(sampleJSON, `"usbnet": "mbim"`, `"usbnet": "rndis"`, 1)
	if err := a.Validate(context.Background(), keyConfig, badUsb); err == nil {
		t.Fatal("want usbnet rejected")
	}
}

func TestBackupMissingIsNil(t *testing.T) {
	a, _ := newAdapter(t, "")
	got, err := a.Backup(context.Background(), keyConfig)
	if err != nil {
		t.Fatal(err)
	}
	bs := got.(backupState)
	if bs.bytes != nil {
		t.Fatalf("want nil backup, got %q", bs.bytes)
	}
}

func TestApplyVerifyRestore(t *testing.T) {
	a, path := newAdapter(t, "")
	ctx := context.Background()

	bs, err := a.Backup(ctx, keyConfig)
	if err != nil {
		t.Fatal(err)
	}
	rd, err := a.Render(ctx, keyConfig, sampleJSON)
	if err != nil {
		t.Fatal(err)
	}
	if err := a.Apply(ctx, keyConfig, rd); err != nil {
		t.Fatal(err)
	}
	if err := a.Verify(ctx, keyConfig, sampleJSON); err != nil {
		t.Fatal(err)
	}
	got, err := os.ReadFile(path)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != sampleJSON {
		t.Fatalf("disk=%q", got)
	}

	if err := a.Restore(ctx, keyConfig, bs); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("restore of nil backup should remove file, err=%v", err)
	}
}

func TestSnapshot(t *testing.T) {
	a, _ := newAdapter(t, sampleJSON)
	got, err := a.Snapshot(context.Background(), keyConfig)
	if err != nil {
		t.Fatal(err)
	}
	if got != sampleJSON {
		t.Fatalf("snapshot=%q", got)
	}
}
