package handlers

import (
	"context"
	"net"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/test/bufconn"
	"google.golang.org/protobuf/types/known/emptypb"

	apppb "aipc/platform/app-manager/proto"
	"aipc/platform/platform-api/model"
)

// fakeAppLister answers ListApps with a fixed app table so model reference
// counting can be exercised without a container runtime. (The package
// already has a fakeAppManager; this one exists purely for ListApps.)
type fakeAppLister struct {
	apppb.UnimplementedAppManagerServer
	apps []*apppb.AppInfo
}

func (f *fakeAppLister) ListApps(context.Context, *emptypb.Empty) (*apppb.AppList, error) {
	return &apppb.AppList{Apps: f.apps}, nil
}

func writeAppManifest(t *testing.T, content string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "app.yaml")
	if err := os.WriteFile(path, []byte(content), 0644); err != nil {
		t.Fatalf("write manifest: %v", err)
	}
	return path
}

func newAppsUsingModelEnv(t *testing.T, apps []*apppb.AppInfo) (*APIHandlers, *fakeAIRuntime) {
	t.Helper()
	h, fake, _ := newAIUpdateTestEnv(t)

	lis := bufconn.Listen(1024 * 1024)
	srv := grpc.NewServer()
	apppb.RegisterAppManagerServer(srv, &fakeAppLister{apps: apps})
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)

	conn, err := grpc.DialContext(context.Background(), "bufnet",
		grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) { return lis.Dial() }),
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		t.Fatalf("dial app-manager bufnet: %v", err)
	}
	t.Cleanup(func() { conn.Close() })

	h.grpcClients.AppManager = conn
	return h, fake
}

// deleteModel drives the delete endpoint without a live gin engine.
func deleteModel(t *testing.T, h *APIHandlers, modelID string) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Params = gin.Params{{Key: "model_id", Value: modelID}}
	c.Request = httptest.NewRequest(http.MethodDelete, "/api/v1/ai/models/"+modelID, nil)
	h.UnregisterModel(c)
	return w
}

// A stopped app whose manifest names the model still blocks deletion — the
// app would fail to start again if its model were removed.
func TestGetAppsUsingModelCountsStoppedAppReference(t *testing.T) {
	manifest := writeAppManifest(t, `spec:
  permissions:
    inference:
      models:
        - det_model
`)
	h, _ := newAppsUsingModelEnv(t, []*apppb.AppInfo{
		{Id: "stopped-app", Name: "stopped", State: "stopped", ManifestPath: manifest},
	})

	apps, err := h.getAppsUsingModel(context.Background(), "det_model")
	if err != nil {
		t.Fatalf("getAppsUsingModel: %v", err)
	}
	if len(apps) != 1 || apps[0] != "stopped-app" {
		t.Errorf("stopped app with explicit models[] ref must count, got %v", apps)
	}
}

// The broad allow_register_model grant is a live capability only: a stopped
// app that may register models does not shield every model from deletion.
func TestGetAppsUsingModelStoppedAllowRegisterDoesNotBlock(t *testing.T) {
	manifest := writeAppManifest(t, `spec:
  permissions:
    inference:
      allow_register_model: true
`)
	h, _ := newAppsUsingModelEnv(t, []*apppb.AppInfo{
		{Id: "stopped-app", Name: "stopped", State: "stopped", ManifestPath: manifest},
	})

	apps, err := h.getAppsUsingModel(context.Background(), "any_model")
	if err != nil {
		t.Fatalf("getAppsUsingModel: %v", err)
	}
	if len(apps) != 0 {
		t.Errorf("stopped app with only allow_register_model must not count, got %v", apps)
	}
}

func TestGetAppsUsingModelRunningAllowRegisterStillBlocks(t *testing.T) {
	manifest := writeAppManifest(t, `spec:
  permissions:
    inference:
      allow_register_model: true
`)
	h, _ := newAppsUsingModelEnv(t, []*apppb.AppInfo{
		{Id: "live-app", Name: "live", State: "running", ManifestPath: manifest},
	})

	apps, err := h.getAppsUsingModel(context.Background(), "any_model")
	if err != nil {
		t.Fatalf("getAppsUsingModel: %v", err)
	}
	if len(apps) != 1 || apps[0] != "live-app" {
		t.Errorf("running app with allow_register_model must count, got %v", apps)
	}
}

// Unrelated manifest entries must not keep a model pinned.
func TestGetAppsUsingModelIgnoresOtherModelRefs(t *testing.T) {
	manifest := writeAppManifest(t, `spec:
  permissions:
    inference:
      models:
        - other_model
`)
	h, _ := newAppsUsingModelEnv(t, []*apppb.AppInfo{
		{Id: "stopped-app", Name: "stopped", State: "stopped", ManifestPath: manifest},
	})

	apps, err := h.getAppsUsingModel(context.Background(), "det_model")
	if err != nil {
		t.Fatalf("getAppsUsingModel: %v", err)
	}
	if len(apps) != 0 {
		t.Errorf("app referencing another model must not count, got %v", apps)
	}
}

