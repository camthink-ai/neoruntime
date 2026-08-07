package handlers

import (
	"bufio"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"mime/multipart"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"aipc/platform/osupgrade"

	"github.com/gin-gonic/gin"
	"github.com/google/uuid"
)

const (
	osUpgradeReserveBytes = uint64(2 << 30)
	osUpgradeMinMemory    = uint64(512 << 20)
)

type OSUpgradeHandlers struct {
	store            *osupgrade.Store
	expectedMachine  string
	expectedProduct  string
	expectedHW       string
	allowDowngrade   bool
	requireSig       bool
	requireBuildTime bool
	filesystemDevice string
	layoutChecker    *osupgrade.LayoutChecker
	recoveryDir      string
	mu               sync.Mutex
}

func NewOSUpgradeHandlers(root string) *OSUpgradeHandlers {
	filesystemDevice := envDefault("AIPC_FILESYSTEM_DEVICE", osupgrade.DefaultFilesystemDevice)
	return &OSUpgradeHandlers{
		store:            osupgrade.NewStore(root),
		expectedMachine:  envDefault("AIPC_OS_MACHINE", "hailo15-ne503"),
		expectedProduct:  os.Getenv("AIPC_OS_PRODUCT"),
		expectedHW:       os.Getenv("AIPC_OS_HARDWARE_VERSION"),
		allowDowngrade:   envBool("AIPC_OS_ALLOW_DOWNGRADE"),
		requireSig:       envBool("AIPC_OS_REQUIRE_SIGNATURE"),
		requireBuildTime: envBool("AIPC_OS_REQUIRE_BUILD_TIME"),
		filesystemDevice: filesystemDevice,
		layoutChecker:    osupgrade.NewLayoutChecker(filesystemDevice),
		recoveryDir:      envDefault("AIPC_RECOVERY_DIR", osupgrade.DefaultRecoveryDir),
	}
}

// ReconcileOnBoot advances any active OS-upgrade job to a terminal state.
// It covers the case where aipc-os-verify.service does not run after a
// reboot (masked / degraded / never triggered): platform-api is the other
// long-lived process guaranteed to come up on the new system, so on startup
// it drives an idempotent Verify() to push a lingering rebooting/verifying
// job to success/failed. Verify() is a no-op for already-terminal jobs, so
// this is safe even when the verify service already completed the work.
func (h *OSUpgradeHandlers) ReconcileOnBoot() {
	go func() {
		if _, err := h.store.Active(); err != nil {
			// No active job (os.IsNotExist) or unreadable — nothing to reconcile.
			return
		}
		if err := osupgrade.NewRunner(h.store).Verify(); err != nil {
			log.Printf("osupgrade boot reconcile: %v", err)
		}
	}()
}

