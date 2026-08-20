package main

import (
	"context"
	"crypto/tls"
	"encoding/csv"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"time"

	"github.com/wailsapp/wails/v2/pkg/runtime"
)

type RecordConfig struct {
	Devices          []DeviceItem `json:"devices"`
	OutputPath       string       `json:"outputPath"`
	Format           string       `json:"format"`
	APIScheme        string       `json:"apiScheme"`
	APIPort          int          `json:"apiPort"`
	OneFilePerDevice bool         `json:"oneFilePerDevice"`
	IntervalSeconds  int          `json:"intervalSeconds"`
	Samples          int          `json:"samples"`
	DurationMinutes  int          `json:"durationMinutes"`
	Token            string       `json:"token"`
	Username         string       `json:"username"`
	SkipTLSVerify    bool         `json:"skipTLSVerify"`
	RequestTimeoutMS int          `json:"requestTimeoutMs"`
}

type RecordStatus struct {
	Running        bool   `json:"running"`
	OutputPath     string `json:"outputPath"`
	Format         string `json:"format"`
	StartedAt      string `json:"startedAt"`
	StoppedAt      string `json:"stoppedAt"`
	LastSampleAt   string `json:"lastSampleAt"`
	LastError      string `json:"lastError"`
	SamplesWritten int    `json:"samplesWritten"`
	RecordsWritten int    `json:"recordsWritten"`
	TargetCount    int    `json:"targetCount"`
}

type MetricRecord struct {
	Timestamp        string  `json:"timestamp"`
	UnixMillis       int64   `json:"unix_ms"`
	SN               string  `json:"sn,omitempty"`
	MAC              string  `json:"mac,omitempty"`
	Product          string  `json:"product,omitempty"`
	IP               string  `json:"ip,omitempty"`
	APIURL           string  `json:"api_url"`
	Online           bool    `json:"online"`
	MetricsOK        bool    `json:"metrics_ok"`
	Error            string  `json:"error,omitempty"`
	Hostname         string  `json:"hostname,omitempty"`
	Platform         string  `json:"platform,omitempty"`
	UptimeSeconds    uint64  `json:"uptime_seconds,omitempty"`
	CPUPercent       float64 `json:"cpu_percent"`
	MemoryPercent    float64 `json:"memory_percent"`
	MemoryUsedBytes  uint64  `json:"memory_used_bytes"`
	MemoryTotalBytes uint64  `json:"memory_total_bytes"`
	DiskPercent      float64 `json:"disk_percent"`
	DiskUsedBytes    uint64  `json:"disk_used_bytes"`
	DiskTotalBytes   uint64  `json:"disk_total_bytes"`
	DiskMountpoint   string  `json:"disk_mountpoint,omitempty"`
	NPUPercent       float64 `json:"npu_percent"`
	TempCPU          float64 `json:"temp_cpu"`
	TempNPU          float64 `json:"temp_npu"`
	TempBoard        float64 `json:"temp_board"`
	LatencyMS        int64   `json:"latency_ms"`
}

type monitorSummary struct {
	CPU struct {
		UsagePercent float64 `json:"usage_percent"`
	} `json:"cpu"`
	Memory struct {
		Total        uint64  `json:"total"`
		Used         uint64  `json:"used"`
		UsagePercent float64 `json:"usage_percent"`
		Virtual      struct {
			Total        uint64  `json:"total"`
			Used         uint64  `json:"used"`
			UsagePercent float64 `json:"usage_percent"`
		} `json:"virtual"`
	} `json:"memory"`
	Disk struct {
		Total        uint64  `json:"total"`
		Used         uint64  `json:"used"`
		UsagePercent float64 `json:"usage_percent"`
		Mountpoint   string  `json:"mountpoint"`
		Partitions   []struct {
			Total        uint64  `json:"total"`
			Used         uint64  `json:"used"`
			UsagePercent float64 `json:"usage_percent"`
			Mountpoint   string  `json:"mountpoint"`
		} `json:"partitions"`
	} `json:"disk"`
	Host struct {
		Hostname      string `json:"hostname"`
		OS            string `json:"os"`
		Platform      string `json:"platform"`
		UptimeSeconds uint64 `json:"uptime_seconds"`
	} `json:"host"`
	NPU float64 `json:"npu"`
}

