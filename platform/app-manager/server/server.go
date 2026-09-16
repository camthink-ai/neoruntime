package server

import (
	"bufio"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"os"
	"os/signal"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/containerd/containerd/namespaces"
	"github.com/glebarez/sqlite"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
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
	"aipc/platform/modelload"
	"aipc/platform/platform-api/model"
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

	// Per-app lifecycle serialization. Install/update/start/uninstall mutate the
	// same registry, manifest and app-models tree; different apps remain fully
	// parallel while operations for one ID cannot cross transaction boundaries.
	appMutationMu    sync.Mutex
	appMutationLocks map[string]*sync.Mutex

	// extractModelFile pulls a file out of a containerd image; swappable in
	// tests. Wired to containerd.Client.ExtractFileFromImage in
	// NewAppManagerServer; nil when containerd is unavailable.
	extractModelFile func(ctx context.Context, imageRef, containerPath, destDir string) (string, error)
}

func (s *AppManagerServer) lockAppMutation(appID string) func() {
	s.appMutationMu.Lock()
	if s.appMutationLocks == nil {
		s.appMutationLocks = make(map[string]*sync.Mutex)
	}
	mu := s.appMutationLocks[appID]
	if mu == nil {
		mu = &sync.Mutex{}
		s.appMutationLocks[appID] = mu
	}
	s.appMutationMu.Unlock()
	mu.Lock()
	return mu.Unlock
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
	if containerdClientWrapper != nil {
		server.extractModelFile = containerdClientWrapper.ExtractFileFromImage
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
		st, err := s.StopApp(ctx, stopReq)
		if err != nil {
			return fmt.Errorf("stop app %s: %w", appID, err)
		}
		if st == nil || !st.Success {
			message := "missing stop status"
			if st != nil && st.Message != "" {
				message = st.Message
			}
			return fmt.Errorf("stop app %s was refused: %s", appID, message)
		}
	}

	if appInfo.ContainerID != "" && s.client != nil && s.runtime != nil {
		container, err := s.client.GetContainer(ctx, appInfo.ContainerID)
		if err == nil {
			if task, taskErr := container.Task(ctx, nil); taskErr == nil {
				if stopErr := s.runtime.StopAppContainer(ctx, task, 5); stopErr != nil {
					return fmt.Errorf("stop container during update: %w", stopErr)
				}
			}
			if err := s.runtime.RemoveAppContainer(ctx, container); err != nil {
				return fmt.Errorf("remove container during update: %w", err)
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
	unlockApp := s.lockAppMutation(appManifest.Metadata.ID)
	defer unlockApp()

	// Resolve spec.model dependencies before pulling the image (fail fast):
	// by id first (runtime/platform.db), then by declared bundled path, which
	// is extracted after the image is available.
	resolution, err := s.resolveModelDependencies(ctx, appManifest, nil)
	if err != nil {
		return &proto.InstallResponse{
			Status: &proto.Status{
				Success: false,
				Message: fmt.Sprintf("Model dependency validation failed: %v", err),
				Code:    400,
			},
		}, nil
	}

	// Check duplicate BEFORE pulling image
	appID := appManifest.Metadata.ID
	var isUpdate bool
	var wasRunning bool
	var oldAppInfo *registry.AppInfo
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
		oldAppInfo = existingApp
		wasRunning = existingApp.State == registry.AppStateRunning
		oldWebURL = existingApp.WebURL
		oldRestartCount = existingApp.RestartCount
		oldInstalledAt = existingApp.InstalledAt
		isUpdate = true

		logger.Info("Preparing update of existing app %s (was running=%v)", appID, wasRunning)
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

	// Prepare every bundled package in a sibling tree. Force updates always run
	// this transaction, including an empty replacement (which removes old models).
	var registeredFresh bool
	committed := false
	stoppedExisting := false
	defer func() {
		if !committed && stoppedExisting && wasRunning {
			go func() {
				if st, err := s.StartApp(context.Background(), &proto.StartRequest{AppId: appID}); err != nil || st == nil || !st.Success {
					logger.Error("Rollback: failed to restart previous app %s: rpc=%v status=%v", appID, err, st)
				}
			}()
		}
	}()
	var modelTx *bundledModelTransaction
	if isUpdate || len(resolution.pathPending) > 0 {
		modelTx, err = s.prepareBundledModelTransaction(ctx, appID, appManifest, resolution.pathPending, nil)
		if err != nil {
			return &proto.InstallResponse{Status: &proto.Status{Success: false, Message: fmt.Sprintf("Bundled model preparation failed: %v", err), Code: 400}}, nil
		}
	}

	// All new packages are now verified while the old app/runtime/tree remain
	// intact. Stop the old container before quiescing its registrations;
	// otherwise it can race a new inference into the strict unload window.
	if isUpdate {
		stoppedExisting = true
		if err := s.stopAndCleanupContainer(ctx, appID); err != nil {
			if modelTx != nil {
				modelTx.Rollback(ctx)
			}
			return &proto.InstallResponse{Status: &proto.Status{Success: false, Message: fmt.Sprintf("Failed to stop existing app before update: %v", err), Code: 409}}, nil
		}
	}
	if modelTx != nil {
		if err := modelTx.Publish(ctx); err != nil {
			modelTx.Rollback(ctx)
			return &proto.InstallResponse{Status: &proto.Status{Success: false, Message: fmt.Sprintf("Bundled model publish failed: %v", err), Code: 409}}, nil
		}
	}

	// Same rollback contract as runAsyncInstall for fresh installs: from
	// here until the install is committed, a failure rolls the extracted
	// models back (and undoes a registry entry saved by a later step) —
	// nothing of a failed install survives. Updates keep their artifacts:
	// the registry entry (old or new) owns them and UninstallApp cleans up.
	defer func() {
		if committed {
			return
		}
		if registeredFresh {
			if unErr := s.registry.Unregister(appID); unErr != nil {
				logger.Warn("Rollback: failed to unregister app %s: %v", appID, unErr)
			}
		} else if isUpdate && oldAppInfo != nil {
			if upErr := s.registry.Update(oldAppInfo); upErr != nil {
				logger.Error("Rollback: failed to restore old app registry %s: %v", appID, upErr)
			}
		}
		if modelTx != nil {
			modelTx.Rollback(ctx)
		}
	}()

	// Warn when a shadowed bundled copy differs from the platform copy that
	// won resolution (best-effort, never fails the install).
	s.checkShadowedModels(ctx, appManifest, resolution.shadowed, nil)

	// Snapshot the canonical manifest so a failure after publication can restore
	// the previous app metadata together with the old model tree.
	manifestRollback, err := manifestPublishRollback(appID)
	if err != nil {
		return &proto.InstallResponse{Status: &proto.Status{Success: false, Message: fmt.Sprintf("Failed to snapshot manifest: %v", err), Code: 500}}, nil
	}
	defer func() {
		if !committed {
			manifestRollback()
		}
	}()

	// Store the manifest under the managed manifests root before
	// registering, so cleanup can never target a caller-owned parent
	// directory (CLI tarball unpack dirs, legacy top-level paths).
	canonicalPath, err := canonicalizeManifest(req.ManifestPath, appManifest.Metadata.ID)
	if err != nil {
		return &proto.InstallResponse{
			Status: &proto.Status{
				Success: false,
				Message: fmt.Sprintf("Failed to store manifest: %v", err),
				Code:    500,
			},
		}, nil
	}

	// Create app info with plugin fields
	appInfo := &registry.AppInfo{
		ID:           appManifest.Metadata.ID,
		Name:         appManifest.Metadata.Name,
		Version:      appManifest.Metadata.Version,
		Image:        appManifest.GetNormalizedImage(),
		State:        registry.AppStateInstalled,
		ManifestPath: canonicalPath,
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
		registeredFresh = true
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

	// The install is committed from here: discard the old bundled tree backup.
	committed = true
	if modelTx != nil {
		modelTx.Commit()
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

	// Claim an immutable manifest snapshot before returning a task id. The
	// caller may close/cancel the wizard immediately after acceptance, and its
	// request staging can then be cleaned without racing the background parse.
	manifestData, err := os.ReadFile(req.ManifestPath)
	if err != nil {
		return nil, fmt.Errorf("failed to read manifest before accepting task: %w", err)
	}
	appManifest, err := manifest.ParseManifest(manifestData)
	if err != nil {
		return nil, fmt.Errorf("failed to parse manifest before accepting task: %w", err)
	}
	if err := appManifest.Validate(); err != nil {
		return nil, fmt.Errorf("failed to validate manifest before accepting task: %w", err)
	}

	task := s.taskStore.Create()
	claimed, err := claimAppStaging(task.ID, req.ManifestPath, req.ImagePath)
	if err != nil {
		task.Fail(err.Error())
		return nil, err
	}
	manifestPath, imagePath := claimed[0], claimed[1]
	logger.Info("Created install task: %s", task.ID)
	go s.runAsyncInstallSnapshot(task.ID, manifestData, manifestPath, imagePath, req.Force)

	return &proto.AsyncInstallResponse{TaskId: task.ID}, nil
}

// GetInstallProgress returns the current progress of an async install task.
func (s *AppManagerServer) GetInstallProgress(ctx context.Context, req *proto.InstallProgressRequest) (*proto.InstallProgressResponse, error) {
	task, ok := s.taskStore.Get(req.TaskId)
	if !ok {
		return nil, status.Errorf(codes.NotFound, "task not found: %s", req.TaskId)
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

// runAsyncInstall is the test/legacy wrapper; the production RPC snapshots
// bytes before accepting the task and calls runAsyncInstallSnapshot directly.
func (s *AppManagerServer) runAsyncInstall(taskID, manifestPath, imagePath string, force bool) {
	data, err := os.ReadFile(manifestPath)
	if err != nil {
		if task, ok := s.taskStore.Get(taskID); ok {
			task.Fail(fmt.Sprintf("Failed to load manifest: %v", err))
		}
		return
	}
	s.runAsyncInstallSnapshot(taskID, data, manifestPath, imagePath, force)
}

// runAsyncInstallSnapshot executes the full installation flow from immutable
// manifest bytes owned by the task.
func (s *AppManagerServer) runAsyncInstallSnapshot(taskID string, manifestData []byte, manifestPath, imagePath string, force bool) {
	task, ok := s.taskStore.Get(taskID)
	if !ok {
		return
	}
	defer cleanupOwnedAppStaging(manifestPath, imagePath)

	ctx := namespaces.WithNamespace(context.Background(), s.config.Containerd.Namespace)

	// Phase: validating (0-5%)
	task.Update("validating", 2, "Validating manifest...")
	appManifest, err := manifest.ParseManifest(manifestData)
	if err != nil {
		task.Fail(fmt.Sprintf("Failed to load manifest: %v", err))
		return
	}
	if err := appManifest.Validate(); err != nil {
		task.Fail(fmt.Sprintf("Invalid manifest: %v", err))
		return
	}
	unlockApp := s.lockAppMutation(appManifest.Metadata.ID)
	defer unlockApp()
	if err := security.ValidateSeccompProfile(s.config.Security.SeccompProfile); err != nil {
		task.Fail(fmt.Sprintf("Invalid seccomp profile: %v", err))
		return
	}
	task.Update("validating", 5, "Validation complete")

	// Resolve spec.models dependencies before pulling the image: a missing
	// required model fails here instead of after a multi-hundred-MB pull.
	// Id misses with a declared path are extracted after the image arrives.
	resolution, err := s.resolveModelDependencies(ctx, appManifest, task)
	if err != nil {
		task.Fail(err.Error())
		return
	}

	// Check duplicate BEFORE pulling image. Force update keeps the old registry
	// record until the replacement transaction commits.
	asyncAppID := appManifest.Metadata.ID
	var isUpdate, wasRunning bool
	var oldAppInfo *registry.AppInfo
	if s.registry.Exists(asyncAppID) {
		if !force {
			task.Fail(fmt.Sprintf("App %s already exists. Use force=true to overwrite.", asyncAppID))
			return
		}
		oldAppInfo, _ = s.registry.Get(asyncAppID)
		isUpdate = oldAppInfo != nil
		wasRunning = isUpdate && oldAppInfo.State == registry.AppStateRunning
		task.Update("validating", 4, "Preparing existing app replacement...")
	}

	// Phase: pulling (5-80%)
	// All image references the app needs at start time: spec.image for
	// single-container manifests, one per container for multi-container ones
	// (spec.image is empty there, so GetNormalizedImage() alone cannot be used).
	imageRefs := appManifest.ImageReferences()
	primaryRef := ""
	if len(imageRefs) > 0 {
		primaryRef = imageRefs[0]
	}
	if imagePath != "" && s.client != nil {
		imageName := primaryRef

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

			// Reconcile the tar's true RepoTags against the manifest
			// references and verify every image reference the app needs is
			// now resolvable in containerd. This catches the "tar tag !=
			// manifest image" failure mode at install time instead of letting
			// StartApp fail later (and auto-uninstall on offline devices).
			tarRepoTags := utils.ExtractAllImageNamesFromTar(imagePath)
			tarRepoTag := ""
			if len(tarRepoTags) > 0 {
				tarRepoTag = tarRepoTags[0]
			}
			tarNormalized := manifest.NormalizeImageName(tarRepoTag)
			if tarRepoTag == "" {
				logger.Warn("Install reconcile: could not read RepoTag from tar %s (primary ref=%s)", imagePath, imageName)
			} else if tarNormalized != imageName {
				logger.Warn("Install reconcile: tar RepoTag %q (normalized %q) differs from primary ref %q — retagged on import", tarRepoTag, tarNormalized, imageName)
			} else {
				logger.Info("Install reconcile: tar RepoTag %q matches primary ref %q", tarRepoTag, imageName)
			}
			// docker save can bundle several images in one archive, and the
			// containerd import above brings in every entry under its own
			// archived tag. A non-primary reference the manifest needs is
			// therefore satisfied when the archive carries it (its own image
			// is retagged to the manifest's normalized ref and unpacked —
			// ImportImage only unpacks the first entry) or when it already
			// exists on the device. A reference the archive does not carry
			// used to be retagged from the imported primary image, silently
			// launching that image for a container whose manifest named
			// another one; it now fails the install instead.
			tarTagsByRef := make(map[string]string, len(tarRepoTags)) // normalized ref -> archived tag
			for _, tag := range tarRepoTags {
				if n := manifest.NormalizeImageName(tag); n != "" {
					if _, ok := tarTagsByRef[n]; !ok {
						tarTagsByRef[n] = tag
					}
				}
			}
			for _, ref := range imageRefs {
				if ref == imageName {
					continue
				}
				if _, getErr := s.client.GetImage(ctx, ref); getErr == nil {
					logger.Info("Install reconcile: image %s already present, left untouched", ref)
					continue
				}
				archivedTag, inArchive := tarTagsByRef[ref]
				if !inArchive {
					task.Fail(fmt.Sprintf("Install verify failed: image %s is referenced by the manifest but is neither in the uploaded archive (RepoTags: %v) nor present on the device. Bundle every image the app needs into the archive (docker save <image1> <image2> ... -o app.tar) or pre-install the missing image.", ref, tarRepoTags))
					return
				}
				if archivedTag != ref {
					if tagErr := s.client.TagImage(ctx, archivedTag, ref); tagErr != nil {
						task.Fail(fmt.Sprintf("Install verify failed: image %s could not be retagged from its archived tag %s. Error: %v", ref, archivedTag, tagErr))
						return
					}
					logger.Info("Retagged imported image %s -> %s (container image)", archivedTag, ref)
				}
				if unErr := s.client.EnsureImageUnpacked(ctx, ref); unErr != nil {
					task.Fail(fmt.Sprintf("Install verify failed: image %s could not be prepared after import. Error: %v", ref, unErr))
					return
				}
			}
			for _, ref := range imageRefs {
				if _, verifyErr := s.client.GetImage(ctx, ref); verifyErr != nil {
					task.Fail(fmt.Sprintf("Install verify failed: image %s is not resolvable after import. tar RepoTags: %v. Error: %v", ref, tarRepoTags, verifyErr))
					return
				}
			}

			// Save tar file for self-healing recovery after power loss
			if saveErr := containerd.SaveImageTar(appManifest.Metadata.ID, imagePath); saveErr != nil {
				logger.Warn("Failed to save image tar for recovery: %v", saveErr)
			}

			// Clean up uploaded tar file after successful import
			s.cleanupUploadedTar(imagePath)
		}
	} else if s.client != nil && len(imageRefs) > 0 {
		// No local tar was uploaded. Before proceeding, verify every image
		// reference (spec.image, or one per container) is already resolvable
		// in containerd (e.g. reinstall of an app whose image was imported
		// previously). If one is not present, fail fast: an offline device
		// cannot pull it, so StartApp would fail later and the app would
		// auto-uninstall — leaving the user with no error signal that the
		// image was never provided.
		for _, ref := range imageRefs {
			if _, verifyErr := s.client.GetImage(ctx, ref); verifyErr != nil {
				task.Fail(fmt.Sprintf("No image tar was uploaded and image %q is not present in containerd. An offline device cannot pull it — re-import the app together with its image tar, or ensure the image already exists on the device. Error: %v", ref, verifyErr))
				return
			}
			logger.Info("No image tar provided; image %s already present in containerd", ref)
		}
		task.Update("pulling", 80, "Image already present")
	} else {
		task.Update("pulling", 80, "No image to pull")
	}

	var modelTx *bundledModelTransaction
	committed := false
	stoppedExisting := false
	defer func() {
		if !committed && stoppedExisting && wasRunning {
			go func() {
				if st, err := s.StartApp(context.Background(), &proto.StartRequest{AppId: asyncAppID}); err != nil || st == nil || !st.Success {
					logger.Error("Rollback: failed to restart previous app %s: rpc=%v status=%v", asyncAppID, err, st)
				}
			}()
		}
	}()
	if isUpdate || len(resolution.pathPending) > 0 {
		modelTx, err = s.prepareBundledModelTransaction(ctx, asyncAppID, appManifest, resolution.pathPending, task)
		if err != nil {
			task.Fail(fmt.Sprintf("Bundled model preparation failed: %v", err))
			return
		}
	}
	if isUpdate {
		stoppedExisting = true
		if err := s.stopAndCleanupContainer(ctx, asyncAppID); err != nil {
			if modelTx != nil {
				modelTx.Rollback(ctx)
			}
			task.Fail(fmt.Sprintf("Failed to stop existing app before update: %v", err))
			return
		}
	}
	if modelTx != nil {
		if err := modelTx.Publish(ctx); err != nil {
			modelTx.Rollback(ctx)
			task.Fail(fmt.Sprintf("Bundled model publish failed: %v", err))
			return
		}
	}

	// From here until commit, restore the old tree/runtime registrations on any
	// manifest, registry, or instance failure.
	// until the install is committed, every later failure (manifest storage,
	// registry save, instance dir) must roll the extracted models back too —
	// the transient registrations and <root>/app-models/<app_id> live outside
	// the app registry, and a failed install never runs UninstallApp for
	// them. A registry entry saved before a later failure is unregistered as
	// well: an app without its instance dir is half-installed.
	var registeredApp *registry.AppInfo
	defer func() {
		if committed {
			return
		}
		if registeredApp != nil {
			if isUpdate && oldAppInfo != nil {
				if upErr := s.registry.Update(oldAppInfo); upErr != nil {
					logger.Error("Rollback: failed to restore old app registry %s: %v", oldAppInfo.ID, upErr)
				}
			} else if unErr := s.registry.Unregister(registeredApp.ID); unErr != nil {
				logger.Warn("Rollback: failed to unregister app %s: %v", registeredApp.ID, unErr)
			}
		}
		if modelTx != nil {
			modelTx.Rollback(ctx)
		}
	}()

	// Warn when a shadowed bundled copy differs from the platform copy that
	// won resolution (best-effort, never fails the install).
	s.checkShadowedModels(ctx, appManifest, resolution.shadowed, task)

	// Phase: registering (80-95%)
	task.Update("registering", 85, "Registering application...")

	manifestRollback, err := manifestPublishRollback(asyncAppID)
	if err != nil {
		task.Fail(fmt.Sprintf("Failed to snapshot manifest: %v", err))
		return
	}
	defer func() {
		if !committed {
			manifestRollback()
		}
	}()

	// Store the manifest under the managed manifests root before
	// registering, so cleanup can never target a caller-owned parent
	// directory (CLI tarball unpack dirs, legacy top-level paths).
	canonicalPath, err := canonicalizeManifestBytes(manifestData, manifestPath, appManifest.Metadata.ID)
	if err != nil {
		task.Fail(fmt.Sprintf("Failed to store manifest: %v", err))
		return
	}

	appInfo := &registry.AppInfo{
		ID:           appManifest.Metadata.ID,
		Name:         appManifest.Metadata.Name,
		Version:      appManifest.Metadata.Version,
		Image:        appManifest.GetNormalizedImage(),
		State:        registry.AppStateInstalled,
		ManifestPath: canonicalPath,
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
	if isUpdate && oldAppInfo != nil {
		appInfo.WebURL = oldAppInfo.WebURL
		appInfo.RestartCount = oldAppInfo.RestartCount
		appInfo.InstalledAt = oldAppInfo.InstalledAt
	}

	task.Update("registering", 90, "Saving app registry...")
	if isUpdate {
		err = s.registry.Update(appInfo)
	} else {
		err = s.registry.Register(appInfo)
	}
	if err != nil {
		task.Fail(fmt.Sprintf("Failed to save app registry: %v", err))
		return
	}
	registeredApp = appInfo

	// Create instance directory
	if err := os.MkdirAll(appInfo.InstancePath, 0755); err != nil {
		task.Fail(fmt.Sprintf("Failed to create instance directory: %v", err))
		return
	}

	task.Update("registering", 95, "Registering permissions...")
	if err := s.registerAppPermissions(ctx, appInfo.ID, appManifest); err != nil {
		logger.Warn("Failed to register app permissions: %v", err)
	}

	// The install is committed from here: discard the old bundled tree backup.
	committed = true
	if modelTx != nil {
		modelTx.Commit()
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

	logger.Info("Async install complete: %s", appInfo.ID)

	if isUpdate && wasRunning {
		go func() {
			if _, err := s.StartApp(context.Background(), &proto.StartRequest{AppId: appInfo.ID}); err != nil {
				logger.Warn("Failed to auto-restart updated app %s: %v", appInfo.ID, err)
			}
		}()
	}

	// Phase: complete (100%)
	task.Complete(appInfo.ID)
}

// StartApp implements AppManager.StartApp
func (s *AppManagerServer) StartApp(ctx context.Context, req *proto.StartRequest) (*proto.Status, error) {
	logger.Info("StartApp called: app_id=%s", req.AppId)
	unlockApp := s.lockAppMutation(req.AppId)
	defer unlockApp()

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
		// Preload required models before creating the container. A logical
		// registration refusal (including an id/path/config collision) must
		// stop the start — otherwise the app launches against no model or
		// somebody else's incumbent registration.
		if err := s.PreloadModels(ctx, appID, appManifest); err != nil {
			return &proto.Status{
				Success: false,
				Message: "Required model preload failed: " + err.Error(),
				Code:    412,
			}, nil
		}

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

	// Preload required models before touching the existing multi-container
	// instance. A failed restore leaves the currently running instance alone
	// instead of tearing it down and then discovering the replacement cannot
	// infer.
	if err := s.PreloadModels(ctx, appID, appManifest); err != nil {
		return &proto.Status{
			Success: false,
			Message: "Required model preload failed: " + err.Error(),
			Code:    412,
		}, nil
	}

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
	unlockApp := s.lockAppMutation(req.AppId)
	defer unlockApp()
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

	// Remove the manifest. The app's own directory under the managed
	// manifests root is removed with it; a manifest registered from
	// anywhere else (legacy installs at the install-root top level, CLI
	// installs from unpacked tarballs) gets only the file removed — never
	// a parent directory the platform does not own.
	if appInfo.ManifestPath != "" {
		if manifestDir, ok := managedManifestDir(appInfo.ManifestPath, appInfo.ID); ok {
			if err := os.RemoveAll(manifestDir); err != nil {
				logger.Warn("Failed to remove manifest dir %s: %v", manifestDir, err)
			}
		} else if err := os.Remove(appInfo.ManifestPath); err != nil && !os.IsNotExist(err) {
			logger.Warn("Failed to remove manifest file %s: %v", appInfo.ManifestPath, err)
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

// getModelMeta retrieves the full model row from platform.db: the load-time
// composition needs output_mode/config/threshold/max_detections beyond the
// three columns this used to select. Columns missing from an older
// platform-api schema scan as zero values, which the composition degrades
// gracefully (e.g. ResolveOutputMode("") → platform mode).
func (s *AppManagerServer) getModelMeta(modelID string) *model.AIModel {
	if s.db == nil {
		return nil
	}
	var meta model.AIModel
	result := s.db.Table("ai_models").
		Where("model_id = ?", modelID).
		Scan(&meta)
	if result.Error != nil || meta.FilePath == "" {
		return nil
	}
	return &meta
}

// modelRef pairs a spec.models entry with its alias; sorted by alias to keep
// group membership and error/warning order deterministic across Go's
// randomized map iteration.
type modelRef struct {
	alias   string
	mapping manifest.ModelMapping
}

// pendingBundledModel is a spec.models entry whose id was not found on the
// device but which declared a bundled image path: the AMPK package gets
// extracted from the app image once it is available, unpacked and registered
// as a transient model.
type pendingBundledModel struct {
	alias    string
	id       string
	path     string // absolute container path of the .bin package inside the app image
	required bool
	request  *inferencepb.ModelRegisterRequest
}

// shadowedBundledModel is a spec.models entry whose id was found on the
// platform (runtime or platform.db) even though a bundled image path was
// also declared: the platform copy wins resolution, so the bundled copy is
// never used. checkShadowedModels compares both files' hashes to warn when
// the ignored bundled version differs from the one that will actually run.
type shadowedBundledModel struct {
	alias        string
	id           string
	path         string // absolute container path inside the app image
	platformPath string // host path of the platform copy that won resolution
}

// modelResolution is the outcome of resolving spec.models against the device.
type modelResolution struct {
	resolved    []string               // ids satisfied by the platform (runtime or platform.db)
	pathPending []pendingBundledModel  // id misses with a declared path, to extract from the image
	shadowed    []shadowedBundledModel // id hits with a declared path: platform copy wins, compare hashes
}

// resolveModelDependencies resolves spec.models per the resolution chain
// id → bundled path → report: an id counts as found when ai-runtime has it
// loaded OR platform.db knows it (PreloadModels registers db models at app
// start, so a db hit must not fail the install). Id hits that also declared
// a path are recorded as shadowed — the platform copy wins, and the bundled
// copy gets a best-effort hash comparison later (checkShadowedModels). Id
// misses with a declared path become pathPending, extracted by
// extractImageModels once the image is in containerd. Ids found by neither
// are reported immediately: missing
// required models fail the install with one combined error; missing optional
// ones surface as warnings (task progress message + log). When ai-runtime is
// unreachable db hits still count as resolved; the rest cannot be verified:
// required models fail, optional ones warn with the reason. task may be nil
// (sync InstallApp path). Call before image pull so a missing model with no
// fallback fails fast. Uses a single ListModels call (no per-id N+1).
func (s *AppManagerServer) resolveModelDependencies(ctx context.Context, appManifest *manifest.AppManifest, task *InstallTask) (*modelResolution, error) {
	res := &modelResolution{}
	if appManifest == nil || len(appManifest.Spec.Models) == 0 {
		return res, nil
	}

	aliases := make([]string, 0, len(appManifest.Spec.Models))
	for alias := range appManifest.Spec.Models {
		aliases = append(aliases, alias)
	}
	sort.Strings(aliases)
	refs := make([]modelRef, 0, len(aliases))
	byID := make(map[string]int, len(aliases))
	for _, alias := range aliases {
		mapping := appManifest.Spec.Models[alias]
		if i, ok := byID[mapping.ID]; ok {
			if refs[i].mapping.Path != mapping.Path {
				return res, fmt.Errorf("model id %q has conflicting bundled paths", mapping.ID)
			}
			refs[i].mapping.Required = refs[i].mapping.Required || mapping.Required
			continue
		}
		byID[mapping.ID] = len(refs)
		refs = append(refs, modelRef{alias: alias, mapping: mapping})
	}

	// classify sorts each ref into resolved / pathPending / reported-missing.
	// runtimeUp is false when the loaded set could not be queried, in which
	// case only platform.db can confirm an id and unconfirmed ones report as
	// "cannot be verified" rather than flat-out missing.
	classify := func(runtimeUp bool, loaded map[string]*inferencepb.ModelInfo, unavailable string) error {
		var requiredErrs, warnings []string
		for _, ref := range refs {
			// Membership in the runtime's loaded set is the hit; its
			// ModelPath (when present) feeds the shadowed-hash comparison.
			var info *inferencepb.ModelInfo
			runtimeHit := false
			if runtimeUp {
				info, runtimeHit = loaded[ref.mapping.ID]
			}
			// But only a durable runtime entry resolves a dependency: this
			// install must keep working after a reboot and when other apps
			// come and go. Transient registrations are another app's
			// bundled models — PreloadModels can neither restore them (no
			// platform.db row, no bundled fallback for this app) nor keep
			// them alive (uninstalling the owner removes them) — and
			// app-owned non-transient entries share that fragility. Such
			// hits fall through to the platform.db lookup, this app's own
			// bundled path, or the missing-model report instead of
			// silently passing validation.
			if runtimeHit && (info.GetTransient() || !isRuntimeSystemOwner(info.GetOwnerId())) {
				runtimeHit = false
			}
			platformPath := ""
			if runtimeHit {
				platformPath = info.ModelPath
			}
			if platformPath == "" {
				if meta := s.getModelMeta(ref.mapping.ID); meta != nil {
					platformPath = meta.FilePath
				}
			}
			if runtimeHit || platformPath != "" {
				res.resolved = append(res.resolved, ref.mapping.ID)
				if ref.mapping.Path != "" && platformPath != "" {
					res.shadowed = append(res.shadowed, shadowedBundledModel{
						alias:        ref.alias,
						id:           ref.mapping.ID,
						path:         ref.mapping.Path,
						platformPath: platformPath,
					})
				}
				continue
			}
			if ref.mapping.Path != "" {
				res.pathPending = append(res.pathPending, pendingBundledModel{
					alias:    ref.alias,
					id:       ref.mapping.ID,
					path:     ref.mapping.Path,
					required: ref.mapping.Required,
				})
				continue
			}
			entry := fmt.Sprintf("%q (alias %q)", ref.mapping.ID, ref.alias)
			if runtimeUp {
				if ref.mapping.Required {
					requiredErrs = append(requiredErrs, fmt.Sprintf("required model %s is not available on the device and no bundled path is declared", entry))
				} else {
					warnings = append(warnings, fmt.Sprintf("optional model %s is not available on the device and no bundled path is declared", entry))
				}
			} else {
				if ref.mapping.Required {
					requiredErrs = append(requiredErrs, fmt.Sprintf("required model %s cannot be verified (%s)", entry, unavailable))
				} else {
					warnings = append(warnings, fmt.Sprintf("optional model %s cannot be verified (%s)", entry, unavailable))
				}
			}
		}
		return reportModelValidation(requiredErrs, warnings, task)
	}

	s.aiRuntimeMutex.RLock()
	client := s.aiRuntimeClient
	s.aiRuntimeMutex.RUnlock()
	if client == nil || !s.config.AIRuntime.Enabled {
		return res, classify(false, nil, "ai-runtime is not available")
	}
	resp, err := client.ListModels(ctx, &inferencepb.Empty{})
	if err != nil {
		return res, classify(false, nil, fmt.Sprintf("ai-runtime list models failed: %v", err))
	}
	loaded := make(map[string]*inferencepb.ModelInfo, len(resp.Models))
	for _, m := range resp.Models {
		loaded[m.ModelId] = m
	}
	return res, classify(true, loaded, "")
}

// isRuntimeSystemOwner reports whether a runtime registration owner denotes
// the platform itself rather than an app. grpc_service.cpp normalizes
// ownerless registrations to "<system>" before storing, so both spellings
// count (tests seed raw gRPC shapes with "").
func isRuntimeSystemOwner(owner string) bool {
	return owner == "" || owner == systemOwnerMarker
}

// systemOwnerMarker mirrors AIRuntimeServiceImpl's owner normalization
// (platform-api's systemOwnerID); duplicated here so app-manager does not
// import platform-api internals.
const systemOwnerMarker = "<system>"

// appModelsDir is where bundled models unpacked from app images live:
// {RootPath}/app-models/<app_id>/<alias>/ holding the extracted HEF plus its
// registration.json sidecar (the .bin package itself is deleted after unpack).
// Reinstalling an app overwrites its directory, making unpacking idempotent.
func appModelsDir(appID string) string {
	return filepath.Join(constants.RootPath(), "app-models", appID)
}

// extractImageModels unpacks path-pending bundled models — AMPK .bin packages
// declared in spec.models — out of the app image (now present in containerd)
// and registers them with ai-runtime as transient models: usable by the app,
// invisible on the model page. Each package is digest-verified, its HEF
// staged beside a registration.json sidecar (the durable record PreloadModels
// restores from at every reboot), and registered with the composed variant;
// the .bin itself is deleted after unpack. Fresh detection registrations get
// a postprocess smoke test — a profile/HEF mismatch would otherwise surface
// as per-frame infer failures once the app runs — and a failure rolls that
// registration back and removes the unpacked files. Called on both install
// paths after image availability and before the app is registered. A failing
// required model aborts the install via the returned error; optional failures
// warn and continue. task may be nil (sync InstallApp path). On failure,
// models registered by this call are unregistered again so a failed install
// leaves nothing behind (the app is never registered, so UninstallApp would
// not run for it).
func (s *AppManagerServer) extractImageModels(ctx context.Context, appID string, appManifest *manifest.AppManifest, pending []pendingBundledModel, task *InstallTask) error {
	if len(pending) == 0 {
		return nil
	}

	// Path safety precedes everything else: the app ID and model aliases
	// become directory names under the data root, and this runs before
	// canonicalizeManifest (the install step that rejects unsafe IDs).
	if err := requireSafePathSegment("app id", appID); err != nil {
		return err
	}
	for _, p := range pending {
		if err := requireSafePathSegment("model alias", p.alias); err != nil {
			return err
		}
	}

	s.aiRuntimeMutex.RLock()
	client := s.aiRuntimeClient
	s.aiRuntimeMutex.RUnlock()
	extractFn := s.extractModelFile
	if client == nil || !s.config.AIRuntime.Enabled || extractFn == nil {
		reason := "ai-runtime is not available"
		if client != nil && s.config.AIRuntime.Enabled {
			reason = "containerd is not available"
		}
		var requiredErrs, warnings []string
		for _, p := range pending {
			if p.required {
				requiredErrs = append(requiredErrs, fmt.Sprintf("required model %q (alias %q) cannot be extracted (%s)", p.id, p.alias, reason))
			} else {
				warnings = append(warnings, fmt.Sprintf("optional model %q (alias %q) cannot be extracted (%s)", p.id, p.alias, reason))
			}
		}
		return reportModelValidationAt("registering", 82, requiredErrs, warnings, task)
	}

	// Bundled packages are extracted from the primary image (spec.image, or
	// the main container's image for multi-container apps).
	imageRefs := appManifest.ImageReferences()
	imageRef := ""
	if len(imageRefs) > 0 {
		imageRef = imageRefs[0]
	}

	var requiredErrs, warnings []string
	registered := make([]string, 0, len(pending)) // ids this call registered; rolled back on failure
	baseDir := appModelsDir(appID)
	for _, p := range pending {
		if task != nil {
			task.Update("registering", 82, fmt.Sprintf("Extracting bundled model %q (alias %q)...", p.id, p.alias))
		}
		fail := func(format string, args ...any) {
			msg := fmt.Sprintf(format, args...)
			entry := fmt.Sprintf("model %q (alias %q)", p.id, p.alias)
			if p.required {
				requiredErrs = append(requiredErrs, fmt.Sprintf("required %s: %s", entry, msg))
			} else {
				warnings = append(warnings, fmt.Sprintf("optional %s: %s", entry, msg))
			}
		}
		if imageRef == "" {
			fail("the app declares no image to extract from")
			continue
		}
		aliasDir := filepath.Join(baseDir, p.alias)
		binPath, err := extractFn(ctx, imageRef, p.path, aliasDir)
		if err != nil {
			fail("extract %q from image %s failed: %v", p.path, imageRef, err)
			continue
		}
		reg, unpackErr := unpackBundledPackage(binPath, aliasDir, p.id)
		os.Remove(binPath) // the .bin is transport only; HEF + sidecar remain
		if unpackErr != nil {
			fail("unpacking bundled package %s failed: %v", p.path, unpackErr)
			continue
		}
		hefPath := filepath.Join(aliasDir, reg.HEF)
		regResp, err := client.RegisterModel(ctx, &inferencepb.ModelRegisterRequest{
			ModelId:       p.id,
			ModelPath:     hefPath,
			OwnerId:       appID,
			ModelType:     reg.ModelType,
			ModelVariant:  reg.ModelVariant,
			Transient:     true,
			RawOutputOnly: reg.RawOutputOnly,
		})
		if err != nil {
			fail("registering unpacked model at %s failed: %v", hefPath, err)
			continue
		}
		if regResp.Status != nil && !regResp.Status.Success {
			fail("registering unpacked model at %s failed: %s", hefPath, regResp.Status.Message)
			continue
		}
		if reg.ModelType == "detection" {
			if smokeErr := s.probeFreshRegistration(ctx, client, appID, p.id); smokeErr != nil {
				fail("postprocess smoke test failed (%v); registration rolled back", smokeErr)
				// The registration is gone; drop the unpacked files too so a
				// later PreloadModels cannot resurrect a known-broken model.
				if rmErr := os.RemoveAll(aliasDir); rmErr != nil {
					logger.Warn("Failed to remove unpacked files for failed model %s: %v", p.id, rmErr)
				}
				continue
			}
		}
		registered = append(registered, p.id)
		logger.Info("Unpacked and registered bundled model %q (alias %q) from %s%s for app %s (transient)",
			p.id, p.alias, imageRef, p.path, appID)
	}

	err := reportModelValidationAt("registering", 82, requiredErrs, warnings, task)
	if err != nil {
		s.rollbackBundledModels(ctx, appID, registered)
	}
	return err
}

// rollbackBundledModels undoes a (possibly partial) extractImageModels run:
// transient registrations are released and the unpack dir is removed, so a
// failed install leaves nothing behind (the app is never registered on this
// path, so UninstallApp would not run for it). The dir removal runs even
// with no registrations to release — extracted files can outlive a
// registration that already rolled itself back. Best-effort and idempotent:
// unregistering with OwnerId set only releases this app's ownership (a
// never-owned id is a no-op at the runtime), and RemoveAll on a missing
// directory succeeds.
func (s *AppManagerServer) rollbackBundledModels(ctx context.Context, appID string, modelIDs []string) {
	s.aiRuntimeMutex.RLock()
	client := s.aiRuntimeClient
	s.aiRuntimeMutex.RUnlock()
	if client != nil {
		for _, id := range modelIDs {
			if _, unErr := client.UnregisterModel(ctx, &inferencepb.ModelInfo{ModelId: id, OwnerId: appID}); unErr != nil {
				logger.Warn("Rollback: failed to unregister model %s for app %s: %v", id, appID, unErr)
			}
		}
	}
	if rmErr := os.RemoveAll(appModelsDir(appID)); rmErr != nil {
		logger.Warn("Rollback: failed to remove %s: %v", appModelsDir(appID), rmErr)
	}
}

// reportModelValidation turns collected per-model errors/warnings into the
// install result: warnings always log and, when a task is attached, replace
// its progress message; errors combine into a single returned error. Progress
// stays in the validating phase (see reportModelValidationAt for other
// phases).
func reportModelValidation(requiredErrs, warnings []string, task *InstallTask) error {
	return reportModelValidationAt("validating", 5, requiredErrs, warnings, task)
}

// reportModelValidationAt is reportModelValidation for install stages past
// validation (e.g. bundled-model extraction reports at registering/82 so the
// task never moves backwards).
func reportModelValidationAt(phase string, pct float64, requiredErrs, warnings []string, task *InstallTask) error {
	if len(warnings) > 0 {
		msg := "Warning: " + strings.Join(warnings, "; ")
		logger.Warn("%s", msg)
		if task != nil {
			task.Update(phase, pct, msg)
		}
	}
	if len(requiredErrs) > 0 {
		return fmt.Errorf("%s", strings.Join(requiredErrs, "; "))
	}
	return nil
}

// fileSHA256 streams a file through sha256 and returns the hex digest.
// Always computed fresh from disk, never read back from a db hash column —
// a stale stored hash would silently bless a mismatched pair.
func fileSHA256(path string) (string, error) {
	f, err := os.Open(path)
	if err != nil {
		return "", err
	}
	defer f.Close()
	h := sha256.New()
	if _, err := io.Copy(h, f); err != nil {
		return "", err
	}
	return hex.EncodeToString(h.Sum(nil)), nil
}

// checkShadowedModels hardens the shadowing case: spec.models entries whose
// id resolved from the platform while the image also bundles a copy. The
// platform copy wins resolution, so a differing bundled version is silently
// ignored — hash the platform file against the bundled package's inner HEF
// and warn "平台已有同 id 模型，镜像内版本被忽略" on mismatch so the operator
// knows the app is not running its bundled version. Purely informational:
// any failure to read or hash either side (platform file gone, package absent
// from the image, digest mismatch, containerd unavailable) skips that entry
// via a debug log instead of failing the install. The extracted .bin lands
// in a temp dir that is always removed — nothing is registered and nothing
// persists under app-models. Runs on both install paths after the image is
// in containerd; task may be nil (sync path).
func (s *AppManagerServer) checkShadowedModels(ctx context.Context, appManifest *manifest.AppManifest, shadowed []shadowedBundledModel, task *InstallTask) {
	if len(shadowed) == 0 || appManifest == nil {
		return
	}
	extractFn := s.extractModelFile
	if extractFn == nil {
		logger.Debug("Skipping shadowed-model hash comparison for %d entries (containerd extraction unavailable)", len(shadowed))
		return
	}
	imageRefs := appManifest.ImageReferences()
	if len(imageRefs) == 0 {
		return
	}
	imageRef := imageRefs[0]

	var warnings []string
	for _, m := range shadowed {
		platformHash, err := fileSHA256(m.platformPath)
		if err != nil {
			logger.Debug("Shadowed model %q (alias %q): platform copy %s unreadable (%v), skipping hash comparison", m.id, m.alias, m.platformPath, err)
			continue
		}
		tmpDir, err := os.MkdirTemp("", "aipc-shadowed-")
		if err != nil {
			logger.Debug("Shadowed model %q (alias %q): temp dir unavailable (%v), skipping hash comparison", m.id, m.alias, err)
			continue
		}
		extractedPath, err := extractFn(ctx, imageRef, m.path, tmpDir)
		if err != nil {
			os.RemoveAll(tmpDir)
			logger.Debug("Shadowed model %q (alias %q): bundled copy at %s unreadable (%v), skipping hash comparison", m.id, m.alias, m.path, err)
			continue
		}
		// The bundled copy is an AMPK .bin: hash the inner HEF (digest-verified
		// while streaming) so it compares against the platform's HEF hash — the
		// package container itself would never match.
		imageHash, hashErr := bundledPackageHEFHash(extractedPath)
		os.RemoveAll(tmpDir)
		if hashErr != nil {
			logger.Debug("Shadowed model %q (alias %q): bundled package %s unreadable (%v), skipping hash comparison", m.id, m.alias, extractedPath, hashErr)
			continue
		}
		if imageHash == platformHash {
			logger.Info("Shadowed model %q (alias %q): bundled copy matches the platform copy at %s", m.id, m.alias, m.platformPath)
			continue
		}
		warnings = append(warnings, fmt.Sprintf(
			"model %q (alias %q): 平台已有同 id 模型，镜像内版本被忽略 (bundled copy at %s differs from platform copy at %s)",
			m.id, m.alias, m.path, m.platformPath))
	}
	if len(warnings) > 0 {
		msg := "Warning: " + strings.Join(warnings, "; ")
		logger.Warn("%s", msg)
		if task != nil {
			task.Update("registering", 82, msg)
		}
	}
}

// PreloadModels registers the models the app depends on with ai-runtime:
// platform models from platform.db, and bundled models from the HEF +
// registration.json sidecar unpacked from the app image's AMPK .bin packages
// at install time (restored e.g. after a device reboot, where the runtime
// lost the registration but the unpacked files survived). Registering an
// already-loaded model adds co-ownership, so repeated starts are safe.
func (s *AppManagerServer) PreloadModels(ctx context.Context, appID string, appManifest *manifest.AppManifest) error {
	s.aiRuntimeMutex.RLock()
	client := s.aiRuntimeClient
	s.aiRuntimeMutex.RUnlock()
	if client == nil || !s.config.AIRuntime.Enabled {
		return nil
	}

	if appManifest == nil || len(appManifest.Spec.Permissions.Inference.Models) == 0 {
		return nil
	}

	// Legacy permissions.inference.models entries have no optional bit and
	// are therefore required. spec.models entries carry Required explicitly;
	// optional restore failures remain warnings, while a required model must
	// veto StartApp rather than launch a container that cannot infer.
	requiredByID := make(map[string]bool, len(appManifest.Spec.Models))
	for _, mapping := range appManifest.Spec.Models {
		requiredByID[mapping.ID] = requiredByID[mapping.ID] || mapping.Required
	}
	var requiredErrs []string
	recordFailure := func(modelID, message string) {
		required, declared := requiredByID[modelID]
		if !declared || required {
			requiredErrs = append(requiredErrs, message)
			logger.Error("%s", message)
		} else {
			logger.Warn("Optional %s", message)
		}
	}

	// Bundled (path-declared) mappings by model id, for the platform.db-miss
	// fallback: the alias locates the unpack dir whose registration.json sidecar
	// holds the composed registration from install time (the .bin was deleted
	// after unpack, so the type/variant cannot be re-derived). First alias per
	// id wins; spec.models is tiny so determinism only matters for logging.
	bundled := make(map[string]string, len(appManifest.Spec.Models))
	aliases := make([]string, 0, len(appManifest.Spec.Models))
	for alias := range appManifest.Spec.Models {
		aliases = append(aliases, alias)
	}
	sort.Strings(aliases)
	for _, alias := range aliases {
		mapping := appManifest.Spec.Models[alias]
		if mapping.Path == "" {
			continue
		}
		if _, exists := bundled[mapping.ID]; !exists {
			bundled[mapping.ID] = alias
		}
	}

	// The runtime's current registrations: a model someone else already
	// loaded only gains this app as a co-owner here (path and variant stay
	// as registered), so preexisting entries are neither probed nor rolled
	// back — only registrations this preload actually creates are ours to
	// verify.
	preexisting := make(map[string]bool)
	if resp, err := client.ListModels(ctx, &inferencepb.Empty{}); err != nil {
		logger.Warn("Failed to list runtime models before preloading for app %s: %v", appID, err)
	} else {
		for _, m := range resp.Models {
			preexisting[m.ModelId] = true
		}
	}

	seenModelIDs := make(map[string]bool, len(appManifest.Spec.Permissions.Inference.Models))
	for _, modelID := range appManifest.Spec.Permissions.Inference.Models {
		if seenModelIDs[modelID] {
			continue
		}
		seenModelIDs[modelID] = true
		if meta := s.getModelMeta(modelID); meta != nil {
			if err := s.preloadPlatformModel(ctx, client, appID, modelID, meta, !preexisting[modelID]); err != nil {
				recordFailure(modelID, fmt.Sprintf("failed to preload model %s for app %s: %v", modelID, appID, err))
			}
			continue
		}
		if alias, ok := bundled[modelID]; ok {
			aliasDir := filepath.Join(appModelsDir(appID), alias)
			reg, regErr := loadBundledRegistration(aliasDir)
			if regErr != nil {
				recordFailure(modelID, fmt.Sprintf("app %s declares bundled model %s (alias %q), but its unpack record is unreadable (reinstall the app): %v",
					appID, modelID, alias, regErr))
				continue
			}
			hefPath := filepath.Join(aliasDir, reg.HEF)
			if _, statErr := os.Stat(hefPath); statErr != nil {
				recordFailure(modelID, fmt.Sprintf("app %s declares bundled model %s, but the unpacked file %s is missing (reinstall the app): %v",
					appID, modelID, hefPath, statErr))
				continue
			}
			regResp, err := client.RegisterModel(ctx, &inferencepb.ModelRegisterRequest{
				ModelId:       modelID,
				ModelPath:     hefPath,
				OwnerId:       appID,
				ModelType:     reg.ModelType,
				ModelVariant:  reg.ModelVariant,
				Transient:     true,
				RawOutputOnly: reg.RawOutputOnly,
			})
			if err != nil {
				recordFailure(modelID, fmt.Sprintf("failed to restore bundled model %s for app %s: %v", modelID, appID, err))
				continue
			}
			// Same refusal shape as preloadPlatformModel: a collision with
			// another registration under this id (different path or variant)
			// or a postprocess init failure answers success=false over an OK
			// transport status. Logging "restored" and probing anyway would
			// start the container against somebody else's weights.
			if regResp != nil && regResp.Status != nil && !regResp.Status.Success {
				recordFailure(modelID, fmt.Sprintf("restore of bundled model %s for app %s was refused: %s (another registration under this id wins; stop the owning app or reinstall with a unique model id)",
					modelID, appID, regResp.Status.Message))
				continue
			}
			logger.Info("Restored bundled model %s (path: %s, type: %s) for app %s (transient)", modelID, hefPath, reg.ModelType, appID)
			// Fresh detection restore gets the same install-time smoke test:
			// the HEF and sidecar are kept either way (the next start retries;
			// only reinstall rewrites them), but a known-broken registration
			// is not left behind for the app to infer against.
			if !preexisting[modelID] && reg.ModelType == "detection" {
				if err := s.probeFreshRegistration(ctx, client, appID, modelID); err != nil {
					recordFailure(modelID, fmt.Sprintf("restored bundled model %s for app %s failed its postprocess smoke test: %v", modelID, appID, err))
				}
			}
			continue
		}
		recordFailure(modelID, fmt.Sprintf("app %s requires model %s, but it is neither in platform.db nor bundled in the app image", appID, modelID))
	}
	if len(requiredErrs) > 0 {
		return fmt.Errorf("%s", strings.Join(requiredErrs, "; "))
	}
	return nil
}

// preloadPlatformModel registers a platform.db model with ai-runtime through
// the shared load-time composition, so a preloaded model behaves exactly like
// one loaded via platform-api: detection models are materialized under a
// plugin-recognized basename with a full-key variant blob, raw-output models
// skip the postprocess session entirely. A freshly registered detection
// model gets a load smoke test — a postprocess mismatch would otherwise
// surface only as per-frame infer failures once the app is running — and the
// registration is rolled back on failure with an Error log, since the app
// cannot work without this model either way.
func (s *AppManagerServer) preloadPlatformModel(ctx context.Context, client inferencepb.InferenceServiceClient, appID, modelID string, meta *model.AIModel, fresh bool) error {
	path, variant, grpcType, err := modelload.RuntimeRegistration(meta)
	if err != nil {
		return fmt.Errorf("compose runtime registration: %w", err)
	}
	regResp, err := client.RegisterModel(ctx, &inferencepb.ModelRegisterRequest{
		ModelId:      modelID,
		ModelPath:    path,
		OwnerId:      appID,
		ModelType:    grpcType,
		ModelVariant: variant,
	})
	if err != nil {
		return fmt.Errorf("register with ai-runtime: %w", err)
	}
	// The runtime reports logical refusals as OK transport status with
	// success=false — e.g. the same id registered from a different path
	// (another app's bundled model won the race), or this variant conflicts
	// with the incumbent. Treating that as restored would start the app's
	// container against a registration that is not this model.
	if regResp != nil && regResp.Status != nil && !regResp.Status.Success {
		return fmt.Errorf("runtime refused registration: %s", regResp.Status.Message)
	}
	logger.Info("Preloaded model %s (path: %s, type: %s) for app %s", modelID, path, grpcType, appID)

	if !fresh || model.ResolveModelType(meta.ModelType) != "detection" {
		return nil
	}
	// The stored file stays (the platform row owns it); only this freshly
	// created registration is rolled back on failure.
	return s.probeFreshRegistration(ctx, client, appID, modelID)
}

// probeFreshRegistration smoke-tests a registration this app just created and
// rolls it back on failure. A detection model whose postprocess profile does
// not match its HEF registers fine but then fails every infer; probing one
// frame right after RegisterModel catches the mismatch while the caller can
// still undo the registration. Missing tensor info skips the probe (that is
// RunLoadSmokeTest's contract). The smoke failure is returned so install-path
// callers can route it into the install result; preload-path callers just
// take the rollback and its Error log.
func (s *AppManagerServer) probeFreshRegistration(ctx context.Context, client inferencepb.InferenceServiceClient, appID, modelID string) error {
	info, infoErr := client.GetModelInfo(ctx, &inferencepb.ModelInfo{ModelId: modelID})
	if infoErr != nil {
		info = nil // no tensor info: RunLoadSmokeTest skips the probe
	}
	if smokeErr := modelload.RunLoadSmokeTest(ctx, client, modelID, info); smokeErr != nil {
		logger.Error("Load smoke test failed for freshly registered model %s (app %s); rolling back registration: %v", modelID, appID, smokeErr)
		if _, unregErr := client.UnregisterModel(ctx, &inferencepb.ModelInfo{ModelId: modelID, OwnerId: appID}); unregErr != nil {
			logger.Error("Failed to roll back registration of model %s for app %s: %v", modelID, appID, unregErr)
		}
		return smokeErr
	}
	return nil
}

// unregisterAppModel unregisters one model and reports whether the runtime
// confirmed it gone. Transport errors and OK-with-success=false statuses
// both count as "not confirmed" — success=false is how ai-runtime refuses
// (e.g. while sessions are still in use).
func (s *AppManagerServer) unregisterAppModel(ctx context.Context, client inferencepb.InferenceServiceClient, appID, modelID string) bool {
	resp, err := client.UnregisterModel(ctx, &inferencepb.ModelInfo{
		ModelId: modelID,
		OwnerId: appID,
	})
	if err != nil {
		logger.Warn("Failed to unload model %s for app %s: %v", modelID, appID, err)
		return false
	}
	if !resp.GetSuccess() {
		logger.Warn("Runtime refused to unload model %s for app %s: %s", modelID, appID, resp.GetMessage())
		return false
	}
	logger.Info("Unloaded model %s for app %s", modelID, appID)
	return true
}

// UnloadModels unregisters the models for this app_id and removes the model
// files extracted from its image
func (s *AppManagerServer) UnloadModels(ctx context.Context, appID string, manifestPath string) {
	s.aiRuntimeMutex.RLock()
	client := s.aiRuntimeClient
	s.aiRuntimeMutex.RUnlock()
	runtimeEnabled := client != nil && s.config.AIRuntime.Enabled

	// Unregister first; the file removal at the end is gated on the outcome.
	// A refused unregister leaves a live transient registration that still
	// needs its HEF — deleting the files would orphan it with no retry path.
	// When the runtime is unreachable there is no live registration
	// (registrations die with the process), so the files can still go.
	unregisterHiccup := false
	if runtimeEnabled {
		// Unload models declared in manifest
		if manifestPath != "" {
			appManifest, err := manifest.LoadManifest(manifestPath)
			if err == nil && appManifest != nil {
				for _, modelID := range appManifest.Spec.Permissions.Inference.Models {
					if !s.unregisterAppModel(ctx, client, appID, modelID) {
						unregisterHiccup = true
					}
				}
			}
		}

		// Unload dynamically registered models by querying AI Runtime
		resp, err := client.ListModels(ctx, &inferencepb.Empty{})
		if err != nil {
			logger.Warn("Failed to list models for app %s cleanup: %v", appID, err)
			// Live registrations are unknown, not absent — keep the files.
			unregisterHiccup = true
		} else {
			unloaded := 0
			for _, m := range resp.Models {
				if m.OwnerId == appID {
					if s.unregisterAppModel(ctx, client, appID, m.ModelId) {
						unloaded++
					} else {
						unregisterHiccup = true
					}
				}
			}
			if unloaded > 0 {
				logger.Info("Unloaded %d dynamic models for app %s", unloaded, appID)
			}
		}
	}

	// Remove bundled model files extracted at install time. UninstallApp is
	// the only caller and reinstall recreates the directory, so this is safe
	// and idempotent. Kept (with a warning) when an unregister above failed:
	// a live registration may still reference them.
	if err := requireSafePathSegment("app id", appID); err != nil {
		// Install rejects unsafe IDs long before any files exist, so there is
		// nothing to remove — and RemoveAll on the derived path could act far
		// outside this app's own tree.
		logger.Warn("Skipping extracted-model cleanup for app %s: %v", appID, err)
	} else if unregisterHiccup {
		logger.Warn("Keeping extracted model files for app %s: an unregister was not confirmed — a live registration may still need them; retry the uninstall", appID)
	} else if err := os.RemoveAll(appModelsDir(appID)); err != nil {
		logger.Warn("Failed to remove extracted model files for app %s: %v", appID, err)
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
