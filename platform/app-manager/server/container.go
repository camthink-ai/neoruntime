package server

import (
	"aipc/platform/common/constants"
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"aipc/platform/app-manager/proto"
	"aipc/platform/app-manager/registry"
	"aipc/platform/common/logger"
	"google.golang.org/grpc"
)

// ListContainers lists all containers
func (s *AppManagerServer) ListContainers(ctx context.Context, req *proto.ListContainersRequest) (*proto.ContainerList, error) {
	ctx = s.withNamespace(ctx)
	containers, err := s.client.ListContainers(ctx)
	if err != nil {
		return nil, err
	}

	var results []*proto.ContainerInfo
	for _, c := range containers {
		info, err := s.client.GetContainerInfo(ctx, c)
		if err != nil {
			logger.Warn("Failed to get container info for %s: %v", c.ID(), err)
			continue
		}

		// Filter
		if req.State != "" && req.State != "all" {
			if strings.ToLower(info.Status) != strings.ToLower(req.State) {
				continue
			}
		}

		if req.Search != "" {
			if !strings.Contains(info.ID, req.Search) && !strings.Contains(info.Image, req.Search) {
				continue
			}
		}

		// Get stats for running containers
		var cpuPercent float64
		var memoryUsage int64
		if info.Status == "running" {
			stats, err := s.client.GetContainerStats(ctx, c)
			if err == nil {
				cpuPercent = stats.CPUPercent
				memoryUsage = int64(stats.MemoryUsage)
			}
		}

		results = append(results, &proto.ContainerInfo{
			Id:          info.ID,
			Name:        info.ID, // We use ID as name currently
			Image:       info.Image,
			Status:      info.Status,
			State:       info.Status,
			Pid:         int32(info.Pid),
			CreatedAt:   info.CreatedAt.Format(time.RFC3339),
			CpuPercent:  cpuPercent,
			MemoryUsage: memoryUsage,
		})
	}

	return &proto.ContainerList{Containers: results}, nil
}

// GetContainer gets detailed info for a single container
func (s *AppManagerServer) GetContainer(ctx context.Context, req *proto.GetContainerRequest) (*proto.ContainerDetail, error) {
	ctx = s.withNamespace(ctx)
	c, err := s.client.GetContainer(ctx, req.Id)
	if err != nil {
		return nil, fmt.Errorf("failed to get container: %w", err)
	}

	info, err := s.client.GetContainerInfo(ctx, c)
	if err != nil {
		return nil, err
	}

	return &proto.ContainerDetail{
		Info: &proto.ContainerInfo{
			Id:        info.ID,
			Name:      info.ID,
			Image:     info.Image,
			Status:    info.Status,
			State:     info.Status,
			Pid:       int32(info.Pid),
			CreatedAt: info.CreatedAt.Format(time.RFC3339),
		},
	}, nil
}

// GetContainerStats gets resource stats for a container
func (s *AppManagerServer) GetContainerStats(ctx context.Context, req *proto.GetContainerRequest) (*proto.ContainerStats, error) {
	ctx = s.withNamespace(ctx)
	c, err := s.client.GetContainer(ctx, req.Id)
	if err != nil {
		return nil, fmt.Errorf("failed to get container: %w", err)
	}

	stats, err := s.client.GetContainerStats(ctx, c)
	if err != nil {
		return nil, err
	}

	// Calculate memory percentage
	var memoryPercent float64
	if stats.MemoryLimit > 0 {
		memoryPercent = float64(stats.MemoryUsage) / float64(stats.MemoryLimit) * 100
	}

	return &proto.ContainerStats{
		CpuUsage:      float64(stats.CPUUsage),
		CpuPercent:    stats.CPUPercent,
		MemoryUsage:   int64(stats.MemoryUsage),
		MemoryLimit:   int64(stats.MemoryLimit),
		MemoryPercent: memoryPercent,
		Pids:          int32(stats.Pids),
	}, nil
}

