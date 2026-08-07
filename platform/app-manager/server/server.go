package server

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/containerd/containerd/namespaces"
	"github.com/glebarez/sqlite"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/protobuf/types/known/emptypb"
	"gorm.io/gorm"

	inferencepb "aipc/platform/ai-runtime/proto"
	"aipc/platform/app-manager/containerd"
	"aipc/platform/app-manager/manifest"
	"aipc/platform/app-manager/monitor"
	"aipc/platform/app-manager/plugin"
	"aipc/platform/app-manager/proto"
	"aipc/platform/app-manager/registry"
	"aipc/platform/app-manager/security"
	"aipc/platform/common/constants"
	"aipc/platform/common/logger"
	"aipc/platform/common/socket"
	"aipc/platform/common/utils"
	eventpb "aipc/platform/event-bus/proto"
)

type AppManagerServer struct {
	proto.UnimplementedAppManagerServer
	registry    *registry.Registry
	config      *Config
	client      *containerd.Client          // containerd client wrapper
	runtime     *containerd.Runtime         // container runtime wrapper
	autoRestart *monitor.AutoRestartManager // auto-restart manager
	db          *gorm.DB                    // direct sqlite reader for platform data

	// Plugin system
	pluginRegistry *registry.PluginRegistry
	discovery      *plugin.DiscoveryManager
	resolver       *plugin.DependencyResolver

	// AI Runtime client
	aiRuntimeClient inferencepb.InferenceServiceClient
	aiRuntimeConn   *grpc.ClientConn
	aiRuntimeMutex  sync.RWMutex

	// Event Bus client
	eventBusClient eventpb.EventBusClient
	eventBusConn   *grpc.ClientConn
	eventBusMutex  sync.RWMutex

	// Multi-container instances (app_id -> instance)
	multiContainerInstances map[string]*containerd.MultiContainerInstance
	multiContainerMutex     sync.RWMutex

	// Async install tasks
	taskStore *InstallTaskStore
}

type Config struct {
	Containerd struct {
		Address   string
		Namespace string
	}
	Apps struct {
		RegistryPath  string
		InstancesPath string
		ManifestsPath string
	}
	Security struct {
		SeccompProfile string
	}
	AIRuntime struct {
		Enabled                 bool
		Endpoint                string
		AutoRegisterPermissions bool
	}
	EventBus struct {
		Enabled       bool
		Endpoint      string
		PublishEvents []string
	}
}

// stoppedAtUnix returns 0 for Go's zero time, otherwise the Unix timestamp.
func stoppedAtUnix(t time.Time) int64 {
	if t.IsZero() {
		return 0
	}
	return t.Unix()
}

func (s *AppManagerServer) withNamespace(ctx context.Context) context.Context {
	return namespaces.WithNamespace(ctx, s.config.Containerd.Namespace)
}

func NewAppManagerServer(cfg *Config) (*AppManagerServer, error) {
	// Create registry
	reg, err := registry.NewRegistry(cfg.Apps.RegistryPath)
	if err != nil {
		return nil, fmt.Errorf("failed to create registry: %w", err)
	}

	// Create containerd client wrapper
	var containerdClientWrapper *containerd.Client
	var runtime *containerd.Runtime
	if cfg.Containerd.Address != "" {
		if cfg.Containerd.Namespace == "" {
			cfg.Containerd.Namespace = "aipc"
			logger.Info("No namespace specified, defaulting to 'aipc'")
		}
		clientWrapper, err := containerd.NewClient(cfg.Containerd.Address, cfg.Containerd.Namespace)
		if err != nil {
			// Log warning but don't fail - containerd may not be available in all environments
			logger.Warn("Failed to connect to containerd: %v (container operations will be limited)", err)
		} else {
			containerdClientWrapper = clientWrapper
			// Create runtime wrapper
			runtime = containerd.NewRuntime(clientWrapper, cfg.Apps.InstancesPath)
		}
	}

	// Create auto-restart manager if containerd is available
	var autoRestart *monitor.AutoRestartManager
	if containerdClientWrapper != nil && runtime != nil {
		autoRestart = monitor.NewAutoRestartManager(containerdClientWrapper, runtime, reg, cfg.Containerd.Namespace)
		// Start monitoring in background
		ctx := context.Background()
		autoRestart.Start(ctx)
	}

	// Initialize plugin system
	pluginReg := registry.NewPluginRegistry(reg)
	discoveryMgr, err := plugin.NewDiscoveryManager(plugin.DiscoveryDir)
	if err != nil {
		logger.Warn("Failed to create discovery manager: %v (plugin discovery disabled)", err)
	}
	depResolver := plugin.NewDependencyResolver(pluginReg, reg)

	server := &AppManagerServer{
		registry:                reg,
		config:                  cfg,
		client:                  containerdClientWrapper,
		runtime:                 runtime,
		autoRestart:             autoRestart,
		pluginRegistry:          pluginReg,
		discovery:               discoveryMgr,
		resolver:                depResolver,
		multiContainerInstances: make(map[string]*containerd.MultiContainerInstance),
		taskStore:               NewInstallTaskStore(),
	}

	// Create read-only sqlite connection for accessing model paths
	// Registry path and platform.db are under RootPath
	dbPath := filepath.Join(filepath.Dir(filepath.Dir(cfg.Apps.RegistryPath)), "data", "platform.db")
	logger.Info("Opening platform.db for model preloading: %s", dbPath)

	db, err := gorm.Open(sqlite.Open(dbPath+"?mode=ro"), &gorm.Config{})
	if err != nil {
		logger.Warn("Failed to open platform.db: %v (model preloading may fail)", err)
	} else {
		server.db = db
		logger.Info("Successfully opened platform.db in read-only mode")
	}

	// Connect to AI Runtime if enabled
	if cfg.AIRuntime.Enabled && cfg.AIRuntime.Endpoint != "" {
		if err := server.connectToAIRuntime(cfg.AIRuntime.Endpoint); err != nil {
			logger.Warn("Failed to connect to AI Runtime: %v (will continue without permission registration)", err)
		} else {
			logger.Info("Connected to AI Runtime: %s", cfg.AIRuntime.Endpoint)
		}
	}

	// Connect to Event Bus if enabled
	if cfg.EventBus.Enabled && cfg.EventBus.Endpoint != "" {
		if err := server.connectToEventBus(cfg.EventBus.Endpoint); err != nil {
			logger.Warn("Failed to connect to Event Bus: %v (will continue without event publishing)", err)
		} else {
			logger.Info("Connected to Event Bus: %s", cfg.EventBus.Endpoint)
		}
	}

	return server, nil
}

// connectToAIRuntime connects to the AI Runtime service
func (s *AppManagerServer) connectToAIRuntime(endpoint string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	parsedAddr, err := utils.ParseListenAddress(endpoint)
	if err != nil {
		return fmt.Errorf("failed to parse AI runtime address: %w", err)
	}

	conn, err := grpc.DialContext(ctx, "unix://"+parsedAddr,
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return fmt.Errorf("failed to dial AI runtime: %w", err)
	}

	s.aiRuntimeMutex.Lock()
	s.aiRuntimeConn = conn
	s.aiRuntimeClient = inferencepb.NewInferenceServiceClient(conn)
	s.aiRuntimeMutex.Unlock()

	return nil
}

// connectToEventBus connects to the Event Bus service
func (s *AppManagerServer) connectToEventBus(endpoint string) error {
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	parsedAddr, err := utils.ParseListenAddress(endpoint)
	if err != nil {
		return fmt.Errorf("failed to parse event bus address: %w", err)
	}

	conn, err := grpc.DialContext(ctx, "unix://"+parsedAddr,
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		return fmt.Errorf("failed to dial event bus: %w", err)
	}

	s.eventBusMutex.Lock()
	s.eventBusConn = conn
	s.eventBusClient = eventpb.NewEventBusClient(conn)
	s.eventBusMutex.Unlock()

	return nil
}

// registerAppPermissions registers app permissions with AI Runtime
func (s *AppManagerServer) registerAppPermissions(ctx context.Context, appID string, appManifest *manifest.AppManifest) error {
	if !s.config.AIRuntime.AutoRegisterPermissions {
		return nil
	}

	s.aiRuntimeMutex.RLock()
	client := s.aiRuntimeClient
	s.aiRuntimeMutex.RUnlock()

	if client == nil {
		return fmt.Errorf("AI Runtime client not available")
	}

	// Register models that the app is allowed to use
	// Note: This is a simplified implementation. In a real system, you might
	// need to create a session or register permissions differently.
	// For now, we'll just log the models the app can access.
	models := appManifest.Spec.Permissions.Inference.Models
	if len(models) > 0 {
		logger.Info("App %s has access to models: %v", appID, models)
		// TODO: Implement actual permission registration API in AI Runtime
		// This might require a new gRPC method like RegisterAppPermissions
	}

	return nil
}

// publishAppEvent publishes an app lifecycle event to Event Bus
func (s *AppManagerServer) publishAppEvent(eventType string, appID string, data map[string]interface{}) {
	if !s.config.EventBus.Enabled {
		return
	}

	// Check if this event type should be published
	shouldPublish := false
	for _, event := range s.config.EventBus.PublishEvents {
		if event == eventType {
			shouldPublish = true
			break
		}
	}
	if !shouldPublish {
		return
	}

	s.eventBusMutex.RLock()
	client := s.eventBusClient
	s.eventBusMutex.RUnlock()

	if client == nil {
		return
	}

	// Build event data
	eventData := map[string]interface{}{
		"app_id": appID,
		"event":  eventType,
	}
	for k, v := range data {
		eventData[k] = v
	}

	// Serialize to JSON
	payload, err := json.Marshal(eventData)
	if err != nil {
		logger.Warn("Failed to serialize event data for app %s: %v", appID, err)
		return
	}

	// Publish event
	ctx, cancel := context.WithTimeout(context.Background(), constants.EventPublishTimeout)
	defer cancel()

	topic := fmt.Sprintf("app/%s", eventType)
	_, err = client.Publish(ctx, &eventpb.PublishRequest{
		Event: &eventpb.Event{
			Topic:       topic,
			TimestampNs: uint64(time.Now().UnixNano()),
			Source:      "app-manager",
			EventId:     fmt.Sprintf("app-%s-%d", appID, time.Now().UnixNano()),
			Payload:     payload,
			PayloadType: "json",
			Metadata: map[string]string{
				"app_id":     appID,
				"event_type": eventType,
			},
		},
	})

	if err != nil {
		// Event publishing failure is non-critical, but should be logged as warning
		logger.Warn("Failed to publish app event '%s' for app %s to event bus: %v", eventType, appID, err)
	}
}

