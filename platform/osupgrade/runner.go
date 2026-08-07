package osupgrade

import (
	"bufio"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"syscall"
	"time"
)

type Runner struct {
	Store                    *Store
	GetImageScript           string
	SetImageScript           string
	SWUpdateBinary           string
	RequiredServices         []string
	HealthDuration           time.Duration
	OSReleasePath            string
	ConfigProbePath          string
	DatabaseProbePath        string
	CopyAValue               string
	CopyBValue               string
	LayoutChecker            *LayoutChecker
	FWSetenvBinary           string
	FWPrintenvBinary         string
	DataMountPath            string
	RecoveryMountPath        string
	RecoveryDir              string
	Machine                  string
	BackupRoot               string
	BackupSourceRoot         string
	OSCompatibilityPath      string
	AppManifestPath          string
	DataSchemaPath           string
	CurrentRootInstallerPath string
	CurrentRootUnitsPath     string
}

func NewRunner(store *Store) *Runner {
	return &Runner{
		Store:                    store,
		GetImageScript:           "/etc/get_sw_image.sh",
		SetImageScript:           "/etc/set_sw_image.sh",
		SWUpdateBinary:           "swupdate",
		RequiredServices:         []string{"platform-api", "camera-daemon"},
		HealthDuration:           60 * time.Second,
		OSReleasePath:            "/etc/os-release",
		ConfigProbePath:          envOrDefault("AIPC_PERSISTENT_DATA", "/data"),
		DatabaseProbePath:        os.Getenv("AIPC_DATABASE_PROBE"),
		CopyAValue:               envOrDefault("AIPC_COPY_A_VALUE", "a"),
		CopyBValue:               envOrDefault("AIPC_COPY_B_VALUE", "b"),
		LayoutChecker:            NewDeviceLayoutChecker(),
		FWSetenvBinary:           "fw_setenv",
		FWPrintenvBinary:         "fw_printenv",
		DataMountPath:            envOrDefault("AIPC_DATA_MOUNT", "/data"),
		RecoveryMountPath:        envOrDefault("AIPC_RECOVERY_BOOT_MOUNT", "/run/aipc-os-recovery-boot"),
		RecoveryDir:              envOrDefault("AIPC_RECOVERY_DIR", DefaultRecoveryDir),
		Machine:                  envOrDefault("AIPC_OS_MACHINE", "hailo15-ne503"),
		BackupRoot:               envOrDefault("AIPC_BACKUP_ROOT", DefaultBackupRoot),
		BackupSourceRoot:         envOrDefault("AIPC_BACKUP_SOURCE_ROOT", "/"),
		OSCompatibilityPath:      envOrDefault("AIPC_OS_COMPATIBILITY_FILE", DefaultOSCompatibilityPath),
		AppManifestPath:          envOrDefault("AIPC_APP_MANIFEST", DefaultAppManifestPath),
		DataSchemaPath:           envOrDefault("AIPC_DATA_SCHEMA_FILE", DefaultDataSchemaPath),
		CurrentRootInstallerPath: envOrDefault("AIPC_CURRENT_ROOT_INSTALLER", "/data/aipc/scripts/aipc-install-current-root.sh"),
		CurrentRootUnitsPath:     envOrDefault("AIPC_CURRENT_ROOT_UNITS", "/data/aipc/systemd"),
	}
}

