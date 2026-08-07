package handlers

import (
	"bufio"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	eventLoggerPkg "aipc/platform/common/events"
	"github.com/gin-gonic/gin"
	"github.com/shirou/gopsutil/v4/disk"
)

// StorageHandlers handles storage APIs
type StorageHandlers struct {
	publishEvent func(topic string, payload map[string]interface{})
	eventLogger  *eventLoggerPkg.Logger
}

// NewStorageHandlers creates a new storage handler
func NewStorageHandlers(publisher func(topic string, payload map[string]interface{}), eventLogger *eventLoggerPkg.Logger) *StorageHandlers {
	h := &StorageHandlers{
		publishEvent: publisher,
		eventLogger:  eventLogger,
	}
	if publisher != nil {
		go h.monitorHotplug()
	}
	return h
}

// SetEventLogger sets the event logger (for dependency injection)
func (h *StorageHandlers) SetEventLogger(logger *eventLoggerPkg.Logger) {
	h.eventLogger = logger
}

// LsblkBlock represents a parsed block device from lsblk
type LsblkBlock struct {
	Name       string        `json:"name"`
	Size       int64         `json:"size" string:"size"` // Might need custom unmarshal if lsblk returns string, but -b returns bytes
	Type       string        `json:"type"`
	Mountpoint *string       `json:"mountpoint"`
	Fstype     *string       `json:"fstype"`
	Model      *string       `json:"model"`
	Vendor     *string       `json:"vendor"`
	Children   []*LsblkBlock `json:"children,omitempty"`
}

type LsblkOutput struct {
	Blockdevices []LsblkBlock `json:"blockdevices"`
}

// ListDisks lists available disks and partitions
func (h *StorageHandlers) ListDisks(c *gin.Context) {
	cmd := exec.Command("lsblk", "-b", "-J", "-o", "NAME,SIZE,TYPE,MOUNTPOINT,FSTYPE,MODEL,VENDOR")
	out, err := cmd.Output()
	if err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "Failed to list disks: "+err.Error())
		return
	}

	var parsed map[string]interface{}
	if err := json.Unmarshal(out, &parsed); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "Failed to parse disks: "+err.Error())
		return
	}

	Resp(c).OK(parsed)
}

// MountDisk mounts a block device
func (h *StorageHandlers) MountDisk(c *gin.Context) {
	var req struct {
		Device string `json:"device" binding:"required"`
		Target string `json:"target"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	if req.Target == "" {
		// Auto generate a target based on device name
		devName := filepath.Base(req.Device)
		req.Target = filepath.Join("/mnt", devName)
	}

	if err := isSafeDevice(req.Device); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	// Ensure target directory exists
	if err := os.MkdirAll(req.Target, 0755); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "Failed to create mount point: "+err.Error())
		return
	}

	cmd := exec.Command("mount", req.Device, req.Target)
	if out, err := cmd.CombinedOutput(); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "Mount failed: "+string(out))
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"storage.disk.mounted",
			eventLoggerPkg.MessageParams{
				"device": req.Device,
				"target": req.Target,
			},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{
		"message": "Successfully mounted",
		"device":  req.Device,
		"target":  req.Target,
	})
}

// UnmountDisk unmounts a block device
func (h *StorageHandlers) UnmountDisk(c *gin.Context) {
	var req struct {
		Target string `json:"target" binding:"required"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	cleaned := path.Clean(req.Target)
	if strings.Contains(cleaned, "..") {
		Resp(c).FailMsg(CodeInvalidRequest, "invalid target path")
		return
	}
	if isProtectedPartition(cleaned) {
		Resp(c).FailMsg(CodeOperationFailed, "cannot unmount protected partition: "+cleaned)
		return
	}

	cmd := exec.Command("umount", req.Target)
	if out, err := cmd.CombinedOutput(); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "Unmount failed: "+string(out))
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"storage.disk.unmounted",
			eventLoggerPkg.MessageParams{"target": req.Target},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{
		"message": "Successfully unmounted",
		"target":  req.Target,
	})
}

// FormatDisk formats a block device
func (h *StorageHandlers) FormatDisk(c *gin.Context) {
	var req struct {
		Device string `json:"device" binding:"required"`
		FSType string `json:"fstype"` // ext4, vfat etc. Default: ext4
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	if req.FSType == "" {
		req.FSType = "ext4"
	}

	if err := isSafeDevice(req.Device); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}
	if isProtectedDevice(req.Device) {
		Resp(c).FailMsg(CodeOperationFailed, "cannot format protected partition")
		return
	}

	// Ensure it's not mounted first
	_ = exec.Command("umount", req.Device).Run()

	var cmd *exec.Cmd
	if req.FSType == "vfat" || req.FSType == "fat32" {
		cmd = exec.Command("mkfs.vfat", "-I", req.Device)
	} else if req.FSType == "ext4" {
		cmd = exec.Command("mkfs.ext4", "-F", req.Device)
	} else {
		Resp(c).FailMsg(CodeInvalidRequest, "Unsupported filesystem type")
		return
	}

	if out, err := cmd.CombinedOutput(); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "Formatting failed: "+string(out))
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			"storage.disk.formatted",
			eventLoggerPkg.MessageParams{
				"device": req.Device,
				"fstype": req.FSType,
			},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{
		"message": "Successfully formatted",
		"device":  req.Device,
		"fstype":  req.FSType,
	})
}

