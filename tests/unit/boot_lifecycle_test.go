package unit

import (
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

func TestOSVerifyDoesNotPullRuntimeBeforeCondition(t *testing.T) {
	raw, err := os.ReadFile("../../systemd/aipc-os-verify.service")
	if err != nil {
		t.Fatal(err)
	}
	unit := string(raw)

	for _, forbidden := range []string{
		"Wants=aipc-autostart.service",
		"Wants=aipc-platform.target",
		"Requires=aipc-autostart.service",
		"Requires=aipc-platform.target",
	} {
		if strings.Contains(unit, forbidden) {
			t.Fatalf("verify unit unconditionally pulls the runtime through %q", forbidden)
		}
	}
	if !strings.Contains(unit, "ExecCondition=/usr/libexec/aipc-os-updater needs-verify") {
		t.Fatal("verify unit is missing the upgrade-state ExecCondition")
	}
	if !strings.Contains(unit, "ExecStartPre=/bin/systemctl restart aipc-autostart.service") {
		t.Fatal("verify unit does not restore the runtime for a real verify boot")
	}
}

func TestFirstbootDoesNotEnableRuntimeServices(t *testing.T) {
	raw, err := os.ReadFile("../../scripts/aipc-firstboot.sh")
	if err != nil {
		t.Fatal(err)
	}
	if strings.Contains(string(raw), "systemctl enable aipc-healthmon.service") {
		t.Fatal("firstboot must not override aipc-cli system disable")
	}
}

func TestPlatformAPIDoesNotLogEveryForwardedEvent(t *testing.T) {
	raw, err := os.ReadFile("../../platform/platform-api/websocket/events.go")
	if err != nil {
		t.Fatal(err)
	}
	for _, noisy := range []string{"Received event: topic=", "Broadcasted event to %d clients"} {
		if strings.Contains(string(raw), noisy) {
			t.Fatalf("per-event log remains enabled under the default debug level: %q", noisy)
		}
	}
}

func TestBootPrepMCURTCUsesExactEpochWindow(t *testing.T) {
	raw, err := os.ReadFile("../../hal_v2/examples/ne503_boot_prep/ne503_boot_prep.cpp")
	if err != nil {
		t.Fatal(err)
	}
	source := string(raw)
	for _, want := range []string{
		"1704067200LL",
		"2208988800LL",
		"sec < min_plausible_ts || sec > max_plausible_ts",
		"const struct tm requested_tm = tm",
		"tm.tm_mday != requested_tm.tm_mday",
	} {
		if !strings.Contains(source, want) {
			t.Fatalf("MCU RTC pre-set validation is missing %q", want)
		}
	}
	if strings.Index(source, "sec < min_plausible_ts") > strings.Index(source, "clock_settime(CLOCK_REALTIME") {
		t.Fatal("MCU RTC epoch validation occurs after clock_settime")
	}
}

func TestTimeSaveScriptIsPackagedAndValidated(t *testing.T) {
	checks := map[string]string{
		"../../Makefile":                             `scripts/aipc-time-save.sh "$(STAGE_DIR)/opt/aipc/scripts/aipc-time-save.sh"`,
		"../../scripts/install.sh":                   `scripts/aipc-time-save.sh ${INSTALL_PREFIX}/scripts/aipc-time-save.sh`,
		"../../scripts/aipc-install-current-root.sh": `$DATA_ROOT/scripts/aipc-time-save.sh`,
		"../../scripts/aipc-restore.sh":              `/data/aipc/scripts/aipc-time-save.sh`,
		"../../scripts/deploy.sh":                    `aipc-time-save.sh`,
	}
	for path, want := range checks {
		raw, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if !strings.Contains(string(raw), want) {
			t.Fatalf("%s does not package or validate %q", path, want)
		}
	}
}

func TestDirectInstallerEnablesTimeSaver(t *testing.T) {
	raw, err := os.ReadFile("../../scripts/install.sh")
	if err != nil {
		t.Fatal(err)
	}
	if strings.Count(string(raw), "aipc-os-verify.service aipc-time-save.service") != 2 {
		t.Fatal("direct installer does not enable time saver in both boot ownership modes")
	}
}

func TestReleaseValidatorsRejectEmptyTLSCredentials(t *testing.T) {
	for _, path := range []string{
		"../../scripts/deploy.sh",
		"../../scripts/aipc-install-current-root.sh",
		"../../scripts/aipc-restore.sh",
	} {
		raw, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if strings.Contains(string(raw), "etc/ssl/*") {
			t.Fatalf("%s still exempts empty TLS credentials", path)
		}
	}
}

func TestFirstbootAllowsLargeContainerdMigrationToFinish(t *testing.T) {
	raw, err := os.ReadFile("../../systemd/aipc-firstboot.service")
	if err != nil {
		t.Fatal(err)
	}
	unit := string(raw)
	if !strings.Contains(unit, "TimeoutStartSec=0") || strings.Contains(unit, "TimeoutStartSec=180") {
		t.Fatal("firstboot still imposes a fixed deadline on containerd migration")
	}
}

func TestTimeSaveUsesExactWindowAndUniqueTemporaryFile(t *testing.T) {
	script := "../../scripts/aipc-time-save.sh"
	raw, err := os.ReadFile(script)
	if err != nil {
		t.Fatal(err)
	}
	for _, want := range []string{
		"MIN_PLAUSIBLE_TS=1704067200",
		"MAX_PLAUSIBLE_TS=2208988800",
		`mktemp "${DEST}.tmp.XXXXXX"`,
	} {
		if !strings.Contains(string(raw), want) {
			t.Fatalf("time saver is missing %q", want)
		}
	}

	fakeBin := t.TempDir()
	dateStub := `#!/bin/sh
if [ "$1" = "+%s" ]; then
    printf '%s\n' "$FAKE_TS"
else
    printf '%s\n' '2024-01-01T00:00:00Z'
fi
`
	if err := os.WriteFile(filepath.Join(fakeBin, "date"), []byte(dateStub), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(filepath.Join(fakeBin, "timedatectl"), []byte("#!/bin/sh\nprintf 'UTC\\n'\n"), 0755); err != nil {
		t.Fatal(err)
	}

	destination := filepath.Join(t.TempDir(), "last-known-time.json")
	fixedTmp := destination + ".tmp"
	if err := os.WriteFile(destination, []byte("known-good"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(fixedTmp, []byte("platform-api-writer"), 0644); err != nil {
		t.Fatal(err)
	}
	run := func(ts string) {
		t.Helper()
		cmd := exec.Command("bash", script)
		cmd.Env = append(os.Environ(),
			"PATH="+fakeBin+":"+os.Getenv("PATH"),
			"FAKE_TS="+ts,
			"AIPC_TIME_SAVE_DEST="+destination,
		)
		if output, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("time saver failed: %v\n%s", err, output)
		}
	}

	run("2208988801")
	got, err := os.ReadFile(destination)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "known-good" {
		t.Fatalf("out-of-range clock overwrote snapshot: %q", got)
	}

	run("1704067200")
	got, err = os.ReadFile(destination)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(got), `"unix_timestamp": 1704067200`) {
		t.Fatalf("valid clock was not persisted: %q", got)
	}
	fixed, err := os.ReadFile(fixedTmp)
	if err != nil {
		t.Fatal(err)
	}
	if string(fixed) != "platform-api-writer" {
		t.Fatalf("time saver collided with platform-api temporary file: %q", fixed)
	}
	leftovers, err := filepath.Glob(destination + ".tmp.*")
	if err != nil {
		t.Fatal(err)
	}
	if len(leftovers) != 0 {
		t.Fatalf("unique temporary files remain: %v", leftovers)
	}
}

func TestFirstbootCreatesLoginRecordTargetsBeforeBind(t *testing.T) {
	raw, err := os.ReadFile("../../scripts/aipc-firstboot.sh")
	if err != nil {
		t.Fatal(err)
	}
	script := string(raw)
	create := strings.Index(script, `: >"$ut_dst"`)
	mount := strings.Index(script, `mount -o bind "$ut_src" "$ut_dst"`)
	if create < 0 || mount < 0 || create > mount {
		t.Fatal("firstboot must create volatile login-record targets before bind mounting")
	}
}

func TestFirstbootBindsLoginRecordsBeforeExposingGetty(t *testing.T) {
	raw, err := os.ReadFile("../../scripts/aipc-firstboot.sh")
	if err != nil {
		t.Fatal(err)
	}
	script := string(raw)
	bind := strings.Index(script, `mount -o bind "$ut_src" "$ut_dst"`)
	getty := strings.Index(script, `getty_unit="serial-getty@`)
	if bind < 0 || getty < 0 || bind > getty {
		t.Fatal("firstboot must bind persistent login records before enabling the serial getty; logins during the containerd migration window would otherwise be hidden by the bind mount")
	}
	if !strings.Contains(script, `cat "$ut_dst" >>"$ut_src"`) {
		t.Fatal("firstboot must merge this boot's volatile wtmp records into the persistent copy before bind mounting")
	}
}

func TestFirstbootDoesNotPersistLoginRecordsThroughFstab(t *testing.T) {
	raw, err := os.ReadFile("../../scripts/aipc-firstboot.sh")
	if err != nil {
		t.Fatal(err)
	}
	script := string(raw)
	// The journal bind may stay: it targets a directory, and systemd creates
	// missing mountpoint directories before starting fstab mount units. The
	// login records are files, which nothing creates before local-fs.target.
	if strings.Contains(script, `echo "${ut_src} ${ut_dst} none bind 0 0"`) {
		t.Fatal("firstboot must not append login-record file bind entries to /etc/fstab: /var/log is an empty tmpfs when local-fs.target runs the fstab mounts and systemd only auto-creates mountpoint directories, so file binds fail with ENOENT and the next boot lands in emergency mode; the runtime bind already provides the persistence")
	}
	if !strings.Contains(script, `/data/aipc-data/log/\(wtmp\|lastlog\)`) {
		t.Fatal("firstboot must clean stale login-record fstab entries appended by earlier builds; without the heal, devices that took those builds keep failing local-fs.target after the runtime upgrade")
	}
}

func TestRestoreValidationToleratesPreviousRuntimeRelease(t *testing.T) {
	raw, err := os.ReadFile("../../scripts/aipc-restore.sh")
	if err != nil {
		t.Fatal(err)
	}
	script := string(raw)
	start := strings.Index(script, "validate_restored_release() {")
	if start < 0 {
		t.Fatal("validate_restored_release not found")
	}
	body := script[start:]
	hard := body
	if idx := strings.Index(body, "while IFS"); idx >= 0 {
		hard = body[:idx]
	}
	if strings.Contains(hard, "aipc-time-save.sh") {
		t.Fatal("the hard executable whitelist must not require the v1.2.0-only time saver; an OS A/B upgrade booting an older runtime would roll back a healthy upgrade")
	}
	if !strings.Contains(body, "RESTORE_LEGACY_LAYOUT") {
		t.Fatal("restore must warn when the restored runtime predates the shutdown time saver")
	}
}

func TestFirstbootContainerdMigrationPreservesPersistentMetadata(t *testing.T) {
	raw, err := os.ReadFile("../../scripts/aipc-firstboot.sh")
	if err != nil {
		t.Fatal(err)
	}
	script := string(raw)
	start := strings.Index(script, "recover_pending_containerd_migration() {")
	if start < 0 {
		t.Fatal("could not find containerd migration helpers in firstboot script")
	}
	endMarker := "\nconfigure_containerd_root() {"
	end := strings.Index(script[start:], endMarker)
	if end < 0 {
		t.Fatal("could not extract containerd migration helpers from firstboot script")
	}
	functions := script[start : start+end]

	t.Run("persistent destination wins over fresh rootfs", func(t *testing.T) {
		tmp := t.TempDir()
		source := filepath.Join(tmp, "rootfs-containerd")
		destination := filepath.Join(tmp, "persistent-containerd")
		for _, dir := range []string{source, destination} {
			if err := os.MkdirAll(filepath.Join(dir, "io.containerd.metadata.v1.bolt"), 0755); err != nil {
				t.Fatal(err)
			}
		}
		sourceMeta := filepath.Join(source, "io.containerd.metadata.v1.bolt", "meta.db")
		destinationMeta := filepath.Join(destination, "io.containerd.metadata.v1.bolt", "meta.db")
		if err := os.WriteFile(sourceMeta, []byte("fresh-empty-index"), 0644); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(destinationMeta, []byte("persistent-image-index"), 0644); err != nil {
			t.Fatal(err)
		}

		runFirstbootMigration(t, functions, source, destination, source)

		got, err := os.ReadFile(destinationMeta)
		if err != nil {
			t.Fatal(err)
		}
		if string(got) != "persistent-image-index" {
			t.Fatalf("persistent meta.db was overwritten: got %q", got)
		}
		if _, err := os.Stat(sourceMeta); err != nil {
			t.Fatalf("authoritative source was removed before config cutover: %v", err)
		}
		if _, err := os.Stat(destination + ".migration-pending"); !os.IsNotExist(err) {
			t.Fatalf("pre-existing destination unexpectedly marked pending: %v", err)
		}
	})

	t.Run("legacy partial destination is moved aside and reseeded", func(t *testing.T) {
		tmp := t.TempDir()
		source := filepath.Join(tmp, "rootfs-containerd")
		destination := filepath.Join(tmp, "persistent-containerd")
		// A complete source store carries real image content.
		blobs := filepath.Join(source, "io.containerd.content.v1.content", "blobs", "sha256")
		if err := os.MkdirAll(blobs, 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(blobs, "layer"), []byte("complete-store"), 0644); err != nil {
			t.Fatal(err)
		}
		if err := os.MkdirAll(filepath.Join(source, "io.containerd.metadata.v1.bolt"), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(source, "io.containerd.metadata.v1.bolt", "meta.db"), []byte("complete-index"), 0644); err != nil {
			t.Fatal(err)
		}
		// A pre-staging firstboot killed mid cp -a leaves a partial
		// destination with no migration marker and the config still on the
		// source.
		if err := os.MkdirAll(filepath.Join(destination, "io.containerd.metadata.v1.bolt"), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(destination, "io.containerd.metadata.v1.bolt", "meta.db"), []byte("partial-index"), 0644); err != nil {
			t.Fatal(err)
		}

		runFirstbootMigration(t, functions, source, destination, source)

		got, err := os.ReadFile(filepath.Join(destination, "io.containerd.metadata.v1.bolt", "meta.db"))
		if err != nil || string(got) != "complete-index" {
			t.Fatalf("destination was not reseeded from the complete source: %q, %v", got, err)
		}
		legacy, err := filepath.Glob(destination + ".legacy-partial.*")
		if err != nil || len(legacy) != 1 {
			t.Fatalf("the partial copy must be preserved aside, got %v (err %v)", legacy, err)
		}
		partial, err := os.ReadFile(filepath.Join(legacy[0], "io.containerd.metadata.v1.bolt", "meta.db"))
		if err != nil || string(partial) != "partial-index" {
			t.Fatalf("the moved-aside partial copy changed: %q, %v", partial, err)
		}
	})

	t.Run("committed destination survives a contentful source", func(t *testing.T) {
		tmp := t.TempDir()
		source := filepath.Join(tmp, "rootfs-containerd")
		destination := filepath.Join(tmp, "persistent-containerd")
		blobs := filepath.Join(source, "io.containerd.content.v1.content", "blobs", "sha256")
		if err := os.MkdirAll(blobs, 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(blobs, "layer"), []byte("complete-store"), 0644); err != nil {
			t.Fatal(err)
		}
		if err := os.MkdirAll(destination, 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(destination, "committed"), []byte("persistent-state"), 0644); err != nil {
			t.Fatal(err)
		}

		// The configured root names the destination, so it is committed
		// state no matter how much content the source carries.
		runFirstbootMigration(t, functions, source, destination, destination)

		got, err := os.ReadFile(filepath.Join(destination, "committed"))
		if err != nil || string(got) != "persistent-state" {
			t.Fatalf("committed destination was disturbed: %q, %v", got, err)
		}
		if legacy, _ := filepath.Glob(destination + ".legacy-partial.*"); len(legacy) != 0 {
			t.Fatalf("committed destination must never be triaged aside: %v", legacy)
		}
	})

	t.Run("duplicate second pass preserves the just-published store", func(t *testing.T) {
		tmp := t.TempDir()
		source := filepath.Join(tmp, "rootfs-containerd")
		destination := filepath.Join(tmp, "persistent-containerd")
		// A complete source store with real image content, like the stock
		// /var/lib/containerd after images were pulled under it.
		blobs := filepath.Join(source, "io.containerd.content.v1.content", "blobs", "sha256")
		if err := os.MkdirAll(blobs, 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(blobs, "layer"), []byte("complete-store"), 0644); err != nil {
			t.Fatal(err)
		}
		if err := os.MkdirAll(filepath.Join(source, "io.containerd.metadata.v1.bolt"), 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(source, "io.containerd.metadata.v1.bolt", "meta.db"), []byte("complete-index"), 0644); err != nil {
			t.Fatal(err)
		}
		// configure_containerd_root runs recovery once, then the migration
		// helper twice with the same default source (current_root resolves
		// to /var/lib/containerd). The second pass must honor the pending
		// marker left by the first and keep the store it just published
		// instead of triaging it aside and re-copying the multi-GB source.
		command := "set -euo pipefail\nwarn() { :; }\nlog() { :; }\n" + functions +
			"\nrecover_pending_containerd_migration \"$2\" \"$3\"\n" +
			"migrate_containerd_root \"$1\" \"$2\" \"$3\"\n" +
			"migrate_containerd_root \"$1\" \"$2\" \"$3\""
		cmd := exec.Command("bash", "-c", command, "migration-duplicate-pass", source, destination, source)
		if output, err := cmd.CombinedOutput(); err != nil {
			t.Fatalf("duplicate migration pass failed: %v\n%s", err, output)
		}
		got, err := os.ReadFile(filepath.Join(destination, "io.containerd.metadata.v1.bolt", "meta.db"))
		if err != nil || string(got) != "complete-index" {
			t.Fatalf("just-published destination was disturbed: %q, %v", got, err)
		}
		if legacy, _ := filepath.Glob(destination + ".legacy-partial.*"); len(legacy) != 0 {
			t.Fatalf("duplicate pass triaged the just-published store aside: %v", legacy)
		}
		if _, err := os.Stat(destination + ".migration-pending"); err != nil {
			t.Fatalf("pending marker missing after publication: %v", err)
		}
	})

	t.Run("interrupted seed is refreshed from authoritative source", func(t *testing.T) {
		tmp := t.TempDir()
		source := filepath.Join(tmp, "old-containerd")
		destination := filepath.Join(tmp, "persistent-containerd")
		if err := os.MkdirAll(source, 0755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(source, "seed"), []byte("old-state"), 0644); err != nil {
			t.Fatal(err)
		}

		runFirstbootMigration(t, functions, source, destination, source)

		got, err := os.ReadFile(filepath.Join(destination, "seed"))
		if err != nil {
			t.Fatal(err)
		}
		if string(got) != "old-state" {
			t.Fatalf("destination was not seeded: got %q", got)
		}
		if _, err := os.Stat(filepath.Join(source, "seed")); err != nil {
			t.Fatalf("source removed before config cutover: %v", err)
		}
		if _, err := os.Stat(destination + ".migration-pending"); err != nil {
			t.Fatalf("seed publication missing pending marker: %v", err)
		}

		// Simulate firstboot being interrupted after destination publication but before
		// the atomic config rename. containerd continues on source and writes
		// newer metadata; the next boot must discard and refresh the stale seed.
		if err := os.WriteFile(filepath.Join(source, "seed"), []byte("newer-source-state"), 0644); err != nil {
			t.Fatal(err)
		}
		runFirstbootMigration(t, functions, source, destination, source)
		got, err = os.ReadFile(filepath.Join(destination, "seed"))
		if err != nil {
			t.Fatal(err)
		}
		if string(got) != "newer-source-state" {
			t.Fatalf("interrupted seed was treated as authoritative: got %q", got)
		}

		// Once the config commit points at destination, recovery keeps the
		// published tree and only clears the stale marker.
		runFirstbootRecovery(t, functions, destination, destination)
		if _, err := os.Stat(destination + ".migration-pending"); !os.IsNotExist(err) {
			t.Fatalf("committed migration marker not cleared: %v", err)
		}
		got, err = os.ReadFile(filepath.Join(destination, "seed"))
		if err != nil || string(got) != "newer-source-state" {
			t.Fatalf("committed destination changed during recovery: %q, %v", got, err)
		}

		// Compatibility path replacement happens only after config commit.
		runFirstbootLink(t, functions, source, destination)
		assertSymlinkTarget(t, source, destination)
	})

	t.Run("copy failure keeps source authoritative", func(t *testing.T) {
		tmp := t.TempDir()
		source := filepath.Join(tmp, "rootfs-containerd")
		destination := filepath.Join(tmp, "persistent-containerd")
		if err := os.MkdirAll(source, 0755); err != nil {
			t.Fatal(err)
		}
		sourceData := filepath.Join(source, "meta.db")
		if err := os.WriteFile(sourceData, []byte("only-complete-copy"), 0644); err != nil {
			t.Fatal(err)
		}

		command := "set -euo pipefail\nwarn() { :; }\nlog() { :; }\ncp() { return 1; }\n" + functions +
			"\nmigrate_containerd_root \"$1\" \"$2\""
		cmd := exec.Command("bash", "-c", command, "migration-copy-failure", source, destination)
		if output, err := cmd.CombinedOutput(); err == nil {
			t.Fatalf("migration succeeded despite copy failure:\n%s", output)
		}
		got, err := os.ReadFile(sourceData)
		if err != nil || string(got) != "only-complete-copy" {
			t.Fatalf("copy failure damaged source: %q, %v", got, err)
		}
		if entries, err := os.ReadDir(destination); err != nil || len(entries) != 0 {
			t.Fatalf("copy failure published destination: entries=%v err=%v", entries, err)
		}
		if _, err := os.Stat(destination + ".migration-pending"); !os.IsNotExist(err) {
			t.Fatalf("copy failure left migration marker: %v", err)
		}
	})
}

func TestFirstbootContainerdConfigCommitPreservesExistingConfig(t *testing.T) {
	raw, err := os.ReadFile("../../scripts/aipc-firstboot.sh")
	if err != nil {
		t.Fatal(err)
	}
	script := string(raw)
	start := strings.Index(script, "containerd_set_config_value() {")
	end := strings.Index(script[start:], "\nrecover_pending_containerd_migration() {")
	if start < 0 || end < 0 {
		t.Fatal("could not extract containerd config helpers")
	}
	functions := script[start : start+end]

	tmp := t.TempDir()
	config := filepath.Join(tmp, "config.toml")
	original := "version = 1\nroot = \"/var/lib/containerd\"\nstate = \"/run/old\"\n[plugins]\n  keep = true\n"
	if err := os.WriteFile(config, []byte(original), 0640); err != nil {
		t.Fatal(err)
	}
	target := filepath.Join(tmp, "persistent-containerd")
	command := "set -euo pipefail\n" + functions + "\ncontainerd_commit_config \"$1\" \"$2\""
	cmd := exec.Command("bash", "-c", command, "config-test", target, config)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("containerd config commit failed: %v\n%s", err, output)
	}

	gotBytes, err := os.ReadFile(config)
	if err != nil {
		t.Fatal(err)
	}
	got := string(gotBytes)
	for _, want := range []string{
		`root = "` + target + `"`,
		`state = "/run/containerd"`,
		`version = 2`,
		`[plugins]`,
		`keep = true`,
	} {
		if !strings.Contains(got, want) {
			t.Fatalf("committed config missing %q:\n%s", want, got)
		}
	}
	info, err := os.Stat(config)
	if err != nil {
		t.Fatal(err)
	}
	if info.Mode().Perm() != 0640 {
		t.Fatalf("config mode changed to %o, want 0640", info.Mode().Perm())
	}
}

func runFirstbootMigration(t *testing.T, functions, source, destination, configuredRoot string) {
	t.Helper()
	command := "set -euo pipefail\nwarn() { :; }\nlog() { :; }\n" + functions +
		"\nrecover_pending_containerd_migration \"$2\" \"$3\"\nmigrate_containerd_root \"$1\" \"$2\" \"$3\""
	cmd := exec.Command("bash", "-c", command, "migration-test", source, destination, configuredRoot)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("migration failed: %v\n%s", err, output)
	}
}

