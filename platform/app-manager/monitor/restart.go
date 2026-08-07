package monitor

import (
	"context"
	"fmt"
	"sync"
	"time"

	"aipc/platform/app-manager/containerd"
	"aipc/platform/app-manager/manifest"
	"aipc/platform/app-manager/registry"
	"aipc/platform/common/logger"

	containerdclient "github.com/containerd/containerd"
	"github.com/containerd/containerd/namespaces"
)

// RestartPolicy defines restart behavior
type RestartPolicy struct {
	Enabled           bool
	MaxRetries        int
	RetryDelay        time.Duration
	BackoffMultiplier float64
}

// AutoRestartManager manages automatic container restarts
type AutoRestartManager struct {
	client        *containerd.Client
	runtime       *containerd.Runtime
	registry      *registry.Registry
	healthChecker *HealthChecker
	namespace     string

	monitoredApps map[string]*MonitoredApp
	mu            sync.RWMutex

	// Callback when app max retries exceeded
	OnGiveUpFn func(appID string, manifestPath string)

	stopCh chan struct{}
	wg     sync.WaitGroup
}

// MonitoredApp tracks a monitored application
type MonitoredApp struct {
	AppID        string
	Container    containerdclient.Container
	Manifest     *manifest.AppManifest
	ManifestPath string
	RestartCount int
	LastFailure  time.Time
	StopCh       chan struct{}
}

// NewAutoRestartManager creates a new auto-restart manager
func NewAutoRestartManager(client *containerd.Client, runtime *containerd.Runtime, reg *registry.Registry, namespace string) *AutoRestartManager {
	healthChecker := NewHealthChecker(client)

	return &AutoRestartManager{
		client:        client,
		runtime:       runtime,
		registry:      reg,
		healthChecker: healthChecker,
		namespace:     namespace,
		monitoredApps: make(map[string]*MonitoredApp),
		stopCh:        make(chan struct{}),
	}
}

// Start begins monitoring and auto-restart
func (m *AutoRestartManager) Start(ctx context.Context) {
	logger.Info("Starting auto-restart manager")

	m.wg.Add(1)
	go m.monitorLoop(ctx)
}

// Stop stops monitoring
func (m *AutoRestartManager) Stop() {
	logger.Info("Stopping auto-restart manager")
	close(m.stopCh)
	m.wg.Wait()
}

// AddApp adds an app to monitoring
func (m *AutoRestartManager) AddApp(ctx context.Context, appID string, container containerdclient.Container, appManifest *manifest.AppManifest, manifestPath string) error {
	m.mu.Lock()
	defer m.mu.Unlock()

	// Check if app should be auto-restarted
	if !appManifest.IsAutoRestartEnabled() {
		return nil // No monitoring needed
	}

	monitored := &MonitoredApp{
		AppID:        appID,
		Container:    container,
		Manifest:     appManifest,
		ManifestPath: manifestPath,
		RestartCount: 0,
		StopCh:       make(chan struct{}),
	}

	m.monitoredApps[appID] = monitored

	// Start monitoring goroutine for this app
	// Use background context instead of request context to avoid cancellation
	m.wg.Add(1)
	go m.monitorApp(context.Background(), monitored)

	logger.Info("Added app to auto-restart monitoring: app_id=%s", appID)
	return nil
}

// RemoveApp removes an app from monitoring
func (m *AutoRestartManager) RemoveApp(appID string) {
	m.mu.Lock()
	defer m.mu.Unlock()

	if monitored, exists := m.monitoredApps[appID]; exists {
		close(monitored.StopCh)
		delete(m.monitoredApps, appID)
		logger.Info("Removed app from auto-restart monitoring: app_id=%s", appID)
	}
}

// monitorLoop is the main monitoring loop
func (m *AutoRestartManager) monitorLoop(ctx context.Context) {
	defer m.wg.Done()

	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()

	for {
		select {
		case <-m.stopCh:
			return
		case <-ticker.C:
			m.checkAllApps(ctx)
		}
	}
}

// checkAllApps checks all monitored apps
func (m *AutoRestartManager) checkAllApps(ctx context.Context) {
	m.mu.RLock()
	apps := make([]*MonitoredApp, 0, len(m.monitoredApps))
	for _, app := range m.monitoredApps {
		apps = append(apps, app)
	}
	m.mu.RUnlock()

	for _, app := range apps {
		m.checkApp(ctx, app)
	}
}

// monitorApp monitors a single app
func (m *AutoRestartManager) monitorApp(ctx context.Context, app *MonitoredApp) {
	defer m.wg.Done()

	interval := time.Duration(app.Manifest.Spec.AutoRestart.HealthCheckIntervalSeconds) * time.Second
	if interval == 0 {
		interval = 30 * time.Second // Default 30 seconds
	}

	ticker := time.NewTicker(interval)
	defer ticker.Stop()

	for {
		select {
		case <-app.StopCh:
			return
		case <-m.stopCh:
			return
		case <-ticker.C:
			m.checkApp(ctx, app)
		}
	}
}

