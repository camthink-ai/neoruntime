package handlers

import (
	"context"
	"encoding/json"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/shirou/gopsutil/v4/cpu"
	"github.com/shirou/gopsutil/v4/disk"
	"github.com/shirou/gopsutil/v4/host"
	"github.com/shirou/gopsutil/v4/mem"
	"github.com/shirou/gopsutil/v4/net"
	"google.golang.org/grpc"

	inferencepb "aipc/platform/ai-runtime/proto"
	"aipc/platform/platform-api/gyro"
)

// Partitions that must never be unmounted or formatted.
var protectedMountpoints = map[string]bool{
	"/":              true,
	"/boot":          true,
	"/boot/firmware": true,
	"/data":          true,
}

// isRemovableDevice determines if a block device is removable (SD card, USB drive).
// Checks sysfs removable flag and device/type for MMC/SD distinction.
// getParentBlockDev returns the parent block device name for a partition.
// e.g. mmcblk1p2 -> mmcblk1, sda1 -> sda
func getParentBlockDev(partitionName string) string {
	if idx := strings.LastIndex(partitionName, "p"); idx > 0 && strings.HasPrefix(partitionName, "mmcblk") {
		return partitionName[:idx]
	}
	if strings.HasPrefix(partitionName, "sd") && len(partitionName) > 3 {
		return partitionName[:3]
	}
	return partitionName
}

// blockDeviceExists checks if a block device is present in sysfs.
// Returns false for stale mount entries where the physical device was removed.
func blockDeviceExists(devicePath string) bool {
	base := strings.TrimPrefix(devicePath, "/dev/")
	if base == devicePath {
		return false
	}
	parent := getParentBlockDev(base)
	_, err := os.Stat("/sys/block/" + parent)
	return err == nil
}

func isRemovableDevice(devicePath string) bool {
	// Extract block device name from /dev/xxx
	base := strings.TrimPrefix(devicePath, "/dev/")
	if base == devicePath {
		return false
	}
	blockDev := getParentBlockDev(base)

	// Method 1: sysfs removable flag (works for USB and most SD card readers)
	data, err := os.ReadFile("/sys/block/" + blockDev + "/removable")
	if err == nil && strings.TrimSpace(string(data)) == "1" {
		return true
	}

	// Method 2: device/type distinguishes SD cards from internal eMMC
	// mmcblk0 with type=SD is an SD card slot; mmcblk1 with type=MMC is internal eMMC
	if strings.HasPrefix(blockDev, "mmcblk") {
		devType, err := os.ReadFile("/sys/block/" + blockDev + "/device/type")
		if err == nil && strings.TrimSpace(string(devType)) == "SD" {
			return true
		}
	}

	// Method 3: USB devices by path prefix
	if strings.HasPrefix(devicePath, "/dev/sd") {
		return true
	}

	return false
}

func isProtectedPartition(mountpoint string) bool {
	return protectedMountpoints[mountpoint]
}

func getMountpointLabel(mountpoint string) string {
	switch mountpoint {
	case "/":
		return "System (Root)"
	case "/data":
		return "Data"
	case "/boot":
		return "Boot"
	default:
		return mountpoint
	}
}

// Pseudo filesystems to skip when listing disk partitions.
var pseudoFstypes = map[string]bool{
	"sysfs": true, "proc": true, "devfs": true, "devtmpfs": true,
	"securityfs": true, "tmpfs": true, "cgroup": true, "cgroup2": true,
	"pstore": true, "debugfs": true, "tracefs": true, "configfs": true,
	"fusectl": true, "mqueue": true, "hugetlbfs": true, "overlay": true,
}

// Thermal sensor sysfs paths (Hailo-15)
const (
	thermalZoneCPU = "/sys/class/thermal/thermal_zone0/temp"                                   // pvt-ts-0
	thermalZoneNPU = "/sys/class/thermal/thermal_zone1/temp"                                   // pvt-ts-1
	thermalBoardHW = "/sys/devices/platform/105000.i2c/i2c-1/1-0049/hwmon/hwmon1/temp1_input"  // I2C sensor
	thermalSCMI0   = "/sys/devices/platform/firmware:scmi/scmi_dev.4/hwmon/hwmon0/temp1_input" // SCMI fallback CPU
	thermalSCMI1   = "/sys/devices/platform/firmware:scmi/scmi_dev.4/hwmon/hwmon0/temp2_input" // SCMI fallback NPU
)

