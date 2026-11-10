package handlers

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"testing"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/credentials/insecure"
	"google.golang.org/grpc/status"
	"google.golang.org/grpc/test/bufconn"

	apppb "aipc/platform/app-manager/proto"
	devicepb "aipc/platform/device-control/proto"
)

// fakeAppManager returns canned responses so the not-found mapping in the
// HTTP handlers can be exercised without a running app-manager.
type fakeAppManager struct {
	apppb.UnimplementedAppManagerServer

	appStatus       *apppb.Status // returned by Start/Stop/Uninstall when set
	containerStatus *apppb.Status // returned by container ops when set
	progressErr     error         // returned by GetInstallProgress when set
}

func (f *fakeAppManager) StartApp(ctx context.Context, req *apppb.StartRequest) (*apppb.Status, error) {
	if f.appStatus != nil {
		return f.appStatus, nil
	}
	return &apppb.Status{Success: true}, nil
}

func (f *fakeAppManager) StopApp(ctx context.Context, req *apppb.StopRequest) (*apppb.Status, error) {
	if f.appStatus != nil {
		return f.appStatus, nil
	}
	return &apppb.Status{Success: true}, nil
}

func (f *fakeAppManager) UninstallApp(ctx context.Context, req *apppb.UninstallRequest) (*apppb.Status, error) {
	if f.appStatus != nil {
		return f.appStatus, nil
	}
	return &apppb.Status{Success: true}, nil
}

func (f *fakeAppManager) GetInstallProgress(ctx context.Context, req *apppb.InstallProgressRequest) (*apppb.InstallProgressResponse, error) {
	if f.progressErr != nil {
		return nil, f.progressErr
	}
	return &apppb.InstallProgressResponse{TaskId: req.TaskId}, nil
}

func (f *fakeAppManager) StartContainer(ctx context.Context, req *apppb.ContainerRequest) (*apppb.Status, error) {
	return f.container(f.containerStatus)
}

func (f *fakeAppManager) StopContainer(ctx context.Context, req *apppb.ContainerRequest) (*apppb.Status, error) {
	return f.container(f.containerStatus)
}

func (f *fakeAppManager) RestartContainer(ctx context.Context, req *apppb.ContainerRequest) (*apppb.Status, error) {
	return f.container(f.containerStatus)
}

func (f *fakeAppManager) RemoveContainer(ctx context.Context, req *apppb.RemoveContainerRequest) (*apppb.Status, error) {
	return f.container(f.containerStatus)
}

func (f *fakeAppManager) container(st *apppb.Status) (*apppb.Status, error) {
	if st != nil {
		return st, nil
	}
	return &apppb.Status{Success: true}, nil
}

// fakeDeviceControl returns a canned GPIORead outcome.
type fakeDeviceControl struct {
	devicepb.UnimplementedDeviceControlServer
	readErr error
}

func (f *fakeDeviceControl) GPIORead(ctx context.Context, req *devicepb.GPIOReadRequest) (*devicepb.GPIOReadResponse, error) {
	if f.readErr != nil {
		return nil, f.readErr
	}
	return &devicepb.GPIOReadResponse{
		Pin:    req.Pin,
		Value:  true,
		Status: &devicepb.Status{Success: true},
	}, nil
}

// newBufconnServer serves any gRPC server over bufconn and returns a client
// connection to it.
func newBufconnServer(t *testing.T, register func(*grpc.Server)) *grpc.ClientConn {
	t.Helper()
	lis := bufconn.Listen(1024 * 1024)
	srv := grpc.NewServer()
	register(srv)
	go srv.Serve(lis)
	t.Cleanup(srv.Stop)

	conn, err := grpc.DialContext(context.Background(), "bufnet",
		grpc.WithContextDialer(func(context.Context, string) (net.Conn, error) { return lis.Dial() }),
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Fatalf("grpc.DialContext: %v", err)
	}
	t.Cleanup(func() { conn.Close() })
	return conn
}

