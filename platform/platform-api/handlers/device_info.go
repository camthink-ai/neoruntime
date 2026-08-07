package handlers

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"github.com/shirou/gopsutil/v4/host"
	"github.com/shirou/gopsutil/v4/mem"
	"google.golang.org/grpc"

	camerapb "aipc/platform/camera-daemon/proto"
	"aipc/platform/common/constants"
	"aipc/platform/common/factoryeeprom"
	"aipc/platform/platform-api/config"
)

// DeviceInfoHandler handles device information requests
type DeviceInfoHandler struct {
	configPath   string
	cameraClient *grpc.ClientConn
	configMgr    *config.Manager
}

// NewDeviceInfoHandler creates a new device info handler. configMgr is
// optional: when non-nil, UpdateDeviceName routes through the Config Manager
// (revision history + audit + auto-restore); when nil it falls back to the
// direct setHostname+saveDeviceName path.
func NewDeviceInfoHandler(configPath string, cameraClient *grpc.ClientConn, configMgr *config.Manager) *DeviceInfoHandler {
	if configPath == "" {
		configPath = constants.ConfigPath() + "/device.conf"
	}
	return &DeviceInfoHandler{configPath: configPath, cameraClient: cameraClient, configMgr: configMgr}
}

// DeviceInfo represents comprehensive device information
type DeviceInfo struct {
	DeviceName      string             `json:"device_name"`
	Model           string             `json:"model"`
	SerialNumber    string             `json:"serial_number"`
	FirmwareVersion string             `json:"firmware_version"`
	BuildDate       string             `json:"build_date"`
	GitCommit       string             `json:"git_commit"`
	HardwareVersion string             `json:"hardware_version"`
	OSVersion       string             `json:"os_version"`     // e.g. "Hailo15 1.11.0"
	OSBuildTime     string             `json:"os_build_time"`  // from BUILD_TIME in /etc/build-info
	Distro          string             `json:"distro"`         // e.g. "Poky 4.0.23"
	KernelVersion   string             `json:"kernel_version"` // e.g. "5.15.0-hailo"
	SoC             SoCInfo            `json:"soc"`
	CPU             CPUInfo            `json:"cpu"`
	Memory          MemoryInfo         `json:"memory"`
	CameraModule    CameraInfo         `json:"camera_module"`
	MACAddress      string             `json:"mac_address"`
	IPAddress       string             `json:"ip_address"`
	Uptime          uint64             `json:"uptime"`
	UptimeFormatted string             `json:"uptime_formatted"`
	InstallPrefix   string             `json:"install_prefix"`
	OTA             OTAInfo            `json:"ota"`
	Factory         factoryeeprom.Info `json:"factory"`
}

// CameraInfo contains camera sensor module information
type CameraInfo struct {
	Model       string `json:"model"`
	I2CBus      int32  `json:"i2c_bus,omitempty"`
	I2CAddress  string `json:"i2c_address,omitempty"`
	PixelFormat int32  `json:"pixel_format,omitempty"`
}

// OTAInfo contains OTA update information
type OTAInfo struct {
	UpdateAvailable bool   `json:"update_available"`
	CurrentVersion  string `json:"current_version"`
	LatestVersion   string `json:"latest_version,omitempty"`
	Changelog       string `json:"changelog,omitempty"`
}

// SoCInfo contains SoC specific information
type SoCInfo struct {
	Vendor string `json:"vendor"`
	Model  string `json:"model"`
	Core   string `json:"core,omitempty"`
	NPU    string `json:"npu,omitempty"`
}

// CPUInfo contains CPU information
type CPUInfo struct {
	Model     string  `json:"model"`
	Cores     int     `json:"cores"`
	Frequency float64 `json:"frequency_mhz"`
}

// MemoryInfo contains memory information
type MemoryInfo struct {
	TotalGB     float64 `json:"total_gb"`
	UsedGB      float64 `json:"used_gb"`
	UsedPercent float64 `json:"used_percent"`
}

// DeviceInfoRequest for updating device name
type DeviceInfoRequest struct {
	DeviceName string `json:"device_name"`
}

// FactoryFieldRequest writes one whitelisted factory EEPROM field.
type FactoryFieldRequest struct {
	Field string `json:"field"`
	Value string `json:"value"`
}