func (r *Runner) Install() error {
	if err := r.Store.Init(); err != nil {
		return err
	}
	lock, err := r.lockOperation()
	if err != nil {
		return err
	}
	defer lock.Close()
	defer unlockOperation(lock)

	job, err := r.Store.Active()
	if err != nil {
		return err
	}
	if job.State == StateInstalled {
		if err := exec.Command("sync").Run(); err != nil {
			return r.fail(job, "sync failed while resuming install: "+err.Error())
		}
		if err := r.setCopy(job.TargetCopy); err != nil {
			_ = r.setCopy(job.PreviousCopy)
			return r.fail(job, "cannot select target boot copy while resuming: "+err.Error())
		}
		job.State = StateAwaitingReboot
		job.Progress = 100
		job.Message = "Upgrade installed; reboot required"
		job.RebootRequired = true
		return r.Store.Save(job)
	}
	if job.State != StateReady && job.State != StateInstalling {
		return fmt.Errorf("job %s is not ready (state %s)", job.ID, job.State)
	}
	if err := r.checkCurrentRootContract(); err != nil {
		return r.fail(job, "AIPC current-root contract check failed: "+err.Error())
	}
	current, err := r.currentCopy()
	if err != nil {
		return r.fail(job, "cannot identify current boot copy: "+err.Error())
	}
	layout, err := r.LayoutChecker.Detect(current)
	if err != nil {
		return r.fail(job, "partition precheck failed: "+err.Error())
	}
	if job.UpgradeMode != "" && job.UpgradeMode != layout.Mode {
		return r.fail(job, fmt.Sprintf("partition layout changed after validation: was %s, now %s", job.UpgradeMode, layout.Mode))
	}
	if err := r.checkTargetCompatibility(job); err != nil {
		return r.fail(job, "OS/App compatibility check failed: "+err.Error())
	}
	if job.SHA256 != "" {
		sum, err := fileSHA256(job.PackagePath)
		if err != nil {
			return r.fail(job, "cannot re-read validated OS package: "+err.Error())
		}
		if !strings.EqualFold(sum, job.SHA256) {
			return r.fail(job, fmt.Sprintf("OS package changed after validation: expected %s, got %s", job.SHA256, sum))
		}
	}
	job.Message = "Backing up AIPC before OS installation"
	job.Progress = 5
	if err := r.Store.Save(job); err != nil {
		return err
	}
	if err := r.createUpgradeBackup(job); err != nil {
		return r.fail(job, "cannot create persistent AIPC backup: "+err.Error())
	}
	target := layout.TargetCopy
	swupdateMode, err := job.SWUpdateMode(target)
	if err != nil {
		return r.fail(job, err.Error())
	}
	job.State = StateInstalling
	job.Progress = 10
	job.Message = "Writing inactive OS copy"
	job.CurrentCopy = current
	job.PreviousCopy = current
	job.TargetCopy = target
	if strings.TrimSpace(job.UpdateMode) == "" {
		job.UpgradeStrategy = string(job.EffectiveUpgradeStrategy())
	}
	if err := r.Store.Save(job); err != nil {
		return err
	}
	if layout.Mode == LayoutSingle {
		return r.installSingleRecovery(job, swupdateMode)
	}
	if err := r.LayoutChecker.Check(current, target); err != nil {
		return r.fail(job, "A/B partition precheck failed: "+err.Error())
	}

	logFile, err := os.OpenFile(r.Store.LogPath(job.ID), os.O_CREATE|os.O_WRONLY|os.O_TRUNC, 0640)
	if err != nil {
		return r.fail(job, err.Error())
	}
	defer logFile.Close()
	cmd := exec.Command(r.SWUpdateBinary, "-i", job.PackagePath, "-v", "-m", "-M", "-e", "stable,"+swupdateMode)
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return r.fail(job, err.Error())
	}
	cmd.Stderr = cmd.Stdout
	if err := cmd.Start(); err != nil {
		return r.fail(job, err.Error())
	}
	r.copyProgress(job, stdout, logFile)
	if err := cmd.Wait(); err != nil {
		_ = r.setCopy(current)
		return r.fail(job, "SWUpdate failed; boot copy was not changed: "+err.Error())
	}

	job.State = StateInstalled
	job.Progress = 95
	job.Message = "OS image written successfully"
	if err := r.Store.Save(job); err != nil {
		_ = r.setCopy(current)
		return err
	}
	if err := exec.Command("sync").Run(); err != nil {
		_ = r.setCopy(current)
		return r.fail(job, "sync failed: "+err.Error())
	}
	if err := r.setCopy(target); err != nil {
		_ = r.setCopy(current)
		return r.fail(job, "cannot select target boot copy: "+err.Error())
	}
	job.State = StateAwaitingReboot
	job.Progress = 100
	job.Message = "Upgrade installed; reboot required"
	job.RebootRequired = true
	return r.Store.Save(job)
}

func (r *Runner) checkCurrentRootContract() error {
	installer := strings.TrimSpace(r.CurrentRootInstallerPath)
	if installer != "" {
		info, err := os.Stat(installer)
		if err != nil {
			return fmt.Errorf("upgrade the AIPC application before upgrading the OS: installer %s is unavailable: %w", installer, err)
		}
		if !info.Mode().IsRegular() || info.Mode().Perm()&0111 == 0 {
			return fmt.Errorf("upgrade the AIPC application before upgrading the OS: installer %s is not executable", installer)
		}
	}

	unitsPath := strings.TrimSpace(r.CurrentRootUnitsPath)
	if unitsPath != "" {
		entries, err := os.ReadDir(unitsPath)
		if err != nil {
			return fmt.Errorf("canonical systemd unit directory %s is unavailable: %w", unitsPath, err)
		}
		found := false
		for _, entry := range entries {
			name := entry.Name()
			if !entry.IsDir() && (strings.HasSuffix(name, ".service") || strings.HasSuffix(name, ".timer") || strings.HasSuffix(name, ".target")) {
				found = true
				break
			}
		}
		if !found {
			return fmt.Errorf("canonical systemd unit directory %s is empty", unitsPath)
		}
	}
	return nil
}

