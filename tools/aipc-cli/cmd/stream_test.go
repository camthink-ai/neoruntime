package cmd

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// withStreamAPI points the stream commands at a test server for the duration
// of fn, restoring the previous base afterwards.
func withStreamAPI(t *testing.T, baseURL string) {
	t.Helper()
	prev := streamAPIBase
	streamAPIBase = baseURL
	t.Cleanup(func() { streamAPIBase = prev })
}

func TestWSURL(t *testing.T) {
	tests := []struct {
		name    string
		apiBase string
		path    string
		want    string
	}{
		{"http to ws", "http://192.168.1.10:8080", "/api/v1/h264/main", "ws://192.168.1.10:8080/api/v1/h264/main"},
		{"https to wss", "https://camera.example.com", "/api/v1/h264/sub", "wss://camera.example.com/api/v1/h264/sub"},
		{"no scheme defaults to ws", "localhost:8080", "/api/v1/h264/main", "ws://localhost:8080/api/v1/h264/main"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if got := wsURL(tc.apiBase, tc.path); got != tc.want {
				t.Errorf("wsURL(%q, %q) = %q, want %q", tc.apiBase, tc.path, got, tc.want)
			}
		})
	}
}

// fakeStreamServer serves the platform-api envelope for the stream endpoints.
func fakeStreamServer(t *testing.T) *httptest.Server {
	t.Helper()
	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/media/streams", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"code":0,"message":"success","data":{"streams":[
			{"id":"main","name":"Main Stream (2160p)","codec":"h264","width":3840,"height":2160,
			 "fps":30,"bitrate":12000000,"gop":60,"enabled":true,"status":"active",
			 "rtsp_url":"rtsp://192.168.1.10:8554/main","h264_ws_path":"/api/v1/h264/main"},
			{"id":"sub","name":"Sub Stream (720p)","codec":"h264","width":1280,"height":720,
			 "fps":15,"bitrate":1000000,"gop":30,"enabled":false,"status":"stopped",
			 "rtsp_url":"rtsp://192.168.1.10:8554/sub","h264_ws_path":"/api/v1/h264/sub"}
		]}}`)
	})
	mux.HandleFunc("/api/v1/media/streams/main", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"code":0,"message":"success","data":{
			"id":"main","name":"Main Stream (2160p)","codec":"h264","width":3840,"height":2160,
			"fps":30,"bitrate":12000000,"gop":60,"enabled":true,"status":"active",
			"rtsp_url":"rtsp://192.168.1.10:8554/main","h264_ws_path":"/api/v1/h264/main"}}`)
	})
	mux.HandleFunc("/api/v1/media/streams/", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusNotFound)
		fmt.Fprint(w, `{"code":4000,"message":"Resource not found","error":{"detail":"Stream not found"}}`)
	})
	return httptest.NewServer(mux)
}

func TestFetchStreamsDecodesEnvelope(t *testing.T) {
	srv := fakeStreamServer(t)
	defer srv.Close()
	withStreamAPI(t, srv.URL)

	streams, err := fetchStreams()
	if err != nil {
		t.Fatalf("fetchStreams: %v", err)
	}
	if len(streams) != 2 {
		t.Fatalf("streams = %d, want 2", len(streams))
	}
	main := streams[0]
	if main.ID != "main" || main.Width != 3840 || main.FPS != 30 {
		t.Errorf("main = %+v", main)
	}
	if main.RtspURL != "rtsp://192.168.1.10:8554/main" {
		t.Errorf("rtsp_url = %q", main.RtspURL)
	}
	if main.H264WsPath != "/api/v1/h264/main" {
		t.Errorf("h264_ws_path = %q", main.H264WsPath)
	}
}

func TestFetchStreamFound(t *testing.T) {
	srv := fakeStreamServer(t)
	defer srv.Close()
	withStreamAPI(t, srv.URL)

	stream, err := fetchStream("main")
	if err != nil {
		t.Fatalf("fetchStream(main): %v", err)
	}
	if stream.Name != "Main Stream (2160p)" || stream.Codec != "h264" || !stream.Enabled {
		t.Errorf("stream = %+v", stream)
	}
}

func TestFetchStreamNotFound(t *testing.T) {
	srv := fakeStreamServer(t)
	defer srv.Close()
	withStreamAPI(t, srv.URL)

	_, err := fetchStream("nope")
	if err == nil {
		t.Fatal("fetchStream(nope) = nil error, want stream-not-found")
	}
	if !strings.Contains(err.Error(), "stream not found") {
		t.Errorf("error = %q, want stream-not-found", err.Error())
	}
}
