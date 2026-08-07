package osupgrade

import (
	"archive/tar"
	"compress/gzip"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"io/fs"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"time"
)

const DefaultBackupRoot = "/data/backups/aipc-os-upgrade"

type backupArchive struct {
	Name   string `json:"name"`
	SHA256 string `json:"sha256"`
	Size   int64  `json:"size"`
}

type backupManifest struct {
	FormatVersion int             `json:"format_version"`
	JobID         string          `json:"job_id"`
	CreatedAt     time.Time       `json:"created_at"`
	OSVersion     string          `json:"os_version,omitempty"`
	Archives      []backupArchive `json:"archives"`
}

type backupGroup struct {
	name    string
	paths   []string
	exclude func(string) bool
}

func (r *Runner) createUpgradeBackup(job *Job) error {
	if strings.TrimSpace(r.BackupRoot) == "" {
		return nil
	}
	root := filepath.Clean(r.BackupRoot)
	if err := os.MkdirAll(root, 0700); err != nil {
		return fmt.Errorf("create backup root: %w", err)
	}
	finalDir := filepath.Join(root, job.ID)
	if manifest, err := os.ReadFile(filepath.Join(finalDir, "manifest.json")); err == nil {
		var existing backupManifest
		if json.Unmarshal(manifest, &existing) == nil && existing.JobID == job.ID {
			return r.activateBackup(root, job.ID)
		}
	}

	tempDir, err := os.MkdirTemp(root, "."+job.ID+".tmp-")
	if err != nil {
		return fmt.Errorf("create backup staging directory: %w", err)
	}
	defer os.RemoveAll(tempDir)

	deviceConfigPaths := []string{
		"/etc/timezone",
		"/etc/localtime",
		"/etc/systemd/timesyncd.conf",
		"/etc/systemd/ntp-units.d/10-aipc-timesyncd.list",
		"/usr/local/share/ca-certificates",
		"/home/root/apps/resources/final_calibration.json",
	}
	// /etc/resolv.conf is safe to restore only when the commissioned system
	// owns it as a regular file. If it is a systemd-resolved symlink, the new
	// OS image must keep its resolver topology and regenerate the target.
	if regularRootedFile(r.BackupSourceRoot, "/etc/resolv.conf") {
		deviceConfigPaths = append(deviceConfigPaths, "/etc/resolv.conf")
	}

	groups := []backupGroup{
		{
			name:  "systemd.tar.gz",
			paths: append(aipcUnitPaths("/etc/systemd/system"), aipcUnitPaths("/lib/systemd/system")...),
		},
		{
			name: "network.tar.gz",
			paths: []string{
				"/etc/systemd/network",
				"/etc/NetworkManager/system-connections",
				"/etc/network/interfaces",
				"/etc/network/interfaces.d",
				"/etc/hostname",
				"/etc/hosts",
			},
		},
		{
			name: "ssh.tar.gz",
			paths: []string{
				"/home/root/.ssh",
				"/root/.ssh",
				"/etc/ssh/sshd_config",
				"/etc/ssh/sshd_config.d",
				"/etc/ssh/ssh_host_rsa_key",
				"/etc/ssh/ssh_host_rsa_key.pub",
				"/etc/ssh/ssh_host_ecdsa_key",
				"/etc/ssh/ssh_host_ecdsa_key.pub",
				"/etc/ssh/ssh_host_ed25519_key",
				"/etc/ssh/ssh_host_ed25519_key.pub",
			},
		},
		{
			name:  "device-config.tar.gz",
			paths: deviceConfigPaths,
		},
	}

	manifest := backupManifest{
		FormatVersion: 1,
		JobID:         job.ID,
		CreatedAt:     time.Now().UTC(),
		OSVersion:     readOSVersion(r.OSReleasePath),
	}
	var checksumLines []string
	appManifestSource := rootedPath(r.BackupSourceRoot, r.AppManifestPath)
	appManifest, err := LoadAppManifest(appManifestSource)
	if err != nil {
		return fmt.Errorf("read App compatibility manifest: %w", err)
	}
	appManifestData, err := json.MarshalIndent(appManifest, "", "  ")
	if err != nil {
		return err
	}
	appManifestPath := filepath.Join(tempDir, "app-manifest.json")
	if err := os.WriteFile(appManifestPath, append(appManifestData, '\n'), 0600); err != nil {
		return err
	}
	appManifestSHA, appManifestSize, err := backupFileSHA256(appManifestPath)
	if err != nil {
		return err
	}
	manifest.Archives = append(manifest.Archives, backupArchive{
		Name: "app-manifest.json", SHA256: appManifestSHA, Size: appManifestSize,
	})
	checksumLines = append(checksumLines, appManifestSHA+"  app-manifest.json")
	for _, group := range groups {
		archivePath := filepath.Join(tempDir, group.name)
		written, err := createTarGzip(archivePath, r.BackupSourceRoot, group.paths, group.exclude)
		if err != nil {
			return fmt.Errorf("create %s: %w", group.name, err)
		}
		if !written {
			continue
		}
		sum, size, err := backupFileSHA256(archivePath)
		if err != nil {
			return err
		}
		manifest.Archives = append(manifest.Archives, backupArchive{Name: group.name, SHA256: sum, Size: size})
		checksumLines = append(checksumLines, sum+"  "+group.name)
	}
	data, err := json.MarshalIndent(manifest, "", "  ")
	if err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(tempDir, "manifest.json"), append(data, '\n'), 0600); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(tempDir, "SHA256SUMS"), []byte(strings.Join(checksumLines, "\n")+"\n"), 0600); err != nil {
		return err
	}
	if err := os.WriteFile(filepath.Join(tempDir, "status"), []byte("ready\n"), 0600); err != nil {
		return err
	}
	syncPath(tempDir)
	if err := os.RemoveAll(finalDir); err != nil {
		return err
	}
	if err := os.Rename(tempDir, finalDir); err != nil {
		return err
	}
	syncPath(root)
	return r.activateBackup(root, job.ID)
}