func (r *Runner) installSingleRecovery(job *Job, swupdateMode string) error {
	if strings.TrimSpace(swupdateMode) == "" {
		return r.fail(job, "OS update mode is empty")
	}
	if strings.TrimSpace(job.UpdateMode) == "" {
		job.UpgradeStrategy = string(job.EffectiveUpgradeStrategy())
	}
	bundle, err := r.updateRecoveryBundleFromPackage(job)
	if err != nil {
		return r.fail(job, "cannot update bundled recovery from target package: "+err.Error())
	}
	mountPath, mountedHere, err := r.ensureRecoveryBootMounted()
	if err != nil {
		return r.fail(job, "cannot mount recovery boot partition: "+err.Error())
	}
	if mountedHere {
		defer exec.Command("umount", mountPath).Run()
	}
	restoreBoot, err := r.stageBundledRecovery(job, bundle, mountPath)
	if err != nil {
		return r.fail(job, "cannot stage bundled recovery: "+err.Error())
	}
	relative, err := filepath.Rel(r.DataMountPath, job.PackagePath)
	if err != nil || relative == "." || strings.HasPrefix(relative, "..") || filepath.IsAbs(relative) {
		_ = restoreBoot()
		return r.fail(job, "single-copy package must be stored below "+r.DataMountPath)
	}
	// Recovery mounts the persistent partition at /data, so pass a path
	// relative to that mount. The recovery init script uses the "local:"
	// prefix to select run_local_update vs run_tftp_update.
	localPath := "local:/" + filepath.ToSlash(relative)
	settings := [][2]string{
		{"swupdate_update_filename", localPath},
		{"swupdate_update_modes", swupdateMode},
		// Neutralize the U-Boot helper that resets swupdate_update_filename
		// to its TFTP default during recovery boot (bootargs_swupdate →
		// setup_swupdate_update_filename). Without this, swupdate inside the
		// recovery initramfs never finds the uploaded SWU and falls back to
		// the bundled recovery image (stale version).
		{"setup_swupdate_update_filename", "true"},
	}
	previousEnv := make(map[string]*string, len(settings))
	for _, setting := range settings {
		previous, readErr := r.readBootEnv(setting[0])
		if readErr != nil {
			_ = restoreBoot()
			return r.fail(job, fmt.Sprintf("cannot read U-Boot %s: %v", setting[0], readErr))
		}
		previousEnv[setting[0]] = previous
	}
	if err := r.saveBootEnvSnapshot(job, previousEnv); err != nil {
		_ = restoreBoot()
		return r.fail(job, "cannot persist U-Boot environment snapshot: "+err.Error())
	}
	for _, setting := range settings {
		output, setErr := exec.Command(r.FWSetenvBinary, setting[0], setting[1]).CombinedOutput()
		if setErr != nil {
			_ = restoreBoot()
			if r.restoreBootEnv(previousEnv) == nil {
				_ = os.Remove(r.bootEnvSnapshotPath(job))
			}
			return r.fail(job, fmt.Sprintf("cannot set U-Boot %s: %s: %v", setting[0], strings.TrimSpace(string(output)), setErr))
		}
	}
	if err := r.setCopy("remote_update"); err != nil {
		_ = restoreBoot()
		if r.restoreBootEnv(previousEnv) == nil {
			_ = os.Remove(r.bootEnvSnapshotPath(job))
		}
		return r.fail(job, "cannot select recovery boot: "+err.Error())
	}
	if err := exec.Command("sync").Run(); err != nil {
		_ = r.setCopy("A")
		_ = restoreBoot()
		if r.restoreBootEnv(previousEnv) == nil {
			_ = os.Remove(r.bootEnvSnapshotPath(job))
		}
		return r.fail(job, "sync failed after staging recovery: "+err.Error())
	}
	job.State = StateAwaitingReboot
	job.Progress = 100
	job.Message = "Single-copy recovery upgrade staged; reboot required"
	job.RebootRequired = true
	return r.Store.Save(job)
}