// stopAndCleanupApp stops a running container and unregisters the app.
// Used by force overwrite install to clean up before re-installing.
func (s *AppManagerServer) stopAndCleanupApp(ctx context.Context, appID string) error {
	// Remove from auto-restart monitoring
	if s.autoRestart != nil {
		s.autoRestart.RemoveApp(appID)
	}

	appInfo, err := s.registry.Get(appID)
	if err != nil {
		return nil // App doesn't exist, nothing to clean up
	}

	// Stop if running
	if appInfo.State == registry.AppStateRunning {
		stopReq := &proto.StopRequest{AppId: appID, TimeoutSeconds: 10}
		if _, err := s.StopApp(ctx, stopReq); err != nil {
			logger.Warn("Failed to stop app %s during cleanup: %v", appID, err)
		}
	}

	// Remove container if exists
	if appInfo.ContainerID != "" && s.client != nil && s.runtime != nil {
		container, err := s.client.GetContainer(ctx, appInfo.ContainerID)
		if err == nil {
			if task, err := container.Task(ctx, nil); err == nil {
				if stopErr := s.runtime.StopAppContainer(ctx, task, 5); stopErr != nil {
					logger.Warn("Failed to stop container: %v", stopErr)
				}
			}
			if err := s.runtime.RemoveAppContainer(ctx, container); err != nil {
				logger.Warn("Failed to remove container: %v", err)
			}
		}
	}

	// Unregister from registry
	if err := s.registry.Unregister(appID); err != nil {
		return fmt.Errorf("failed to unregister app: %w", err)
	}

	return nil
}

// stopAndCleanupContainer stops and removes the container without unregistering the app.
// Used by the update flow (force install) to clean up the running container
// while preserving registry state (WebURL, RestartCount, InstalledAt).
func (s *AppManagerServer) stopAndCleanupContainer(ctx context.Context, appID string) error {
	if s.autoRestart != nil {
		s.autoRestart.RemoveApp(appID)
	}

	appInfo, err := s.registry.Get(appID)
	if err != nil {
		return nil
	}

	if appInfo.State == registry.AppStateRunning {
		stopReq := &proto.StopRequest{AppId: appID, TimeoutSeconds: 10}
		if _, err := s.StopApp(ctx, stopReq); err != nil {
			logger.Warn("Failed to stop app %s during update: %v", appID, err)
		}
	}

	if appInfo.ContainerID != "" && s.client != nil && s.runtime != nil {
		container, err := s.client.GetContainer(ctx, appInfo.ContainerID)
		if err == nil {
			if task, err := container.Task(ctx, nil); err == nil {
				if stopErr := s.runtime.StopAppContainer(ctx, task, 5); stopErr != nil {
					logger.Warn("Failed to stop container during update: %v", stopErr)
				}
			}
			if err := s.runtime.RemoveAppContainer(ctx, container); err != nil {
				logger.Warn("Failed to remove container during update: %v", err)
			}
		}
	}

	return nil
}

// InstallApp implements AppManager.InstallApp
func (s *AppManagerServer) InstallApp(ctx context.Context, req *proto.InstallRequest) (*proto.InstallResponse, error) {
	logger.Info("InstallApp called: manifest=%s, image=%s", req.ManifestPath, req.ImagePath)

	// Parse manifest
	appManifest, err := manifest.LoadManifest(req.ManifestPath)
	if err != nil {
		return &proto.InstallResponse{
			Status: &proto.Status{
				Success: false,
				Message: fmt.Sprintf("Failed to load manifest: %v", err),
				Code:    400,
			},
		}, nil
	}
	if err := appManifest.Validate(); err != nil {
		return &proto.InstallResponse{
			Status: &proto.Status{
				Success: false,
				Message: fmt.Sprintf("Failed to validate manifest: %v", err),
				Code:    400,
			},
		}, nil
	}

	// Check duplicate BEFORE pulling image
	appID := appManifest.Metadata.ID
	var isUpdate bool
	var wasRunning bool
	var oldWebURL string
	var oldRestartCount int
	var oldInstalledAt time.Time

	if s.registry.Exists(appID) {
		if !req.Force {
			return &proto.InstallResponse{
				Status: &proto.Status{
					Success: false,
					Message: fmt.Sprintf("App %s already exists. Use force=true to overwrite.", appID),
					Code:    409,
				},
			}, nil
		}
		// Force overwrite: preserve state then stop + remove container
		existingApp, _ := s.registry.Get(appID)
		wasRunning = existingApp.State == registry.AppStateRunning
		oldWebURL = existingApp.WebURL
		oldRestartCount = existingApp.RestartCount
		oldInstalledAt = existingApp.InstalledAt
		isUpdate = true

		logger.Info("Updating existing app %s (was running=%v)", appID, wasRunning)
		if err := s.stopAndCleanupContainer(ctx, appID); err != nil {
			logger.Warn("Failed to cleanup existing app %s: %v", appID, err)
		}
	}

	// Validate seccomp profile
	if err := security.ValidateSeccompProfile(s.config.Security.SeccompProfile); err != nil {
		return &proto.InstallResponse{
			Status: &proto.Status{
				Success: false,
				Message: fmt.Sprintf("Invalid seccomp profile: %v", err),
				Code:    500,
			},
		}, nil
	}

	// Pull image from remote registry if image path is a URL/reference
	// Import image from local file if image path is a local file path
	if req.ImagePath != "" && s.client != nil {
		imageName := appManifest.GetNormalizedImage()

		// Check if it's a remote image reference (not a local file path)
		// A local file path typically ends with .tar/.tar.gz or starts with / or ./
		isLocalFile := strings.HasSuffix(req.ImagePath, ".tar") ||
			strings.HasSuffix(req.ImagePath, ".tar.gz") ||
			strings.HasSuffix(req.ImagePath, ".tgz") ||
			strings.HasPrefix(req.ImagePath, "/") ||
			strings.HasPrefix(req.ImagePath, "./")
		isRemoteImage := !isLocalFile

		if isRemoteImage {
			// Pull from remote registry
			logger.Info("Pulling image from remote registry: %s", req.ImagePath)
			err := s.client.PullImage(ctx, req.ImagePath)
			if err != nil {
				return &proto.InstallResponse{
					Status: &proto.Status{
						Success: false,
						Message: fmt.Sprintf("Failed to pull image: %v", err),
						Code:    500,
					},
				}, nil
			}
			logger.Info("Image pulled successfully: %s", req.ImagePath)

			// IMPORTANT: After pulling, override the manifest's image to match
			// the normalized reference that containerd stores. This ensures
			// StartApp -> CreateAppContainer looks up the correct image.
			normalizedRef := manifest.NormalizeImageName(req.ImagePath)
			appManifest.Spec.Image = normalizedRef
			logger.Info("Image reference normalized for manifest: %s", normalizedRef)
		} else {
			// Import from local tar file
			logger.Info("Importing image from local file: %s", req.ImagePath)
			importedName, err := s.client.ImportImage(ctx, req.ImagePath, imageName)
			if err != nil {
				logger.Warn("Failed to import image: %v", err)
				return &proto.InstallResponse{
					Status: &proto.Status{
						Success: false,
						Message: fmt.Sprintf("Failed to import image: %v", err),
						Code:    400,
					},
				}, nil
			}
			logger.Info("Image imported successfully: %s", importedName)

			// Save tar file for self-healing recovery after power loss
			if saveErr := containerd.SaveImageTar(appManifest.Metadata.ID, req.ImagePath); saveErr != nil {
				logger.Warn("Failed to save image tar for recovery: %v", saveErr)
			}

			// Clean up uploaded tar file after successful import
			s.cleanupUploadedTar(req.ImagePath)
		}
	}

	// Create app info with plugin fields
	appInfo := &registry.AppInfo{
		ID:           appManifest.Metadata.ID,
		Name:         appManifest.Metadata.Name,
		Version:      appManifest.Metadata.Version,
		Image:        appManifest.GetNormalizedImage(),
		State:        registry.AppStateInstalled,
		ManifestPath: req.ManifestPath,
		InstancePath: fmt.Sprintf("%s/%s", s.config.Apps.InstancesPath, appManifest.Metadata.ID),
		IsPlugin:     appManifest.IsPlugin(),
	}

	// Populate plugin capabilities
	if appManifest.IsPlugin() {
		for _, cap := range appManifest.Spec.Plugin.Capabilities {
			appInfo.Capabilities = append(appInfo.Capabilities, registry.CapabilityInfo{
				ID:        cap.ID,
				Version:   cap.Version,
				Transport: cap.Transport,
			})
		}
	}

	// Populate plugin dependencies
	for _, dep := range appManifest.Spec.PluginDependencies {
		appInfo.Dependencies = append(appInfo.Dependencies, registry.DependencyInfo{
			Capability: dep.Capability,
			MinVersion: dep.MinVersion,
			Required:   dep.Required,
		})
	}

	// Preserve state on update
	if isUpdate {
		appInfo.WebURL = oldWebURL
		appInfo.RestartCount = oldRestartCount
		appInfo.InstalledAt = oldInstalledAt
	}

	// Register or update app
	if isUpdate {
		if err := s.registry.Update(appInfo); err != nil {
			return &proto.InstallResponse{
				Status: &proto.Status{
					Success: false,
					Message: fmt.Sprintf("Failed to update app registry: %v", err),
					Code:    500,
				},
			}, nil
		}
	} else {
		if err := s.registry.Register(appInfo); err != nil {
			return &proto.InstallResponse{
				Status: &proto.Status{
					Success: false,
					Message: fmt.Sprintf("Failed to register app: %v", err),
					Code:    500,
				},
			}, nil
		}
	}

	// Create instance directory
	if err := os.MkdirAll(appInfo.InstancePath, 0755); err != nil {
		return &proto.InstallResponse{
			Status: &proto.Status{
				Success: false,
				Message: fmt.Sprintf("Failed to create instance directory: %v", err),
				Code:    500,
			},
		}, nil
	}

	// Register plugin capabilities
	if appInfo.IsPlugin && s.pluginRegistry != nil {
		s.pluginRegistry.Register(appInfo.ID, appInfo.Capabilities)
		logger.Info("Plugin %s registered capabilities: %v", appInfo.ID, appInfo.Capabilities)
	}

	logger.Info("App installed successfully: %s", appInfo.ID)

	// Register permissions with AI Runtime
	if err := s.registerAppPermissions(ctx, appInfo.ID, appManifest); err != nil {
		logger.Warn("Failed to register app permissions: %v", err)
	}

	// Publish event
	eventType := "installed"
	if isUpdate {
		eventType = "updated"
	}
	s.publishAppEvent(eventType, appInfo.ID, map[string]interface{}{
		"name":    appManifest.Metadata.Name,
		"version": appManifest.Metadata.Version,
	})

	// Auto-restart if updating a running app
	if isUpdate && wasRunning {
		logger.Info("Auto-starting updated app %s", appInfo.ID)
		go func() {
			startReq := &proto.StartRequest{AppId: appInfo.ID}
			if _, err := s.StartApp(context.Background(), startReq); err != nil {
				logger.Warn("Failed to auto-restart updated app %s: %v", appInfo.ID, err)
			}
		}()
	}

	return &proto.InstallResponse{
		Status: &proto.Status{
			Success: true,
			Message: "App installed successfully",
		},
		AppId:   appInfo.ID,
		Updated: isUpdate,
	}, nil
}

