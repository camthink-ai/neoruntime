package storage

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"
)

var (
	shapeGroupRE      = regexp.MustCompile(`[\[\(]([^\]\)]*)[\]\)]`)
	shapeAssignmentRE = regexp.MustCompile(`(?i)\bshape\s*[:=]\s*([0-9]+(?:\s*[,xX_]\s*[0-9]+){1,4})`)
	dimensionPairRE   = regexp.MustCompile(`\b([1-9][0-9]{1,4})\s*[_xX]\s*([1-9][0-9]{1,4})\b`)
	numberRE          = regexp.MustCompile(`\d+`)
)

// ModelStorage manages model binary files using Content Addressable Storage (CAS).
type ModelStorage struct {
	blobDir       string     // directory for hash-named blobs
	minFreeBytes  uint64     // minimum free disk space to allow writes
	maxTotalBytes uint64     // cap on total blob bytes; 0 = uncapped
	writeMu       sync.Mutex // serializes SaveWithHash admission through publish
}

// HEFInfo holds metadata extracted from a HEF file via hailortcli.
type HEFInfo struct {
	NetworkName string   `json:"network_name"`
	RawOutput   string   `json:"raw_output"`
	VStreams    []string `json:"vstreams,omitempty"`
	InputWidth  int      `json:"input_width"`  // Extracted from input vstream shape
	InputHeight int      `json:"input_height"` // Extracted from input vstream shape
}

// NewModelStorage creates a ModelStorage backed by the given blob directory.
// minFreeBytes is the free-space floor every write must leave on the disk;
// maxTotalBytes caps how much the blobs themselves may consume (0 = uncapped).
func NewModelStorage(blobDir string, minFreeBytes, maxTotalBytes uint64) (*ModelStorage, error) {
	if err := os.MkdirAll(blobDir, 0755); err != nil {
		return nil, fmt.Errorf("failed to create blob directory %s: %w", blobDir, err)
	}
	return &ModelStorage{
		blobDir:       blobDir,
		minFreeBytes:  minFreeBytes,
		maxTotalBytes: maxTotalBytes,
	}, nil
}

// SaveResult is returned by SaveWithHash on success.
type SaveResult struct {
	Hash    string // hex-encoded SHA256
	Path    string // absolute path to the blob file
	Size    int64  // file size in bytes
	Existed bool   // true if the blob already existed (dedup)
}

// SaveWithHash streams the reader content to a temporary file, computes SHA256,
// and atomically renames it to blobs/<hash><ext>. Returns dedup info.
func (s *ModelStorage) SaveWithHash(r io.Reader, ext string, size int64) (*SaveResult, error) {
	// Admission must be atomic with publishing: two concurrent uploads that
	// each pass an unsynchronized quota check can both stream and rename
	// their blob, overshooting maxTotalBytes (and the free-space floor)
	// by however many raced. Serializing the check-through-rename section
	// keeps the budget exact; model uploads are rare admin operations, so
	// the coarse lock costs nothing in practice.
	s.writeMu.Lock()
	defer s.writeMu.Unlock()

	// Check disk quota before writing, counting the claimed payload against
	// the free-space floor. Uploads know their real size (multipart
	// FileHeader.Size, package length), so a disk near minFreeBytes is refused
	// up front instead of failing mid-rename. A negative size (caller never
	// measured) degrades to the legacy floor-only check.
	if size < 0 {
		size = 0
	}
	if err := s.CheckQuota(uint64(size)); err != nil {
		return nil, err
	}

	// Total-usage admission: the free-space floor alone lets the store grow
	// until only the floor remains. A configured budget keeps the blob
	// directory inside it. Checked before streaming with the claimed size —
	// the hash is unknowable until the bytes are read, so a dedup re-upload
	// against a full budget is refused too (conservative by design).
	if s.maxTotalBytes > 0 {
		usage, err := s.UsageBytes()
		if err != nil {
			return nil, fmt.Errorf("failed to measure model storage usage: %w", err)
		}
		if usage+uint64(size) > s.maxTotalBytes {
			return nil, fmt.Errorf("model storage quota exceeded: blobs use %d bytes, this write needs %d more, budget is %d bytes",
				usage, size, s.maxTotalBytes)
		}
	}

	// Write to temp file while computing hash
	tmpFile, err := os.CreateTemp(s.blobDir, "upload-*.tmp")
	if err != nil {
		return nil, fmt.Errorf("failed to create temp file: %w", err)
	}
	tmpPath := tmpFile.Name()
	defer func() {
		// Clean up temp file on any error path
		tmpFile.Close()
		os.Remove(tmpPath)
	}()

	hasher := sha256.New()
	writer := io.MultiWriter(tmpFile, hasher)

	written, err := io.Copy(writer, r)
	if err != nil {
		return nil, fmt.Errorf("failed to write model data: %w", err)
	}
	tmpFile.Close()

	hash := hex.EncodeToString(hasher.Sum(nil))
	blobName := hash + ext
	blobPath := filepath.Join(s.blobDir, blobName)

	// Check if blob already exists (dedup)
	if _, err := os.Stat(blobPath); err == nil {
		// Already exists — remove temp and return existing
		os.Remove(tmpPath)
		return &SaveResult{
			Hash:    hash,
			Path:    blobPath,
			Size:    written,
			Existed: true,
		}, nil
	}

	// Atomic rename temp → blob
	if err := os.Rename(tmpPath, blobPath); err != nil {
		return nil, fmt.Errorf("failed to rename blob: %w", err)
	}

	return &SaveResult{
		Hash:    hash,
		Path:    blobPath,
		Size:    written,
		Existed: false,
	}, nil
}

