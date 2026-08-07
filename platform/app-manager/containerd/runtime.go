/**
 * @file runtime.go
 * @brief Container Runtime Operations
 *
 * Provides high-level container runtime operations for app management.
 * Supports both single-container and multi-container (Main/Sub) applications.
 */

package containerd

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"

	"aipc/platform/app-manager/manifest"
	"aipc/platform/app-manager/security"
	"aipc/platform/common/constants"
	"aipc/platform/common/logger"

	"github.com/containerd/containerd"
	"github.com/containerd/containerd/containers"
	"github.com/containerd/containerd/oci"
	"github.com/opencontainers/runtime-spec/specs-go"
)

const (
	AIPCGroupGID uint32 = 1001
)

// Runtime provides container runtime operations
type Runtime struct {
	client        *Client
	instancesPath string
}

// NewRuntime creates a new container runtime
func NewRuntime(client *Client, instancesPath string) *Runtime {
	return &Runtime{
		client:        client,
		instancesPath: instancesPath,
	}
}

// ============================================
// Single Container Operations (Existing)
// ============================================

// CreateAppContainer creates and configures a container for an app
func (r *Runtime) CreateAppContainer(ctx context.Context, appID string, appManifest *manifest.AppManifest, containerConfig *security.ContainerConfig) (containerd.Container, error) {
	containerID := fmt.Sprintf("aipc-%s", appID)

	// Build OCI spec options
	opts := r.buildOCIOptions(appID, appManifest, containerConfig)

	// Get normalized image name (adds docker.io/ prefix if needed)
	imageName := appManifest.GetNormalizedImage()

	// Create container
	container, err := r.client.CreateContainer(ctx, containerID, imageName, opts...)
	if err != nil {
		return nil, fmt.Errorf("failed to create container: %w", err)
	}

	logger.Info("App container created: app_id=%s, container_id=%s, image=%s", appID, containerID, imageName)
	return container, nil
}

// StartAppContainer starts an app container
func (r *Runtime) StartAppContainer(ctx context.Context, container containerd.Container, appID string) (containerd.Task, error) {
	// Create log path
	logPath := filepath.Join(r.instancesPath, appID, "logs", "app.log")
	if err := os.MkdirAll(filepath.Dir(logPath), 0755); err != nil {
		logger.Warn("Failed to create log directory: %v", err)
		logPath = "" // Fallback to stdout/stderr
	}

	// Start container
	task, err := r.client.StartContainer(ctx, container, logPath)
	if err != nil {
		return nil, fmt.Errorf("failed to start container: %w", err)
	}

	logger.Info("App container started: app_id=%s, container_id=%s", appID, container.ID())
	return task, nil
}

// StopAppContainer stops an app container gracefully
func (r *Runtime) StopAppContainer(ctx context.Context, task containerd.Task, timeoutSeconds int32) error {
	timeout := time.Duration(timeoutSeconds) * time.Second
	if timeout <= 0 {
		timeout = 10 * time.Second // Default timeout
	}

	if err := r.client.StopContainer(ctx, task, timeout); err != nil {
		return fmt.Errorf("failed to stop container: %w", err)
	}

	logger.Info("App container stopped: task_id=%s", task.ID())
	return nil
}

// RemoveAppContainer removes an app container
func (r *Runtime) RemoveAppContainer(ctx context.Context, container containerd.Container) error {
	if err := r.client.RemoveContainer(ctx, container); err != nil {
		return fmt.Errorf("failed to remove container: %w", err)
	}

	logger.Info("App container removed: container_id=%s", container.ID())
	return nil
}

// GetContainerStatus gets the status of a container
func (r *Runtime) GetContainerStatus(ctx context.Context, container containerd.Container) (string, error) {
	info, err := r.client.GetContainerInfo(ctx, container)
	if err != nil {
		return "", err
	}
	return info.Status, nil
}

// ============================================
// Multi-Container Operations (Main/Sub Architecture)
// ============================================

// MultiContainerInstance represents a running multi-container application
type MultiContainerInstance struct {
	AppID      string
	Containers map[string]containerd.Container // name -> container
	Tasks      map[string]containerd.Task      // name -> task
	NetworkNS  string                          // Shared network namespace path
}

