package auth

import (
	"strings"
	"testing"
	"time"
)

// rollbackProbeArgs builds the full systemd-run argv. The script is the last
// element. These tests pin the flag shape (so --collect/--unit stay present)
// and the script's behavior (wait/probe/restore semantics) without systemd.

func TestRollbackProbeArgs_FlagShape(t *testing.T) {
	args := rollbackProbeArgs("/run/aipc-auth-backup.yaml", "/data/aipc/etc/platform-api.yaml", "platform-api", 8, 30)

	if args[0] != "systemd-run" {
		t.Errorf("argv[0] = %q, want systemd-run", args[0])
	}
	joined := strings.Join(args, " ")
	for _, want := range []string{"--quiet", "--collect", "--unit=" + probeUnit, "/bin/sh", "-c"} {
		if !strings.Contains(joined, want) {
			t.Errorf("argv missing %q in %q", want, joined)
		}
	}
	// The script is the final element.
	if args[len(args)-1] == "" {
		t.Errorf("script (last argv element) is empty")
	}
}

func TestRollbackProbeArgs_LastElementIsScript(t *testing.T) {
	backup, config, service := "/run/b.yaml", "/data/c.yaml", "platform-api"
	args := rollbackProbeArgs(backup, config, service, 8, 30)
	if args[len(args)-1] != rollbackProbeScript(backup, config, service, 8, 30) {
		t.Errorf("last argv element is not rollbackProbeScript(...)")
	}
}

func TestRollbackProbeScript_ContainsWaitAndProbe(t *testing.T) {
	s := rollbackProbeScript("/run/b.yaml", "/data/c.yaml", "platform-api", 8, 30)
	if !strings.Contains(s, "sleep 8") {
		t.Errorf("script missing initial sleep 8: %q", s)
	}
	// probeSec appears as the while bound.
	if !strings.Contains(s, "-lt 30") {
		t.Errorf("script missing probe window -lt 30: %q", s)
	}
}

func TestRollbackProbeScript_PollsServiceActive(t *testing.T) {
	s := rollbackProbeScript("/run/b.yaml", "/data/c.yaml", "platform-api", 8, 30)
	if !strings.Contains(s, "systemctl is-active \"platform-api\"") {
		t.Errorf("script missing `systemctl is-active \"platform-api\"` poll: %q", s)
	}
	if !strings.Contains(s, `"active"`) {
		t.Errorf("script missing active comparison: %q", s)
	}
}

func TestRollbackProbeScript_RestoresOnFailure(t *testing.T) {
	s := rollbackProbeScript("/run/b.yaml", "/data/c.yaml", "platform-api", 8, 30)
	// On failure path: restore backup over config + restart + log + cleanup.
	for _, want := range []string{
		`cp "/run/b.yaml" "/data/c.yaml"`,
		`systemctl restart "platform-api"`,
		`logger -t aipc-auth-rollback`,
		`rm -f "/run/b.yaml"`,
	} {
		if !strings.Contains(s, want) {
			t.Errorf("script missing restore-path fragment %q in: %q", want, s)
		}
	}
}

func TestRollbackProbeScript_SuccessPathRemovesBackup(t *testing.T) {
	s := rollbackProbeScript("/run/b.yaml", "/data/c.yaml", "platform-api", 8, 30)
	if !strings.Contains(s, `rm -f "/run/b.yaml"; exit 0`) {
		t.Errorf("script missing success-path backup cleanup + exit 0: %q", s)
	}
}

func TestRollbackProbeScript_QuotingIsShellSafe(t *testing.T) {
	// Paths/service are inserted via %q (Go double-quoted). A path with a space
	// must arrive quoted so the shell treats it as one token.
	s := rollbackProbeScript("/run/aipc auth backup.yaml", "/data/aipc/etc/platform-api.yaml", "platform-api", 8, 30)
	if !strings.Contains(s, `"/run/aipc auth backup.yaml"`) {
		t.Errorf("space-containing backup path not Go-quoted: %q", s)
	}
}

func TestRollbackProbeScript_GuardedOnBackupExisting(t *testing.T) {
	s := rollbackProbeScript("/run/b.yaml", "/data/c.yaml", "platform-api", 8, 30)
	// The restore branch must be guarded on the backup file existing (a prior
	// probe may have cleaned it up), else cp would fail.
	if !strings.Contains(s, `if [ -f "/run/b.yaml" ]; then`) {
		t.Errorf("script missing backup-existence guard: %q", s)
	}
}

func TestScheduleAuthRollbackProbe_NonBlocking(t *testing.T) {
	// ScheduleAuthRollbackProbe launches systemd-run via cmd.Start() (non-blocking).
	// On a non-systemd dev box systemd-run is absent, so Start returns a PATH
	// lookup error — the expected, harmless failure mode. We only assert the
	// function is callable and does not hang or panic; the error is discarded.
	done := make(chan error, 1)
	go func() {
		done <- ScheduleAuthRollbackProbe("/run/none.yaml", "/data/none.yaml", "platform-api")
	}()
	select {
	case <-done:
		// returned (err may be nil or a PathError) — pass
	case <-time.After(2 * time.Second):
		t.Fatal("ScheduleAuthRollbackProbe blocked longer than 2s (should be non-blocking)")
	}
}