// GetDeviceInfo returns comprehensive device information
func (h *DeviceInfoHandler) GetDeviceInfo(c *gin.Context) {
	versionInfo := h.getVersionInfo()
	factoryInfo := h.getFactoryInfo(c.Request.Context())

	info := &DeviceInfo{
		DeviceName:      h.getDeviceName(),
		Model:           h.getModel(),
		SerialNumber:    h.getSerialNumber(factoryInfo),
		FirmwareVersion: versionInfo.Version,
		BuildDate:       versionInfo.BuildDate,
		GitCommit:       versionInfo.GitCommit,
		HardwareVersion: h.getHardwareVersion(factoryInfo),
		OSVersion:       h.getOSVersion(),
		OSBuildTime:     h.getOSBuildTime(),
		Distro:          h.getDistro(),
		KernelVersion:   h.getKernelVersion(),
		SoC:             h.getSoCInfo(),
		CPU:             h.getCPUInfo(),
		Memory:          h.getMemoryInfo(),
		CameraModule:    h.getCameraInfo(),
		MACAddress:      firstNonEmpty(h.getMACAddress(), factoryInfo.MACAddress),
		IPAddress:       getHostFromRequest(c),
		Uptime:          h.getUptime(),
		UptimeFormatted: h.formatUptime(h.getUptime()),
		InstallPrefix:   constants.RootPath(),
		Factory:         factoryInfo,
		OTA: OTAInfo{
			UpdateAvailable: false,
			CurrentVersion:  versionInfo.Version,
		},
	}

	Resp(c).OK(info)
}

// GetFactoryInfo returns the raw factory EEPROM fields read through the
// platform-approved factory-eeprom.sh tool.
func (h *DeviceInfoHandler) GetFactoryInfo(c *gin.Context) {
	Resp(c).OK(h.getFactoryInfo(c.Request.Context()))
}

// UpdateFactoryField writes one production-maintenance factory field and
// verifies the value by reading it back immediately.
func (h *DeviceInfoHandler) UpdateFactoryField(c *gin.Context) {
	if !requireJSONContentType(c) {
		return
	}

	var req FactoryFieldRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request: "+err.Error())
		return
	}

	field, err := factoryeeprom.NormalizeField(req.Field)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidParameter, err.Error())
		return
	}
	value, err := factoryeeprom.ValidateValue(field, req.Value)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidParameter, err.Error())
		return
	}

	info, err := factoryeeprom.DefaultClient().SetAndVerify(c.Request.Context(), string(field), value)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidParameter, err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"field":   string(field),
		"value":   value,
		"factory": info,
		"message": "Factory EEPROM field updated and verified",
	})
}

// UpdateDeviceName updates the device name
func (h *DeviceInfoHandler) UpdateDeviceName(c *gin.Context) {
	var req DeviceInfoRequest
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request: "+err.Error())
		return
	}

	if req.DeviceName == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Device name cannot be empty")
		return
	}

	// Validate device name (no special characters)
	if matched, _ := regexp.MatchString(`^[a-zA-Z0-9_-]+$`, req.DeviceName); !matched {
		Resp(c).FailMsg(CodeInvalidRequest, "Device name can only contain letters, numbers, underscores and hyphens")
		return
	}

	// Route through the Config Controller when available: the adapter does the
	// hostname set + device.conf write atomically with Verify + auto-restore,
	// and the Manager records the revision + audit event. Falls back to the
	// direct path when the Manager is absent (no DB).
	if h.configMgr != nil {
		desired, err := json.Marshal(struct {
			DeviceName string `json:"device_name"`
		}{DeviceName: req.DeviceName})
		if err != nil {
			Resp(c).FailMsg(CodeInvalidRequest, "Failed to encode value: "+err.Error())
			return
		}
		if _, _, err := h.configMgr.Apply(c.Request.Context(), "device_info", "device_name", string(desired), getUsernameFromContext(c)); err != nil {
			Resp(c).FailMsg(CodeOperationFailed, "Failed to set device name: "+err.Error())
			return
		}
		Resp(c).OK(gin.H{
			"device_name": req.DeviceName,
			"message":     "Device name updated successfully",
		})
		return
	}

	// No DB (configMgr is nil): preserve the original direct-set behavior.
	// Update hostname
	if err := h.setHostname(req.DeviceName); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "Failed to set hostname: "+err.Error())
		return
	}

	// Save to config file
	if err := h.saveDeviceName(req.DeviceName); err != nil {
		Resp(c).FailMsg(CodeOperationFailed, "Failed to save device name: "+err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"device_name": req.DeviceName,
		"message":     "Device name updated successfully",
	})
}

func (h *DeviceInfoHandler) getDeviceName() string {
	// First try config file
	if data, err := os.ReadFile(h.configPath); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "DEVICE_NAME=") {
				return strings.TrimSpace(strings.TrimPrefix(line, "DEVICE_NAME="))
			}
		}
	}

	// Fallback to hostname
	if hostname, err := os.Hostname(); err == nil {
		return hostname
	}

	return "AIPC-Device"
}

