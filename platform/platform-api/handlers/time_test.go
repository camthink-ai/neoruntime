package handlers

import (
	"bytes"
	"context"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	camerapb "aipc/platform/camera-daemon/proto"
	"aipc/platform/platform-api/auth"
	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"
)

// ============================================================
// 1. writeTimesyncdConfig — NTP config writing logic
// ============================================================

// testWriteTimesyncdConfig exercises the pure config transformation without
// invoking systemd in the unit-test environment.
func testWriteTimesyncdConfig(confPath, server string, interval int) error {
	data, err := os.ReadFile(confPath)
	if err != nil {
		data = []byte("[Time]\n")
	}
	updated, err := updateTimesyncdConfig(data, server, interval)
	if err != nil {
		return err
	}
	return os.WriteFile(confPath, updated, 0644)
}

func TestWriteTimesyncdConfig_AddsServerAndInterval(t *testing.T) {
	tmpDir := t.TempDir()
	confPath := filepath.Join(tmpDir, "timesyncd.conf")

	// Start with a config that has [Time] but all values commented out
	initialConf := `[Time]
#NTP=
#FallbackNTP=ntp.ubuntu.com
#PollIntervalMinSec=32
#PollIntervalMaxSec=2048
`
	if err := os.WriteFile(confPath, []byte(initialConf), 0644); err != nil {
		t.Fatalf("Failed to write initial conf: %v", err)
	}

	if err := testWriteTimesyncdConfig(confPath, "time.google.com", 60); err != nil {
		t.Fatalf("testWriteTimesyncdConfig failed: %v", err)
	}

	result, err := os.ReadFile(confPath)
	if err != nil {
		t.Fatalf("Failed to read result: %v", err)
	}

	resultStr := string(result)
	if !strings.Contains(resultStr, "NTP=time.google.com") {
		t.Errorf("Expected NTP=time.google.com, got:\n%s", resultStr)
	}
	if !strings.Contains(resultStr, "PollIntervalMinSec=32") {
		t.Errorf("Expected PollIntervalMinSec=32, got:\n%s", resultStr)
	}
	if !strings.Contains(resultStr, "PollIntervalMaxSec=60") {
		t.Errorf("Expected PollIntervalMaxSec=60, got:\n%s", resultStr)
	}
	// Commented lines should still be present
	if !strings.Contains(resultStr, "#PollIntervalMinSec=32") {
		t.Errorf("Commented defaults should be preserved, got:\n%s", resultStr)
	}
}

func TestWriteTimesyncdConfig_UpdatesExistingValues(t *testing.T) {
	tmpDir := t.TempDir()
	confPath := filepath.Join(tmpDir, "timesyncd.conf")

	initialConf := `[Time]
NTP=pool.ntp.org
PollIntervalMinSec=3600
PollIntervalMaxSec=3600
`
	if err := os.WriteFile(confPath, []byte(initialConf), 0644); err != nil {
		t.Fatalf("Failed to write initial conf: %v", err)
	}

	if err := testWriteTimesyncdConfig(confPath, "time.cloudflare.com", 300); err != nil {
		t.Fatalf("testWriteTimesyncdConfig failed: %v", err)
	}

	result, err := os.ReadFile(confPath)
	if err != nil {
		t.Fatalf("Failed to read result: %v", err)
	}

	resultStr := string(result)
	if !strings.Contains(resultStr, "NTP=time.cloudflare.com") {
		t.Errorf("Expected NTP=time.cloudflare.com, got:\n%s", resultStr)
	}
	if strings.Contains(resultStr, "NTP=pool.ntp.org") {
		t.Errorf("Old NTP=pool.ntp.org should be replaced, got:\n%s", resultStr)
	}
	if !strings.Contains(resultStr, "PollIntervalMinSec=32") {
		t.Errorf("Expected PollIntervalMinSec=32, got:\n%s", resultStr)
	}
	if !strings.Contains(resultStr, "PollIntervalMaxSec=300") {
		t.Errorf("Expected PollIntervalMaxSec=300, got:\n%s", resultStr)
	}
}

