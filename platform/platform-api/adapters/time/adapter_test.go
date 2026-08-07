package time

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// newAdapter builds an Adapter whose three paths all live under a temp dir,
// optionally seeding existing file content for timezone/timesyncd/userConfig.
func newAdapter(t *testing.T, seedTZ, seedTimesyncd, seedUserCfg string) (*Adapter, string, string, string) {
	t.Helper()
	dir := t.TempDir()
	tzPath := filepath.Join(dir, "timezone")
	tsPath := filepath.Join(dir, "systemd", "timesyncd.conf")
	ucPath := filepath.Join(dir, "time-config.json")
	for path, content := range map[string]string{tzPath: seedTZ, tsPath: seedTimesyncd, ucPath: seedUserCfg} {
		if content == "" {
			continue
		}
		if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
			t.Fatalf("mkdir %s: %v", path, err)
		}
		if err := os.WriteFile(path, []byte(content), 0644); err != nil {
			t.Fatalf("seed %s: %v", path, err)
		}
	}
	return New(tzPath, tsPath, ucPath), tzPath, tsPath, ucPath
}

func TestValidate(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	ctx := context.Background()
	cases := []struct {
		name    string
		key     string
		json    string
		wantErr error
	}{
		{"tz ok", keyTimezone, `{"timezone":"Asia/Shanghai"}`, nil},
		{"tz empty", keyTimezone, `{"timezone":""}`, ErrMissingTimezone},
		{"tz bad json", keyTimezone, `{not}`, ErrInvalidJSON},
		{"ntp ok", keyNTP, `{"enabled":true,"server":"pool.ntp.org","interval":3600}`, nil},
		{"ntp disabled ok", keyNTP, `{"enabled":false}`, nil},
		{"ntp bad interval", keyNTP, `{"enabled":true,"interval":-1}`, ErrBadInterval},
		{"ntp bad json", keyNTP, `{x}`, ErrInvalidJSON},
		{"user ok", keyUserConfig, `{"time_format":"24h","sync_mode":"ntp","ntp_interval":3600}`, nil},
		{"user bad interval", keyUserConfig, `{"ntp_interval":-5}`, ErrBadInterval},
		{"config ok", keyConfig, `{"timezone":"UTC","sync_mode":"ntp","ntp_interval":3600}`, nil},
		{"config missing tz", keyConfig, `{"timezone":""}`, ErrMissingTimezone},
		{"config bad interval", keyConfig, `{"timezone":"UTC","ntp_interval":-1}`, ErrBadInterval},
		{"config bad json", keyConfig, `{bad}`, ErrInvalidJSON},
		{"unknown key", "nope", `{}`, ErrUnknownKey},
	}
	for _, tc := range cases {
		err := a.Validate(ctx, tc.key, tc.json)
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

func TestBackup_PerKey(t *testing.T) {
	a, _, _, _ := newAdapter(t, "Asia/Shanghai\n", "[Time]\nNTP=old\n", `{"time_format":"24h"}`)
	ctx := context.Background()

	bs, err := a.Backup(ctx, keyTimezone)
	if err != nil {
		t.Fatalf("backup tz: %v", err)
	}
	if string(bs.(backupState).timezoneBytes) != "Asia/Shanghai\n" {
		t.Fatalf("tz backup = %q", bs.(backupState).timezoneBytes)
	}

	bs, err = a.Backup(ctx, keyNTP)
	if err != nil {
		t.Fatalf("backup ntp: %v", err)
	}
	if string(bs.(backupState).timesyncdBytes) != "[Time]\nNTP=old\n" {
		t.Fatalf("ntp backup = %q", bs.(backupState).timesyncdBytes)
	}

	bs, err = a.Backup(ctx, keyUserConfig)
	if err != nil {
		t.Fatalf("backup uc: %v", err)
	}
	if string(bs.(backupState).userConfigBytes) != `{"time_format":"24h"}` {
		t.Fatalf("uc backup = %q", bs.(backupState).userConfigBytes)
	}

	bs, err = a.Backup(ctx, keyConfig)
	if err != nil {
		t.Fatalf("backup config: %v", err)
	}
	b := bs.(backupState)
	if b.timezoneBytes == nil || b.timesyncdBytes == nil || b.userConfigBytes == nil {
		t.Fatalf("config backup missing a file: %+v", b)
	}
}

func TestBackup_MissingFilesAreNil(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	bs, err := a.Backup(context.Background(), keyConfig)
	if err != nil {
		t.Fatalf("backup: %v", err)
	}
	b := bs.(backupState)
	if b.timezoneBytes != nil || b.timesyncdBytes != nil || b.userConfigBytes != nil {
		t.Fatalf("want all nil, got %+v", b)
	}
}

func TestBackup_UnknownKey(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	if _, err := a.Backup(context.Background(), "nope"); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("want ErrUnknownKey, got %v", err)
	}
}

func TestRender_Timezone(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	r, err := a.Render(context.Background(), keyTimezone, `{"timezone":"Asia/Shanghai"}`)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	rd := r.(rendered)
	if string(rd.timezoneBytes) != "Asia/Shanghai\n" {
		t.Fatalf("tz bytes = %q", rd.timezoneBytes)
	}
	if rd.timesyncdBytes != nil || rd.userConfigBytes != nil {
		t.Fatalf("timezone render touched other files: %+v", rd)
	}
}

func TestRender_NTPEnabled_MergesExisting(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "[Time]\nNTP=old.example\nPollIntervalMaxSec=9999\n", "")
	r, err := a.Render(context.Background(), keyNTP, `{"enabled":true,"server":"pool.ntp.org","interval":3600}`)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	rd := r.(rendered)
	out := string(rd.timesyncdBytes)
	if !strings.Contains(out, "NTP=pool.ntp.org") || !strings.Contains(out, "PollIntervalMinSec=32") || !strings.Contains(out, "PollIntervalMaxSec=3600") {
		t.Fatalf("timesyncd bytes = %q", out)
	}
	if strings.Contains(out, "NTP=old.example") {
		t.Fatalf("old server not replaced: %q", out)
	}
}