// CreateMultiContainerApp creates all containers for a multi-container application
// Returns the instance containing all containers
func (r *Runtime) CreateMultiContainerApp(
	ctx context.Context,
	appID string,
	appManifest *manifest.AppManifest,
	seccompProfilePath string,
) (*MultiContainerInstance, error) {

	instance := &MultiContainerInstance{
		AppID:      appID,
		Containers: make(map[string]containerd.Container),
		Tasks:      make(map[string]containerd.Task),
	}

	// Get startup order
	startupOrder := appManifest.GetStartupOrder()

	// Create containers in startup order
	for _, containerName := range startupOrder {
		containerSpec, ok := appManifest.Spec.Containers[containerName]
		if !ok {
			return nil, fmt.Errorf("container %q not found in manifest", containerName)
		}

		// Build container config
		containerConfig, err := security.BuildContainerConfigForMultiContainer(
			appManifest,
			containerName,
			&containerSpec,
			seccompProfilePath,
		)
		if err != nil {
			return nil, fmt.Errorf("failed to build config for container %s: %w", containerName, err)
		}

		// Set shared network namespace for all containers (internal communication)
		// In internal mode, all containers share the same network namespace
		if appManifest.Spec.Networking.Mode == "internal" || appManifest.Spec.Networking.Mode == "" {
			// First container creates the network namespace
			// Subsequent containers join it
			if instance.NetworkNS != "" {
				// TODO: Implement network namespace sharing
				// This requires creating a network namespace and passing it to other containers
			}
		}

		// Build OCI options
		containerID := fmt.Sprintf("aipc-%s-%s", appID, containerName)
		opts := r.buildOCIOptionsForContainer(appID, containerName, &containerSpec, containerConfig)

		// Normalize image name
		imageName := manifest.NormalizeImageName(containerSpec.Image)

		// Create container
		container, err := r.client.CreateContainer(ctx, containerID, imageName, opts...)
		if err != nil {
			// Cleanup already created containers on failure
			r.cleanupMultiContainer(ctx, instance)
			return nil, fmt.Errorf("failed to create container %s: %w", containerName, err)
		}

		instance.Containers[containerName] = container
		logger.Info("Multi-container created: app_id=%s, container=%s, role=%s, image=%s",
			appID, containerName, containerSpec.Role, imageName)
	}

	return instance, nil
}

// StartMultiContainerApp starts all containers in the correct order
func (r *Runtime) StartMultiContainerApp(
	ctx context.Context,
	appID string,
	instance *MultiContainerInstance,
	appManifest *manifest.AppManifest,
) error {
	startupOrder := appManifest.GetStartupOrder()

	for _, containerName := range startupOrder {
		container, ok := instance.Containers[containerName]
		if !ok {
			return fmt.Errorf("container %q not found in instance", containerName)
		}

		// Create log path
		logPath := filepath.Join(r.instancesPath, appID, "logs", containerName+".log")
		if err := os.MkdirAll(filepath.Dir(logPath), 0755); err != nil {
			logger.Warn("Failed to create log directory for %s: %v", containerName, err)
			logPath = ""
		}

		// Start container
		task, err := r.client.StartContainer(ctx, container, logPath)
		if err != nil {
			// Stop already started containers on failure
			r.stopMultiContainer(ctx, instance, appManifest)
			return fmt.Errorf("failed to start container %s: %w", containerName, err)
		}

		instance.Tasks[containerName] = task
		logger.Info("Multi-container started: app_id=%s, container=%s", appID, containerName)

		// Small delay between container starts to allow dependencies to initialize
		time.Sleep(500 * time.Millisecond)
	}

	return nil
}

// StopMultiContainerApp stops all containers in the correct order
func (r *Runtime) StopMultiContainerApp(
	ctx context.Context,
	instance *MultiContainerInstance,
	appManifest *manifest.AppManifest,
	timeoutSeconds int32,
) error {
	return r.stopMultiContainer(ctx, instance, appManifest)
}

// stopMultiContainer internal implementation
func (r *Runtime) stopMultiContainer(
	ctx context.Context,
	instance *MultiContainerInstance,
	appManifest *manifest.AppManifest,
) error {
	shutdownOrder := appManifest.GetShutdownOrder()

	for _, containerName := range shutdownOrder {
		task, ok := instance.Tasks[containerName]
		if !ok {
			continue
		}

		// Stop task
		timeout := 10 * time.Second
		if err := r.client.StopContainer(ctx, task, timeout); err != nil {
			logger.Warn("Failed to stop container %s: %v", containerName, err)
		} else {
			logger.Info("Multi-container stopped: app_id=%s, container=%s", instance.AppID, containerName)
		}

		delete(instance.Tasks, containerName)
	}

	return nil
}

