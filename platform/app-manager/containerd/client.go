/**
 * @file client.go
 * @brief Containerd Client Wrapper
 *
 * Provides a high-level interface for containerd operations.
 */

package containerd

import (
	"context"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"aipc/platform/common/logger"

	"github.com/containerd/containerd"
	"github.com/containerd/containerd/cio"
	"github.com/containerd/containerd/errdefs"
	"github.com/containerd/containerd/images"
	"github.com/containerd/containerd/mount"
	"github.com/containerd/containerd/namespaces"
	"github.com/containerd/containerd/oci"
	"github.com/containerd/containerd/runtime/v2/runc/options"
	"github.com/containerd/containerd/snapshots"
	"github.com/opencontainers/image-spec/identity"
	specs "github.com/opencontainers/runtime-spec/specs-go"
)

// Client wraps containerd client with app-manager specific operations
type Client struct {
	client    *containerd.Client
	namespace string
	rootDir   string // containerd root directory (e.g., /data/aipc-data/containerd)
}

// SavedImagesDir is the persistent archive directory used to rebuild
// containerd image metadata after an OS upgrade or storage repair.
const SavedImagesDir = "/data/apps/images"

// SavedImageTarPath returns the recovery archive path for an installed app.
func SavedImageTarPath(appID string) string {
	return filepath.Join(SavedImagesDir, appID+".tar")
}

// SavedImageDigestsPath returns the sidecar recorded next to a recovery
// archive at publication time. It maps each image reference to the digest
// containerd held when the archive was saved, proving whether the archive
// still matches the images behind an app's current references.
func SavedImageDigestsPath(appID string) string {
	return SavedImageTarPath(appID) + ".digests.json"
}

// ContainerInfo holds information about a running container
type ContainerInfo struct {
	ID        string
	Image     string
	Status    string
	Pid       uint32
	CreatedAt time.Time
}

// ContainerStats holds resource statistics for a container
type ContainerStats struct {
	CPUUsage    uint64  // Total CPU usage in nanoseconds
	CPUPercent  float64 // CPU usage percentage
	MemoryUsage uint64  // Memory usage in bytes
	MemoryLimit uint64  // Memory limit in bytes
	MemoryRSS   uint64  // RSS memory in bytes
	Pids        uint64  // Number of PIDs
}

// cpuUsageTracker tracks CPU usage for percentage calculation
type cpuUsageTracker struct {
	lastCPUUsage uint64
	lastTime     time.Time
}

var (
	cpuTrackers   = make(map[string]*cpuUsageTracker)
	cpuTrackersMu sync.RWMutex
)

// ImageInfo holds information about an image
type ImageInfo struct {
	ID        string
	Name      string
	Tags      []string
	Size      int64
	CreatedAt time.Time
	InUse     bool
}

// DiskUsage holds disk usage statistics
type DiskUsage struct {
	ImagesSize      int64
	ContainersSize  int64
	SnapshotsSize   int64
	TotalSize       int64
	ReclaimableSize int64
}

// NewClient creates a new containerd client
func NewClient(address, namespace string) (*Client, error) {
	client, err := containerd.New(address)
	if err != nil {
		return nil, fmt.Errorf("failed to connect to containerd: %w", err)
	}

	c := &Client{
		client:    client,
		namespace: namespace,
	}

	// Detect containerd root directory from config or default path
	c.rootDir = detectContainerdRoot()

	logger.Info("Connected to containerd: address=%s, namespace=%s, root=%s", address, namespace, c.rootDir)
	return c, nil
}

// RootDir returns the containerd root directory path
func (c *Client) RootDir() string {
	return c.rootDir
}

// detectContainerdRoot finds the containerd root directory by reading its config
func detectContainerdRoot() string {
	// Try common config paths
	configPaths := []string{"/etc/containerd/config.toml", "/data/etc/containerd/config.toml"}
	for _, p := range configPaths {
		data, err := os.ReadFile(p)
		if err != nil {
			continue
		}
		for _, line := range strings.Split(string(data), "\n") {
			line = strings.TrimSpace(line)
			if strings.HasPrefix(line, "root") {
				parts := strings.SplitN(line, "=", 2)
				if len(parts) == 2 {
					root := strings.TrimSpace(parts[1])
					root = strings.Trim(root, "\"'")
					if _, err := os.Stat(root); err == nil {
						return root
					}
				}
			}
		}
	}

	// Check default paths
	defaults := []string{"/data/aipc-data/containerd", "/data/containerd", "/var/lib/containerd"}
	for _, d := range defaults {
		if _, err := os.Stat(d); err == nil {
			return d
		}
	}

	return "/var/lib/containerd"
}

// Close closes the containerd client
func (c *Client) Close() error {
	if c.client != nil {
		return c.client.Close()
	}
	return nil
}

// ImportImage imports a container image from a tar file
// Returns the image name that can be used to retrieve the image
func (c *Client) ImportImage(ctx context.Context, imagePath, imageName string) (string, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	file, err := os.Open(imagePath)
	if err != nil {
		return "", fmt.Errorf("failed to open image file: %w", err)
	}
	defer file.Close()

	// Import image
	imgs, err := c.client.Import(ctx, file)
	if err != nil {
		return "", fmt.Errorf("failed to import image: %w", err)
	}

	if len(imgs) == 0 {
		return "", fmt.Errorf("no images found in tar file")
	}

	// docker save archives can contain several images. Import registers every
	// image, but none is guaranteed to be unpacked, so prepare all of them. A
	// later reference reconciliation step maps the archived tags to the app
	// manifest's normalized names.
	for _, imported := range imgs {
		logger.Info("Image imported: %s", imported.Name)
		image, getErr := c.client.GetImage(ctx, imported.Name)
		if getErr != nil {
			return "", fmt.Errorf("failed to get imported image %s: %w", imported.Name, getErr)
		}
		if unpackErr := image.Unpack(ctx, "overlayfs"); unpackErr != nil {
			// Preserve the existing import contract: an already-unpacked image
			// can report an error here, and the definitive availability check is
			// performed by EnsureImageUnpacked below.
			logger.Warn("Failed to unpack image %s (may already be unpacked): %v", imported.Name, unpackErr)
		} else {
			logger.Info("Image unpacked successfully: %s", imported.Name)
		}
	}

	// The first imported image remains the primary return value for backwards
	// compatibility with the install API.
	img := imgs[0]

	// If the caller requested a specific image name, re-tag the imported image
	// so that spec.image in the manifest remains the authoritative reference.
	// A retag failure is fatal: if manifest.image cannot be created, StartApp
	// would later fail to resolve it (and on offline devices cannot pull).
	// Surface the error at install time instead of deferring it to start time.
	if imageName != "" && img.Name != imageName {
		if err := c.TagImage(ctx, img.Name, imageName); err != nil {
			return "", fmt.Errorf("failed to retag imported image %s -> %s (manifest.image): %w", img.Name, imageName, err)
		}
		logger.Info("Retagged imported image %s -> %s (manifest.image)", img.Name, imageName)
		return imageName, nil
	}

	return img.Name, nil
}