func rootedPath(root, path string) string {
	if root == "" || root == "/" {
		return filepath.Clean(path)
	}
	return filepath.Join(filepath.Clean(root), strings.TrimPrefix(filepath.Clean(path), "/"))
}

func (r *Runner) activateBackup(root, jobID string) error {
	linkTemp := filepath.Join(root, ".current-"+jobID)
	_ = os.Remove(linkTemp)
	if err := os.Symlink(jobID, linkTemp); err != nil {
		return err
	}
	if err := os.Rename(linkTemp, filepath.Join(root, "current")); err != nil {
		_ = os.Remove(linkTemp)
		return err
	}
	syncPath(root)
	return nil
}

func (r *Runner) deactivateBackup(jobID string) error {
	if strings.TrimSpace(r.BackupRoot) == "" {
		return nil
	}
	root := filepath.Clean(r.BackupRoot)
	current := filepath.Join(root, "current")
	target, err := os.Readlink(current)
	if os.IsNotExist(err) {
		return nil
	}
	if err != nil {
		return err
	}
	if target != jobID {
		return nil
	}
	if err := os.Remove(current); err != nil && !os.IsNotExist(err) {
		return err
	}
	syncPath(root)
	return nil
}

func aipcUnitPaths(root string) []string {
	// Application units track the independently deployed App version. The OS
	// image owns only restore/firstboot/autostart/verify and must not receive an
	// older copy of that boot control plane.
	names := []string{
		"aipc-healthmon.service", "aipc-logrotate.service", "aipc-logrotate.timer",
		"aipc-os-reboot.service", "aipc-os-updater.service",
		"event-bus.service", "camera-daemon.service",
		"ai-runtime.service", "device-control.service", "device-discovery.service",
		"platform-api.service", "app-manager.service",
	}
	paths := make([]string, 0, len(names)*2)
	for _, name := range names {
		paths = append(paths, filepath.Join(root, name), filepath.Join(root, "multi-user.target.wants", name))
	}
	return paths
}

func regularRootedFile(root, path string) bool {
	info, err := os.Lstat(rootedPath(root, path))
	if err != nil {
		return false
	}
	return info.Mode().IsRegular()
}