// AsyncInstallApp starts an async installation and returns a task ID immediately.
func (s *AppManagerServer) AsyncInstallApp(ctx context.Context, req *proto.AsyncInstallRequest) (*proto.AsyncInstallResponse, error) {
	logger.Info("AsyncInstallApp called: manifest=%s, image=%s", req.ManifestPath, req.ImagePath)

	if req.ManifestPath == "" {
		return nil, fmt.Errorf("manifest_path is required")
	}

	task := s.taskStore.Create()
	logger.Info("Created install task: %s", task.ID)

	go s.runAsyncInstall(task.ID, req.ManifestPath, req.ImagePath, req.Force)

	return &proto.AsyncInstallResponse{TaskId: task.ID}, nil
}

// GetInstallProgress returns the current progress of an async install task.
func (s *AppManagerServer) GetInstallProgress(ctx context.Context, req *proto.InstallProgressRequest) (*proto.InstallProgressResponse, error) {
	task, ok := s.taskStore.Get(req.TaskId)
	if !ok {
		return nil, fmt.Errorf("task not found: %s", req.TaskId)
	}

	phase, percent, message, appID, errMsg := task.Snapshot()
	return &proto.InstallProgressResponse{
		TaskId:  req.TaskId,
		Phase:   phase,
		Percent: percent,
		Message: message,
		AppId:   appID,
		Error:   errMsg,
	}, nil
}

// runAsyncInstall executes the full installation flow in a background goroutine.
func (s *AppManagerServer) runAsyncInstall(taskID, manifestPath, imagePath string, force bool) {
	task, ok := s.taskStore.Get(taskID)
	if !ok {
		return
	}

	ctx := namespaces.WithNamespace(context.Background(), s.config.Containerd.Namespace)

	// Phase: validating (0-5%)
	task.Update("validating", 2, "Validating manifest...")
	appManifest, err := manifest.LoadManifest(manifestPath)
	if err != nil {
		task.Fail(fmt.Sprintf("Failed to load manifest: %v", err))
		return
	}
	if err := appManifest.Validate(); err != nil {
		task.Fail(fmt.Sprintf("Invalid manifest: %v", err))
		return
	}
	if err := security.ValidateSeccompProfile(s.config.Security.SeccompProfile); err != nil {
		task.Fail(fmt.Sprintf("Invalid seccomp profile: %v", err))
		return
	}
	task.Update("validating", 5, "Validation complete")

	// Check duplicate BEFORE pulling image
	asyncAppID := appManifest.Metadata.ID
	if s.registry.Exists(asyncAppID) {
		if !force {
			task.Fail(fmt.Sprintf("App %s already exists. Use force=true to overwrite.", asyncAppID))
			return
		}
		task.Update("validating", 4, "Removing existing app for overwrite...")
		if err := s.stopAndCleanupApp(ctx, asyncAppID); err != nil {
			logger.Warn("Failed to cleanup existing app %s: %v", asyncAppID, err)
		}
	}

	// Phase: pulling (5-80%)
	// Phase: pulling (5-80%)
	if imagePath != "" && s.client != nil {
		imageName := appManifest.GetNormalizedImage()

		isLocalFile := strings.HasSuffix(imagePath, ".tar") ||
			strings.HasSuffix(imagePath, ".tar.gz") ||
			strings.HasSuffix(imagePath, ".tgz") ||
			strings.HasPrefix(imagePath, "/") ||
			strings.HasPrefix(imagePath, "./")

		if !isLocalFile {
			// Remote image — pull with progress tracking
			task.Update("pulling", 5, "Starting image pull...")
			progressCh := make(chan containerd.PullProgress, 8)
			pullDone := make(chan struct{})
			go func() {
				defer close(pullDone)
				for p := range progressCh {
					if p.Phase == "error" {
						task.Fail(fmt.Sprintf("Failed to pull image: %s", p.Error))
						return
					}
					// Map pull percent (0-100) to task percent (5-80)
					mapped := 5 + p.Percent*0.75
					task.Update("pulling", mapped, p.Message)
				}
			}()
			s.client.PullImageAsync(ctx, imagePath, progressCh)
			<-pullDone
			if phase, _, _, _, _ := task.Snapshot(); phase == "error" {
				return
			}
			normalizedRef := manifest.NormalizeImageName(imagePath)
			appManifest.Spec.Image = normalizedRef
			task.Update("pulling", 80, "Image pull complete")
		} else {
			// Local file — import (fast)
			task.Update("pulling", 10, "Importing local image...")
			importedName, err := s.client.ImportImage(ctx, imagePath, imageName)
			if err != nil {
				task.Fail(fmt.Sprintf("Failed to import image: %v", err))
				return
			}
			logger.Info("Image imported: %s", importedName)
			task.Update("pulling", 80, "Image import complete")

			// Reconcile the tar's true RepoTag against manifest.image and verify
			// the manifest name is now resolvable in containerd. This catches the
			// "tar tag != manifest.image" failure mode at install time instead of
			// letting StartApp fail later (and auto-uninstall on offline devices).
			tarRepoTag := utils.ExtractImageNameFromTar(imagePath)
			tarNormalized := manifest.NormalizeImageName(tarRepoTag)
			if tarRepoTag == "" {
				logger.Warn("Install reconcile: could not read RepoTag from tar %s (manifest.image=%s)", imagePath, imageName)
			} else if tarNormalized != imageName {
				logger.Warn("Install reconcile: tar RepoTag %q (normalized %q) differs from manifest.image %q — retagged on import", tarRepoTag, tarNormalized, imageName)
			} else {
				logger.Info("Install reconcile: tar RepoTag %q matches manifest.image %q", tarRepoTag, imageName)
			}
			if _, verifyErr := s.client.GetImage(ctx, imageName); verifyErr != nil {
				task.Fail(fmt.Sprintf("Install verify failed: manifest.image %s is not resolvable after import (retag may have failed). tar RepoTag=%q. Error: %v", imageName, tarRepoTag, verifyErr))
				return
			}

			// Save tar file for self-healing recovery after power loss
			if saveErr := containerd.SaveImageTar(appManifest.Metadata.ID, imagePath); saveErr != nil {
				logger.Warn("Failed to save image tar for recovery: %v", saveErr)
			}

			// Clean up uploaded tar file after successful import
			s.cleanupUploadedTar(imagePath)
		}
	} else if s.client != nil && appManifest.GetNormalizedImage() != "" {
		// No local tar was uploaded. Before proceeding, verify the manifest
		// image is already resolvable in containerd (e.g. reinstall of an app
		// whose image was imported previously). If it is not present, fail fast:
		// an offline device cannot pull it, so StartApp would fail later and the
		// app would auto-uninstall — leaving the user with no error signal that
		// the image was never provided.
		imageName := appManifest.GetNormalizedImage()
		if _, verifyErr := s.client.GetImage(ctx, imageName); verifyErr != nil {
			task.Fail(fmt.Sprintf("No image tar was uploaded and manifest.image %q is not present in containerd. An offline device cannot pull it — re-import the app together with its image tar, or ensure the image already exists on the device. Error: %v", imageName, verifyErr))
			return
		}
		logger.Info("No image tar provided; manifest.image %s already present in containerd", imageName)
		task.Update("pulling", 80, "Image already present")
	} else {
		task.Update("pulling", 80, "No image to pull")
	}

	// Phase: registering (80-95%)
	task.Update("registering", 85, "Registering application...")
	appInfo := &registry.AppInfo{
		ID:           appManifest.Metadata.ID,
		Name:         appManifest.Metadata.Name,
		Version:      appManifest.Metadata.Version,
		Image:        appManifest.GetNormalizedImage(),
		State:        registry.AppStateInstalled,
		ManifestPath: manifestPath,
		InstancePath: fmt.Sprintf("%s/%s", s.config.Apps.InstancesPath, appManifest.Metadata.ID),
		IsPlugin:     appManifest.IsPlugin(),
	}

	if appManifest.IsPlugin() {
		for _, cap := range appManifest.Spec.Plugin.Capabilities {
			appInfo.Capabilities = append(appInfo.Capabilities, registry.CapabilityInfo{
				ID:        cap.ID,
				Version:   cap.Version,
				Transport: cap.Transport,
			})
		}
	}

	task.Update("registering", 90, "Saving app registry...")
	if err := s.registry.Register(appInfo); err != nil {
		task.Fail(fmt.Sprintf("Failed to register app: %v", err))
		return
	}

	// Create instance directory
	if err := os.MkdirAll(appInfo.InstancePath, 0755); err != nil {
		task.Fail(fmt.Sprintf("Failed to create instance directory: %v", err))
		return
	}

	task.Update("registering", 95, "Registering permissions...")
	if err := s.registerAppPermissions(ctx, appInfo.ID, appManifest); err != nil {
		logger.Warn("Failed to register app permissions: %v", err)
	}

	// Publish event
	s.publishAppEvent("installed", appInfo.ID, map[string]interface{}{
		"name":    appManifest.Metadata.Name,
		"version": appManifest.Metadata.Version,
	})

	logger.Info("Async install complete: %s", appInfo.ID)

	// Phase: complete (100%)
	task.Complete(appInfo.ID)
}