// GetContainerLogs streams container logs by reading from log files.
// Uses the same proven approach as GetAppLogs for reliability.
func (s *AppManagerServer) GetContainerLogs(req *proto.GetContainerLogsRequest, stream grpc.ServerStreamingServer[proto.LogLine]) error {
	logPath := s.findContainerLogPath(req.Id)
	if logPath == "" {
		return fmt.Errorf("no log file found for container %s", req.Id)
	}
	logger.Info("GetContainerLogs: container=%s logPath=%s follow=%v tail=%d", req.Id, logPath, req.Follow, req.Tail)

	tail := int(req.Tail)
	if tail <= 0 {
		tail = 100
	}

	// Use efficient tail reading — avoids loading entire file into memory
	lines, err := tailFile(logPath, tail)
	if err != nil {
		return fmt.Errorf("failed to read log file: %w", err)
	}
	for _, line := range lines {
		if err := stream.Send(&proto.LogLine{Message: line}); err != nil {
			return err
		}
	}

	if !req.Follow {
		return nil
	}

	file, err := os.Open(logPath)
	if err != nil {
		return fmt.Errorf("failed to open log file for follow: %w", err)
	}
	defer file.Close()

	stat, err := file.Stat()
	if err != nil {
		return fmt.Errorf("failed to stat log file: %w", err)
	}
	lastSize := stat.Size()
	var partialLine string

	logger.Info("GetContainerLogs: entering follow mode, initial size=%d", lastSize)

	for {
		select {
		case <-stream.Context().Done():
			logger.Info("GetContainerLogs: stream context done for %s", req.Id)
			return nil
		default:
			stat, err := os.Stat(logPath)
			if err != nil {
				time.Sleep(500 * time.Millisecond)
				continue
			}

			currentSize := stat.Size()
			if currentSize > lastSize {
				file.Seek(lastSize, 0)
				newBytes := make([]byte, currentSize-lastSize)
				n, err := file.Read(newBytes)
				if err != nil && err != io.EOF {
					time.Sleep(500 * time.Millisecond)
					continue
				}

				content := partialLine + string(newBytes[:n])
				logLines := strings.Split(content, "\n")

				for i, line := range logLines {
					if i == len(logLines)-1 {
						if !strings.HasSuffix(content, "\n") {
							partialLine = line
						} else if line != "" {
							if err := stream.Send(&proto.LogLine{Message: line}); err != nil {
								return err
							}
						}
					} else if line != "" {
						if err := stream.Send(&proto.LogLine{Message: line}); err != nil {
							return err
						}
					}
				}

				lastSize = currentSize
			} else if currentSize < lastSize {
				lastSize = 0
				partialLine = ""
			}

			time.Sleep(200 * time.Millisecond)
		}
	}
}

// tailFile reads the last n lines from a file efficiently without loading
// the entire file into memory.
func tailFile(path string, n int) ([]string, error) {
	f, err := os.Open(path)
	if err != nil {
		return nil, err
	}
	defer f.Close()

	const blockSize = 4096
	stat, err := f.Stat()
	if err != nil {
		return nil, err
	}
	size := stat.Size()
	if size == 0 {
		return nil, nil
	}

	// Read backwards from end of file, counting newlines
	var buf []byte
	remaining := n + 1 // we need n+1 newlines to get n complete lines
	offset := size

	for remaining > 0 && offset > 0 {
		readSize := int64(blockSize)
		if offset < readSize {
			readSize = offset
		}
		offset -= readSize

		chunk := make([]byte, readSize)
		if _, err := f.ReadAt(chunk, offset); err != nil {
			return nil, err
		}
		buf = append(chunk, buf...)

		for i := len(chunk) - 1; i >= 0; i-- {
			if chunk[i] == '\n' {
				remaining--
				if remaining <= 0 {
					break
				}
			}
		}
	}

	// Split and return non-empty lines
	lines := strings.Split(string(buf), "\n")
	var result []string
	for _, l := range lines {
		l = strings.TrimRight(l, "\r")
		if l != "" {
			result = append(result, l)
		}
	}
	if len(result) > n {
		result = result[len(result)-n:]
	}
	return result, nil
}

// findContainerLogPath locates the active log file for a container.
// Prioritizes app-based paths (matching GetAppLogs behavior) over direct container paths.
func (s *AppManagerServer) findContainerLogPath(containerID string) string {
	// Priority 1: lookup by container ID in app registry
	if app, ok := s.registry.GetByContainerID(containerID); ok && app.InstancePath != "" {
		p := filepath.Join(app.InstancePath, "logs", "app.log")
		if _, err := os.Stat(p); err == nil {
			logger.Debug("findContainerLogPath: found via registry containerID=%s -> %s", containerID, p)
			return p
		}
	}

	// Priority 2: strip "aipc-" prefix and try as app ID
	if appID := strings.TrimPrefix(containerID, "aipc-"); appID != containerID {
		if app, err := s.registry.Get(appID); err == nil && app.InstancePath != "" {
			p := filepath.Join(app.InstancePath, "logs", "app.log")
			if _, err := os.Stat(p); err == nil {
				logger.Debug("findContainerLogPath: found via appID=%s -> %s", appID, p)
				return p
			}
		}
	}

	// Priority 3: try containerID as app ID directly
	if app, err := s.registry.Get(containerID); err == nil && app.InstancePath != "" {
		p := filepath.Join(app.InstancePath, "logs", "app.log")
		if _, err := os.Stat(p); err == nil {
			logger.Debug("findContainerLogPath: found via direct appID=%s -> %s", containerID, p)
			return p
		}
	}

	// Priority 4: fallback to direct container log path (non-app containers)
	p := fmt.Sprintf(constants.LogPath()+"/containers/%s.log", containerID)
	if _, err := os.Stat(p); err == nil {
		logger.Debug("findContainerLogPath: found container log -> %s", p)
		return p
	}

	logger.Warn("findContainerLogPath: no log file found for %s", containerID)
	return ""
}

