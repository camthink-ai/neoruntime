package handlers

import (
	"aipc/platform/common/constants"
	"aipc/platform/common/utils"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/gin-gonic/gin"
	"google.golang.org/protobuf/types/known/emptypb"
	"gopkg.in/yaml.v3"

	apppb "aipc/platform/app-manager/proto"
	"aipc/platform/common/events"
	"aipc/platform/common/logger"
)

// AppPermissions mirrors the manifest permissions for JSON response.
type AppPermissions struct {
	Video     []string               `json:"video,omitempty" yaml:"video"`
	Inference *InferencePermsSummary `json:"inference,omitempty"`
	Events    *EventPermsSummary     `json:"events,omitempty"`
	Device    *DevicePermsSummary    `json:"device,omitempty"`
	Network   *NetworkPermsSummary   `json:"network,omitempty"`
}

type InferencePermsSummary struct {
	Models        []string `json:"models,omitempty" yaml:"models"`
	MaxQPS        int      `json:"max_qps,omitempty" yaml:"max_qps"`
	MaxConcurrent int      `json:"max_concurrent,omitempty" yaml:"max_concurrent"`
	AllowRegister bool     `json:"allow_register_model,omitempty" yaml:"allow_register_model"`
}

type EventPermsSummary struct {
	Publish   []string `json:"publish,omitempty" yaml:"publish"`
	Subscribe []string `json:"subscribe,omitempty" yaml:"subscribe"`
}

type DevicePermsSummary struct {
	Light bool `json:"light,omitempty" yaml:"light"`
	IrCut bool `json:"ir_cut,omitempty" yaml:"ir_cut"`
	PTZ   bool `json:"ptz,omitempty" yaml:"ptz"`
	Lens  bool `json:"lens,omitempty" yaml:"lens"`
}

type NetworkPermsSummary struct {
	Mode     string   `json:"mode,omitempty" yaml:"mode"`
	Outbound []string `json:"outbound,omitempty" yaml:"outbound"`
	Inbound  []int    `json:"inbound,omitempty" yaml:"inbound"`
}

// readAppPermissions reads the manifest YAML and extracts permissions.
func readAppPermissions(manifestPath string) *AppPermissions {
	if manifestPath == "" {
		return nil
	}
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		return nil
	}
	var raw struct {
		Spec struct {
			Permissions struct {
				Video     []string `yaml:"video"`
				Inference struct {
					Models        []string `yaml:"models"`
					MaxQPS        int      `yaml:"max_qps"`
					MaxConcurrent int      `yaml:"max_concurrent"`
					AllowRegister bool     `yaml:"allow_register_model"`
				} `yaml:"inference"`
				Events struct {
					Publish   []string `yaml:"publish"`
					Subscribe []string `yaml:"subscribe"`
				} `yaml:"events"`
				Device struct {
					Light bool `yaml:"light"`
					IrCut bool `yaml:"ir_cut"`
					PTZ   bool `yaml:"ptz"`
					Lens  bool `yaml:"lens"`
				} `yaml:"device"`
				Network struct {
					Mode     string   `yaml:"mode"`
					Outbound []string `yaml:"outbound"`
					Inbound  []int    `yaml:"inbound"`
				} `yaml:"network"`
			} `yaml:"permissions"`
		} `yaml:"spec"`
	}
	if err := yaml.Unmarshal(data, &raw); err != nil {
		return nil
	}

	p := raw.Spec.Permissions
	result := &AppPermissions{}

	if len(p.Video) > 0 {
		result.Video = p.Video
	}
	if len(p.Inference.Models) > 0 || p.Inference.MaxQPS > 0 || p.Inference.AllowRegister {
		result.Inference = &InferencePermsSummary{
			Models:        p.Inference.Models,
			MaxQPS:        p.Inference.MaxQPS,
			MaxConcurrent: p.Inference.MaxConcurrent,
			AllowRegister: p.Inference.AllowRegister,
		}
	}
	if len(p.Events.Publish) > 0 || len(p.Events.Subscribe) > 0 {
		result.Events = &EventPermsSummary{
			Publish:   p.Events.Publish,
			Subscribe: p.Events.Subscribe,
		}
	}
	if p.Device.Light || p.Device.IrCut || p.Device.PTZ || p.Device.Lens {
		result.Device = &DevicePermsSummary{
			Light: p.Device.Light,
			IrCut: p.Device.IrCut,
			PTZ:   p.Device.PTZ,
			Lens:  p.Device.Lens,
		}
	}
	if p.Network.Mode != "" && p.Network.Mode != "isolated" {
		result.Network = &NetworkPermsSummary{
			Mode:     p.Network.Mode,
			Outbound: p.Network.Outbound,
			Inbound:  p.Network.Inbound,
		}
	}

	return result
}

