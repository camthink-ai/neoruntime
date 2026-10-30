package handlers

import (
	"context"
	"net/http"
	"testing"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"

	apppb "aipc/platform/app-manager/proto"
)

type timeoutRecordingAppManager struct {
	apppb.UnimplementedAppManagerServer
	stopRequests chan *apppb.StopRequest
}

func (f *timeoutRecordingAppManager) StopApp(_ context.Context, req *apppb.StopRequest) (*apppb.Status, error) {
	copyReq := *req
	f.stopRequests <- &copyReq
	return &apppb.Status{Success: true}, nil
}

func (f *timeoutRecordingAppManager) StartApp(context.Context, *apppb.StartRequest) (*apppb.Status, error) {
	return &apppb.Status{Success: true}, nil
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
			name:    "restart default",
			pattern: "/apps/:app_id/restart",
			path:    "/apps/demo/restart",
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