// TagImage creates a new tag for an existing image.
func (c *Client) TagImage(ctx context.Context, sourceRef, targetRef string) error {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	image, err := c.client.GetImage(ctx, sourceRef)
	if err != nil {
		return fmt.Errorf("failed to get source image: %w", err)
	}

	// Create a new image with the target reference name
	if _, err := c.client.ImageService().Create(ctx, images.Image{
		Name:   targetRef,
		Target: image.Target(),
	}); err != nil {
		// If tag already exists, update it
		if strings.Contains(err.Error(), "already exists") {
			existing, getErr := c.client.GetImage(ctx, targetRef)
			if getErr == nil {
				_ = c.client.ImageService().Delete(ctx, existing.Name())
			}
			if _, err := c.client.ImageService().Create(ctx, images.Image{
				Name:   targetRef,
				Target: image.Target(),
			}); err != nil {
				return fmt.Errorf("failed to re-tag image: %w", err)
			}
		} else {
			return fmt.Errorf("failed to tag image: %w", err)
		}
	}

	logger.Info("Tagged image: %s -> %s", sourceRef, targetRef)
	return nil
}

// EnsureImageUnpacked prepares an already-imported image for container
// creation. An unpack error is returned so callers never treat an unusable
// image record as a successful recovery.
func (c *Client) EnsureImageUnpacked(ctx context.Context, ref string) error {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	image, err := c.client.GetImage(ctx, ref)
	if err != nil {
		return fmt.Errorf("failed to get image %s: %w", ref, err)
	}
	unpacked, err := image.IsUnpacked(ctx, "overlayfs")
	if err != nil {
		return fmt.Errorf("failed to check unpack state for %s: %w", ref, err)
	}
	if unpacked {
		return nil
	}
	if err := image.Unpack(ctx, "overlayfs"); err != nil {
		return fmt.Errorf("failed to unpack image %s: %w", ref, err)
	}
	return nil
}

func stageImageTarAt(srcPath, destinationDir string) (string, error) {
	src, err := os.Open(srcPath)
	if err != nil {
		return "", fmt.Errorf("failed to open source: %w", err)
	}
	defer src.Close()

	if err := os.MkdirAll(destinationDir, 0755); err != nil {
		return "", fmt.Errorf("failed to create images dir: %w", err)
	}
	dst, err := os.CreateTemp(destinationDir, ".image-recovery-*.tmp")
	if err != nil {
		return "", fmt.Errorf("failed to create temporary archive: %w", err)
	}
	tmpPath := dst.Name()
	keep := false
	defer func() {
		if !keep {
			os.Remove(tmpPath)
		}
	}()

	if _, err := io.Copy(dst, src); err != nil {
		dst.Close()
		return "", fmt.Errorf("failed to copy: %w", err)
	}
	if err := dst.Sync(); err != nil {
		dst.Close()
		return "", fmt.Errorf("failed to sync temporary archive: %w", err)
	}
	if err := dst.Close(); err != nil {
		return "", fmt.Errorf("failed to close temporary archive: %w", err)
	}
	if err := os.Chmod(tmpPath, 0644); err != nil {
		return "", fmt.Errorf("failed to set archive mode: %w", err)
	}
	keep = true
	return tmpPath, nil
}

func publishStagedImageTarAt(stagedPath, dstPath string) error {
	if filepath.Dir(stagedPath) != filepath.Dir(dstPath) {
		return fmt.Errorf("staged archive must share destination directory")
	}
	if err := os.Rename(stagedPath, dstPath); err != nil {
		return fmt.Errorf("failed to publish archive: %w", err)
	}
	return nil
}

func saveImageTarAt(srcPath, dstPath string) error {
	stagedPath, err := stageImageTarAt(srcPath, filepath.Dir(dstPath))
	if err != nil {
		return err
	}
	defer os.Remove(stagedPath)
	return publishStagedImageTarAt(stagedPath, dstPath)
}

// StageImageTar performs the only large/fallible copy before an update stops
// the installed app or imports a replacement under its image reference.
func StageImageTar(appID, srcPath string) (string, error) {
	stagedPath, err := stageImageTarAt(srcPath, SavedImagesDir)
	if err != nil {
		return "", fmt.Errorf("stage recovery archive for %s: %w", appID, err)
	}
	return stagedPath, nil
}

// PublishStagedImageTar commits a fully synced same-directory archive with a
// single rename; no large copy remains after the running app is stopped.
func PublishStagedImageTar(appID, stagedPath string) error {
	if err := publishStagedImageTarAt(stagedPath, SavedImageTarPath(appID)); err != nil {
		return fmt.Errorf("publish recovery archive for %s: %w", appID, err)
	}
	logger.Info("Published staged image recovery archive: %s", SavedImageTarPath(appID))
	return nil
}

func DiscardStagedImageTar(stagedPath string) {
	if stagedPath != "" {
		_ = os.Remove(stagedPath)
	}
}

// SaveImageTar atomically replaces the persistent recovery archive. A failed
// update therefore cannot truncate the archive owned by the installed app.
func SaveImageTar(appID, srcPath string) error {
	dstPath := SavedImageTarPath(appID)
	if err := saveImageTarAt(srcPath, dstPath); err != nil {
		return err
	}

	logger.Info("Saved image tar for recovery: %s -> %s", srcPath, dstPath)
	return nil
}

// RemoveSavedImageTar invalidates local recovery when an install commits an
// image obtained from another source. Keeping the old archive would allow a
// later metadata repair to run an old image under the new reference.
func removeSavedImageTarAt(path string) error {
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("failed to remove stale recovery archive %s: %w", path, err)
	}
	return nil
}

func RemoveSavedImageTar(appID string) error {
	if err := removeSavedImageTarAt(SavedImageTarPath(appID)); err != nil {
		return err
	}
	// Drop the digest sidecar with the archive so a later archive never
	// inherits a binding recorded for different content.
	return removeSavedImageTarAt(SavedImageDigestsPath(appID))
}

// RepairResult describes the outcome of snapshot repair for one image.
type RepairResult struct {
	ImageName string
	Repaired  bool
	Error     error
}

// overlayfsMetadataDBPath returns the path to the overlayfs snapshotter's metadata.db
func (c *Client) overlayfsMetadataDBPath() string {
	return filepath.Join(c.rootDir, "io.containerd.snapshotter.v1.overlayfs", "metadata.db")
}

// overlayfsSnapshotsDir returns the path to the overlayfs snapshotter's snapshots directory
func (c *Client) overlayfsSnapshotsDir() string {
	return filepath.Join(c.rootDir, "io.containerd.snapshotter.v1.overlayfs", "snapshots")
}

// metaDBPath returns the path to the main containerd metadata bolt DB
func (c *Client) metaDBPath() string {
	return filepath.Join(c.rootDir, "io.containerd.metadata.v1.bolt", "meta.db")
}

// isBoltDBValid checks if a bolt DB file has a valid header.
// Bolt DB meta pages start with a non-zero magic number. All-zero means corruption.
func isBoltDBValid(path string) bool {
	f, err := os.Open(path)
	if err != nil {
		return false
	}
	defer f.Close()

	buf := make([]byte, 4096)
	n, err := f.Read(buf)
	if err != nil || n < 4096 {
		return false
	}

	// Bolt DB page 0 meta: bytes 0-3 should be a valid page ID (typically 0),
	// bytes 8-11 should contain flags, and bytes 12-15 contain the version.
	// A completely zeroed page indicates corruption.
	allZero := true
	for _, b := range buf[:32] {
		if b != 0 {
			allZero = false
			break
		}
	}
	return !allZero
}

