package manifest

import (
	"fmt"
	"strings"
	"testing"

	"aipc/platform/app-manager/stopbudget"
)

func TestValidateMultiContainerRejectsMoreThanStopBudgetLimit(t *testing.T) {
	containers := make(map[string]ContainerSpec, stopbudget.MaxContainersPerApp+1)
	containers["main"] = ContainerSpec{Role: "main"}
	for i := 0; i < stopbudget.MaxContainersPerApp; i++ {
		containers[fmt.Sprintf("sub-%d", i)] = ContainerSpec{Role: "sub"}
	}

	m := &AppManifest{Spec: Spec{Containers: containers}}
	err := m.ValidateMultiContainer()
	if err == nil || !strings.Contains(err.Error(), "maximum is") {
		t.Fatalf("ValidateMultiContainer() error = %v, want maximum-container error", err)
	}
}