// Delete removes a blob by hash and extension.
func (s *ModelStorage) Delete(hash, ext string) error {
	path := filepath.Join(s.blobDir, hash+ext)
	if err := os.Remove(path); err != nil && !os.IsNotExist(err) {
		return fmt.Errorf("failed to delete blob %s: %w", hash, err)
	}
	return nil
}

// Exists checks if a blob exists.
func (s *ModelStorage) Exists(hash, ext string) bool {
	path := filepath.Join(s.blobDir, hash+ext)
	_, err := os.Stat(path)
	return err == nil
}

// BlobPath returns the absolute path for a given hash and extension.
func (s *ModelStorage) BlobPath(hash, ext string) string {
	return filepath.Join(s.blobDir, hash+ext)
}

// StoredBlob describes one hash-named blob file currently in the store.
type StoredBlob struct {
	Hash    string    // hex sha256 (the file name without extension)
	Ext     string    // canonical extension, e.g. ".hef"
	Size    int64     // file size in bytes
	ModTime time.Time // file mtime — the sweep's age signal
}

// blobNameRE matches the hash-named files SaveWithHash produces; upload-*.tmp
// staging files and anything else in the directory are not blobs.
var blobNameRE = regexp.MustCompile(`^[a-f0-9]{64}$`)

// ListBlobs enumerates the content-addressed blob files. Order unspecified.
func (s *ModelStorage) ListBlobs() ([]StoredBlob, error) {
	entries, err := os.ReadDir(s.blobDir)
	if err != nil {
		return nil, fmt.Errorf("failed to list blob directory: %w", err)
	}
	var blobs []StoredBlob
	for _, e := range entries {
		if e.IsDir() {
			continue
		}
		ext := strings.ToLower(filepath.Ext(e.Name()))
		hash := strings.TrimSuffix(e.Name(), ext)
		if !blobNameRE.MatchString(hash) {
			continue
		}
		info, err := e.Info()
		if err != nil {
			continue
		}
		blobs = append(blobs, StoredBlob{Hash: hash, Ext: ext, Size: info.Size(), ModTime: info.ModTime()})
	}
	return blobs, nil
}

// RemoveStaleTempFiles deletes upload-*.tmp staging files older than maxAge —
// the leftovers of uploads whose process died between CreateTemp and the
// deferred cleanup. Returns how many were removed.
func (s *ModelStorage) RemoveStaleTempFiles(maxAge time.Duration) int {
	entries, err := os.ReadDir(s.blobDir)
	if err != nil {
		return 0
	}
	cutoff := time.Now().Add(-maxAge)
	removed := 0
	for _, e := range entries {
		if e.IsDir() || !strings.HasPrefix(e.Name(), "upload-") || !strings.HasSuffix(e.Name(), ".tmp") {
			continue
		}
		info, err := e.Info()
		if err != nil || !info.ModTime().Before(cutoff) {
			continue
		}
		if os.Remove(filepath.Join(s.blobDir, e.Name())) == nil {
			removed++
		}
	}
	return removed
}

// FreeBytes reports the free space available to unprivileged writes on the
// filesystem hosting blobDir.
func (s *ModelStorage) FreeBytes() (uint64, error) {
	var stat syscall.Statfs_t
	if err := syscall.Statfs(s.blobDir, &stat); err != nil {
		return 0, fmt.Errorf("failed to check disk space: %w", err)
	}
	return stat.Bavail * uint64(stat.Bsize), nil
}