// HasSnapshotCorruption checks for overlayfs snapshot corruption by:
// 1. Validating the overlayfs snapshotter's metadata.db bolt DB header
// 2. Checking if images are unpacked (via snapshotter.Stat)
func (c *Client) HasSnapshotCorruption(ctx context.Context) (bool, error) {
	// Check overlayfs metadata.db file integrity
	metadataDBPath := c.overlayfsMetadataDBPath()
	if _, err := os.Stat(metadataDBPath); err == nil {
		if !isBoltDBValid(metadataDBPath) {
			logger.Warn("[SELF-HEAL] Overlayfs metadata.db is corrupted (invalid bolt DB header)")
			return true, nil
		}
	}

	ctx = namespaces.WithNamespace(ctx, c.namespace)

	imgs, err := c.client.ListImages(ctx)
	if err != nil {
		return false, fmt.Errorf("failed to list images: %w", err)
	}

	for _, img := range imgs {
		unpacked, err := img.IsUnpacked(ctx, "overlayfs")
		if err != nil {
			return true, nil
		}
		if !unpacked {
			return true, nil
		}
	}

	return false, nil
}

// CheckAndRepairSnapshots repairs corrupted overlayfs snapshots.
// When corruption is detected, it nukes the overlayfs snapshotter state
// (metadata.db + snapshot dirs) and the main metadata DB's snapshot entries,
// then re-unpacks all images from the content store.
func (c *Client) CheckAndRepairSnapshots(ctx context.Context) ([]RepairResult, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	// Phase 1: Clean up overlayfs snapshotter state
	c.nukeOverlayfsSnapshotter()

	// Phase 2: Clean snapshot entries from main meta.db by removing and
	// re-creating the metadata DB. Content store blobs are preserved.
	c.nukeMetadataDB()

	// Phase 3: Reconnect to containerd (it will recreate fresh DBs)
	if err := c.client.Close(); err != nil {
		logger.Warn("[SELF-HEAL] Error closing containerd client: %v", err)
	}

	// Restart containerd to pick up clean state
	logger.Info("[SELF-HEAL] Restarting containerd to apply clean state")
	if err := exec.Command("systemctl", "restart", "containerd").Run(); err != nil {
		return nil, fmt.Errorf("failed to restart containerd: %w", err)
	}
	time.Sleep(3 * time.Second)

	// Reconnect
	newClient, err := containerd.New("/run/containerd/containerd.sock")
	if err != nil {
		return nil, fmt.Errorf("failed to reconnect to containerd: %w", err)
	}
	c.client = newClient

	// Phase 4: Re-import images from saved tar files
	return c.reimportAllImages(ctx)
}

// nukeOverlayfsSnapshotter deletes the overlayfs snapshotter's metadata.db
// and all snapshot directories.
func (c *Client) nukeOverlayfsSnapshotter() {
	metadataDBPath := c.overlayfsMetadataDBPath()
	snapshotsDir := c.overlayfsSnapshotsDir()

	// Delete metadata.db
	if err := os.Remove(metadataDBPath); err != nil && !os.IsNotExist(err) {
		logger.Warn("[SELF-HEAL] Failed to delete %s: %v", metadataDBPath, err)
	}

	// Delete all snapshot directories
	entries, err := os.ReadDir(snapshotsDir)
	if err != nil {
		logger.Warn("[SELF-HEAL] Failed to read snapshots dir: %v", err)
		return
	}
	for _, e := range entries {
		if e.IsDir() {
			os.RemoveAll(filepath.Join(snapshotsDir, e.Name()))
		}
	}
	logger.Info("[SELF-HEAL] Cleaned overlayfs snapshotter: deleted metadata.db and %d snapshot dirs", len(entries))
}

// nukeMetadataDB deletes the main containerd metadata DB.
// Content store blobs on disk are preserved. After restart, containerd
// recreates meta.db and images are re-imported from saved tar files.
func (c *Client) nukeMetadataDB() {
	metaDBPath := c.metaDBPath()
	if err := os.Remove(metaDBPath); err != nil && !os.IsNotExist(err) {
		logger.Warn("[SELF-HEAL] Failed to delete %s: %v", metaDBPath, err)
	}
	logger.Info("[SELF-HEAL] Deleted main metadata DB: %s", metaDBPath)
}

// reimportAllImages re-imports images from saved tar files in /data/apps/images/
func (c *Client) reimportAllImages(ctx context.Context) ([]RepairResult, error) {
	entries, err := os.ReadDir(SavedImagesDir)
	if err != nil {
		return nil, fmt.Errorf("no saved image tar files found: %w", err)
	}

	var results []RepairResult
	for _, e := range entries {
		if e.IsDir() || !strings.HasSuffix(e.Name(), ".tar") {
			continue
		}
		tarPath := filepath.Join(SavedImagesDir, e.Name())
		appID := strings.TrimSuffix(e.Name(), ".tar")

		logger.Info("[SELF-HEAL] Re-importing image from %s", tarPath)

		ctx = namespaces.WithNamespace(ctx, c.namespace)
		file, err := os.Open(tarPath)
		if err != nil {
			results = append(results, RepairResult{ImageName: appID, Error: fmt.Errorf("open tar: %w", err)})
			continue
		}

		imgs, err := c.client.Import(ctx, file)
		file.Close()
		if err != nil {
			results = append(results, RepairResult{ImageName: appID, Error: fmt.Errorf("import: %w", err)})
			continue
		}

		if len(imgs) == 0 {
			results = append(results, RepairResult{ImageName: appID, Error: fmt.Errorf("no images in tar")})
			continue
		}

		img := imgs[0]
		image, err := c.client.GetImage(ctx, img.Name)
		if err != nil {
			results = append(results, RepairResult{ImageName: appID, Error: fmt.Errorf("get image: %w", err)})
			continue
		}

		if err := image.Unpack(ctx, "overlayfs"); err != nil {
			results = append(results, RepairResult{ImageName: img.Name, Error: fmt.Errorf("unpack: %w", err)})
			continue
		}

		logger.Info("[SELF-HEAL] Re-imported and unpacked: %s", img.Name)
		results = append(results, RepairResult{ImageName: img.Name, Repaired: true})
	}

	return results, nil
}

// PullImage pulls a container image from a remote registry (e.g. Docker Hub)
// ref should be a standard image reference like "docker.io/library/nginx:latest" or "nginx:latest"
func (c *Client) PullImage(ctx context.Context, ref string) error {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	// Normalize short Docker Hub references:
	//   "nginx"         → "docker.io/library/nginx:latest"
	//   "nginx:1.25"    → "docker.io/library/nginx:1.25"
	//   "myuser/myimg"  → "docker.io/myuser/myimg:latest"
	ref = normalizeImageRef(ref)

	logger.Info("Pulling image: %s", ref)

	// Pull with automatic unpack using the overlayfs snapshotter
	image, err := c.client.Pull(ctx, ref,
		containerd.WithPullUnpack,
		containerd.WithPullSnapshotter("overlayfs"),
	)
	if err != nil {
		return fmt.Errorf("failed to pull image %s: %w", ref, err)
	}

	logger.Info("Image pulled and unpacked: %s (digest: %s)", image.Name(), image.Target().Digest)
	return nil
}

// PullProgress represents the progress of an image pull operation.
type PullProgress struct {
	Phase       string  // "pulling", "unpacking", "complete", "error"
	Percent     float64 // 0-100
	Message     string
	BytesPulled int64
	BytesTotal  int64
	Error       string
}