func (h *DeviceInfoHandler) getModel() string {
	// Try config file first (allows OEM customization)
	if data, err := os.ReadFile(h.configPath); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "MODEL=") {
				if v := strings.TrimSpace(strings.TrimPrefix(line, "MODEL=")); v != "" {
					return v
				}
			}
		}
	}

	// Try device tree model
	if data, err := os.ReadFile("/sys/firmware/devicetree/base/model"); err == nil {
		return strings.TrimRight(string(data), "\x00")
	}

	// Try cpuinfo for model
	if data, err := os.ReadFile("/proc/cpuinfo"); err == nil {
		scanner := bufio.NewScanner(strings.NewReader(string(data)))
		for scanner.Scan() {
			line := scanner.Text()
			if strings.HasPrefix(line, "Model") || strings.HasPrefix(line, "Hardware") {
				parts := strings.SplitN(line, ":", 2)
				if len(parts) == 2 {
					return strings.TrimSpace(parts[1])
				}
			}
		}
	}

	return "Unknown"
}

func (h *DeviceInfoHandler) getFactoryInfo(ctx context.Context) factoryeeprom.Info {
	info, _ := factoryeeprom.DefaultClient().Read(ctx)
	return info
}

func (h *DeviceInfoHandler) getSerialNumber(factoryInfo factoryeeprom.Info) string {
	if serial := strings.TrimSpace(factoryInfo.SerialNumber); serial != "" {
		return serial
	}

	// Try device tree serial number
	if data, err := os.ReadFile("/sys/firmware/devicetree/base/serial-number"); err == nil {
		serial := strings.TrimRight(string(data), "\x00")
		if serial != "" {
			return serial
		}
	}

	// Try cpuinfo for serial
	if data, err := os.ReadFile("/proc/cpuinfo"); err == nil {
		scanner := bufio.NewScanner(strings.NewReader(string(data)))
		for scanner.Scan() {
			line := scanner.Text()
			if strings.HasPrefix(line, "Serial") {
				parts := strings.SplitN(line, ":", 2)
				if len(parts) == 2 {
					return strings.TrimSpace(parts[1])
				}
			}
		}
	}

	return ""
}

// VersionInfo contains version information from VERSION file
type VersionInfo struct {
	Version   string
	BuildDate string
	GitCommit string
}

func (h *DeviceInfoHandler) getVersionInfo() VersionInfo {
	info := VersionInfo{
		Version:   "1.0.0",
		BuildDate: "",
		GitCommit: "",
	}

	// Read VERSION file
	data, err := os.ReadFile(constants.RootPath() + "/VERSION")
	if err != nil {
		return info
	}

	for _, line := range strings.Split(string(data), "\n") {
		if key, value, found := strings.Cut(line, "="); found {
			switch key {
			case "version":
				info.Version = strings.TrimSpace(value)
			case "build_date":
				info.BuildDate = strings.TrimSpace(value)
			case "git_commit":
				info.GitCommit = strings.TrimSpace(value)
			}
		}
	}

	return info
}

func (h *DeviceInfoHandler) getHardwareVersion(factoryInfo factoryeeprom.Info) string {
	if hwRev := strings.TrimSpace(factoryInfo.HardwareRevision); hwRev != "" {
		return hwRev
	}

	// Try device tree
	if data, err := os.ReadFile("/sys/firmware/devicetree/base/hardware-version"); err == nil {
		return strings.TrimRight(string(data), "\x00")
	}

	// Try config file
	if data, err := os.ReadFile(h.configPath); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			if strings.HasPrefix(line, "HARDWARE_VERSION=") {
				return strings.TrimSpace(strings.TrimPrefix(line, "HARDWARE_VERSION="))
			}
		}
	}

	return "1.0"
}

func (h *DeviceInfoHandler) getOSVersion() string {
	data, err := os.ReadFile("/etc/os-release")
	if err != nil {
		return ""
	}

	var name, version string
	for _, line := range strings.Split(string(data), "\n") {
		if key, value, found := strings.Cut(line, "="); found {
			v := strings.Trim(value, `"'`)
			switch key {
			case "PRETTY_NAME":
				return v
			case "NAME":
				name = v
			case "VERSION":
				version = v
			}
		}
	}
	if name != "" {
		if version != "" {
			return name + " " + version
		}
		return name
	}
	return ""
}