func (r *Runner) updateRecoveryBundleFromPackage(job *Job) (*RecoveryBundle, error) {
	candidate, err := PrepareRecoveryBundleFromPackage(
		job.PackagePath,
		r.Store.RecoveryCandidateDir(job.ID),
		r.Machine,
		job.TargetVersion,
		job.SecureBootKeyID,
	)
	if err != nil {
		return nil, err
	}
	if err := os.MkdirAll(r.RecoveryDir, 0755); err != nil {
		return nil, err
	}
	for _, path := range []string{candidate.FitPath, candidate.RootPath, filepath.Join(candidate.Dir, "manifest.json")} {
		if err := copyFileAtomic(path, filepath.Join(r.RecoveryDir, filepath.Base(path)), 0644); err != nil {
			return nil, err
		}
	}
	syncPath(r.RecoveryDir)
	return LoadRecoveryBundle(r.RecoveryDir, r.Machine)
}

func (r *Runner) stageBundledRecovery(
	job *Job,
	bundle *RecoveryBundle,
	mountPath string,
) (func() error, error) {
	backupDir := filepath.Join(r.Store.JobDir(job.ID), "boot-backup")
	if err := os.MkdirAll(backupDir, 0700); err != nil {
		return nil, err
	}
	type stagedFile struct {
		source      string
		destination string
		backup      string
		existed     bool
	}
	files := []stagedFile{
		{
			source:      bundle.FitPath,
			destination: filepath.Join(mountPath, filepath.Base(bundle.FitPath)),
			backup:      filepath.Join(backupDir, filepath.Base(bundle.FitPath)),
		},
		{
			source:      bundle.RootPath,
			destination: filepath.Join(mountPath, filepath.Base(bundle.RootPath)),
			backup:      filepath.Join(backupDir, filepath.Base(bundle.RootPath)),
		},
	}
	restore := func() error {
		var restoreErr error
		for _, file := range files {
			if file.existed {
				if err := copyFileAtomic(file.backup, file.destination, 0644); err != nil {
					restoreErr = errors.Join(restoreErr, err)
				}
			} else if err := os.Remove(file.destination); err != nil && !os.IsNotExist(err) {
				restoreErr = errors.Join(restoreErr, err)
			}
		}
		syncPath(mountPath)
		return restoreErr
	}
	for index := range files {
		if _, err := os.Stat(files[index].destination); err == nil {
			files[index].existed = true
			if err := copyFileAtomic(files[index].destination, files[index].backup, 0600); err != nil {
				return nil, err
			}
		} else if !os.IsNotExist(err) {
			return nil, err
		}
		if err := copyFileAtomic(files[index].source, files[index].destination, 0644); err != nil {
			_ = restore()
			return nil, err
		}
	}
	syncPath(mountPath)
	return restore, nil
}

func (r *Runner) readBootEnv(key string) (*string, error) {
	output, err := exec.Command(r.FWPrintenvBinary, "-n", key).CombinedOutput()
	if err == nil {
		value := strings.TrimSpace(string(output))
		return &value, nil
	}
	var exitErr *exec.ExitError
	if errors.As(err, &exitErr) && exitErr.ExitCode() == 1 {
		return nil, nil
	}
	return nil, fmt.Errorf("%s: %w", strings.TrimSpace(string(output)), err)
}

func (r *Runner) restoreBootEnv(values map[string]*string) error {
	var restoreErr error
	for key, value := range values {
		var command *exec.Cmd
		if value == nil {
			command = exec.Command(r.FWSetenvBinary, key)
		} else {
			command = exec.Command(r.FWSetenvBinary, key, *value)
		}
		if output, err := command.CombinedOutput(); err != nil {
			restoreErr = errors.Join(restoreErr, fmt.Errorf("%s: %s: %w", key, strings.TrimSpace(string(output)), err))
		}
	}
	return restoreErr
}

func (r *Runner) bootEnvSnapshotPath(job *Job) string {
	return filepath.Join(r.Store.JobDir(job.ID), "boot-env.json")
}

func (r *Runner) saveBootEnvSnapshot(job *Job, values map[string]*string) error {
	data, err := json.MarshalIndent(values, "", "  ")
	if err != nil {
		return err
	}
	return atomicWrite(r.bootEnvSnapshotPath(job), append(data, '\n'), 0600)
}

func (r *Runner) loadBootEnvSnapshot(job *Job) (map[string]*string, error) {
	data, err := os.ReadFile(r.bootEnvSnapshotPath(job))
	if err != nil {
		return nil, err
	}
	var values map[string]*string
	if err := json.Unmarshal(data, &values); err != nil {
		return nil, err
	}
	if len(values) == 0 {
		return nil, fmt.Errorf("U-Boot environment snapshot is empty")
	}
	return values, nil
}