// RemoveMultiContainerApp removes all containers
func (r *Runtime) RemoveMultiContainerApp(ctx context.Context, instance *MultiContainerInstance) error {
	return r.cleanupMultiContainer(ctx, instance)
}

// cleanupMultiContainer removes all containers in the instance
func (r *Runtime) cleanupMultiContainer(ctx context.Context, instance *MultiContainerInstance) error {
	for name, container := range instance.Containers {
		if err := r.client.RemoveContainer(ctx, container); err != nil {
			logger.Warn("Failed to remove container %s: %v", name, err)
		} else {
			logger.Info("Multi-container removed: app_id=%s, container=%s", instance.AppID, name)
		}
	}
	return nil
}

// buildOCIOptions builds OCI spec options from manifest and security config
func (r *Runtime) buildOCIOptions(appID string, appManifest *manifest.AppManifest, containerConfig *security.ContainerConfig) []oci.SpecOpts {
	var opts []oci.SpecOpts

	// Set working directory
	opts = append(opts, oci.WithProcessCwd("/app"))

	// Set environment variables
	envVars := appManifest.ToContainerEnv()
	envVars = append(envVars, fmt.Sprintf("AIPC_HOST_PREFIX=%s", constants.RootPath()))
	envVars = append(envVars, fmt.Sprintf("APP_ID=%s", appManifest.Metadata.ID))
	opts = append(opts, oci.WithEnv(envVars))

	// Resource limits
	if containerConfig.CPUQuota > 0 && containerConfig.CPUPeriod > 0 {
		// WithCPUCFS expects (quota int64, period uint64) in microseconds
		opts = append(opts, oci.WithCPUCFS(
			containerConfig.CPUQuota,          // int64
			uint64(containerConfig.CPUPeriod), // uint64
		))
	}

	if containerConfig.MemoryLimit > 0 {
		opts = append(opts, oci.WithMemoryLimit(uint64(containerConfig.MemoryLimit)))
	}

	if containerConfig.PidsLimit > 0 {
		opts = append(opts, oci.WithPidsLimit(int64(containerConfig.PidsLimit)))
	}

	// Mounts
	for _, mount := range containerConfig.Mounts {
		mountOptions := mount.Options
		if mount.Readonly {
			mountOptions = append(mountOptions, "ro")
		}
		opts = append(opts, oci.WithMounts([]specs.Mount{
			{
				Destination: mount.Destination,
				Type:        mount.Type,
				Source:      mount.Source,
				Options:     mountOptions,
			},
		}))
	}

	// Security options
	if containerConfig.ReadonlyRootfs {
		opts = append(opts, oci.WithRootFSReadonly())
	}

	if containerConfig.NoNewPrivileges {
		opts = append(opts, oci.WithNoNewPrivileges)
	}

	// Capabilities
	if len(containerConfig.DroppedCapabilities) > 0 {
		// Convert string capabilities to specs.LinuxCapability
		droppedCaps := make([]string, len(containerConfig.DroppedCapabilities))
		copy(droppedCaps, containerConfig.DroppedCapabilities)
		opts = append(opts, oci.WithDroppedCapabilities(droppedCaps))
	}
	if len(containerConfig.AddedCapabilities) > 0 {
		addedCaps := make([]string, len(containerConfig.AddedCapabilities))
		copy(addedCaps, containerConfig.AddedCapabilities)
		opts = append(opts, oci.WithAddedCapabilities(addedCaps))
	}

	// Namespaces: containerd creates new namespaces by default.
	if !containerConfig.NETNamespace {
		opts = append(opts, oci.WithHostNamespace(specs.NetworkNamespace))
		opts = append(opts, oci.WithHostNamespace(specs.UTSNamespace))
	}
	if !containerConfig.IPCNamespace {
		opts = append(opts, oci.WithHostNamespace(specs.IPCNamespace))
	}

	// Seccomp (if profile path provided)
	if containerConfig.SeccompProfile != "" {
		seccompOpt, err := r.loadSeccompProfile(containerConfig.SeccompProfile)
		if err != nil {
			logger.Warn("Failed to load seccomp profile %s: %v, container will run without seccomp", containerConfig.SeccompProfile, err)
		} else {
			opts = append(opts, seccompOpt)
			logger.Info("Seccomp profile loaded: %s", containerConfig.SeccompProfile)
		}
	}

	// Device permissions
	// Use WithAllDevicesAllowed to avoid BPF_CGROUP_DEVICE errors on kernels
	// that don't support BPF device control. This allows all devices and
	// uses traditional devices cgroup controller instead of BPF.
	// Note: Device access is still controlled via capabilities (CAP_MKNOD dropped)
	// and readonly rootfs, so security is maintained.
	opts = append(opts, oci.WithAllDevicesAllowed)

	// Inject AIPC group GID for socket access
	// This allows containers to access platform sockets (ai-runtime.sock, event-bus.sock, etc.)
	// without requiring users to configure group membership in their Dockerfiles
	opts = append(opts, r.withAIPCGroupAccess())

	return opts
}