// MonitorHandler handles system resource monitoring endpoints.
type MonitorHandler struct {
	aiRuntimeConn *grpc.ClientConn
	gyro          gyro.Source
	sampler       *cpuSampler
}

// NewMonitorHandler creates a new MonitorHandler.
//
// gyro may be nil when the IMU source is disabled or unavailable; the gyro
// attitude SSE endpoint then returns 503 while the other monitor endpoints work
// normally.
//
// A background cpuSampler is started so /monitor/* handlers return cached CPU%
// instantly instead of each running a 500ms blocking sample (which
// self-polluted the first dashboard reading and blocked per request).
func NewMonitorHandler(aiRuntimeConn *grpc.ClientConn, gyro gyro.Source) *MonitorHandler {
	s := newCPUSampler()
	s.start()
	return &MonitorHandler{aiRuntimeConn: aiRuntimeConn, gyro: gyro, sampler: s}
}

// Stop shuts down the background CPU sampler goroutine. Safe to call on a nil
// receiver or after the handler is no longer in use.
func (h *MonitorHandler) Stop() {
	if h == nil {
		return
	}
	h.sampler.stopSampler()
}

// GetSummary returns a combined overview of all system resources.
func (h *MonitorHandler) GetSummary(c *gin.Context) {
	cpuPercent := h.sampler.totalPercent()
	cpuCount, _ := cpu.Counts(true)
	memStat, _ := mem.VirtualMemory()
	swapStat, _ := mem.SwapMemory()
	hostInfo, _ := host.Info()
	netCounters, _ := net.IOCounters(false)
	uptime, _ := host.Uptime()

	// Find the largest real (non-pseudo) data partition instead of hardcoding "/".
	// On embedded devices, "/" is a small rootfs while "/data" holds apps/models/logs.
	diskPartitions, _ := disk.Partitions(false)
	var mainDiskUsage *disk.UsageStat
	var mainDiskSize uint64
	for _, p := range diskPartitions {
		if pseudoFstypes[p.Fstype] {
			continue
		}
		usage, err := disk.Usage(p.Mountpoint)
		if err != nil || usage == nil {
			continue
		}
		if usage.Total > mainDiskSize {
			mainDiskSize = usage.Total
			mainDiskUsage = usage
		}
	}

	diskData := gin.H{"total": uint64(0), "used": uint64(0), "free": uint64(0), "usage_percent": float64(0)}
	if mainDiskUsage != nil {
		diskData = gin.H{
			"total":         mainDiskUsage.Total,
			"used":          mainDiskUsage.Used,
			"free":          mainDiskUsage.Free,
			"usage_percent": mainDiskUsage.UsedPercent,
			"mountpoint":    mainDiskUsage.Path,
		}
	}

	summary := gin.H{
		"cpu": gin.H{
			"usage_percent": cpuPercent,
			"cores":         cpuCount,
			"arch":          runtime.GOARCH,
		},
		"memory": gin.H{
			"total":         memStat.Total,
			"used":          memStat.Used,
			"available":     memStat.Available,
			"usage_percent": memStat.UsedPercent,
			"swap_total":    swapStat.Total,
			"swap_used":     swapStat.Used,
		},
		"disk": diskData,
		"host": gin.H{
			"hostname":         hostInfo.Hostname,
			"os":               hostInfo.OS,
			"platform":         hostInfo.Platform,
			"platform_version": hostInfo.PlatformVersion,
			"kernel_version":   hostInfo.KernelVersion,
			"kernel_arch":      hostInfo.KernelArch,
			"uptime_seconds":   uptime,
		},
	}

	if len(netCounters) > 0 {
		summary["network"] = gin.H{
			"bytes_sent": netCounters[0].BytesSent,
			"bytes_recv": netCounters[0].BytesRecv,
		}
	}

	summary["npu"] = h.getNPUUsageFromRuntime()

	Resp(c).OK(summary)
}