// App Manager handlers

func (h *APIHandlers) ListApps(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.ListApps(ctx, &emptypb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	// Sync container actual state
	containersResp, _ := client.ListContainers(ctx, &apppb.ListContainersRequest{})
	containerStates := make(map[string]string)
	if containersResp != nil {
		for _, ct := range containersResp.Containers {
			containerStates[ct.Id] = ct.State
		}
	}

	type AppWithSyncedState struct {
		Id           string          `json:"id"`
		Name         string          `json:"name"`
		Version      string          `json:"version"`
		State        string          `json:"state"`
		ContainerId  string          `json:"container_id,omitempty"`
		Pid          int32           `json:"pid,omitempty"`
		InstalledAt  int64           `json:"installed_at"`
		StartedAt    int64           `json:"started_at"`
		StoppedAt    int64           `json:"stopped_at"`
		ManifestPath string          `json:"manifest_path"`
		InstancePath string          `json:"instance_path"`
		Permissions  *AppPermissions `json:"permissions,omitempty"`
		WebURL       string          `json:"web_url,omitempty"`
	}

	apps := make([]AppWithSyncedState, 0, len(resp.Apps))
	for _, app := range resp.Apps {
		syncedApp := AppWithSyncedState{
			Id:           app.Id,
			Name:         app.Name,
			Version:      app.Version,
			State:        app.State,
			ContainerId:  app.ContainerId,
			Pid:          app.Pid,
			InstalledAt:  app.InstalledAt,
			StartedAt:    app.StartedAt,
			StoppedAt:    app.StoppedAt,
			ManifestPath: app.ManifestPath,
			InstancePath: app.InstancePath,
			Permissions:  readAppPermissions(app.ManifestPath),
			WebURL:       app.WebUrl,
		}

		// Check container actual state
		containerID := "aipc-" + app.Id
		if actualState, ok := containerStates[containerID]; ok {
			if app.State == "running" && actualState != "running" {
				syncedApp.State = "stopped"
			}
		} else if app.State == "running" {
			// Container does not exist but app shows as running
			syncedApp.State = "stopped"
		}

		apps = append(apps, syncedApp)
	}

	Resp(c).OK(apps)
}

func (h *APIHandlers) GetApp(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	appID := c.Param("app_id")
	if appID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "App ID is required")
		return
	}

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GetApp(ctx, &apppb.GetAppRequest{
		AppId: appID,
	})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) GetAppStats(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	appID := c.Param("app_id")
	if appID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "App ID is required")
		return
	}

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GetAppStats(ctx, &apppb.GetAppRequest{
		AppId: appID,
	})
	if err != nil {
		if strings.Contains(err.Error(), "not found") {
			Resp(c).FailMsg(CodeAppNotFound, err.Error())
		} else {
			Resp(c).FailMsg(CodeServiceError, err.Error())
		}
		return
	}

	// Get container statistics data
	containerID := "aipc-" + appID
	stats, _ := client.GetContainerStats(ctx, &apppb.GetContainerRequest{Id: containerID})

	result := gin.H{
		"app_id":         resp.GetAppId(),
		"uptime_seconds": resp.GetUptimeSeconds(),
	}

	if stats != nil {
		result["cpu_usage_percent"] = stats.CpuPercent
		result["memory_usage_bytes"] = stats.MemoryUsage
		result["memory_limit_bytes"] = stats.MemoryLimit
		result["memory_percent"] = stats.MemoryPercent
	}

	Resp(c).OK(result)
}