// PullImageAsync pulls an image in the background and sends progress updates to progressCh.
// The caller should read from progressCh until it is closed.
func (c *Client) PullImageAsync(ctx context.Context, ref string, progressCh chan<- PullProgress) {
	defer close(progressCh)

	ref = normalizeImageRef(ref)
	logger.Info("Async pulling image: %s", ref)

	pullCtx, pullCancel := context.WithCancel(namespaces.WithNamespace(context.Background(), c.namespace))
	defer pullCancel()

	// Progress reporter: reads ContentStore statuses every 500ms
	// ListStatuses only returns ACTIVE ingests — completed layers are removed.
	// We track peakTotal to keep a stable denominator as layers finish.
	doneCh := make(chan struct{})
	go func() {
		ticker := time.NewTicker(500 * time.Millisecond)
		defer ticker.Stop()
		var peakTotal int64
		for {
			select {
			case <-ticker.C:
				statuses, err := c.client.ContentStore().ListStatuses(pullCtx, "")
				if err != nil {
					continue
				}
				var activeTotal, activePulled int64
				for _, s := range statuses {
					activeTotal += s.Total
					activePulled += s.Offset
				}
				if activeTotal > peakTotal {
					peakTotal = activeTotal
				}
				// remaining = bytes still needed for active layers
				// downloaded = peak - remaining (completed layers + partial active)
				remaining := activeTotal - activePulled
				downloaded := peakTotal - remaining
				pct := float64(0)
				if peakTotal > 0 {
					pct = float64(downloaded) / float64(peakTotal) * 100
					if pct > 99 {
						pct = 99
					}
				}
				select {
				case progressCh <- PullProgress{
					Phase:       "pulling",
					Percent:     pct,
					Message:     fmt.Sprintf("Downloading %.1f%% (%s / %s)", pct, formatBytes(downloaded), formatBytes(peakTotal)),
					BytesPulled: downloaded,
					BytesTotal:  peakTotal,
				}:
				default:
				}
			case <-doneCh:
				return
			}
		}
	}()

	// Blocking pull
	image, err := c.client.Pull(pullCtx, ref,
		containerd.WithPullUnpack,
		containerd.WithPullSnapshotter("overlayfs"),
	)
	close(doneCh)

	if err != nil {
		progressCh <- PullProgress{Phase: "error", Error: err.Error(), Message: fmt.Sprintf("Failed: %v", err)}
		return
	}

	progressCh <- PullProgress{
		Phase:   "complete",
		Percent: 100,
		Message: fmt.Sprintf("Pulled %s (digest: %s)", ref, image.Target().Digest),
	}
}

func formatBytes(b int64) string {
	const (
		KB = 1024
		MB = KB * 1024
		GB = MB * 1024
	)
	switch {
	case b >= GB:
		return fmt.Sprintf("%.1f GB", float64(b)/float64(GB))
	case b >= MB:
		return fmt.Sprintf("%.1f MB", float64(b)/float64(MB))
	case b >= KB:
		return fmt.Sprintf("%.1f KB", float64(b)/float64(KB))
	default:
		return fmt.Sprintf("%d B", b)
	}
}

// normalizeImageRef ensures the reference is fully qualified for containerd.
// Docker CLI auto-prepends "docker.io/library/" for official images; containerd does not.
func normalizeImageRef(ref string) string {
	// Already fully qualified (contains a dot in the registry part)
	if strings.Contains(strings.SplitN(ref, "/", 2)[0], ".") {
		// Ensure tag exists
		if !strings.Contains(ref, ":") && !strings.Contains(ref, "@") {
			ref += ":latest"
		}
		return ref
	}

	// No registry specified — assume Docker Hub
	parts := strings.SplitN(ref, "/", 2)
	if len(parts) == 1 {
		// Official image: "nginx" → "docker.io/library/nginx"
		ref = "docker.io/library/" + ref
	} else {
		// User image: "myuser/myimg" → "docker.io/myuser/myimg"
		ref = "docker.io/" + ref
	}

	// Ensure tag
	if !strings.Contains(ref, ":") && !strings.Contains(ref, "@") {
		ref += ":latest"
	}

	return ref
}

// GetImage retrieves an image by name
func (c *Client) GetImage(ctx context.Context, imageName string) (containerd.Image, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)
	return c.client.GetImage(ctx, imageName)
}

// ExtractFileFromImage copies a single regular file out of an image's rootfs
// into destDir and returns the destination path. Used by the install flow to
// pull app-bundled model files (spec.models.<alias>.path) out of the app
// image so they can be registered with ai-runtime as transient models.
//
// The image rootfs is exposed through a readonly overlayfs view snapshot,
// mounted at a temporary mount point, and both are always released on exit.
// containerPath must be absolute and clean (validated by the manifest layer);
// symlinks and non-regular files are rejected so an in-image symlink cannot
// resolve against the host filesystem.
func (c *Client) ExtractFileFromImage(ctx context.Context, imageRef, containerPath, destDir string) (string, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	image, err := c.client.GetImage(ctx, imageRef)
	if err != nil {
		return "", fmt.Errorf("failed to get image %s: %w", imageRef, err)
	}

	// Ensure the layer chain has snapshots. Both install paths (PullImage /
	// ImportImage) already unpack, so this is normally cheap; like
	// ImportImage, treat an unpack failure as non-fatal and let Prepare
	// decide (the image may already be unpacked).
	if err := image.Unpack(ctx, "overlayfs"); err != nil {
		logger.Warn("ExtractFileFromImage: unpack reported an error for %s (may already be unpacked): %v", imageRef, err)
	}

	diffIDs, err := image.RootFS(ctx)
	if err != nil {
		return "", fmt.Errorf("failed to resolve image rootfs for %s: %w", imageRef, err)
	}
	parent := identity.ChainID(diffIDs).String()

	snapshotter := c.client.SnapshotService("overlayfs")
	viewKey := fmt.Sprintf("extract-%s-%d", parent, time.Now().UnixNano())
	// Registered before the unmount defer so it runs last (LIFO): the mount
	// must be released before the backing view snapshot is removed.
	defer func() {
		if err := snapshotter.Remove(ctx, viewKey); err != nil {
			logger.Warn("ExtractFileFromImage: failed to remove view snapshot %s: %v", viewKey, err)
		}
	}()

	mounts, err := snapshotter.View(ctx, viewKey, parent)
	if err != nil {
		return "", fmt.Errorf("failed to create readonly view of image %s: %w", imageRef, err)
	}

	if err := os.MkdirAll(destDir, 0755); err != nil {
		return "", fmt.Errorf("failed to create extraction dir %s: %w", destDir, err)
	}
	mountPoint := filepath.Join(destDir, ".extract-mount")
	if err := os.MkdirAll(mountPoint, 0700); err != nil {
		return "", fmt.Errorf("failed to create mount point %s: %w", mountPoint, err)
	}
	defer func() {
		if err := mount.UnmountAll(mountPoint, 0); err != nil {
			logger.Warn("ExtractFileFromImage: failed to unmount %s: %v", mountPoint, err)
		}
		if err := os.Remove(mountPoint); err != nil && !os.IsNotExist(err) {
			logger.Warn("ExtractFileFromImage: failed to remove mount point %s: %v", mountPoint, err)
		}
	}()

	if err := mount.All(mounts, mountPoint); err != nil {
		return "", fmt.Errorf("failed to mount image rootfs at %s: %w", mountPoint, err)
	}

	src := filepath.Join(mountPoint, containerPath)
	srcInfo, err := os.Lstat(src)
	if err != nil {
		return "", fmt.Errorf("file %s not found in image %s: %w", containerPath, imageRef, err)
	}
	if !srcInfo.Mode().IsRegular() {
		// Rejects symlinks (which would resolve against the host), dirs and
		// devices alike: a bundled model must be a plain file in the image.
		return "", fmt.Errorf("%s in image %s is not a regular file (mode %s)", containerPath, imageRef, srcInfo.Mode())
	}

	destPath := filepath.Join(destDir, filepath.Base(containerPath))
	if err := copyFile(destPath, src, srcInfo.Mode().Perm()); err != nil {
		return "", fmt.Errorf("failed to extract %s from image %s: %w", containerPath, imageRef, err)
	}
	logger.Info("Extracted %s from image %s to %s (%d bytes)", containerPath, imageRef, destPath, srcInfo.Size())
	return destPath, nil
}