// GetCPU returns CPU usage details.
func (h *MonitorHandler) GetCPU(c *gin.Context) {
	percentAll := h.sampler.perCPUPercents()
	percentTotal := h.sampler.totalPercent()
	counts, _ := cpu.Counts(true)
	infos, _ := cpu.Info()

	cores := make([]gin.H, 0, len(infos))
	for i, info := range infos {
		entry := gin.H{"model": info.ModelName, "mhz": info.Mhz}
		if i < len(percentAll) {
			entry["usage_percent"] = percentAll[i]
		}
		cores = append(cores, entry)
	}

	Resp(c).OK(gin.H{
		"total_percent": percentTotal,
		"core_count":    counts,
		"cores":         cores,
	})
}

// GetMemory returns memory and swap usage.
func (h *MonitorHandler) GetMemory(c *gin.Context) {
	v, _ := mem.VirtualMemory()
	s, _ := mem.SwapMemory()

	Resp(c).OK(gin.H{
		"virtual": gin.H{
			"total":         v.Total,
			"used":          v.Used,
			"available":     v.Available,
			"free":          v.Free,
			"usage_percent": v.UsedPercent,
			"buffers":       v.Buffers,
			"cached":        v.Cached,
		},
		"swap": gin.H{
			"total":         s.Total,
			"used":          s.Used,
			"free":          s.Free,
			"usage_percent": s.UsedPercent,
		},
	})
}

// GetDisk returns disk partition usage including unmounted removable partitions.
func (h *MonitorHandler) GetDisk(c *gin.Context) {
	partitions, _ := disk.Partitions(false)
	parts := make([]gin.H, 0, len(partitions))
	seenDevices := make(map[string]bool)

	for _, p := range partitions {
		if pseudoFstypes[p.Fstype] {
			continue
		}

		// Skip stale mount entries where the physical device was removed
		// (e.g. SD card pulled while still mounted at /mnt/mmcblk0p2)
		if !blockDeviceExists(p.Device) {
			continue
		}

		usage, err := disk.Usage(p.Mountpoint)
		if err != nil {
			continue
		}

		isSystem := p.Mountpoint == "/" || p.Mountpoint == "/boot" || p.Mountpoint == "/boot/firmware"

		// Determine if this is a removable device by reading sysfs removable flag.
		// USB drives and SD cards report removable=1; internal eMMC reports removable=0.
		isRemovable := isRemovableDevice(p.Device)

		parts = append(parts, gin.H{
			"device":           p.Device,
			"mountpoint":       p.Mountpoint,
			"fstype":           p.Fstype,
			"total":            usage.Total,
			"used":             usage.Used,
			"free":             usage.Free,
			"usage_percent":    usage.UsedPercent,
			"is_system":        isSystem,
			"is_removable":     isRemovable,
			"is_protected":     isProtectedPartition(p.Mountpoint),
			"mountpoint_label": getMountpointLabel(p.Mountpoint),
		})
		seenDevices[p.Device] = true
	}

	// Scan for unmounted removable partitions (SD cards, USB drives not yet mounted).
	// Use lsblk to find partitions with a filesystem but no mountpoint.
	unmounted := scanUnmountedPartitions(seenDevices)
	parts = append(parts, unmounted...)

	Resp(c).OK(gin.H{"partitions": parts})
}

// scanUnmountedPartitions uses lsblk to find removable partitions with a filesystem
// that are not currently mounted (e.g., freshly inserted SD cards).
func scanUnmountedPartitions(seenDevices map[string]bool) []gin.H {
	cmd := exec.Command("lsblk", "-b", "-J", "-o", "NAME,SIZE,TYPE,MOUNTPOINT,FSTYPE")
	out, err := cmd.Output()
	if err != nil {
		return nil
	}

	var parsed struct {
		Blockdevices []struct {
			Name     string `json:"name"`
			Size     int64  `json:"size"`
			Type     string `json:"type"`
			Children []struct {
				Name       string  `json:"name"`
				Size       int64   `json:"size"`
				Type       string  `json:"type"`
				Mountpoint *string `json:"mountpoint"`
				Fstype     *string `json:"fstype"`
			} `json:"children,omitempty"`
		} `json:"blockdevices"`
	}
	if err := json.Unmarshal(out, &parsed); err != nil {
		return nil
	}

	var result []gin.H
	for _, bd := range parsed.Blockdevices {
		if bd.Type != "disk" {
			continue
		}
		devicePath := "/dev/" + bd.Name
		if !isRemovableDevice(devicePath) {
			continue
		}
		for _, child := range bd.Children {
			if child.Type != "part" || child.Fstype == nil || *child.Fstype == "" {
				continue
			}
			childPath := "/dev/" + child.Name
			if seenDevices[childPath] {
				continue // already mounted and listed above
			}
			if child.Mountpoint != nil && *child.Mountpoint != "" {
				continue // mounted but somehow missed — skip
			}
			result = append(result, gin.H{
				"device":           childPath,
				"mountpoint":       "",
				"fstype":           *child.Fstype,
				"total":            uint64(child.Size),
				"used":             uint64(0),
				"free":             uint64(child.Size),
				"usage_percent":    float64(0),
				"is_system":        false,
				"is_removable":     true,
				"is_protected":     false,
				"mountpoint_label": child.Name,
			})
			seenDevices[childPath] = true
		}
	}
	return result
}

