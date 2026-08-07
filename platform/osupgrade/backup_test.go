package osupgrade

import (
	"archive/tar"
	"compress/gzip"
	"encoding/json"
	"io"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// TestCreateUpgradeBackup verifies the device-specific backup set:
// app-manifest.json, application systemd units, network, SSH and the small
// system configuration whitelist. OS-owned manager/journal/sysctl
// configuration must always come from the new image.
func TestCreateUpgradeBackup(t *testing.T) {
	dir := t.TempDir()
	sourceRoot := filepath.Join(dir, "root")
	// OS configuration is excluded, while the independently versioned App unit
	// is retained.
	writeBackupFixture(t, sourceRoot, "/etc/aipc/aipc.conf", "key=value\n")
	writeBackupFixture(t, sourceRoot, "/etc/systemd/system/camera-daemon.service", "[Service]\n")
	// network + ssh groups
	writeBackupFixture(t, sourceRoot, "/etc/systemd/network/20-wired.network", "Address=192.0.2.72/24\n")
	writeBackupFixture(t, sourceRoot, "/etc/resolv.conf", "nameserver 192.0.2.1\n")
	writeBackupFixture(t, sourceRoot, "/home/root/.ssh/authorized_keys", "ssh-ed25519 test\n")
	writeBackupFixture(t, sourceRoot, "/etc/ssh/sshd_config", "PermitRootLogin prohibit-password\n")
	// device-config group
	writeBackupFixture(t, sourceRoot, "/etc/timezone", "Asia/Shanghai\n")
	writeBackupFixture(t, sourceRoot, "/etc/systemd/timesyncd.conf", "[Time]\nNTP=ntp.example\n")
	writeBackupFixture(t, sourceRoot, "/usr/local/share/ca-certificates/site.crt", "test certificate\n")
	writeBackupFixture(t, sourceRoot, "/home/root/apps/resources/final_calibration.json", "{}\n")
	// app-manifest.json is backed up separately from the archive groups.
	writeBackupFixture(t, sourceRoot, "/data/app-manifest.json",
		`{"app_version":"1.2.0","machine":"hailo15-ne503","required_compat_level":1,"supported_data_schema":[1],"target_data_schema":1}`)

	runner := NewRunner(NewStore(filepath.Join(dir, "state")))
	runner.BackupRoot = filepath.Join(dir, "backups")
	runner.BackupSourceRoot = sourceRoot
	runner.OSReleasePath = filepath.Join(sourceRoot, "etc", "os-release")
	writeBackupFixture(t, sourceRoot, "/etc/os-release", "VERSION_ID=\"1.2.0\"\n")
	job := &Job{ID: "job-backup"}

	if err := runner.createUpgradeBackup(job); err != nil {
		t.Fatal(err)
	}
	current, err := os.Readlink(filepath.Join(runner.BackupRoot, "current"))
	if err != nil || current != job.ID {
		t.Fatalf("current backup link = %q, %v", current, err)
	}
	manifestData, err := os.ReadFile(filepath.Join(runner.BackupRoot, job.ID, "manifest.json"))
	if err != nil {
		t.Fatal(err)
	}
	var manifest backupManifest
	if err := json.Unmarshal(manifestData, &manifest); err != nil {
		t.Fatal(err)
	}
	if manifest.OSVersion != "1.2.0" {
		t.Fatalf("OSVersion = %q, want 1.2.0", manifest.OSVersion)
	}
	// app-manifest.json + systemd + network + ssh + device-config = 5 archives.
	wantArchives := 5
	if len(manifest.Archives) != wantArchives {
		t.Fatalf("archives = %d, want %d: %+v", len(manifest.Archives), wantArchives, manifest.Archives)
	}
	if _, err := os.Stat(filepath.Join(runner.BackupRoot, job.ID, "app-manifest.json")); err != nil {
		t.Fatalf("App manifest was not backed up: %v", err)
	}
	for _, name := range []string{"aipc.tar.gz", "runtime.tar.gz", "os-config.tar.gz"} {
		if _, err := os.Stat(filepath.Join(runner.BackupRoot, job.ID, name)); err == nil {
			t.Fatalf("legacy archive %s should no longer be produced", name)
		}
	}
	systemdNames := readTarNames(t, filepath.Join(runner.BackupRoot, job.ID, "systemd.tar.gz"))
	if !containsString(systemdNames, "etc/systemd/system/camera-daemon.service") {
		t.Fatalf("application unit missing from systemd archive: %v", systemdNames)
	}
	sshNames := readTarNames(t, filepath.Join(runner.BackupRoot, job.ID, "ssh.tar.gz"))
	if !containsString(sshNames, "etc/ssh/sshd_config") {
		t.Fatalf("sshd_config missing from ssh archive: %v", sshNames)
	}
	deviceConfigNames := readTarNames(t, filepath.Join(runner.BackupRoot, job.ID, "device-config.tar.gz"))
	for _, want := range []string{
		"etc/timezone",
		"etc/systemd/timesyncd.conf",
		"etc/resolv.conf",
		"usr/local/share/ca-certificates/site.crt",
		"home/root/apps/resources/final_calibration.json",
	} {
		if !containsString(deviceConfigNames, want) {
			t.Fatalf("%s missing from device-config archive: %v", want, deviceConfigNames)
		}
	}
}

func TestCreateUpgradeBackupSkipsGeneratedResolvConf(t *testing.T) {
	dir := t.TempDir()
	sourceRoot := filepath.Join(dir, "root")

	writeBackupFixture(t, sourceRoot, "/etc/timezone", "Asia/Shanghai\n")
	writeBackupFixture(t, sourceRoot, "/data/app-manifest.json",
		`{"app_version":"1.2.0","machine":"hailo15-ne503","required_compat_level":1,"supported_data_schema":[1],"target_data_schema":1}`)
	writeBackupFixture(t, sourceRoot, "/etc/os-release", "VERSION_ID=\"1.2.0\"\n")
	resolvPath := filepath.Join(sourceRoot, "etc", "resolv.conf")
	if err := os.MkdirAll(filepath.Dir(resolvPath), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.Symlink("../run/systemd/resolve/stub-resolv.conf", resolvPath); err != nil {
		t.Fatal(err)
	}

	runner := NewRunner(NewStore(filepath.Join(dir, "state")))
	runner.BackupRoot = filepath.Join(dir, "backups")
	runner.BackupSourceRoot = sourceRoot
	runner.OSReleasePath = filepath.Join(sourceRoot, "etc", "os-release")
	job := &Job{ID: "job-resolv"}

	if err := runner.createUpgradeBackup(job); err != nil {
		t.Fatal(err)
	}
	deviceConfigNames := readTarNames(t, filepath.Join(runner.BackupRoot, job.ID, "device-config.tar.gz"))
	if containsString(deviceConfigNames, "etc/resolv.conf") {
		t.Fatalf("generated resolv.conf symlink should not be archived: %v", deviceConfigNames)
	}
}

func writeBackupFixture(t *testing.T, root, path, content string) {
	t.Helper()
	full := filepath.Join(root, strings.TrimPrefix(path, "/"))
	if err := os.MkdirAll(filepath.Dir(full), 0755); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(full, []byte(content), 0644); err != nil {
		t.Fatal(err)
	}
}

func readTarNames(t *testing.T, path string) []string {
	t.Helper()
	file, err := os.Open(path)
	if err != nil {
		t.Fatal(err)
	}
	defer file.Close()
	gz, err := gzip.NewReader(file)
	if err != nil {
		t.Fatal(err)
	}
	defer gz.Close()
	reader := tar.NewReader(gz)
	var names []string
	for {
		header, err := reader.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			t.Fatal(err)
		}
		names = append(names, header.Name)
	}
	return names
}

func containsString(values []string, wanted string) bool {
	for _, value := range values {
		if value == wanted {
			return true
		}
	}
	return false
}
