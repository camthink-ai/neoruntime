package main

import (
	"context"
	"crypto/tls"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strconv"
	"strings"
	"time"
)

type ManualDeviceProbeConfig struct {
	Host             string `json:"host"`
	APIScheme        string `json:"apiScheme"`
	APIPort          int    `json:"apiPort"`
	Token            string `json:"token"`
	Username         string `json:"username"`
	SkipTLSVerify    bool   `json:"skipTLSVerify"`
	RequestTimeoutMS int    `json:"requestTimeoutMs"`
}

func (a *App) ProbeManualDevice(configJSON string) (DeviceItem, error) {
	var cfg ManualDeviceProbeConfig
	if err := json.Unmarshal([]byte(configJSON), &cfg); err != nil {
		return DeviceItem{}, fmt.Errorf("invalid probe config: %w", err)
	}
	cfg = normalizeProbeConfig(cfg)
	if cfg.Host == "" {
		return DeviceItem{}, fmt.Errorf("host is required")
	}

	item := fallbackManualDevice(cfg)
	baseURL := probeBaseURL(cfg)
	transport := http.DefaultTransport.(*http.Transport).Clone()
	if cfg.SkipTLSVerify {
		transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec
	}
	client := &http.Client{
		Timeout:   time.Duration(cfg.RequestTimeoutMS) * time.Millisecond,
		Transport: transport,
	}
	ctx, cancel := context.WithTimeout(a.ctx, time.Duration(cfg.RequestTimeoutMS)*time.Millisecond)
	defer cancel()

	var errs []string
	loaded := false
	if body, err := fetchProbeJSON(ctx, client, baseURL+"/api/v1/device-info", cfg); err == nil {
		mergeProbeMap(&item, decodeProbeMap(body))
		loaded = true
	} else {
		errs = append(errs, err.Error())
	}

	if body, err := fetchProbeJSON(ctx, client, baseURL+"/api/v1/network/config", cfg); err == nil {
		mergeProbeMap(&item, decodeProbeMap(body))
		loaded = true
	} else {
		errs = append(errs, err.Error())
	}

	if body, err := fetchProbeJSON(ctx, client, baseURL+"/api/v1/monitor/summary", cfg); err == nil {
		mergeProbeMap(&item, decodeProbeMap(body))
		loaded = true
	} else {
		errs = append(errs, err.Error())
	}

	if loaded {
		return item, nil
	}
	return item, fmt.Errorf("probe failed: %s", strings.Join(errs, "; "))
}

func normalizeProbeConfig(cfg ManualDeviceProbeConfig) ManualDeviceProbeConfig {
	cfg.Host = normalizeProbeHost(cfg.Host)
	cfg.APIScheme = strings.ToLower(strings.TrimSpace(cfg.APIScheme))
	if cfg.APIScheme != "http" && cfg.APIScheme != "https" {
		cfg.APIScheme = "https"
	}
	if cfg.APIPort <= 0 {
		if cfg.APIScheme == "https" {
			cfg.APIPort = 443
		} else {
			cfg.APIPort = 8080
		}
	}
	if cfg.RequestTimeoutMS <= 0 {
		cfg.RequestTimeoutMS = 3000
	}
	cfg.Token = strings.TrimSpace(cfg.Token)
	cfg.Username = strings.TrimSpace(cfg.Username)
	return cfg
}

func normalizeProbeHost(raw string) string {
	raw = strings.TrimSpace(raw)
	if raw == "" {
		return ""
	}
	if parsed, err := url.Parse(raw); err == nil && parsed.Hostname() != "" {
		return parsed.Hostname()
	}
	withoutScheme := strings.TrimSpace(raw)
	if strings.Contains(withoutScheme, "://") {
		if parsed, err := url.Parse(withoutScheme); err == nil && parsed.Hostname() != "" {
			return parsed.Hostname()
		}
	}
	withoutScheme = strings.TrimPrefix(strings.TrimPrefix(withoutScheme, "http://"), "https://")
	withoutPath := strings.Split(withoutScheme, "/")[0]
	if host, _, err := net.SplitHostPort(withoutPath); err == nil {
		return strings.Trim(host, "[]")
	}
	return strings.Trim(strings.Split(withoutPath, ":")[0], "[]")
}

