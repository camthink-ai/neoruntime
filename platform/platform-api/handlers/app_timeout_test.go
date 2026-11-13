package handlers

import (
	"context"
	"net/http"
	"testing"
	"time"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"

	apppb "aipc/platform/app-manager/proto"
	"aipc/platform/app-manager/stopbudget"
)

type timeoutRecordingAppManager struct {
	apppb.UnimplementedAppManagerServer
	stopRequests chan *apppb.StopRequest
	startBudgets chan time.Duration
}

func (f *timeoutRecordingAppManager) StopApp(_ context.Context, req *apppb.StopRequest) (*apppb.Status, error) {
	copyReq := *req
	f.stopRequests <- &copyReq
	return &apppb.Status{Success: true}, nil
}

func (f *timeoutRecordingAppManager) StartApp(ctx context.Context, _ *apppb.StartRequest) (*apppb.Status, error) {
	if f.startBudgets != nil {
		deadline, ok := ctx.Deadline()
		if !ok {
			f.startBudgets <- 0
		} else {
			f.startBudgets <- time.Until(deadline)
		}
	}
	return &apppb.Status{Success: true}, nil
}

func TestAppStartUsesImageRecoveryBudget(t *testing.T) {
	fake := &timeoutRecordingAppManager{startBudgets: make(chan time.Duration, 1)}
	conn := newBufconnServer(t, func(s *grpc.Server) {
		apppb.RegisterAppManagerServer(s, fake)
	})
	h := &APIHandlers{grpcClients: &GRPCClients{AppManager: conn}}

	w := performRoute(t, http.MethodPost, "/apps/:app_id/start", "/apps/demo/start", h.StartApp)
	if w.Code != http.StatusOK {
		t.Fatalf("status = %d (%s), want 200", w.Code, w.Body.String())
	}

	remaining := <-fake.startBudgets
	want := stopbudget.ForStartAPI()
	if remaining <= want-time.Second || remaining > want {
		t.Fatalf("StartApp deadline remaining = %s, want approximately %s", remaining, want)
	}
}

func TestAppStopAndRestartTimeouts(t *testing.T) {
	cases := []struct {
		name    string
		pattern string
		path    string
		handler func(*APIHandlers) gin.HandlerFunc
		want    int32
	}{
		{
			name:    "stop default",
			pattern: "/apps/:app_id/stop",
			path:    "/apps/demo/stop",
			handler: func(h *APIHandlers) gin.HandlerFunc { return h.StopApp },
			want:    10,
		},
		{
			name:    "stop override",
			pattern: "/apps/:app_id/stop",
			path:    "/apps/demo/stop?timeout=23",
			handler: func(h *APIHandlers) gin.HandlerFunc { return h.StopApp },
			want:    23,
		},
		{
			name:    "stop non-positive uses default",
			pattern: "/apps/:app_id/stop",
			path:    "/apps/demo/stop?timeout=-4",
			handler: func(h *APIHandlers) gin.HandlerFunc { return h.StopApp },
			want:    10,
		},
		{
			name:    "restart default",
			pattern: "/apps/:app_id/restart",
			path:    "/apps/demo/restart",
			handler: func(h *APIHandlers) gin.HandlerFunc { return h.RestartApp },
			want:    10,
		},
		{
			name:    "restart zero uses default",
			pattern: "/apps/:app_id/restart",
			path:    "/apps/demo/restart?timeout=0",
			handler: func(h *APIHandlers) gin.HandlerFunc { return h.RestartApp },
			want:    10,
		},
	}

	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			fake := &timeoutRecordingAppManager{stopRequests: make(chan *apppb.StopRequest, 1)}
			conn := newBufconnServer(t, func(s *grpc.Server) {
				apppb.RegisterAppManagerServer(s, fake)
			})
			h := &APIHandlers{grpcClients: &GRPCClients{AppManager: conn}}

			w := performRoute(t, http.MethodPost, tc.pattern, tc.path, tc.handler(h))
			if w.Code != http.StatusOK {
				t.Fatalf("status = %d (%s), want 200", w.Code, w.Body.String())
			}
			req := <-fake.stopRequests
			if req.AppId != "demo" || req.TimeoutSeconds != tc.want {
				t.Fatalf("StopRequest = %+v, want app=demo timeout=%d", req, tc.want)
			}
		})
	}
}
