package handlers

import (
	"context"
	"crypto/rsa"
	"time"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"
	"gorm.io/gorm"

	inferencepb "aipc/platform/ai-runtime/proto"
	"aipc/platform/common/constants"
	eventLoggerPkg "aipc/platform/common/events"
	authadapter "aipc/platform/platform-api/adapters/auth"
	"aipc/platform/platform-api/adapters/deviceinfo"
	"aipc/platform/platform-api/adapters/media"
	"aipc/platform/platform-api/adapters/network"
	"aipc/platform/platform-api/adapters/systemparams"
	timeadapter "aipc/platform/platform-api/adapters/time"
	"aipc/platform/platform-api/auth"
	"aipc/platform/platform-api/config"
	"aipc/platform/platform-api/repo"
	"aipc/platform/platform-api/storage"
)

// GRPCClients holds gRPC client connections
type GRPCClients struct {
	AIRuntime     *grpc.ClientConn
	EventBus      *grpc.ClientConn
	DeviceControl *grpc.ClientConn
	AppManager    *grpc.ClientConn
}

// APIHandlers contains gRPC clients and database repos for handling requests
type APIHandlers struct {
	grpcClients    *GRPCClients
	modelStorage   string
	modelStore     *storage.ModelStorage
	db             *gorm.DB
	aiModelRepo    *repo.AIModelRepo
	settingRepo    *repo.SettingRepo
	configRepo     *repo.ConfigRepo
	configMgr      *config.Manager
	authUser       string
	authPass       string
	tokenValidator *auth.TokenValidator
	rsaPriv        *rsa.PrivateKey // decrypts frontend-encrypted passwords; nil -> plaintext fallback
	rsaPubPEM      string          // served verbatim by GetPublicKey
	eventLogger    *eventLoggerPkg.Logger
}

// NewAPIHandlers creates a new API handlers instance
func NewAPIHandlers(clients *GRPCClients, modelStorage string, modelStore *storage.ModelStorage, db *gorm.DB, authUser, authPass string, tokenValidator *auth.TokenValidator, rsaPriv *rsa.PrivateKey, rsaPubPEM string, eventLogger *eventLoggerPkg.Logger) *APIHandlers {
	h := &APIHandlers{
		grpcClients:    clients,
		modelStorage:   modelStorage,
		modelStore:     modelStore,
		db:             db,
		authUser:       authUser,
		authPass:       authPass,
		tokenValidator: tokenValidator,
		rsaPriv:        rsaPriv,
		rsaPubPEM:      rsaPubPEM,
		eventLogger:    eventLogger,
	}
	if db != nil {
		h.aiModelRepo = repo.NewAIModelRepo(db)
		h.settingRepo = repo.NewSettingRepo(db)
		// Config Controller: desired-state DB + per-domain adapters. The system
		// domain (settings KV) is registered now; file-projecting domains
		// (media/network/time/device_info/auth) register as their adapters land.
		h.configRepo = repo.NewConfigRepo(db)
		h.configMgr = config.NewManager(h.configRepo, eventLogger)
		h.configMgr.Register("system", systemparams.New(h.settingRepo))
		// device_info: device name → hostname + device.conf (file+exec adapter).
		h.configMgr.Register("device_info", deviceinfo.New(constants.ConfigPath()+"/device.conf"))
		// network: per-interface .network + /etc/network/interfaces projection.
		// The async restart+rollback stays in the handler post-Apply (the
		// management interface cannot be bounced synchronously inside Verify).
		h.configMgr.Register("network", network.New("", ""))
		// time: /etc/timezone + timesyncd.conf + time-config.json projection. The
		// live reconfiguration (timedatectl set-timezone/set-time, systemctl
		// restart systemd-timesyncd, NTP-state verify) stays in the handler
		// post-Apply — the adapter only projects files.
		h.configMgr.Register("time", timeadapter.New("", "", ""))
		// media: camera-daemon.yaml (encoders/streams/rtsp). Single-key "config":
		// desired is the full YAML the handler already marshals. The live pipeline
		// reconfigure (gRPC UpdateEncoderConfig / ReloadStreams) stays in the
		// handler post-Apply — the adapter only projects the file atomically.
		h.configMgr.Register("media", media.New(""))
		// auth: platform-api.yaml (username/password/token_key/enabled). Single-key
		// "config": desired is the full YAML the handler marshals. The restart so
		// the new password takes effect stays in the handler post-Apply, plus a
		// detached systemd-run probe that restores the pre-Apply backup if the
		// service fails to come up (R-auth-rollback — see rollback_probe.go).
		h.configMgr.Register("auth", authadapter.New(""))
	}
	return h
}

// ConfigManager returns the Config Controller Manager, or nil when no DB was
// provided (the apply state machine is disabled). Handlers constructed outside
// APIHandlers (device_info, network, time, auth) use this to route their writes
// through the state machine for revision history + audit + auto-restore.
func (h *APIHandlers) ConfigManager() *config.Manager {
	return h.configMgr
}

// System handlers

func (h *APIHandlers) GetSystemInfo(c *gin.Context) {
	info := map[string]interface{}{
		"version": "0.1.0",
		"services": map[string]bool{
			"ai-runtime":     h.grpcClients.AIRuntime != nil,
			"event-bus":      h.grpcClients.EventBus != nil,
			"device-control": h.grpcClients.DeviceControl != nil,
			"app-manager":    h.grpcClients.AppManager != nil,
		},
	}

	Resp(c).OK(info)
}

func (h *APIHandlers) GetSystemStats(c *gin.Context) {
	stats := map[string]interface{}{
		"timestamp": time.Now().Unix(),
		"services":  map[string]interface{}{},
	}

	// Get stats from AI Runtime
	if h.grpcClients.AIRuntime != nil {
		client := inferencepb.NewInferenceServiceClient(h.grpcClients.AIRuntime)
		ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()

		aiStats, err := client.GetStats(ctx, &inferencepb.Empty{})
		if err == nil {
			stats["services"].(map[string]interface{})["ai-runtime"] = aiStats
		}
	}

	Resp(c).OK(stats)
}

func (h *APIHandlers) HealthCheck(c *gin.Context) {
	health := map[string]interface{}{
		"status":  "healthy",
		"time":    time.Now().Unix(),
		"service": "platform-api",
	}

	Resp(c).OK(health)
}