func fallbackManualDevice(cfg ManualDeviceProbeConfig) DeviceItem {
	now := time.Now().Format("2006-01-02 15:04:05")
	return DeviceItem{
		SN:        "manual-" + cfg.Host,
		Product:   "Manual",
		IP:        cfg.Host,
		Port:      cfg.APIPort,
		FW:        "",
		Caps:      []string{"http"},
		HW:        "",
		MAC:       "",
		Online:    true,
		LastSeen:  now,
		FirstSeen: now,
		Manual:    true,
	}
}

func probeBaseURL(cfg ManualDeviceProbeConfig) string {
	host := cfg.Host
	if strings.Contains(host, ":") && !strings.HasPrefix(host, "[") {
		host = "[" + host + "]"
	}
	if (cfg.APIScheme == "https" && cfg.APIPort == 443) || (cfg.APIScheme == "http" && cfg.APIPort == 80) {
		return (&url.URL{Scheme: cfg.APIScheme, Host: host}).String()
	}
	return (&url.URL{Scheme: cfg.APIScheme, Host: host + ":" + strconv.Itoa(cfg.APIPort)}).String()
}

func fetchProbeJSON(ctx context.Context, client *http.Client, endpoint string, cfg ManualDeviceProbeConfig) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/json")
	if cfg.Token != "" {
		if strings.HasPrefix(strings.ToLower(cfg.Token), "bearer ") {
			req.Header.Set("Authorization", cfg.Token)
		} else {
			req.Header.Set("Authorization", "Bearer "+cfg.Token)
		}
	}
	if cfg.Username != "" {
		req.Header.Set("X-User", cfg.Username)
	}

	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	body, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode < 200 || resp.StatusCode >= 300 {
		return nil, fmt.Errorf("%s returned %s", endpoint, resp.Status)
	}
	return unwrapMonitorResponse(body)
}

func decodeProbeMap(body []byte) map[string]any {
	var out map[string]any
	if err := json.Unmarshal(body, &out); err != nil {
		return nil
	}
	return out
}

func mergeProbeMap(item *DeviceItem, data map[string]any) {
	if len(data) == 0 {
		return
	}

	factory := nestedProbeMap(data, "factory")
	sn := firstProbeString(data, "serial_number", "serialNumber", "sn", "SN")
	if sn == "" {
		sn = firstProbeString(factory, "serial_number", "serialNumber", "sn", "SN")
	}
	if sn != "" {
		item.SN = sn
	}

	product := firstProbeString(data, "product", "model", "device_name", "hostname")
	if product == "" {
		product = firstProbeString(factory, "product", "model", "product_number", "productNumber")
	}
	if product == "" {
		product = firstProbeString(nestedProbeMap(data, "host"), "hostname", "platform", "os")
	}
	if product != "" {
		item.Product = product
	}

	fw := firstProbeString(data, "firmware_version", "firmwareVersion", "firmware", "fw", "version")
	if fw == "" {
		fw = firstProbeString(factory, "firmware_version", "firmwareVersion", "fw", "version")
	}
	if fw != "" {
		item.FW = fw
	}

	hw := firstProbeString(data, "hardware_version", "hardwareVersion", "hw")
	if hw == "" {
		hw = firstProbeString(factory, "hardware_version", "hardwareVersion", "hardware_revision", "hardwareRevision", "hw")
	}
	if hw != "" {
		item.HW = hw
	}

	mac := firstProbeString(data, "mac_address", "macAddress", "mac", "MAC")
	if mac == "" {
		mac = firstProbeString(factory, "mac_address", "macAddress", "mac", "MAC")
	}
	if mac != "" {
		item.MAC = mac
	}

	ip := firstProbeString(data, "ip_address", "ipAddress", "ip")
	if ip != "" {
		item.IP = ip
	}
	item.Online = true
	item.Manual = true
	item.LastSeen = time.Now().Format("2006-01-02 15:04:05")
}

func nestedProbeMap(data map[string]any, key string) map[string]any {
	if data == nil {
		return nil
	}
	value, ok := data[key]
	if !ok {
		return nil
	}
	if nested, ok := value.(map[string]any); ok {
		return nested
	}
	return nil
}

func firstProbeString(data map[string]any, keys ...string) string {
	if data == nil {
		return ""
	}
	for _, key := range keys {
		if value, ok := data[key]; ok {
			switch v := value.(type) {
			case string:
				if strings.TrimSpace(v) != "" {
					return strings.TrimSpace(v)
				}
			case float64:
				return strconv.FormatFloat(v, 'f', -1, 64)
			case int:
				return strconv.Itoa(v)
			}
		}
	}
	return ""
}