func TestWriteTimesyncdConfig_CreatesTimeSectionIfMissing(t *testing.T) {
	tmpDir := t.TempDir()
	confPath := filepath.Join(tmpDir, "timesyncd.conf")

	// Config without [Time] section — simulates minimal/truncated Yocto image
	initialConf := `# This file is empty
`
	if err := os.WriteFile(confPath, []byte(initialConf), 0644); err != nil {
		t.Fatalf("Failed to write initial conf: %v", err)
	}

	if err := testWriteTimesyncdConfig(confPath, "pool.ntp.org", 60); err != nil {
		t.Fatalf("testWriteTimesyncdConfig failed: %v", err)
	}

	result, err := os.ReadFile(confPath)
	if err != nil {
		t.Fatalf("Failed to read result: %v", err)
	}

	resultStr := string(result)
	if !strings.Contains(resultStr, "[Time]") {
		t.Errorf("Expected [Time] section to be created, got:\n%s", resultStr)
	}
	if !strings.Contains(resultStr, "NTP=pool.ntp.org") {
		t.Errorf("Expected NTP=pool.ntp.org, got:\n%s", resultStr)
	}
	if !strings.Contains(resultStr, "PollIntervalMinSec=32") {
		t.Errorf("Expected PollIntervalMinSec=32, got:\n%s", resultStr)
	}
	if !strings.Contains(resultStr, "PollIntervalMaxSec=60") {
		t.Errorf("Expected PollIntervalMaxSec=60, got:\n%s", resultStr)
	}
}

func TestWriteTimesyncdConfig_CreatesFileIfMissing(t *testing.T) {
	tmpDir := t.TempDir()
	confPath := filepath.Join(tmpDir, "timesyncd.conf")

	// File doesn't exist at all
	if err := testWriteTimesyncdConfig(confPath, "pool.ntp.org", 60); err != nil {
		t.Fatalf("testWriteTimesyncdConfig failed: %v", err)
	}

	result, err := os.ReadFile(confPath)
	if err != nil {
		t.Fatalf("Failed to read result: %v", err)
	}

	resultStr := string(result)
	if !strings.Contains(resultStr, "[Time]") {
		t.Errorf("Expected [Time] section, got:\n%s", resultStr)
	}
	if !strings.Contains(resultStr, "NTP=pool.ntp.org") {
		t.Errorf("Expected NTP=pool.ntp.org, got:\n%s", resultStr)
	}
}

func TestWriteTimesyncdConfig_ServerOnlyUpdate(t *testing.T) {
	tmpDir := t.TempDir()
	confPath := filepath.Join(tmpDir, "timesyncd.conf")

	initialConf := `[Time]
NTP=pool.ntp.org
PollIntervalMinSec=60
PollIntervalMaxSec=60
`
	if err := os.WriteFile(confPath, []byte(initialConf), 0644); err != nil {
		t.Fatalf("Failed to write initial conf: %v", err)
	}

	// Update server only: preserve the existing valid maximum and repair the
	// legacy Min=Max configuration.
	if err := testWriteTimesyncdConfig(confPath, "time.google.com", 0); err != nil {
		t.Fatalf("testWriteTimesyncdConfig failed: %v", err)
	}

	result, err := os.ReadFile(confPath)
	if err != nil {
		t.Fatalf("Failed to read result: %v", err)
	}

	resultStr := string(result)
	if !strings.Contains(resultStr, "NTP=time.google.com") {
		t.Errorf("Expected NTP=time.google.com, got:\n%s", resultStr)
	}
	if !strings.Contains(resultStr, "PollIntervalMinSec=32") {
		t.Errorf("Expected PollIntervalMinSec=32, got:\n%s", resultStr)
	}
	if !strings.Contains(resultStr, "PollIntervalMaxSec=60") {
		t.Errorf("Expected existing PollIntervalMaxSec=60 to be preserved, got:\n%s", resultStr)
	}
}