// buildOCIOptionsForContainer builds OCI spec options for a single container in multi-container mode
func (r *Runtime) buildOCIOptionsForContainer(
	appID string,
	containerName string,
	containerSpec *manifest.ContainerSpec,
	containerConfig *security.ContainerConfig,
) []oci.SpecOpts {
	var opts []oci.SpecOpts

	// Set working directory
	opts = append(opts, oci.WithProcessCwd("/app"))

	// Set environment variables from container spec
	envVars := make([]string, 0, len(containerSpec.Env))
	for _, envVar := range containerSpec.Env {
		envVars = append(envVars, fmt.Sprintf("%s=%s", envVar.Name, manifest.ExpandEnvRefs(envVar.Value)))
	}
	// Add role environment variable for app to know its role
	envVars = append(envVars, fmt.Sprintf("APP_ROLE=%s", containerSpec.Role))
	envVars = append(envVars, fmt.Sprintf("AIPC_HOST_PREFIX=%s", constants.RootPath()))
	envVars = append(envVars, fmt.Sprintf("APP_ID=%s", appID))
	envVars = append(envVars, fmt.Sprintf("CONTAINER_NAME=%s", containerName))
	opts = append(opts, oci.WithEnv(envVars))

	// Resource limits
	if containerConfig.CPUQuota > 0 && containerConfig.CPUPeriod > 0 {
		opts = append(opts, oci.WithCPUCFS(
			containerConfig.CPUQuota,
			uint64(containerConfig.CPUPeriod),
		))
	}

	if containerConfig.MemoryLimit > 0 {
		opts = append(opts, oci.WithMemoryLimit(uint64(containerConfig.MemoryLimit)))
	}

	if containerConfig.PidsLimit > 0 {
		opts = append(opts, oci.WithPidsLimit(int64(containerConfig.PidsLimit)))
	}

	// Mounts
	for _, mount := range containerConfig.Mounts {
		mountOptions := mount.Options
		if mount.Readonly {
			mountOptions = append(mountOptions, "ro")
		}
		opts = append(opts, oci.WithMounts([]specs.Mount{
			{
				Destination: mount.Destination,
				Type:        mount.Type,
				Source:      mount.Source,
				Options:     mountOptions,
			},
		}))
	}

	// Security options
	if containerConfig.ReadonlyRootfs {
		opts = append(opts, oci.WithRootFSReadonly())
	}

	if containerConfig.NoNewPrivileges {
		opts = append(opts, oci.WithNoNewPrivileges)
	}

	// Capabilities
	if len(containerConfig.DroppedCapabilities) > 0 {
		droppedCaps := make([]string, len(containerConfig.DroppedCapabilities))
		copy(droppedCaps, containerConfig.DroppedCapabilities)
		opts = append(opts, oci.WithDroppedCapabilities(droppedCaps))
	}
	if len(containerConfig.AddedCapabilities) > 0 {
		addedCaps := make([]string, len(containerConfig.AddedCapabilities))
		copy(addedCaps, containerConfig.AddedCapabilities)
		opts = append(opts, oci.WithAddedCapabilities(addedCaps))
	}

	// Namespaces
	if !containerConfig.NETNamespace {
		opts = append(opts, oci.WithHostNamespace(specs.NetworkNamespace))
		opts = append(opts, oci.WithHostNamespace(specs.UTSNamespace))
	}
	if !containerConfig.IPCNamespace {
		opts = append(opts, oci.WithHostNamespace(specs.IPCNamespace))
	}

	// Seccomp
	if containerConfig.SeccompProfile != "" {
		seccompOpt, err := r.loadSeccompProfile(containerConfig.SeccompProfile)
		if err != nil {
			logger.Warn("Failed to load seccomp profile %s: %v", containerConfig.SeccompProfile, err)
		} else {
			opts = append(opts, seccompOpt)
		}
	}

	// Device permissions
	opts = append(opts, oci.WithAllDevicesAllowed)

	// Inject AIPC group GID for socket access
	// Note: For sub containers, this is harmless since they don't have the sockets mounted
	opts = append(opts, r.withAIPCGroupAccess())

	return opts
}

