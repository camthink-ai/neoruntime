package monitor

import (
	"context"
	"fmt"
	"net"
	"os"
	"os/exec"
	"strings"
	"syscall"
	"time"

	"aipc/platform/app-manager/containerd"
	"aipc/platform/app-manager/manifest"
	"aipc/platform/common/logger"

	containerdclient "github.com/containerd/containerd"
)

// HealthChecker performs health checks on containers
type HealthChecker struct {
	client *containerd.Client
}

// NewHealthChecker creates a new health checker
func NewHealthChecker(client *containerd.Client) *HealthChecker {
	return &HealthChecker{
		client: client,
	}
}

// CheckHealth performs a health check on a container
func (h *HealthChecker) CheckHealth(ctx context.Context, container containerdclient.Container, healthcheck manifest.Healthcheck) (bool, error) {
	if !healthcheck.Enabled {
		return true, nil // Health check disabled, assume healthy
	}

	switch healthcheck.Type {
	case "command":
		return h.checkCommand(ctx, container, healthcheck)
	case "http":
		return h.checkHTTP(ctx, container, healthcheck)
	case "tcp":
		return h.checkTCP(ctx, container, healthcheck)
	default:
		logger.Warn("Unknown health check type: %s, assuming healthy", healthcheck.Type)
		return true, nil
	}
}

// checkCommand executes a command inside the container
func (h *HealthChecker) checkCommand(ctx context.Context, container containerdclient.Container, healthcheck manifest.Healthcheck) (bool, error) {
	if healthcheck.Command == "" {
		return false, fmt.Errorf("health check command is empty")
	}

	// Get container task
	task, err := container.Task(ctx, nil)
	if err != nil {
		return false, fmt.Errorf("failed to get container task: %w", err)
	}

	// Execute command in container
	ctx, cancel := context.WithTimeout(ctx, time.Duration(healthcheck.TimeoutSeconds)*time.Second)
	defer cancel()

	// Use exec to run command in container
	execID := fmt.Sprintf("healthcheck-%d", time.Now().UnixNano())
	spec, err := container.Spec(ctx)
	if err != nil {
		return false, fmt.Errorf("failed to get container spec: %w", err)
	}

	// Create exec process
	process, err := task.Exec(ctx, execID, spec.Process, nil)
	if err != nil {
		return false, fmt.Errorf("failed to create exec process: %w", err)
	}

	// Start process
	if err := process.Start(ctx); err != nil {
		return false, fmt.Errorf("failed to start exec process: %w", err)
	}

	// Wait for completion with timeout
	statusCh, err := process.Wait(ctx)
	if err != nil {
		return false, fmt.Errorf("failed to wait for process: %w", err)
	}

	select {
	case status := <-statusCh:
		// Check exit code
		if status.ExitCode() == 0 {
			return true, nil
		}
		return false, fmt.Errorf("health check command exited with code %d", status.ExitCode())
	case <-ctx.Done():
		process.Kill(ctx, syscall.SIGKILL)
		return false, fmt.Errorf("health check timeout")
	}
}