func (r *Runner) restoreStoredBootEnv(job *Job, removeSnapshot bool) error {
	values, err := r.loadBootEnvSnapshot(job)
	if err != nil {
		return fmt.Errorf("cannot load U-Boot environment snapshot: %w", err)
	}
	if err := r.restoreBootEnv(values); err != nil {
		return err
	}
	if removeSnapshot {
		if err := os.Remove(r.bootEnvSnapshotPath(job)); err != nil && !os.IsNotExist(err) {
			return err
		}
	}
	return nil
}

func (r *Runner) ensureRecoveryBootMounted() (string, bool, error) {
	bootDevice := r.LayoutChecker.Layout.BootA
	output, err := exec.Command("findmnt", "-n", "-o", "TARGET", "-S", bootDevice).CombinedOutput()
	if err == nil && strings.TrimSpace(string(output)) != "" {
		return strings.TrimSpace(string(output)), false, nil
	}
	if err := os.MkdirAll(r.RecoveryMountPath, 0755); err != nil {
		return "", false, err
	}
	output, err = exec.Command("mount", bootDevice, r.RecoveryMountPath).CombinedOutput()
	if err != nil {
		return "", false, fmt.Errorf("%s: %w", strings.TrimSpace(string(output)), err)
	}
	return r.RecoveryMountPath, true, nil
}

func (r *Runner) Reboot() error {
	job, err := r.Store.Active()
	if err != nil {
		return err
	}
	if job.State != StateAwaitingReboot {
		return fmt.Errorf("job is not awaiting reboot")
	}
	job.State = StateRebooting
	job.Message = "Device is rebooting into the new OS copy"
	if err := r.Store.Save(job); err != nil {
		return err
	}
	if err := exec.Command("sync").Run(); err != nil {
		return err
	}
	return exec.Command("reboot").Run()
}

func (r *Runner) Verify() error {
	lock, acquired, err := r.tryLockOperation()
	if err != nil {
		return err
	}
	if !acquired {
		return nil
	}
	defer lock.Close()
	defer unlockOperation(lock)

	job, err := r.Store.Active()
	if err != nil {
		return err
	}
	if job.UpgradeMode == LayoutSingle {
		if data, readErr := os.ReadFile(filepath.Join(r.Store.JobDir(job.ID), "recovery.failed")); readErr == nil {
			job.State = StateFailed
			job.Progress = 100
			job.RebootRequired = false
			job.Error = "single-copy recovery failed with code " + strings.TrimSpace(string(data))
			if restoreErr := r.restoreStoredBootEnv(job, true); restoreErr != nil {
				job.Error += "; cannot restore U-Boot environment: " + restoreErr.Error()
			}
			job.Message = "Single-copy recovery upgrade failed"
			return r.Store.Save(job)
		}
	}
	current, err := r.currentCopy()
	if err != nil {
		return err
	}
	if job.State == StateRollback && current == job.PreviousCopy {
		job.State = StateFailed
		job.Progress = 100
		job.RebootRequired = false
		job.Message = "New OS failed verification and the device rolled back"
		_ = r.deactivateBackup(job.ID)
		return r.Store.Save(job)
	}
	if job.State == StateInstalled && current == job.PreviousCopy {
		job.State = StateAwaitingReboot
		job.Progress = 100
		job.RebootRequired = true
		job.Message = "Upgrade installed; reboot required"
		return r.Store.Save(job)
	}
	if job.State != StateRebooting && job.State != StateAwaitingReboot && job.State != StateVerifying {
		if job.State != StateInstalled || current != job.TargetCopy {
			return nil
		}
	}
	job.State = StateVerifying
	job.Progress = 98
	job.Message = "Verifying the new OS"
	job.RebootRequired = false
	if err := r.Store.Save(job); err != nil {
		return err
	}
	if current != job.TargetCopy {
		return r.verificationFailure(job, "device did not boot the target copy")
	}
	if err := r.checkVersion(job.TargetVersion); err != nil {
		return r.verificationFailure(job, err.Error())
	}
	if err := r.checkBootedCompatibility(job); err != nil {
		return r.verificationFailure(job, err.Error())
	}

	// Verify that the post-upgrade restore
	// (aipc-restore.service) completed successfully before checking service
	// health. The .done marker is written by aipc-restore.sh as its final
	// step; its absence means the restore failed or never ran, so the OS
	// cannot be considered healthy even if platform-api happens to be up.
	donePath := filepath.Join("/var/lib/aipc-restore", job.ID+".done")
	if _, err := os.Stat(donePath); err != nil {
		return r.verificationFailure(job, "post-upgrade restore did not complete (missing "+donePath+")")
	}

	overallDeadline := time.Now().Add(3 * r.HealthDuration)
	stableSince := time.Time{}
	var lastHealthErr error
	for {
		if err := r.checkHealth(); err != nil {
			lastHealthErr = err
			stableSince = time.Time{}
		} else {
			if stableSince.IsZero() {
				stableSince = time.Now()
			}
			if time.Since(stableSince) >= r.HealthDuration {
				break
			}
		}
		if time.Now().After(overallDeadline) {
			if lastHealthErr == nil {
				lastHealthErr = fmt.Errorf("services did not remain healthy for %s", r.HealthDuration)
			}
			return r.verificationFailure(job, lastHealthErr.Error())
		}
		time.Sleep(5 * time.Second)
	}
	if job.UpgradeMode == LayoutSingle {
		if err := r.restoreStoredBootEnv(job, true); err != nil {
			return r.verificationFailure(job, "cannot restore U-Boot environment after recovery: "+err.Error())
		}
		_ = os.RemoveAll(filepath.Join(r.Store.JobDir(job.ID), "boot-backup"))
	}
	job.State = StateSuccess
	job.Progress = 100
	job.Message = "OS upgrade verified successfully"
	job.Error = ""
	_ = os.Remove(filepath.Join(r.Store.JobDir(job.ID), "recovery.success"))
	_ = r.deactivateBackup(job.ID)
	return r.Store.Save(job)
}

