// Package stopbudget defines the shared deadline contract for stopping apps.
package stopbudget

import "time"

const (
	// DefaultGracefulTimeoutSeconds is the per-container SIGTERM window used
	// when callers omit or provide a non-positive timeout.
	DefaultGracefulTimeoutSeconds int32 = 10

	// MaxContainersPerApp bounds synchronous StopApp/RestartApp latency. Both
	// manifest validation and the platform API's worst-case RPC deadline use
	// this value, so the API cannot time out before app-manager finishes an
	// ordered multi-container stop.
	MaxContainersPerApp = 16

	// KillOpTimeout bounds each individual containerd Kill/Delete call in the
	// force-kill phase.
	KillOpTimeout = 10 * time.Second
	// SigkillExitWait is how long a task is given to exit after SIGKILL before
	// deletion is escalated to WithProcessKill.
	SigkillExitWait = 5 * time.Second
	// KillPhaseBudget is the worst-case duration of one container's force-kill
	// phase.
	KillPhaseBudget = KillOpTimeout + SigkillExitWait + KillOpTimeout

	cleanupSlack       = 5 * time.Second
	restartStartBudget = 30 * time.Second
)

// NormalizeGracefulTimeoutSeconds applies the StopApp default consistently at
// every API boundary.
func NormalizeGracefulTimeoutSeconds(seconds int32) int32 {
	if seconds <= 0 {
		return DefaultGracefulTimeoutSeconds
	}
	return seconds
}

// ForContainers returns a deadline that covers sequential ordered shutdown of
// containerCount tasks. Each task can consume its graceful window and the full
// detached force-kill phase before the next task starts.
func ForContainers(gracefulSeconds int32, containerCount int) time.Duration {
	if containerCount < 1 {
		containerCount = 1
	}
	graceful := time.Duration(NormalizeGracefulTimeoutSeconds(gracefulSeconds)) * time.Second
	perContainer := graceful + KillPhaseBudget
	const maxDuration = time.Duration(1<<63 - 1)
	if perContainer > (maxDuration-cleanupSlack)/time.Duration(containerCount) {
		return maxDuration
	}
	return time.Duration(containerCount)*perContainer + cleanupSlack
}

// ForAPI returns the platform API deadline for StopApp. The API cannot inspect
// app-manager's in-memory task set, so it budgets for the validated maximum.
func ForAPI(gracefulSeconds int32) time.Duration {
	return ForContainers(gracefulSeconds, MaxContainersPerApp)
}

// ForRestartAPI also covers the StartApp phase following a successful stop.
func ForRestartAPI(gracefulSeconds int32) time.Duration {
	return ForAPI(gracefulSeconds) + restartStartBudget
}