func (h *OSUpgradeHandlers) Upload(c *gin.Context) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if err := h.store.Init(); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	if active, err := h.store.Active(); err == nil {
		if !active.Terminal() {
			Resp(c).FailMsg(CodeAlreadyExists, "another OS upgrade job is active")
			return
		}
	} else if !os.IsNotExist(err) {
		Resp(c).FailMsg(CodeServiceError, "cannot read active OS upgrade state: "+err.Error())
		return
	}
	if err := precheckResources(h.store.Root, c.Request.ContentLength); err != nil {
		Resp(c).FailMsg(CodeStorageFull, err.Error())
		return
	}
	reader, err := c.Request.MultipartReader()
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "multipart upload required: "+err.Error())
		return
	}
	part, err := nextPackagePart(reader)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}
	defer part.Close()
	fileName := filepath.Base(part.FileName())
	if !strings.HasSuffix(strings.ToLower(fileName), ".swu") {
		Resp(c).FailMsg(CodeInvalidParameter, "only .swu packages are accepted")
		return
	}

	id := uuid.NewString()
	job := &osupgrade.Job{
		ID:        id,
		State:     osupgrade.StateUploading,
		Progress:  0,
		Message:   "Uploading OS package",
		FileName:  fileName,
		CreatedAt: time.Now().UTC(),
	}
	if err := h.store.Save(job); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	if err := h.store.SetActive(id); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	partPath := h.store.IncomingPath(id)
	dst, err := os.OpenFile(partPath, os.O_CREATE|os.O_EXCL|os.O_WRONLY, 0640)
	if err != nil {
		h.failJob(job, err)
		Resp(c).FailMsg(CodeFileUploadFailed, err.Error())
		return
	}
	hash := sha256.New()
	size, copyErr := io.Copy(io.MultiWriter(dst, hash), part)
	syncErr := dst.Sync()
	closeErr := dst.Close()
	if copyErr != nil || syncErr != nil || closeErr != nil {
		err = errors.Join(copyErr, syncErr, closeErr)
		_ = os.Remove(partPath)
		h.failJob(job, err)
		Resp(c).FailMsg(CodeFileUploadFailed, err.Error())
		return
	}
	if err := ensureFinalFreeSpace(h.store.Root, uint64(size)); err != nil {
		_ = os.Remove(partPath)
		h.failJob(job, err)
		Resp(c).FailMsg(CodeStorageFull, err.Error())
		return
	}
	sum := hex.EncodeToString(hash.Sum(nil))
	if expected := strings.ToLower(strings.TrimSpace(c.GetHeader("X-Content-SHA256"))); expected != "" && expected != sum {
		err := fmt.Errorf("SHA-256 mismatch: expected %s, got %s", expected, sum)
		_ = os.Remove(partPath)
		h.failJob(job, err)
		Resp(c).FailMsg(CodeInvalidParameter, err.Error())
		return
	}
	if err := os.Rename(partPath, h.store.PackagePath(id)); err != nil {
		_ = os.Remove(partPath)
		h.failJob(job, err)
		Resp(c).FailMsg(CodeFileUploadFailed, err.Error())
		return
	}
	syncDirectory(h.store.PackagesDir())
	job.State = osupgrade.StateValidating
	job.Progress = 100
	job.Message = "Upload complete; validation required"
	job.Size = size
	job.SHA256 = sum
	if err := h.store.Save(job); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	Resp(c).OK(job)
}