func (r *Runner) lockOperation() (*os.File, error) {
	if err := r.Store.Init(); err != nil {
		return nil, err
	}
	lock, err := os.OpenFile(r.Store.LockPath(), os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, err
	}
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		_ = lock.Close()
		return nil, fmt.Errorf("another OS upgrade is running")
	}
	return lock, nil
}

func (r *Runner) tryLockOperation() (*os.File, bool, error) {
	if err := r.Store.Init(); err != nil {
		return nil, false, err
	}
	lock, err := os.OpenFile(r.Store.LockPath(), os.O_CREATE|os.O_RDWR, 0600)
	if err != nil {
		return nil, false, err
	}
	if err := syscall.Flock(int(lock.Fd()), syscall.LOCK_EX|syscall.LOCK_NB); err != nil {
		_ = lock.Close()
		if errors.Is(err, syscall.EWOULDBLOCK) || errors.Is(err, syscall.EAGAIN) {
			return nil, false, nil
		}
		return nil, false, err
	}
	return lock, true, nil
}

func unlockOperation(lock *os.File) {
	_ = syscall.Flock(int(lock.Fd()), syscall.LOCK_UN)
}

func (r *Runner) verificationFailure(job *Job, reason string) error {
	if job.UpgradeMode == LayoutSingle {
		if err := r.restoreStoredBootEnv(job, true); err != nil && !errors.Is(err, os.ErrNotExist) {
			reason += "; cannot restore U-Boot environment: " + err.Error()
		}
		job.State = StateFailed
		job.Progress = 100
		job.RebootRequired = false
		job.Error = reason
		job.Message = "Single-copy OS verification failed; automatic rollback is unavailable"
		_ = r.Store.Save(job)
		return fmt.Errorf("%s", reason)
	}
	return r.rollback(job, reason)
}

func (r *Runner) currentCopy() (string, error) {
	out, err := exec.Command(r.GetImageScript, "--boot").CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("%s: %w", strings.TrimSpace(string(out)), err)
	}
	value := strings.ToUpper(strings.TrimSpace(string(out)))
	switch {
	case value == "A", strings.HasSuffix(value, "COPY-A"), strings.HasSuffix(value, "COPY A"):
		return "A", nil
	case value == "B", strings.HasSuffix(value, "COPY-B"), strings.HasSuffix(value, "COPY B"):
		return "B", nil
	}
	return "", fmt.Errorf("unrecognized output %q", value)
}

func oppositeCopy(current string) string {
	if current == "A" {
		return "B"
	}
	if current == "B" {
		return "A"
	}
	return ""
}

func (r *Runner) setCopy(copy string) error {
	value := copy
	if copy == "A" {
		value = r.CopyAValue
	} else if copy == "B" {
		value = r.CopyBValue
	}
	out, err := exec.Command(r.SetImageScript, value).CombinedOutput()
	if err != nil {
		return fmt.Errorf("%s: %w", strings.TrimSpace(string(out)), err)
	}
	return nil
}