// getContainerIP gets the container's IP address
func (h *HealthChecker) getContainerIP(ctx context.Context, container containerdclient.Container) (string, error) {
	// Get container task
	task, err := container.Task(ctx, nil)
	if err != nil {
		return "", fmt.Errorf("failed to get container task: %w", err)
	}

	pid := task.Pid()
	if pid == 0 {
		return "", fmt.Errorf("container task has no PID")
	}

	// Method 1: Try using nsenter to get IP from container's network namespace
	nsPath := fmt.Sprintf("/proc/%d/ns/net", pid)
	if _, err := os.Stat(nsPath); err == nil {
		// Use nsenter to get IP address from container's network namespace
		cmd := exec.CommandContext(ctx, "nsenter", "-t", fmt.Sprintf("%d", pid), "-n", "ip", "-4", "addr", "show")
		output, err := cmd.Output()
		if err == nil {
			// Parse output to find the first non-loopback IP
			lines := strings.Split(string(output), "\n")
			for _, line := range lines {
				if strings.Contains(line, "inet ") && !strings.Contains(line, "127.0.0.1") {
					fields := strings.Fields(line)
					if len(fields) >= 2 {
						ip := strings.Split(fields[1], "/")[0]
						if parsedIP := net.ParseIP(ip); parsedIP != nil && !parsedIP.IsLoopback() {
							return ip, nil
						}
					}
				}
			}
		}
	}

	// Method 2: Try reading from /proc/{pid}/net/route to find default gateway interface
	// Then read that interface's IP from /proc/{pid}/net/if_inet6 or use ip command
	routeFile := fmt.Sprintf("/proc/%d/net/route", pid)
	if routeData, err := os.ReadFile(routeFile); err == nil {
		// Parse route file to find default interface
		lines := strings.Split(string(routeData), "\n")
		for _, line := range lines {
			fields := strings.Fields(line)
			if len(fields) >= 2 && fields[1] == "00000000" { // Default route
				iface := fields[0]
				// Try to get IP for this interface using nsenter
				cmd := exec.CommandContext(ctx, "nsenter", "-t", fmt.Sprintf("%d", pid), "-n", "ip", "-4", "addr", "show", iface)
				if output, err := cmd.Output(); err == nil {
					outputStr := string(output)
					for _, line := range strings.Split(outputStr, "\n") {
						if strings.Contains(line, "inet ") {
							fields := strings.Fields(line)
							if len(fields) >= 2 {
								ip := strings.Split(fields[1], "/")[0]
								if parsedIP := net.ParseIP(ip); parsedIP != nil && !parsedIP.IsLoopback() {
									return ip, nil
								}
							}
						}
					}
				}
			}
		}
	}

	// Method 3: Try common interface names (eth0, veth*, etc.)
	commonIfaces := []string{"eth0", "eth1", "veth0", "veth1"}
	for _, iface := range commonIfaces {
		cmd := exec.CommandContext(ctx, "nsenter", "-t", fmt.Sprintf("%d", pid), "-n", "ip", "-4", "addr", "show", iface)
		if output, err := cmd.Output(); err == nil {
			outputStr := string(output)
			for _, line := range strings.Split(outputStr, "\n") {
				if strings.Contains(line, "inet ") {
					fields := strings.Fields(line)
					if len(fields) >= 2 {
						ip := strings.Split(fields[1], "/")[0]
						if parsedIP := net.ParseIP(ip); parsedIP != nil && !parsedIP.IsLoopback() {
							return ip, nil
						}
					}
				}
			}
		}
	}

	// Fallback: return error, caller will use localhost
	return "", fmt.Errorf("could not determine container IP address")
}

// checkHTTP performs an HTTP health check
func (h *HealthChecker) checkHTTP(ctx context.Context, container containerdclient.Container, healthcheck manifest.Healthcheck) (bool, error) {
	if healthcheck.Path == "" {
		return false, fmt.Errorf("health check path is empty")
	}

	// Get container IP address
	containerIP, err := h.getContainerIP(ctx, container)
	if err != nil {
		// If we can't get IP, fallback to localhost (might be host network)
		logger.Warn("Failed to get container IP: %v, using localhost", err)
		containerIP = "127.0.0.1"
	}

	// Build URL
	url := fmt.Sprintf("http://%s:%d%s", containerIP, healthcheck.Port, healthcheck.Path)
	if healthcheck.Port == 0 {
		url = fmt.Sprintf("http://%s%s", containerIP, healthcheck.Path)
	}

	// Use curl or http client to check
	ctx, cancel := context.WithTimeout(ctx, time.Duration(healthcheck.TimeoutSeconds)*time.Second)
	defer cancel()

	cmd := exec.CommandContext(ctx, "curl", "-f", "-s", "--max-time", fmt.Sprintf("%d", healthcheck.TimeoutSeconds), url)
	if err := cmd.Run(); err != nil {
		return false, fmt.Errorf("HTTP health check failed for %s: %w", url, err)
	}

	return true, nil
}

// checkTCP performs a TCP health check
func (h *HealthChecker) checkTCP(ctx context.Context, container containerdclient.Container, healthcheck manifest.Healthcheck) (bool, error) {
	if healthcheck.Port == 0 {
		return false, fmt.Errorf("health check port is required for TCP check")
	}

	// Get container IP address
	containerIP, err := h.getContainerIP(ctx, container)
	if err != nil {
		// If we can't get IP, fallback to localhost (might be host network)
		logger.Warn("Failed to get container IP: %v, using localhost", err)
		containerIP = "127.0.0.1"
	}

	address := fmt.Sprintf("%s:%d", containerIP, healthcheck.Port)

	// Try to connect
	ctx, cancel := context.WithTimeout(ctx, time.Duration(healthcheck.TimeoutSeconds)*time.Second)
	defer cancel()

	conn, err := (&net.Dialer{
		Timeout: time.Duration(healthcheck.TimeoutSeconds) * time.Second,
	}).DialContext(ctx, "tcp", address)
	if err != nil {
		return false, fmt.Errorf("TCP health check failed for %s: %w", address, err)
	}
	conn.Close()

	return true, nil
}