// StartApp implements AppManager.StartApp
func (s *AppManagerServer) StartApp(ctx context.Context, req *proto.StartRequest) (*proto.Status, error) {
	logger.Info("StartApp called: app_id=%s", req.AppId)

	// Get app info
	appInfo, err := s.registry.Get(req.AppId)
	if err != nil {
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("App not found: %s", req.AppId),
			Code:    404,
		}, nil
	}

	if appInfo.State == registry.AppStateRunning {
		return &proto.Status{
			Success: true,
			Message: "App is already running",
		}, nil
	}

	// Load manifest
	appManifest, err := manifest.LoadManifest(appInfo.ManifestPath)
	if err != nil {
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("Failed to load manifest: %v", err),
			Code:    500,
		}, nil
	}

	// Validate plugin dependencies before start
	if appManifest.HasPluginDependencies() && s.resolver != nil {
		unsatisfied, err := s.resolver.Resolve(appManifest)
		if err != nil {
			return &proto.Status{
				Success: false,
				Message: fmt.Sprintf("Plugin dependency check failed: %v (unsatisfied: %v)", err, unsatisfied),
				Code:    412,
			}, nil
		}
	}

	// Check if this is a multi-container application
	if appManifest.IsMultiContainer() {
		return s.startMultiContainerApp(ctx, req.AppId, appInfo, appManifest)
	}

	// Single container mode (existing logic)
	return s.startSingleContainerApp(ctx, req.AppId, appInfo, appManifest)
}

// startSingleContainerApp starts a single-container application
func (s *AppManagerServer) startSingleContainerApp(ctx context.Context, appID string, appInfo *registry.AppInfo, appManifest *manifest.AppManifest) (*proto.Status, error) {
	// IMPORTANT: Use the image reference stored in the registry (set during
	// InstallApp) rather than re-reading from the manifest file. The registry
	// entry contains the normalized image name that matches what containerd
	// actually stores (e.g. "docker.io/nginx/nginx-ingress:edge-alpine").
	if appInfo.Image != "" {
		appManifest.Spec.Image = appInfo.Image
	}

	// Build container config
	containerConfig, err := security.BuildContainerConfig(appManifest, s.config.Security.SeccompProfile)
	if err != nil {
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("Failed to build container config: %v", err),
			Code:    500,
		}, nil
	}
	// Apply dev mode if enabled in manifest
	if appManifest.Spec.Dev != nil && appManifest.Spec.Dev.Enabled {
		manifestDir := filepath.Dir(appInfo.ManifestPath)
		security.ApplyDevMode(containerConfig, appManifest.Spec.Dev, manifestDir)
		logger.Info("Dev mode enabled for app %s", appID)
	}
	// Create and start container via containerd
	if s.runtime != nil {
		// Preload models before creating container
		s.PreloadModels(ctx, appID, appManifest)

		// Ensure namespace is set in context for containerd operations
		ctxWithNamespace := namespaces.WithNamespace(ctx, s.config.Containerd.Namespace)

		// Auto-pull: if the image isn't in containerd yet, try to pull it.
		// This handles cases where the app was installed before PullImage was
		// implemented, or the image was pruned.
		imageName := appManifest.GetNormalizedImage()
		if _, err := s.client.GetImage(ctxWithNamespace, imageName); err != nil {
			logger.Warn("Image %s (manifest.image=%q) not found locally, attempting to pull", imageName, appManifest.Spec.Image)
			if pullErr := s.client.PullImage(ctxWithNamespace, imageName); pullErr != nil {
				// Readable failure: name the image, explain the two likely root
				// causes (tag mismatch on the imported tar, or an offline device
				// that cannot reach a registry), point to the remediation, and
				// keep the original pull error for diagnostics.
				return &proto.Status{
					Success: false,
					Message: fmt.Sprintf(
						"Image %s is not present locally and could not be pulled. "+
							"This usually means the imported image tar's tag does not match manifest.image (%q), "+
							"or the device is offline and cannot reach a registry. "+
							"Re-import the app together with its image tar, or ensure the image tag matches manifest.image. "+
							"Pull error: %v",
						imageName, appManifest.Spec.Image, pullErr),
					Code: 500,
				}, nil
			}
			logger.Info("Image pulled successfully on start: %s", imageName)
		}

		// Check if container already exists (from previous run)
		// If exists, remove it first to avoid snapshot conflicts
		containerID := fmt.Sprintf("aipc-%s", appID)
		if existingContainer, err := s.client.GetContainer(ctxWithNamespace, containerID); err == nil {
			logger.Info("Container %s already exists, removing it first", containerID)
			// Try to stop and remove existing container
			if task, taskErr := existingContainer.Task(ctxWithNamespace, nil); taskErr == nil {
				// Task exists, stop it first
				if stopErr := s.runtime.StopAppContainer(ctxWithNamespace, task, 5); stopErr != nil {
					logger.Warn("Failed to stop existing container task: %v", stopErr)
				}
			}
			// Remove container (this also cleans up snapshot)
			if removeErr := s.runtime.RemoveAppContainer(ctxWithNamespace, existingContainer); removeErr != nil {
				logger.Warn("Failed to remove existing container: %v", removeErr)
				// Continue anyway, CreateContainer will handle snapshot conflict
			}
		}

		container, err := s.runtime.CreateAppContainer(ctxWithNamespace, appID, appManifest, containerConfig)
		if err != nil {
			// Retry: snapshot corruption from power loss can cause this failure.
			// Re-unpack the image from content store and try once more.
			errMsg := err.Error()
			if strings.Contains(errMsg, "snapshot") ||
				strings.Contains(errMsg, "parent") ||
				strings.Contains(errMsg, "bucket: not found") {
				logger.Info("[SELF-HEAL] Container creation failed with snapshot error, re-unpacking: %s", imageName)
				// Remove stale snapshot before re-unpacking so WithNewSnapshot
				// does not hit "already exists" again.
				snapshotName := fmt.Sprintf("aipc-%s-snapshot", appID)
				if snapErr := s.client.RemoveSnapshot(ctxWithNamespace, snapshotName); snapErr != nil {
					logger.Warn("[SELF-HEAL] Failed to remove stale snapshot %s: %v", snapshotName, snapErr)
				}
				if img, imgErr := s.client.GetImage(ctxWithNamespace, imageName); imgErr == nil {
					if unpackErr := img.Unpack(ctxWithNamespace, "overlayfs"); unpackErr == nil {
						container, err = s.runtime.CreateAppContainer(ctxWithNamespace, appID, appManifest, containerConfig)
					}
				}
			}
			if err != nil {
				return &proto.Status{
					Success: false,
					Message: fmt.Sprintf("Failed to create container: %v", err),
					Code:    500,
				}, nil
			}
		}

		// Start container
		task, err := s.runtime.StartAppContainer(ctxWithNamespace, container, appID)
		if err != nil {
			// Try to remove container if start failed
			if removeErr := s.runtime.RemoveAppContainer(ctxWithNamespace, container); removeErr != nil {
				logger.Warn("Failed to remove container after start failure: %v", removeErr)
			}
			return &proto.Status{
				Success: false,
				Message: fmt.Sprintf("Failed to start container: %v", err),
				Code:    500,
			}, nil
		}

		// Update app info with container details
		appInfo.ContainerID = container.ID()
		if taskPid := task.Pid(); taskPid > 0 {
			appInfo.PID = int(taskPid)
		}
		appInfo.StartedAt = time.Now()

		// Update registry
		if err := s.registry.Update(appInfo); err != nil {
			logger.Warn("Failed to update registry: %v", err)
		}

		// Add to auto-restart monitoring if enabled
		if s.autoRestart != nil && appManifest.IsAutoRestartEnabled() {
			if err := s.autoRestart.AddApp(ctx, appID, container, appManifest, appInfo.ManifestPath); err != nil {
				logger.Warn("Failed to add app to auto-restart monitoring: %v", err)
			}
		}
	} else {
		logger.Warn("Containerd runtime not available, skipping container creation")
	}

	// Update state
	if err := s.registry.SetState(appID, registry.AppStateRunning); err != nil {
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("Failed to update state: %v", err),
			Code:    500,
		}, nil
	}

	// Update plugin discovery if this is a plugin
	if appManifest.IsPlugin() && s.discovery != nil {
		entry := s.buildDiscoveryEntry(appManifest, "running")
		if err := s.discovery.RegisterPlugin(entry); err != nil {
			logger.Warn("Failed to update plugin discovery for %s: %v", appID, err)
		} else {
			logger.Info("Plugin %s registered in discovery", appID)
		}
		// Publish plugin status event
		s.publishAppEvent("plugin/status", appID, map[string]interface{}{
			"state":        "running",
			"capabilities": appManifest.Spec.Plugin.Capabilities,
		})
	}

	logger.Info("App started successfully: %s", appID)

	// Publish event
	s.publishAppEvent("started", appID, map[string]interface{}{
		"container_id": appInfo.ContainerID,
		"pid":          appInfo.PID,
	})

	return &proto.Status{
		Success: true,
		Message: "App started successfully",
	}, nil
}