func (h *APIHandlers) GetAppLogs(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	appID := c.Param("app_id")
	if appID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "App ID is required")
		return
	}

	// Parse query parameters
	maxLines := int32(100)
	if maxLinesStr := c.Query("max_lines"); maxLinesStr != "" {
		if parsed, err := strconv.ParseInt(maxLinesStr, 10, 32); err == nil {
			maxLines = int32(parsed)
		}
	}

	follow := c.Query("follow") == "true"

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	stream, err := client.GetAppLogs(ctx, &apppb.GetLogsRequest{
		AppId:    appID,
		MaxLines: maxLines,
		Follow:   follow,
	})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	// Set up streaming response
	c.Writer.Header().Set("Content-Type", "application/json")
	c.Writer.Header().Set("Transfer-Encoding", "chunked")
	c.Writer.WriteHeader(http.StatusOK)

	// Stream logs
	encoder := json.NewEncoder(c.Writer)
	for {
		logLine, err := stream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			logger.Error("Error receiving log line: %v", err)
			break
		}

		if err := encoder.Encode(logLine); err != nil {
			logger.Error("Error encoding log line: %v", err)
			break
		}

		c.Writer.Flush()
	}
}

func (h *APIHandlers) StartApp(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	appID := c.Param("app_id")
	if appID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "App ID is required")
		return
	}

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.StartApp(ctx, &apppb.StartRequest{
		AppId: appID,
	})
	if err != nil {
		if h.eventLogger != nil {
			h.eventLogger.LogWithCodeAsync(
				"app.crashed",
				events.MessageParams{"app_id": appID, "error": err.Error()},
				getUsernameFromContext(c),
			)
		}
		Resp(c).FailMsg(CodeAppStartFailed, err.Error())
		return
	}

	if !resp.Success {
		if h.eventLogger != nil {
			h.eventLogger.LogWithCodeAsync(
				"app.crashed",
				events.MessageParams{"app_id": appID, "reason": resp.Message},
				getUsernameFromContext(c),
			)
		}
		Resp(c).FailMsg(CodeAppStartFailed, resp.Message)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			string(events.EventAppStarted),
			events.MessageParams{"app_id": appID},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{"message": resp.Message})
}

func (h *APIHandlers) StopApp(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	appID := c.Param("app_id")
	if appID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "App ID is required")
		return
	}

	// Parse timeout from query or body
	timeoutSeconds := int32(30)
	if timeoutStr := c.Query("timeout"); timeoutStr != "" {
		if parsed, err := strconv.ParseInt(timeoutStr, 10, 32); err == nil {
			timeoutSeconds = int32(parsed)
		}
	}

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(timeoutSeconds+5)*time.Second)
	defer cancel()

	resp, err := client.StopApp(ctx, &apppb.StopRequest{
		AppId:          appID,
		TimeoutSeconds: timeoutSeconds,
	})
	if err != nil {
		Resp(c).FailMsg(CodeAppStopFailed, err.Error())
		return
	}

	if !resp.Success {
		Resp(c).FailMsg(CodeAppStopFailed, resp.Message)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			string(events.EventAppStopped),
			events.MessageParams{"app_id": appID, "reason": resp.Message},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{"message": resp.Message})
}

func (h *APIHandlers) RestartApp(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	appID := c.Param("app_id")
	if appID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "App ID is required")
		return
	}

	// Parse timeout from query
	timeoutSeconds := int32(30)
	if timeoutStr := c.Query("timeout"); timeoutStr != "" {
		if parsed, err := strconv.ParseInt(timeoutStr, 10, 32); err == nil {
			timeoutSeconds = int32(parsed)
		}
	}

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(timeoutSeconds+15)*time.Second)
	defer cancel()

	// Stop the app
	stopResp, err := client.StopApp(ctx, &apppb.StopRequest{
		AppId:          appID,
		TimeoutSeconds: timeoutSeconds,
	})
	if err != nil {
		Resp(c).FailMsg(CodeAppStopFailed, "Stop failed: "+err.Error())
		return
	}
	if !stopResp.Success {
		Resp(c).FailMsg(CodeAppStopFailed, stopResp.Message)
		return
	}

	// Start the app
	startResp, err := client.StartApp(ctx, &apppb.StartRequest{
		AppId: appID,
	})
	if err != nil {
		Resp(c).FailMsg(CodeAppStartFailed, "Start failed: "+err.Error())
		return
	}
	if !startResp.Success {
		Resp(c).FailMsg(CodeAppStartFailed, startResp.Message)
		return
	}

	Resp(c).OK(gin.H{"message": "App restarted successfully"})
}