// copyFile streams src into dst using the given permission bits.
func copyFile(dst, src string, perm os.FileMode) error {
	srcFile, err := os.Open(src)
	if err != nil {
		return fmt.Errorf("open source: %w", err)
	}
	defer srcFile.Close()

	dstFile, err := os.OpenFile(dst, os.O_CREATE|os.O_WRONLY|os.O_TRUNC, perm)
	if err != nil {
		return fmt.Errorf("create destination: %w", err)
	}
	defer dstFile.Close()

	if _, err := io.Copy(dstFile, srcFile); err != nil {
		return fmt.Errorf("copy contents: %w", err)
	}
	return dstFile.Sync()
}

// CreateContainer creates a new container with the specified configuration
func (c *Client) CreateContainer(ctx context.Context, containerID, imageName string, opts ...oci.SpecOpts) (containerd.Container, error) {

	ctx = namespaces.WithNamespace(ctx, c.namespace)

	// 1. Get image
	image, err := c.client.GetImage(ctx, imageName)
	if err != nil {
		return nil, fmt.Errorf("failed to get image: %w", err)
	}

	// 2. Build spec options
	specOpts := []oci.SpecOpts{
		oci.WithImageConfig(image),
		// Configure cgroup path
		oci.WithCgroup(fmt.Sprintf("system.slice:group:%s", containerID)),
	}
	// Append external opts
	specOpts = append(specOpts, opts...)

	// 3. Configure runc options
	runcOptions := &options.Options{
		SystemdCgroup: true,
	}

	// 4. Create container
	// Clean up stale snapshot from a previous install/uninstall cycle.
	// UninstallApp removes the container and image but containerd may leave
	// the named snapshot behind, causing WithNewSnapshot to fail with
	// "already exists". Remove it proactively so the create always succeeds.
	snapshotName := containerID + "-snapshot"
	snapshotter := c.client.SnapshotService("overlayfs")
	if _, statErr := snapshotter.Stat(ctx, snapshotName); statErr == nil {
		if removeErr := snapshotter.Remove(ctx, snapshotName); removeErr != nil {
			logger.Warn("Failed to remove stale snapshot %s: %v", snapshotName, removeErr)
		} else {
			logger.Info("Removed stale snapshot %s before container creation", snapshotName)
		}
	}

	container, err := c.client.NewContainer(
		ctx,
		containerID,
		containerd.WithImage(image),
		containerd.WithNewSnapshot(snapshotName, image),
		containerd.WithNewSpec(specOpts...),

		containerd.WithRuntime("io.containerd.runc.v2", runcOptions),
	)
	if err != nil {
		return nil, fmt.Errorf("failed to create container: %w", err)
	}

	return container, nil
}

// StartContainer starts a container task
func (c *Client) StartContainer(ctx context.Context, container containerd.Container, logPath string) (containerd.Task, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	// Create IO
	var taskIO cio.Creator
	if logPath != "" {
		// Create log file
		logFile, err := os.OpenFile(logPath, os.O_CREATE|os.O_WRONLY|os.O_APPEND, 0644)
		if err != nil {
			return nil, fmt.Errorf("failed to create log file: %w", err)
		}
		// Note: cio.WithStreams will handle closing the file when the task is deleted
		taskIO = cio.NewCreator(cio.WithStreams(nil, logFile, logFile))
	} else {
		taskIO = cio.NewCreator(cio.WithStdio)
	}

	// Create task
	task, err := container.NewTask(ctx, taskIO)
	if err != nil {
		return nil, fmt.Errorf("failed to create task: %w", err)
	}

	// Start task
	if err := task.Start(ctx); err != nil {
		// Cleanup task on failure
		if _, delErr := task.Delete(ctx); delErr != nil {
		}
		return nil, fmt.Errorf("failed to start task: %w", err)
	}

	return task, nil
}

// Force-kill budgets for StopContainer. The force-kill phase runs on an
// independent context so that a cancelled or expired caller context can never
// prevent the kill. Callers sizing their own stop deadlines should budget for
// graceful timeout + KillPhaseBudget in the worst case.
const (
	// KillOpTimeout bounds each individual containerd call (Kill/Delete) in
	// the force-kill phase.
	KillOpTimeout = 10 * time.Second
	// SigkillExitWait is how long a task is given to exit after SIGKILL
	// before deletion is escalated to WithProcessKill.
	SigkillExitWait = 5 * time.Second
	// KillPhaseBudget is the worst-case duration of the force-kill phase.
	KillPhaseBudget = KillOpTimeout + SigkillExitWait + KillOpTimeout
)

// killOpContext returns a fresh context for force-kill operations. It is
// deliberately not derived from the caller's context: the force-kill phase
// must complete exactly when the caller has already timed out.
func (c *Client) killOpContext() (context.Context, context.CancelFunc) {
	return context.WithTimeout(
		namespaces.WithNamespace(context.Background(), c.namespace),
		KillOpTimeout,
	)
}

// stopTask is the subset of containerd.Task used by the stop state machine.
// Keeping the state machine behind this narrow seam lets its timeout and
// escalation behavior be tested without a live containerd daemon.
type stopTask interface {
	ID() string
	Wait(context.Context) (<-chan containerd.ExitStatus, error)
	Kill(context.Context, syscall.Signal, ...containerd.KillOpts) error
	Delete(context.Context, ...containerd.ProcessDeleteOpts) (*containerd.ExitStatus, error)
}

// StopContainer stops a container task in two phases.
//
// Graceful phase: SIGTERM is delivered to every process in the container
// (WithKillAll) and the task is given `timeout` to exit on its own.
//
// Force-kill phase: SIGKILL is sent to the whole container on an independent
// context, the exit is verified, and — if the task still has not exited —
// task.Delete with WithProcessKill makes the shim kill whatever is left as
// part of deletion.
//
// A nil return means the task is gone from containerd. A non-nil return means
// the task could not be proven stopped; callers must not report success.
func (c *Client) StopContainer(ctx context.Context, task containerd.Task, timeout time.Duration) error {
	return c.stopContainer(ctx, task, timeout)
}

