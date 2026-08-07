package constants

import (
	"os"
	"path/filepath"
	"time"
)

// Service connection timeouts
const (
	DefaultServiceTimeout   = 5 * time.Second
	EventPublishTimeout     = 2 * time.Second
	InferenceRequestTimeout = 30 * time.Second
	ContainerStartTimeout   = 60 * time.Second
	ContainerStopTimeout    = 10 * time.Second
)

// Queue and buffer sizes
const (
	InferenceQueueSize  = 100
	EventBusChannelSize = 100
	LogChannelSize      = 100
	WebSocketBufferSize = 256
)

// Retry configuration
const (
	MaxRetries        = 3
	DefaultRetryDelay = 1 * time.Second
	MaxRetryDelay     = 30 * time.Second
)

// Resource limits
const (
	DefaultMaxQPS      = 30
	DefaultMaxFPS      = 30
	DefaultWorkerCount = 4
)

// File paths
const (
	DefaultRootPath   = "/data/aipc"
	DefaultConfigPath = DefaultRootPath + "/etc"
	DefaultLogPath    = DefaultRootPath + "/logs"
	DefaultDataPath   = DefaultRootPath + "/data"
	DefaultSHMPath    = "/run/aipc/shm"
)

// rootPath can be overridden at startup via SetRootPath, or at process start
// via the AIPC_ROOT environment variable (handy for tests and on-device
// debugging, e.g. pointing back at /opt/aipc on a not-yet-migrated device).
var rootPath = DefaultRootPath

func init() {
	if env := os.Getenv("AIPC_ROOT"); env != "" {
		if filepath.IsAbs(env) {
			rootPath = filepath.Clean(env)
		}
	}
}

// SetRootPath overrides the global install prefix (e.g. "/data/aipc").
func SetRootPath(p string) { rootPath = p }

// RootPath returns the current install prefix.
func RootPath() string { return rootPath }

// Helper derivations using the active rootPath.
func ConfigPath() string { return rootPath + "/etc" }
func LogPath() string    { return rootPath + "/logs" }
func DataPath() string   { return rootPath + "/data" }
func ModelsPath() string { return rootPath + "/models" }
func BinPath() string    { return rootPath + "/bin" }
func WebPath() string    { return rootPath + "/web" }
func LibPath() string    { return rootPath + "/lib" }