func runFirstbootRecovery(t *testing.T, functions, destination, configuredRoot string) {
	t.Helper()
	command := "set -euo pipefail\nwarn() { :; }\nlog() { :; }\n" + functions +
		"\nrecover_pending_containerd_migration \"$1\" \"$2\""
	cmd := exec.Command("bash", "-c", command, "migration-test", destination, configuredRoot)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("migration recovery failed: %v\n%s", err, output)
	}
}

func runFirstbootLink(t *testing.T, functions, source, destination string) {
	t.Helper()
	command := "set -euo pipefail\nwarn() { :; }\nlog() { :; }\n" + functions +
		"\nensure_containerd_varlib_link \"$2\" \"$1\""
	cmd := exec.Command("bash", "-c", command, "migration-test", source, destination)
	if output, err := cmd.CombinedOutput(); err != nil {
		t.Fatalf("containerd compatibility link failed: %v\n%s", err, output)
	}
}

func assertSymlinkTarget(t *testing.T, link, want string) {
	t.Helper()
	got, err := filepath.EvalSymlinks(link)
	if err != nil {
		t.Fatal(err)
	}
	want, err = filepath.EvalSymlinks(want)
	if err != nil {
		t.Fatal(err)
	}
	if got != want {
		t.Fatalf("symlink target = %q, want %q", got, want)
	}
}

func TestCurrentRootInstallerPreservesRuntimeDisable(t *testing.T) {
	raw, err := os.ReadFile("../../scripts/aipc-install-current-root.sh")
	if err != nil {
		t.Fatal(err)
	}
	script := string(raw)

	if !strings.Contains(script, `unit_enable_state["$name"]="$("$SYSTEMCTL" is-enabled "$name"`) {
		t.Fatal("current-root installer does not snapshot existing unit enable state")
	}
	if !strings.Contains(script, `disabled|masked|masked-runtime)`) {
		t.Fatal("current-root installer does not suppress explicitly disabled boot units")
	}
	if strings.Contains(script, "        aipc-platform.target\n") {
		t.Fatal("current-root installer must not explicitly start aipc-platform.target")
	}
}