// isSafeDevice validates that a device path is a legitimate block device partition.
func isSafeDevice(device string) error {
	if strings.Contains(device, "..") {
		return fmt.Errorf("invalid device path: path traversal detected")
	}
	cleaned := path.Clean(device)
	if !strings.HasPrefix(cleaned, "/dev/") {
		return fmt.Errorf("invalid device path: must be under /dev/")
	}
	base := filepath.Base(cleaned)
	// Disallow operating on entire disks (e.g., mmcblk1, sda) — only partitions
	if strings.HasPrefix(base, "mmcblk") && !strings.Contains(base, "p") {
		return fmt.Errorf("cannot operate on entire device %s, specify a partition", base)
	}
	if matched, _ := regexp.MatchString(`^sd[a-z]$`, base); matched {
		return fmt.Errorf("cannot operate on entire device %s, specify a partition", base)
	}
	return nil
}

// isProtectedDevice checks whether a device or mountpoint is protected from dangerous operations.
func isProtectedDevice(deviceOrMount string) bool {
	partitions, _ := disk.Partitions(false)
	for _, p := range partitions {
		if p.Device == deviceOrMount || p.Mountpoint == deviceOrMount {
			return isProtectedPartition(p.Mountpoint)
		}
	}
	return false
}

// autoUnmount finds and lazy-unmounts any mount points belonging to the given device.
// devName can be a whole disk (mmcblk0) or a partition (mmcblk0p2, sda1).
func autoUnmount(devName string) {
	// For whole-disk events (e.g. mmcblk0), match any partition of that disk.
	// For partition events (e.g. mmcblk0p2), match exactly.
	prefix := devName
	data, err := os.ReadFile("/proc/mounts")
	if err != nil {
		return
	}

	var mountpoints []string
	for _, line := range strings.Split(string(data), "\n") {
		fields := strings.Fields(line)
		if len(fields) < 2 {
			continue
		}
		device := fields[0] // e.g. /dev/mmcblk0p2
		mount := fields[1]  // e.g. /mnt/mmcblk0p2

		// Skip protected mount points
		if isProtectedPartition(mount) {
			continue
		}

		base := filepath.Base(device)
		if base == prefix || (len(base) > len(prefix) && strings.HasPrefix(base, prefix+"p")) {
			mountpoints = append(mountpoints, mount)
		}
	}

	for _, mp := range mountpoints {
		_ = exec.Command("umount", "-l", mp).Run()
	}
}

// monitorHotplug monitors for udev block device events
func (h *StorageHandlers) monitorHotplug() {
	// Need stdbuf to prevent output buffering from udevadm when running via pipe
	cmd := exec.Command("stdbuf", "-o0", "udevadm", "monitor", "--kernel", "--subsystem-match=block")
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		return
	}

	if err := cmd.Start(); err != nil {
		return
	}

	scanner := bufio.NewScanner(stdout)
	for scanner.Scan() {
		line := scanner.Text()

		// KERNEL[timestamp] add      /devices/platform/.../block/sda (block)
		if strings.Contains(line, " add ") || strings.Contains(line, " remove ") {
			action := "unknown"
			if strings.Contains(line, " add ") {
				action = "add"
			} else if strings.Contains(line, " remove ") {
				action = "remove"
			}

			// Extract device name e.g. sda, sda1
			parts := strings.Split(line, "/")
			devName := parts[len(parts)-1]
			devName = strings.Split(devName, " ")[0] // remove (block)

			// ignore loop devices usually
			if strings.HasPrefix(devName, "loop") || strings.HasPrefix(devName, "ram") {
				continue
			}

			// Auto-unmount stale mount points on device removal
			if action == "remove" {
				go autoUnmount(devName)
			}

			// Publish to EventBus
			if h.publishEvent != nil {
				h.publishEvent("storage/hotplug", map[string]interface{}{
					"action": action,
					"device": devName,
					"path":   "/dev/" + devName,
					"time":   strconv.FormatInt(time.Now().Unix(), 10),
				})
			}
		}
	}
	_ = cmd.Wait()
}