func (r *Runner) copyProgress(job *Job, input io.Reader, log io.Writer) {
	scanner := bufio.NewScanner(input)
	scanner.Buffer(make([]byte, 64*1024), 1024*1024)
	last := 10
	for scanner.Scan() {
		line := scanner.Text()
		_, _ = fmt.Fprintln(log, line)
		for _, field := range strings.Fields(line) {
			if !strings.HasSuffix(field, "%") {
				continue
			}
			raw := strings.TrimSuffix(strings.Trim(field, "[](),"), "%")
			if value, err := strconv.Atoi(raw); err == nil {
				progress := 10 + value*8/10
				if progress >= last+2 && progress < 95 {
					last = progress
					job.Progress = progress
					_ = r.Store.Save(job)
				}
			}
		}
	}
}

func (r *Runner) checkVersion(target string) error {
	booted := readOSVersion(r.OSReleasePath)
	if booted == "" {
		return fmt.Errorf("cannot read OS version metadata from %s", r.OSReleasePath)
	}
	if target != "" && booted != target {
		return fmt.Errorf("booted OS version %s does not match target %s", booted, target)
	}
	return nil
}

func (r *Runner) checkHealth() error {
	for _, service := range r.RequiredServices {
		if err := exec.Command("systemctl", "is-active", "--quiet", service).Run(); err != nil {
			return fmt.Errorf("required service %s is not active", service)
		}
	}
	if r.ConfigProbePath != "" {
		if _, err := os.Stat(r.ConfigProbePath); err != nil {
			return fmt.Errorf("persistent configuration is not readable: %w", err)
		}
	}
	if r.DatabaseProbePath != "" {
		f, err := os.Open(r.DatabaseProbePath)
		if err != nil {
			return fmt.Errorf("database is not readable: %w", err)
		}
		_ = f.Close()
	}
	return nil
}

func (r *Runner) checkTargetCompatibility(job *Job) error {
	app, err := LoadAppManifest(rootedPath(r.BackupSourceRoot, r.AppManifestPath))
	if err != nil {
		return fmt.Errorf("cannot read App manifest: %w", err)
	}
	schema, err := ReadDataSchema(rootedPath(r.BackupSourceRoot, r.DataSchemaPath))
	if err != nil {
		return fmt.Errorf("cannot read data schema: %w", err)
	}
	target := &OSCompatibility{
		OSVersion:   job.TargetVersion,
		Machine:     job.Machine,
		Product:     job.Product,
		CompatLevel: job.CompatLevel,
		DataSchema:  job.DataSchema,
	}
	return CheckCompatibility(target, app, schema)
}

func (r *Runner) checkBootedCompatibility(job *Job) error {
	osInfo, err := LoadOSCompatibility(r.OSCompatibilityPath)
	if err != nil {
		return fmt.Errorf("cannot read booted OS compatibility metadata: %w", err)
	}
	app, err := LoadAppManifest(r.AppManifestPath)
	if err != nil {
		return fmt.Errorf("cannot read restored App manifest: %w", err)
	}
	schema, err := ReadDataSchema(r.DataSchemaPath)
	if err != nil {
		return fmt.Errorf("cannot read data schema: %w", err)
	}
	if job.CompatLevel > 0 && osInfo.CompatLevel != job.CompatLevel {
		return fmt.Errorf("booted OS compatibility level %d does not match target %d", osInfo.CompatLevel, job.CompatLevel)
	}
	if job.DataSchema > 0 && osInfo.DataSchema != job.DataSchema {
		return fmt.Errorf("booted OS data schema %d does not match target %d", osInfo.DataSchema, job.DataSchema)
	}
	return CheckCompatibility(osInfo, app, schema)
}

func (r *Runner) rollback(job *Job, reason string) error {
	job.State = StateRollback
	job.Error = reason
	job.Message = "Verification failed; rolling back to the previous OS copy"
	if err := r.Store.Save(job); err != nil {
		return err
	}
	if err := r.setCopy(job.PreviousCopy); err != nil {
		return r.fail(job, "verification failed and rollback selection failed: "+err.Error())
	}
	_ = exec.Command("sync").Run()
	return exec.Command("reboot").Run()
}

func (r *Runner) fail(job *Job, message string) error {
	job.State = StateFailed
	job.Error = message
	job.Message = "OS upgrade failed"
	job.RebootRequired = false
	_ = r.deactivateBackup(job.ID)
	_ = r.Store.Save(job)
	return fmt.Errorf("%s", message)
}