func TestRender_NTPDisabled_NoFileTouch(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "[Time]\nNTP=keep\n", "")
	r, err := a.Render(context.Background(), keyNTP, `{"enabled":false}`)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	rd := r.(rendered)
	if rd.timesyncdBytes != nil {
		t.Fatalf("disabled NTP must not render timesyncd bytes, got %q", rd.timesyncdBytes)
	}
}

func TestRender_UserConfig(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	r, err := a.Render(context.Background(), keyUserConfig, `{"time_format":"24h","sync_mode":"ntp","ntp_interval":3600}`)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	rd := r.(rendered)
	s := string(rd.userConfigBytes)
	if !strings.Contains(s, `"time_format"`) || !strings.Contains(s, `"24h"`) || !strings.Contains(s, `"ntp_interval": 3600`) {
		t.Fatalf("user config bytes = %q", rd.userConfigBytes)
	}
}

func TestRender_Config_AllThree(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "[Time]\n", "")
	r, err := a.Render(context.Background(), keyConfig, `{"timezone":"UTC","time_format":"24h","sync_mode":"ntp","ntp_server":"pool.ntp.org","ntp_interval":3600}`)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	rd := r.(rendered)
	if string(rd.timezoneBytes) != "UTC\n" {
		t.Fatalf("tz = %q", rd.timezoneBytes)
	}
	if !strings.Contains(string(rd.timesyncdBytes), "NTP=pool.ntp.org") {
		t.Fatalf("timesyncd = %q", rd.timesyncdBytes)
	}
	if !strings.Contains(string(rd.userConfigBytes), `"sync_mode"`) || !strings.Contains(string(rd.userConfigBytes), `"ntp"`) {
		t.Fatalf("usercfg = %q", rd.userConfigBytes)
	}
}

func TestRender_Config_ManualSkipsTimesyncd(t *testing.T) {
	a, _, tsPath, _ := newAdapter(t, "", "[Time]\nNTP=keep\n", "")
	r, err := a.Render(context.Background(), keyConfig, `{"timezone":"UTC","sync_mode":"manual"}`)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	rd := r.(rendered)
	if rd.timesyncdBytes != nil {
		t.Fatalf("manual sync_mode must not render timesyncd, got %q", rd.timesyncdBytes)
	}
	// Existing file untouched.
	got, _ := os.ReadFile(tsPath)
	if string(got) != "[Time]\nNTP=keep\n" {
		t.Fatalf("timesyncd file mutated: %q", got)
	}
}

func TestRender_BadJSON(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	for _, key := range []string{keyTimezone, keyNTP, keyUserConfig, keyConfig} {
		if _, err := a.Render(context.Background(), key, `{bad}`); !errors.Is(err, ErrInvalidJSON) {
			t.Fatalf("key %s: want ErrInvalidJSON, got %v", key, err)
		}
	}
}

func TestRender_UnknownKey(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	if _, err := a.Render(context.Background(), "nope", `{}`); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("want ErrUnknownKey, got %v", err)
	}
}