// GetNetwork returns network interface I/O counters.
func (h *MonitorHandler) GetNetwork(c *gin.Context) {
	counters, _ := net.IOCounters(true)
	interfaces := make([]gin.H, 0, len(counters))
	for _, ioc := range counters {
		interfaces = append(interfaces, gin.H{
			"name":         ioc.Name,
			"bytes_sent":   ioc.BytesSent,
			"bytes_recv":   ioc.BytesRecv,
			"packets_sent": ioc.PacketsSent,
			"packets_recv": ioc.PacketsRecv,
			"errin":        ioc.Errin,
			"errout":       ioc.Errout,
		})
	}
	Resp(c).OK(gin.H{"interfaces": interfaces})
}

// GetResourceSnapshot returns a single combined snapshot for dashboard trend charts.
// Includes CPU%, memory%, NPU%, temperatures, and cumulative network bytes.
// Frontend computes network speed from two consecutive snapshots.
func (h *MonitorHandler) GetResourceSnapshot(c *gin.Context) {
	cpuPercent := h.sampler.totalPercent()
	memStat, _ := mem.VirtualMemory()
	netCounters, _ := net.IOCounters(false)
	cpuTemp, npuTemp, boardTemp := getTemperatures()

	snapshot := gin.H{
		"timestamp": time.Now().UnixMilli(),
		"cpu":       cpuPercent,
		"memory":    memStat.UsedPercent,
		"npu":       h.getNPUUsageFromRuntime(),
		"temperatures": gin.H{
			"cpu":   cpuTemp,
			"npu":   npuTemp,
			"board": boardTemp,
		},
	}

	if len(netCounters) > 0 {
		snapshot["network"] = gin.H{
			"bytes_sent": netCounters[0].BytesSent,
			"bytes_recv": netCounters[0].BytesRecv,
		}
	}

	Resp(c).OK(snapshot)
}

// readSysfsTemp reads a sysfs thermal file and converts m°C to °C.
// Returns 0 if the file cannot be read.
func readSysfsTemp(path string) float64 {
	data, err := os.ReadFile(path)
	if err != nil {
		return 0
	}
	raw := strings.TrimSpace(string(data))
	millideg, err := strconv.ParseFloat(raw, 64)
	if err != nil {
		return 0
	}
	return millideg / 1000.0
}

// getTemperatures reads CPU, NPU and board temperatures from sysfs.
// Falls back to SCMI sensors if primary thermal zones are unavailable.
func getTemperatures() (cpuTemp, npuTemp, boardTemp float64) {
	// Primary: thermal_zone0 (pvt-ts-0) and thermal_zone1 (pvt-ts-1)
	cpuTemp = readSysfsTemp(thermalZoneCPU)
	npuTemp = readSysfsTemp(thermalZoneNPU)
	boardTemp = readSysfsTemp(thermalBoardHW)

	// Fallback to SCMI if primary readings are zero
	if cpuTemp == 0 {
		cpuTemp = readSysfsTemp(thermalSCMI0)
	}
	if npuTemp == 0 {
		npuTemp = readSysfsTemp(thermalSCMI1)
	}

	return cpuTemp, npuTemp, boardTemp
}

// getNPUUsageFromRuntime queries the AI Runtime gRPC service for NPU utilization.
// Returns percentage (0-100) or 0 if unavailable.
func (h *MonitorHandler) getNPUUsageFromRuntime() float64 {
	if h.aiRuntimeConn == nil {
		return 0
	}
	client := inferencepb.NewInferenceServiceClient(h.aiRuntimeConn)
	ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
	defer cancel()

	resp, err := client.GetStats(ctx, &inferencepb.Empty{})
	if err != nil {
		return 0
	}
	// device_utilization is 0.0-1.0 from proto, convert to 0-100
	return float64(resp.GetDeviceUtilization()) * 100
}