func (c *Client) stopContainer(ctx context.Context, task stopTask, timeout time.Duration) error {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	if timeout <= 0 {
		timeout = 10 * time.Second
	}

	// Get exit channel first (before sending signal)
	// This must be called before Kill() to properly wait for exit
	exitStatusC, err := task.Wait(ctx)
	if err != nil {
		// Task might already be deleted or in invalid state
		logger.Warn("Failed to get exit channel: %v, attempting force kill and delete", err)
		killCtx, killCancel := c.killOpContext()
		if killErr := task.Kill(killCtx, syscall.SIGKILL, containerd.WithKillAll); killErr != nil && !errdefs.IsNotFound(killErr) {
			logger.Warn("Failed to send SIGKILL: %v", killErr)
		}
		killCancel()
		// Try to delete task anyway
		delCtx, delCancel := c.killOpContext()
		defer delCancel()
		if _, delErr := task.Delete(delCtx, containerd.WithProcessKill); delErr != nil && !errdefs.IsNotFound(delErr) {
			return fmt.Errorf("failed to delete task: %w", delErr)
		}
		return nil
	}

	// Graceful phase: signal every process in the container, then wait.
	if killErr := task.Kill(ctx, syscall.SIGTERM, containerd.WithKillAll); killErr != nil && !errdefs.IsNotFound(killErr) {
		// Not fatal: the task may already be exiting, and the phases below
		// still drive it to a verified stop.
		logger.Warn("Failed to send SIGTERM: %v, continuing", killErr)
	}

	graceTimer := time.NewTimer(timeout)
	defer graceTimer.Stop()

	exited := false
	waitBroken := false
	select {
	case exitStatus := <-exitStatusC:
		if waitErr := exitStatus.Error(); waitErr != nil {
			// The wait itself failed (e.g. the task vanished): fall through
			// to the force-kill phase, which tolerates already-gone tasks.
			waitBroken = true
			logger.Warn("Task wait failed: %v, continuing with force kill", waitErr)
		} else {
			exited = true
			logger.Info("Task exited gracefully with status: %v", exitStatus)
		}
	case <-graceTimer.C:
		logger.Warn("Task did not exit within timeout (%v), sending SIGKILL", timeout)
	case <-ctx.Done():
		// The caller gave up on the graceful window; the force-kill phase
		// below still guarantees the task is stopped.
		logger.Warn("Stop context done before graceful exit (%v), sending SIGKILL", ctx.Err())
	}

	// Force-kill phase: runs on a context independent of the caller's so a
	// caller timeout cannot leave the task running.
	if !exited {
		killCtx, killCancel := c.killOpContext()
		defer killCancel()
		if killErr := task.Kill(killCtx, syscall.SIGKILL, containerd.WithKillAll); killErr != nil && !errdefs.IsNotFound(killErr) {
			// WithProcessKill deletion below is the remaining enforcement path.
			logger.Warn("Failed to send SIGKILL: %v, relying on WithProcessKill delete", killErr)
		}

		if !waitBroken {
			exitTimer := time.NewTimer(SigkillExitWait)
			defer exitTimer.Stop()
			select {
			case exitStatus := <-exitStatusC:
				logger.Info("Task exited after SIGKILL with status: %v", exitStatus)
			case <-exitTimer.C:
				logger.Warn("Task did not exit after SIGKILL, escalating to WithProcessKill delete")
			}
		}
	}

	// Delete the task so containerd stops tracking it; without this,
	// task.Status() may still report "running" for a dead process.
	// WithProcessKill makes the shim kill any process that is somehow still
	// alive as part of deletion. A fresh context is used because the caller's
	// may already be expired.
	delCtx, delCancel := c.killOpContext()
	defer delCancel()
	if _, delErr := task.Delete(delCtx, containerd.WithProcessKill); delErr != nil && !errdefs.IsNotFound(delErr) {
		return fmt.Errorf("task did not stop: force delete failed: %w", delErr)
	}

	logger.Info("Container stopped and task deleted: task=%s", task.ID())
	return nil
}

// RemoveContainer removes a container
func (c *Client) RemoveContainer(ctx context.Context, container containerd.Container) error {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	// Best-effort: ensure task is deleted (container.Delete won't remove running task)
	if task, err := container.Task(ctx, nil); err == nil {
		if _, delErr := task.Delete(ctx, containerd.WithProcessKill); delErr != nil {
			logger.Warn("Failed to delete task before container delete: %v", delErr)
		}
	}

	// IMPORTANT: also cleanup snapshot created by WithNewSnapshot(containerID+"-snapshot", ...)
	// Otherwise re-install/re-start may fail with snapshot already exists.
	if err := container.Delete(ctx, containerd.WithSnapshotCleanup); err != nil {
		return fmt.Errorf("failed to delete container: %w", err)
	}

	logger.Info("Container removed: id=%s", container.ID())
	return nil
}

// GetContainer retrieves a container by ID
func (c *Client) GetContainer(ctx context.Context, containerID string) (containerd.Container, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)
	return c.client.LoadContainer(ctx, containerID)
}

// GetContainerInfo retrieves information about a container
func (c *Client) GetContainerInfo(ctx context.Context, container containerd.Container) (*ContainerInfo, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	info := &ContainerInfo{
		ID: container.ID(),
	}

	// Get container info
	containerInfo, err := container.Info(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to get container info: %w", err)
	}

	info.Image = containerInfo.Image
	info.CreatedAt = containerInfo.CreatedAt

	// Get task if running
	task, err := container.Task(ctx, nil)
	if err == nil {
		status, err := task.Status(ctx)
		if err == nil {
			info.Status = string(status.Status)
			// Get PID from task
			if pid := task.Pid(); pid > 0 {
				info.Pid = uint32(pid)
			}
		}
	} else {
		info.Status = "stopped"
	}

	return info, nil
}

// GetContainerStats retrieves resource statistics for a container
func (c *Client) GetContainerStats(ctx context.Context, container containerd.Container) (*ContainerStats, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	stats := &ContainerStats{}
	containerID := container.ID()

	// Get task if running
	task, err := container.Task(ctx, nil)
	if err != nil {
		// Container not running, return empty stats and clear tracker
		delete(cpuTrackers, containerID)
		return stats, nil
	}

	status, err := task.Status(ctx)
	if err != nil {
		delete(cpuTrackers, containerID)
		return stats, nil
	}

	if status.Status != containerd.Running {
		delete(cpuTrackers, containerID)
		return stats, nil
	}

	// Read stats directly from cgroup v2 filesystem (more reliable than containerd metrics)
	// cgroup v2 path for containerd with systemd cgroup driver:
	// /sys/fs/cgroup/system.slice/group-<container-id>.scope/
	// Also try: /sys/fs/cgroup/system.slice/<container-id>/
	cgroupPaths := []string{
		fmt.Sprintf("/sys/fs/cgroup/system.slice/group-%s.scope", containerID),
		fmt.Sprintf("/sys/fs/cgroup/system.slice/%s", containerID),
		fmt.Sprintf("/sys/fs/cgroup/system.slice/%s.scope", containerID),
	}

	var cgroupPath string
	for _, p := range cgroupPaths {
		if _, err := os.Stat(p); err == nil {
			cgroupPath = p
			break
		}
	}

	if cgroupPath == "" {
		logger.Warn("Cgroup path not found for container: %s", containerID)
		return stats, nil
	}

	// Read CPU usage (cgroup v2: cpu.stat)
	if data, err := os.ReadFile(cgroupPath + "/cpu.stat"); err == nil {
		lines := strings.Split(string(data), "\n")
		for _, line := range lines {
			parts := strings.Fields(line)
			if len(parts) >= 2 {
				switch parts[0] {
				case "usage_usec":
					// Convert microseconds to nanoseconds
					if u, err := strconv.ParseUint(parts[1], 10, 64); err == nil {
						stats.CPUUsage = u * 1000
					}
				}
			}
		}
	}

	// Read memory usage (cgroup v2: memory.current, memory.max)
	if data, err := os.ReadFile(cgroupPath + "/memory.current"); err == nil {
		if u, err := strconv.ParseUint(strings.TrimSpace(string(data)), 10, 64); err == nil {
			stats.MemoryUsage = u
		}
	}
	if data, err := os.ReadFile(cgroupPath + "/memory.max"); err == nil {
		val := strings.TrimSpace(string(data))
		if val != "max" {
			if u, err := strconv.ParseUint(val, 10, 64); err == nil {
				stats.MemoryLimit = u
			}
		}
	}

	// Calculate CPU percentage
	if stats.CPUUsage > 0 {
		now := time.Now()
		cpuTrackersMu.Lock()
		if tracker, ok := cpuTrackers[containerID]; ok {
			timeDiff := now.Sub(tracker.lastTime).Nanoseconds()
			if timeDiff > 0 {
				cpuDiff := int64(stats.CPUUsage - tracker.lastCPUUsage)
				// CPU percentage = (cpu_time_diff / wall_time_diff / num_cores) * 100
				// Normalized to 0-100% regardless of core count
				numCPU := float64(runtime.NumCPU())
				stats.CPUPercent = float64(cpuDiff) / float64(timeDiff) / numCPU * 100
				if stats.CPUPercent < 0 {
					stats.CPUPercent = 0
				}
			}
		}
		// Update tracker
		cpuTrackers[containerID] = &cpuUsageTracker{
			lastCPUUsage: stats.CPUUsage,
			lastTime:     now,
		}
		cpuTrackersMu.Unlock()
	}

	// Read PIDs (cgroup v2: pids.current)
	if data, err := os.ReadFile(cgroupPath + "/pids.current"); err == nil {
		if u, err := strconv.ParseUint(strings.TrimSpace(string(data)), 10, 64); err == nil {
			stats.Pids = u
		}
	}

	return stats, nil
}