func (h *OSUpgradeHandlers) Validate(c *gin.Context) {
	h.mu.Lock()
	defer h.mu.Unlock()
	var req struct {
		JobID          string `json:"job_id"`
		ExpectedSHA    string `json:"sha256"`
		AllowDowngrade bool   `json:"allow_downgrade"`
	}
	if c.Request.ContentLength > 0 {
		if err := c.ShouldBindJSON(&req); err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, err.Error())
			return
		}
	}
	job, err := h.job(req.JobID)
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, err.Error())
		return
	}
	if job.State != osupgrade.StateValidating && job.State != osupgrade.StateReady {
		Resp(c).FailMsg(CodeInvalidRequest, "job cannot be validated in state "+string(job.State))
		return
	}
	job.State = osupgrade.StateValidating
	job.Message = "Validating OS package"
	job.Error = ""
	_ = h.store.Save(job)
	currentCopy, err := readBootCopy()
	if err != nil {
		h.failJob(job, err)
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	layout, err := h.layoutChecker.Detect(currentCopy)
	if err != nil {
		h.failJob(job, err)
		Resp(c).FailMsg(CodeInvalidParameter, err.Error())
		return
	}
	if layout.Mode == osupgrade.LayoutDual {
		if err := h.layoutChecker.Check(currentCopy, layout.TargetCopy); err != nil {
			h.failJob(job, err)
			Resp(c).FailMsg(CodeInvalidParameter, err.Error())
			return
		}
	}
	expectedSHA := req.ExpectedSHA
	if expectedSHA == "" {
		expectedSHA = job.SHA256
	}
	result, err := osupgrade.ValidatePackage(job.PackagePath, osupgrade.ValidationOptions{
		ExpectedSHA256:       expectedSHA,
		ExpectedMachine:      h.expectedMachine,
		ExpectedProduct:      h.expectedProduct,
		ExpectedHW:           h.expectedHW,
		ExpectedDevice:       h.filesystemDevice,
		RequireBuildTime:     h.requireBuildTime,
		RequireAB:            false,
		RequireCompatibility: true,
		RequireSignature:     h.requireSig,
	})
	if err != nil {
		h.failJob(job, err)
		Resp(c).FailMsg(CodeInvalidParameter, err.Error())
		return
	}
	var recovery *osupgrade.RecoveryBundle
	if layout.Mode == osupgrade.LayoutSingle {
		recovery, err = osupgrade.PrepareRecoveryBundleFromPackage(
			job.PackagePath,
			h.store.RecoveryCandidateDir(job.ID),
			result.Machine,
			result.Version,
			result.SecureBootKeyID,
		)
		if err != nil {
			h.failJob(job, err)
			Resp(c).FailMsg(CodeInvalidParameter, "target package recovery bundle is unavailable: "+err.Error())
			return
		}
	}
	if recovery != nil {
		if err = recovery.Compatible(result); err != nil {
			h.failJob(job, err)
			Resp(c).FailMsg(CodeInvalidParameter, err.Error())
			return
		}
	}
	appManifestPath := envDefault("AIPC_APP_MANIFEST", osupgrade.DefaultAppManifestPath)
	dataSchemaPath := envDefault("AIPC_DATA_SCHEMA_FILE", osupgrade.DefaultDataSchemaPath)
	appManifest, err := osupgrade.LoadAppManifest(appManifestPath)
	if err != nil {
		h.failJob(job, fmt.Errorf("cannot read current App manifest: %w", err))
		Resp(c).FailMsg(CodeInvalidParameter, "current App compatibility manifest is unavailable: "+err.Error())
		return
	}
	currentDataSchema, err := osupgrade.ReadDataSchema(dataSchemaPath)
	if err != nil {
		h.failJob(job, fmt.Errorf("cannot read current data schema: %w", err))
		Resp(c).FailMsg(CodeInvalidParameter, "current data schema is unavailable: "+err.Error())
		return
	}
	targetCompatibility := &osupgrade.OSCompatibility{
		OSVersion: result.Version, Machine: result.Machine, Product: result.Product,
		CompatLevel: result.CompatLevel, DataSchema: result.DataSchema,
	}
	if err := osupgrade.CheckCompatibility(targetCompatibility, appManifest, currentDataSchema); err != nil {
		h.failJob(job, err)
		Resp(c).FailMsg(CodeInvalidParameter, err.Error())
		return
	}
	signatureValid := false
	if h.requireSig {
		cmd := exec.Command("swupdate", "-c", "-i", job.PackagePath)
		if output, checkErr := cmd.CombinedOutput(); checkErr != nil {
			err = fmt.Errorf("SWUpdate signature check failed: %s", strings.TrimSpace(string(output)))
			h.failJob(job, err)
			Resp(c).FailMsg(CodeInvalidParameter, err.Error())
			return
		}
		signatureValid = true
	}
	currentVersion := readOSVersion()
	allowDowngrade := h.allowDowngrade && req.AllowDowngrade
	if !allowDowngrade && compareVersions(result.Version, currentVersion) < 0 {
		err = fmt.Errorf("downgrade from %s to %s is not allowed", currentVersion, result.Version)
		h.failJob(job, err)
		Resp(c).FailMsg(CodeInvalidParameter, err.Error())
		return
	}
	job.State = osupgrade.StateReady
	job.Message = "OS package is ready to install"
	job.Error = ""
	job.CurrentVersion = currentVersion
	job.TargetVersion = result.Version
	job.BuildTime = result.BuildTime
	job.Machine = result.Machine
	job.Product = result.Product
	job.HardwareVersion = result.HardwareVersion
	job.CurrentCopy = currentCopy
	job.PreviousCopy = currentCopy
	job.TargetCopy = layout.TargetCopy
	job.UpgradeMode = layout.Mode
	job.AvailableUpdateModes = append([]string(nil), result.UpdateModes...)
	job.AvailableUpdateModeOptions = osupgrade.BuildUpdateModeOptions(
		layout.Mode,
		layout.TargetCopy,
		result,
		recovery,
	)
	job.AvailableUpgradeStrategies = osupgrade.BuildUpgradeStrategyOptions(layout.Mode, result, recovery)
	job.SupportsStandardUpgrade = osupgrade.UpgradeStrategyOptionSupported(
		job.AvailableUpgradeStrategies,
		osupgrade.UpgradeStrategyStandard,
	)
	job.SupportsFullUpgrade = osupgrade.UpgradeStrategyOptionSupported(
		job.AvailableUpgradeStrategies,
		osupgrade.UpgradeStrategyFull,
	)
	job.UpgradeStrategy = string(job.EffectiveUpgradeStrategy())
	job.RollbackSupported = layout.Mode == osupgrade.LayoutDual
	job.ServiceInterruptionRequired = layout.Mode == osupgrade.LayoutSingle
	if recovery != nil {
		job.RecoverySource = "target-package"
		job.RecoveryVersion = recovery.Manifest.RecoveryVersion
	}
	job.SecureBootKeyID = result.SecureBootKeyID
	job.AppVersion = appManifest.AppVersion
	job.CompatLevel = result.CompatLevel
	job.DataSchema = result.DataSchema
	job.CompatibilityValid = true
	job.SHA256 = result.SHA256
	job.DowngradeAllowed = allowDowngrade
	job.SignatureValid = signatureValid
	if err := h.store.Save(job); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	Resp(c).OK(job)
}

