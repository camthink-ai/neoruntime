package osupgrade

import (
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

func TestInstallFailureKeepsCurrentCopy(t *testing.T) {
	runner, store, setLog := testRunner(t, "exit 7")
	err := runner.Install()
	if err == nil {
		t.Fatal("expected install failure")
	}
	job, loadErr := store.Active()
	if loadErr != nil {
		t.Fatal(loadErr)
	}
	if job.State != StateFailed {
		t.Fatalf("expected failed state, got %s", job.State)
	}
	if _, statErr := os.Lstat(filepath.Join(runner.BackupRoot, "current")); !os.IsNotExist(statErr) {
		t.Fatalf("failed install left backup selected: %v", statErr)
	}
	data, _ := os.ReadFile(setLog)
	if got := strings.TrimSpace(string(data)); got != "a" {
		t.Fatalf("failed install selected %q, want current copy a", got)
	}
}

func TestInstallSuccessSelectsInactiveCopy(t *testing.T) {
	runner, store, setLog := testRunner(t, `echo "50%"; exit 0`)
	if err := runner.Install(); err != nil {
		t.Fatal(err)
	}
	job, err := store.Active()
	if err != nil {
		t.Fatal(err)
	}
	if job.State != StateAwaitingReboot || job.TargetCopy != "B" || !job.RebootRequired {
		t.Fatalf("unexpected successful job: %+v", job)
	}
	data, _ := os.ReadFile(setLog)
	if got := strings.TrimSpace(string(data)); got != "b" {
		t.Fatalf("successful install selected %q, want inactive copy b", got)
	}
}

func TestInstallStandardUpgradeUsesTargetCopyMode(t *testing.T) {
	runner, store, _ := testRunner(t, "exit 0")
	argsLog := filepath.Join(filepath.Dir(runner.SWUpdateBinary), "swupdate.args")
	writeExecutable(t, runner.SWUpdateBinary, "#!/bin/sh\necho \"$@\" > \""+argsLog+"\"\nexit 0\n")
	job, err := store.Active()
	if err != nil {
		t.Fatal(err)
	}
	job.SupportsStandardUpgrade = true
	job.SupportsFullUpgrade = true
	job.UpgradeStrategy = string(UpgradeStrategyStandard)
	if err := store.Save(job); err != nil {
		t.Fatal(err)
	}

	if err := runner.Install(); err != nil {
		t.Fatal(err)
	}
	args, err := os.ReadFile(argsLog)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(args), "stable,copy-b") {
		t.Fatalf("standard upgrade did not use target copy mode:\n%s", args)
	}
}

func TestSingleRecoveryStandardUpgradeUsesCopyA(t *testing.T) {
	job := Job{
		UpgradeMode:             LayoutSingle,
		UpgradeStrategy:         string(UpgradeStrategyStandard),
		SupportsStandardUpgrade: true,
	}
	mode, err := job.SWUpdateMode("A")
	if err != nil {
		t.Fatal(err)
	}
	if mode != "copy-a" {
		t.Fatalf("unexpected single-recovery standard mode %q", mode)
	}
}

