package security

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"aipc/platform/app-manager/manifest"
	"aipc/platform/common/constants"
)

// ContainerConfig represents container security configuration
type ContainerConfig struct {
	// Namespaces
	PIDNamespace   bool
	NETNamespace   bool
	IPCNamespace   bool
	UTSNamespace   bool
	MountNamespace bool
	UserNamespace  bool

	// Capabilities
	DroppedCapabilities []string
	AddedCapabilities   []string

	// Seccomp
	SeccompProfile string

	// Resources
	CPUQuota    int64 // CPU quota in microseconds
	CPUPeriod   int64 // CPU period in microseconds
	MemoryLimit int64 // Memory limit in bytes
	PidsLimit   int64 // Max number of PIDs

	// Filesystem
	ReadonlyRootfs bool
	Mounts         []Mount

	// Other
	NoNewPrivileges bool

	// Multi-container specific
	IsMainContainer bool   // true if this is the main container in multi-container mode
	Role            string // "main" or "sub"
}

type Mount struct {
	Source      string
	Destination string
	Type        string
	Options     []string
	Readonly    bool
}

const writableVolumeMode os.FileMode = 0777

func normalizeHostPath(hostPath string) string {
	if constants.RootPath() != "/opt/aipc" {
		hostPath = strings.Replace(hostPath, "/opt/aipc", constants.RootPath(), 1)
	}
	return hostPath
}

func looksLikeDirectoryMount(hostPath, containerPath string) bool {
	if strings.HasSuffix(hostPath, "/") || strings.HasSuffix(containerPath, "/") {
		return true
	}
	base := filepath.Base(filepath.Clean(containerPath))
	return !strings.Contains(base, ".")
}

func ensureWritableVolumeHostPath(hostPath, containerPath string, readonly bool) error {
	if readonly || hostPath == "" || !filepath.IsAbs(hostPath) {
		return nil
	}

	info, err := os.Stat(hostPath)
	if err == nil {
		if !info.IsDir() || !looksLikeDirectoryMount(hostPath, containerPath) {
			return nil
		}
		return nil
	}
	if !os.IsNotExist(err) {
		return fmt.Errorf("failed to stat host volume path %s: %w", hostPath, err)
	}
	if !looksLikeDirectoryMount(hostPath, containerPath) {
		return fmt.Errorf("writable host volume path %s does not exist and container path %s does not look like a directory", hostPath, containerPath)
	}

	if err := os.MkdirAll(hostPath, writableVolumeMode); err != nil {
		return fmt.Errorf("failed to create writable host volume path %s: %w", hostPath, err)
	}
	if err := os.Chmod(hostPath, writableVolumeMode); err != nil {
		return fmt.Errorf("failed to set permissions on writable host volume path %s: %w", hostPath, err)
	}
	return nil
}