func (h *OSUpgradeHandlers) Install(c *gin.Context) {
	h.mu.Lock()
	defer h.mu.Unlock()
	var req osUpgradeJobRequest
	if c.Request.ContentLength > 0 {
		if err := json.NewDecoder(c.Request.Body).Decode(&req); err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, err.Error())
			return
		}
	}
	job, err := h.job(req.JobID)
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, err.Error())
		return
	}
	if job.State != osupgrade.StateReady {
		Resp(c).FailMsg(CodeInvalidRequest, "job is not ready")
		return
	}
	var strategy osupgrade.UpgradeStrategy
	updateMode, err := osupgrade.ParseUpdateMode(req.UpdateMode)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}
	if updateMode != "" {
		if err := job.CheckUpdateMode(job.TargetCopy, updateMode); err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, err.Error())
			return
		}
	} else {
		strategy, err = osupgrade.ParseUpgradeStrategy(req.UpgradeStrategy)
		if err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, err.Error())
			return
		}
		if strategy == "" {
			strategy = job.EffectiveUpgradeStrategy()
		}
		if err := job.CheckUpgradeStrategy(strategy); err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, err.Error())
			return
		}
	}
	if err := h.store.SetActive(job.ID); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	job.State = osupgrade.StateInstalling
	job.Progress = 1
	job.Message = "Starting independent OS updater service"
	if updateMode != "" {
		job.UpdateMode = updateMode
		job.UpgradeStrategy = ""
	} else {
		job.UpdateMode = ""
		job.UpgradeStrategy = string(strategy)
	}
	if err := h.store.Save(job); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	if output, err := exec.Command("systemctl", "start", "--no-block", "aipc-os-updater.service").CombinedOutput(); err != nil {
		job.State = osupgrade.StateReady
		job.Progress = 100
		job.Message = "OS package is ready to install"
		_ = h.store.Save(job)
		Resp(c).FailMsg(CodeServiceError, "cannot start updater service: "+strings.TrimSpace(string(output)))
		return
	}
	Resp(c).OK(gin.H{"job_id": job.ID, "status": osupgrade.StateInstalling})
}

func (h *OSUpgradeHandlers) Status(c *gin.Context) {
	job, err := h.job(c.Query("job_id"))
	if err != nil {
		if os.IsNotExist(err) {
			Resp(c).OK(gin.H{"status": osupgrade.StateIdle})
			return
		}
		Resp(c).FailMsg(CodeNotFound, err.Error())
		return
	}
	response := struct {
		*osupgrade.Job
		Log string `json:"log,omitempty"`
	}{Job: job, Log: tailFile(h.store.LogPath(job.ID), 64*1024)}
	Resp(c).OK(response)
}