func TestApply_WritesTouchedFilesOnly(t *testing.T) {
	a, tzPath, tsPath, ucPath := newAdapter(t, "", "", "")
	r := rendered{
		timezoneBytes:   []byte("UTC\n"),
		userConfigBytes: []byte(`{"time_format":"24h"}`),
		// timesyncdBytes nil → must not be touched (and must not be created).
	}
	if err := a.Apply(context.Background(), keyConfig, r); err != nil {
		t.Fatalf("apply: %v", err)
	}
	if got, _ := os.ReadFile(tzPath); string(got) != "UTC\n" {
		t.Fatalf("tz = %q", got)
	}
	if got, _ := os.ReadFile(ucPath); string(got) != `{"time_format":"24h"}` {
		t.Fatalf("uc = %q", got)
	}
	if _, err := os.Stat(tsPath); !os.IsNotExist(err) {
		t.Fatalf("untouched timesyncd must not exist: %v", err)
	}
}

func TestApply_BadRenderedType(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	if err := a.Apply(context.Background(), keyTimezone, 123); err == nil {
		t.Fatal("apply wrong type want error")
	}
}

func TestVerify_Match(t *testing.T) {
	a, tzPath, tsPath, ucPath := newAdapter(t, "", "", "")
	// Render + Apply so disk matches what Verify will re-render.
	r, _ := a.Render(context.Background(), keyConfig, `{"timezone":"UTC","sync_mode":"ntp","ntp_server":"pool.ntp.org","ntp_interval":3600}`)
	_ = a.Apply(context.Background(), keyConfig, r)
	if err := a.Verify(context.Background(), keyConfig, `{"timezone":"UTC","sync_mode":"ntp","ntp_server":"pool.ntp.org","ntp_interval":3600}`); err != nil {
		t.Fatalf("verify: %v", err)
	}
	_ = tzPath
	_ = tsPath
	_ = ucPath
}

func TestVerify_NTPDisabled_OKWithoutFile(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	// Disabled NTP renders no timesyncd bytes → Verify skips the file and
	// succeeds even though the file does not exist.
	if err := a.Verify(context.Background(), keyNTP, `{"enabled":false}`); err != nil {
		t.Fatalf("verify disabled ntp: %v", err)
	}
}

func TestVerify_Mismatch(t *testing.T) {
	a, tzPath, _, _ := newAdapter(t, "", "", "")
	_ = os.WriteFile(tzPath, []byte("Europe/Paris\n"), 0644)
	if err := a.Verify(context.Background(), keyTimezone, `{"timezone":"Asia/Shanghai"}`); err == nil {
		t.Fatal("verify mismatched tz want error")
	}
}

func TestVerify_MissingFile(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	if err := a.Verify(context.Background(), keyTimezone, `{"timezone":"UTC"}`); err == nil {
		t.Fatal("verify missing tz want error")
	}
}

func TestRestore_RevertsFiles(t *testing.T) {
	a, tzPath, tsPath, ucPath := newAdapter(t, "", "", "")
	_ = os.WriteFile(tzPath, []byte("NEW"), 0644)
	_ = os.WriteFile(tsPath, []byte("NEW"), 0644)
	_ = os.WriteFile(ucPath, []byte("NEW"), 0644)
	bs := backupState{
		timezoneBytes:   []byte("Asia/Shanghai\n"),
		timesyncdBytes:  []byte("[Time]\nNTP=old\n"),
		userConfigBytes: []byte(`{"time_format":"24h"}`),
	}
	if err := a.Restore(context.Background(), keyConfig, bs); err != nil {
		t.Fatalf("restore: %v", err)
	}
	if got, _ := os.ReadFile(tzPath); string(got) != "Asia/Shanghai\n" {
		t.Fatalf("tz after restore = %q", got)
	}
	if got, _ := os.ReadFile(tsPath); string(got) != "[Time]\nNTP=old\n" {
		t.Fatalf("ts after restore = %q", got)
	}
	if got, _ := os.ReadFile(ucPath); string(got) != `{"time_format":"24h"}` {
		t.Fatalf("uc after restore = %q", got)
	}
}

func TestRestore_RemovesCreatedFiles(t *testing.T) {
	a, tzPath, tsPath, ucPath := newAdapter(t, "", "", "")
	_ = os.WriteFile(tzPath, []byte("NEW"), 0644)
	_ = os.WriteFile(tsPath, []byte("NEW"), 0644)
	_ = os.WriteFile(ucPath, []byte("NEW"), 0644)
	bs := backupState{} // all nil → files removed
	if err := a.Restore(context.Background(), keyConfig, bs); err != nil {
		t.Fatalf("restore: %v", err)
	}
	for _, p := range []string{tzPath, tsPath, ucPath} {
		if _, err := os.Stat(p); !os.IsNotExist(err) {
			t.Fatalf("want %s removed, got %v", p, err)
		}
	}
}