// startMultiContainerApp starts a multi-container application (Main/Sub architecture)
func (s *AppManagerServer) startMultiContainerApp(ctx context.Context, appID string, appInfo *registry.AppInfo, appManifest *manifest.AppManifest) (*proto.Status, error) {
	logger.Info("Starting multi-container app: app_id=%s, containers=%d", appID, len(appManifest.Spec.Containers))

	if s.runtime == nil {
		return &proto.Status{
			Success: false,
			Message: "Containerd runtime not available",
			Code:    500,
		}, nil
	}

	ctxWithNamespace := namespaces.WithNamespace(ctx, s.config.Containerd.Namespace)

	// Preload models
	s.PreloadModels(ctx, appID, appManifest)

	// Check if there's an existing instance
	s.multiContainerMutex.Lock()
	if existingInstance, ok := s.multiContainerInstances[appID]; ok {
		// Cleanup existing instance
		s.runtime.StopMultiContainerApp(ctxWithNamespace, existingInstance, appManifest, 5)
		s.runtime.RemoveMultiContainerApp(ctxWithNamespace, existingInstance)
		delete(s.multiContainerInstances, appID)
	}
	s.multiContainerMutex.Unlock()

	// Create all containers
	instance, err := s.runtime.CreateMultiContainerApp(
		ctxWithNamespace,
		appID,
		appManifest,
		s.config.Security.SeccompProfile,
	)
	if err != nil {
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("Failed to create multi-container app: %v", err),
			Code:    500,
		}, nil
	}

	// Start all containers in order
	if err := s.runtime.StartMultiContainerApp(ctxWithNamespace, appID, instance, appManifest); err != nil {
		// Cleanup on failure
		s.runtime.RemoveMultiContainerApp(ctxWithNamespace, instance)
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("Failed to start multi-container app: %v", err),
			Code:    500,
		}, nil
	}

	// Store instance
	s.multiContainerMutex.Lock()
	s.multiContainerInstances[appID] = instance
	s.multiContainerMutex.Unlock()

	// Update app info
	mainName, mainContainer := appManifest.GetMainContainer()
	if mainContainer != nil {
		if mainC, ok := instance.Containers[mainName]; ok {
			appInfo.ContainerID = mainC.ID()
		}
		if mainTask, ok := instance.Tasks[mainName]; ok {
			appInfo.PID = int(mainTask.Pid())
		}
	}
	appInfo.StartedAt = time.Now()
	appInfo.IsMultiContainer = true

	// Update registry
	if err := s.registry.Update(appInfo); err != nil {
		logger.Warn("Failed to update registry: %v", err)
	}

	// Update state
	if err := s.registry.SetState(appID, registry.AppStateRunning); err != nil {
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("Failed to update state: %v", err),
			Code:    500,
		}, nil
	}

	logger.Info("Multi-container app started successfully: %s (%d containers)", appID, len(instance.Containers))

	// Publish event
	containerNames := make([]string, 0, len(instance.Containers))
	for name := range instance.Containers {
		containerNames = append(containerNames, name)
	}
	s.publishAppEvent("started", appID, map[string]interface{}{
		"container_id":    appInfo.ContainerID,
		"pid":             appInfo.PID,
		"multi_container": true,
		"containers":      containerNames,
	})

	return &proto.Status{
		Success: true,
		Message: fmt.Sprintf("Multi-container app started successfully (%d containers)", len(instance.Containers)),
	}, nil
}

// StopApp implements AppManager.StopApp
func (s *AppManagerServer) StopApp(ctx context.Context, req *proto.StopRequest) (*proto.Status, error) {
	ctx = s.withNamespace(ctx)
	logger.Info("StopApp called: app_id=%s, timeout=%d", req.AppId, req.TimeoutSeconds)

	// Get app info
	appInfo, err := s.registry.Get(req.AppId)
	if err != nil {
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("App not found: %s", req.AppId),
			Code:    404,
		}, nil
	}

	if appInfo.State != registry.AppStateRunning {
		return &proto.Status{
			Success: true,
			Message: "App is not running",
		}, nil
	}

	// Use a background context for the actual stop operation
	termCtx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	termCtx = s.withNamespace(termCtx)

	// Check if this is a multi-container application
	if appInfo.IsMultiContainer {
		return s.stopMultiContainerApp(termCtx, req.AppId, appInfo, req.TimeoutSeconds)
	}

	// Single container mode
	return s.stopSingleContainerApp(termCtx, req.AppId, appInfo, req.TimeoutSeconds)
}

// stopSingleContainerApp stops a single-container application
func (s *AppManagerServer) stopSingleContainerApp(ctx context.Context, appID string, appInfo *registry.AppInfo, timeoutSeconds int32) (*proto.Status, error) {
	// Stop container via containerd
	if appInfo.ContainerID != "" && s.client != nil && s.runtime != nil {
		// Ensure namespace is set in context for containerd operations
		ctxWithNamespace := namespaces.WithNamespace(ctx, s.config.Containerd.Namespace)

		container, err := s.client.GetContainer(ctxWithNamespace, appInfo.ContainerID)
		if err == nil {
			// Get task with namespace context
			task, err := container.Task(ctxWithNamespace, nil)
			if err == nil {
				timeout := int32(timeoutSeconds)
				if timeout <= 0 {
					timeout = 10 // Default 10 seconds
				}
				if err := s.runtime.StopAppContainer(ctxWithNamespace, task, timeout); err != nil {
					logger.Warn("Failed to stop container: %v", err)
					// Continue with state update even if stop fails
				}
			} else {
				logger.Warn("Failed to get container task: %v", err)
			}
		} else {
			logger.Warn("Failed to get container: %v", err)
		}
	}

	// Remove from auto-restart monitoring
	if s.autoRestart != nil {
		s.autoRestart.RemoveApp(appID)
	}

	// Update state
	if err := s.registry.SetState(appID, registry.AppStateStopped); err != nil {
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("Failed to update state: %v", err),
			Code:    500,
		}, nil
	}

	// Clear SDK-registered web URL
	s.registry.SetWebURL(appID, "")

	// Update plugin discovery if this was a plugin
	if s.discovery != nil {
		if err := s.discovery.SetPluginState(appID, "stopped"); err != nil {
			// Not fatal: plugin may not be in discovery
			_ = err
		}
		// Publish plugin status event
		s.publishAppEvent("plugin/status", appID, map[string]interface{}{
			"state": "stopped",
		})
	}

	logger.Info("App stopped successfully: %s", appID)

	// Publish event
	s.publishAppEvent("stopped", appID, map[string]interface{}{
		"timeout_seconds": timeoutSeconds,
	})

	return &proto.Status{
		Success: true,
		Message: "App stopped successfully",
	}, nil
}

// stopMultiContainerApp stops a multi-container application
func (s *AppManagerServer) stopMultiContainerApp(ctx context.Context, appID string, appInfo *registry.AppInfo, timeoutSeconds int32) (*proto.Status, error) {
	logger.Info("Stopping multi-container app: app_id=%s", appID)

	// Get the instance
	s.multiContainerMutex.RLock()
	instance, ok := s.multiContainerInstances[appID]
	s.multiContainerMutex.RUnlock()

	if !ok {
		logger.Warn("Multi-container instance not found for app %s, updating state only", appID)
		s.registry.SetState(appID, registry.AppStateStopped)
		return &proto.Status{
			Success: true,
			Message: "App stopped (instance not found, state updated)",
		}, nil
	}

	// Load manifest to get shutdown order
	appManifest, err := manifest.LoadManifest(appInfo.ManifestPath)
	if err != nil {
		logger.Warn("Failed to load manifest for shutdown: %v", err)
	}

	ctxWithNamespace := namespaces.WithNamespace(ctx, s.config.Containerd.Namespace)

	// Stop containers in order
	if appManifest != nil {
		if err := s.runtime.StopMultiContainerApp(ctxWithNamespace, instance, appManifest, timeoutSeconds); err != nil {
			logger.Warn("Failed to stop multi-container app: %v", err)
		}
	}

	// Remove from tracking
	s.multiContainerMutex.Lock()
	delete(s.multiContainerInstances, appID)
	s.multiContainerMutex.Unlock()

	// Update state
	if err := s.registry.SetState(appID, registry.AppStateStopped); err != nil {
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("Failed to update state: %v", err),
			Code:    500,
		}, nil
	}

	logger.Info("Multi-container app stopped successfully: %s", appID)

	// Publish event
	s.publishAppEvent("stopped", appID, map[string]interface{}{
		"timeout_seconds": timeoutSeconds,
		"multi_container": true,
	})

	return &proto.Status{
		Success: true,
		Message: "Multi-container app stopped successfully",
	}, nil
}

// UninstallApp implements AppManager.UninstallApp
func (s *AppManagerServer) UninstallApp(ctx context.Context, req *proto.UninstallRequest) (*proto.Status, error) {
	// Use background context for namespace and nested calls
	bgCtx, cancel := context.WithTimeout(context.Background(), 60*time.Second)
	defer cancel()
	bgCtx = s.withNamespace(bgCtx)

	logger.Info("UninstallApp called: app_id=%s, keep_logs=%v", req.AppId, req.KeepLogs)

	// Remove from auto-restart monitoring first
	if s.autoRestart != nil {
		s.autoRestart.RemoveApp(req.AppId)
	}

	// Get app info
	appInfo, err := s.registry.Get(req.AppId)
	if err != nil {
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("App not found: %s", req.AppId),
			Code:    404,
		}, nil
	}

	// Stop if running
	if appInfo.State == registry.AppStateRunning {
		stopReq := &proto.StopRequest{
			AppId:          req.AppId,
			TimeoutSeconds: 10,
		}
		if _, err := s.StopApp(bgCtx, stopReq); err != nil {
			logger.Warn("Failed to stop app before uninstall: %v", err)
		}
	}

	// Remove container if exists
	if appInfo.ContainerID != "" && s.client != nil && s.runtime != nil {
		container, err := s.client.GetContainer(bgCtx, appInfo.ContainerID)
		if err == nil {
			// Stop if running
			task, err := container.Task(bgCtx, nil)
			if err == nil {
				if stopErr := s.runtime.StopAppContainer(bgCtx, task, 10); stopErr != nil {
					logger.Warn("Failed to stop container: %v, will retry remove anyway", stopErr)
				}
			}
			// Remove container (with retry)
			if err := s.runtime.RemoveAppContainer(bgCtx, container); err != nil {
				logger.Warn("First remove attempt failed: %v, retrying...", err)
				retryCtx, retryCancel := context.WithTimeout(context.Background(), 30*time.Second)
				retryCtx = s.withNamespace(retryCtx)
				if retryErr := s.runtime.RemoveAppContainer(retryCtx, container); retryErr != nil {
					logger.Warn("Retry remove also failed: %v", retryErr)
				}
				retryCancel()
			}
		} else {
			// Container not found in containerd — stale registry entry, continue cleanup
			logger.Warn("Container not found in containerd (may already be removed): %v", err)
		}
	}

	// Remove instance directory (unless keeping logs)
	if !req.KeepLogs {
		if err := os.RemoveAll(appInfo.InstancePath); err != nil {
			logger.Warn("Failed to remove instance directory: %v", err)
		}
	}

	// Unload models from AI Runtime
	if appInfo.ManifestPath != "" {
		s.UnloadModels(bgCtx, req.AppId, appInfo.ManifestPath)
	}

	// Remove containerd image
	if appInfo.Image != "" && s.client != nil {
		if err := s.client.RemoveImage(bgCtx, appInfo.Image, true); err != nil {
			logger.Warn("Failed to remove image %s: %v", appInfo.Image, err)
		}
	}

	// Remove saved tar file (self-healing backup)
	tarPath := filepath.Join("/data/apps/images", req.AppId+".tar")
	if _, err := os.Stat(tarPath); err == nil {
		if err := os.Remove(tarPath); err != nil {
			logger.Warn("Failed to remove tar file %s: %v", tarPath, err)
		}
	}

	// Remove uploaded manifest file and its parent directory
	if appInfo.ManifestPath != "" {
		manifestDir := filepath.Dir(appInfo.ManifestPath)
		if err := os.RemoveAll(manifestDir); err != nil {
			logger.Warn("Failed to remove manifest dir %s: %v", manifestDir, err)
		}
	}

	// Unregister from plugin system
	if s.pluginRegistry != nil {
		s.pluginRegistry.Unregister(req.AppId)
	}
	if s.discovery != nil {
		s.discovery.UnregisterPlugin(req.AppId)
	}

	// Unregister
	if err := s.registry.Unregister(req.AppId); err != nil {
		return &proto.Status{
			Success: false,
			Message: fmt.Sprintf("Failed to unregister app: %v", err),
			Code:    500,
		}, nil
	}

	logger.Info("App uninstalled successfully: %s", req.AppId)

	// Publish event
	s.publishAppEvent("uninstalled", req.AppId, map[string]interface{}{
		"keep_logs": req.KeepLogs,
	})

	return &proto.Status{
		Success: true,
		Message: "App uninstalled successfully",
	}, nil
}