// An update that would swap the runtime registration (config change on a
// loaded detection model alters the composed variant) is refused while an
// app is using the model — same guard as UnloadModel.
func TestUpdateModelReloadBlockedWhenAppUsesModel(t *testing.T) {
	manifest := writeAppManifest(t, `spec:
  permissions:
    inference:
      models:
        - det_model
`)
	h, fake := newAppsUsingModelEnv(t, []*apppb.AppInfo{
		{Id: "live-app", Name: "live", State: "running", ManifestPath: manifest},
	})
	// The reload guard now probes runtime presence (批次6b "trust the
	// runtime"): mark the model live so the row's "loaded" claim is real and
	// the guard is actually exercised.
	fake.markLive("det_model")
	seedAIModel(t, h, &model.AIModel{
		ModelID: "det_model", Name: "det", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
		DesiredState: "loaded",
	})

	w := putUpdate(t, h, "det_model", `{"config":{"threshold":0.9}}`)
	if respCode(t, w) != CodeOperationFailed {
		t.Fatalf("code = %d body=%s, want %d (in use)", respCode(t, w), w.Body.String(), CodeOperationFailed)
	}
	if !strings.Contains(w.Body.String(), "stop them first") {
		t.Errorf("response must name the blocking apps, got %s", w.Body.String())
	}
	if calls, _ := fake.snapshot(); len(calls) != 0 {
		t.Errorf("blocked update must not touch the runtime, got %v", calls)
	}
	row, _ := h.aiModelRepo.GetByModelID("det_model")
	if row.Status != "loaded" || row.Threshold == 0.9 {
		t.Errorf("blocked update must leave row untouched, got status=%q threshold=%v", row.Status, row.Threshold)
	}
}

// A metadata-only update (nothing the runtime consumes) skips the reload
// path and therefore the occupancy guard — operators can fix dims or size
// while apps run.
func TestUpdateModelMetadataOnlyAllowedWhileAppUsesModel(t *testing.T) {
	manifest := writeAppManifest(t, `spec:
  permissions:
    inference:
      models:
        - det_model
`)
	h, fake := newAppsUsingModelEnv(t, []*apppb.AppInfo{
		{Id: "live-app", Name: "live", State: "running", ManifestPath: manifest},
	})
	seedAIModel(t, h, &model.AIModel{
		ModelID: "det_model", Name: "det", Status: "loaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
		DesiredState: "loaded",
	})

	w := putUpdate(t, h, "det_model", `{"file_size":1234,"input_width":640,"input_height":384}`)
	if respCode(t, w) != 0 {
		t.Fatalf("metadata-only update must succeed, got: %s", w.Body.String())
	}
	if calls, _ := fake.snapshot(); len(calls) != 0 {
		t.Errorf("metadata-only update must not reload, got %v", calls)
	}
}

// Deleting a model a stopped app explicitly declares must be refused: the
// app's next start would fail on its missing model.
func TestUnregisterModelBlockedWhenAppDeclaresModel(t *testing.T) {
	manifest := writeAppManifest(t, `spec:
  permissions:
    inference:
      models:
        - det_model
`)
	h, _ := newAppsUsingModelEnv(t, []*apppb.AppInfo{
		{Id: "stopped-app", Name: "stopped", State: "stopped", ManifestPath: manifest},
	})
	seedAIModel(t, h, &model.AIModel{
		ModelID: "det_model", Name: "det", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
	})

	w := deleteModel(t, h, "det_model")
	if respCode(t, w) != CodeOperationFailed {
		t.Fatalf("code = %d body=%s, want %d (in use)", respCode(t, w), w.Body.String(), CodeOperationFailed)
	}
	if row, err := h.aiModelRepo.GetByModelID("det_model"); err != nil || row == nil {
		t.Error("blocked delete must leave the row in place")
	}
}

// With no app relying on the model, deletion proceeds.
func TestUnregisterModelAllowedWithoutAppRefs(t *testing.T) {
	manifest := writeAppManifest(t, `spec:
  permissions:
    inference:
      models:
        - other_model
`)
	h, _ := newAppsUsingModelEnv(t, []*apppb.AppInfo{
		{Id: "stopped-app", Name: "stopped", State: "stopped", ManifestPath: manifest},
	})
	seedAIModel(t, h, &model.AIModel{
		ModelID: "det_model", Name: "det", Status: "uploaded", Source: "web",
		ModelType: "detection", FilePath: "/blobs/h1.hef", FileHash: "h1",
	})

	w := deleteModel(t, h, "det_model")
	if respCode(t, w) != 0 {
		t.Fatalf("unreferenced model must be deletable, got: %s", w.Body.String())
	}
	if _, err := h.aiModelRepo.GetByModelID("det_model"); err == nil {
		t.Error("row must be gone after delete")
	}
}