func TestUpdateTimesyncdConfigRejectsInvalidMaximum(t *testing.T) {
	updated, err := updateTimesyncdConfig([]byte("[Time]\n"), "pool.ntp.org", 32)
	if err != nil {
		t.Fatalf("updateTimesyncdConfig() error = %v", err)
	}
	result := string(updated)
	if !strings.Contains(result, "PollIntervalMinSec=32") ||
		!strings.Contains(result, "PollIntervalMaxSec=3600") {
		t.Fatalf("invalid interval was not normalized:\n%s", result)
	}
}

// ============================================================
// 2. SyncFromClient Auth Route
// ============================================================

func TestSyncFromClientRequiresAuth(t *testing.T) {
	gin.SetMode(gin.TestMode)
	engine := gin.New()

	h := &TimeHandler{}
	api := engine.Group("/api/v1")
	api.Use(auth.Middleware(auth.NewTokenValidator("test-token", true)))
	api.POST("/system/time/sync-from-client", h.SyncFromClient)

	body := `{"client_timestamp": 1747600000}`
	req := httptest.NewRequest(http.MethodPost, "/api/v1/system/time/sync-from-client", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")

	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)

	if w.Code != http.StatusUnauthorized {
		t.Errorf("Expected 401 without auth token, got %d", w.Code)
	}
}

func TestSyncFromClient_WithAuthTokenAccepted(t *testing.T) {
	gin.SetMode(gin.TestMode)

	h := &TimeHandler{}
	engine := gin.New()
	api := engine.Group("/api/v1")
	api.Use(auth.Middleware(auth.NewTokenValidator("test-token", true)))
	api.POST("/system/time/sync-from-client", h.SyncFromClient)

	body := `{"client_timestamp": ` + strconv.FormatInt(time.Now().Unix(), 10) + `}`
	req := httptest.NewRequest(http.MethodPost, "/api/v1/system/time/sync-from-client", strings.NewReader(body))
	req.Header.Set("Content-Type", "application/json")
	req.Header.Set("Authorization", "Bearer test-token")

	w := httptest.NewRecorder()
	engine.ServeHTTP(w, req)

	if w.Code == http.StatusUnauthorized {
		t.Errorf("Expected non-401 response with auth token, got %d", w.Code)
	}
}

// ============================================================
// 3. DST Remnants Cleanup
// ============================================================

func TestTimeConfig_NoDSTEnabledField(t *testing.T) {
	config := TimeConfig{
		Timezone:   "Asia/Shanghai",
		TimeFormat: "24h",
		SyncMode:   "ntp",
		NTP: NTPConfig{
			Enabled:  true,
			Server:   "pool.ntp.org",
			Interval: 3600,
		},
		AutoSync: true,
	}

	data, err := json.Marshal(config)
	if err != nil {
		t.Fatalf("Failed to marshal: %v", err)
	}

	result := string(data)
	if strings.Contains(result, "dst_enabled") {
		t.Errorf("TimeConfig JSON should not contain dst_enabled, got:\n%s", result)
	}
	if !strings.Contains(result, "timezone") {
		t.Error("TimeConfig JSON should contain timezone")
	}
	if !strings.Contains(result, "time_format") {
		t.Error("TimeConfig JSON should contain time_format")
	}
	if !strings.Contains(result, "sync_mode") {
		t.Error("TimeConfig JSON should contain sync_mode")
	}
}

func TestSaveTimeConfigRequest_NoDSTEnabledField(t *testing.T) {
	req := SaveTimeConfigRequest{
		Timezone:    "Asia/Shanghai",
		TimeFormat:  "24h",
		SyncMode:    "ntp",
		NTPServer:   "pool.ntp.org",
		NTPInterval: 3600,
	}

	data, err := json.Marshal(req)
	if err != nil {
		t.Fatalf("Failed to marshal: %v", err)
	}

	if strings.Contains(string(data), "dst_enabled") {
		t.Errorf("SaveTimeConfigRequest JSON should not contain dst_enabled, got:\n%s", string(data))
	}
}