// UsageBytes sums the sizes of the hash-named blobs — what the store consumes
// of its budget. Staging temp files and foreign entries do not count.
func (s *ModelStorage) UsageBytes() (uint64, error) {
	blobs, err := s.ListBlobs()
	if err != nil {
		return 0, err
	}
	var total uint64
	for _, b := range blobs {
		if b.Size > 0 {
			total += uint64(b.Size)
		}
	}
	return total, nil
}

// CheckQuota verifies that the filesystem hosting blobDir has enough free space.
func (s *ModelStorage) CheckQuota(additionalBytes uint64) error {
	freeBytes, err := s.FreeBytes()
	if err != nil {
		return err
	}
	required := s.minFreeBytes + additionalBytes
	if freeBytes < required {
		return fmt.Errorf("insufficient disk space: %d bytes free, need at least %d bytes",
			freeBytes, required)
	}
	return nil
}

// ValidateHEF runs hailortcli parse-hef on the given file and extracts metadata.
// Returns an error if the file is not a valid HEF or doesn't target the expected hardware.
func (s *ModelStorage) ValidateHEF(filePath string) (*HEFInfo, error) {
	// A missing hailortcli is an explicit failure, not a marker-string
	// success: callers stored the marker in vstream_info and could not
	// tell "validated" from "silently unvalidated". On a properly
	// provisioned device the tool ships with the Hailo runtime — its
	// absence means the deployment is broken.
	hailortcli, err := exec.LookPath("hailortcli")
	if err != nil {
		return nil, fmt.Errorf("hailortcli not found in PATH — HEF validation requires the Hailo runtime tooling: %w", err)
	}

	cmd := exec.Command(hailortcli, "parse-hef", filePath)
	output, err := cmd.CombinedOutput()
	if err != nil {
		return nil, fmt.Errorf("HEF validation failed: %s (exit: %v)", string(output), err)
	}

	return parseHEFInfo(string(output)), nil
}

func parseHEFInfo(output string) *HEFInfo {
	info := &HEFInfo{RawOutput: output}

	section := ""
	for _, rawLine := range strings.Split(output, "\n") {
		line := strings.TrimSpace(rawLine)
		if line == "" {
			continue
		}
		lower := strings.ToLower(line)

		if strings.HasPrefix(line, "Network group name:") {
			// Extract name, remove any trailing metadata after comma.
			name := strings.TrimSpace(strings.TrimPrefix(line, "Network group name:"))
			if idx := strings.Index(name, ","); idx > 0 {
				name = strings.TrimSpace(name[:idx])
			}
			info.NetworkName = name
		}

		switch {
		case strings.Contains(lower, "input") && strings.Contains(lower, "vstream"):
			section = "input"
		case strings.Contains(lower, "output") && strings.Contains(lower, "vstream"):
			section = "output"
		case strings.HasPrefix(lower, "input"):
			section = "input"
		case strings.HasPrefix(lower, "output"):
			section = "output"
		}

		if isVStreamLine(line) {
			info.VStreams = append(info.VStreams, line)
		}

		if info.InputWidth == 0 && shouldParseInputDimensions(line, section) {
			if w, h := parseInputDimensions(line); w > 0 && h > 0 {
				info.InputWidth = w
				info.InputHeight = h
			}
		}
	}

	return info
}

func isVStreamLine(line string) bool {
	lower := strings.ToLower(line)
	return strings.Contains(lower, "vstream") ||
		strings.Contains(lower, "stream") ||
		strings.HasPrefix(lower, "input") ||
		strings.HasPrefix(lower, "output")
}

func isInputVStreamLine(line, section string) bool {
	lower := strings.ToLower(line)
	if strings.HasPrefix(lower, "output") || strings.Contains(lower, "output vstream") {
		return false
	}
	if strings.HasPrefix(lower, "input") {
		return true
	}
	if section == "input" && !strings.Contains(lower, "output") {
		return true
	}
	return strings.Contains(lower, "input") && !strings.Contains(lower, "output")
}

func shouldParseInputDimensions(line, section string) bool {
	if !isInputVStreamLine(line, section) || !hasShapeHint(line) {
		return false
	}

	lower := strings.ToLower(line)
	return isVStreamLine(line) ||
		strings.Contains(lower, "shape") ||
		layoutFromLine(line) != ""
}