// withAIPCGroupAccess returns an oci.SpecOpts that injects the AIPC group GID
// into the container's additional groups, enabling access to platform sockets.
// This is transparent to users - they don't need to configure anything in their Dockerfiles.
func (r *Runtime) withAIPCGroupAccess() oci.SpecOpts {
	return func(_ context.Context, _ oci.Client, _ *containers.Container, s *oci.Spec) error {
		if s.Process == nil {
			s.Process = &specs.Process{}
		}

		// Check if GID is already in AdditionalGids
		for _, gid := range s.Process.User.AdditionalGids {
			if gid == AIPCGroupGID {
				return nil
			}
		}

		// Inject AIPC group GID
		s.Process.User.AdditionalGids = append(s.Process.User.AdditionalGids, AIPCGroupGID)

		logger.Debug("Injected AIPC group GID %d for socket access", AIPCGroupGID)
		return nil
	}
}

// loadSeccompProfile loads and parses a seccomp profile from file
// Returns an oci.SpecOpts that applies the seccomp profile to the container spec
func (r *Runtime) loadSeccompProfile(profilePath string) (oci.SpecOpts, error) {
	// Resolve profile path (handle relative paths)
	if !filepath.IsAbs(profilePath) {
		// If relative, try common locations
		possiblePaths := []string{
			filepath.Join(constants.ConfigPath(), profilePath),
			filepath.Join("/etc/aipc", profilePath),
			filepath.Join("/opt/aipc/etc/security", profilePath),
		}
		found := false
		for _, path := range possiblePaths {
			if _, err := os.Stat(path); err == nil {
				profilePath = path
				found = true
				break
			}
		}
		if !found {
			return nil, fmt.Errorf("seccomp profile not found: %s (tried: %v)", profilePath, possiblePaths)
		}
	}

	// Read profile file
	profileData, err := os.ReadFile(profilePath)
	if err != nil {
		return nil, fmt.Errorf("failed to read seccomp profile: %w", err)
	}

	// Parse JSON to specs.LinuxSeccomp
	var seccompProfile specs.LinuxSeccomp
	if err := json.Unmarshal(profileData, &seccompProfile); err != nil {
		return nil, fmt.Errorf("failed to parse seccomp profile JSON: %w", err)
	}

	// Validate profile structure
	if err := validateSeccompProfile(&seccompProfile); err != nil {
		return nil, fmt.Errorf("invalid seccomp profile: %w", err)
	}

	// Return SpecOpts that applies the seccomp profile
	return func(_ context.Context, _ oci.Client, _ *containers.Container, s *oci.Spec) error {
		if s.Linux == nil {
			s.Linux = &specs.Linux{}
		}
		s.Linux.Seccomp = &seccompProfile
		return nil
	}, nil
}

// validateSeccompProfile validates the structure of a seccomp profile
func validateSeccompProfile(profile *specs.LinuxSeccomp) error {
	if profile == nil {
		return fmt.Errorf("profile is nil")
	}

	// Validate default action
	validActions := map[specs.LinuxSeccompAction]bool{
		specs.ActKill:        true,
		specs.ActTrap:        true,
		specs.ActErrno:       true,
		specs.ActTrace:       true,
		specs.ActAllow:       true,
		specs.ActLog:         true,
		specs.ActKillProcess: true,
	}
	if !validActions[profile.DefaultAction] {
		return fmt.Errorf("invalid default action: %s", profile.DefaultAction)
	}

	// Validate architectures (if specified)
	// Keep compatibility across runtime-spec versions by checking the canonical
	// seccomp architecture prefix instead of hard-coding a complete enum list.
	for _, arch := range profile.Architectures {
		archName := strings.TrimSpace(string(arch))
		if archName == "" {
			return fmt.Errorf("empty architecture in profile")
		}
		if !strings.HasPrefix(archName, "SCMP_ARCH_") {
			return fmt.Errorf("invalid architecture: %s", archName)
		}
	}

	// Validate syscalls
	for i, syscall := range profile.Syscalls {
		if len(syscall.Names) == 0 {
			return fmt.Errorf("syscall entry %d: must specify names", i)
		}
		if !validActions[syscall.Action] {
			return fmt.Errorf("syscall entry %d: invalid action %s", i, syscall.Action)
		}
	}

	return nil
}
