package server

import (
	"errors"
	"testing"

	"aipc/platform/app-manager/proto"
)

func TestCreateOperationReplacesStableLifecycleTask(t *testing.T) {
	store := NewInstallTaskStore()
	first := store.CreateOperation("start-demo", "checking_image", 5, "checking")
	first.Update("restoring_image", 20, "restoring")

	second := store.CreateOperation("start-demo", "checking_image", 5, "checking again")
	got, ok := store.Get("start-demo")
	if !ok || got != second || got == first {
		t.Fatal("stable lifecycle task was not replaced by the new operation")
	}
	phase, percent, message, _, _ := got.Snapshot()
	if phase != "checking_image" || percent != 5 || message != "checking again" {
		t.Fatalf("replacement snapshot = (%q, %.0f, %q)", phase, percent, message)
	}
}

func TestFinishStartProgressReportsOutcome(t *testing.T) {
	tests := []struct {
		name      string
		response  *proto.Status
		rpcErr    error
		wantPhase string
		wantError string
	}{
		{name: "success", response: &proto.Status{Success: true}, wantPhase: "complete"},
		{name: "status failure", response: &proto.Status{Success: false, Message: "image unavailable"}, wantPhase: "error", wantError: "image unavailable"},
		{name: "rpc failure", rpcErr: errors.New("context deadline exceeded"), wantPhase: "error", wantError: "context deadline exceeded"},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			task := &InstallTask{ID: "start-demo", Phase: "checking_image"}
			finishStartProgress(task, "demo", tt.response, tt.rpcErr)
			phase, percent, _, appID, errMsg := task.Snapshot()
			if phase != tt.wantPhase || errMsg != tt.wantError {
				t.Fatalf("result phase/error = %q/%q, want %q/%q", phase, errMsg, tt.wantPhase, tt.wantError)
			}
			if tt.wantPhase == "complete" && (percent != 100 || appID != "demo") {
				t.Fatalf("completion = %.0f%% app=%q, want 100%% app=demo", percent, appID)
			}
		})
	}
}