func hasShapeHint(line string) bool {
	lower := strings.ToLower(line)
	return strings.Contains(lower, "shape") ||
		strings.Contains(lower, "nv12") ||
		strings.Contains(lower, "nhwc") ||
		strings.Contains(lower, "nchw") ||
		strings.Contains(lower, "hwc") ||
		strings.Contains(lower, "chw") ||
		shapeGroupRE.MatchString(line) ||
		dimensionPairRE.MatchString(line)
}

// parseInputDimensions extracts width and height from a vstream line.
// Supports formats like: "[640, 640, 3]" "640x640" "shape: (640, 640, 3)" etc.
func parseInputDimensions(line string) (width, height int) {
	layout := layoutFromLine(line)

	for _, shape := range extractShapeCandidates(line) {
		if w, h := dimsFromShape(shape, layout); w > 0 && h > 0 {
			return w, h
		}
	}

	if match := dimensionPairRE.FindStringSubmatch(line); len(match) == 3 {
		w, _ := strconv.Atoi(match[1])
		h, _ := strconv.Atoi(match[2])
		if w > 0 && h > 0 {
			return w, h
		}
	}

	return 0, 0
}

func layoutFromLine(line string) string {
	upper := strings.ToUpper(line)
	for _, layout := range []string{"NV12", "NHWC", "NCHW", "HWC", "CHW"} {
		if strings.Contains(upper, layout) {
			return layout
		}
	}
	return ""
}

func extractShapeCandidates(line string) [][]int {
	var candidates [][]int
	for _, match := range shapeGroupRE.FindAllStringSubmatch(line, -1) {
		if len(match) != 2 {
			continue
		}
		if nums := parseShapeNumbers(match[1]); len(nums) >= 2 {
			candidates = append(candidates, nums)
		}
	}
	for _, match := range shapeAssignmentRE.FindAllStringSubmatch(line, -1) {
		if len(match) != 2 {
			continue
		}
		if nums := parseShapeNumbers(match[1]); len(nums) >= 2 {
			candidates = append(candidates, nums)
		}
	}
	return candidates
}

func parseShapeNumbers(raw string) []int {
	matches := numberRE.FindAllString(raw, -1)
	nums := make([]int, 0, len(matches))
	for _, match := range matches {
		n, err := strconv.Atoi(match)
		if err == nil {
			nums = append(nums, n)
		}
	}
	return nums
}

func dimsFromShape(nums []int, layout string) (width, height int) {
	if len(nums) > 4 {
		return 0, 0
	}

	switch layout {
	case "NV12":
		if len(nums) == 4 {
			return validDims(nums[2], nums[1]*2)
		}
		if len(nums) == 3 {
			return validDims(nums[1], nums[0]*2)
		}
	case "NHWC":
		if len(nums) == 4 {
			return validDims(nums[2], nums[1])
		}
		if len(nums) == 3 {
			return validDims(nums[1], nums[0])
		}
	case "NCHW":
		if len(nums) == 4 {
			return validDims(nums[3], nums[2])
		}
		if len(nums) == 3 {
			return validDims(nums[2], nums[1])
		}
	case "HWC":
		if len(nums) >= 2 {
			return validDims(nums[1], nums[0])
		}
	case "CHW":
		if len(nums) == 4 {
			return validDims(nums[3], nums[2])
		}
		if len(nums) == 3 {
			return validDims(nums[2], nums[1])
		}
	}

	switch len(nums) {
	case 4:
		if nums[0] == 1 && isChannelDim(nums[3]) {
			return validDims(nums[2], nums[1])
		}
		if nums[0] == 1 && isChannelDim(nums[1]) {
			return validDims(nums[3], nums[2])
		}
	case 3:
		if isChannelDim(nums[2]) {
			return validDims(nums[1], nums[0])
		}
		if isChannelDim(nums[0]) {
			return validDims(nums[2], nums[1])
		}
	case 2:
		return validDims(nums[1], nums[0])
	}
	return 0, 0
}

func isChannelDim(v int) bool {
	switch v {
	case 3, 4:
		return true
	default:
		return false
	}
}

func validDims(width, height int) (int, int) {
	if width > 0 && height > 0 {
		return width, height
	}
	return 0, 0
}

// ValidateHEFToJSON runs ValidateHEF and returns the info as a JSON string.
func (s *ModelStorage) ValidateHEFToJSON(filePath string) (string, *HEFInfo, error) {
	info, err := s.ValidateHEF(filePath)
	if err != nil {
		return "", nil, err
	}

	jsonBytes, err := json.Marshal(info)
	if err != nil {
		return "", info, fmt.Errorf("failed to marshal HEF info: %w", err)
	}

	return string(jsonBytes), info, nil
}