func (h *APIHandlers) InstallApp(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	var req struct {
		ManifestPath string `json:"manifest_path"`
		ImagePath    string `json:"image_path"`
		Force        bool   `json:"force"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	if req.ManifestPath == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "manifest_path is required")
		return
	}

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()

	resp, err := client.InstallApp(ctx, &apppb.InstallRequest{
		ManifestPath: req.ManifestPath,
		ImagePath:    req.ImagePath,
		Force:        req.Force,
	})
	if err != nil {
		Resp(c).FailMsg(CodeAppInstallFailed, err.Error())
		return
	}

	if !resp.Status.Success {
		Resp(c).FailMsg(CodeAppInstallFailed, resp.Status.Message)
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			string(events.EventAppInstalled),
			events.MessageParams{
				"app_id":        resp.AppId,
				"manifest_path": req.ManifestPath,
			},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{"message": resp.Status.Message, "app_id": resp.AppId, "updated": resp.Updated})
}

func (h *APIHandlers) UninstallApp(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	appID := c.Param("app_id")
	if appID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "App ID is required")
		return
	}

	// Parse keep_logs from query or body
	keepLogs := false
	if keepLogsStr := c.Query("keep_logs"); keepLogsStr == "true" {
		keepLogs = true
	}

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	resp, err := client.UninstallApp(ctx, &apppb.UninstallRequest{
		AppId:    appID,
		KeepLogs: keepLogs,
	})
	if err != nil {
		Resp(c).FailMsg(CodeAppStopFailed, err.Error())
		return
	}

	if !resp.Success {
		Resp(c).FailMsg(CodeAppStopFailed, resp.Message)
		return
	}

	// Clean up AI models owned by this app
	if h.aiModelRepo != nil {
		affected, err := h.aiModelRepo.DeleteByOwnerAppID(appID)
		if err != nil {
			log.Printf("Failed to clean up models for app %s: %v", appID, err)
		} else if affected > 0 {
			log.Printf("Cleaned up %d models for app %s", affected, appID)
		}
	}
	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			string(events.EventAppUninstalled),
			events.MessageParams{"app_id": appID},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{"message": resp.Message})
}

// GetAppPermissions returns the permissions for a specific app by reading its manifest.
func (h *APIHandlers) GetAppPermissions(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	appID := c.Param("app_id")
	if appID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "App ID is required")
		return
	}

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	appInfo, err := client.GetApp(ctx, &apppb.GetAppRequest{AppId: appID})
	if err != nil {
		Resp(c).FailMsg(CodeAppNotFound, err.Error())
		return
	}

	perms := readAppPermissions(appInfo.ManifestPath)
	if perms == nil {
		Resp(c).OK(gin.H{})
		return
	}

	Resp(c).OK(perms)
}

// GetInstallProgress returns the progress of an async install task
func (h *APIHandlers) GetInstallProgress(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	taskID := c.Param("task_id")
	if taskID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Task ID is required")
		return
	}

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.GetInstallProgress(ctx, &apppb.InstallProgressRequest{
		TaskId: taskID,
	})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(gin.H{
		"task_id": resp.TaskId,
		"phase":   resp.Phase,
		"percent": resp.Percent,
		"message": resp.Message,
		"app_id":  resp.AppId,
		"error":   resp.Error,
	})
}

// UploadImage handles container image upload
// POST /api/v1/apps/upload-image
func (h *APIHandlers) UploadImage(c *gin.Context) {
	// Parse multipart form (max 2GB)
	if err := c.Request.ParseMultipartForm(2 << 30); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Failed to parse form: "+err.Error())
		return
	}

	file, header, err := c.Request.FormFile("file")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "No file uploaded: "+err.Error())
		return
	}
	defer file.Close()

	// Validate file extension
	filename := header.Filename
	if !strings.HasSuffix(filename, ".tar") && !strings.HasSuffix(filename, ".tar.gz") && !strings.HasSuffix(filename, ".tgz") {
		Resp(c).FailMsg(CodeInvalidRequest, "Only .tar, .tar.gz or .tgz files are allowed")
		return
	}

	// Create upload directory
	uploadDir := constants.RootPath() + "/images"
	if err := os.MkdirAll(uploadDir, 0755); err != nil {
		Resp(c).FailMsg(CodeFileUploadFailed, "Failed to create upload directory: "+err.Error())
		return
	}

	// Generate unique filename
	timestamp := time.Now().Unix()
	savedName := fmt.Sprintf("%d_%s", timestamp, filename)
	savedPath := filepath.Join(uploadDir, savedName)

	// Save file
	dst, err := os.Create(savedPath)
	if err != nil {
		Resp(c).FailMsg(CodeFileUploadFailed, "Failed to create file: "+err.Error())
		return
	}
	defer dst.Close()

	written, err := io.Copy(dst, file)
	if err != nil {
		os.Remove(savedPath)
		Resp(c).FailMsg(CodeFileUploadFailed, "Failed to save file: "+err.Error())
		return
	}

	logger.Info("Image uploaded: %s (%d bytes)", savedPath, written)

	// Extract image name from tar manifest.json
	imageName := utils.ExtractImageNameFromTar(savedPath)
	if imageName != "" {
		logger.Info("Extracted image name from tar: %s", imageName)
	}

	Resp(c).OK(gin.H{
		"path":     savedPath,
		"image":    imageName,
		"filename": filename,
		"size":     written,
	})
}

// UploadManifest handles app.yaml manifest file upload
// POST /api/v1/apps/upload-manifest
func (h *APIHandlers) UploadManifest(c *gin.Context) {
	if err := c.Request.ParseMultipartForm(32 << 20); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Failed to parse form: "+err.Error())
		return
	}

	file, header, err := c.Request.FormFile("file")
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "No file uploaded: "+err.Error())
		return
	}
	defer file.Close()

	filename := header.Filename
	if !strings.HasSuffix(filename, ".yaml") && !strings.HasSuffix(filename, ".yml") {
		Resp(c).FailMsg(CodeInvalidRequest, "Only .yaml or .yml files are allowed")
		return
	}

	data, err := io.ReadAll(file)
	if err != nil {
		Resp(c).FailMsg(CodeFileUploadFailed, "Failed to read file: "+err.Error())
		return
	}

	// Parse YAML to extract metadata.id
	var manifest struct {
		Metadata struct {
			ID          string `yaml:"id"`
			Name        string `yaml:"name"`
			Version     string `yaml:"version"`
			Description string `yaml:"description"`
		} `yaml:"metadata"`
	}
	if err := yaml.Unmarshal(data, &manifest); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid YAML format: "+err.Error())
		return
	}
	if manifest.Metadata.ID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "manifest metadata.id is required")
		return
	}

	// Save to manifests directory
	manifestDir := fmt.Sprintf(constants.RootPath()+"/apps/manifests/%s", manifest.Metadata.ID)
	if err := os.MkdirAll(manifestDir, 0755); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create manifest directory: "+err.Error())
		return
	}

	manifestPath := filepath.Join(manifestDir, "app.yaml")
	if err := os.WriteFile(manifestPath, data, 0644); err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to save manifest: "+err.Error())
		return
	}

	logger.Info("Manifest uploaded: %s (app_id=%s)", manifestPath, manifest.Metadata.ID)

	Resp(c).OK(gin.H{
		"path": manifestPath,
		"metadata": gin.H{
			"id":          manifest.Metadata.ID,
			"name":        manifest.Metadata.Name,
			"version":     manifest.Metadata.Version,
			"description": manifest.Metadata.Description,
		},
	})
}

// InstallPackage handles async app installation from a pre-made manifest + optional image
// POST /api/v1/apps/install-package
func (h *APIHandlers) InstallPackage(c *gin.Context) {
	if h.grpcClients.AppManager == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}

	var req struct {
		ManifestPath string `json:"manifest_path"`
		ImagePath    string `json:"image_path,omitempty"`
		Force        bool   `json:"force"`
	}
	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}
	if req.ManifestPath == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "manifest_path is required")
		return
	}

	// Verify manifest exists
	if _, err := os.Stat(req.ManifestPath); os.IsNotExist(err) {
		Resp(c).FailMsg(CodeInvalidRequest, "Manifest file not found: "+req.ManifestPath)
		return
	}

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.AsyncInstallApp(ctx, &apppb.AsyncInstallRequest{
		ManifestPath: req.ManifestPath,
		ImagePath:    req.ImagePath,
		Force:        req.Force,
	})
	if err != nil {
		Resp(c).FailMsg(CodeAppInstallFailed, err.Error())
		return
	}

	if h.eventLogger != nil {
		h.eventLogger.LogWithCodeAsync(
			string(events.EventAppInstalled),
			events.MessageParams{
				"app_id":  resp.TaskId,
				"version": "",
			},
			getUsernameFromContext(c),
		)
	}

	Resp(c).OK(gin.H{"task_id": resp.TaskId})
}

// extractImageNameFromTar moved to shared package aipc/platform/common/utils
// (utils.ExtractImageNameFromTar) so app-manager can reuse it during install
// to reconcile the tar's RepoTag against manifest.image.
