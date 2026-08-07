package main

import (
	"context"
	"crypto/rand"
	"crypto/tls"
	"encoding/hex"
	"encoding/json"
	"flag"
	"fmt"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"
	"time"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"gorm.io/gorm"

	"aipc/platform/common/config"
	"aipc/platform/common/constants"
	"aipc/platform/common/events"
	"aipc/platform/common/logger"
	eventpb "aipc/platform/event-bus/proto"
	"aipc/platform/platform-api/auth"
	cfgctrl "aipc/platform/platform-api/config"
	platformdb "aipc/platform/platform-api/db"
	"aipc/platform/platform-api/gyro"
	"aipc/platform/platform-api/handlers"
	"aipc/platform/platform-api/internal/secrets"
	"aipc/platform/platform-api/storage"
	"aipc/platform/platform-api/websocket"
)

var (
	configPath = flag.String("config", "/data/aipc/etc/platform-api.yaml", "Path to configuration file")
)

type Config struct {
	Service struct {
		Name               string    `yaml:"name"`
		HTTPAddr           string    `yaml:"http_addr"`
		ReadTimeoutSeconds int       `yaml:"read_timeout_seconds" default:"1800"`
		LogLevel           string    `yaml:"log_level"`
		LogFile            string    `yaml:"log_file"`
		TLS                TLSConfig `yaml:"tls"`
	} `yaml:"service"`

	Services struct {
		AIRuntime     string `yaml:"ai_runtime"`
		EventBus      string `yaml:"event_bus"`
		DeviceControl string `yaml:"device_control"`
		AppManager    string `yaml:"app_manager"`
		CameraControl string `yaml:"camera_control"`
		Discovery     string `yaml:"discovery"`
	} `yaml:"services"`

	Web struct {
		StaticPath string `yaml:"static_path"`
		EnableCORS bool   `yaml:"enable_cors"`
	} `yaml:"web"`

	Stream struct {
		CameraConfig  string `yaml:"camera_config"`
		RtspBaseURL   string `yaml:"rtsp_base_url"`
		EncodedPubDir string `yaml:"encoded_pub_dir"`
	} `yaml:"stream"`

	Model struct {
		StoragePath string `yaml:"storage_path"`
	} `yaml:"model"`

	Storage struct {
		RootPath      string `yaml:"root_path"`
		ModelBlobPath string `yaml:"model_blob_path"`
		MinFreeBytes  uint64 `yaml:"min_free_bytes"`
	} `yaml:"storage"`

	Files struct {
		AllowedRoots []string `yaml:"allowed_roots"`
	} `yaml:"files"`

	Auth struct {
		Enabled  bool   `yaml:"enabled"`
		TokenKey string `yaml:"token_key"`
		Username string `yaml:"username"`
		Password string `yaml:"password"`
	} `yaml:"auth"`

	Database struct {
		Path string `yaml:"path"`
	} `yaml:"database"`

	Gyro GyroConfig `yaml:"gyro"`
}

// GyroConfig configures the on-board IMU attitude source for the gyro
// calibration SSE endpoint. MountMatrix is a row-major 3x3 matrix mapping the
// LSM6DSR sensor body frame to the spec world frame (X forward, Y right, Z up);
// calibrate it per PCB orientation so a level, front-facing device reads as the
// identity quaternion. A zero matrix is treated as identity.
type GyroConfig struct {
	Enabled             bool       `yaml:"enabled"`
	PollRateHz          int        `yaml:"poll_rate_hz"`
	ODRHz               int        `yaml:"odr_hz"`
	FusionAlpha         float64    `yaml:"fusion_alpha"`
	AccelPath           string     `yaml:"accel_path"`
	GyroPath            string     `yaml:"gyro_path"`
	IIOBase             string     `yaml:"iio_base"`
	CalibrationPath     string     `yaml:"calibration_path"`
	UseCalibrationMount bool       `yaml:"use_calibration_mount"`
	UseCalibrationBias  bool       `yaml:"use_calibration_bias"`
	MountMatrix         [9]float64 `yaml:"mount_matrix"`
	CalibrateBias       bool       `yaml:"calibrate_bias"`
	CalibrationMs       int        `yaml:"calibration_ms"`
}

// TLSConfig configures the HTTPS listener. When Enabled, platform-api serves
// the web console over TLS (in-process ListenAndServeTLS) and, optionally,
// 301-redirects the plain-HTTP listener to HTTPS. If CertFile/KeyFile are both
// empty the device generates a self-signed certificate once into AutoCertDir
// (see tls.go); supplying both switches to operator-owned rotation.
type TLSConfig struct {
	Enabled      bool   `yaml:"enabled"`
	HTTPSAddr    string `yaml:"https_addr"`    // e.g. ":443"; defaults to ":443"
	CertFile     string `yaml:"cert_file"`     // user-supplied cert; empty = auto-generate
	KeyFile      string `yaml:"key_file"`      // user-supplied key; empty = auto-generate
	AutoCertDir  string `yaml:"auto_cert_dir"` // e.g. "/data/aipc/etc/ssl"
	RedirectHTTP bool   `yaml:"redirect_http"` // http_addr 301 -> https
}

func (c *Config) Validate() error {
	if c.Service.HTTPAddr == "" {
		return fmt.Errorf("service.http_addr is required")
	}
	if c.Service.ReadTimeoutSeconds <= 0 {
		return fmt.Errorf("service.read_timeout_seconds must be positive")
	}
	// TLS defaults + mutual-exclusivity: cert/key are either both set
	// (operator-supplied) or both empty (device auto-generates).
	tls := &c.Service.TLS
	if tls.Enabled {
		if tls.HTTPSAddr == "" {
			tls.HTTPSAddr = ":443"
		}
		if (tls.CertFile == "") != (tls.KeyFile == "") {
			return fmt.Errorf("service.tls.cert_file and key_file must both be set or both be empty")
		}
	}
	return nil
}

type PlatformAPIServer struct {
	config        *Config
	engine        *gin.Engine
	httpServer    *http.Server
	tlsServer     *http.Server // non-nil when Config.Service.TLS.Enabled
	tlsCertFile   string
	tlsKeyFile    string
	db            *gorm.DB
	eventLogger   *events.Logger
	persistCtx    context.Context
	persistCancel context.CancelFunc
	gyroSrc       gyro.Source
	gyroCancel    context.CancelFunc
	monitor       *handlers.MonitorHandler
	grpcClients   struct {
		aiRuntime     *grpc.ClientConn
		eventBus      *grpc.ClientConn
		deviceControl *grpc.ClientConn
		appManager    *grpc.ClientConn
		cameraControl *grpc.ClientConn
		discovery     *grpc.ClientConn
	}
	eventStream *websocket.EventStream
}