// StartContainer manually starts a container
func (s *AppManagerServer) StartContainer(ctx context.Context, req *proto.ContainerRequest) (*proto.Status, error) {
	ctx = s.withNamespace(ctx)
	c, err := s.client.GetContainer(ctx, req.Id)
	if err != nil {
		return &proto.Status{Success: false, Message: err.Error()}, nil
	}

	// Prefer app instance log path if container belongs to an app
	var logPath string
	if app, ok := s.registry.GetByContainerID(req.Id); ok && app.InstancePath != "" {
		logPath = filepath.Join(app.InstancePath, "logs", "app.log")
	} else if appID := strings.TrimPrefix(req.Id, "aipc-"); appID != req.Id {
		if app, err := s.registry.Get(appID); err == nil && app.InstancePath != "" {
			logPath = filepath.Join(app.InstancePath, "logs", "app.log")
		}
	}
	if logPath == "" {
		logPath = fmt.Sprintf(constants.LogPath()+"/containers/%s.log", req.Id)
	}
	os.MkdirAll(filepath.Dir(logPath), 0755)

	_, err = s.client.StartContainer(ctx, c, logPath)
	if err != nil {
		return &proto.Status{Success: false, Message: err.Error()}, nil
	}

	// Check if this container belongs to an app and update state
	if app, ok := s.registry.GetByContainerID(req.Id); ok {
		logger.Info("Container %s belongs to app %s, updating state to running", req.Id, app.ID)
		if err := s.registry.SetState(app.ID, registry.AppStateRunning); err != nil {
			logger.Warn("Failed to update app state: %v", err)
		}
	}

	return &proto.Status{Success: true, Message: "Container started"}, nil
}

// StopContainer manually stops a container
func (s *AppManagerServer) StopContainer(ctx context.Context, req *proto.ContainerRequest) (*proto.Status, error) {
	ctx = s.withNamespace(ctx)
	c, err := s.client.GetContainer(ctx, req.Id)
	if err != nil {
		return &proto.Status{Success: false, Message: err.Error()}, nil
	}

	task, err := c.Task(ctx, nil)
	if err != nil {
		return &proto.Status{Success: false, Message: err.Error()}, nil
	}

	// Use a background context for termination to ensure it completes even if RPC times out
	termCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	termCtx = s.withNamespace(termCtx)

	err = s.client.StopContainer(termCtx, task, 10*time.Second)
	if err != nil {
		return &proto.Status{Success: false, Message: err.Error()}, nil
	}

	// Check if this container belongs to an app and update state
	if app, ok := s.registry.GetByContainerID(req.Id); ok {
		logger.Info("Container %s belongs to app %s, updating state to stopped", req.Id, app.ID)
		// Remove from auto-restart
		if s.autoRestart != nil {
			s.autoRestart.RemoveApp(app.ID)
		}
		// Update state to stopped
		if err := s.registry.SetState(app.ID, registry.AppStateStopped); err != nil {
			logger.Warn("Failed to update app state: %v", err)
		}
	}

	return &proto.Status{Success: true, Message: "Container stopped"}, nil
}

// RestartContainer manually restarts a container
func (s *AppManagerServer) RestartContainer(ctx context.Context, req *proto.ContainerRequest) (*proto.Status, error) {
	_, err := s.StopContainer(ctx, req)
	if err != nil {
		return &proto.Status{Success: false, Message: err.Error()}, nil
	}

	return s.StartContainer(ctx, req)
}

// RemoveContainer manually completely removes a container
func (s *AppManagerServer) RemoveContainer(ctx context.Context, req *proto.RemoveContainerRequest) (*proto.Status, error) {
	ctx = s.withNamespace(ctx)
	c, err := s.client.GetContainer(ctx, req.Id)
	if err != nil {
		return &proto.Status{Success: false, Message: err.Error()}, nil
	}

	// Check if this container belongs to an app
	containerID := c.ID()
	if app, ok := s.registry.GetByContainerID(containerID); ok {
		logger.Info("Container %s belongs to app %s, updating registry before removal", containerID, app.ID)
		// Remove from auto-restart
		if s.autoRestart != nil {
			s.autoRestart.RemoveApp(app.ID)
		}
		// Update state to stopped (since we are removing the container)
		if err := s.registry.SetState(app.ID, registry.AppStateStopped); err != nil {
			logger.Warn("Failed to update app state in registry: %v", err)
		}
		// Clear container ID in registry
		if err := s.registry.SetContainerID(app.ID, ""); err != nil {
			logger.Warn("Failed to clear container ID in registry: %v", err)
		}
	}

	// Use a background context for removal
	remCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	remCtx = s.withNamespace(remCtx)

	err = s.client.RemoveContainer(remCtx, c)
	if err != nil {
		return &proto.Status{Success: false, Message: err.Error()}, nil
	}

	return &proto.Status{Success: true, Message: "Container removed"}, nil
}