// checkApp checks if an app is healthy and restarts if needed
func (m *AutoRestartManager) checkApp(ctx context.Context, app *MonitoredApp) {
	// Ensure namespace is set in context for containerd operations
	ctxWithNamespace := namespaces.WithNamespace(ctx, m.namespace)

	// Check if container is running
	task, err := app.Container.Task(ctxWithNamespace, nil)
	if err != nil {
		// Container is not running, check if we should restart
		logger.Debug("Task not found for app %s: %v", app.AppID, err)
		if app.Manifest.IsAutoRestartEnabled() {
			m.handleContainerDown(ctxWithNamespace, app)
		}
		return
	}

	// Check task status
	status, err := task.Status(ctxWithNamespace)
	if err != nil {
		logger.Warn("Failed to get task status for app %s: %v", app.AppID, err)
		return
	}

	// If task exited, check if we should restart
	// Check if task is still running by checking status
	if status.Status != containerdclient.Running {
		// Task is not running (exited or stopped)
		logger.Info("Task for app %s is not running (status: %v), will restart", app.AppID, status.Status)
		if app.Manifest.IsAutoRestartEnabled() {
			m.handleContainerDown(ctx, app)
		}
		return
	}

	// Perform health check if configured
	if app.Manifest.Spec.Healthcheck.Enabled {
		healthy, err := m.healthChecker.CheckHealth(ctxWithNamespace, app.Container, app.Manifest.Spec.Healthcheck)
		if err != nil {
			logger.Warn("Health check failed for app %s: %v", app.AppID, err)
		}
		if !healthy {
			logger.Warn("App %s is unhealthy, will restart", app.AppID)
			m.handleContainerDown(ctxWithNamespace, app)
		}
	}
}

// handleContainerDown handles a container that has stopped
func (m *AutoRestartManager) handleContainerDown(ctx context.Context, app *MonitoredApp) {
	// Check max retries
	maxRetries := app.Manifest.EffectiveRestartMaxRetries()
	if maxRetries > 0 && app.RestartCount >= maxRetries {
		logger.Error("App %s exceeded max retries (%d), stopping auto-restart", app.AppID, maxRetries)
		m.RemoveApp(app.AppID)
		if m.OnGiveUpFn != nil {
			m.OnGiveUpFn(app.AppID, app.ManifestPath)
		}
		return
	}

	// Calculate backoff delay
	delay := time.Duration(app.Manifest.Spec.AutoRestart.RetryDelaySeconds) * time.Second
	if delay == 0 {
		delay = 5 * time.Second // Default 5 seconds
	}

	// Apply backoff multiplier
	if app.RestartCount > 0 {
		multiplier := app.Manifest.Spec.AutoRestart.BackoffMultiplier
		if multiplier == 0 {
			multiplier = 1.5 // Default 1.5x
		}
		for i := 0; i < app.RestartCount; i++ {
			delay = time.Duration(float64(delay) * multiplier)
		}
		// Cap at 5 minutes
		if delay > 5*time.Minute {
			delay = 5 * time.Minute
		}
	}

	logger.Info("Scheduling restart for app %s in %v (attempt %d/%d)",
		app.AppID, delay, app.RestartCount+1, maxRetries)

	// Wait before restarting (cancellable via StopCh or global stopCh)
	timer := time.NewTimer(delay)
	defer timer.Stop()
	select {
	case <-timer.C:
		// Proceed with restart
	case <-app.StopCh:
		logger.Info("Restart cancelled for app %s (app removed)", app.AppID)
		return
	case <-m.stopCh:
		logger.Info("Restart cancelled for app %s (manager stopping)", app.AppID)
		return
	}

	// Restart container
	if err := m.restartContainer(ctx, app); err != nil {
		logger.Error("Failed to restart app %s: %v", app.AppID, err)
		app.LastFailure = time.Now()
		app.RestartCount++
	} else {
		logger.Info("Successfully restarted app %s", app.AppID)
		app.RestartCount = 0 // Reset on success
	}
}

// restartContainer restarts a container
func (m *AutoRestartManager) restartContainer(ctx context.Context, app *MonitoredApp) error {
	// Ensure namespace is set in context for containerd operations
	ctxWithNamespace := namespaces.WithNamespace(ctx, m.namespace)

	// Ensure any existing task is cleaned up before restarting
	// This prevents "task already exists" errors
	task, err := app.Container.Task(ctxWithNamespace, nil)
	if err == nil {
		// Task exists, stop and delete it
		logger.Info("Stopping existing task for app %s before restart", app.AppID)
		timeout := 10 * time.Second
		if err := m.runtime.StopAppContainer(ctxWithNamespace, task, int32(timeout.Seconds())); err != nil {
			logger.Warn("Failed to stop container before restart: %v, attempting force delete", err)
			if _, delErr := task.Delete(ctxWithNamespace, containerdclient.WithProcessKill); delErr != nil {
				logger.Warn("Failed to force delete task: %v", delErr)
			}
		}
	}

	// Start container
	newTask, err := m.runtime.StartAppContainer(ctxWithNamespace, app.Container, app.AppID)
	if err != nil {
		return fmt.Errorf("failed to start container: %w", err)
	}

	// Update registry
	if err := m.registry.IncrementRestartCount(app.AppID); err != nil {
		logger.Warn("Failed to update restart count: %v", err)
	}

	logger.Info("Container restarted: app_id=%s, task_id=%s", app.AppID, newTask.ID())
	return nil
}