// ListContainers lists all containers
func (c *Client) ListContainers(ctx context.Context) ([]containerd.Container, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)
	return c.client.Containers(ctx)
}

// GetTaskLogs retrieves container logs
// Note: This is a simplified implementation. In production, logs should be read from log files
// that were configured when starting the container, not from task IO.
func (c *Client) GetTaskLogs(ctx context.Context, task containerd.Task, maxLines int32, follow bool) (<-chan string, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	logChan := make(chan string, 100)

	go func() {
		defer close(logChan)

		// For now, return empty logs
		// In a real implementation, logs should be read from the log file
		// that was specified when starting the container (see StartContainer)
		// This requires tracking log file paths per container
		logger.Warn("GetTaskLogs: Log reading from task IO not implemented. Use log files instead.")
	}()

	return logChan, nil
}

// ListImages lists all images in the namespace
func (c *Client) ListImages(ctx context.Context) ([]ImageInfo, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	imgs, err := c.client.ListImages(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to list images: %w", err)
	}

	// Get containers to check which images are in use
	containers, err := c.client.Containers(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to list containers: %w", err)
	}

	usedImages := make(map[string]bool)
	for _, container := range containers {
		img, err := container.Image(ctx)
		if err == nil {
			usedImages[img.Name()] = true
		}
	}

	var result []ImageInfo
	for _, img := range imgs {
		size, _ := img.Size(ctx)
		info := ImageInfo{
			ID:        img.Target().Digest.String(),
			Name:      img.Name(),
			Size:      size,
			CreatedAt: img.Metadata().CreatedAt,
			InUse:     usedImages[img.Name()],
		}
		result = append(result, info)
	}

	return result, nil
}

// RemoveImage removes an image by name or ID
// RemoveSnapshot removes a named overlayfs snapshot. This is used to clean up
// stale snapshots left behind after an uninstall, which would otherwise cause
// WithNewSnapshot to fail with "already exists" on the next install.
func (c *Client) RemoveSnapshot(ctx context.Context, snapshotName string) error {
	ctx = namespaces.WithNamespace(ctx, c.namespace)
	snapshotter := c.client.SnapshotService("overlayfs")
	return snapshotter.Remove(ctx, snapshotName)
}

func (c *Client) RemoveImage(ctx context.Context, imageID string, force bool) error {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	// Check if image is in use
	if !force {
		containers, err := c.client.Containers(ctx)
		if err != nil {
			return fmt.Errorf("failed to list containers: %w", err)
		}

		for _, container := range containers {
			img, err := container.Image(ctx)
			if err == nil && (img.Name() == imageID || img.Target().Digest.String() == imageID) {
				return fmt.Errorf("image is in use by container %s", container.ID())
			}
		}
	}

	// Try to find image by name or digest
	imgs, err := c.client.ListImages(ctx)
	if err != nil {
		return fmt.Errorf("failed to list images: %w", err)
	}

	imgService := c.client.ImageService()
	for _, img := range imgs {
		if img.Name() == imageID || img.Target().Digest.String() == imageID {
			if err := imgService.Delete(ctx, img.Name(), images.SynchronousDelete()); err != nil {
				return fmt.Errorf("failed to delete image: %w", err)
			}
			logger.Info("Deleted image: %s", img.Name())
			return nil
		}
	}

	return fmt.Errorf("image not found: %s", imageID)
}

// GetDiskUsage returns disk usage statistics
func (c *Client) GetDiskUsage(ctx context.Context) (*DiskUsage, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	usage := &DiskUsage{}

	// Calculate images size
	imgs, err := c.client.ListImages(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to list images: %w", err)
	}

	usedImages := make(map[string]bool)
	containers, _ := c.client.Containers(ctx)
	for _, container := range containers {
		img, err := container.Image(ctx)
		if err == nil {
			usedImages[img.Name()] = true
		}
	}

	for _, img := range imgs {
		size, _ := img.Size(ctx)
		usage.ImagesSize += size
		if !usedImages[img.Name()] {
			usage.ReclaimableSize += size
		}
	}

	// Calculate snapshots size
	snapshotter := c.client.SnapshotService("overlayfs")
	if snapshotter != nil {
		err := snapshotter.Walk(ctx, func(walkCtx context.Context, info snapshots.Info) error {
			su, err := snapshotter.Usage(ctx, info.Name)
			if err == nil {
				usage.SnapshotsSize += su.Size
			}
			return nil
		})
		if err != nil {
			logger.Warn("Failed to walk snapshots: %v", err)
		}
	}

	usage.TotalSize = usage.ImagesSize + usage.ContainersSize + usage.SnapshotsSize

	return usage, nil
}

// PruneContainers removes stopped containers
func (c *Client) PruneContainers(ctx context.Context) (int64, []string, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	var reclaimedSpace int64
	var deletedItems []string

	containers, err := c.client.Containers(ctx)
	if err != nil {
		return 0, nil, fmt.Errorf("failed to list containers: %w", err)
	}

	for _, container := range containers {
		task, err := container.Task(ctx, nil)
		if err != nil {
			// No task means container is stopped, can be pruned
			if err := container.Delete(ctx, containerd.WithSnapshotCleanup); err != nil {
				logger.Warn("Failed to delete container %s: %v", container.ID(), err)
				continue
			}
			deletedItems = append(deletedItems, "container:"+container.ID())
			logger.Info("Pruned container: %s", container.ID())
			continue
		}

		status, err := task.Status(ctx)
		if err != nil || status.Status == containerd.Stopped {
			task.Delete(ctx)
			if err := container.Delete(ctx, containerd.WithSnapshotCleanup); err != nil {
				logger.Warn("Failed to delete container %s: %v", container.ID(), err)
				continue
			}
			deletedItems = append(deletedItems, "container:"+container.ID())
			logger.Info("Pruned stopped container: %s", container.ID())
		}
	}

	return reclaimedSpace, deletedItems, nil
}