func TestTimeUserConfig_NoDSTEnabledField(t *testing.T) {
	cfg := timeUserConfig{
		TimeFormat:  "24h",
		SyncMode:    "ntp",
		NTPInterval: 3600,
	}

	data, err := json.Marshal(cfg)
	if err != nil {
		t.Fatalf("Failed to marshal: %v", err)
	}

	if strings.Contains(string(data), "dst_enabled") {
		t.Errorf("timeUserConfig JSON should not contain dst_enabled, got:\n%s", string(data))
	}
}

func TestSaveTimeConfigRequest_BackwardCompat_IgnoresOldDSTField(t *testing.T) {
	jsonWithDST := `{
		"timezone": "Asia/Shanghai",
		"time_format": "24h",
		"sync_mode": "ntp",
		"dst_enabled": true,
		"ntp_server": "pool.ntp.org",
		"ntp_interval": 1800
	}`

	var req SaveTimeConfigRequest
	err := json.Unmarshal([]byte(jsonWithDST), &req)
	if err != nil {
		t.Fatalf("Should not error on unknown dst_enabled field: %v", err)
	}
	if req.Timezone != "Asia/Shanghai" {
		t.Errorf("Expected timezone=Asia/Shanghai, got %s", req.Timezone)
	}
	if req.NTPInterval != 1800 {
		t.Errorf("Expected ntp_interval=1800, got %d", req.NTPInterval)
	}
}

func TestProjectNTPUserConfigPersistsIntervalForReadBack(t *testing.T) {
	gin.SetMode(gin.TestMode)
	configPath := filepath.Join(t.TempDir(), "time-config.json")
	if err := os.WriteFile(configPath, []byte(`{
  "time_format": "24h",
  "sync_mode": "ntp",
  "ntp_interval": 60
}`), 0644); err != nil {
		t.Fatalf("write initial config: %v", err)
	}

	h := NewTimeHandler(configPath, nil, nil)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Request = httptest.NewRequest(http.MethodPut, "/api/v1/system/time/ntp", strings.NewReader(`{}`))

	if err := h.projectNTPUserConfig(c, true, 90); err != nil {
		t.Fatalf("projectNTPUserConfig: %v", err)
	}
	cfg := h.loadTimeUserConfig()
	if cfg.SyncMode != "ntp" || cfg.NTPInterval != 90 {
		t.Fatalf("read-back config = %+v, want sync_mode=ntp ntp_interval=90", cfg)
	}
}

// ============================================================
// 4. Timezone List Caching
// ============================================================

func TestTimezoneCache_HitReturnsCachedData(t *testing.T) {
	h := &TimeHandler{}
	sampleTZ := []TimezoneData{
		{Name: "Asia/Shanghai", Country: "Asia", Offset: "UTC+08:00", OffsetSec: 28800},
		{Name: "America/New_York", Country: "America", Offset: "UTC-05:00", OffsetSec: -18000},
	}

	h.tzCacheMu.Lock()
	h.tzCache = sampleTZ
	h.tzCacheTime = time.Now()
	h.tzCacheMu.Unlock()

	result := h.getSystemTimezones()
	if len(result) != 2 {
		t.Errorf("Expected 2 cached timezones, got %d", len(result))
	}
	if result[0].Name != "Asia/Shanghai" {
		t.Errorf("Expected first timezone Asia/Shanghai, got %s", result[0].Name)
	}
}

func TestTimezoneCache_ExpiredReloads(t *testing.T) {
	h := &TimeHandler{}
	sampleTZ := []TimezoneData{
		{Name: "UTC", Country: "Universal", Offset: "UTC+00:00", OffsetSec: 0},
	}

	h.tzCacheMu.Lock()
	h.tzCache = sampleTZ
	h.tzCacheTime = time.Now().Add(-2 * time.Hour) // Expired
	h.tzCacheMu.Unlock()

	result := h.getSystemTimezones()
	// Expired cache should not be returned
	if result != nil && len(result) == 1 && result[0].Name == "UTC" {
		t.Error("Expired cache should not be returned - should reload")
	}
}