// BuildContainerConfig builds security config from manifest
// seccompProfilePath: absolute path to seccomp profile file, or empty string to use default
func BuildContainerConfig(m *manifest.AppManifest, seccompProfilePath string) (*ContainerConfig, error) {
	cfg := &ContainerConfig{
		// Enable all namespaces by default
		PIDNamespace:   true,
		NETNamespace:   true,
		IPCNamespace:   true,
		UTSNamespace:   true,
		MountNamespace: true,
		UserNamespace:  false, // May require privileges

		// Drop all capabilities by default
		DroppedCapabilities: []string{
			"CAP_SYS_ADMIN",
			"CAP_NET_ADMIN",
			"CAP_SYS_MODULE",
			"CAP_SYS_TIME",
			"CAP_SYS_BOOT",
			"CAP_SYS_NICE",
			"CAP_SYS_RESOURCE",
			"CAP_SYS_RAWIO",
			"CAP_SYS_PTRACE",
			"CAP_SYS_CHROOT",
			"CAP_MKNOD",
		},

		// Readonly rootfs
		ReadonlyRootfs:  false,
		NoNewPrivileges: false,

		// Seccomp profile path
		SeccompProfile: seccompProfilePath,
	}

	// Apply security overrides from manifest
	if m.Spec.Security.NoNewPrivileges != nil {
		cfg.NoNewPrivileges = *m.Spec.Security.NoNewPrivileges
	}
	if m.Spec.Security.ReadonlyRootfs != nil {
		cfg.ReadonlyRootfs = *m.Spec.Security.ReadonlyRootfs
	}

	// Parse CPU quota (optional)
	if m.Spec.Resources.CPU != "" {
		cpuQuota, err := m.Spec.Resources.GetCPUQuota()
		if err != nil {
			return nil, fmt.Errorf("invalid CPU quota: %w", err)
		}
		cfg.CPUPeriod = 100000 // 100ms
		cfg.CPUQuota = int64(cpuQuota * float64(cfg.CPUPeriod))
	} else {
		// No CPU limit if not specified
		cfg.CPUQuota = 0
		cfg.CPUPeriod = 0
	}

	// Parse memory limit (optional)
	if m.Spec.Resources.Memory != "" {
		memBytes, err := m.Spec.Resources.GetMemoryBytes()
		if err != nil {
			return nil, fmt.Errorf("invalid memory limit: %w", err)
		}
		cfg.MemoryLimit = memBytes
	} else {
		// No memory limit if not specified
		cfg.MemoryLimit = 0
	}

	// Default PIDs limit
	cfg.PidsLimit = 128

	// Add volume mounts (replace /opt/aipc prefix with runtime root)
	for _, vol := range m.Spec.Volumes {
		hostPath := normalizeHostPath(vol.Host)
		if err := ensureWritableVolumeHostPath(hostPath, vol.Container, vol.Readonly); err != nil {
			return nil, err
		}
		mount := Mount{
			Source:      hostPath,
			Destination: vol.Container,
			Type:        "bind",
			Options:     []string{"rbind"},
			Readonly:    vol.Readonly,
		}

		cfg.Mounts = append(cfg.Mounts, mount)
	}

	// Mount the /run/aipc directory for all IPC sockets.
	// Mounting the directory (not individual files) ensures containers see
	// fresh socket inodes when services restart and recreate their sockets.
	if fi, err := os.Stat("/run/aipc"); err == nil && fi.IsDir() {
		cfg.Mounts = append(cfg.Mounts, Mount{
			Source:      "/run/aipc",
			Destination: "/run/aipc",
			Type:        "bind",
			Options:     []string{"rbind"},
			Readonly:    false,
		})
	}

	// DMA-BUF device required for mmap on fd-passed video frames
	if len(m.Spec.Permissions.Video) > 0 {
		dmaHeapDir := "/dev/dma_heap"
		if _, err := os.Stat(dmaHeapDir); err == nil {
			cfg.Mounts = append(cfg.Mounts, Mount{
				Source:      dmaHeapDir,
				Destination: dmaHeapDir,
				Type:        "bind",
				Options:     []string{"rbind"},
				Readonly:    true,
			})
		}
	}

	// Host network mode support (required for inbound port plugins like RTSP)
	if m.IsHostNetwork() {
		cfg.NETNamespace = false // Share host network namespace
	}

	return cfg, nil
}

// ApplyDevMode relaxes security for local development and adds source-code bind mounts.
func ApplyDevMode(cfg *ContainerConfig, dev *manifest.DevConfig, appDir string) {
	cfg.ReadonlyRootfs = false
	cfg.NoNewPrivileges = false
	cfg.AddedCapabilities = append(cfg.AddedCapabilities, "CAP_SYS_PTRACE")

	for _, sync := range dev.Sync {
		hostPath := sync.Host
		if !filepath.IsAbs(hostPath) {
			hostPath = filepath.Join(appDir, hostPath)
		}
		cfg.Mounts = append(cfg.Mounts, Mount{
			Source:      hostPath,
			Destination: sync.Container,
			Options:     []string{"rbind"},
		})
	}

	if dev.DebugPort > 0 {
		cfg.NETNamespace = false
	}
}