func (h *OSUpgradeHandlers) Reboot(c *gin.Context) {
	h.mu.Lock()
	defer h.mu.Unlock()
	job, err := h.requestedJob(c)
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, err.Error())
		return
	}
	if job.State != osupgrade.StateAwaitingReboot {
		Resp(c).FailMsg(CodeInvalidRequest, "job is not awaiting reboot")
		return
	}
	if output, err := exec.Command("systemctl", "start", "--no-block", "aipc-os-reboot.service").CombinedOutput(); err != nil {
		Resp(c).FailMsg(CodeServiceError, "cannot start reboot service: "+strings.TrimSpace(string(output)))
		return
	}
	Resp(c).OK(gin.H{"job_id": job.ID, "status": osupgrade.StateRebooting})
}

func (h *OSUpgradeHandlers) Cancel(c *gin.Context) {
	h.mu.Lock()
	defer h.mu.Unlock()
	job, err := h.requestedJob(c)
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, err.Error())
		return
	}
	switch job.State {
	case osupgrade.StateUploading, osupgrade.StateValidating, osupgrade.StateReady, osupgrade.StateFailed:
	case osupgrade.StateAwaitingReboot:
		runner := osupgrade.NewRunner(h.store)
		if err := runner.CancelStaged(job); err != nil {
			Resp(c).FailMsg(CodeServiceError, "cannot undo staged upgrade: "+err.Error())
			return
		}
	default:
		Resp(c).FailMsg(CodeInvalidRequest, "installation cannot be cancelled safely in state "+string(job.State))
		return
	}
	_ = os.Remove(h.store.IncomingPath(job.ID))
	job.State = osupgrade.StateCancelled
	job.Message = "OS upgrade cancelled"
	job.Error = ""
	if err := h.store.Save(job); err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}
	if data, readErr := os.ReadFile(h.store.ActivePath()); readErr == nil &&
		strings.TrimSpace(string(data)) == job.ID {
		_ = os.Remove(h.store.ActivePath())
	}
	Resp(c).OK(job)
}

func (h *OSUpgradeHandlers) DeletePackage(c *gin.Context) {
	h.mu.Lock()
	defer h.mu.Unlock()
	job, err := h.requestedJob(c)
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, err.Error())
		return
	}
	if err := h.store.RemovePackage(job); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}
	Resp(c).OK(gin.H{"job_id": job.ID})
}

func (h *OSUpgradeHandlers) requestedJob(c *gin.Context) (*osupgrade.Job, error) {
	var req osUpgradeJobRequest
	if c.Request.ContentLength > 0 {
		if err := json.NewDecoder(c.Request.Body).Decode(&req); err != nil {
			return nil, err
		}
	}
	return h.job(req.JobID)
}

type osUpgradeJobRequest struct {
	JobID           string `json:"job_id"`
	UpgradeStrategy string `json:"upgrade_strategy"`
	UpdateMode      string `json:"update_mode"`
}

func (h *OSUpgradeHandlers) job(id string) (*osupgrade.Job, error) {
	if id != "" {
		return h.store.Load(id)
	}
	return h.store.Active()
}

func (h *OSUpgradeHandlers) failJob(job *osupgrade.Job, err error) {
	job.State = osupgrade.StateFailed
	job.Message = "OS upgrade failed"
	job.Error = err.Error()
	_ = h.store.Save(job)
}

func nextPackagePart(reader *multipart.Reader) (*multipart.Part, error) {
	for {
		part, err := reader.NextPart()
		if err != nil {
			if errors.Is(err, io.EOF) {
				return nil, errors.New("missing package form field")
			}
			return nil, err
		}
		if part.FormName() == "package" || part.FormName() == "firmware" {
			return part, nil
		}
		_ = part.Close()
	}
}