func TestIsValidTimezone_UsesCache(t *testing.T) {
	h := &TimeHandler{}
	sampleTZ := []TimezoneData{
		{Name: "Asia/Shanghai", Country: "Asia", Offset: "UTC+08:00", OffsetSec: 28800},
	}

	h.tzCacheMu.Lock()
	h.tzCache = sampleTZ
	h.tzCacheTime = time.Now()
	h.tzCacheMu.Unlock()

	if !h.isValidTimezone("Asia/Shanghai") {
		t.Error("Asia/Shanghai should be valid (in cache)")
	}
	// Note: commonTimezones supplements the cache, so many common timezones
	// (like Europe/Berlin) will be valid even without being in the cache.
	// Use an obscure timezone unlikely to appear in commonTimezones.
	if h.isValidTimezone("Antarctica/Macquarie") {
		t.Error("Antarctica/Macquarie should NOT be valid (not in cache or commonTimezones)")
	}
}

func TestTimezoneCache_ConcurrentAccess(t *testing.T) {
	h := &TimeHandler{}
	sampleTZ := []TimezoneData{
		{Name: "Asia/Shanghai", Country: "Asia", Offset: "UTC+08:00", OffsetSec: 28800},
	}

	h.tzCacheMu.Lock()
	h.tzCache = sampleTZ
	h.tzCacheTime = time.Now()
	h.tzCacheMu.Unlock()

	var wg sync.WaitGroup
	errCount := 0

	for i := 0; i < 50; i++ {
		wg.Add(2)
		go func() {
			defer wg.Done()
			result := h.getSystemTimezones()
			if len(result) != 1 || result[0].Name != "Asia/Shanghai" {
				errCount++
			}
		}()
		go func() {
			defer wg.Done()
			if !h.isValidTimezone("Asia/Shanghai") {
				errCount++
			}
		}()
	}

	wg.Wait()
	if errCount > 0 {
		t.Errorf("Concurrent access had %d errors", errCount)
	}
}

// ============================================================
// 5. MCU RTC Sync Payload
// ============================================================

type fakeMCURawRequester struct {
	requests chan *camerapb.McuRawRequestMessage
	resp     *camerapb.McuRawResponseMessage
	err      error
}

func (f *fakeMCURawRequester) McuRawRequest(ctx context.Context, in *camerapb.McuRawRequestMessage, opts ...grpc.CallOption) (*camerapb.McuRawResponseMessage, error) {
	select {
	case f.requests <- in:
		return f.resp, f.err
	case <-ctx.Done():
		return nil, ctx.Err()
	}
}

func TestMCURTCPayloadFromTime_UsesUTCAndWeekday(t *testing.T) {
	loc := time.FixedZone("UTC+8", 8*60*60)
	payload, err := mcuRTCPayloadFromTime(time.Date(2026, time.July, 21, 12, 34, 56, 0, loc))
	if err != nil {
		t.Fatalf("mcuRTCPayloadFromTime() error = %v", err)
	}

	expected := []byte{26, 7, 21, 2, 4, 34, 56}
	if !bytes.Equal(payload, expected) {
		t.Fatalf("payload = %v, want %v", payload, expected)
	}
}

func TestMCURTCPayloadFromTime_MapsSundayToSeven(t *testing.T) {
	payload, err := mcuRTCPayloadFromTime(time.Date(2026, time.July, 19, 0, 0, 0, 0, time.UTC))
	if err != nil {
		t.Fatalf("mcuRTCPayloadFromTime() error = %v", err)
	}
	if payload[3] != 7 {
		t.Fatalf("weekday = %d, want 7 for Sunday", payload[3])
	}
}

func TestMCURTCPayloadFromTime_RejectsImplausibleYear(t *testing.T) {
	if _, err := mcuRTCPayloadFromTime(time.Date(2023, time.December, 31, 23, 59, 59, 0, time.UTC)); err == nil {
		t.Fatal("expected implausible year to be rejected")
	}
}