func (h *DeviceInfoHandler) getDistro() string {
	data, err := os.ReadFile("/etc/build-info")
	if err != nil {
		return ""
	}

	var distro, distroVer string
	for _, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSpace(line)
		if key, value, found := strings.Cut(line, "="); found {
			key = strings.TrimSpace(key)
			value = strings.TrimSpace(value)
			switch key {
			case "DISTRO":
				distro = value
			case "DISTRO_VERSION":
				distroVer = value
			}
		}
	}
	if distro != "" {
		if distroVer != "" {
			return distro + " " + distroVer
		}
		return distro
	}
	return ""
}

func (h *DeviceInfoHandler) getOSBuildTime() string {
	data, err := os.ReadFile("/etc/build-info")
	if err != nil {
		return ""
	}

	for _, line := range strings.Split(string(data), "\n") {
		if key, value, found := strings.Cut(line, "="); found && strings.TrimSpace(key) == "BUILD_TIME" {
			return strings.TrimSpace(value)
		}
	}
	return ""
}

func (h *DeviceInfoHandler) getKernelVersion() string {
	if out, err := exec.Command("uname", "-r").Output(); err == nil {
		return strings.TrimSpace(string(out))
	}
	if data, err := os.ReadFile("/proc/version"); err == nil {
		fields := strings.Fields(string(data))
		if len(fields) >= 3 {
			return fields[2]
		}
	}
	return ""
}

func (h *DeviceInfoHandler) getSoCInfo() SoCInfo {
	info := SoCInfo{}

	// Read from cpuinfo
	data, err := os.ReadFile("/proc/cpuinfo")
	if err != nil {
		return info
	}

	content := string(data)
	scanner := bufio.NewScanner(strings.NewReader(content))

	for scanner.Scan() {
		line := scanner.Text()
		parts := strings.SplitN(line, ":", 2)
		if len(parts) != 2 {
			continue
		}
		key := strings.TrimSpace(parts[0])
		value := strings.TrimSpace(parts[1])

		switch {
		case strings.Contains(strings.ToLower(key), "vendor_id") || strings.Contains(strings.ToLower(key), "vendor"):
			info.Vendor = value
		case strings.Contains(strings.ToLower(key), "model name") || strings.Contains(strings.ToLower(key), "model"):
			if info.Model == "" || !strings.Contains(info.Model, "NPU") {
				info.Model = value
			}
		case strings.Contains(strings.ToLower(key), "cpu"):
			info.Core = value
		}
	}

	// Detect NPU based on model
	if strings.Contains(strings.ToLower(info.Model), "hailo") {
		info.Vendor = "Hailo"
		info.NPU = "Hailo-15 NPU"
	} else if strings.Contains(strings.ToLower(info.Model), "rk3588") || strings.Contains(strings.ToLower(content), "rk3588") {
		info.Vendor = "Rockchip"
		info.Model = "RK3588"
		info.NPU = "NPU 6 TOPS"
	} else if strings.Contains(strings.ToLower(info.Model), "tegra") || strings.Contains(strings.ToLower(content), "tegra") {
		info.Vendor = "NVIDIA"
		info.NPU = "Jetson GPU"
	}

	if info.Vendor == "" {
		info.Vendor = "Unknown"
	}

	return info
}

func (h *DeviceInfoHandler) getCPUInfo() CPUInfo {
	info := CPUInfo{}

	// Get CPU model and cores from cpuinfo
	if data, err := os.ReadFile("/proc/cpuinfo"); err == nil {
		scanner := bufio.NewScanner(strings.NewReader(string(data)))
		coreCount := 0
		for scanner.Scan() {
			line := scanner.Text()
			if strings.HasPrefix(line, "model name") || strings.HasPrefix(line, "Model") {
				parts := strings.SplitN(line, ":", 2)
				if len(parts) == 2 && info.Model == "" {
					info.Model = strings.TrimSpace(parts[1])
				}
			}
			if strings.HasPrefix(line, "processor") {
				coreCount++
			}
		}
		info.Cores = coreCount
	}

	// Get CPU frequency
	if data, err := os.ReadFile("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq"); err == nil {
		if freq, err := strconv.ParseFloat(strings.TrimSpace(string(data)), 64); err == nil {
			info.Frequency = freq / 1000 // Convert kHz to MHz
		}
	} else if data, err := os.ReadFile("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq"); err == nil {
		if freq, err := strconv.ParseFloat(strings.TrimSpace(string(data)), 64); err == nil {
			info.Frequency = freq / 1000 // Convert kHz to MHz
		}
	}

	if info.Cores == 0 {
		info.Cores = 1
	}

	return info
}