func NewPlatformAPIServer(cfg *Config) (*PlatformAPIServer, error) {
	// Initialize gin engine. Keep request logs free of query strings because
	// WebSocket/SSE clients may pass bearer tokens in ?token= when browsers
	// cannot set Authorization headers for the upgrade request.
	engine := gin.New()
	engine.Use(gin.LoggerWithFormatter(sanitizedGinLogFormatter), gin.Recovery())

	// Set max multipart form size to 200MB for OTA firmware uploads
	engine.MaxMultipartMemory = 200 << 20 // 200 MB

	server := &PlatformAPIServer{
		config: cfg,
		engine: engine,
	}

	// Initialize database
	dbPath := cfg.Database.Path
	if dbPath == "" {
		dbPath = constants.DataPath() + "/platform.db"
	}
	db, err := platformdb.Init(dbPath)
	if err != nil {
		return nil, fmt.Errorf("failed to initialize database: %w", err)
	}
	server.db = db
	logger.Info("Database initialized: %s", dbPath)

	// Initialize event logger
	server.eventLogger = events.NewLogger(db)
	logger.Info("Event logger initialized")

	// Seed initial data
	if err := platformdb.Seed(db); err != nil {
		logger.Warn("Failed to seed database: %v", err)
	}

	// Connect to gRPC services
	if err := server.connectToServices(); err != nil {
		return nil, fmt.Errorf("failed to connect to services: %w", err)
	}

	// Setup routes
	server.setupRoutes()

	readTimeout := time.Duration(cfg.Service.ReadTimeoutSeconds) * time.Second

	// Setup HTTP server
	server.httpServer = &http.Server{
		Addr:         cfg.Service.HTTPAddr,
		Handler:      server.engine,
		ReadTimeout:  readTimeout, // Allows large OS upgrade uploads on slower links.
		WriteTimeout: 0,           // disabled: streaming endpoints (WebSocket, SSE, H264) need long-lived connections
		IdleTimeout:  60 * time.Second,
	}

	// Setup HTTPS (TLS) server when enabled. The certificate is resolved once
	// here (generated or validated) and reused for the lifetime of the process.
	if cfg.Service.TLS.Enabled {
		certPath, keyPath, err := ensureCert(cfg.Service.TLS)
		if err != nil {
			return nil, fmt.Errorf("failed to resolve TLS certificate: %w", err)
		}
		server.tlsCertFile = certPath
		server.tlsKeyFile = keyPath
		server.tlsServer = &http.Server{
			Addr:         cfg.Service.TLS.HTTPSAddr,
			Handler:      server.engine,
			ReadTimeout:  readTimeout,
			WriteTimeout: 0, // streaming endpoints (WebSocket, SSE, H264) need long-lived connections
			IdleTimeout:  60 * time.Second,
			TLSConfig:    &tls.Config{MinVersion: tls.VersionTLS12},
		}
		logger.Info("HTTPS enabled on %s (cert=%s)", cfg.Service.TLS.HTTPSAddr, certPath)
	}

	return server, nil
}

func sanitizedGinLogFormatter(param gin.LogFormatterParams) string {
	path := param.Path
	if param.Request != nil && param.Request.URL != nil {
		path = param.Request.URL.Path
	}
	if path == "" {
		path = "-"
	}
	return fmt.Sprintf("[GIN] %v | %3d | %13v | %15s | %-7s %#v\n%s",
		param.TimeStamp.Format("2006/01/02 - 15:04:05"),
		param.StatusCode,
		param.Latency,
		param.ClientIP,
		param.Method,
		path,
		param.ErrorMessage,
	)
}

func (s *PlatformAPIServer) connectToServices() error {
	// All gRPC connections use non-blocking dial so that:
	// 1. platform-api starts regardless of downstream service readiness
	// 2. gRPC handles reconnection automatically in the background
	// 3. RPCs fail transiently until the target service comes up, then recover

	// Connect to AI Runtime
	if s.config.Services.AIRuntime != "" {
		conn, err := grpc.DialContext(context.Background(), s.config.Services.AIRuntime,
			grpc.WithTransportCredentials(insecure.NewCredentials()))
		if err != nil {
			logger.Warn("Failed to create AI Runtime client: %v", err)
		} else {
			s.grpcClients.aiRuntime = conn
			logger.Info("AI Runtime client created (lazy connect): %s", s.config.Services.AIRuntime)
		}
	}

	// Connect to Event Bus
	if s.config.Services.EventBus != "" {
		conn, err := grpc.DialContext(context.Background(), s.config.Services.EventBus,
			grpc.WithTransportCredentials(insecure.NewCredentials()))
		if err != nil {
			logger.Warn("Failed to create Event Bus client: %v", err)
		} else {
			s.grpcClients.eventBus = conn
			logger.Info("Event Bus client created (lazy connect): %s", s.config.Services.EventBus)
		}
	}

	// Connect to Device Control
	if s.config.Services.DeviceControl != "" {
		conn, err := grpc.DialContext(context.Background(), s.config.Services.DeviceControl,
			grpc.WithTransportCredentials(insecure.NewCredentials()))
		if err != nil {
			logger.Warn("Failed to create Device Control client: %v", err)
		} else {
			s.grpcClients.deviceControl = conn
			logger.Info("Device Control client created (lazy connect): %s", s.config.Services.DeviceControl)
		}
	}

	// Connect to App Manager
	if s.config.Services.AppManager != "" {
		conn, err := grpc.DialContext(context.Background(), s.config.Services.AppManager,
			grpc.WithTransportCredentials(insecure.NewCredentials()))
		if err != nil {
			logger.Warn("Failed to create App Manager client: %v", err)
		} else {
			s.grpcClients.appManager = conn
			logger.Info("App Manager client created (lazy connect): %s", s.config.Services.AppManager)
		}
	}

	// Connect to Camera Control
	if s.config.Services.CameraControl != "" {
		conn, err := grpc.DialContext(context.Background(), s.config.Services.CameraControl,
			grpc.WithTransportCredentials(insecure.NewCredentials()))
		if err != nil {
			logger.Warn("Failed to create Camera Control client: %v", err)
		} else {
			s.grpcClients.cameraControl = conn
			logger.Info("Camera Control client created (lazy connect): %s", s.config.Services.CameraControl)
		}
	}

	// Connect to Discovery Service
	if s.config.Services.Discovery != "" {
		conn, err := grpc.DialContext(context.Background(), s.config.Services.Discovery,
			grpc.WithTransportCredentials(insecure.NewCredentials()))
		if err != nil {
			logger.Warn("Failed to create Discovery client: %v", err)
		} else {
			s.grpcClients.discovery = conn
			logger.Info("Discovery client created (lazy connect): %s", s.config.Services.Discovery)
		}
	}

	return nil
}