// BuildContainerConfigForMultiContainer builds security config for a single container
// in a multi-container application. Only main containers get platform socket access.
func BuildContainerConfigForMultiContainer(
	appManifest *manifest.AppManifest,
	containerName string,
	containerSpec *manifest.ContainerSpec,
	seccompProfilePath string,
) (*ContainerConfig, error) {
	cfg := &ContainerConfig{
		// Enable all namespaces by default
		PIDNamespace:   true,
		NETNamespace:   true, // Will be set to false for shared network
		IPCNamespace:   true,
		UTSNamespace:   true,
		MountNamespace: true,
		UserNamespace:  false,

		// Drop all capabilities by default
		DroppedCapabilities: []string{
			"CAP_SYS_ADMIN",
			"CAP_NET_ADMIN",
			"CAP_SYS_MODULE",
			"CAP_SYS_TIME",
			"CAP_SYS_BOOT",
			"CAP_SYS_NICE",
			"CAP_SYS_RESOURCE",
			"CAP_SYS_RAWIO",
			"CAP_SYS_PTRACE",
			"CAP_SYS_CHROOT",
			"CAP_MKNOD",
		},

		// Readonly rootfs
		ReadonlyRootfs:  false,
		NoNewPrivileges: false,

		// Seccomp profile path
		SeccompProfile: seccompProfilePath,

		// Role tracking
		Role:            containerSpec.Role,
		IsMainContainer: containerSpec.Role == "main",
	}

	// Apply security overrides from container spec (falls back to app-level)
	security := containerSpec.Security
	if security.NoNewPrivileges == nil {
		security.NoNewPrivileges = appManifest.Spec.Security.NoNewPrivileges
	}
	if security.ReadonlyRootfs == nil {
		security.ReadonlyRootfs = appManifest.Spec.Security.ReadonlyRootfs
	}
	if security.NoNewPrivileges != nil {
		cfg.NoNewPrivileges = *security.NoNewPrivileges
	}
	if security.ReadonlyRootfs != nil {
		cfg.ReadonlyRootfs = *security.ReadonlyRootfs
	}

	// Parse CPU quota (optional)
	if containerSpec.Resources.CPU != "" {
		cpuQuota, err := containerSpec.Resources.GetCPUQuota()
		if err != nil {
			return nil, fmt.Errorf("invalid CPU quota for container %s: %w", containerName, err)
		}
		cfg.CPUPeriod = 100000 // 100ms
		cfg.CPUQuota = int64(cpuQuota * float64(cfg.CPUPeriod))
	}

	// Parse memory limit (optional)
	if containerSpec.Resources.Memory != "" {
		memBytes, err := containerSpec.Resources.GetMemoryBytes()
		if err != nil {
			return nil, fmt.Errorf("invalid memory limit for container %s: %w", containerName, err)
		}
		cfg.MemoryLimit = memBytes
	}

	// Default PIDs limit
	cfg.PidsLimit = 128

	// Add container-specific volume mounts
	for _, vol := range containerSpec.Volumes {
		hostPath := normalizeHostPath(vol.Name)
		if err := ensureWritableVolumeHostPath(hostPath, vol.Container, vol.Readonly); err != nil {
			return nil, err
		}
		mount := Mount{
			Source:      hostPath, // Volume name or host path
			Destination: vol.Container,
			Type:        "bind",
			Options:     []string{"rbind"},
			Readonly:    vol.Readonly,
		}
		cfg.Mounts = append(cfg.Mounts, mount)
	}

	// Add shared volume mounts from app-level volumes
	for _, vol := range appManifest.Spec.Volumes {
		hostPath := normalizeHostPath(vol.Host)
		if err := ensureWritableVolumeHostPath(hostPath, vol.Container, vol.Readonly); err != nil {
			return nil, err
		}
		mount := Mount{
			Source:      hostPath,
			Destination: vol.Container,
			Type:        "bind",
			Options:     []string{"rbind"},
			Readonly:    vol.Readonly,
		}
		cfg.Mounts = append(cfg.Mounts, mount)
	}

	// ============================================
	// KEY: Only main container gets platform socket access
	// ============================================
	if containerSpec.Role == "main" {
		// Mount the /run/aipc directory for all IPC sockets.
		// Directory-level mount keeps inodes fresh across service restarts.
		if fi, err := os.Stat("/run/aipc"); err == nil && fi.IsDir() {
			cfg.Mounts = append(cfg.Mounts, Mount{
				Source:      "/run/aipc",
				Destination: "/run/aipc",
				Type:        "bind",
				Options:     []string{"rbind"},
				Readonly:    false,
			})
		}

		// DMA-BUF device required for mmap on fd-passed video frames
		if len(containerSpec.Permissions.Video) > 0 {
			dmaHeapDir := "/dev/dma_heap"
			if _, err := os.Stat(dmaHeapDir); err == nil {
				cfg.Mounts = append(cfg.Mounts, Mount{
					Source:      dmaHeapDir,
					Destination: dmaHeapDir,
					Type:        "bind",
					Options:     []string{"rbind"},
					Readonly:    true,
				})
			}
		}

		// Host network mode support
		if appManifest.IsHostNetwork() {
			cfg.NETNamespace = false
		}

		// Video apps need host IPC namespace for DMA-BUF mmap
		if len(containerSpec.Permissions.Video) > 0 {
			cfg.IPCNamespace = false
		}
	}
	// Sub containers: no platform socket mounts, isolated from platform services

	return cfg, nil
}