func (h *DeviceInfoHandler) getMemoryInfo() MemoryInfo {
	info := MemoryInfo{}

	if vmStat, err := mem.VirtualMemory(); err == nil {
		info.TotalGB = float64(vmStat.Total) / (1024 * 1024 * 1024)
		info.UsedGB = float64(vmStat.Used) / (1024 * 1024 * 1024)
		info.UsedPercent = vmStat.UsedPercent
	}

	return info
}

func (h *DeviceInfoHandler) getCameraInfo() CameraInfo {
	// Try gRPC to camera-daemon first (provides full sensor details)
	if h.cameraClient != nil {
		client := camerapb.NewCameraControlClient(h.cameraClient)
		ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
		defer cancel()

		resp, err := client.GetSensorInfo(ctx, &camerapb.GetSensorInfoRequest{SensorIndex: 0})
		if err == nil && resp.Available {
			return CameraInfo{
				Model:       resp.SensorModel,
				I2CBus:      resp.I2CBus,
				I2CAddress:  resp.I2CAddress,
				PixelFormat: resp.PixelFormat,
			}
		}
	}

	// Fallback: read from device tree / config file (model name only)
	info := CameraInfo{Model: "Unknown"}
	paths := []string{
		"/sys/firmware/devicetree/base/camera/model",
		"/sys/firmware/devicetree/base/camera0/model",
		constants.ConfigPath() + "/camera-module",
	}
	for _, path := range paths {
		if data, err := os.ReadFile(path); err == nil {
			if model := strings.TrimRight(string(data), "\x00"); model != "" {
				info.Model = model
				break
			}
		}
	}
	return info
}

func (h *DeviceInfoHandler) getMACAddress() string {
	// Get MAC from eth0
	path := "/sys/class/net/eth0/address"
	if data, err := os.ReadFile(path); err == nil {
		return strings.TrimSpace(string(data))
	}

	// Try other interfaces
	interfaces := []string{"enp0s0", "wlan0", "en0"}
	for _, iface := range interfaces {
		path := fmt.Sprintf("/sys/class/net/%s/address", iface)
		if data, err := os.ReadFile(path); err == nil {
			return strings.TrimSpace(string(data))
		}
	}

	return ""
}

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if trimmed := strings.TrimSpace(value); trimmed != "" {
			return trimmed
		}
	}
	return ""
}

// getHostFromRequest extracts the host IP from the HTTP request Host header.
// This is the IP the client used to reach the device, which is the correct
// address for constructing URLs (RTSP, etc.).
func getHostFromRequest(c *gin.Context) string {
	host := c.Request.Host
	if idx := strings.LastIndex(host, ":"); idx != -1 {
		host = host[:idx]
	}
	return host
}

func (h *DeviceInfoHandler) getUptime() uint64 {
	if hostInfo, err := host.Info(); err == nil {
		return hostInfo.Uptime
	}
	return 0
}

func (h *DeviceInfoHandler) formatUptime(seconds uint64) string {
	days := seconds / 86400
	hours := (seconds % 86400) / 3600
	mins := (seconds % 3600) / 60

	if days > 0 {
		return fmt.Sprintf("%d days, %d hours, %d minutes", days, hours, mins)
	}
	if hours > 0 {
		return fmt.Sprintf("%d hours, %d minutes", hours, mins)
	}
	return fmt.Sprintf("%d minutes", mins)
}

func (h *DeviceInfoHandler) setHostname(name string) error {
	// Set transient hostname
	if err := exec.Command("hostname", name).Run(); err != nil {
		return fmt.Errorf("failed to set transient hostname: %w", err)
	}

	// Write to /etc/hostname
	if err := os.WriteFile("/etc/hostname", []byte(name+"\n"), 0644); err != nil {
		return fmt.Errorf("failed to write /etc/hostname: %w", err)
	}

	return nil
}

func (h *DeviceInfoHandler) saveDeviceName(name string) error {
	// Ensure directory exists
	dir := filepath.Dir(h.configPath)
	if err := os.MkdirAll(dir, 0755); err != nil {
		return fmt.Errorf("failed to create config directory: %w", err)
	}

	// Read existing config
	var lines []string
	existingData, err := os.ReadFile(h.configPath)
	if err == nil {
		lines = strings.Split(string(existingData), "\n")
	}

	// Update or add DEVICE_NAME
	found := false
	for i, line := range lines {
		if strings.HasPrefix(line, "DEVICE_NAME=") {
			lines[i] = "DEVICE_NAME=" + name
			found = true
			break
		}
	}

	if !found {
		lines = append(lines, "DEVICE_NAME="+name)
	}

	// Write back
	content := strings.Join(lines, "\n")
	if err := os.WriteFile(h.configPath, []byte(content), 0644); err != nil {
		return fmt.Errorf("failed to write config file: %w", err)
	}

	return nil
}
