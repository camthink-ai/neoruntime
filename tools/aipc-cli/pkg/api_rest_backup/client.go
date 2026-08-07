/*
Package api provides the REST API client for AIPC platform.
*/
package api

import (
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"mime/multipart"
	"net/http"
	"os"
	"path/filepath"
	"time"
)

// Client is the REST API client for AIPC platform
type Client struct {
	baseURL    string
	httpClient *http.Client
	authToken  string
}

// NewClient creates a new API client
func NewClient(baseURL string, timeout time.Duration) (*Client, error) {
	if timeout <= 0 {
		timeout = 30 * time.Second
	}

	return &Client{
		baseURL: baseURL,
		httpClient: &http.Client{
			Timeout: timeout,
		},
	}, nil
}

// SetAuthToken sets the authentication token
func (c *Client) SetAuthToken(token string) {
	c.authToken = token
}

// Close closes the client (cleanup if needed)
func (c *Client) Close() {
	// Currently no cleanup needed
}

// ============ Common Types ============

// Status represents API response status
type Status struct {
	Success bool   `json:"success"`
	Message string `json:"message"`
	Code    int    `json:"code,omitempty"`
}

// ErrorResponse represents an error response
type ErrorResponse struct {
	Error   string `json:"error"`
	Code    int    `json:"code,omitempty"`
	Details string `json:"details,omitempty"`
}

// ============ App Types ============

// AppInfo represents application information
type AppInfo struct {
	ID           string `json:"id"`
	Name         string `json:"name"`
	Version      string `json:"version"`
	State        string `json:"state"`
	ContainerID  string `json:"container_id,omitempty"`
	PID          int    `json:"pid,omitempty"`
	InstalledAt  int64  `json:"installed_at,omitempty"`
	StartedAt    int64  `json:"started_at,omitempty"`
	StoppedAt    int64  `json:"stopped_at,omitempty"`
	RestartCount int    `json:"restart_count,omitempty"`
}

// AppList represents a list of applications
type AppList struct {
	Apps []AppInfo `json:"apps"`
}

// AppStats represents application statistics
type AppStats struct {
	AppID            string  `json:"app_id"`
	CPUUsagePercent  float64 `json:"cpu_usage_percent"`
	MemoryUsageBytes int64   `json:"memory_usage_bytes"`
	MemoryLimitBytes int64   `json:"memory_limit_bytes"`
	ThreadCount      int     `json:"thread_count"`
	UptimeSeconds    int64   `json:"uptime_seconds"`
}

// InstallRequest represents app install request
type InstallRequest struct {
	ManifestPath string `json:"manifest_path"`
	ImagePath    string `json:"image_path"`
}

// InstallResponse represents app install response
type InstallResponse struct {
	Status  Status `json:"status"`
	AppID   string `json:"app_id"`
	Message string `json:"message"`
}

// ============ Model Types ============

// ModelInfo represents AI model information
type ModelInfo struct {
	ModelID         string  `json:"model_id"`
	ModelPath       string  `json:"model_path"`
	Version         string  `json:"version"`
	EstimatedTOPS   float32 `json:"estimated_tops"`
	EstimatedMemory uint32  `json:"estimated_memory"`
	LoadTimestamp   uint64  `json:"load_timestamp"`
}

// ModelList represents a list of models
type ModelList struct {
	Models []ModelInfo `json:"models"`
}

// ============ Device Types ============

// DeviceStatus represents device status
type DeviceStatus struct {
	SocTempC    float32 `json:"soc_temp_c"`
	McuTempC    float32 `json:"mcu_temp_c"`
	LightSensor int32   `json:"light_sensor"`
	PtzPanPos   int32   `json:"ptz_pan_pos"`
	PtzTiltPos  int32   `json:"ptz_tilt_pos"`
	ZoomPos     int32   `json:"zoom_pos"`
	FocusPos    int32   `json:"focus_pos"`
	McuVersion  string  `json:"mcu_version"`
}