// ValidateSeccompProfile validates seccomp profile exists
func ValidateSeccompProfile(path string) error {
	if _, err := os.Stat(path); err != nil {
		return fmt.Errorf("seccomp profile not found: %s", path)
	}
	return nil
}

// GenerateDefaultSeccompProfile generates a default seccomp profile
func GenerateDefaultSeccompProfile(outputPath string) error {
	// Basic seccomp profile that blocks dangerous syscalls
	profile := map[string]interface{}{
		"defaultAction": "SCMP_ACT_ERRNO",
		"architectures": []string{"SCMP_ARCH_X86_64", "SCMP_ARCH_AARCH64"},
		"syscalls": []map[string]interface{}{
			{
				"names": []string{
					"read", "write", "open", "close", "stat", "fstat",
					"lseek", "mmap", "mprotect", "munmap", "brk",
					"rt_sigaction", "rt_sigprocmask", "rt_sigreturn",
					"ioctl", "pread64", "pwrite64", "readv", "writev",
					"access", "pipe", "select", "sched_yield", "mremap",
					"msync", "mincore", "madvise", "shmget", "shmat", "shmctl",
					"dup", "dup2", "pause", "nanosleep", "getitimer", "alarm",
					"setitimer", "getpid", "sendfile", "socket", "connect",
					"accept", "sendto", "recvfrom", "sendmsg", "recvmsg",
					"shutdown", "bind", "listen", "getsockname", "getpeername",
					"socketpair", "setsockopt", "getsockopt", "clone", "fork",
					"vfork", "execve", "exit", "wait4", "kill", "uname",
					"fcntl", "flock", "fsync", "fdatasync", "truncate",
					"ftruncate", "getcwd", "chdir", "fchdir", "mkdir",
				},
				"action": "SCMP_ACT_ALLOW",
			},
		},
	}

	data, err := json.MarshalIndent(profile, "", "  ")
	if err != nil {
		return err
	}

	return os.WriteFile(outputPath, data, 0644)
}