// ListApps implements AppManager.ListApps
func (s *AppManagerServer) ListApps(ctx context.Context, req *emptypb.Empty) (*proto.AppList, error) {
	apps := s.registry.List()

	pbApps := make([]*proto.AppInfo, len(apps))
	for i, app := range apps {
		pbApps[i] = &proto.AppInfo{
			Id:           app.ID,
			Name:         app.Name,
			Version:      app.Version,
			State:        string(app.State),
			ContainerId:  app.ContainerID,
			Pid:          int32(app.PID),
			InstalledAt:  app.InstalledAt.Unix(),
			StartedAt:    app.StartedAt.Unix(),
			StoppedAt:    stoppedAtUnix(app.StoppedAt),
			RestartCount: int32(app.RestartCount),
			ManifestPath: app.ManifestPath,
			InstancePath: app.InstancePath,
			WebUrl:       app.WebURL,
		}
	}

	return &proto.AppList{
		Apps: pbApps,
	}, nil
}

// GetApp implements AppManager.GetApp
func (s *AppManagerServer) GetApp(ctx context.Context, req *proto.GetAppRequest) (*proto.AppInfo, error) {
	app, err := s.registry.Get(req.AppId)
	if err != nil {
		return nil, err
	}

	return &proto.AppInfo{
		Id:           app.ID,
		Name:         app.Name,
		Version:      app.Version,
		State:        string(app.State),
		ContainerId:  app.ContainerID,
		Pid:          int32(app.PID),
		InstalledAt:  app.InstalledAt.Unix(),
		StartedAt:    app.StartedAt.Unix(),
		StoppedAt:    stoppedAtUnix(app.StoppedAt),
		RestartCount: int32(app.RestartCount),
		ManifestPath: app.ManifestPath,
		InstancePath: app.InstancePath,
		WebUrl:       app.WebURL,
	}, nil
}

// GetAppStats implements AppManager.GetAppStats
func (s *AppManagerServer) GetAppStats(ctx context.Context, req *proto.GetAppRequest) (*proto.AppStats, error) {
	// Get app info
	appInfo, err := s.registry.Get(req.AppId)
	if err != nil {
		return &proto.AppStats{
			AppId: req.AppId,
		}, fmt.Errorf("app not found: %w", err)
	}

	stats := &proto.AppStats{
		AppId: req.AppId,
	}

	// Calculate uptime
	if !appInfo.StartedAt.IsZero() {
		stats.UptimeSeconds = int64(time.Since(appInfo.StartedAt).Seconds())
	}

	// If container is running, get actual stats from cgroup
	if appInfo.ContainerID != "" && s.client != nil {
		ctxWithNamespace := namespaces.WithNamespace(ctx, s.config.Containerd.Namespace)

		container, err := s.client.GetContainer(ctxWithNamespace, appInfo.ContainerID)
		if err == nil {
			cstats, err := s.client.GetContainerStats(ctxWithNamespace, container)
			if err == nil {
				stats.CpuUsagePercent = cstats.CPUPercent
				stats.MemoryUsageBytes = int64(cstats.MemoryUsage)
				stats.MemoryLimitBytes = int64(cstats.MemoryLimit)
				stats.ThreadCount = int32(cstats.Pids)
			} else {
				logger.Warn("Failed to get container stats: %v", err)
			}
		} else {
			logger.Warn("Failed to get container: %v", err)
		}
	}

	return stats, nil
}

// RegisterWebUrl registers a web access path for an app (called by app via SDK)
func (s *AppManagerServer) RegisterWebUrl(ctx context.Context, req *proto.RegisterWebUrlRequest) (*proto.RegisterWebUrlResponse, error) {
	if req.AppId == "" {
		return &proto.RegisterWebUrlResponse{Success: false, Message: "app_id is required"}, nil
	}
	if req.Path == "" {
		req.Path = "/"
	}
	if err := s.registry.SetWebURL(req.AppId, req.Path); err != nil {
		return &proto.RegisterWebUrlResponse{Success: false, Message: err.Error()}, nil
	}
	logger.Info("App %s registered web_url: %s", req.AppId, req.Path)
	return &proto.RegisterWebUrlResponse{Success: true}, nil
}

// GetAppLogs implements AppManager.GetAppLogs
// Reads logs from the log file that container stdout/stderr is redirected to.
func (s *AppManagerServer) GetAppLogs(req *proto.GetLogsRequest, stream proto.AppManager_GetAppLogsServer) error {
	// Get app info
	appInfo, err := s.registry.Get(req.AppId)
	if err != nil {
		return fmt.Errorf("app not found: %w", err)
	}

	logPath := fmt.Sprintf("%s/logs/app.log", appInfo.InstancePath)

	file, err := os.Open(logPath)
	if err != nil {
		return fmt.Errorf("no logs available for app %s: %w", req.AppId, err)
	}
	defer file.Close()

	// Read existing lines (with optional tail limit)
	var linesToSend []string
	if req.MaxLines > 0 {
		// Read last N lines
		lines, err := readLastNLines(file, int(req.MaxLines))
		if err != nil {
			return fmt.Errorf("failed to read log file: %w", err)
		}
		linesToSend = lines
	} else {
		// Read all lines
		scanner := bufio.NewScanner(file)
		for scanner.Scan() {
			linesToSend = append(linesToSend, scanner.Text())
		}
	}

	// Send existing lines
	for _, line := range linesToSend {
		logEntry := parseLogLine(line)
		if err := stream.Send(logEntry); err != nil {
			return err
		}
	}

	// If not following, we're done
	if !req.Follow {
		return nil
	}

	// Follow mode: tail the file for new lines
	// Get current file size as starting position
	stat, err := file.Stat()
	if err != nil {
		return fmt.Errorf("failed to stat log file: %w", err)
	}
	lastSize := stat.Size()
	var partialLine string

	ticker := time.NewTicker(200 * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-stream.Context().Done():
			return nil
		case <-ticker.C:
			// Check if file has grown
			stat, err := os.Stat(logPath)
			if err != nil {
				continue
			}

			currentSize := stat.Size()
			if currentSize > lastSize {
				// File has grown, read new content
				file.Seek(lastSize, 0)
				newBytes := make([]byte, currentSize-lastSize)
				n, err := file.Read(newBytes)
				if err != nil && err != io.EOF {
					continue
				}

				// Parse lines from new content
				content := partialLine + string(newBytes[:n])
				lines := strings.Split(content, "\n")

				// Last element might be partial line (no trailing newline)
				for i, line := range lines {
					if i == len(lines)-1 {
						// Last part - might be partial
						if !strings.HasSuffix(content, "\n") {
							partialLine = line
						} else if line != "" {
							logEntry := parseLogLine(line)
							if err := stream.Send(logEntry); err != nil {
								return err
							}
						}
					} else if line != "" {
						logEntry := parseLogLine(line)
						if err := stream.Send(logEntry); err != nil {
							return err
						}
					}
				}

				lastSize = currentSize
			} else if currentSize < lastSize {
				// File was truncated/rotated, reset
				lastSize = 0
				partialLine = ""
			}
		}
	}
}

// parseLogLine parses a raw log line into a LogLine proto message.
// Recognized formats:
//
//	[LEVEL] message
//	2006-01-02T15:04:05Z LEVEL message
//	2006/01/02 15:04:05 LEVEL message
//
// Falls back to "info" level with current timestamp.
func parseLogLine(line string) *proto.LogLine {
	entry := &proto.LogLine{
		Timestamp: time.Now().UnixNano(),
		Level:     "info",
		Message:   line,
	}

	// Try to parse timestamp from known log formats
	remaining := line
	if len(line) > 20 && (line[4] == '-' && line[10] == 'T') {
		// Try RFC3339-like timestamp
		tsFormats := []string{
			"2006-01-02T15:04:05Z07:00 ",
			"2006-01-02T15:04:05.000Z07:00 ",
			"2006-01-02T15:04:05 ",
		}
		for _, fmt := range tsFormats {
			if len(line) >= len(fmt) {
				if t, err := time.Parse(fmt, line[:len(fmt)]); err == nil {
					entry.Timestamp = t.UnixNano()
					remaining = strings.TrimSpace(line[len(fmt):])
					break
				}
			}
		}
	} else if len(line) > 20 && (line[4] == '-' && line[7] == '-' && line[10] == ' ') {
		// Format: "2006-01-02 15:04:05"
		if t, err := time.Parse("2006-01-02 15:04:05 ", line[:20]); err == nil {
			entry.Timestamp = t.UnixNano()
			remaining = strings.TrimSpace(line[20:])
		}
	}

	// Parse level from remaining text (prefix matching only to avoid false positives)
	switch {
	case strings.HasPrefix(remaining, "[ERROR]") || strings.HasPrefix(remaining, "ERROR"):
		entry.Level = "error"
	case strings.HasPrefix(remaining, "[WARN]") || strings.HasPrefix(remaining, "WARN"):
		entry.Level = "warn"
	case strings.HasPrefix(remaining, "[DEBUG]") || strings.HasPrefix(remaining, "DEBUG"):
		entry.Level = "debug"
	case strings.HasPrefix(remaining, "[INFO]") || strings.HasPrefix(remaining, "INFO"):
		entry.Level = "info"
	}

	return entry
}