// performRoute registers h at pattern and fires a single request at path.
func performRoute(t *testing.T, method, pattern, path string, h gin.HandlerFunc) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	r := gin.New()
	r.Handle(method, pattern, h)
	w := httptest.NewRecorder()
	r.ServeHTTP(w, httptest.NewRequest(method, path, nil))
	return w
}

// decodeEnvelope asserts an error envelope and returns its business code.
func decodeEnvelope(t *testing.T, w *httptest.ResponseRecorder) int {
	t.Helper()
	var env envelope
	if err := json.Unmarshal(w.Body.Bytes(), &env); err != nil {
		t.Fatalf("unmarshal %s: %v", w.Body.String(), err)
	}
	return env.Code
}

func TestAppOps_NotFound_MapTo404(t *testing.T) {
	notFound := &apppb.Status{Success: false, Message: "App not found: __apitest__", Code: 404}
	conn := newBufconnServer(t, func(s *grpc.Server) {
		apppb.RegisterAppManagerServer(s, &fakeAppManager{appStatus: notFound})
	})
	h := &APIHandlers{grpcClients: &GRPCClients{AppManager: conn}}

	// StartApp is excluded: it now returns 202 immediately (fire-and-poll);
	// a 404 surfaces through the start-<appID> task progress polling, not
	// the initial POST response.
	cases := []struct {
		name    string
		method  string
		pattern string
		path    string
		handler gin.HandlerFunc
	}{
		{"stop app", http.MethodPost, "/apps/:app_id/stop", "/apps/__apitest__/stop", h.StopApp},
		{"restart app", http.MethodPost, "/apps/:app_id/restart", "/apps/__apitest__/restart", h.RestartApp},
		{"uninstall app", http.MethodDelete, "/apps/:app_id", "/apps/__apitest__", h.UninstallApp},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			w := performRoute(t, tc.method, tc.pattern, tc.path, tc.handler)
			if w.Code != http.StatusNotFound {
				t.Fatalf("status = %d (%s), want 404", w.Code, w.Body.String())
			}
			if code := decodeEnvelope(t, w); code != CodeAppNotFound {
				t.Fatalf("business code = %d, want %d", code, CodeAppNotFound)
			}
		})
	}
}

func TestStopApp_GenericFailure_Stays500(t *testing.T) {
	conn := newBufconnServer(t, func(s *grpc.Server) {
		apppb.RegisterAppManagerServer(s, &fakeAppManager{
			appStatus: &apppb.Status{Success: false, Message: "containerd rpc timeout"},
		})
	})
	h := &APIHandlers{grpcClients: &GRPCClients{AppManager: conn}}

	w := performRoute(t, http.MethodPost, "/apps/:app_id/stop", "/apps/api-tour/stop", h.StopApp)
	if w.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500", w.Code)
	}
	if code := decodeEnvelope(t, w); code != CodeAppStopFailed {
		t.Fatalf("business code = %d, want %d", code, CodeAppStopFailed)
	}
}

func TestGetInstallProgress_NotFound(t *testing.T) {
	conn := newBufconnServer(t, func(s *grpc.Server) {
		apppb.RegisterAppManagerServer(s, &fakeAppManager{
			progressErr: status.Errorf(codes.NotFound, "task not found: 1"),
		})
	})
	h := &APIHandlers{grpcClients: &GRPCClients{AppManager: conn}}

	w := performRoute(t, http.MethodGet, "/apps/install-progress/:task_id", "/apps/install-progress/1", h.GetInstallProgress)
	if w.Code != http.StatusNotFound {
		t.Fatalf("status = %d (%s), want 404", w.Code, w.Body.String())
	}
	if code := decodeEnvelope(t, w); code != CodeNotFound {
		t.Fatalf("business code = %d, want %d", code, CodeNotFound)
	}
}

func TestGetInstallProgress_GenericError_Stays500(t *testing.T) {
	conn := newBufconnServer(t, func(s *grpc.Server) {
		apppb.RegisterAppManagerServer(s, &fakeAppManager{
			progressErr: fmt.Errorf("task store unavailable"),
		})
	})
	h := &APIHandlers{grpcClients: &GRPCClients{AppManager: conn}}

	w := performRoute(t, http.MethodGet, "/apps/install-progress/:task_id", "/apps/install-progress/1", h.GetInstallProgress)
	if w.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500", w.Code)
	}
	if code := decodeEnvelope(t, w); code != CodeServiceError {
		t.Fatalf("business code = %d, want %d", code, CodeServiceError)
	}
}