func (s *PlatformAPIServer) setupRoutes() {
	// Create handlers
	modelStoragePath := s.config.Model.StoragePath
	if modelStoragePath == "" {
		modelStoragePath = constants.ModelsPath()
	}

	// Initialize CAS model storage
	blobPath := s.config.Storage.ModelBlobPath
	if blobPath == "" {
		blobPath = filepath.Join(modelStoragePath, "blobs")
	}
	minFree := s.config.Storage.MinFreeBytes
	if minFree == 0 {
		minFree = 100 * 1024 * 1024 // 100MB default
	}
	modelStore, err := storage.NewModelStorage(blobPath, minFree)
	if err != nil {
		logger.Warn("Failed to initialize model storage: %v (uploads will use legacy path)", err)
	}

	// Password crypto setup (once per boot, before handlers are built):
	//   1. Load or first-boot-generate the RSA keypair the frontend encrypts
	//      passwords with. On failure rsaPriv is nil and login falls back to
	//      plaintext comparison (still correct, just unencrypted in transit).
	//   2. One-time migrate any legacy plaintext auth.password in the config
	//      to a bcrypt hash, and keep the in-memory value in sync with disk.
	rsaPriv, rsaPubPEM, rsaErr := ensureRSAKeyPair(constants.ConfigPath() + "/rsa")
	if rsaErr != nil {
		logger.Warn("RSA keypair unavailable; encrypted login falls back to plaintext: %v", rsaErr)
	}
	if finalPwd, migrateErr := secrets.MigratePlaintextPassword(*configPath, os.Getenv("AIPC_AUTH_PASSWORD")); migrateErr == nil && finalPwd != "" {
		s.config.Auth.Password = finalPwd
	}

	// Web logins use revocable in-memory sessions. The configured token key is
	// retained separately as an API key for integrations.
	authValidator := auth.NewTokenValidator(s.config.Auth.TokenKey, s.config.Auth.Enabled)

	apiHandlers := handlers.NewAPIHandlers(&handlers.GRPCClients{
		AIRuntime:     s.grpcClients.aiRuntime,
		EventBus:      s.grpcClients.eventBus,
		DeviceControl: s.grpcClients.deviceControl,
		AppManager:    s.grpcClients.appManager,
	}, modelStoragePath, modelStore, s.db, s.config.Auth.Username, s.config.Auth.Password, authValidator, rsaPriv, rsaPubPEM, s.eventLogger)

	// Phase 2: reconcile the desired-state store with live config at startup.
	// Currently scopes the media domain (camera-daemon.yaml): confirms the live
	// file matches the desired row (in-sync no-op), re-projects the desired
	// value to disk on drift, or imports a pre-existing live file into an empty
	// store (R-migration) without overwriting the file. Non-fatal: a reconcile
	// error is logged and the server continues serving.
	if cm := apiHandlers.ConfigManager(); cm != nil {
		targets := []cfgctrl.ReconcileTarget{{Domain: "media", Key: "config"}}
		if err := cm.Reconcile(context.Background(), targets, "system"); err != nil {
			logger.Warn("Config reconcile completed with errors: %v", err)
		} else {
			logger.Info("Config reconcile completed")
		}
	}

	// Create authentication middleware
	authMiddleware := auth.Middleware(authValidator)

	// CORS wrapper
	corsMiddleware := func() gin.HandlerFunc {
		return func(c *gin.Context) {
			if s.config.Web.EnableCORS {
				c.Header("Access-Control-Allow-Origin", "*")
				c.Header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, OPTIONS")
				c.Header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-API-Key")
				if c.Request.Method == "OPTIONS" {
					c.AbortWithStatus(http.StatusOK)
					return
				}
			}
			c.Next()
		}
	}()

	s.engine.Use(corsMiddleware)

	// HSTS: instruct browsers to prefer HTTPS for one year. Only emitted on TLS
	// responses (r.TLS != nil) so the plain-HTTP redirect listener never pins a
	// host that the browser has never authenticated. See RFC 6797 §11.2.
	if s.config.Service.TLS.Enabled {
		s.engine.Use(func(c *gin.Context) {
			if c.Request.TLS != nil {
				c.Header("Strict-Transport-Security", "max-age=31536000")
			}
			c.Next()
		})
	}

	// Public Routes
	s.engine.POST("/api/login", apiHandlers.Login)
	s.engine.POST("/api/v1/logout", apiHandlers.Logout)
	s.engine.GET("/api/v1/auth/public-key", apiHandlers.GetPublicKey) // Pre-login: frontend fetches it to encrypt the password

	// Create API V1 Group
	api := s.engine.Group("/api/v1")
	api.Use(authMiddleware)

	// System routes
	api.GET("/system/info", apiHandlers.GetSystemInfo)
	api.GET("/system/stats", apiHandlers.GetSystemStats)

	systemHandler := handlers.NewSystemHandlers("", apiHandlers.ConfigManager(), rsaPriv)
	systemHandler.SetEventLogger(s.eventLogger)
	api.POST("/system/password", systemHandler.UpdatePassword)
	api.POST("/system/restart", systemHandler.RestartSystem)
	api.GET("/system/ota/detect", systemHandler.OTADetect)
	api.POST("/system/ota/parse", systemHandler.OTAParseFirmware)
	api.POST("/system/ota/install", systemHandler.OTAInstall)
	api.POST("/system/ota/install-from-path", systemHandler.OTAInstallFromPath)

	// OTA status is intentionally public (no session auth). Web session tokens
	// are stored in platform-api's process memory and are wiped when it
	// restarts as part of the upgrade, so an authenticated GET would 401 after
	// the reboot and the frontend could never observe completion. The endpoint
	// is read-only, and the job_id query param already scopes which upgrade
	// session is being polled.
	s.engine.GET("/api/v1/system/ota/status", systemHandler.OTAGetStatus)

	osUpgradeHandler := handlers.NewOSUpgradeHandlers(os.Getenv("AIPC_OS_UPGRADE_DIR"))
	// On boot, advance any job stuck in rebooting/verifying to a terminal
	// state in case aipc-os-verify.service did not fire after the reboot.
	osUpgradeHandler.ReconcileOnBoot()
	// OS upgrade status is read-only and must survive platform-api / device
	// restarts. Web session tokens are in-memory, so keep status outside auth
	// just like firmware OTA status; mutating OS upgrade endpoints remain
	// protected below.
	s.engine.GET("/api/v1/system/os-upgrade/status", osUpgradeHandler.Status)
	api.POST("/system/os-upgrade/upload", osUpgradeHandler.Upload)
	api.POST("/system/os-upgrade/validate", osUpgradeHandler.Validate)
	api.POST("/system/os-upgrade/install", osUpgradeHandler.Install)
	api.POST("/system/os-upgrade/reboot", osUpgradeHandler.Reboot)
	api.POST("/system/os-upgrade/cancel", osUpgradeHandler.Cancel)
	api.DELETE("/system/os-upgrade/package", osUpgradeHandler.DeletePackage)

	s.engine.GET("/api/v1/system/health", apiHandlers.HealthCheck) // Health check doesn't need auth

	// Time routes
	timeHandler := handlers.NewTimeHandler("", s.eventLogger, apiHandlers.ConfigManager(), s.grpcClients.cameraControl)
	api.POST("/system/time/sync-from-client", timeHandler.SyncFromClient)
	timeHandler.RestoreTimeOnBoot()
	s.persistCtx, s.persistCancel = context.WithCancel(context.Background())
	go timeHandler.StartTimePersistLoop(s.persistCtx)
	api.GET("/system/time", timeHandler.GetSystemTime)
	api.POST("/system/time/set", timeHandler.SetSystemTime)
	api.GET("/system/time/config", timeHandler.GetTimeConfig)
	api.PUT("/system/time/timezone", timeHandler.SetTimezone)
	api.PUT("/system/time/ntp", timeHandler.SetNTPConfig)
	api.POST("/system/time/ntp/sync", timeHandler.SyncNTP)
	api.GET("/system/time/timezones", timeHandler.GetTimezones)
	api.PUT("/system/time/config", timeHandler.SaveTimeConfig)

	// AI Runtime proxy routes
	ai := api.Group("/ai")
	ai.GET("/capabilities", apiHandlers.GetCapabilities)
	ai.POST("/models/parse", apiHandlers.ParseModel)
	ai.POST("/models/upload", apiHandlers.UploadModel)
	ai.GET("/models", apiHandlers.ListModels)
	ai.POST("/models/scan", apiHandlers.ScanModels)
	ai.POST("/models", apiHandlers.RegisterModel)
	ai.GET("/models/:model_id", apiHandlers.GetModelInfo)
	ai.DELETE("/models/:model_id", apiHandlers.UnregisterModel)
	ai.GET("/models/:model_id/apps", apiHandlers.GetModelApps)
	ai.POST("/models/:model_id/load", apiHandlers.LoadModel)
	ai.POST("/models/:model_id/unload", apiHandlers.UnloadModel)
	ai.GET("/stats", apiHandlers.GetAIStats)

	// Event Bus routes
	events := api.Group("/events")
	events.GET("/topics", apiHandlers.ListTopics)
	events.POST("/publish", apiHandlers.PublishEvent)

	// Event Logs routes
	eventLogHandler := handlers.NewEventLogHandler(s.db, s.eventLogger)
	eventLogs := api.Group("/event-logs")
	eventLogs.GET("", eventLogHandler.List)
	eventLogs.GET("/statistics", eventLogHandler.GetStatistics)
	eventLogs.GET("/templates", eventLogHandler.GetTemplates)
	eventLogs.DELETE("", eventLogHandler.Cleanup)
	eventLogs.POST("", eventLogHandler.Create)

	// Debug Logs routes (admin only - export only)
	debugLogHandler := handlers.NewDebugLogHandler()
	debugLogs := api.Group("/debug-logs")
	debugLogs.POST("/export", debugLogHandler.Export)
	debugLogs.GET("/services", debugLogHandler.GetServices)
	debugLogs.GET("/files", debugLogHandler.GetFiles)

	// Device Control routes
	device := api.Group("/device")
	device.GET("/status", apiHandlers.GetDeviceStatus)
	device.POST("/light", apiHandlers.SetLight)
	device.POST("/ir-led", apiHandlers.SetIrLed)
	device.POST("/ir-cut", apiHandlers.SetIrCut)
	device.POST("/ptz", apiHandlers.ControlPTZ)
	device.POST("/zoom", apiHandlers.ControlZoom)
	device.POST("/focus", apiHandlers.ControlFocus)
	device.POST("/autofocus", apiHandlers.SetAutofocus)
	device.POST("/lens/oneshot-af", apiHandlers.OneshotAutofocus)
	device.POST("/lens/af/oneshot", apiHandlers.OneshotAutofocus)
	device.POST("/lens/zoom-follow", apiHandlers.StartZoomFollow)
	device.GET("/lens/af/status", apiHandlers.GetAutofocusStatus)
	device.POST("/lens/af/cancel", apiHandlers.CancelAutofocus)
	device.GET("/lens/status", apiHandlers.GetLensStatus)
	device.PUT("/lens/zoom-level", apiHandlers.SetZoomLevel)
	device.PUT("/lens/focus-level", apiHandlers.SetFocusLevel)
	device.POST("/lens/reset-zero", apiHandlers.LensResetZero)
	device.POST("/lens/iris", apiHandlers.ControlIris)
	device.POST("/lens/iris-target", apiHandlers.SetIrisTarget)
	device.PUT("/lens/limits", apiHandlers.SetLensLimits)
	device.POST("/lens/init", apiHandlers.LensInit)
	device.POST("/lens/goto", apiHandlers.LensGotoRatioDistance)
	device.POST("/gpio", apiHandlers.GPIOWrite)
	device.GET("/gpio", apiHandlers.GPIOBatchRead)
	device.GET("/gpio/:pin", apiHandlers.GPIORead)

	// Peripheral control routes
	device.POST("/fan", apiHandlers.SetFan)
	device.GET("/fan", apiHandlers.GetFan)
	device.POST("/heat", apiHandlers.SetHeat)
	device.GET("/heat", apiHandlers.GetHeat)
	device.POST("/radar", apiHandlers.SetRadar)
	device.GET("/radar", apiHandlers.GetRadar)
	device.POST("/alarm-out", apiHandlers.SetAlarmOut)
	device.GET("/alarm-out/:channel", apiHandlers.GetAlarmOut)
	device.POST("/wiegand", apiHandlers.SetWiegand)
	device.GET("/wiegand/:channel", apiHandlers.GetWiegand)
	device.GET("/alarm-outputs", apiHandlers.GetAlarmOutputs)
	device.POST("/rs485/init", apiHandlers.Rs485Init)
	device.POST("/rs485/deinit", apiHandlers.Rs485Deinit)
	device.POST("/rs485/tx", apiHandlers.Rs485Tx)
	device.GET("/capabilities", apiHandlers.GetDeviceCapabilities)

	// App Manager routes
	apps := api.Group("/apps")
	apps.GET("", apiHandlers.ListApps)
	apps.POST("", apiHandlers.InstallApp)
	apps.POST("/wizard", apiHandlers.WizardInstall)
	apps.POST("/upload-image", apiHandlers.UploadImage)
	apps.POST("/upload-manifest", apiHandlers.UploadManifest)
	apps.POST("/install-package", apiHandlers.InstallPackage)
	apps.GET("/install-progress/:task_id", apiHandlers.GetInstallProgress)
	apps.GET("/:app_id/stats", apiHandlers.GetAppStats)
	apps.GET("/:app_id/logs", apiHandlers.GetAppLogs)
	apps.POST("/:app_id/start", apiHandlers.StartApp)
	apps.POST("/:app_id/stop", apiHandlers.StopApp)
	apps.POST("/:app_id/restart", apiHandlers.RestartApp)
	apps.DELETE("/:app_id", apiHandlers.UninstallApp)
	apps.GET("/:app_id", apiHandlers.GetApp)
	apps.GET("/:app_id/permissions", apiHandlers.GetAppPermissions)

	// Settings routes (database-backed key-value store)
	settings := api.Group("/settings")
	settings.GET("", apiHandlers.GetAllSettings)
	settings.POST("", apiHandlers.SetSetting)
	settings.DELETE("/:key", apiHandlers.DeleteSetting)

	// Config Controller job audit (read-only). Exposes apply/delete/reconcile
	// job history, including the Phase 2 startup reconcile.
	configJobs := api.Group("/config/jobs")
	configJobs.GET("", apiHandlers.ListConfigJobs)
	configJobs.GET("/:id", apiHandlers.GetConfigJob)

	// Store routes (App Store)
	storeHandlers := handlers.NewStoreHandlers(s.db)
	store := api.Group("/store")
	store.GET("/apps", storeHandlers.ListApps)
	store.GET("/apps/:key", storeHandlers.GetApp)
	store.POST("/apps/:key/install", storeHandlers.InstallFromStore)
	store.GET("/categories", storeHandlers.ListCategories)
	store.GET("/tags", storeHandlers.ListTags)
	// App Install management
	store.GET("/installs", storeHandlers.ListInstalls)
	store.GET("/installs/:app_id", storeHandlers.GetInstall)
	store.POST("/installs", storeHandlers.CreateInstall)
	store.PUT("/installs/:app_id", storeHandlers.UpdateInstall)
	store.DELETE("/installs/:app_id", storeHandlers.DeleteInstall)

	// Dev routes (App Development Workbench)
	devHandlers := handlers.NewDevHandlers(s.db, s.grpcClients.appManager)
	dev := api.Group("/dev")
	dev.GET("/base-images", devHandlers.ListBaseImages)
	dev.GET("/projects", devHandlers.ListProjects)
	dev.POST("/projects", devHandlers.CreateProject)
	dev.GET("/projects/:id", devHandlers.GetProject)
	dev.PUT("/projects/:id", devHandlers.UpdateProject)
	dev.DELETE("/projects/:id", devHandlers.DeleteProject)
	dev.POST("/projects/:id/upload", devHandlers.UploadFile)
	dev.POST("/projects/:id/source", devHandlers.UploadSource)
	dev.GET("/projects/:id/files", devHandlers.ListFiles)
	dev.GET("/projects/:id/file", devHandlers.GetFileContent)
	dev.POST("/projects/:id/file", devHandlers.SaveFileContent)
	dev.GET("/projects/:id/builds", devHandlers.ListBuilds)
	dev.POST("/projects/:id/build", devHandlers.CreateBuild)

	// === Container Management ===
	containerHandler := handlers.NewContainerHandlers(s.grpcClients.appManager)
	containers := api.Group("/containers")
	containers.GET("", containerHandler.ListContainers)
	containers.GET("/:id", containerHandler.GetContainer)
	containers.GET("/:id/stats", containerHandler.GetContainerStats)
	containers.GET("/:id/logs", containerHandler.GetContainerLogs)
	containers.GET("/:id/logs/stream", containerHandler.StreamContainerLogs)
	containers.GET("/:id/logs/ws", containerHandler.StreamContainerLogsWS)
	containers.POST("/:id/start", containerHandler.StartContainer)
	containers.POST("/:id/stop", containerHandler.StopContainer)
	containers.POST("/:id/restart", containerHandler.RestartContainer)
	containers.DELETE("/:id", containerHandler.RemoveContainer)
	containers.GET("/:id/exec/ws", containerHandler.ExecContainerWS)

	// Images
	images := api.Group("/images")
	images.GET("", containerHandler.ListImages)
	images.POST("/pull", containerHandler.PullImage)
	images.DELETE("/:image", containerHandler.DeleteImage)

	// === DevOps / Operations routes ===

	// Resource Monitoring
	s.gyroSrc = s.buildGyroSource()
	monitorHandler := handlers.NewMonitorHandler(s.grpcClients.aiRuntime, s.gyroSrc)
	s.monitor = monitorHandler
	monitor := api.Group("/monitor")
	monitor.GET("/summary", monitorHandler.GetSummary)
	monitor.GET("/cpu", monitorHandler.GetCPU)
	monitor.GET("/memory", monitorHandler.GetMemory)
	monitor.GET("/disk", monitorHandler.GetDisk)
	monitor.GET("/network", monitorHandler.GetNetwork)
	monitor.GET("/snapshot", monitorHandler.GetResourceSnapshot)
	monitor.GET("/gyro/attitude", monitorHandler.StreamGyroAttitude)

	// Process Management
	processHandler := handlers.NewProcessHandler()
	procs := api.Group("/processes")
	procs.GET("", processHandler.List)
	procs.GET("/:pid", processHandler.GetInfo)
	procs.POST("/:pid/kill", processHandler.Kill)

	// File Management
	fileHandler := handlers.NewFileHandler(s.config.Files.AllowedRoots, constants.RootPath()) // default: /data, /tmp
	files := api.Group("/files")
	files.GET("", fileHandler.List)
	files.GET("/content", fileHandler.ReadContent)
	files.POST("/content", fileHandler.WriteContent)
	files.POST("/upload", fileHandler.Upload)
	files.GET("/download", fileHandler.Download)
	files.POST("/batch-download", fileHandler.BatchDownload)
	files.DELETE("", fileHandler.Delete)
	files.POST("/batch-delete", fileHandler.BatchDelete)
	files.POST("/mkdir", fileHandler.MakeDir)
	files.POST("/rename", fileHandler.Rename)

	// Web Terminal (WebSocket PTY)
	terminalHandler := handlers.NewTerminalHandler()
	api.GET("/terminal/ws", terminalHandler.HandleTerminalWS)

	// SSH Settings
	sshHandler := handlers.NewSSHHandler(s.eventLogger)
	sshGroup := api.Group("/ssh")
	sshGroup.GET("/config", sshHandler.GetConfig)
	sshGroup.POST("/config", sshHandler.SetConfig)
	sshGroup.GET("/status", sshHandler.GetStatus)
	sshGroup.GET("/logs", sshHandler.GetLogs)

	// Logs Viewer
	logHandler := handlers.NewLogHandler()
	logs := api.Group("/logs")
	logs.GET("/services", logHandler.GetServices)
	logs.GET("/files", logHandler.GetFiles)
	logs.GET("/content", logHandler.GetContent)
	logs.GET("/download", logHandler.Download)
	logs.GET("/stream/ws", logHandler.HandleStreamWS)

	// Network Configuration
	networkHandler := handlers.NewNetworkHandler(s.eventLogger, apiHandlers.ConfigManager())
	network := api.Group("/network")
	network.GET("/config", networkHandler.GetConfig)
	network.POST("/config", networkHandler.UpdateConfig)
	network.GET("/interfaces", networkHandler.GetInterfaces)

	// Device Information
	deviceInfoHandler := handlers.NewDeviceInfoHandler("", s.grpcClients.cameraControl, apiHandlers.ConfigManager())
	deviceInfo := api.Group("/device-info")
	deviceInfo.GET("", deviceInfoHandler.GetDeviceInfo)
	deviceInfo.PUT("", deviceInfoHandler.UpdateDeviceName)
	deviceInfo.GET("/factory", deviceInfoHandler.GetFactoryInfo)
	deviceInfo.POST("/factory", deviceInfoHandler.UpdateFactoryField)

	// WebSocket routes (using new websocket package)
	if s.grpcClients.eventBus != nil {
		s.eventStream = websocket.NewEventStream(s.grpcClients.eventBus)
		// Apply auth middleware to WebSocket endpoint
		events.GET("/stream", func(c *gin.Context) {
			s.eventStream.HandleWebSocket(c.Writer, c.Request)
		})
		// Start event stream in background
		ctx := context.Background()
		go s.eventStream.Start(ctx, "*") // Subscribe to all topics (wildcard)
	}

	// Storage Management
	var publishFunc func(topic string, payload map[string]interface{})
	if s.grpcClients.eventBus != nil {
		publishFunc = func(topic string, payload map[string]interface{}) {
			payloadBytes, err := json.Marshal(payload)
			if err != nil {
				return
			}
			client := eventpb.NewEventBusClient(s.grpcClients.eventBus)
			ctx, cancel := context.WithTimeout(context.Background(), 2*time.Second)
			defer cancel()
			client.Publish(ctx, &eventpb.PublishRequest{
				Event: &eventpb.Event{
					Topic:       topic,
					TimestampNs: uint64(time.Now().UnixNano()),
					Payload:     payloadBytes,
					PayloadType: "json",
				},
			})
		}
	}
	storageHandler := handlers.NewStorageHandlers(publishFunc, s.eventLogger)
	storageGroup := api.Group("/storage")
	storageGroup.GET("/disks", storageHandler.ListDisks)
	storageGroup.POST("/mount", storageHandler.MountDisk)
	storageGroup.POST("/unmount", storageHandler.UnmountDisk)
	storageGroup.POST("/format", storageHandler.FormatDisk)

	// Media Configuration
	mediaHandler := handlers.NewMediaHandlers(s.config.Stream.CameraConfig, s.grpcClients.cameraControl, s.eventLogger, apiHandlers.ConfigManager())
	mediaGroup := api.Group("/media")
	mediaGroup.GET("/config", mediaHandler.GetConfig)
	mediaGroup.POST("/config", mediaHandler.SetConfig)
	mediaGroup.PUT("/image", mediaHandler.UpdateImageConfig)
	mediaGroup.GET("/image", mediaHandler.GetImageConfig)
	mediaGroup.GET("/transform", mediaHandler.GetTransformConfig)
	mediaGroup.PUT("/transform", mediaHandler.UpdateTransformConfig)
	// Scalar profile config field (allow-listed): platform owns the value,
	// camera-daemon HAL set/get. Read/write one knob at a time (e.g. VDevice
	// sharing). Separate from the bulk /config above.
	mediaGroup.GET("/config/field", mediaHandler.GetConfigField)
	mediaGroup.PUT("/config/field", mediaHandler.SetConfigField)
	// Unified media-config import/export (Option B aggregation layer).
	// Export aggregates the base YAML + six runtime JSONs into one versioned
	// envelope; import writes them back and restarts camera-daemon to apply.
	mediaGroup.GET("/config/export", mediaHandler.ExportMediaConfig)
	mediaGroup.POST("/config/import", mediaHandler.ImportMediaConfig)
	// Hot reload endpoints (no service restart required)
	mediaGroup.PUT("/encoder", mediaHandler.UpdateEncoderConfig)
	mediaGroup.PUT("/rtsp", mediaHandler.SetRtspEnabled)
	mediaGroup.PUT("/ai-overlay", mediaHandler.UpdateAiOverlay)
	mediaGroup.GET("/osd", mediaHandler.GetOsdConfig)
	mediaGroup.PUT("/osd", mediaHandler.UpdateOsdConfig)
	mediaGroup.POST("/osd/upload-image", mediaHandler.UploadOsdImage)
	mediaGroup.GET("/osd/font", mediaHandler.ServeOsdFont)
	mediaGroup.GET("/osd/image/:name", mediaHandler.ServeOsdImage)
	// Privacy mask
	mediaGroup.GET("/privacy-mask", mediaHandler.GetPrivacyMaskConfig)
	mediaGroup.PUT("/privacy-mask", mediaHandler.UpdatePrivacyMaskConfig)
	mediaGroup.PUT("/encoder/reconfig", mediaHandler.ReconfigureEncoder)
	// Profile management (FROM_MEDIA pipeline profiles)
	mediaGroup.GET("/profile", mediaHandler.GetProfile)
	mediaGroup.GET("/profiles", mediaHandler.ListProfiles)
	mediaGroup.POST("/profile/switch", mediaHandler.SwitchProfile)
	mediaGroup.POST("/profile/backup", mediaHandler.BackupProfile)
	mediaGroup.POST("/pipeline/reconfigure", mediaHandler.ReconfigurePipeline)
	mediaGroup.GET("/status", mediaHandler.GetStreamStatus)
	mediaGroup.POST("/streams", mediaHandler.AddStream)
	mediaGroup.DELETE("/streams/:name", mediaHandler.RemoveStream)
	// Stream enable/disable via ReconfigurePipeline (real pipeline-level control)
	mediaGroup.POST("/streams/:name/enable", mediaHandler.EnableStream)
	mediaGroup.DELETE("/streams/:name/disable", mediaHandler.DisableStream)

	// Stream routes — load stream config from camera-daemon.yaml
	rtspBase := s.config.Stream.RtspBaseURL
	if rtspBase == "" {
		rtspBase = "rtsp://localhost:8554"
	}
	encodedPubDir := s.config.Stream.EncodedPubDir
	if encodedPubDir == "" {
		encodedPubDir = "/run/aipc/encoded"
	}
	streamConfigs := handlers.LoadStreamsFromCameraConfig(s.config.Stream.CameraConfig, rtspBase, encodedPubDir)
	streamHandlers := handlers.NewStreamHandlers(streamConfigs, rtspBase, encodedPubDir)
	mediaHandler.SetStreamReloader(streamHandlers)

	// Audio control endpoints
	audioHandler := handlers.NewAudioHandlers(s.grpcClients.cameraControl, s.config.Stream.CameraConfig, apiHandlers.ConfigManager())
	audioGroup := api.Group("/audio")
	audioGroup.GET("/capture-devices", audioHandler.ListCaptureDevices)
	audioGroup.GET("/playback-devices", audioHandler.ListPlaybackDevices)
	audioGroup.GET("/status", audioHandler.GetStatus)
	audioGroup.POST("/capture/start", audioHandler.StartCapture)
	audioGroup.POST("/capture/stop", audioHandler.StopCapture)
	audioGroup.PUT("/config", audioHandler.SetConfig)
	audioGroup.POST("/playback/start", audioHandler.StartPlayback)
	audioGroup.POST("/playback/stop", audioHandler.StopPlayback)
	audioGroup.GET("/stream", audioHandler.HandleAudioStreamWebSocket)
	// Two-way talk: browser mic PCM → device speaker (push-to-talk).
	audioGroup.GET("/talk", audioHandler.HandleAudioTalkWebSocket)

	// H264 streaming endpoints (for MSE playback)
	h264 := api.Group("/h264")
	h264.GET("/:stream_id", streamHandlers.HandleH264WebSocket)

	// Swagger UI - API documentation
	s.setupSwaggerRoutes()

	// Static file serving (must be registered last via NoRoute to avoid wildcard conflicts)
	if s.config.Web.StaticPath != "" {
		staticPath := s.config.Web.StaticPath

		s.engine.NoRoute(func(c *gin.Context) {
			path := c.Request.URL.Path
			// Sanitize path to prevent directory traversal
			cleanPath := filepath.Clean(path)
			filePath := filepath.Join(staticPath, cleanPath)
			// Ensure resolved path is within static root
			if !strings.HasPrefix(filePath, staticPath) {
				c.JSON(403, gin.H{"code": 403, "message": "Forbidden"})
				return
			}
			// Root path, directory, or index.html -> serve with no-cache
			isHTML := cleanPath == "." || cleanPath == "/" || cleanPath == "/index.html"
			if isHTML {
				data, err := os.ReadFile(staticPath + "/index.html")
				if err != nil {
					c.JSON(500, gin.H{"code": 500, "message": "Internal error"})
					return
				}
				c.Writer.Header().Set("Content-Type", "text/html; charset=utf-8")
				c.Writer.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
				c.Writer.WriteHeader(http.StatusOK)
				c.Writer.Write(data)
				return
			}
			// Check if file exists and is not a directory
			info, err := os.Stat(filePath)
			if err == nil && !info.IsDir() {
				// Ensure correct MIME type for JS files - AudioWorklet
				// addModule() rejects responses that are not application/javascript.
				if strings.HasSuffix(cleanPath, ".js") {
					c.Writer.Header().Set("Content-Type", "application/javascript; charset=utf-8")
				}
				c.File(filePath)
				return
			}
			// For SPA routing, return index.html for non-API paths
			if !strings.HasPrefix(path, "/api/") {
				data, err := os.ReadFile(staticPath + "/index.html")
				if err != nil {
					c.JSON(500, gin.H{"code": 500, "message": "Internal error"})
					return
				}
				c.Writer.Header().Set("Content-Type", "text/html; charset=utf-8")
				c.Writer.Header().Set("Cache-Control", "no-cache, no-store, must-revalidate")
				c.Writer.WriteHeader(http.StatusOK)
				c.Writer.Write(data)
				return
			}
			c.JSON(404, gin.H{"code": 404, "message": "Not found"})
		})
	}
}