// readLastNLines reads the last n lines from a file
func readLastNLines(file *os.File, n int) ([]string, error) {
	scanner := bufio.NewScanner(file)
	var lines []string
	for scanner.Scan() {
		lines = append(lines, scanner.Text())
	}
	if err := scanner.Err(); err != nil {
		return nil, err
	}
	if len(lines) > n {
		lines = lines[len(lines)-n:]
	}
	return lines, nil
}

// buildDiscoveryEntry creates a DiscoveryEntry from a manifest
func (s *AppManagerServer) buildDiscoveryEntry(m *manifest.AppManifest, state string) plugin.DiscoveryEntry {
	entry := plugin.DiscoveryEntry{
		AppID:   m.Metadata.ID,
		Version: m.Metadata.Version,
		State:   state,
	}

	if m.Spec.Plugin != nil {
		for _, cap := range m.Spec.Plugin.Capabilities {
			dc := plugin.DiscoveryCapability{
				ID:        cap.ID,
				Version:   cap.Version,
				Transport: cap.Transport,
			}

			if cap.Transport == "grpc" || cap.Transport == "both" {
				dc.GRPC = &plugin.DiscoveryGRPC{
					SocketPath: fmt.Sprintf("/run/aipc/plugins/%s.sock", m.Metadata.ID),
					Service:    cap.Proto,
				}
			}

			if cap.Transport == "event" || cap.Transport == "both" {
				if cap.Topics != nil {
					dc.Event = &plugin.DiscoveryEvent{
						Publish:   cap.Topics.Publish,
						Subscribe: cap.Topics.Subscribe,
					}
				}
			}

			entry.Capabilities = append(entry.Capabilities, dc)
		}
	}

	return entry
}

// GetDiskUsage returns disk usage statistics for images, containers, and snapshots
func (s *AppManagerServer) GetDiskUsage(ctx context.Context, _ *emptypb.Empty) (*proto.DiskUsageResponse, error) {
	if s.client == nil {
		return nil, fmt.Errorf("containerd client not available")
	}

	ctx = namespaces.WithNamespace(ctx, s.config.Containerd.Namespace)
	usage, err := s.client.GetDiskUsage(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to get disk usage: %w", err)
	}

	return &proto.DiskUsageResponse{
		ImagesSize:      usage.ImagesSize,
		ContainersSize:  usage.ContainersSize,
		SnapshotsSize:   usage.SnapshotsSize,
		TotalSize:       usage.TotalSize,
		ReclaimableSize: usage.ReclaimableSize,
	}, nil
}

// PruneResources cleans up unused containers, images, and snapshots
func (s *AppManagerServer) PruneResources(ctx context.Context, req *proto.PruneRequest) (*proto.PruneResponse, error) {
	if s.client == nil {
		return nil, fmt.Errorf("containerd client not available")
	}

	ctx = namespaces.WithNamespace(ctx, s.config.Containerd.Namespace)

	var totalReclaimed int64
	var deletedItems []string

	if req.PruneContainers {
		reclaimed, items, err := s.client.PruneContainers(ctx)
		if err != nil {
			logger.Warn("Failed to prune containers: %v", err)
		} else {
			totalReclaimed += reclaimed
			deletedItems = append(deletedItems, items...)
		}
	}

	if req.PruneImages {
		reclaimed, items, err := s.client.PruneImages(ctx)
		if err != nil {
			logger.Warn("Failed to prune images: %v", err)
		} else {
			totalReclaimed += reclaimed
			deletedItems = append(deletedItems, items...)
		}
	}

	if req.PruneSnapshots {
		reclaimed, items, err := s.client.PruneSnapshots(ctx)
		if err != nil {
			logger.Warn("Failed to prune snapshots: %v", err)
		} else {
			totalReclaimed += reclaimed
			deletedItems = append(deletedItems, items...)
		}
	}

	return &proto.PruneResponse{
		Status:         &proto.Status{Success: true, Message: "Prune completed"},
		SpaceReclaimed: totalReclaimed,
		DeletedItems:   deletedItems,
	}, nil
}

// ListImages returns all images in the namespace
func (s *AppManagerServer) ListImages(ctx context.Context, _ *emptypb.Empty) (*proto.ImageList, error) {
	if s.client == nil {
		return nil, fmt.Errorf("containerd client not available")
	}

	ctx = namespaces.WithNamespace(ctx, s.config.Containerd.Namespace)
	images, err := s.client.ListImages(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to list images: %w", err)
	}

	var protoImages []*proto.ImageInfo
	for _, img := range images {
		protoImages = append(protoImages, &proto.ImageInfo{
			Id:        img.ID,
			Name:      img.Name,
			Size:      img.Size,
			CreatedAt: img.CreatedAt.Unix(),
			InUse:     img.InUse,
		})
	}

	return &proto.ImageList{Images: protoImages}, nil
}

// RemoveImage removes an image by ID or name
func (s *AppManagerServer) RemoveImage(ctx context.Context, req *proto.RemoveImageRequest) (*proto.Status, error) {
	if s.client == nil {
		return nil, fmt.Errorf("containerd client not available")
	}

	ctx = namespaces.WithNamespace(ctx, s.config.Containerd.Namespace)
	if err := s.client.RemoveImage(ctx, req.ImageId, req.Force); err != nil {
		return &proto.Status{Success: false, Message: err.Error()}, nil
	}

	return &proto.Status{Success: true, Message: "Image removed"}, nil
}

// InspectApp returns detailed container information as JSON
func (s *AppManagerServer) InspectApp(ctx context.Context, req *proto.GetAppRequest) (*proto.InspectResponse, error) {
	if s.client == nil {
		return nil, fmt.Errorf("containerd client not available")
	}

	// Get app info from registry
	appInfo, err := s.registry.Get(req.AppId)
	if err != nil {
		return nil, fmt.Errorf("app not found: %w", err)
	}

	if appInfo.ContainerID == "" {
		return nil, fmt.Errorf("app has no container")
	}

	ctx = namespaces.WithNamespace(ctx, s.config.Containerd.Namespace)
	container, err := s.client.GetContainer(ctx, appInfo.ContainerID)
	if err != nil {
		return nil, fmt.Errorf("failed to get container: %w", err)
	}

	info, err := container.Info(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to get container info: %w", err)
	}

	spec, err := container.Spec(ctx)
	if err != nil {
		return nil, fmt.Errorf("failed to get container spec: %w", err)
	}

	// Build inspect data
	inspectData := map[string]interface{}{
		"id":          info.ID,
		"image":       info.Image,
		"snapshotKey": info.SnapshotKey,
		"snapshotter": info.Snapshotter,
		"createdAt":   info.CreatedAt,
		"updatedAt":   info.UpdatedAt,
		"labels":      info.Labels,
		"spec":        spec,
	}

	jsonData, err := json.MarshalIndent(inspectData, "", "  ")
	if err != nil {
		return nil, fmt.Errorf("failed to marshal inspect data: %w", err)
	}

	return &proto.InspectResponse{JsonData: string(jsonData)}, nil
}

// BatchOperation performs operations on multiple apps
func (s *AppManagerServer) BatchOperation(ctx context.Context, req *proto.BatchRequest) (*proto.BatchResponse, error) {
	results := make(map[string]string)
	var successCount, failedCount int32

	for _, appID := range req.AppIds {
		var err error
		var msg string

		switch req.Operation {
		case "start":
			_, err = s.StartApp(ctx, &proto.StartRequest{AppId: appID})
			msg = "started"
		case "stop":
			timeout := req.TimeoutSeconds
			if timeout == 0 {
				timeout = 10
			}
			_, err = s.StopApp(ctx, &proto.StopRequest{AppId: appID, TimeoutSeconds: timeout})
			msg = "stopped"
		case "restart":
			// Stop then start
			timeout := req.TimeoutSeconds
			if timeout == 0 {
				timeout = 10
			}
			_, err = s.StopApp(ctx, &proto.StopRequest{AppId: appID, TimeoutSeconds: timeout})
			if err == nil {
				_, err = s.StartApp(ctx, &proto.StartRequest{AppId: appID})
			}
			msg = "restarted"
		case "uninstall":
			_, err = s.UninstallApp(ctx, &proto.UninstallRequest{AppId: appID})
			msg = "uninstalled"
		default:
			err = fmt.Errorf("unknown operation: %s", req.Operation)
		}

		if err != nil {
			results[appID] = fmt.Sprintf("failed: %v", err)
			failedCount++
		} else {
			results[appID] = msg
			successCount++
		}
	}

	return &proto.BatchResponse{
		Status:       &proto.Status{Success: failedCount == 0, Message: fmt.Sprintf("%d succeeded, %d failed", successCount, failedCount)},
		Results:      results,
		SuccessCount: successCount,
		FailedCount:  failedCount,
	}, nil
}