// CancelStaged undoes a staged upgrade before the device reboots.
// It selects the appropriate recovery path based on the upgrade mode.
func (r *Runner) CancelStaged(job *Job) error {
	var err error
	switch job.UpgradeMode {
	case LayoutSingle:
		err = r.cancelSingleRecovery(job)
	case LayoutDual:
		err = r.cancelDualCopy(job)
	default:
		return fmt.Errorf("unknown upgrade mode %q", job.UpgradeMode)
	}
	if err != nil {
		return err
	}
	return r.deactivateBackup(job.ID)
}

// cancelSingleRecovery undoes the single-recovery staging:
//   - Restores boot partition files from the backup directory
//   - Restores the exact U-Boot environment captured before staging
//   - Restores the original boot copy
func (r *Runner) cancelSingleRecovery(job *Job) error {
	mountPath, mountedHere, err := r.ensureRecoveryBootMounted()
	if err != nil {
		return fmt.Errorf("cannot mount recovery boot partition: %w", err)
	}
	if mountedHere {
		defer exec.Command("umount", mountPath).Run()
	}

	// Restore backed-up boot files and remove the staged recovery files.
	backupDir := filepath.Join(r.Store.JobDir(job.ID), "boot-backup")

	type stagedEntry struct {
		name   string
		backup string
		target string
	}
	staged := []stagedEntry{
		{"fitImage", filepath.Join(backupDir, "fitImage"), filepath.Join(mountPath, "fitImage")},
		{"rootfs", filepath.Join(backupDir, "swupdate-image-hailo15-ne503.ext4.gz"), filepath.Join(mountPath, "swupdate-image-hailo15-ne503.ext4.gz")},
	}
	for _, e := range staged {
		if _, backupStat := os.Stat(e.backup); backupStat == nil {
			if copyErr := copyFileAtomic(e.backup, e.target, 0644); copyErr != nil {
				return fmt.Errorf("cannot restore %s from backup: %w", e.target, copyErr)
			}
		} else if os.IsNotExist(backupStat) {
			if rmErr := os.Remove(e.target); rmErr != nil && !os.IsNotExist(rmErr) {
				return fmt.Errorf("cannot remove staged %s: %w", e.target, rmErr)
			}
		} else {
			return fmt.Errorf("cannot stat backup %s: %w", e.backup, backupStat)
		}
	}
	syncPath(mountPath)

	if err := r.restoreStoredBootEnv(job, false); err != nil {
		return fmt.Errorf("cannot restore staged U-Boot environment: %w", err)
	}

	if job.PreviousCopy == "" {
		return fmt.Errorf("cannot restore boot copy: previous copy is unknown")
	}
	if setErr := r.setCopy(job.PreviousCopy); setErr != nil {
		return fmt.Errorf("cannot restore boot copy to %s: %w", job.PreviousCopy, setErr)
	}
	_ = exec.Command("sync").Run()
	if err := os.Remove(r.bootEnvSnapshotPath(job)); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("cannot remove restored U-Boot environment snapshot: %w", err)
	}

	// Keep the backup until every boot-environment operation has succeeded.
	// A partial cancellation must remain recoverable on the next attempt.
	if removeErr := os.RemoveAll(backupDir); removeErr != nil {
		return fmt.Errorf("cannot remove recovery boot backup: %w", removeErr)
	}

	return nil
}

// cancelDualCopy undoes a dual-copy staged upgrade by restoring the
// boot copy to the previous (pre-upgrade) copy.
func (r *Runner) cancelDualCopy(job *Job) error {
	if job.PreviousCopy == "" {
		return fmt.Errorf("cannot cancel dual-copy upgrade: previous copy is unknown")
	}
	if setErr := r.setCopy(job.PreviousCopy); setErr != nil {
		return fmt.Errorf("cannot restore boot copy to %s: %w", job.PreviousCopy, setErr)
	}
	_ = exec.Command("sync").Run()
	return nil
}

func envOrDefault(key, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(key)); value != "" {
		return value
	}
	return fallback
}

func copyFileAtomic(source, destination string, mode os.FileMode) error {
	input, err := os.Open(source)
	if err != nil {
		return err
	}
	defer input.Close()
	return writeAtomicStream(destination, input, mode)
}

func syncPath(path string) {
	if directory, err := os.Open(path); err == nil {
		_ = directory.Sync()
		_ = directory.Close()
	}
}