// Gyro attitude SSE cadences (see docs/references/gyro-attitude-sse.md).
const (
	gyroHeartbeatInterval  = 15 * time.Second
	gyroStatusPollInterval = 2 * time.Second
	gyroDefaultRate        = 50
	gyroMinRate            = 1
	gyroMaxRate            = 200
)

// StreamGyroAttitude streams real-time device attitude (orientation) over SSE.
//
// Query params:
//
//	rate - output frequency cap in Hz, 1..200 (default 50)
//
// The fused quaternion is projected to two-axis level-tilt {pitch, roll} (yaw
// dropped) before emission — see docs/references/gyro-attitude-sse.md. The legacy
// `format=quaternion|euler` param is no longer supported.
//
// Events: orientation, status, error, heartbeat. The connection is kept alive
// across transient sensor unavailability so the client can recover without
// reconnecting. Returns 503 (not SSE) only when the gyro source is disabled
// server-side.
func (h *MonitorHandler) StreamGyroAttitude(c *gin.Context) {
	if h.gyro == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "gyro sensor source not available")
		return
	}

	rate := parseGyroRate(c.Query("rate"))
	minInterval := time.Duration(float64(time.Second) / float64(rate))

	c.Header("Content-Type", "text/event-stream")
	c.Header("Cache-Control", "no-cache")
	c.Header("Connection", "keep-alive")
	c.Header("X-Accel-Buffering", "no")

	sub := h.gyro.Subscribe()
	defer h.gyro.Unsubscribe(sub)

	// Initial status. If the sensor is not online, also push an error event but
	// keep the stream open so the client survives recovery.
	lastStatus := emitGyroStatus(c, h.gyro.Status())

	heartbeat := time.NewTicker(gyroHeartbeatInterval)
	defer heartbeat.Stop()
	statusPoll := time.NewTicker(gyroStatusPollInterval)
	defer statusPoll.Stop()

	ctx := c.Request.Context()
	var lastEmit time.Time
	for {
		select {
		case <-ctx.Done():
			return // client disconnected
		case sample, ok := <-sub:
			if !ok {
				return
			}
			now := time.Now()
			if !lastEmit.IsZero() && now.Sub(lastEmit) < minInterval {
				continue // rate-limit: drop this sample
			}
			lastEmit = now
			pitch, roll := sample.PitchDeg, sample.RollDeg
			if !sample.TiltValid {
				pitch, roll = gyro.QuatToTiltDegrees(sample.Quat)
			}
			c.SSEvent("orientation", gin.H{
				"pitch": pitch,
				"roll":  roll,
			})
			c.Writer.Flush()
		case <-heartbeat.C:
			c.SSEvent("heartbeat", gin.H{"timestamp": time.Now().UnixMilli()})
			c.Writer.Flush()
		case <-statusPoll.C:
			st := h.gyro.Status()
			if st != lastStatus {
				lastStatus = emitGyroStatus(c, st)
			}
		}
	}
}

// emitGyroStatus writes a status event and, when the sensor is not online, a
// following error event. Returns the status it emitted.
func emitGyroStatus(c *gin.Context, st gyro.StatusCode) gyro.StatusCode {
	c.SSEvent("status", gin.H{"sensor": string(st)})
	c.Writer.Flush()
	if st != gyro.StatusOnline {
		code, msg := gyroStatusError(st)
		c.SSEvent("error", gin.H{"code": code, "message": msg})
		c.Writer.Flush()
	}
	return st
}

// gyroStatusError maps a non-online status to an SSE error code/message.
func gyroStatusError(st gyro.StatusCode) (code, msg string) {
	switch st {
	case gyro.StatusError:
		return "SENSOR_ERROR", "IMU read failure"
	default:
		return "SENSOR_UNAVAILABLE", "IMU not detected"
	}
}

// parseGyroRate parses the rate query param, clamped to [1, 200], default 50.
func parseGyroRate(s string) int {
	if s == "" {
		return gyroDefaultRate
	}
	n, err := strconv.Atoi(s)
	if err != nil || n < gyroMinRate {
		return gyroMinRate
	}
	if n > gyroMaxRate {
		return gyroMaxRate
	}
	return n
}