func precheckResources(root string, contentLength int64) error {
	if availableMemory() < osUpgradeMinMemory {
		return fmt.Errorf("at least 512 MB available memory is required")
	}
	var stat syscall.Statfs_t
	if err := syscall.Statfs(filepath.Dir(root), &stat); err != nil {
		if err := syscall.Statfs("/", &stat); err != nil {
			return err
		}
	}
	available := stat.Bavail * uint64(stat.Bsize)
	required := osUpgradeReserveBytes
	if contentLength > 0 {
		required += uint64(contentLength)
	}
	if available < required {
		return fmt.Errorf("insufficient space: need package size plus 2 GB reserve")
	}
	return nil
}

func ensureFinalFreeSpace(root string, packageSize uint64) error {
	var stat syscall.Statfs_t
	if err := syscall.Statfs(root, &stat); err != nil {
		return err
	}
	if stat.Bavail*uint64(stat.Bsize) < osUpgradeReserveBytes {
		return fmt.Errorf("less than 2 GB would remain after storing the %d-byte package", packageSize)
	}
	return nil
}

func availableMemory() uint64 {
	file, err := os.Open("/proc/meminfo")
	if err != nil {
		return 0
	}
	defer file.Close()
	scanner := bufio.NewScanner(file)
	for scanner.Scan() {
		fields := strings.Fields(scanner.Text())
		if len(fields) >= 2 && fields[0] == "MemAvailable:" {
			value, _ := strconv.ParseUint(fields[1], 10, 64)
			return value * 1024
		}
	}
	return 0
}

func readOSVersion() string {
	data, err := os.ReadFile("/etc/os-release")
	if err != nil {
		return ""
	}
	values := map[string]string{}
	for _, line := range strings.Split(string(data), "\n") {
		key, value, ok := strings.Cut(line, "=")
		if ok {
			values[key] = strings.Trim(value, `"' `)
		}
	}
	for _, key := range []string{"IMAGE_VERSION", "VERSION_ID", "VERSION"} {
		if values[key] != "" {
			return values[key]
		}
	}
	return ""
}

func readBootCopy() (string, error) {
	output, err := exec.Command("/etc/get_sw_image.sh", "--boot").CombinedOutput()
	if err != nil {
		return "", fmt.Errorf("cannot identify current boot copy: %s", strings.TrimSpace(string(output)))
	}
	value := strings.ToUpper(strings.TrimSpace(string(output)))
	switch {
	case value == "A", strings.HasSuffix(value, "COPY-A"), strings.HasSuffix(value, "COPY A"):
		return "A", nil
	case value == "B", strings.HasSuffix(value, "COPY-B"), strings.HasSuffix(value, "COPY B"):
		return "B", nil
	default:
		return "", fmt.Errorf("cannot identify current boot copy from %q", value)
	}
}

func compareVersions(a, b string) int {
	parse := func(value string) []int {
		fields := strings.FieldsFunc(value, func(r rune) bool { return r < '0' || r > '9' })
		out := make([]int, 0, len(fields))
		for _, field := range fields {
			number, _ := strconv.Atoi(field)
			out = append(out, number)
		}
		return out
	}
	left, right := parse(a), parse(b)
	length := len(left)
	if len(right) > length {
		length = len(right)
	}
	for i := 0; i < length; i++ {
		var l, r int
		if i < len(left) {
			l = left[i]
		}
		if i < len(right) {
			r = right[i]
		}
		if l < r {
			return -1
		}
		if l > r {
			return 1
		}
	}
	return 0
}

func tailFile(path string, limit int64) string {
	file, err := os.Open(path)
	if err != nil {
		return ""
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil {
		return ""
	}
	if info.Size() > limit {
		_, _ = file.Seek(info.Size()-limit, io.SeekStart)
	}
	data, _ := io.ReadAll(io.LimitReader(file, limit))
	return string(data)
}

func syncDirectory(path string) {
	if dir, err := os.Open(path); err == nil {
		_ = dir.Sync()
		_ = dir.Close()
	}
}

func envDefault(key, fallback string) string {
	if value := strings.TrimSpace(os.Getenv(key)); value != "" {
		return value
	}
	return fallback
}

func envBool(key string) bool {
	value, _ := strconv.ParseBool(os.Getenv(key))
	return value
}