func TestRestore_PerKey(t *testing.T) {
	a, tzPath, tsPath, _ := newAdapter(t, "", "", "")
	_ = os.MkdirAll(filepath.Dir(tsPath), 0755)
	_ = os.WriteFile(tzPath, []byte("NEW"), 0644)
	_ = os.WriteFile(tsPath, []byte("NEW"), 0644)
	if err := a.Restore(context.Background(), keyTimezone, backupState{timezoneBytes: []byte("UTC\n")}); err != nil {
		t.Fatalf("restore tz: %v", err)
	}
	if got, _ := os.ReadFile(tzPath); string(got) != "UTC\n" {
		t.Fatalf("tz = %q", got)
	}
	// tsPath was not part of this key's backup → left as-is ("NEW").
	if got, _ := os.ReadFile(tsPath); string(got) != "NEW" {
		t.Fatalf("untouched ts should remain NEW, got %q", got)
	}
}

func TestRestore_BadBackupType(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	if err := a.Restore(context.Background(), keyTimezone, "nope"); err == nil {
		t.Fatal("restore bad type want error")
	}
}

func TestRestore_UnknownKey(t *testing.T) {
	a, _, _, _ := newAdapter(t, "", "", "")
	if err := a.Restore(context.Background(), "nope", backupState{}); !errors.Is(err, ErrUnknownKey) {
		t.Fatalf("want ErrUnknownKey, got %v", err)
	}
}

func TestUpdateTimesyncdConfig(t *testing.T) {
	cases := []struct {
		name     string
		existing string
		server   string
		interval int
		wantHas  []string
		wantNot  []string
	}{
		{
			name:     "replaces existing NTP and polls",
			existing: "[Time]\nNTP=old\nPollIntervalMinSec=1\nPollIntervalMaxSec=1\n",
			server:   "pool.ntp.org",
			interval: 3600,
			wantHas:  []string{"NTP=pool.ntp.org", "PollIntervalMinSec=32", "PollIntervalMaxSec=3600"},
			wantNot:  []string{"NTP=old"},
		},
		{
			name:     "appends [Time] section when missing",
			existing: "# comment only\n",
			server:   "pool.ntp.org",
			interval: 7200,
			wantHas:  []string{"[Time]", "NTP=pool.ntp.org", "PollIntervalMaxSec=7200"},
		},
		{
			name:     "empty server keeps existing NTP line",
			existing: "[Time]\nNTP=keep.me\n",
			server:   "",
			interval: 3600,
			wantHas:  []string{"NTP=keep.me", "PollIntervalMaxSec=3600"},
		},
		{
			name:     "interval<=min normalized to default",
			existing: "[Time]\n",
			server:   "pool.ntp.org",
			interval: 10,
			wantHas:  []string{"PollIntervalMaxSec=3600", "PollIntervalMinSec=32"},
		},
		{
			name:     "interval<=0 reuses existing max",
			existing: "[Time]\nPollIntervalMaxSec=6500\n",
			server:   "pool.ntp.org",
			interval: 0,
			wantHas:  []string{"PollIntervalMaxSec=6500"},
		},
	}
	for _, tc := range cases {
		out, err := updateTimesyncdConfig([]byte(tc.existing), tc.server, tc.interval)
		if err != nil {
			t.Fatalf("%s: %v", tc.name, err)
		}
		s := string(out)
		for _, w := range tc.wantHas {
			if !strings.Contains(s, w) {
				t.Errorf("%s: want %q in %q", tc.name, w, s)
			}
		}
		for _, w := range tc.wantNot {
			if strings.Contains(s, w) {
				t.Errorf("%s: want %q NOT in %q", tc.name, w, s)
			}
		}
	}
}

func TestNewReturnsAdapter(t *testing.T) {
	if a := New("", "", ""); a == nil {
		t.Fatal("New returned nil")
	}
}

func TestDirOf(t *testing.T) {
	cases := map[string]string{
		"/etc/systemd/timesyncd.conf": "/etc/systemd",
		"/etc/timezone":               "/etc",
		"/foo":                        "/",
		"relative.txt":                ".",
	}
	for in, want := range cases {
		if got := dirOf(in); got != want {
			t.Errorf("dirOf(%q) = %q, want %q", in, got, want)
		}
	}
}