func (s *PlatformAPIServer) setupSwaggerRoutes() {
	// Serve swagger.yaml
	s.engine.GET("/api/v1/swagger.yaml", func(c *gin.Context) {
		c.Header("Content-Type", "text/yaml")
		c.Header("Access-Control-Allow-Origin", "*")
		c.File(constants.ConfigPath() + "/swagger.yaml")
	})

	// Serve Swagger UI static files
	swaggerUIPath := constants.RootPath() + "/swagger-ui"
	s.engine.Static("/swagger", swaggerUIPath)

	logger.Info("Swagger UI available at /swagger/")
}

// buildGyroSource constructs and starts the on-board IMU attitude source when
// enabled. It returns nil when disabled (the SSE handler then returns 503 while
// the other monitor endpoints keep working). A missing sensor is non-fatal: the
// source probes lazily in its own goroutine and simply reports offline, so the
// rest of the server is unaffected.
func (s *PlatformAPIServer) buildGyroSource() gyro.Source {
	cfg := s.config.Gyro
	if !cfg.Enabled {
		logger.Info("gyro source disabled by config")
		return nil
	}
	mount := cfg.MountMatrix
	if mount == ([9]float64{}) {
		mount = [9]float64{1, 0, 0, 0, 1, 0, 0, 0, 1} // identity
	}
	mount, gyroBias, hasGyroBias, loadedCalibration, err := resolveGyroCalibration(cfg, mount)
	if err != nil {
		logger.Warn("gyro calibration file not used (%s): %v", cfg.CalibrationPath, err)
	} else if loadedCalibration {
		logger.Info("gyro calibration loaded: %s (mount=%v, bias=%v)", cfg.CalibrationPath, cfg.UseCalibrationMount, cfg.UseCalibrationBias)
	} else if cfg.CalibrationPath != "" {
		logger.Warn("gyro calibration file configured but ignored until use_calibration_mount or use_calibration_bias is true: %s", cfg.CalibrationPath)
	}
	src := gyro.NewIIOSource(gyro.IIOSourceConfig{
		PollRateHz:    cfg.PollRateHz,
		ODRHz:         cfg.ODRHz,
		FusionAlpha:   cfg.FusionAlpha,
		MountMatrix:   mount,
		GyroBias:      gyroBias,
		HasGyroBias:   hasGyroBias,
		AccelPath:     cfg.AccelPath,
		GyroPath:      cfg.GyroPath,
		IIOBase:       cfg.IIOBase,
		CalibrateBias: cfg.CalibrateBias,
		CalibrationMs: cfg.CalibrationMs,
	})
	ctx, cancel := context.WithCancel(context.Background())
	s.gyroCancel = cancel
	go src.Start(ctx)
	logger.Info("gyro source started (poll_rate_hz=%d, odr_hz=%d, fusion_alpha=%.3f, calibrate=%v, calibration_ms=%d)",
		cfg.PollRateHz, cfg.ODRHz, cfg.FusionAlpha, cfg.CalibrateBias, cfg.CalibrationMs)
	return src
}

