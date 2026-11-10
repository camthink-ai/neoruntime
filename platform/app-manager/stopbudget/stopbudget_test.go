package stopbudget

import (
	"math"
	"testing"
	"time"
)

func TestForContainersScalesSequentialStopBudget(t *testing.T) {
	want := 2*(10*time.Second+KillPhaseBudget) + cleanupSlack
	if got := ForContainers(10, 2); got != want {
		t.Fatalf("ForContainers(10, 2) = %s, want %s", got, want)
	}
}

func TestForContainersSaturatesInsteadOfOverflowing(t *testing.T) {
	if got := ForContainers(math.MaxInt32, MaxContainersPerApp); got <= 0 {
		t.Fatalf("ForContainers(MaxInt32, MaxContainersPerApp) overflowed to %s", got)
	}
}

func TestForContainersNormalizesInputs(t *testing.T) {
	want := time.Duration(DefaultGracefulTimeoutSeconds)*time.Second + KillPhaseBudget + cleanupSlack
	if got := ForContainers(-1, 0); got != want {
		t.Fatalf("ForContainers(-1, 0) = %s, want %s", got, want)
	}
}

func TestAPIBudgetsCoverValidatedMaximum(t *testing.T) {
	stopBudget := ForContainers(23, MaxContainersPerApp)
	if got := ForAPI(23); got != stopBudget {
		t.Fatalf("ForAPI(23) = %s, want %s", got, stopBudget)
	}
	if got := ForStartAPI(); got != ImageRecoveryBudget+startOperationSlack {
		t.Fatalf("ForStartAPI() = %s, want %s", got, ImageRecoveryBudget+startOperationSlack)
	}
	if got := ForRestartAPI(23); got != stopBudget+ForStartAPI() {
		t.Fatalf("ForRestartAPI(23) = %s, want %s", got, stopBudget+ForStartAPI())
	}
}
