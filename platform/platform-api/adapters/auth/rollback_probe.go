package auth

import (
	"fmt"
	"os/exec"
)

// R-auth-rollback probe.
//
// Changing platform-api's own auth config + restarting platform-api is the one
// unrecoverable failure mode in the Config Controller: if the new config breaks
// startup, the API never comes back, and the operator cannot reach it to revert.
// The adapter's byte-Verify cannot catch a semantically-broken config (it only
// checks the file was written correctly), so the handler schedules THIS probe
// after Apply.
//
// The probe runs as a detached `systemd-run` transient unit (its own cgroup /
// scope) so it survives the platform-api stop+start that the restart performs.
// It waits for the restart to settle, then polls `systemctl is-active
// platform-api` once per second. If the service becomes active, the staged
// backup is removed (cleanup) and the probe exits. If the service never becomes
// active within the window, the pre-Apply backup is restored over the config
// and the service is restarted again so the known-good config takes effect.
//
// Robustness: the restore path is guarded on the backup file existing (a prior
// probe may have cleaned it up). The probe triggers only on SUSTAINED
// non-active (default 30s), so a slow-but-healthy restart is not mis-flagged.
// On a false trigger (ambient outage unrelated to the auth change) the worst
// case is the OLD, known-good config is restored — not a lockout; the operator
// re-applies the password change once the ambient fault clears.

const (
	// defaultWaitSec is how long the probe sleeps before first polling, letting
	// the 2s restart goroutine fire and the service complete a stop+start cycle.
	defaultWaitSec = 8
	// defaultProbeSec is the sustained-failure window: the service must be
	// non-active for this many consecutive seconds before the backup is restored.
	defaultProbeSec = 30
	// probeUnit is the fixed transient-unit name. A fixed name prevents probe
	// stacking across rapid successive changes; if a probe is already running,
	// a second systemd-run for the same unit name fails harmlessly and the
	// in-flight probe still validates the service after the latest restart.
	probeUnit = "aipc-auth-rollback"
)

// rollbackProbeScript builds the shell body the transient unit runs. It is pure
// string construction so it can be unit-tested without systemd. service, paths
// are inserted via %q (Go-quoted, shell-compatible double-quoted strings), which
// is safe for the plain paths and unit names used here.
func rollbackProbeScript(backupPath, configPath, service string, waitSec, probeSec int) string {
	return fmt.Sprintf(`sleep %d
ok=0
i=0
while [ "$i" -lt %d ]; do
  if [ "$(systemctl is-active %q)" = "active" ]; then ok=1; break; fi
  i=$((i+1))
  sleep 1
done
if [ "$ok" = "1" ]; then rm -f %q; exit 0; fi
if [ -f %q ]; then
  cp %q %q
  systemctl restart %q
  logger -t aipc-auth-rollback "restored %s from %s after %s failed to come up"
  rm -f %q
fi
`, waitSec, probeSec, service, backupPath, backupPath, backupPath, configPath, service, configPath, backupPath, service, backupPath)
}

// rollbackProbeArgs returns the full argv for `systemd-run` of the detached
// rollback probe: argv[0] = "systemd-run", then the flags, then /bin/sh -c
// <script>. --collect removes the transient unit after it completes; --unit
// fixes the name (see probeUnit). --quiet suppresses the "Running as unit"
// notice.
func rollbackProbeArgs(backupPath, configPath, service string, waitSec, probeSec int) []string {
	return []string{
		"systemd-run",
		"--quiet",
		"--collect",
		"--unit=" + probeUnit,
		"--description=AIPC auth config rollback probe",
		"/bin/sh",
		"-c",
		rollbackProbeScript(backupPath, configPath, service, waitSec, probeSec),
	}
}

// ScheduleAuthRollbackProbe launches the detached rollback probe as a best-effort
// background transient unit. It is non-blocking and never returns an error that
// the caller must handle beyond logging: if systemd-run is unavailable (non-
// systemd dev env) or a probe is already running (fixed unit name), the restart
// still happens via the handler's existing restart goroutine — only the
// auto-rollback safety net is skipped.
func ScheduleAuthRollbackProbe(backupPath, configPath, service string) error {
	args := rollbackProbeArgs(backupPath, configPath, service, defaultWaitSec, defaultProbeSec)
	cmd := exec.Command(args[0], args[1:]...)
	return cmd.Start()
}