func resolveGyroCalibration(cfg GyroConfig, mount [9]float64) (resolvedMount [9]float64, gyroBias [3]float64, hasGyroBias bool, loaded bool, err error) {
	resolvedMount = mount
	if cfg.CalibrationPath == "" || (!cfg.UseCalibrationMount && !cfg.UseCalibrationBias) {
		return resolvedMount, gyroBias, false, false, nil
	}

	cal, err := gyro.LoadCalibrationFile(cfg.CalibrationPath)
	if err != nil {
		return resolvedMount, gyroBias, false, false, err
	}
	if cfg.UseCalibrationMount {
		resolvedMount = cal.MountMatrix
	}
	if cfg.UseCalibrationBias {
		gyroBias = cal.GyroBias
		hasGyroBias = true
	}
	return resolvedMount, gyroBias, hasGyroBias, true, nil
}

func (s *PlatformAPIServer) Start() error {
	logger.Info("Starting Platform API server on %s", s.config.Service.HTTPAddr)

	// Handle shutdown gracefully
	sigChan := make(chan os.Signal, 1)
	signal.Notify(sigChan, os.Interrupt, syscall.SIGTERM)

	go func() {
		<-sigChan
		logger.Info("Shutting down Platform API server...")

		// Stop persist loop
		if s.persistCancel != nil {
			s.persistCancel()
		}
		// Stop gyro source read loop
		if s.gyroCancel != nil {
			s.gyroCancel()
		}
		// Stop background CPU sampler goroutine
		if s.monitor != nil {
			s.monitor.Stop()
		}
		ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
		defer cancel()

		if err := s.httpServer.Shutdown(ctx); err != nil {
			logger.Error("Error shutting down server: %v", err)
		}
		if s.tlsServer != nil {
			if err := s.tlsServer.Shutdown(ctx); err != nil {
				logger.Error("Error shutting down TLS server: %v", err)
			}
		}

		// Close gRPC connections
		if s.grpcClients.aiRuntime != nil {
			s.grpcClients.aiRuntime.Close()
		}
		if s.grpcClients.eventBus != nil {
			s.grpcClients.eventBus.Close()
		}
		if s.grpcClients.deviceControl != nil {
			s.grpcClients.deviceControl.Close()
		}
		if s.grpcClients.appManager != nil {
			s.grpcClients.appManager.Close()
		}
		if s.grpcClients.cameraControl != nil {
			s.grpcClients.cameraControl.Close()
		}
		if s.eventStream != nil {
			s.eventStream.Stop()
		}
	}()

	// TLS enabled: serve HTTPS in the background. The HTTP listener either
	// 301-redirects to HTTPS (RedirectHTTP, migrating old :8080 bookmarks) or
	// keeps serving the engine (dual-stack). httpServer.ListenAndServe remains
	// the blocking call so the process lifecycle is unchanged; a fatal TLS bind
	// error is surfaced via the log rather than aborting startup.
	if s.tlsServer != nil {
		if s.config.Service.TLS.RedirectHTTP {
			s.httpServer.Handler = httpRedirectHandler(s.config.Service.TLS.HTTPSAddr)
		}
		go func() {
			if err := s.tlsServer.ListenAndServeTLS(s.tlsCertFile, s.tlsKeyFile); err != nil && err != http.ErrServerClosed {
				logger.Error("TLS server stopped unexpectedly: %v", err)
			}
		}()
		logger.Info("HTTPS listener started on %s", s.config.Service.TLS.HTTPSAddr)
	}

	if err := s.httpServer.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		return fmt.Errorf("failed to start server: %w", err)
	}

	return nil
}

