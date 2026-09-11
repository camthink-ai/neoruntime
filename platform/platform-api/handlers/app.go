package handlers

import (
	"aipc/platform/common/utils"
	"archive/tar"
	"compress/gzip"
	"context"
	cryptorand "crypto/rand"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"log"
	"net/http"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"syscall"
	"time"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/status"
	"google.golang.org/protobuf/types/known/emptypb"

	"aipc/platform/app-manager/manifest"
	apppb "aipc/platform/app-manager/proto"
	"aipc/platform/common/events"
	"aipc/platform/common/logger"
)

// safeAppIDRE bounds app IDs the moment they become directory names under
// the managed manifests root: manifest.Validate only rejects empty IDs, so
// without this a metadata.id like "../../x" would make MkdirAll/WriteFile
// act outside the root. Mirrors app-manager's safeAppIDPattern
// (manifest_guard.go) so upload accepts exactly what install will.
var safeAppIDRE = regexp.MustCompile(`^[A-Za-z0-9][A-Za-z0-9._-]*$`)

// requireSafeAppID rejects a manifest app ID that cannot serve as a single
// path segment.
func requireSafeAppID(id string) error {
	if !safeAppIDRE.MatchString(id) {
		return fmt.Errorf("app id %q is not usable as a manifest directory name (letters, digits, '.', '_', '-' only; must start with a letter or digit)", id)
	}
	return nil
}

// uploadToken makes upload artifact names unique across concurrent requests.
// A second-granularity timestamp alone collides (same second, same generated
// name), and the later os.Create then truncates the other upload's
// half-written file — one install can end up with another package's image.
func uploadToken() string {
	b := make([]byte, 4)
	if _, err := cryptorand.Read(b); err != nil {
		// crypto/rand failing is exotic; nanos still de-conflict in practice.
		return fmt.Sprintf("%d_00000000", time.Now().UnixNano())
	}
	return fmt.Sprintf("%d_%x", time.Now().Unix(), b)
}

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
// Uses the shared manifest.ParseManifest so spec.models ids merged into
// permissions.inference.models show up here too (single source of truth).
func readAppPermissions(manifestPath string) *AppPermissions {
	if manifestPath == "" {
		return nil
	}
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		return nil
	}
	appManifest, err := manifest.ParseManifest(data)
	if err != nil {
		logger.Warn("Failed to parse manifest %s for permissions: %v", manifestPath, err)
		return nil
	}

	p := appManifest.Spec.Permissions
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
		if resp.Code == 404 {
			Resp(c).FailMsg(CodeAppNotFound, resp.Message)
			return
		}
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
		if resp.Code == 404 {
			Resp(c).FailMsg(CodeAppNotFound, resp.Message)
			return
		}
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
		if stopResp.Code == 404 {
			Resp(c).FailMsg(CodeAppNotFound, stopResp.Message)
			return
		}
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
		if startResp.Code == 404 {
			Resp(c).FailMsg(CodeAppNotFound, startResp.Message)
			return
		}
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
		if resp.Code == 404 {
			Resp(c).FailMsg(CodeAppNotFound, resp.Message)
			return
		}
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
		if status.Code(err) == codes.NotFound {
			Resp(c).FailMsg(CodeNotFound, status.Convert(err).Message())
			return
		}
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

// maxImageUploadBytes caps image tar uploads at 2GB (kept in sync with the
// wizard's frontend limit). A var so tests can shrink it.
var maxImageUploadBytes int64 = 2 << 30

// uploadDiskHeadroomBytes is the extra free space required beyond the file
// size before accepting an image upload, so an install can still unpack.
const uploadDiskHeadroomBytes int64 = 1 << 30