type monitorSnapshot struct {
	CPU          float64 `json:"cpu"`
	Memory       float64 `json:"memory"`
	NPU          float64 `json:"npu"`
	Temperatures struct {
		CPU   float64 `json:"cpu"`
		NPU   float64 `json:"npu"`
		Board float64 `json:"board"`
	} `json:"temperatures"`
}

func (a *App) ChooseRecordFile(defaultPath string) (string, error) {
	defaultPath = strings.TrimSpace(defaultPath)
	if defaultPath == "" {
		defaultPath = defaultRecordPath("csv")
	}

	dir := filepath.Dir(defaultPath)
	name := filepath.Base(defaultPath)
	return runtime.SaveFileDialog(a.ctx, runtime.SaveDialogOptions{
		Title:                      "Save Device Metrics",
		DefaultDirectory:           dir,
		DefaultFilename:            name,
		CanCreateDirectories:       true,
		TreatPackagesAsDirectories: false,
		Filters: []runtime.FileFilter{
			{DisplayName: "CSV", Pattern: "*.csv"},
			{DisplayName: "JSON Lines", Pattern: "*.jsonl"},
			{DisplayName: "All Files", Pattern: "*.*"},
		},
	})
}

func (a *App) StartMetricsRecording(configJSON string) (RecordStatus, error) {
	var cfg RecordConfig
	if err := json.Unmarshal([]byte(configJSON), &cfg); err != nil {
		return RecordStatus{}, fmt.Errorf("invalid record config: %w", err)
	}
	cfg = normalizeRecordConfig(cfg)
	if len(cfg.Devices) == 0 {
		return RecordStatus{}, fmt.Errorf("select at least one online device")
	}

	a.recordMu.Lock()
	if a.recordCancel != nil {
		status := a.recordStatus
		a.recordMu.Unlock()
		return status, fmt.Errorf("recording is already running")
	}
	ctx, cancel := context.WithCancel(a.ctx)
	done := make(chan struct{})
	a.recordCancel = cancel
	a.recordStopDone = done
	a.recordStatus = RecordStatus{
		Running:     true,
		OutputPath:  cfg.OutputPath,
		Format:      cfg.Format,
		StartedAt:   time.Now().Format(time.RFC3339),
		TargetCount: len(cfg.Devices),
	}
	a.recordHistory = nil
	status := a.recordStatus
	a.recordMu.Unlock()

	go a.runMetricsRecording(ctx, cfg, done)
	runtime.EventsEmit(a.ctx, "record:status", status)
	return status, nil
}

func (a *App) StopMetricsRecording() (RecordStatus, error) {
	a.recordMu.Lock()
	cancel := a.recordCancel
	done := a.recordStopDone
	a.recordMu.Unlock()
	if cancel == nil {
		return a.GetRecordStatus(), nil
	}
	cancel()
	if done != nil {
		select {
		case <-done:
		case <-time.After(2 * time.Second):
		}
	}
	return a.GetRecordStatus(), nil
}

func (a *App) GetRecordStatus() RecordStatus {
	a.recordMu.Lock()
	defer a.recordMu.Unlock()
	return a.recordStatus
}

func (a *App) GetRecordHistory() []MetricRecord {
	a.recordMu.Lock()
	defer a.recordMu.Unlock()
	history := make([]MetricRecord, len(a.recordHistory))
	copy(history, a.recordHistory)
	return history
}

func (a *App) GetDefaultRecordPath(format string) string {
	return defaultRecordPath(format)
}

