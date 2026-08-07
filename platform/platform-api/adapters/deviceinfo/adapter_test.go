package deviceinfo

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// fakeHost is a test hostnameController that records calls without touching the
// live system. Set can be made to fail via failSet.
type fakeHost struct {
	current string
	failSet error
	setCall string
}

func (f *fakeHost) Set(name string) error {
	f.setCall = name
	if f.failSet != nil {
		return f.failSet
	}
	f.current = name
	return nil
}

func (f *fakeHost) Get() string { return f.current }

// newAdapter builds an Adapter on a temp device.conf, optionally seeded with
// initial content, and a fake host with the given starting hostname.
func newAdapter(t *testing.T, seedConf string, startHost string) (*Adapter, string, *fakeHost) {
	t.Helper()
	dir := t.TempDir()
	path := filepath.Join(dir, "device.conf")
	if seedConf != "" {
		if err := os.WriteFile(path, []byte(seedConf), 0644); err != nil {
			t.Fatalf("seed device.conf: %v", err)
		}
	}
	host := &fakeHost{current: startHost}
	a := &Adapter{configPath: path, host: host}
	return a, path, host
}

func TestValidate(t *testing.T) {
	a, _, _ := newAdapter(t, "", "old")
	ctx := context.Background()

	if err := a.Validate(ctx, "device_name", `{"device_name":"NE503-001"}`); err != nil {
		t.Fatalf("valid name rejected: %v", err)
	}
	if err := a.Validate(ctx, "device_name", `{"device_name":""}`); !errors.Is(err, ErrInvalidName) {
		t.Fatalf("empty name want ErrInvalidName, got %v", err)
	}
	if err := a.Validate(ctx, "device_name", `{"device_name":"bad name!"}`); !errors.Is(err, ErrInvalidName) {
		t.Fatalf("invalid chars want ErrInvalidName, got %v", err)
	}
	if err := a.Validate(ctx, "device_name", `{not-json}`); !errors.Is(err, ErrInvalidJSON) {
		t.Fatalf("bad json want ErrInvalidJSON, got %v", err)
	}
}

func TestBackup_PreservesExisting(t *testing.T) {
	a, _, host := newAdapter(t, "MODEL=X\nDEVICE_NAME=old\n", "oldhost")
	bs, err := a.Backup(context.Background(), "device_name")
	if err != nil {
		t.Fatalf("backup: %v", err)
	}
	b, ok := bs.(backupState)
	if !ok {
		t.Fatalf("backup not backupState: %T", bs)
	}
	if string(b.confBytes) != "MODEL=X\nDEVICE_NAME=old\n" {
		t.Fatalf("confBytes = %q", string(b.confBytes))
	}
	if b.hostname != "oldhost" {
		t.Fatalf("hostname = %q", b.hostname)
	}
	if host.setCall != "" {
		t.Fatalf("backup mutated hostname to %q", host.setCall)
	}
}

func TestBackup_MissingFileIsNilBytes(t *testing.T) {
	a, _, _ := newAdapter(t, "", "h")
	bs, err := a.Backup(context.Background(), "device_name")
	if err != nil {
		t.Fatalf("backup missing file: %v", err)
	}
	b := bs.(backupState)
	if b.confBytes != nil {
		t.Fatalf("want nil confBytes for missing file, got %q", string(b.confBytes))
	}
}

func TestRender_ReplacesLine(t *testing.T) {
	a, _, _ := newAdapter(t, "MODEL=X\nDEVICE_NAME=old\nHARDWARE_VERSION=1\n", "h")
	r, err := a.Render(context.Background(), "device_name", `{"device_name":"new"}`)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	rd := r.(rendered)
	if rd.name != "new" {
		t.Fatalf("name = %q", rd.name)
	}
	got := string(rd.bytes)
	if !strings.Contains(got, "DEVICE_NAME=new") || strings.Contains(got, "DEVICE_NAME=old") {
		t.Fatalf("rendered = %q", got)
	}
	if !strings.Contains(got, "MODEL=X") || !strings.Contains(got, "HARDWARE_VERSION=1") {
		t.Fatalf("other keys not preserved: %q", got)
	}
}

func TestRender_AppendsLine(t *testing.T) {
	a, _, _ := newAdapter(t, "MODEL=X\n", "h")
	r, err := a.Render(context.Background(), "device_name", `{"device_name":"new"}`)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	rd := r.(rendered)
	if !strings.Contains(string(rd.bytes), "DEVICE_NAME=new") {
		t.Fatalf("rendered = %q", string(rd.bytes))
	}
}

func TestApply_SetsHostAndWritesFile(t *testing.T) {
	a, path, host := newAdapter(t, "DEVICE_NAME=old\n", "old")
	r := rendered{bytes: []byte("DEVICE_NAME=new\n"), name: "new"}
	if err := a.Apply(context.Background(), "device_name", r); err != nil {
		t.Fatalf("apply: %v", err)
	}
	if host.setCall != "new" {
		t.Fatalf("hostname set to %q", host.setCall)
	}
	data, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("read back: %v", err)
	}
	if extractDeviceName(data) != "new" {
		t.Fatalf("file = %q", string(data))
	}
}