// ============ System Types ============

// SystemInfo represents system information
type SystemInfo struct {
	Version  string          `json:"version"`
	Services map[string]bool `json:"services"`
}

// SystemStats represents system statistics
type SystemStats struct {
	Timestamp int64                  `json:"timestamp"`
	Services  map[string]interface{} `json:"services"`
}

// HealthStatus represents health check response
type HealthStatus struct {
	Status  string `json:"status"`
	Time    int64  `json:"time"`
	Service string `json:"service"`
}

// ============ Stream Types ============

// StreamInfo represents video stream information
type StreamInfo struct {
	ID         string `json:"id"`
	Resolution string `json:"resolution"`
	FPS        int    `json:"fps"`
	Codec      string `json:"codec"`
	Status     string `json:"status"`
}

// StreamList represents a list of streams
type StreamList struct {
	Streams []StreamInfo `json:"streams"`
}

// ============ Event Types ============

// TopicList represents a list of event topics
type TopicList struct {
	Topics []string `json:"topics"`
}

// ============ HTTP Methods ============

// doRequest performs an HTTP request and handles common response processing
func (c *Client) doRequest(ctx context.Context, method, path string, body interface{}) ([]byte, error) {
	url := c.baseURL + path

	var reqBody io.Reader
	if body != nil {
		jsonData, err := json.Marshal(body)
		if err != nil {
			return nil, fmt.Errorf("failed to marshal request body: %w", err)
		}
		reqBody = bytes.NewReader(jsonData)
	}

	req, err := http.NewRequestWithContext(ctx, method, url, reqBody)
	if err != nil {
		return nil, fmt.Errorf("failed to create request: %w", err)
	}

	req.Header.Set("Content-Type", "application/json")
	if c.authToken != "" {
		req.Header.Set("Authorization", "Bearer "+c.authToken)
	}

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("request failed: %w", err)
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("failed to read response: %w", err)
	}

	if resp.StatusCode >= 400 {
		var errResp ErrorResponse
		if err := json.Unmarshal(respBody, &errResp); err == nil && errResp.Error != "" {
			return nil, fmt.Errorf("API error: %s", errResp.Error)
		}
		return nil, fmt.Errorf("API error: %s (status %d)", string(respBody), resp.StatusCode)
	}

	return respBody, nil
}

// get performs a GET request
func (c *Client) get(ctx context.Context, path string) ([]byte, error) {
	return c.doRequest(ctx, http.MethodGet, path, nil)
}

// post performs a POST request
func (c *Client) post(ctx context.Context, path string, body interface{}) ([]byte, error) {
	return c.doRequest(ctx, http.MethodPost, path, body)
}

// delete performs a DELETE request
func (c *Client) delete(ctx context.Context, path string) ([]byte, error) {
	return c.doRequest(ctx, http.MethodDelete, path, nil)
}

// ============ App API ============