func (a *App) runMetricsRecording(ctx context.Context, cfg RecordConfig, done chan struct{}) {
	defer close(done)
	defer func() {
		a.recordMu.Lock()
		a.recordCancel = nil
		a.recordStopDone = nil
		a.recordStatus.Running = false
		a.recordStatus.StoppedAt = time.Now().Format(time.RFC3339)
		status := a.recordStatus
		a.recordMu.Unlock()
		runtime.EventsEmit(a.ctx, "record:status", status)
	}()

	writer, closeWriter, err := newMetricWriter(cfg)
	if err != nil {
		a.updateRecordError(err)
		return
	}
	defer closeWriter()

	transport := http.DefaultTransport.(*http.Transport).Clone()
	if cfg.SkipTLSVerify {
		transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec
	}
	client := &http.Client{
		Timeout:   time.Duration(cfg.RequestTimeoutMS) * time.Millisecond,
		Transport: transport,
	}

	recordCtx := ctx
	var cancel context.CancelFunc
	if cfg.DurationMinutes > 0 {
		recordCtx, cancel = context.WithTimeout(ctx, time.Duration(cfg.DurationMinutes)*time.Minute)
		defer cancel()
	}

	interval := time.Duration(cfg.IntervalSeconds) * time.Second
	sample := 0
	for {
		if cfg.Samples > 0 && sample >= cfg.Samples {
			return
		}

		started := time.Now()
		records := collectGUIRecords(recordCtx, client, cfg)
		for _, rec := range records {
			if err := writer.Write(rec); err != nil {
				a.updateRecordError(err)
				return
			}
		}
		if err := writer.Flush(); err != nil {
			a.updateRecordError(err)
			return
		}

		sample++
		a.appendRecordHistory(records)
		a.recordMu.Lock()
		a.recordStatus.SamplesWritten = sample
		a.recordStatus.RecordsWritten += len(records)
		a.recordStatus.LastSampleAt = time.Now().Format(time.RFC3339)
		status := a.recordStatus
		a.recordMu.Unlock()
		runtime.EventsEmit(a.ctx, "record:sample", records)
		runtime.EventsEmit(a.ctx, "record:status", status)

		if cfg.Samples > 0 && sample >= cfg.Samples {
			return
		}

		wait := interval - time.Since(started)
		if wait < 0 {
			wait = 0
		}
		timer := time.NewTimer(wait)
		select {
		case <-recordCtx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
	}
}

func normalizeRecordConfig(cfg RecordConfig) RecordConfig {
	cfg.Format = strings.ToLower(strings.TrimSpace(cfg.Format))
	if cfg.Format != "jsonl" {
		cfg.Format = "csv"
	}
	if cfg.IntervalSeconds <= 0 {
		cfg.IntervalSeconds = 5
	}
	if cfg.RequestTimeoutMS <= 0 {
		cfg.RequestTimeoutMS = 3000
	}
	if cfg.APIPort < 0 {
		cfg.APIPort = 0
	}
	cfg.APIScheme = strings.ToLower(strings.TrimSpace(cfg.APIScheme))
	if cfg.APIScheme != "http" && cfg.APIScheme != "https" {
		cfg.APIScheme = "https"
	}
	if strings.TrimSpace(cfg.OutputPath) == "" {
		cfg.OutputPath = defaultRecordPath(cfg.Format)
	}
	cfg.OutputPath = strings.TrimSpace(cfg.OutputPath)
	return cfg
}

type metricWriter interface {
	Write(MetricRecord) error
	Flush() error
}

type csvMetricWriter struct {
	writer *csv.Writer
}

type jsonlMetricWriter struct {
	encoder *json.Encoder
}

type splitMetricWriter struct {
	cfg     RecordConfig
	writers map[string]metricWriter
	closers map[string]func() error
}

func newMetricWriter(cfg RecordConfig) (metricWriter, func() error, error) {
	if cfg.OneFilePerDevice {
		writer := &splitMetricWriter{
			cfg:     cfg,
			writers: make(map[string]metricWriter),
			closers: make(map[string]func() error),
		}
		return writer, writer.Close, nil
	}
	return newSingleMetricWriter(cfg.OutputPath, cfg)
}

func newSingleMetricWriter(path string, cfg RecordConfig) (metricWriter, func() error, error) {
	flag := os.O_CREATE | os.O_WRONLY | os.O_APPEND
	needHeader := true
	if stat, err := os.Stat(path); err == nil && stat.Size() > 0 {
		needHeader = false
	}
	f, err := os.OpenFile(path, flag, 0644)
	if err != nil {
		return nil, nil, err
	}

	if cfg.Format == "jsonl" {
		return &jsonlMetricWriter{encoder: json.NewEncoder(f)}, f.Close, nil
	}

	cw := csv.NewWriter(f)
	if needHeader {
		if err := cw.Write(metricCSVHeader()); err != nil {
			f.Close()
			return nil, nil, err
		}
		cw.Flush()
		if err := cw.Error(); err != nil {
			f.Close()
			return nil, nil, err
		}
	}
	return &csvMetricWriter{writer: cw}, f.Close, nil
}

func (w *splitMetricWriter) Write(rec MetricRecord) error {
	path := perDeviceMetricPath(w.cfg.OutputPath, w.cfg.Format, rec.SN, rec.MAC, rec.IP, rec.APIURL)
	writer, ok := w.writers[path]
	if !ok {
		var closer func() error
		var err error
		writer, closer, err = newSingleMetricWriter(path, w.cfg)
		if err != nil {
			return err
		}
		w.writers[path] = writer
		w.closers[path] = closer
	}
	return writer.Write(rec)
}

func (w *splitMetricWriter) Flush() error {
	for _, writer := range w.writers {
		if err := writer.Flush(); err != nil {
			return err
		}
	}
	return nil
}

func (w *splitMetricWriter) Close() error {
	var firstErr error
	for _, closer := range w.closers {
		if err := closer(); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

// sanitizeCSVField neutralizes CSV formula injection in device-controlled
// values: spreadsheet apps interpret cells starting with =, +, -, @ or a
// control character as formulas, so prefix those with a single quote.
func sanitizeCSVField(value string) string {
	if value == "" {
		return value
	}
	switch value[0] {
	case '=', '+', '-', '@', '\t', '\r':
		return "'" + value
	}
	return value
}

func (w *csvMetricWriter) Write(rec MetricRecord) error {
	return w.writer.Write([]string{
		rec.Timestamp,
		strconv.FormatInt(rec.UnixMillis, 10),
		sanitizeCSVField(rec.SN),
		sanitizeCSVField(rec.MAC),
		sanitizeCSVField(rec.Product),
		sanitizeCSVField(rec.IP),
		sanitizeCSVField(rec.APIURL),
		strconv.FormatBool(rec.Online),
		strconv.FormatBool(rec.MetricsOK),
		sanitizeCSVField(rec.Error),
		sanitizeCSVField(rec.Hostname),
		sanitizeCSVField(rec.Platform),
		strconv.FormatUint(rec.UptimeSeconds, 10),
		formatMetricFloat(rec.CPUPercent),
		formatMetricFloat(rec.MemoryPercent),
		strconv.FormatUint(rec.MemoryUsedBytes, 10),
		strconv.FormatUint(rec.MemoryTotalBytes, 10),
		formatMetricFloat(rec.DiskPercent),
		strconv.FormatUint(rec.DiskUsedBytes, 10),
		strconv.FormatUint(rec.DiskTotalBytes, 10),
		sanitizeCSVField(rec.DiskMountpoint),
		formatMetricFloat(rec.NPUPercent),
		formatMetricFloat(rec.TempCPU),
		formatMetricFloat(rec.TempNPU),
		formatMetricFloat(rec.TempBoard),
		strconv.FormatInt(rec.LatencyMS, 10),
	})
}

func (w *csvMetricWriter) Flush() error {
	w.writer.Flush()
	return w.writer.Error()
}

func (w *jsonlMetricWriter) Write(rec MetricRecord) error {
	return w.encoder.Encode(rec)
}

func (w *jsonlMetricWriter) Flush() error {
	return nil
}

func collectGUIRecords(ctx context.Context, client *http.Client, cfg RecordConfig) []MetricRecord {
	records := make([]MetricRecord, len(cfg.Devices))
	done := make(chan int, len(cfg.Devices))
	for i, device := range cfg.Devices {
		go func(index int, item DeviceItem) {
			records[index] = fetchGUIRecord(ctx, client, cfg, item)
			done <- index
		}(i, device)
	}
	for range cfg.Devices {
		<-done
	}
	return records
}

func fetchGUIRecord(ctx context.Context, client *http.Client, cfg RecordConfig, device DeviceItem) MetricRecord {
	now := time.Now()
	apiURL := deviceAPIURL(device, cfg)
	rec := MetricRecord{
		Timestamp:  now.Format(time.RFC3339),
		UnixMillis: now.UnixMilli(),
		SN:         device.SN,
		MAC:        device.MAC,
		Product:    device.Product,
		IP:         device.IP,
		APIURL:     apiURL,
		Online:     device.Online,
	}

	started := time.Now()
	summaryBody, err := fetchMonitorJSON(ctx, client, apiURL+"/api/v1/monitor/summary", cfg)
	rec.LatencyMS = time.Since(started).Milliseconds()
	if err != nil {
		rec.Error = err.Error()
		return rec
	}

	var summary monitorSummary
	if err := json.Unmarshal(summaryBody, &summary); err != nil {
		rec.Error = "parse summary: " + err.Error()
		return rec
	}

	rec.Online = true
	rec.MetricsOK = true
	rec.Hostname = summary.Host.Hostname
	rec.Platform = firstRecordValue(summary.Host.Platform, summary.Host.OS)
	rec.UptimeSeconds = summary.Host.UptimeSeconds
	rec.CPUPercent = summary.CPU.UsagePercent
	rec.NPUPercent = summary.NPU
	rec.MemoryPercent = summary.Memory.UsagePercent
	rec.MemoryUsedBytes = summary.Memory.Used
	rec.MemoryTotalBytes = summary.Memory.Total
	if rec.MemoryTotalBytes == 0 && summary.Memory.Virtual.Total > 0 {
		rec.MemoryPercent = summary.Memory.Virtual.UsagePercent
		rec.MemoryUsedBytes = summary.Memory.Virtual.Used
		rec.MemoryTotalBytes = summary.Memory.Virtual.Total
	}

	rec.DiskPercent = summary.Disk.UsagePercent
	rec.DiskUsedBytes = summary.Disk.Used
	rec.DiskTotalBytes = summary.Disk.Total
	rec.DiskMountpoint = summary.Disk.Mountpoint
	if rec.DiskTotalBytes == 0 && len(summary.Disk.Partitions) > 0 {
		partition := summary.Disk.Partitions[0]
		rec.DiskPercent = partition.UsagePercent
		rec.DiskUsedBytes = partition.Used
		rec.DiskTotalBytes = partition.Total
		rec.DiskMountpoint = partition.Mountpoint
	}

	if snapshotBody, err := fetchMonitorJSON(ctx, client, apiURL+"/api/v1/monitor/snapshot", cfg); err == nil {
		var snapshot monitorSnapshot
		if json.Unmarshal(snapshotBody, &snapshot) == nil {
			if rec.CPUPercent == 0 {
				rec.CPUPercent = snapshot.CPU
			}
			if rec.MemoryPercent == 0 {
				rec.MemoryPercent = snapshot.Memory
			}
			if rec.NPUPercent == 0 {
				rec.NPUPercent = snapshot.NPU
			}
			rec.TempCPU = snapshot.Temperatures.CPU
			rec.TempNPU = snapshot.Temperatures.NPU
			rec.TempBoard = snapshot.Temperatures.Board
		}
	}

	return rec
}

func fetchMonitorJSON(ctx context.Context, client *http.Client, endpoint string, cfg RecordConfig) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/json")
	if cfg.Token != "" {
		token := strings.TrimSpace(cfg.Token)
		if strings.HasPrefix(strings.ToLower(token), "bearer ") {
			req.Header.Set("Authorization", token)
		} else {
			req.Header.Set("Authorization", "Bearer "+token)
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

func unwrapMonitorResponse(body []byte) ([]byte, error) {
	var envelope struct {
		Code    int             `json:"code"`
		Message string          `json:"message"`
		Data    json.RawMessage `json:"data"`
	}
	if err := json.Unmarshal(body, &envelope); err == nil && len(envelope.Data) > 0 {
		if envelope.Code != 0 {
			msg := envelope.Message
			if msg == "" {
				msg = "API returned non-zero code"
			}
			return nil, fmt.Errorf("%s (code=%d)", msg, envelope.Code)
		}
		return envelope.Data, nil
	}
	return body, nil
}

func deviceAPIURL(device DeviceItem, cfg RecordConfig) string {
	scheme := cfg.APIScheme
	if scheme != "http" && scheme != "https" {
		scheme = "https"
	}
	port := device.Port
	if cfg.APIPort > 0 {
		port = cfg.APIPort
	}
	if port <= 0 {
		if scheme == "https" {
			port = 443
		} else {
			port = 8080
		}
	}
	host := device.IP
	if strings.Contains(host, ":") && !strings.HasPrefix(host, "[") {
		host = "[" + host + "]"
	}
	if (scheme == "https" && port == 443) || (scheme == "http" && port == 80) {
		return (&url.URL{Scheme: scheme, Host: host}).String()
	}
	return (&url.URL{Scheme: scheme, Host: host + ":" + strconv.Itoa(port)}).String()
}

func metricCSVHeader() []string {
	return []string{
		"timestamp",
		"unix_ms",
		"sn",
		"mac",
		"product",
		"ip",
		"api_url",
		"online",
		"metrics_ok",
		"error",
		"hostname",
		"platform",
		"uptime_seconds",
		"cpu_percent",
		"memory_percent",
		"memory_used_bytes",
		"memory_total_bytes",
		"disk_percent",
		"disk_used_bytes",
		"disk_total_bytes",
		"disk_mountpoint",
		"npu_percent",
		"temp_cpu",
		"temp_npu",
		"temp_board",
		"latency_ms",
	}
}

func defaultRecordPath(format string) string {
	ext := "csv"
	if strings.EqualFold(format, "jsonl") {
		ext = "jsonl"
	}
	home, err := os.UserHomeDir()
	if err != nil || home == "" {
		return "ct-disc-metrics." + ext
	}
	return filepath.Join(home, "ct-disc-metrics."+ext)
}

func perDeviceMetricPath(basePath, format, sn, mac, ip, apiURL string) string {
	dir := filepath.Dir(basePath)
	base := filepath.Base(basePath)
	ext := filepath.Ext(base)
	if ext == "" {
		ext = "." + strings.ToLower(strings.TrimSpace(format))
	}
	stem := strings.TrimSuffix(base, filepath.Ext(base))
	device := sanitizeMetricFilePart(metricFileDeviceID(sn, mac, ip, apiURL))
	return filepath.Join(dir, fmt.Sprintf("%s_%s%s", stem, device, ext))
}

func metricFileDeviceID(sn, mac, ip, apiURL string) string {
	ip = strings.TrimSpace(ip)
	apiURL = strings.TrimSpace(apiURL)
	sn = strings.TrimSpace(sn)
	mac = strings.TrimSpace(mac)

	return firstRecordValue(ip, apiURL, sn, mac, "device")
}

func sanitizeMetricFilePart(value string) string {
	value = strings.TrimSpace(value)
	if value == "" {
		return "device"
	}
	var b strings.Builder
	lastDash := false
	for _, r := range value {
		ok := (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9')
		if ok {
			b.WriteRune(r)
			lastDash = false
			continue
		}
		if !lastDash {
			b.WriteByte('-')
			lastDash = true
		}
	}
	out := strings.Trim(b.String(), "-")
	if out == "" {
		return "device"
	}
	return out
}

func formatMetricFloat(value float64) string {
	return strconv.FormatFloat(value, 'f', 2, 64)
}

func firstRecordValue(values ...string) string {
	for _, value := range values {
		if strings.TrimSpace(value) != "" {
			return value
		}
	}
	return ""
}

func (a *App) appendRecordHistory(records []MetricRecord) {
	a.recordMu.Lock()
	defer a.recordMu.Unlock()
	a.recordHistory = append(a.recordHistory, records...)
	const maxHistoryRecords = 5000
	if len(a.recordHistory) > maxHistoryRecords {
		a.recordHistory = a.recordHistory[len(a.recordHistory)-maxHistoryRecords:]
	}
}

func (a *App) updateRecordError(err error) {
	a.recordMu.Lock()
	a.recordStatus.LastError = err.Error()
	status := a.recordStatus
	a.recordMu.Unlock()
	runtime.EventsEmit(a.ctx, "record:status", status)
}