func TestApply_BadRenderedType(t *testing.T) {
	a, _, _ := newAdapter(t, "", "h")
	if err := a.Apply(context.Background(), "device_name", 123); err == nil {
		t.Fatal("apply with wrong type want error")
	}
}

func TestApply_HostFail(t *testing.T) {
	a, _, host := newAdapter(t, "", "h")
	host.failSet = errors.New("boom")
	r := rendered{bytes: []byte("DEVICE_NAME=new\n"), name: "new"}
	if err := a.Apply(context.Background(), "device_name", r); err == nil {
		t.Fatal("apply with host fail want error")
	}
}

func TestVerify_Match(t *testing.T) {
	a, path, host := newAdapter(t, "DEVICE_NAME=old\n", "new")
	_ = os.WriteFile(path, []byte("DEVICE_NAME=new\n"), 0644)
	host.current = "new"
	if err := a.Verify(context.Background(), "device_name", `{"device_name":"new"}`); err != nil {
		t.Fatalf("verify: %v", err)
	}
}

func TestVerify_ConfMismatch(t *testing.T) {
	a, _, host := newAdapter(t, "DEVICE_NAME=other\n", "new")
	host.current = "new"
	err := a.Verify(context.Background(), "device_name", `{"device_name":"new"}`)
	if err == nil || !strings.Contains(err.Error(), "device.conf") {
		t.Fatalf("want device.conf mismatch, got %v", err)
	}
}

func TestVerify_HostMismatch(t *testing.T) {
	a, path, host := newAdapter(t, "", "wrong")
	_ = os.WriteFile(path, []byte("DEVICE_NAME=new\n"), 0644)
	host.current = "wrong"
	err := a.Verify(context.Background(), "device_name", `{"device_name":"new"}`)
	if err == nil || !strings.Contains(err.Error(), "hostname") {
		t.Fatalf("want hostname mismatch, got %v", err)
	}
}

func TestRestore_RevertsConfAndHost(t *testing.T) {
	a, path, host := newAdapter(t, "DEVICE_NAME=old\n", "old")
	// Simulate a failed apply: file now says new, host says new.
	_ = os.WriteFile(path, []byte("DEVICE_NAME=new\n"), 0644)
	host.current = "new"

	bs := backupState{confBytes: []byte("DEVICE_NAME=old\n"), hostname: "old"}
	if err := a.Restore(context.Background(), "device_name", bs); err != nil {
		t.Fatalf("restore: %v", err)
	}
	data, _ := os.ReadFile(path)
	if extractDeviceName(data) != "old" {
		t.Fatalf("conf after restore = %q", string(data))
	}
	if host.setCall != "old" {
		t.Fatalf("hostname restore set to %q", host.setCall)
	}
}

func TestRestore_RemovesCreatedFile(t *testing.T) {
	a, path, _ := newAdapter(t, "", "h")
	_ = os.WriteFile(path, []byte("DEVICE_NAME=new\n"), 0644)
	bs := backupState{confBytes: nil, hostname: ""}
	if err := a.Restore(context.Background(), "device_name", bs); err != nil {
		t.Fatalf("restore: %v", err)
	}
	if _, err := os.Stat(path); !os.IsNotExist(err) {
		t.Fatalf("want file removed, got stat err=%v", err)
	}
}

func TestRestore_BadBackupType(t *testing.T) {
	a, _, _ := newAdapter(t, "", "h")
	if err := a.Restore(context.Background(), "device_name", "nope"); err == nil {
		t.Fatal("restore with bad backup type want error")
	}
}

func TestNewReturnsAdapter(t *testing.T) {
	if a := New("/tmp/whatever-device.conf"); a == nil {
		t.Fatal("New returned nil")
	}
}

// TestLiveHostnameGet covers the production hostnameController read path.
// Set is intentionally not exercised — it execs `hostname` and writes
// /etc/hostname, which mutates the live system and is unsafe in CI.
func TestLiveHostnameGet(t *testing.T) {
	h := liveHostname{}
	want, err := os.Hostname()
	if err != nil {
		t.Skip("os.Hostname unavailable")
	}
	if got := h.Get(); got != want {
		t.Fatalf("liveHostname.Get = %q, want %q", got, want)
	}
}

func TestRender_BadJSON(t *testing.T) {
	a, _, _ := newAdapter(t, "", "h")
	if _, err := a.Render(context.Background(), "device_name", `{bad}`); !errors.Is(err, ErrInvalidJSON) {
		t.Fatalf("want ErrInvalidJSON, got %v", err)
	}
}

func TestVerify_BadJSON(t *testing.T) {
	a, _, _ := newAdapter(t, "", "h")
	if err := a.Verify(context.Background(), "device_name", `{bad}`); !errors.Is(err, ErrInvalidJSON) {
		t.Fatalf("want ErrInvalidJSON, got %v", err)
	}
}

func TestRestore_HostFail(t *testing.T) {
	a, _, host := newAdapter(t, "DEVICE_NAME=old\n", "old")
	host.failSet = errors.New("cannot reset")
	bs := backupState{confBytes: []byte("DEVICE_NAME=old\n"), hostname: "old"}
	if err := a.Restore(context.Background(), "device_name", bs); err == nil {
		t.Fatal("restore with host fail want error")
	}
}