// StartGRPCServer starts the gRPC server
func (s *AppManagerServer) StartGRPCServer(listenAddr string) error {
	// Parse listen address (handle unix:// URLs)
	parsedAddr, err := utils.ParseListenAddress(listenAddr)
	if err != nil {
		return fmt.Errorf("failed to parse listen address: %w", err)
	}

	// Remove socket if exists
	if _, err := os.Stat(parsedAddr); err == nil {
		os.Remove(parsedAddr)
	}

	// Create listener
	lis, err := net.Listen("unix", parsedAddr)
	if err != nil {
		return fmt.Errorf("failed to listen: %w", err)
	}

	// Set socket permissions for container access
	if err := socket.SetSocketGroupPermission(parsedAddr); err != nil {
		logger.Warn("Failed to set socket permissions: %v (containers may not be able to connect)", err)
	} else {
		logger.Info("Socket permissions set for container access: %s", parsedAddr)
	}

	// Sync app states and recover autostart apps.
	go s.recoverAppsOnStartup()

	// Create gRPC server
	grpcServer := grpc.NewServer()
	proto.RegisterAppManagerServer(grpcServer, s)

	// Start server in goroutine
	go func() {
		logger.Info("App Manager gRPC server listening on: %s", parsedAddr)
		if err := grpcServer.Serve(lis); err != nil {
			logger.Fatal("gRPC server failed: %v", err)
		}
	}()

	// Handle shutdown
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)

	<-sigChan
	logger.Info("Shutting down gRPC server...")

	// Close connections (with mutex to avoid race with reconnect)
	s.aiRuntimeMutex.Lock()
	if s.aiRuntimeConn != nil {
		s.aiRuntimeConn.Close()
		s.aiRuntimeConn = nil
		s.aiRuntimeClient = nil
	}
	s.aiRuntimeMutex.Unlock()

	s.eventBusMutex.Lock()
	if s.eventBusConn != nil {
		s.eventBusConn.Close()
		s.eventBusConn = nil
		s.eventBusClient = nil
	}
	s.eventBusMutex.Unlock()

	stopped := make(chan struct{})
	go func() {
		grpcServer.GracefulStop()
		close(stopped)
	}()
	select {
	case <-stopped:
		logger.Info("Graceful stop completed")
	case <-time.After(3 * time.Second):
		logger.Info("Graceful stop timed out, force stopping...")
		grpcServer.Stop()
	}

	return nil
}

// syncAppStates synchronizes app states in registry with actual container states
func (s *AppManagerServer) syncAppStates() {
	ctx := s.withNamespace(context.Background())

	// Get all apps from registry
	apps := s.registry.List()

	// Get all actual containers
	containers, err := s.client.ListContainers(ctx)
	if err != nil {
		logger.Warn("Failed to list containers for state sync: %v", err)
		return
	}

	// Build container status map
	containerStatus := make(map[string]string)
	for _, c := range containers {
		info, err := s.client.GetContainerInfo(ctx, c)
		if err != nil {
			continue
		}
		containerStatus[info.ID] = info.Status
	}

	// Sync each app
	for _, app := range apps {
		actualStatus, exists := containerStatus[app.ContainerID]

		if !exists {
			// Container doesn't exist
			if app.State == registry.AppStateRunning {
				logger.Info("Syncing app %s state: running -> stopped (container not found)", app.ID)
				s.registry.SetState(app.ID, registry.AppStateStopped)
			}
		} else if actualStatus != "running" && app.State == registry.AppStateRunning {
			// Container exists but not running
			logger.Info("Syncing app %s state: running -> stopped (container not running)", app.ID)
			s.registry.SetState(app.ID, registry.AppStateStopped)
		} else if actualStatus == "running" && app.State != registry.AppStateRunning {
			// Container running but app marked as stopped
			logger.Info("Syncing app %s state: %s -> running (container is running)", app.ID, app.State)
			s.registry.SetState(app.ID, registry.AppStateRunning)
		}
	}

	logger.Info("App state sync completed")
}

// recoverAppsOnStartup restores runtime state and starts autostart apps after reboot.
func (s *AppManagerServer) recoverAppsOnStartup() {
	s.syncAppStates()
	s.repairSnapshotsIfNeeded()

	apps := s.registry.List()
	for _, app := range apps {
		if app.State == registry.AppStateRunning {
			continue
		}
		if app.ManifestPath == "" {
			continue
		}

		appManifest, err := manifest.LoadManifest(app.ManifestPath)
		if err != nil {
			logger.Warn("Skip startup recovery for app %s: failed to load manifest: %v", app.ID, err)
			continue
		}
		if !appManifest.Spec.Autostart {
			continue
		}

		logger.Info("Startup recovery: autostart app %s", app.ID)
		if _, err := s.StartApp(context.Background(), &proto.StartRequest{AppId: app.ID}); err != nil {
			logger.Warn("Startup recovery failed for app %s: %v", app.ID, err)
		}
	}
}

// repairSnapshotsIfNeeded detects overlayfs snapshot corruption (common after
// power loss) and re-unpacks all images from the content store.
func (s *AppManagerServer) repairSnapshotsIfNeeded() {
	if s.client == nil {
		return
	}

	ctx := s.withNamespace(context.Background())

	corrupted, err := s.client.HasSnapshotCorruption(ctx)
	if err != nil {
		logger.Warn("[SELF-HEAL] Corruption check failed: %v, attempting repair", err)
	} else if !corrupted {
		logger.Info("Snapshot integrity check passed")
		return
	}

	logger.Info("[SELF-HEAL] Snapshot corruption detected, repairing...")
	results, err := s.client.CheckAndRepairSnapshots(ctx)
	if err != nil {
		logger.Error("[SELF-HEAL] Repair failed: %v", err)
		return
	}

	repaired, failed := 0, 0
	for _, r := range results {
		if r.Repaired {
			repaired++
		} else if r.Error != nil {
			failed++
			logger.Error("[SELF-HEAL] Failed to repair %s: %v", r.ImageName, r.Error)
		}
	}
	logger.Info("[SELF-HEAL] Complete: %d repaired, %d failed", repaired, failed)
}

// ─── Model Preloading Logic ───────────────────────────────────────────────────

// ModelMeta contains model metadata from platform.db
type ModelMeta struct {
	FilePath  string
	ModelType string
	Variant   string
}

// getModelMeta retrieves model metadata from platform.db
func (s *AppManagerServer) getModelMeta(modelID string) *ModelMeta {
	if s.db == nil {
		return nil
	}
	var meta ModelMeta
	result := s.db.Table("ai_models").
		Select("file_path, model_type, variant").
		Where("model_id = ?", modelID).
		Scan(&meta)
	if result.Error != nil || meta.FilePath == "" {
		return nil
	}
	return &meta
}

// PreloadModels registers the models declared in the manifest with ai-runtime
func (s *AppManagerServer) PreloadModels(ctx context.Context, appID string, appManifest *manifest.AppManifest) {
	s.aiRuntimeMutex.RLock()
	client := s.aiRuntimeClient
	s.aiRuntimeMutex.RUnlock()
	if client == nil || !s.config.AIRuntime.Enabled {
		return
	}

	if appManifest == nil || len(appManifest.Spec.Permissions.Inference.Models) == 0 {
		return
	}

	for _, modelID := range appManifest.Spec.Permissions.Inference.Models {
		meta := s.getModelMeta(modelID)
		if meta == nil {
			logger.Warn("App %s requires model %s, but not found in platform.db", appID, modelID)
			continue
		}

		_, err := client.RegisterModel(ctx, &inferencepb.ModelRegisterRequest{
			ModelId:      modelID,
			ModelPath:    meta.FilePath,
			OwnerId:      appID,
			ModelType:    meta.ModelType,
			ModelVariant: meta.Variant,
		})
		if err != nil {
			logger.Warn("Failed to preload model %s for app %s: %v", modelID, appID, err)
		} else {
			logger.Info("Preloaded model %s (path: %s, type: %s) for app %s", modelID, meta.FilePath, meta.ModelType, appID)
		}
	}
}

// UnloadModels unregisters the models for this app_id
func (s *AppManagerServer) UnloadModels(ctx context.Context, appID string, manifestPath string) {
	s.aiRuntimeMutex.RLock()
	client := s.aiRuntimeClient
	s.aiRuntimeMutex.RUnlock()
	if client == nil || !s.config.AIRuntime.Enabled {
		return
	}

	// Unload models declared in manifest
	if manifestPath != "" {
		appManifest, err := manifest.LoadManifest(manifestPath)
		if err == nil && appManifest != nil {
			for _, modelID := range appManifest.Spec.Permissions.Inference.Models {
				_, err := client.UnregisterModel(ctx, &inferencepb.ModelInfo{
					ModelId: modelID,
					OwnerId: appID,
				})
				if err != nil {
					logger.Warn("Failed to unload model %s for app %s: %v", modelID, appID, err)
				} else {
					logger.Info("Unloaded model %s for app %s", modelID, appID)
				}
			}
		}
	}

	// Unload dynamically registered models by querying AI Runtime
	resp, err := client.ListModels(ctx, &inferencepb.Empty{})
	if err != nil {
		logger.Warn("Failed to list models for app %s cleanup: %v", appID, err)
		return
	}
	unloaded := 0
	for _, m := range resp.Models {
		if m.OwnerId == appID {
			_, err := client.UnregisterModel(ctx, &inferencepb.ModelInfo{
				ModelId: m.ModelId,
				OwnerId: appID,
			})
			if err != nil {
				logger.Warn("Failed to unload dynamic model %s for app %s: %v", m.ModelId, appID, err)
			} else {
				unloaded++
			}
		}
	}
	if unloaded > 0 {
		logger.Info("Unloaded %d dynamic models for app %s", unloaded, appID)
	}
}

// cleanupUploadedTar removes the original uploaded tar file from the upload
// directory after a successful image import. Uploaded files are saved by
// platform-api to {RootPath}/images/{timestamp}_{filename}.tar and should
// not persist after the image has been imported into containerd.
func (s *AppManagerServer) cleanupUploadedTar(imagePath string) {
	if imagePath == "" {
		return
	}

	// Only clean up files that look like uploads. Match the configured upload
	// directory (RootPath/images/) plus legacy paths for backward compatibility.
	for _, prefix := range []string{
		constants.RootPath() + "/images/",
		"/data/images/",
	} {
		if strings.HasPrefix(imagePath, prefix) {
			if err := os.Remove(imagePath); err != nil && !os.IsNotExist(err) {
				logger.Warn("Failed to cleanup uploaded tar %s: %v", imagePath, err)
			} else if err == nil {
				logger.Info("Cleaned up uploaded tar: %s", imagePath)
			}
			return
		}
	}
}