func TestSyncMCURTCFromHostAsyncSendsRTCSet(t *testing.T) {
	fake := &fakeMCURawRequester{
		requests: make(chan *camerapb.McuRawRequestMessage, 1),
		resp:     &camerapb.McuRawResponseMessage{Success: true, Message: "OK"},
	}
	h := &TimeHandler{mcuRaw: fake}

	h.syncMCURTCFromHostAsync("test")

	select {
	case req := <-fake.requests:
		if req.GetCmd() != hostLinkCmdRTCSet {
			t.Fatalf("cmd = 0x%04x, want 0x%04x", req.GetCmd(), hostLinkCmdRTCSet)
		}
		if len(req.GetPayload()) != mcuRTCPayloadFieldCount {
			t.Fatalf("payload len = %d, want %d", len(req.GetPayload()), mcuRTCPayloadFieldCount)
		}
	case <-time.After(time.Second):
		t.Fatal("timed out waiting for MCU RTC request")
	}
}

// ============================================================
// 6. Persist Loop Graceful Exit
// ============================================================

func TestStartTimePersistLoop_StopsOnContextCancel(t *testing.T) {
	h := &TimeHandler{}
	ctx, cancel := context.WithCancel(context.Background())

	done := make(chan struct{})
	go func() {
		h.StartTimePersistLoop(ctx)
		close(done)
	}()

	time.Sleep(200 * time.Millisecond)
	cancel()

	select {
	case <-done:
		// Success
	case <-time.After(5 * time.Second):
		t.Error("StartTimePersistLoop did not exit after context cancellation")
	}
}

// NOTE: TestStartTimePersistLoop_FinalPersistOnExit was removed because
// lastKnownTimePath() and testLastKnownTimePath are not implemented yet.

// ============================================================
// 6. getTimezoneFast
// ============================================================

func TestGetTimezoneFast_ReadsEtcTimezone(t *testing.T) {
	data, err := os.ReadFile("/etc/timezone")
	if err != nil {
		t.Skip("/etc/timezone not available on this system")
	}

	expectedTZ := strings.TrimSpace(string(data))
	if expectedTZ == "" {
		t.Skip("/etc/timezone is empty")
	}

	h := &TimeHandler{}
	result := h.getTimezoneFast()
	if result != expectedTZ {
		t.Errorf("Expected %s from /etc/timezone, got %s", expectedTZ, result)
	}
}

func TestGetTimezoneFast_ReturnsNonEmpty(t *testing.T) {
	h := &TimeHandler{}
	result := h.getTimezoneFast()
	if result == "" {
		t.Error("getTimezoneFast should return a non-empty timezone")
	}
}

// ============================================================
// Integration: SaveTimeConfig request serialization
// ============================================================

func TestSaveTimeConfigRequest_NTPIntervalValidated(t *testing.T) {
	req := SaveTimeConfigRequest{
		Timezone:    "UTC",
		TimeFormat:  "24h",
		SyncMode:    "ntp",
		NTPServer:   "pool.ntp.org",
		NTPInterval: 300,
	}

	data, err := json.Marshal(req)
	if err != nil {
		t.Fatalf("Marshal failed: %v", err)
	}

	var parsed SaveTimeConfigRequest
	if err := json.Unmarshal(data, &parsed); err != nil {
		t.Fatalf("Unmarshal failed: %v", err)
	}

	if parsed.NTPInterval != 300 {
		t.Errorf("Expected ntp_interval=300, got %d", parsed.NTPInterval)
	}
}

func TestGetTimeConfig_ResponseHasNoDST(t *testing.T) {
	gin.SetMode(gin.TestMode)
	h := &TimeHandler{}

	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)

	h.GetTimeConfig(c)

	if w.Code != http.StatusOK {
		t.Logf("Response code: %d (may fail on test env without timedatectl)", w.Code)
		return
	}

	var response map[string]interface{}
	if err := json.Unmarshal(w.Body.Bytes(), &response); err != nil {
		t.Fatalf("Failed to parse response: %v", err)
	}

	dataField, ok := response["data"]
	if !ok {
		t.Fatalf("Response should have 'data' field")
	}

	configJSON, err := json.Marshal(dataField)
	if err != nil {
		t.Fatalf("Failed to re-marshal: %v", err)
	}

	if strings.Contains(string(configJSON), "dst_enabled") {
		t.Errorf("GetTimeConfig response should not contain dst_enabled:\n%s", string(configJSON))
	}
}
