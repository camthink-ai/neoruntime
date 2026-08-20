package cmd

import (
	"context"
	"crypto/tls"
	"encoding/csv"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/signal"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/camthink/ct-disc/pkg/discover"
	"github.com/spf13/cobra"
)

var (
	recordAddrs        []string
	recordDiscover     bool
	recordScanTimeout  time.Duration
	recordProbeCount   int
	recordInterval     time.Duration
	recordDuration     time.Duration
	recordSamples      int
	recordHTTPTimeout  time.Duration
	recordFile         string
	recordFormat       string
	recordSplitFiles   bool
	recordAppend       bool
	recordToken        string
	recordUsername     string
	recordProduct      string
	recordSN           string
	recordMAC          string
	recordAPIPort      int
	recordSkipTLSCheck bool
)

var recordCmd = &cobra.Command{
	Use:   "record",
	Short: "Record device resource metrics",
	Long: `Record CPU, memory, disk, NPU and temperature metrics from AIPC devices.

If --addr is omitted, ct-disc actively scans the LAN and records discovered
devices. Output can be CSV or JSON Lines for later analysis.`,
	RunE: runRecord,
}

func init() {
	recordCmd.Flags().StringArrayVarP(&recordAddrs, "addr", "a", nil, "device API base URL, e.g. http://192.168.1.10:8080 (repeatable)")
	recordCmd.Flags().BoolVar(&recordDiscover, "discover", false, "also scan CT-Disc devices when --addr is provided")
	recordCmd.Flags().DurationVar(&recordScanTimeout, "scan-timeout", 3*time.Second, "scan duration when discovering devices")
	recordCmd.Flags().IntVar(&recordProbeCount, "count", 3, "number of CT-Disc probe packets to send during discovery")
	recordCmd.Flags().DurationVar(&recordInterval, "interval", 5*time.Second, "sampling interval")
	recordCmd.Flags().DurationVar(&recordDuration, "duration", 0, "record duration (0 = until --samples is reached or Ctrl+C)")
	recordCmd.Flags().IntVar(&recordSamples, "samples", 0, "number of samples to collect (0 = unlimited)")
	recordCmd.Flags().DurationVar(&recordHTTPTimeout, "timeout", 3*time.Second, "HTTP request timeout per device")
	recordCmd.Flags().StringVar(&recordFile, "file", "ct-disc-record.csv", "output file path, or - for stdout")
	recordCmd.Flags().StringVar(&recordFormat, "format", "csv", "record format: csv, jsonl")
	recordCmd.Flags().BoolVar(&recordSplitFiles, "one-file-per-device", false, "write one output file per device")
	recordCmd.Flags().BoolVar(&recordAppend, "append", true, "append to existing output file")
	recordCmd.Flags().StringVar(&recordToken, "token", "", "HTTP bearer token for device APIs")
	recordCmd.Flags().StringVar(&recordUsername, "username", "", "optional X-User header for device APIs")
	recordCmd.Flags().StringVar(&recordProduct, "product", "", "discovery filter by product name")
	recordCmd.Flags().StringVar(&recordSN, "sn", "", "discovery filter by serial number substring")
	recordCmd.Flags().StringVar(&recordMAC, "mac", "", "discovery filter by MAC address substring")
	recordCmd.Flags().IntVar(&recordAPIPort, "api-port", 0, "override discovered HTTP API port (0 = use announced port or 8080)")
	recordCmd.Flags().BoolVar(&recordSkipTLSCheck, "insecure-skip-tls-verify", false, "skip HTTPS certificate verification")
	rootCmd.AddCommand(recordCmd)
}

type recordTarget struct {
	ID      string `json:"id"`
	SN      string `json:"sn,omitempty"`
	MAC     string `json:"mac,omitempty"`
	Product string `json:"product,omitempty"`
	IP      string `json:"ip,omitempty"`
	APIURL  string `json:"api_url"`
	Online  bool   `json:"online"`
}