// PruneImages removes unused images
func (c *Client) PruneImages(ctx context.Context) (int64, []string, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	var reclaimedSpace int64
	var deletedItems []string

	// Get used images
	containers, err := c.client.Containers(ctx)
	if err != nil {
		return 0, nil, fmt.Errorf("failed to list containers: %w", err)
	}

	usedImages := make(map[string]bool)
	for _, container := range containers {
		img, err := container.Image(ctx)
		if err == nil {
			usedImages[img.Name()] = true
		}
	}

	// Delete unused images
	imgs, err := c.client.ListImages(ctx)
	if err != nil {
		return 0, nil, fmt.Errorf("failed to list images: %w", err)
	}

	imgService := c.client.ImageService()
	for _, img := range imgs {
		if !usedImages[img.Name()] {
			size, _ := img.Size(ctx)
			if err := imgService.Delete(ctx, img.Name(), images.SynchronousDelete()); err != nil {
				logger.Warn("Failed to delete image %s: %v", img.Name(), err)
				continue
			}
			reclaimedSpace += size
			deletedItems = append(deletedItems, "image:"+img.Name())
			logger.Info("Pruned image: %s (size: %d)", img.Name(), size)
		}
	}

	return reclaimedSpace, deletedItems, nil
}

// PruneSnapshots removes orphan snapshots
func (c *Client) PruneSnapshots(ctx context.Context) (int64, []string, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	var reclaimedSpace int64
	var deletedItems []string

	snapshotter := c.client.SnapshotService("overlayfs")
	if snapshotter == nil {
		return 0, nil, fmt.Errorf("snapshotter not available")
	}

	// Get active snapshots used by containers
	containers, _ := c.client.Containers(ctx)
	activeSnapshots := make(map[string]bool)
	for _, container := range containers {
		info, err := container.Info(ctx)
		if err == nil {
			activeSnapshots[info.SnapshotKey] = true
		}
	}

	// Walk and remove orphan snapshots
	var toDelete []string
	err := snapshotter.Walk(ctx, func(walkCtx context.Context, info snapshots.Info) error {
		if !activeSnapshots[info.Name] && info.Kind == snapshots.KindActive {
			toDelete = append(toDelete, info.Name)
		}
		return nil
	})
	if err != nil {
		return 0, nil, fmt.Errorf("failed to walk snapshots: %w", err)
	}

	for _, name := range toDelete {
		su, _ := snapshotter.Usage(ctx, name)
		if err := snapshotter.Remove(ctx, name); err != nil {
			logger.Warn("Failed to remove snapshot %s: %v", name, err)
			continue
		}
		reclaimedSpace += su.Size
		deletedItems = append(deletedItems, "snapshot:"+name)
		logger.Info("Pruned snapshot: %s", name)
	}

	return reclaimedSpace, deletedItems, nil
}

// ExecInContainer executes a command in a container with stdin/stdout/stderr
func (c *Client) ExecInContainer(ctx context.Context, containerID string, cmd []string, stdin io.Reader, stdout, stderr io.Writer) (uint32, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	container, err := c.client.LoadContainer(ctx, containerID)
	if err != nil {
		return 0, fmt.Errorf("failed to load container: %w", err)
	}

	task, err := container.Task(ctx, nil)
	if err != nil {
		return 0, fmt.Errorf("failed to get task: %w", err)
	}

	execID := fmt.Sprintf("exec-%d", time.Now().UnixNano())

	pspec := &specs.Process{
		Args: cmd,
		Cwd:  "/",
		Env:  []string{"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "TERM=xterm"},
	}

	process, err := task.Exec(ctx, execID, pspec, cio.NewCreator(
		cio.WithStreams(stdin, stdout, stderr),
	))
	if err != nil {
		return 0, fmt.Errorf("failed to exec: %w", err)
	}

	if err := process.Start(ctx); err != nil {
		return 0, fmt.Errorf("failed to start exec process: %w", err)
	}

	statusC, err := process.Wait(ctx)
	if err != nil {
		return 0, fmt.Errorf("failed to wait for process: %w", err)
	}

	status := <-statusC
	code, _, err := status.Result()
	if err != nil {
		return 0, fmt.Errorf("failed to get exit status: %w", err)
	}

	process.Delete(ctx)
	return code, nil
}

// ExecInContainerWithPTY executes a command in a container with real PTY allocation
func (c *Client) ExecInContainerWithPTY(ctx context.Context, containerID string, cmd []string, cols, rows int) (*ExecSession, error) {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	container, err := c.client.LoadContainer(ctx, containerID)
	if err != nil {
		return nil, fmt.Errorf("failed to load container: %w", err)
	}

	task, err := container.Task(ctx, nil)
	if err != nil {
		return nil, fmt.Errorf("failed to get task: %w", err)
	}

	execID := fmt.Sprintf("exec-%d", time.Now().UnixNano())

	if cols <= 0 {
		cols = 80
	}
	if rows <= 0 {
		rows = 24
	}

	pspec := &specs.Process{
		Args:     cmd,
		Cwd:      "/",
		Env:      []string{"PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", "TERM=xterm-256color"},
		Terminal: true,
		ConsoleSize: &specs.Box{
			Width:  uint(cols),
			Height: uint(rows),
		},
	}

	stdinR, stdinW := io.Pipe()
	stdoutR, stdoutW := io.Pipe()

	process, err := task.Exec(ctx, execID, pspec, cio.NewCreator(
		cio.WithStreams(stdinR, stdoutW, stdoutW),
		cio.WithTerminal,
	))
	if err != nil {
		stdinR.Close()
		stdinW.Close()
		stdoutR.Close()
		stdoutW.Close()
		return nil, fmt.Errorf("failed to exec: %w", err)
	}

	if err := process.Start(ctx); err != nil {
		stdinR.Close()
		stdinW.Close()
		stdoutR.Close()
		stdoutW.Close()
		return nil, fmt.Errorf("failed to start exec process: %w", err)
	}

	return &ExecSession{
		Process:   process,
		Stdin:     stdinW,
		Stdout:    stdoutR,
		ExecID:    execID,
		Container: container,
		closers:   []io.Closer{stdinR, stdinW, stdoutR, stdoutW},
	}, nil
}

// ExecSession holds an active exec session with PTY
type ExecSession struct {
	Process   containerd.Process
	Stdin     io.WriteCloser
	Stdout    io.ReadCloser
	ExecID    string
	Container containerd.Container
	closers   []io.Closer
}

// Resize resizes the PTY for the exec session
func (s *ExecSession) Resize(ctx context.Context, cols, rows int) error {
	return s.Process.Resize(ctx, uint32(cols), uint32(rows))
}

// Wait waits for the process to complete and returns exit code
func (s *ExecSession) Wait(ctx context.Context) (uint32, error) {
	statusC, err := s.Process.Wait(ctx)
	if err != nil {
		return 0, err
	}
	status := <-statusC
	code, _, err := status.Result()
	return code, err
}

// Close cleans up the exec session resources
func (s *ExecSession) Close(ctx context.Context) {
	s.Process.Delete(ctx)
	for _, c := range s.closers {
		c.Close()
	}
}

// SendSignal sends a signal to a running container task.
func (c *Client) SendSignal(ctx context.Context, containerID string, sig syscall.Signal) error {
	ctx = namespaces.WithNamespace(ctx, c.namespace)

	container, err := c.GetContainer(ctx, containerID)
	if err != nil {
		return fmt.Errorf("failed to get container %s: %w", containerID, err)
	}

	task, err := container.Task(ctx, nil)
	if err != nil {
		return fmt.Errorf("failed to get task for container %s: %w", containerID, err)
	}

	return task.Kill(ctx, sig)
}