func TestContainerOps_NotFound_MapTo404(t *testing.T) {
	notFound := &apppb.Status{Success: false, Message: "container not found: __apitest__", Code: 404}
	conn := newBufconnServer(t, func(s *grpc.Server) {
		apppb.RegisterAppManagerServer(s, &fakeAppManager{containerStatus: notFound})
	})
	h := NewContainerHandlers(conn)

	cases := []struct {
		name    string
		method  string
		pattern string
		path    string
		handler gin.HandlerFunc
	}{
		{"start", http.MethodPost, "/containers/:id/start", "/containers/__apitest__/start", h.StartContainer},
		{"stop", http.MethodPost, "/containers/:id/stop", "/containers/__apitest__/stop", h.StopContainer},
		{"restart", http.MethodPost, "/containers/:id/restart", "/containers/__apitest__/restart", h.RestartContainer},
		{"remove", http.MethodDelete, "/containers/:id", "/containers/__apitest__", h.RemoveContainer},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			w := performRoute(t, tc.method, tc.pattern, tc.path, tc.handler)
			if w.Code != http.StatusNotFound {
				t.Fatalf("status = %d (%s), want 404", w.Code, w.Body.String())
			}
			if code := decodeEnvelope(t, w); code != CodeNotFound {
				t.Fatalf("business code = %d, want %d", code, CodeNotFound)
			}
		})
	}
}

func TestRestartContainer_GenericFailure_Stays500(t *testing.T) {
	conn := newBufconnServer(t, func(s *grpc.Server) {
		apppb.RegisterAppManagerServer(s, &fakeAppManager{
			containerStatus: &apppb.Status{Success: false, Message: "cannot stop task"},
		})
	})
	h := NewContainerHandlers(conn)

	w := performRoute(t, http.MethodPost, "/containers/:id/restart", "/containers/aipc-api-tour/restart", h.RestartContainer)
	if w.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500", w.Code)
	}
	if code := decodeEnvelope(t, w); code != CodeOperationFailed {
		t.Fatalf("business code = %d, want %d", code, CodeOperationFailed)
	}
}

func TestGPIORead_NotFound(t *testing.T) {
	conn := newBufconnServer(t, func(s *grpc.Server) {
		devicepb.RegisterDeviceControlServer(s, &fakeDeviceControl{
			readErr: status.Errorf(codes.NotFound, "pin 1 is not in the GPIO catalog"),
		})
	})
	h := &APIHandlers{grpcClients: &GRPCClients{DeviceControl: conn}}

	w := performRoute(t, http.MethodGet, "/device/gpio/:pin", "/device/gpio/1", h.GPIORead)
	if w.Code != http.StatusNotFound {
		t.Fatalf("status = %d (%s), want 404", w.Code, w.Body.String())
	}
	if code := decodeEnvelope(t, w); code != CodeNotFound {
		t.Fatalf("business code = %d, want %d", code, CodeNotFound)
	}
}

func TestGPIORead_GenericError_Stays500(t *testing.T) {
	conn := newBufconnServer(t, func(s *grpc.Server) {
		devicepb.RegisterDeviceControlServer(s, &fakeDeviceControl{
			readErr: fmt.Errorf("MCU link down"),
		})
	})
	h := &APIHandlers{grpcClients: &GRPCClients{DeviceControl: conn}}

	w := performRoute(t, http.MethodGet, "/device/gpio/:pin", "/device/gpio/12", h.GPIORead)
	if w.Code != http.StatusInternalServerError {
		t.Fatalf("status = %d, want 500", w.Code)
	}
	if code := decodeEnvelope(t, w); code != CodeDeviceError {
		t.Fatalf("business code = %d, want %d", code, CodeDeviceError)
	}
}
