package main

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"testing"
)

func TestProbeManualDeviceMergesSNFromNetworkConfig(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/api/v1/device-info", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"code":0,"data":{"model":"NE503","firmware_version":"v1.3.8"}}`)
	})
	mux.HandleFunc("/api/v1/network/config", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"code":0,"data":{"sn":"CT2026-000812","mac":"aa:bb:cc:dd:ee:ff"}}`)
	})
	mux.HandleFunc("/api/v1/monitor/summary", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"code":0,"data":{"host":{"hostname":"hailo15","platform":"linux"}}}`)
	})
	server := httptest.NewServer(mux)
	defer server.Close()

	parsed, err := url.Parse(server.URL)
	if err != nil {
		t.Fatal(err)
	}
	host, port, err := net.SplitHostPort(parsed.Host)
	if err != nil {
		t.Fatal(err)
	}

	app := &App{ctx: context.Background()}
	got, err := app.ProbeManualDevice(fmt.Sprintf(`{"host":%q,"apiScheme":"http","apiPort":%s}`, host, port))
	if err != nil {
		t.Fatal(err)
	}
	if got.SN != "CT2026-000812" {
		t.Fatalf("SN = %q, want CT2026-000812", got.SN)
	}
	if got.MAC != "aa:bb:cc:dd:ee:ff" {
		t.Fatalf("MAC = %q, want aa:bb:cc:dd:ee:ff", got.MAC)
	}
	if got.FW != "v1.3.8" {
		t.Fatalf("FW = %q, want v1.3.8", got.FW)
	}
	if !got.Manual {
		t.Fatal("manual flag should stay true")
	}
}