func createTarGzip(destination, sourceRoot string, paths []string, exclude func(string) bool) (bool, error) {
	if sourceRoot == "" {
		sourceRoot = "/"
	}
	sourceRoot = filepath.Clean(sourceRoot)
	var existing []string
	for _, path := range paths {
		full := filepath.Join(sourceRoot, strings.TrimPrefix(filepath.Clean(path), "/"))
		if _, err := os.Lstat(full); err == nil {
			existing = append(existing, full)
		} else if !os.IsNotExist(err) {
			return false, err
		}
	}
	if len(existing) == 0 {
		return false, nil
	}
	sort.Strings(existing)

	output, err := os.OpenFile(destination, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0600)
	if err != nil {
		return false, err
	}
	success := false
	defer func() {
		_ = output.Close()
		if !success {
			_ = os.Remove(destination)
		}
	}()
	gzipWriter := gzip.NewWriter(output)
	tarWriter := tar.NewWriter(gzipWriter)
	for _, root := range existing {
		err = filepath.WalkDir(root, func(path string, entry fs.DirEntry, walkErr error) error {
			if walkErr != nil {
				return walkErr
			}
			relative, relativeErr := filepath.Rel(sourceRoot, path)
			if relativeErr != nil || relative == "." || strings.HasPrefix(relative, "..") {
				if relativeErr != nil {
					return relativeErr
				}
				if relative == "." {
					return nil
				}
				return fmt.Errorf("backup path %s escapes source root %s", path, sourceRoot)
			}
			logical := "/" + filepath.ToSlash(relative)
			if exclude != nil && exclude(logical) {
				if entry.IsDir() {
					return filepath.SkipDir
				}
				return nil
			}
			info, statErr := os.Lstat(path)
			if statErr != nil {
				return statErr
			}
			if info.Mode()&(os.ModeSocket|os.ModeDevice|os.ModeNamedPipe) != 0 {
				return nil
			}
			link := ""
			if info.Mode()&os.ModeSymlink != 0 {
				link, statErr = os.Readlink(path)
				if statErr != nil {
					return statErr
				}
			}
			header, headerErr := tar.FileInfoHeader(info, link)
			if headerErr != nil {
				return headerErr
			}
			header.Name = strings.TrimPrefix(logical, "/")
			if headerErr = tarWriter.WriteHeader(header); headerErr != nil {
				return headerErr
			}
			if !info.Mode().IsRegular() {
				return nil
			}
			input, openErr := os.Open(path)
			if openErr != nil {
				return openErr
			}
			_, copyErr := io.Copy(tarWriter, input)
			closeErr := input.Close()
			if copyErr != nil {
				return copyErr
			}
			return closeErr
		})
		if err != nil {
			_ = tarWriter.Close()
			_ = gzipWriter.Close()
			return false, err
		}
	}
	if err := tarWriter.Close(); err != nil {
		return false, err
	}
	if err := gzipWriter.Close(); err != nil {
		return false, err
	}
	if err := output.Sync(); err != nil {
		return false, err
	}
	if err := output.Close(); err != nil {
		return false, err
	}
	success = true
	return true, nil
}

func backupFileSHA256(path string) (string, int64, error) {
	file, err := os.Open(path)
	if err != nil {
		return "", 0, err
	}
	defer file.Close()
	hash := sha256.New()
	size, err := io.Copy(hash, file)
	if err != nil {
		return "", 0, err
	}
	return hex.EncodeToString(hash.Sum(nil)), size, nil
}

func readOSVersion(path string) string {
	data, err := os.ReadFile(path)
	if err != nil {
		return ""
	}
	values := make(map[string]string)
	for _, line := range strings.Split(string(data), "\n") {
		key, value, ok := strings.Cut(line, "=")
		if ok {
			values[strings.TrimSpace(key)] = strings.Trim(strings.TrimSpace(value), `"'`)
		}
	}
	for _, key := range []string{"IMAGE_VERSION", "VERSION_ID", "VERSION"} {
		if values[key] != "" {
			return values[key]
		}
	}
	return ""
}