func main() {
	flag.Parse()

	// Load configuration
	var cfg Config
	if err := config.LoadYAML(*configPath, &cfg); err != nil {
		fmt.Fprintf(os.Stderr, "Failed to load config: %v\n", err)
		os.Exit(1)
	}

	// Auth secrets: allow env override so the same config file can ship without
	// baked-in credentials. Env wins over the YAML value; this is intentional so
	// operators can rotate secrets without editing the on-disk file.
	if v := os.Getenv("AIPC_TOKEN_KEY"); v != "" {
		cfg.Auth.TokenKey = v
	}
	if v := os.Getenv("AIPC_AUTH_USERNAME"); v != "" {
		cfg.Auth.Username = v
	}
	if v := os.Getenv("AIPC_AUTH_PASSWORD"); v != "" {
		cfg.Auth.Password = v
	}
	// Never run with an empty token key: a missing key would otherwise fall back
	// to the insecure hardcoded default in handlers.Login. Generate a strong
	// random secret at startup and surface it in the log once so the operator
	// can retrieve it for the first login, then keep rotating via env.
	if cfg.Auth.Enabled && cfg.Auth.TokenKey == "" {
		buf := make([]byte, 32)
		if _, err := rand.Read(buf); err != nil {
			logger.Fatal("Failed to generate auth token key: %v", err)
		}
		cfg.Auth.TokenKey = hex.EncodeToString(buf)
		logger.Info("AUTH: no token_key configured; generated a random one for this boot (set AIPC_TOKEN_KEY to persist across restarts)")
	}

	// Setup logger
	logger.SetLevelFromString(cfg.Service.LogLevel)

	// Set root path FIRST so that log file remapping uses the correct prefix
	if cfg.Storage.RootPath != "" {
		constants.SetRootPath(cfg.Storage.RootPath)
	}

	// Configure log file output if specified
	if cfg.Service.LogFile != "" {
		if err := logger.SetOutputFile(cfg.Service.LogFile); err != nil {
			fmt.Fprintf(os.Stderr, "Failed to set log file: %v\n", err)
		} else {
			logger.Info("Logging to file: %s", cfg.Service.LogFile)
		}
	}

	logger.Info("Starting %s", cfg.Service.Name)
	logger.Info("Config file: %s", *configPath)
	logger.Info("HTTP address: %s", cfg.Service.HTTPAddr)
	logger.Info("HTTP read timeout: %ds", cfg.Service.ReadTimeoutSeconds)

	// Create and start server
	server, err := NewPlatformAPIServer(&cfg)
	if err != nil {
		logger.Fatal("Failed to create server: %v", err)
	}

	if err := server.Start(); err != nil {
		logger.Fatal("Failed to start server: %v", err)
	}
}