// UploadImage handles container image upload
// POST /api/v1/apps/upload-image
func (h *APIHandlers) UploadImage(c *gin.Context) {
	// Hard cap the request body so oversized uploads are cut off early
	// instead of streaming to disk. ParseMultipartForm's argument is only
	// an in-memory threshold, NOT a size limit.
	c.Request.Body = http.MaxBytesReader(c.Writer, c.Request.Body, maxImageUploadBytes)
	if err := c.Request.ParseMultipartForm(32 << 20); err != nil {
		var maxErr *http.MaxBytesError
		if errors.As(err, &maxErr) {
			Resp(c).FailMsg(CodeInvalidRequest, fmt.Sprintf(
				"Image exceeds the maximum allowed size (%d bytes)", maxImageUploadBytes))
			return
		}
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
	filename := filepath.Base(header.Filename)
	if !strings.HasSuffix(filename, ".tar") && !strings.HasSuffix(filename, ".tar.gz") && !strings.HasSuffix(filename, ".tgz") {
		Resp(c).FailMsg(CodeInvalidRequest, "Only .tar, .tar.gz or .tgz files are allowed")
		return
	}

	// Keep every request's artifacts together in a private staging directory.
	uploadDir, err := newAppStagingDir()
	if err != nil {
		Resp(c).FailMsg(CodeFileUploadFailed, "Failed to create staging directory: "+err.Error())
		return
	}
	stagingCommitted := false
	defer func() {
		if !stagingCommitted {
			os.RemoveAll(uploadDir)
		}
	}()

	// Refuse early when the target partition cannot hold the file plus
	// unpack headroom — failing here is far cheaper than failing at install.
	var statfs syscall.Statfs_t
	if err := syscall.Statfs(uploadDir, &statfs); err == nil {
		free := int64(statfs.Bavail) * int64(statfs.Bsize)
		if need := header.Size + uploadDiskHeadroomBytes; free < need {
			Resp(c).FailMsg(CodeStorageFull, fmt.Sprintf(
				"No space left for upload: need %d bytes (with headroom), %d free", need, free))
			return
		}
	}

	// Generate unique filename
	savedName := fmt.Sprintf("%s_%s", uploadToken(), filename)

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
	if written > maxImageUploadBytes {
		os.Remove(savedPath)
		Resp(c).FailMsg(CodeInvalidRequest, fmt.Sprintf(
			"Image exceeds the maximum allowed size (%d bytes)", maxImageUploadBytes))
		return
	}

	// Structural pre-check: reject anything the containerd importer would
	// only fail on at install time (wrong layout, digest-style layer refs,
	// truncated archive). Fail fast here, not after the user waits through
	// the whole install.
	if err := utils.ValidateDockerSaveTar(savedPath); err != nil {
		os.Remove(savedPath)
		logger.Warn("Rejected invalid image tar %s: %v", savedPath, err)
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid image tar: "+err.Error())
		return
	}

	logger.Info("Image uploaded: %s (%d bytes)", savedPath, written)
	stagingCommitted = true

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

	// Parse via the shared parser: full validation + spec.models merge.
	appManifest, err := manifest.ParseManifest(data)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid manifest: "+err.Error())
		return
	}
	if appManifest.Metadata.ID == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "manifest metadata.id is required")
		return
	}
	// Before the ID becomes a directory name — the install-time guard in
	// app-manager runs too late to prevent the out-of-root write here.
	if err := requireSafeAppID(appManifest.Metadata.ID); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, err.Error())
		return
	}

	// Save the ORIGINAL bytes untouched in request-private staging. The live
	// canonical manifest is published only by app-manager after install checks.
	manifestDir, err := newAppStagingDir()
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to create staging directory: "+err.Error())
		return
	}
	manifestPath := filepath.Join(manifestDir, "app.yaml")
	if err := os.WriteFile(manifestPath, data, 0644); err != nil {
		os.RemoveAll(manifestDir)
		Resp(c).FailMsg(CodeServiceError, "Failed to save manifest: "+err.Error())
		return
	}

	logger.Info("Manifest uploaded: %s (app_id=%s)", manifestPath, appManifest.Metadata.ID)

	Resp(c).OK(gin.H{
		"path": manifestPath,
		"metadata": gin.H{
			"id":          appManifest.Metadata.ID,
			"name":        appManifest.Metadata.Name,
			"version":     appManifest.Metadata.Version,
			"description": appManifest.Metadata.Description,
		},
		// Full parsed manifest (json-tagged struct): the wizard hydrates from
		// this without any client-side YAML parsing. Spec.Models ids are
		// already merged into permissions.inference.models.
		"manifest": appManifest,
		// The wizard cannot express spec.containers; the web layer needs to
		// know before offering editable install for this file.
		"multi_container": appManifest.IsMultiContainer(),
	})
}

// maxPackageManifestBytes caps the app.yaml entry extracted from a package at
// 4MB — far beyond any real manifest, small enough to stop tar bombs.
const maxPackageManifestBytes int64 = 4 << 20