// ListApps lists all installed applications
func (c *Client) ListApps(ctx context.Context) (*AppList, error) {
	data, err := c.get(ctx, "/api/v1/apps")
	if err != nil {
		return nil, err
	}

	var result AppList
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// GetApp gets application information
func (c *Client) GetApp(ctx context.Context, appID string) (*AppInfo, error) {
	data, err := c.get(ctx, "/api/v1/apps/"+appID)
	if err != nil {
		return nil, err
	}

	var result AppInfo
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// GetAppStats gets application statistics
func (c *Client) GetAppStats(ctx context.Context, appID string) (*AppStats, error) {
	data, err := c.get(ctx, "/api/v1/apps/"+appID+"/stats")
	if err != nil {
		return nil, err
	}

	var result AppStats
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// InstallApp installs an application
func (c *Client) InstallApp(ctx context.Context, manifestPath, imagePath string) (*InstallResponse, error) {
	req := InstallRequest{
		ManifestPath: manifestPath,
		ImagePath:    imagePath,
	}

	data, err := c.post(ctx, "/api/v1/apps", req)
	if err != nil {
		return nil, err
	}

	var result InstallResponse
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// StartApp starts an application
func (c *Client) StartApp(ctx context.Context, appID string) (*Status, error) {
	data, err := c.post(ctx, "/api/v1/apps/"+appID+"/start", nil)
	if err != nil {
		return nil, err
	}

	var result Status
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// StopApp stops an application
func (c *Client) StopApp(ctx context.Context, appID string) (*Status, error) {
	data, err := c.post(ctx, "/api/v1/apps/"+appID+"/stop", nil)
	if err != nil {
		return nil, err
	}

	var result Status
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// UninstallApp uninstalls an application
func (c *Client) UninstallApp(ctx context.Context, appID string) (*Status, error) {
	data, err := c.delete(ctx, "/api/v1/apps/"+appID+"/uninstall")
	if err != nil {
		return nil, err
	}

	var result Status
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// ============ Model API ============

// ListModels lists all loaded models
func (c *Client) ListModels(ctx context.Context) (*ModelList, error) {
	data, err := c.get(ctx, "/api/v1/ai/models")
	if err != nil {
		return nil, err
	}

	var result ModelList
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// GetAIStats gets AI runtime statistics
func (c *Client) GetAIStats(ctx context.Context) (map[string]interface{}, error) {
	data, err := c.get(ctx, "/api/v1/ai/stats")
	if err != nil {
		return nil, err
	}

	var result map[string]interface{}
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return result, nil
}

// ============ Device API ============

// GetDeviceStatus gets device status
func (c *Client) GetDeviceStatus(ctx context.Context) (*DeviceStatus, error) {
	data, err := c.get(ctx, "/api/v1/device/status")
	if err != nil {
		return nil, err
	}

	var result DeviceStatus
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// SetLight sets white light level
func (c *Client) SetLight(ctx context.Context, level int) (*Status, error) {
	req := map[string]int{"level": level}
	data, err := c.post(ctx, "/api/v1/device/light", req)
	if err != nil {
		return nil, err
	}

	var result Status
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// SetIrLed sets IR LED state
func (c *Client) SetIrLed(ctx context.Context, on bool) (*Status, error) {
	req := map[string]bool{"on": on}
	data, err := c.post(ctx, "/api/v1/device/ir-led", req)
	if err != nil {
		return nil, err
	}

	var result Status
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// SetIrCut sets IR-Cut mode
func (c *Client) SetIrCut(ctx context.Context, mode string) (*Status, error) {
	req := map[string]string{"mode": mode}
	data, err := c.post(ctx, "/api/v1/device/ir-cut", req)
	if err != nil {
		return nil, err
	}

	var result Status
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// ControlPTZ controls PTZ
func (c *Client) ControlPTZ(ctx context.Context, action string, params map[string]interface{}) (*Status, error) {
	req := map[string]interface{}{"action": action}
	for k, v := range params {
		req[k] = v
	}

	data, err := c.post(ctx, "/api/v1/device/ptz", req)
	if err != nil {
		return nil, err
	}

	var result Status
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// ============ System API ============

// GetSystemInfo gets system information
func (c *Client) GetSystemInfo(ctx context.Context) (*SystemInfo, error) {
	data, err := c.get(ctx, "/api/v1/system/info")
	if err != nil {
		return nil, err
	}

	var result SystemInfo
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// GetSystemStats gets system statistics
func (c *Client) GetSystemStats(ctx context.Context) (*SystemStats, error) {
	data, err := c.get(ctx, "/api/v1/system/stats")
	if err != nil {
		return nil, err
	}

	var result SystemStats
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// HealthCheck performs health check
func (c *Client) HealthCheck(ctx context.Context) (*HealthStatus, error) {
	data, err := c.get(ctx, "/api/v1/system/health")
	if err != nil {
		return nil, err
	}

	var result HealthStatus
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// ============ Event API ============

// ListTopics lists event topics
func (c *Client) ListTopics(ctx context.Context) (*TopicList, error) {
	data, err := c.get(ctx, "/api/v1/events/topics")
	if err != nil {
		return nil, err
	}

	var result TopicList
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// PublishEvent publishes an event
func (c *Client) PublishEvent(ctx context.Context, topic string, payload map[string]interface{}) (*Status, error) {
	req := map[string]interface{}{
		"topic":   topic,
		"payload": payload,
	}

	data, err := c.post(ctx, "/api/v1/events/publish", req)
	if err != nil {
		return nil, err
	}

	var result Status
	if err := json.Unmarshal(data, &result); err != nil {
		return nil, fmt.Errorf("failed to parse response: %w", err)
	}

	return &result, nil
}

// ============ Streaming ============

// StreamGet performs a streaming GET request and returns a channel of lines
func (c *Client) StreamGet(ctx context.Context, path string) (<-chan string, error) {
	url := c.baseURL + path

	req, err := http.NewRequestWithContext(ctx, http.MethodGet, url, nil)
	if err != nil {
		return nil, fmt.Errorf("failed to create request: %w", err)
	}

	if c.authToken != "" {
		req.Header.Set("Authorization", "Bearer "+c.authToken)
	}

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("request failed: %w", err)
	}

	if resp.StatusCode >= 400 {
		body, _ := io.ReadAll(resp.Body)
		resp.Body.Close()
		return nil, fmt.Errorf("API error: %s (status %d)", string(body), resp.StatusCode)
	}

	lines := make(chan string, 100)
	go func() {
		defer resp.Body.Close()
		defer close(lines)

		decoder := json.NewDecoder(resp.Body)
		for {
			var logLine struct {
				Timestamp int64  `json:"timestamp"`
				Level     string `json:"level"`
				Message   string `json:"message"`
			}
			if err := decoder.Decode(&logLine); err != nil {
				return // EOF or error
			}

			line := logLine.Message
			if logLine.Level != "" {
				line = fmt.Sprintf("[%s] %s", logLine.Level, logLine.Message)
			}

			select {
			case lines <- line:
			case <-ctx.Done():
				return
			}
		}
	}()

	return lines, nil
}

// ============ File Upload ============

// UploadFile uploads a file to the server
func (c *Client) UploadFile(ctx context.Context, path, filePath string) ([]byte, error) {
	file, err := os.Open(filePath)
	if err != nil {
		return nil, fmt.Errorf("failed to open file: %w", err)
	}
	defer file.Close()

	body := &bytes.Buffer{}
	writer := multipart.NewWriter(body)

	part, err := writer.CreateFormFile("file", filepath.Base(filePath))
	if err != nil {
		return nil, fmt.Errorf("failed to create form file: %w", err)
	}

	if _, err := io.Copy(part, file); err != nil {
		return nil, fmt.Errorf("failed to copy file: %w", err)
	}

	writer.Close()

	req, err := http.NewRequestWithContext(ctx, http.MethodPost, c.baseURL+path, body)
	if err != nil {
		return nil, fmt.Errorf("failed to create request: %w", err)
	}

	req.Header.Set("Content-Type", writer.FormDataContentType())
	if c.authToken != "" {
		req.Header.Set("Authorization", "Bearer "+c.authToken)
	}

	resp, err := c.httpClient.Do(req)
	if err != nil {
		return nil, fmt.Errorf("upload failed: %w", err)
	}
	defer resp.Body.Close()

	respBody, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("failed to read response: %w", err)
	}

	if resp.StatusCode >= 400 {
		return nil, fmt.Errorf("upload failed: %s (status %d)", string(respBody), resp.StatusCode)
	}

	return respBody, nil
}