func TestInstallRejectsPackageChangedAfterValidation(t *testing.T) {
	runner, store, _ := testRunner(t, "exit 0")
	job, err := store.Active()
	if err != nil {
		t.Fatal(err)
	}
	job.SHA256, err = fileSHA256(job.PackagePath)
	if err != nil {
		t.Fatal(err)
	}
	if err := store.Save(job); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(job.PackagePath, []byte("changed"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := runner.Install(); err == nil || !strings.Contains(err.Error(), "changed after validation") {
		t.Fatalf("expected package mutation failure, got %v", err)
	}
}

func TestInstallRejectsMissingTargetPartitionBeforeSWUpdate(t *testing.T) {
	runner, store, _ := testRunner(t, "exit 0")
	if err := os.Remove(runner.LayoutChecker.Layout.RootB); err != nil {
		t.Fatal(err)
	}
	err := runner.Install()
	if err == nil || !strings.Contains(err.Error(), "partition precheck failed") {
		t.Fatalf("expected partition precheck failure, got %v", err)
	}
	job, loadErr := store.Active()
	if loadErr != nil {
		t.Fatal(loadErr)
	}
	if job.State != StateFailed {
		t.Fatalf("expected failed state, got %s", job.State)
	}
	if _, statErr := os.Stat(store.LogPath(job.ID)); !os.IsNotExist(statErr) {
		t.Fatalf("SWUpdate log exists, so installation advanced past precheck: %v", statErr)
	}
}

func TestInstallRejectsMissingCurrentRootInstaller(t *testing.T) {
	runner, store, _ := testRunner(t, "exit 0")
	if err := os.Remove(runner.CurrentRootInstallerPath); err != nil {
		t.Fatal(err)
	}
	err := runner.Install()
	if err == nil || !strings.Contains(err.Error(), "upgrade the AIPC application before upgrading the OS") {
		t.Fatalf("expected current-root contract rejection, got %v", err)
	}
	job, loadErr := store.Active()
	if loadErr != nil {
		t.Fatal(loadErr)
	}
	if job.State != StateFailed {
		t.Fatalf("expected failed state, got %s", job.State)
	}
}

func TestInstallStagesSingleCopyRecovery(t *testing.T) {
	dir := t.TempDir()
	binDir := filepath.Join(dir, "bin")
	bootMount := filepath.Join(dir, "boot")
	if err := os.MkdirAll(binDir, 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(bootMount, 0755); err != nil {
		t.Fatal(err)
	}
	findmnt := filepath.Join(binDir, "findmnt")
	writeExecutable(t, findmnt, "#!/bin/sh\necho \""+bootMount+"\"\n")
	t.Setenv("PATH", binDir+":"+os.Getenv("PATH"))

	getScript := filepath.Join(dir, "get-copy")
	setScript := filepath.Join(dir, "set-copy")
	setLog := filepath.Join(dir, "selected")
	envLog := filepath.Join(dir, "fw-env")
	fwSetenv := filepath.Join(dir, "fw_setenv")
	fwPrintenv := filepath.Join(dir, "fw_printenv")
	writeExecutable(t, getScript, "#!/bin/sh\necho a\n")
	writeExecutable(t, setScript, "#!/bin/sh\necho \"$1\" > \""+setLog+"\"\n")
	writeExecutable(t, fwSetenv, "#!/bin/sh\necho \"$1=$2\" >> \""+envLog+"\"\n")
	writeExecutable(t, fwPrintenv, "#!/bin/sh\nexit 1\n")

	dataPath := filepath.Join(dir, "data")
	store := NewStore(filepath.Join(dataPath, "aipc-os-upgrade"))
	job := &Job{
		ID:            "job-single",
		State:         StateReady,
		TargetVersion: "1.2.3",
		UpgradeMode:   LayoutSingle,
	}
	if err := store.Save(job); err != nil {
		t.Fatal(err)
	}
	if err := store.SetActive(job.ID); err != nil {
		t.Fatal(err)
	}
	writeCPIO(t, store.PackagePath(job.ID), map[string][]byte{
		"fitImage":                                 []byte("target-kernel"),
		"swupdate-image-hailo15-ne503.ext4.gz":     gzipBytes(t, []byte("target recovery "+SingleRecoveryMarker)),
		"core-image-minimal-hailo15-ne503.ext4.gz": []byte("rootfs"),
	})
	recoveryDir := filepath.Join(dir, "recovery")
	if err := os.MkdirAll(recoveryDir, 0755); err != nil {
		t.Fatal(err)
	}
	fitPath := filepath.Join(recoveryDir, "fitImage")
	rootPath := filepath.Join(recoveryDir, "swupdate-image-hailo15-ne503.ext4.gz")
	if err := os.WriteFile(fitPath, []byte("bundled-kernel"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(rootPath, gzipBytes(t, []byte("rootfs "+SingleRecoveryMarker)), 0644); err != nil {
		t.Fatal(err)
	}
	manifest, err := BuildRecoveryManifest("hailo15-ne503", "1.11.0", "1.0.0", "", fitPath, rootPath)
	if err != nil {
		t.Fatal(err)
	}
	manifestData, _ := json.MarshalIndent(manifest, "", "  ")
	if err := os.WriteFile(filepath.Join(recoveryDir, "manifest.json"), manifestData, 0644); err != nil {
		t.Fatal(err)
	}

	layout := NewABLayout(filepath.Join(dir, "mmcblk1"))
	for _, path := range []string{layout.BootA, layout.RootA, layout.BootB} {
		if err := os.WriteFile(path, nil, 0644); err != nil {
			t.Fatal(err)
		}
	}
	runner := NewRunner(store)
	runner.GetImageScript = getScript
	runner.SetImageScript = setScript
	runner.FWSetenvBinary = fwSetenv
	runner.FWPrintenvBinary = fwPrintenv
	runner.DataMountPath = dataPath
	runner.RecoveryDir = recoveryDir
	configureTestBackup(t, runner, dir)
	runner.LayoutChecker = &LayoutChecker{
		Layout:             layout,
		Stat:               os.Stat,
		RootSource:         func() (string, error) { return layout.RootA, nil },
		IsMounted:          func(string) (bool, error) { return false, nil },
		RequireBlockDevice: false,
	}

	if err := runner.Install(); err != nil {
		t.Fatal(err)
	}
	loaded, err := store.Active()
	if err != nil {
		t.Fatal(err)
	}
	if loaded.State != StateAwaitingReboot || !loaded.RebootRequired {
		t.Fatalf("unexpected staged job: %+v", loaded)
	}
	if _, err := os.Stat(runner.bootEnvSnapshotPath(loaded)); err != nil {
		t.Fatalf("U-Boot environment snapshot was not persisted: %v", err)
	}
	selected, _ := os.ReadFile(setLog)
	if strings.TrimSpace(string(selected)) != "remote_update" {
		t.Fatalf("recovery boot was not selected: %q", selected)
	}
	env, _ := os.ReadFile(envLog)
	if !strings.Contains(string(env), "swupdate_update_filename=local:/aipc-os-upgrade/packages/job-single.swu") ||
		!strings.Contains(string(env), "swupdate_update_modes=copy-a") {
		t.Fatalf("unexpected recovery environment:\n%s", env)
	}
	for _, name := range []string{"fitImage", "swupdate-image-hailo15-ne503.ext4.gz"} {
		if _, err := os.Stat(filepath.Join(bootMount, name)); err != nil {
			t.Fatalf("recovery artifact %s was not staged: %v", name, err)
		}
	}
	fit, _ := os.ReadFile(filepath.Join(bootMount, "fitImage"))
	if string(fit) != "target-kernel" {
		t.Fatalf("target package recovery was not staged: %q", fit)
	}
	refreshedFit, _ := os.ReadFile(filepath.Join(recoveryDir, "fitImage"))
	if string(refreshedFit) != "target-kernel" {
		t.Fatalf("persistent recovery was not refreshed from target package: %q", refreshedFit)
	}
	if err := runner.CancelStaged(loaded); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Lstat(filepath.Join(runner.BackupRoot, "current")); !os.IsNotExist(err) {
		t.Fatalf("cancelled upgrade left backup selected: %v", err)
	}
	selected, _ = os.ReadFile(setLog)
	if strings.TrimSpace(string(selected)) != "a" {
		t.Fatalf("cancellation did not restore previous boot copy: %q", selected)
	}
	for _, name := range []string{"fitImage", "swupdate-image-hailo15-ne503.ext4.gz"} {
		if _, err := os.Stat(filepath.Join(bootMount, name)); !os.IsNotExist(err) {
			t.Fatalf("staged recovery artifact %s was not removed: %v", name, err)
		}
	}
}

func TestCheckVersionRequiresExactVersionID(t *testing.T) {
	dir := t.TempDir()
	path := filepath.Join(dir, "os-release")
	if err := os.WriteFile(path, []byte("VERSION_ID=11.2\n"), 0644); err != nil {
		t.Fatal(err)
	}
	runner := NewRunner(NewStore(filepath.Join(dir, "state")))
	runner.OSReleasePath = path
	if err := runner.checkVersion("1.2"); err == nil {
		t.Fatal("substring version must not pass exact OS verification")
	}
	if err := runner.checkVersion("11.2"); err != nil {
		t.Fatal(err)
	}
}

func TestStoredBootEnvironmentRestoresValuesAndUnsetKeys(t *testing.T) {
	dir := t.TempDir()
	logPath := filepath.Join(dir, "fw-env.log")
	fwSetenv := filepath.Join(dir, "fw_setenv")
	writeExecutable(t, fwSetenv, "#!/bin/sh\nif [ \"$#\" -eq 1 ]; then echo \"$1=<unset>\"; else echo \"$1=$2\"; fi >> \""+logPath+"\"\n")
	store := NewStore(filepath.Join(dir, "state"))
	job := &Job{ID: "job-env"}
	if err := store.Save(job); err != nil {
		t.Fatal(err)
	}
	runner := NewRunner(store)
	runner.FWSetenvBinary = fwSetenv
	oldValue := "tftp:image.swu"
	values := map[string]*string{
		"swupdate_update_filename":       &oldValue,
		"setup_swupdate_update_filename": nil,
	}
	if err := runner.saveBootEnvSnapshot(job, values); err != nil {
		t.Fatal(err)
	}
	if err := runner.restoreStoredBootEnv(job, true); err != nil {
		t.Fatal(err)
	}
	logData, err := os.ReadFile(logPath)
	if err != nil {
		t.Fatal(err)
	}
	logText := string(logData)
	if !strings.Contains(logText, "swupdate_update_filename="+oldValue) ||
		!strings.Contains(logText, "setup_swupdate_update_filename=<unset>") {
		t.Fatalf("unexpected restored environment:\n%s", logText)
	}
	if _, err := os.Stat(runner.bootEnvSnapshotPath(job)); !os.IsNotExist(err) {
		t.Fatalf("snapshot was not removed after restore: %v", err)
	}
}

func TestStoredBootEnvironmentIgnoresAlreadyRemovedSnapshot(t *testing.T) {
	dir := t.TempDir()
	store := NewStore(filepath.Join(dir, "state"))
	job := &Job{ID: "job-env-race"}
	if err := store.Save(job); err != nil {
		t.Fatal(err)
	}
	runner := NewRunner(store)
	snapshotPath := runner.bootEnvSnapshotPath(job)
	logPath := filepath.Join(dir, "fw-env.log")
	fwSetenv := filepath.Join(dir, "fw_setenv")
	writeExecutable(t, fwSetenv, "#!/bin/sh\necho \"$@\" >> \""+logPath+"\"\nrm -f \""+snapshotPath+"\"\n")
	runner.FWSetenvBinary = fwSetenv
	oldValue := "local:/aipc-os-upgrade/packages/image.swu"
	if err := runner.saveBootEnvSnapshot(job, map[string]*string{
		"swupdate_update_filename": &oldValue,
	}); err != nil {
		t.Fatal(err)
	}
	if err := runner.restoreStoredBootEnv(job, true); err != nil {
		t.Fatal(err)
	}
	if _, err := os.Stat(snapshotPath); !os.IsNotExist(err) {
		t.Fatalf("snapshot should be absent after simulated concurrent removal: %v", err)
	}
}

func TestSingleCopyVerificationFailureDoesNotRollback(t *testing.T) {
	dir := t.TempDir()
	setScript := filepath.Join(dir, "set-copy")
	setLog := filepath.Join(dir, "selected")
	writeExecutable(t, setScript, "#!/bin/sh\necho \"$1\" >> \""+setLog+"\"\n")
	store := NewStore(filepath.Join(dir, "state"))
	job := &Job{ID: "single", State: StateVerifying, UpgradeMode: LayoutSingle}
	if err := store.Save(job); err != nil {
		t.Fatal(err)
	}
	runner := NewRunner(store)
	runner.SetImageScript = setScript
	err := runner.verificationFailure(job, "health failed")
	if err == nil {
		t.Fatal("expected verification failure")
	}
	if _, statErr := os.Stat(setLog); !os.IsNotExist(statErr) {
		t.Fatalf("single-copy verification attempted a boot-copy rollback: %v", statErr)
	}
	loaded, loadErr := store.Load(job.ID)
	if loadErr != nil {
		t.Fatal(loadErr)
	}
	if loaded.State != StateFailed || loaded.Error != "health failed" {
		t.Fatalf("unexpected failed job: %+v", loaded)
	}
}

func testRunner(t *testing.T, swupdateBody string) (*Runner, *Store, string) {
	t.Helper()
	dir := t.TempDir()
	getScript := filepath.Join(dir, "get-copy")
	setScript := filepath.Join(dir, "set-copy")
	swupdate := filepath.Join(dir, "swupdate")
	setLog := filepath.Join(dir, "selected")
	writeExecutable(t, getScript, "#!/bin/sh\necho a\n")
	writeExecutable(t, setScript, "#!/bin/sh\necho \"$1\" > \""+setLog+"\"\n")
	writeExecutable(t, swupdate, "#!/bin/sh\n"+swupdateBody+"\n")

	store := NewStore(filepath.Join(dir, "state"))
	job := &Job{ID: "job-1", State: StateReady, TargetVersion: "1.2.3"}
	if err := store.Save(job); err != nil {
		t.Fatal(err)
	}
	if err := store.SetActive(job.ID); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(store.PackagePath(job.ID), []byte("package"), 0644); err != nil {
		t.Fatal(err)
	}
	runner := NewRunner(store)
	runner.GetImageScript = getScript
	runner.SetImageScript = setScript
	runner.SWUpdateBinary = swupdate
	configureTestBackup(t, runner, dir)
	layout := NewABLayout(filepath.Join(dir, "mmcblk1"))
	for _, path := range []string{layout.BootA, layout.RootA, layout.BootB, layout.RootB, layout.Data} {
		if err := os.WriteFile(path, nil, 0644); err != nil {
			t.Fatal(err)
		}
	}
	runner.LayoutChecker = &LayoutChecker{
		Layout:             layout,
		Stat:               os.Stat,
		RootSource:         func() (string, error) { return layout.RootA, nil },
		IsMounted:          func(string) (bool, error) { return false, nil },
		RequireBlockDevice: false,
	}
	return runner, store, setLog
}

func configureTestBackup(t *testing.T, runner *Runner, dir string) {
	t.Helper()
	sourceRoot := filepath.Join(dir, "backup-source")
	configPath := filepath.Join(sourceRoot, "opt", "aipc", "etc", "platform-api.yaml")
	if err := os.MkdirAll(filepath.Dir(configPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(configPath, []byte("test: true\n"), 0644); err != nil {
		t.Fatal(err)
	}
	appManifestPath := filepath.Join(sourceRoot, "opt", "aipc", "app-manifest.json")
	if err := os.WriteFile(appManifestPath, []byte(
		`{"app_version":"1.2.0","machine":"hailo15-ne503","required_compat_level":1,"supported_data_schema":[1],"target_data_schema":1}`,
	), 0644); err != nil {
		t.Fatal(err)
	}
	schemaPath := filepath.Join(sourceRoot, "data", "aipc-data", "schema-version")
	if err := os.MkdirAll(filepath.Dir(schemaPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(schemaPath, []byte("1\n"), 0644); err != nil {
		t.Fatal(err)
	}
	runner.BackupSourceRoot = sourceRoot
	runner.BackupRoot = filepath.Join(dir, "backups")
	runner.AppManifestPath = "/opt/aipc/app-manifest.json"
	runner.DataSchemaPath = "/data/aipc-data/schema-version"
	contractRoot := filepath.Join(dir, "current-root-contract")
	runner.CurrentRootInstallerPath = filepath.Join(contractRoot, "scripts", "aipc-install-current-root.sh")
	runner.CurrentRootUnitsPath = filepath.Join(contractRoot, "systemd")
	if err := os.MkdirAll(filepath.Dir(runner.CurrentRootInstallerPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.MkdirAll(runner.CurrentRootUnitsPath, 0755); err != nil {
		t.Fatal(err)
	}
	writeExecutable(t, runner.CurrentRootInstallerPath, "#!/bin/sh\nexit 0\n")
	if err := os.WriteFile(filepath.Join(runner.CurrentRootUnitsPath, "aipc-platform.target"), []byte("[Unit]\n"), 0644); err != nil {
		t.Fatal(err)
	}
	job, err := runner.Store.Active()
	if err != nil {
		t.Fatal(err)
	}
	job.Machine = "hailo15-ne503"
	job.CompatLevel = 1
	job.DataSchema = 1
	if err := runner.Store.Save(job); err != nil {
		t.Fatal(err)
	}
}

func writeExecutable(t *testing.T, path, content string) {
	t.Helper()
	if err := os.WriteFile(path, []byte(content), 0755); err != nil {
		t.Fatal(err)
	}
}