// UploadPackage handles single-file .neoapp app-package upload: a tar.gz bundle
// holding app.yaml + image.tar (plus optional extras like SHA256SUMS). It
// unpacks both entries server-side — the manifest goes through the same
// parse-and-store path as upload-manifest, the image tar through the same
// validation as upload-image — so the web import dialog can accept one file
// and then call install-package with the returned paths.
// POST /api/v1/apps/upload-package
func (h *APIHandlers) UploadPackage(c *gin.Context) {
	// Hard cap the request body like upload-image: the package holds the
	// same image tar, just gzip-wrapped.
	c.Request.Body = http.MaxBytesReader(c.Writer, c.Request.Body, maxImageUploadBytes)
	if err := c.Request.ParseMultipartForm(32 << 20); err != nil {
		var maxErr *http.MaxBytesError
		if errors.As(err, &maxErr) {
			Resp(c).FailMsg(CodeInvalidRequest, fmt.Sprintf(
				"Package exceeds the maximum allowed size (%d bytes)", maxImageUploadBytes))
			return
		}
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
	filename := filepath.Base(header.Filename)
	if !strings.HasSuffix(filename, ".neoapp") && !strings.HasSuffix(filename, ".tar.gz") && !strings.HasSuffix(filename, ".tgz") {
		Resp(c).FailMsg(CodeInvalidRequest,
			"Only .neoapp packages (tar.gz) are allowed; zip packages are not supported — rebuild with the repo build script")
		return
	}

	// Keep package, extracted image and manifest under one request-private
	// staging directory so cancellation/TTL cleanup can remove ownership as a unit.
	uploadDir, err := newAppStagingDir()
	if err != nil {
		Resp(c).FailMsg(CodeFileUploadFailed, "Failed to create staging directory: "+err.Error())
		return
	}
	stagingCommitted := false
	defer func() {
		if !stagingCommitted {
			os.RemoveAll(uploadDir)
		}
	}()

	// Refuse early when the target partition cannot hold the package plus
	// the unpacked image plus headroom (worst case: package + 2× image).
	var statfs syscall.Statfs_t
	if err := syscall.Statfs(uploadDir, &statfs); err == nil {
		free := int64(statfs.Bavail) * int64(statfs.Bsize)
		if need := header.Size + uploadDiskHeadroomBytes; free < need {
			Resp(c).FailMsg(CodeStorageFull, fmt.Sprintf(
				"No space left for upload: need %d bytes (with headroom), %d free", need, free))
			return
		}
	}

	token := uploadToken()
	pkgPath := filepath.Join(uploadDir, fmt.Sprintf("%s_pkg_%s", token, filename))
	imageTarPath := filepath.Join(uploadDir, fmt.Sprintf("%s_image.tar", token))

	// cleanup removes every partial artifact on failure paths; success
	// removes only the package (the extracted image tar is the payload).
	cleanup := func(keepImage bool) {
		os.Remove(pkgPath)
		if !keepImage {
			os.Remove(imageTarPath)
		}
	}

	// Save the uploaded package to a temp file first: tar.Reader needs a
	// seekable-ish stream and we want the original off disk once unpacked.
	pkgDst, err := os.Create(pkgPath)
	if err != nil {
		Resp(c).FailMsg(CodeFileUploadFailed, "Failed to create file: "+err.Error())
		return
	}
	written, copyErr := io.Copy(pkgDst, file)
	closeErr := pkgDst.Close()
	if copyErr != nil || closeErr != nil {
		cleanup(false)
		Resp(c).FailMsg(CodeFileUploadFailed, "Failed to save package")
		return
	}
	if written > maxImageUploadBytes {
		cleanup(false)
		Resp(c).FailMsg(CodeInvalidRequest, fmt.Sprintf(
			"Package exceeds the maximum allowed size (%d bytes)", maxImageUploadBytes))
		return
	}

	// Unpack: iterate tar.gz entries, match by basename. Entries land under
	// self-constructed paths, so hostile entry names (../x) are inert.
	fail := func(msg string) {
		cleanup(false)
		Resp(c).FailMsg(CodeInvalidRequest, msg)
	}

	pkgFile, err := os.Open(pkgPath)
	if err != nil {
		cleanup(false)
		Resp(c).FailMsg(CodeFileUploadFailed, "Failed to reopen package: "+err.Error())
		return
	}
	defer pkgFile.Close()

	gz, err := gzip.NewReader(pkgFile)
	if err != nil {
		fail("Invalid .neoapp package: not a gzip/tar.gz archive")
		return
	}
	defer gz.Close()

	var manifestData []byte
	manifestFound := false
	imageFound := false
	tr := tar.NewReader(gz)
	for {
		hdr, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			fail("Invalid .neoapp package: failed to read archive: " + err.Error())
			return
		}
		if hdr.Typeflag != tar.TypeReg {
			continue // directories, symlinks and friends are skipped
		}
		switch filepath.Base(hdr.Name) {
		case "app.yaml":
			if manifestFound {
				continue
			}
			data, err := io.ReadAll(io.LimitReader(tr, maxPackageManifestBytes+1))
			if err != nil {
				fail("Invalid .neoapp package: failed to read app.yaml: " + err.Error())
				return
			}
			if int64(len(data)) > maxPackageManifestBytes {
				fail("Invalid .neoapp package: app.yaml exceeds 4MB")
				return
			}
			manifestData = data
			manifestFound = true
		case "image.tar":
			if imageFound {
				continue
			}
			imageDst, err := os.Create(imageTarPath)
			if err != nil {
				fail("Failed to extract image.tar: " + err.Error())
				return
			}
			n, copyErr := io.Copy(imageDst, io.LimitReader(tr, maxImageUploadBytes+1))
			closeErr := imageDst.Close()
			if copyErr != nil || closeErr != nil {
				fail("Failed to extract image.tar")
				return
			}
			if n > maxImageUploadBytes {
				fail(fmt.Sprintf("Invalid .neoapp package: image.tar exceeds the maximum allowed size (%d bytes)", maxImageUploadBytes))
				return
			}
			imageFound = true
		default:
			// SHA256SUMS / README / companion YAMLs: not needed for install
		}
	}
	if !manifestFound || !imageFound {
		fail("Invalid .neoapp package: both app.yaml and image.tar are required")
		return
	}

	// Same structural pre-check as upload-image: fail fast here, not after
	// the user waits through the whole install.
	if err := utils.ValidateDockerSaveTar(imageTarPath); err != nil {
		logger.Warn("Rejected invalid image tar in %s: %v", pkgPath, err)
		fail("Invalid image tar inside package: " + err.Error())
		return
	}

	// Parse via the shared parser (same as upload-manifest): full
	// validation + spec.models merge.
	appManifest, err := manifest.ParseManifest(manifestData)
	if err != nil {
		fail("Invalid manifest: " + err.Error())
		return
	}
	if appManifest.Metadata.ID == "" {
		fail("manifest metadata.id is required")
		return
	}
	// Before the ID becomes a directory name — the install-time guard in
	// app-manager runs too late to prevent the out-of-root write here.
	if err := requireSafeAppID(appManifest.Metadata.ID); err != nil {
		fail(err.Error())
		return
	}

	// Save the ORIGINAL manifest bytes untouched beside the extracted image.
	manifestPath := filepath.Join(uploadDir, "app.yaml")
	if err := os.WriteFile(manifestPath, manifestData, 0644); err != nil {
		cleanup(false)
		Resp(c).FailMsg(CodeServiceError, "Failed to save manifest: "+err.Error())
		return
	}

	// Success: the package file itself is no longer needed; staging ownership
	// remains with the caller until install begins or it is abandoned.
	cleanup(true)
	stagingCommitted = true

	imageName := utils.ExtractImageNameFromTar(imageTarPath)
	logger.Info("Package uploaded: %s (app_id=%s, image=%s, %d bytes)",
		filename, appManifest.Metadata.ID, imageTarPath, written)

	Resp(c).OK(gin.H{
		// Manifest fields mirror upload-manifest so the wizard hydrates the
		// same way; image fields mirror upload-image.
		"path": manifestPath,
		"metadata": gin.H{
			"id":          appManifest.Metadata.ID,
			"name":        appManifest.Metadata.Name,
			"version":     appManifest.Metadata.Version,
			"description": appManifest.Metadata.Description,
		},
		"manifest":        appManifest,
		"multi_container": appManifest.IsMultiContainer(),
		// Original manifest text so the web YAML editor gets the same
		// byte-faithful baseline a direct app.yaml upload provides.
		"manifest_yaml": string(manifestData),
		"image":         imageName,
		"image_path":    imageTarPath,
		"filename":      filename,
		"size":          written,
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

	// The web package endpoint consumes only request-owned staging. CLI and
	// other external-path compatibility remains at app-manager's gRPC surface.
	manifestPath, manifestDir, err := safeStagingArtifact(req.ManifestPath)
	if err != nil || filepath.Base(manifestPath) != "app.yaml" {
		Resp(c).FailMsg(CodeInvalidParameter, "manifest_path must be a staged app.yaml")
		return
	}
	if _, err := safeStagingManifest(manifestPath); err != nil {
		Resp(c).FailMsg(CodeInvalidParameter, err.Error())
		return
	}
	var imageDir string
	if req.ImagePath != "" {
		imagePath, dir, err := safeStagingArtifact(req.ImagePath)
		if err != nil {
			Resp(c).FailMsg(CodeInvalidParameter, "image_path must be a staged upload artifact")
			return
		}
		imageDir = dir
		req.ImagePath = imagePath
	}
	req.ManifestPath = manifestPath

	client := apppb.NewAppManagerClient(h.grpcClients.AppManager)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.AsyncInstallApp(ctx, &apppb.AsyncInstallRequest{
		ManifestPath: req.ManifestPath,
		ImagePath:    req.ImagePath,
		Force:        req.Force,
	})
	if err != nil {
		// Ownership has not transferred because no task was accepted.
		os.RemoveAll(manifestDir)
		if imageDir != "" && imageDir != manifestDir {
			os.RemoveAll(imageDir)
		}
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