type resourceRecord struct {
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

type recordWriter interface {
	Write(resourceRecord) error
	Flush() error
}

type csvRecordWriter struct {
	writer *csv.Writer
}

type jsonlRecordWriter struct {
	encoder *json.Encoder
}

func runRecord(cmd *cobra.Command, args []string) error {
	if recordInterval <= 0 {
		return fmt.Errorf("--interval must be greater than 0")
	}
	if recordHTTPTimeout <= 0 {
		return fmt.Errorf("--timeout must be greater than 0")
	}
	if recordSamples < 0 {
		return fmt.Errorf("--samples cannot be negative")
	}

	targets, err := resolveRecordTargets()
	if err != nil {
		return err
	}
	if len(targets) == 0 {
		return fmt.Errorf("no devices to record; pass --addr or make sure CT-Disc discovery can find devices")
	}

	writer, closeWriter, err := newRecordWriter()
	if err != nil {
		return err
	}
	defer closeWriter()

	transport := http.DefaultTransport.(*http.Transport).Clone()
	if recordSkipTLSCheck {
		transport.TLSClientConfig = &tls.Config{InsecureSkipVerify: true} //nolint:gosec
	}
	client := &http.Client{Timeout: recordHTTPTimeout, Transport: transport}

	baseCtx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	ctx := baseCtx
	var cancel context.CancelFunc
	if recordDuration > 0 {
		ctx, cancel = context.WithTimeout(baseCtx, recordDuration)
		defer cancel()
	}

	fmt.Fprintf(os.Stderr, "Recording %d device(s) every %s to %s (%s). Press Ctrl+C to stop.\n",
		len(targets), recordInterval, outputPathLabel(recordFile), recordFormat)

	sample := 0
	for {
		if recordSamples > 0 && sample >= recordSamples {
			return nil
		}

		started := time.Now()
		records := collectResourceRecords(ctx, client, targets)
		for _, rec := range records {
			if err := writer.Write(rec); err != nil {
				return err
			}
		}
		if err := writer.Flush(); err != nil {
			return err
		}

		sample++
		if verbose {
			fmt.Fprintf(os.Stderr, "sample %d written (%d record(s))\n", sample, len(records))
		}
		if recordSamples > 0 && sample >= recordSamples {
			return nil
		}

		wait := recordInterval - time.Since(started)
		if wait < 0 {
			wait = 0
		}
		timer := time.NewTimer(wait)
		select {
		case <-ctx.Done():
			timer.Stop()
			return nil
		case <-timer.C:
		}
	}
}

func resolveRecordTargets() ([]recordTarget, error) {
	byURL := make(map[string]recordTarget)

	for _, raw := range recordAddrs {
		target, err := targetFromAddr(raw)
		if err != nil {
			return nil, err
		}
		byURL[target.APIURL] = target
	}

	if len(recordAddrs) == 0 || recordDiscover {
		devices, err := discoverRecordDevices()
		if err != nil {
			return nil, err
		}
		for _, target := range devices {
			if _, ok := byURL[target.APIURL]; !ok {
				byURL[target.APIURL] = target
			}
		}
	}

	targets := make([]recordTarget, 0, len(byURL))
	for _, target := range byURL {
		targets = append(targets, target)
	}
	sort.Slice(targets, func(i, j int) bool {
		return targets[i].APIURL < targets[j].APIURL
	})
	return targets, nil
}

func discoverRecordDevices() ([]recordTarget, error) {
	registry := discover.NewRegistry()
	listener, err := discover.NewListener(registry, ifaceName)
	if err != nil {
		return nil, fmt.Errorf("failed to start listener: %w", err)
	}
	defer listener.Close()

	go listener.Listen()

	for i := 0; i < recordProbeCount; i++ {
		if err := discover.SendProbe(ifaceName); err != nil && verbose {
			fmt.Fprintf(os.Stderr, "probe send error: %v\n", err)
		}
		time.Sleep(200 * time.Millisecond)
	}

	if verbose {
		fmt.Fprintf(os.Stderr, "Waiting %s for CT-Disc responses...\n", recordScanTimeout)
	}
	time.Sleep(recordScanTimeout)

	devices := registry.Filter(recordProduct, recordSN, recordMAC)
	targets := make([]recordTarget, 0, len(devices))
	for _, dev := range devices {
		target, ok := targetFromAnnounce(dev)
		if ok {
			targets = append(targets, target)
		}
	}
	return targets, nil
}

func targetFromAnnounce(dev *discover.DeviceInfo) (recordTarget, bool) {
	ip := strings.TrimSpace(dev.Announce.IP)
	if ip == "" {
		return recordTarget{}, false
	}
	port := dev.Announce.Port
	if recordAPIPort > 0 {
		port = recordAPIPort
	}
	if port <= 0 {
		port = 8080
	}
	apiURL := (&url.URL{Scheme: "http", Host: net.JoinHostPort(ip, strconv.Itoa(port))}).String()
	id := firstNonEmpty(dev.Announce.SN, dev.Announce.MAC, ip)
	return recordTarget{
		ID:      id,
		SN:      dev.Announce.SN,
		MAC:     dev.Announce.MAC,
		Product: dev.Announce.Product,
		IP:      ip,
		APIURL:  apiURL,
		Online:  dev.Online,
	}, true
}

func targetFromAddr(raw string) (recordTarget, error) {
	apiURL, host, err := normalizeAPIURL(raw)
	if err != nil {
		return recordTarget{}, err
	}
	ip := host
	if parsedIP := net.ParseIP(host); parsedIP == nil {
		ip = ""
	}
	return recordTarget{
		ID:     host,
		IP:     ip,
		APIURL: apiURL,
		Online: true,
	}, nil
}

func normalizeAPIURL(raw string) (apiURL string, host string, err error) {
	raw = strings.TrimSpace(strings.TrimRight(raw, "/"))
	if raw == "" {
		return "", "", fmt.Errorf("empty device address")
	}
	if !strings.Contains(raw, "://") {
		raw = "http://" + raw
	}
	parsed, err := url.Parse(raw)
	if err != nil {
		return "", "", fmt.Errorf("invalid device address %q: %w", raw, err)
	}
	if parsed.Scheme != "http" && parsed.Scheme != "https" {
		return "", "", fmt.Errorf("invalid device address %q: scheme must be http or https", raw)
	}
	if parsed.Host == "" {
		return "", "", fmt.Errorf("invalid device address %q: missing host", raw)
	}
	parsed.RawQuery = ""
	parsed.Fragment = ""
	parsed.Path = strings.TrimRight(parsed.Path, "/")
	host = parsed.Hostname()
	return parsed.String(), host, nil
}

func collectResourceRecords(ctx context.Context, client *http.Client, targets []recordTarget) []resourceRecord {
	records := make([]resourceRecord, len(targets))
	var wg sync.WaitGroup
	for i, target := range targets {
		i, target := i, target
		wg.Add(1)
		go func() {
			defer wg.Done()
			records[i] = fetchResourceRecord(ctx, client, target)
		}()
	}
	wg.Wait()
	return records
}

func fetchResourceRecord(ctx context.Context, client *http.Client, target recordTarget) resourceRecord {
	now := time.Now()
	rec := resourceRecord{
		Timestamp:  now.Format(time.RFC3339),
		UnixMillis: now.UnixMilli(),
		SN:         target.SN,
		MAC:        target.MAC,
		Product:    target.Product,
		IP:         target.IP,
		APIURL:     target.APIURL,
		Online:     target.Online,
	}

	started := time.Now()
	summaryBody, err := fetchDeviceJSON(ctx, client, target.APIURL+"/api/v1/monitor/summary")
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
	rec.Platform = firstNonEmpty(summary.Host.Platform, summary.Host.OS)
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

	if snapshotBody, err := fetchDeviceJSON(ctx, client, target.APIURL+"/api/v1/monitor/snapshot"); err == nil {
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

type monitorSummary struct {
	CPU struct {
		UsagePercent float64 `json:"usage_percent"`
		Cores        int     `json:"cores"`
		Arch         string  `json:"arch"`
	} `json:"cpu"`
	Memory struct {
		Total        uint64  `json:"total"`
		Used         uint64  `json:"used"`
		Available    uint64  `json:"available"`
		UsagePercent float64 `json:"usage_percent"`
		Virtual      struct {
			Total        uint64  `json:"total"`
			Used         uint64  `json:"used"`
			Available    uint64  `json:"available"`
			UsagePercent float64 `json:"usage_percent"`
		} `json:"virtual"`
	} `json:"memory"`
	Disk struct {
		Total        uint64  `json:"total"`
		Used         uint64  `json:"used"`
		Free         uint64  `json:"free"`
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
	Timestamp    int64   `json:"timestamp"`
	CPU          float64 `json:"cpu"`
	Memory       float64 `json:"memory"`
	NPU          float64 `json:"npu"`
	Temperatures struct {
		CPU   float64 `json:"cpu"`
		NPU   float64 `json:"npu"`
		Board float64 `json:"board"`
	} `json:"temperatures"`
}

func fetchDeviceJSON(ctx context.Context, client *http.Client, endpoint string) ([]byte, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, endpoint, nil)
	if err != nil {
		return nil, err
	}
	req.Header.Set("Accept", "application/json")
	if recordToken != "" {
		token := strings.TrimSpace(recordToken)
		if strings.HasPrefix(strings.ToLower(token), "bearer ") {
			req.Header.Set("Authorization", token)
		} else {
			req.Header.Set("Authorization", "Bearer "+token)
		}
	}
	if recordUsername != "" {
		req.Header.Set("X-User", recordUsername)
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
	return unwrapAPIResponse(body)
}

func unwrapAPIResponse(body []byte) ([]byte, error) {
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

func newRecordWriter() (recordWriter, func() error, error) {
	format := strings.ToLower(strings.TrimSpace(recordFormat))
	if format != "csv" && format != "jsonl" {
		return nil, nil, fmt.Errorf("--format must be csv or jsonl")
	}
	if recordSplitFiles {
		if recordFile == "-" {
			return nil, nil, fmt.Errorf("--one-file-per-device cannot be used with --file -")
		}
		writer := &splitRecordWriter{
			format:  format,
			writers: make(map[string]recordWriter),
			closers: make(map[string]func() error),
		}
		return writer, writer.Close, nil
	}
	return newSingleRecordWriter(recordFile, format)
}

func newSingleRecordWriter(path string, format string) (recordWriter, func() error, error) {
	var out io.Writer = os.Stdout
	closeFn := func() error { return nil }
	needCSVHeader := path == "-"
	if path != "-" {
		flag := os.O_CREATE | os.O_WRONLY
		if recordAppend {
			flag |= os.O_APPEND
		} else {
			flag |= os.O_TRUNC
			needCSVHeader = true
		}
		if stat, err := os.Stat(path); err != nil || stat.Size() == 0 {
			needCSVHeader = true
		}
		f, err := os.OpenFile(path, flag, 0644)
		if err != nil {
			return nil, nil, err
		}
		out = f
		closeFn = f.Close
	}

	switch format {
	case "csv":
		cw := csv.NewWriter(out)
		if needCSVHeader {
			if err := cw.Write(recordCSVHeader()); err != nil {
				closeFn()
				return nil, nil, err
			}
			cw.Flush()
			if err := cw.Error(); err != nil {
				closeFn()
				return nil, nil, err
			}
		}
		return &csvRecordWriter{writer: cw}, closeFn, nil
	case "jsonl":
		return &jsonlRecordWriter{encoder: json.NewEncoder(out)}, closeFn, nil
	default:
		return nil, nil, fmt.Errorf("unsupported record format %q", recordFormat)
	}
}

type splitRecordWriter struct {
	format  string
	writers map[string]recordWriter
	closers map[string]func() error
}

func (w *splitRecordWriter) Write(rec resourceRecord) error {
	path := perDeviceRecordPath(recordFile, recordFormat, rec.SN, rec.MAC, rec.IP, rec.APIURL)
	writer, ok := w.writers[path]
	if !ok {
		var closer func() error
		var err error
		writer, closer, err = newSingleRecordWriter(path, w.format)
		if err != nil {
			return err
		}
		w.writers[path] = writer
		w.closers[path] = closer
	}
	return writer.Write(rec)
}

func (w *splitRecordWriter) Flush() error {
	for _, writer := range w.writers {
		if err := writer.Flush(); err != nil {
			return err
		}
	}
	return nil
}

func (w *splitRecordWriter) Close() error {
	var firstErr error
	for _, closer := range w.closers {
		if err := closer(); err != nil && firstErr == nil {
			firstErr = err
		}
	}
	return firstErr
}

func (w *csvRecordWriter) Write(rec resourceRecord) error {
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
		formatFloat(rec.CPUPercent),
		formatFloat(rec.MemoryPercent),
		strconv.FormatUint(rec.MemoryUsedBytes, 10),
		strconv.FormatUint(rec.MemoryTotalBytes, 10),
		formatFloat(rec.DiskPercent),
		strconv.FormatUint(rec.DiskUsedBytes, 10),
		strconv.FormatUint(rec.DiskTotalBytes, 10),
		sanitizeCSVField(rec.DiskMountpoint),
		formatFloat(rec.NPUPercent),
		formatFloat(rec.TempCPU),
		formatFloat(rec.TempNPU),
		formatFloat(rec.TempBoard),
		strconv.FormatInt(rec.LatencyMS, 10),
	})
}

func (w *csvRecordWriter) Flush() error {
	w.writer.Flush()
	return w.writer.Error()
}

func (w *jsonlRecordWriter) Write(rec resourceRecord) error {
	return w.encoder.Encode(rec)
}

func (w *jsonlRecordWriter) Flush() error {
	return nil
}

func recordCSVHeader() []string {
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

func formatFloat(value float64) string {
	return strconv.FormatFloat(value, 'f', 2, 64)
}

func outputPathLabel(path string) string {
	if recordSplitFiles && path != "-" {
		return perDeviceRecordPath(path, recordFormat, "<device>", "", "", "")
	}
	if path == "-" {
		return "stdout"
	}
	return path
}

func perDeviceRecordPath(basePath, format, sn, mac, ip, apiURL string) string {
	dir := filepath.Dir(basePath)
	base := filepath.Base(basePath)
	ext := filepath.Ext(base)
	if ext == "" {
		ext = "." + strings.ToLower(strings.TrimSpace(format))
	}
	stem := strings.TrimSuffix(base, filepath.Ext(base))
	device := sanitizeRecordFilePart(recordFileDeviceID(sn, mac, ip, apiURL))
	return filepath.Join(dir, fmt.Sprintf("%s_%s%s", stem, device, ext))
}

func recordFileDeviceID(sn, mac, ip, apiURL string) string {
	ip = strings.TrimSpace(ip)
	apiURL = strings.TrimSpace(apiURL)
	sn = strings.TrimSpace(sn)
	mac = strings.TrimSpace(mac)

	return firstNonEmpty(ip, apiURL, sn, mac, "device")
}

func sanitizeRecordFilePart(value string) string {
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

func firstNonEmpty(values ...string) string {
	for _, value := range values {
		if strings.TrimSpace(value) != "" {
			return value
		}
	}
	return ""
}
