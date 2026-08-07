package handlers

import (
	"context"
	"encoding/json"
	"io"
	"net/http"
	"strconv"
	"time"

	apppb "aipc/platform/app-manager/proto"
	"aipc/platform/common/logger"
	"github.com/gin-gonic/gin"
	"github.com/gorilla/websocket"
	"google.golang.org/grpc"
	"google.golang.org/protobuf/types/known/emptypb"
)

// ContainerHandlers container management API handlers
type ContainerHandlers struct {
	appManagerConn *grpc.ClientConn
}

// NewContainerHandlers creates container handlers
func NewContainerHandlers(appManagerConn *grpc.ClientConn) *ContainerHandlers {
	return &ContainerHandlers{
		appManagerConn: appManagerConn,
	}
}

// ListContainers lists all containers
func (h *ContainerHandlers) ListContainers(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	state := c.Query("state")
	search := c.Query("search")
	page, _ := strconv.Atoi(c.DefaultQuery("page", "1"))
	pageSize, _ := strconv.Atoi(c.DefaultQuery("page_size", "20"))

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	resp, err := client.ListContainers(ctx, &apppb.ListContainersRequest{
		State:  state,
		Search: search,
	})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to list containers: "+err.Error())
		return
	}

	containers := resp.Containers
	statusCount := map[string]int{
		"all":     len(containers),
		"running": 0,
		"stopped": 0,
		"error":   0,
	}
	for _, ct := range containers {
		switch ct.State {
		case "running":
			statusCount["running"]++
		case "stopped", "exited":
			statusCount["stopped"]++
		default:
			statusCount["error"]++
		}
	}

	total := len(containers)
	start := (page - 1) * pageSize
	end := start + pageSize
	if start > total {
		start = total
	}
	if end > total {
		end = total
	}

	var paged []*apppb.ContainerInfo
	if total > 0 {
		paged = containers[start:end]
	} else {
		paged = []*apppb.ContainerInfo{}
	}

	Resp(c).OK(gin.H{
		"containers":   paged,
		"total":        total,
		"page":         page,
		"page_size":    pageSize,
		"status_count": statusCount,
	})
}

// GetContainer gets container details
func (h *ContainerHandlers) GetContainer(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	id := c.Param("id")

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	resp, err := client.GetContainer(ctx, &apppb.GetContainerRequest{Id: id})
	if err != nil {
		Resp(c).FailMsg(CodeNotFound, "Container not found: "+err.Error())
		return
	}

	Resp(c).OK(gin.H{"container": resp})
}

// GetContainerStats gets container resource statistics
func (h *ContainerHandlers) GetContainerStats(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	id := c.Param("id")

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	stats, err := client.GetContainerStats(ctx, &apppb.GetContainerRequest{Id: id})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to get stats: "+err.Error())
		return
	}

	Resp(c).OK(gin.H{"stats": stats})
}

// GetContainerLogs gets container logs
func (h *ContainerHandlers) GetContainerLogs(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	id := c.Param("id")
	tail, _ := strconv.Atoi(c.DefaultQuery("tail", "100"))

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	stream, err := client.GetContainerLogs(ctx, &apppb.GetContainerLogsRequest{
		Id:     id,
		Tail:   int32(tail),
		Follow: false,
	})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to call logs: "+err.Error())
		return
	}

	var logs []string
	for {
		line, err := stream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			break
		}
		logs = append(logs, line.Message)
	}

	Resp(c).OK(gin.H{"logs": logs})
}

// StreamContainerLogs streams container logs (SSE)
func (h *ContainerHandlers) StreamContainerLogs(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	id := c.Param("id")

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx := c.Request.Context()

	stream, err := client.GetContainerLogs(ctx, &apppb.GetContainerLogsRequest{
		Id:     id,
		Tail:   100,
		Follow: true,
	})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to stream logs: "+err.Error())
		return
	}

	c.Header("Content-Type", "text/event-stream")
	c.Header("Cache-Control", "no-cache")
	c.Header("Connection", "keep-alive")

	for {
		line, err := stream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			break
		}
		c.SSEvent("log", line.Message)
		c.Writer.Flush()
	}
}

// StartContainer starts a container
func (h *ContainerHandlers) StartContainer(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	id := c.Param("id")

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	status, err := client.StartContainer(ctx, &apppb.ContainerRequest{Id: id})
	if err != nil || !status.Success {
		var msg string
		if err != nil {
			msg = err.Error()
		} else {
			msg = status.Message
		}
		Resp(c).FailMsg(CodeAppStartFailed, "Failed to start container: "+msg)
		return
	}

	Resp(c).OK(gin.H{"message": "Container started"})
}

// StopContainer stops a container
func (h *ContainerHandlers) StopContainer(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	id := c.Param("id")

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	status, err := client.StopContainer(ctx, &apppb.ContainerRequest{Id: id})
	if err != nil || !status.Success {
		var msg string
		if err != nil {
			msg = err.Error()
		} else {
			msg = status.Message
		}
		Resp(c).FailMsg(CodeAppStopFailed, "Failed to stop container: "+msg)
		return
	}

	Resp(c).OK(gin.H{"message": "Container stopped"})
}

// RestartContainer restarts a container
func (h *ContainerHandlers) RestartContainer(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	id := c.Param("id")

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	status, err := client.RestartContainer(ctx, &apppb.ContainerRequest{Id: id})
	if err != nil || !status.Success {
		var msg string
		if err != nil {
			msg = err.Error()
		} else {
			msg = status.Message
		}
		Resp(c).FailMsg(CodeOperationFailed, "Failed to restart container: "+msg)
		return
	}

	Resp(c).OK(gin.H{"message": "Container restarted"})
}

// RemoveContainer removes a container
func (h *ContainerHandlers) RemoveContainer(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	id := c.Param("id")
	force := c.Query("force") == "true"

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithTimeout(context.Background(), 20*time.Second)
	defer cancel()

	status, err := client.RemoveContainer(ctx, &apppb.RemoveContainerRequest{Id: id, Force: force})
	if err != nil || !status.Success {
		var msg string
		if err != nil {
			msg = err.Error()
		} else {
			msg = status.Message
		}
		Resp(c).FailMsg(CodeOperationFailed, "Failed to remove container: "+msg)
		return
	}

	Resp(c).OK(gin.H{"message": "Container removed"})
}

// StreamContainerLogsWS streams container logs (WebSocket)
func (h *ContainerHandlers) StreamContainerLogsWS(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	id := c.Param("id")
	tail, _ := strconv.Atoi(c.DefaultQuery("tail", "100"))

	upgrader := websocket.Upgrader{
		CheckOrigin: func(r *http.Request) bool {
			return true // Allow all origins for dev/embedded environment
		},
	}

	conn, err := upgrader.Upgrade(c.Writer, c.Request, nil)
	if err != nil {
		return
	}
	defer conn.Close()

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	stream, err := client.GetContainerLogs(ctx, &apppb.GetContainerLogsRequest{
		Id:     id,
		Tail:   int32(tail),
		Follow: true,
	})
	if err != nil {
		conn.WriteMessage(websocket.TextMessage, []byte("Error: "+err.Error()))
		return
	}

	// Read from WS to detect disconnect
	go func() {
		for {
			if _, _, err := conn.ReadMessage(); err != nil {
				cancel()
				break
			}
		}
	}()

	for {
		line, err := stream.Recv()
		if err == io.EOF {
			break
		}
		if err != nil {
			conn.WriteMessage(websocket.TextMessage, []byte("Stream error: "+err.Error()))
			break
		}
		if err := conn.WriteMessage(websocket.TextMessage, []byte(line.Message+"\n")); err != nil {
			break
		}
	}
}

// ExecContainerWS executes an interactive command in a container via WebSocket
func (h *ContainerHandlers) ExecContainerWS(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	id := c.Param("id")

	upgrader := websocket.Upgrader{
		CheckOrigin: func(r *http.Request) bool {
			return true
		},
	}

	conn, err := upgrader.Upgrade(c.Writer, c.Request, nil)
	if err != nil {
		return
	}
	defer conn.Close()

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()

	stream, err := client.ExecContainer(ctx)
	if err != nil {
		conn.WriteMessage(websocket.TextMessage, []byte("Error: "+err.Error()))
		return
	}

	// Parse optional rows/cols query parameters
	cols, _ := strconv.Atoi(c.DefaultQuery("cols", "80"))
	rows, _ := strconv.Atoi(c.DefaultQuery("rows", "24"))
	command := c.DefaultQuery("command", "/bin/sh")

	// Send initial request telling appManager which container we want
	err = stream.Send(&apppb.ExecInput{
		AppId:   id,
		Command: command,
		Cols:    int32(cols),
		Rows:    int32(rows),
		Tty:     true, // Enable PTY mode for interactive terminal
	})
	if err != nil {
		conn.WriteMessage(websocket.TextMessage, []byte("Init error: "+err.Error()))
		return
	}

	// Read from AppManager and write to WS (Stdout/Stderr)
	go func() {
		for {
			resp, err := stream.Recv()
			if err == io.EOF {
				conn.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, "Exited"))
				return
			}
			if err != nil {
				conn.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseInternalServerErr, err.Error()))
				return
			}

			if len(resp.Stdout) > 0 {
				logger.Debug("Writing %d bytes to WebSocket Stdout", len(resp.Stdout))
				conn.WriteMessage(websocket.BinaryMessage, resp.Stdout)
			}
			if len(resp.Stderr) > 0 {
				logger.Debug("Writing %d bytes to WebSocket Stderr", len(resp.Stderr))
				conn.WriteMessage(websocket.BinaryMessage, resp.Stderr)
			}
			if resp.Exited {
				logger.Info("Container exec process exited")
				conn.WriteMessage(websocket.CloseMessage, websocket.FormatCloseMessage(websocket.CloseNormalClosure, "Exited"))
				return
			}
		}
	}()

	// Read from WS and write to AppManager (Stdin or Resize events)
	for {
		msgType, msg, err := conn.ReadMessage()
		if err != nil {
			cancel()
			break
		}

		if msgType == websocket.BinaryMessage {
			// Binary data is stdin
			logger.Debug("Received binary message from WS: %d bytes", len(msg))
			stream.Send(&apppb.ExecInput{
				Stdin: msg,
			})
		} else if msgType == websocket.TextMessage {
			// Try to parse as JSON for control messages
			var jsonMsg map[string]interface{}
			if err := json.Unmarshal(msg, &jsonMsg); err == nil {
				// Check if it's a resize message
				if msgType, ok := jsonMsg["type"].(string); ok && msgType == "resize" {
					cols := 80
					rows := 24
					if c, ok := jsonMsg["cols"].(float64); ok {
						cols = int(c)
					}
					if r, ok := jsonMsg["rows"].(float64); ok {
						rows = int(r)
					}
					stream.Send(&apppb.ExecInput{
						Resize: true,
						Cols:   int32(cols),
						Rows:   int32(rows),
					})
					continue
				}
			}
			// Otherwise treat as stdin
			stream.Send(&apppb.ExecInput{
				Stdin: msg,
			})
		}
	}
}

// ========== Image Management ==========

// ListImages lists images
func (h *ContainerHandlers) ListImages(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()

	resp, err := client.ListImages(ctx, &emptypb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, "Failed to list images: "+err.Error())
		return
	}

	Resp(c).OK(gin.H{"images": resp.Images})
}

// PullImage pulls an image
func (h *ContainerHandlers) PullImage(c *gin.Context) {
	Resp(c).FailMsg(CodeOperationFailed, "Pulling remote images is disabled. Please use App installation over .tar via App Manager.")
}

// DeleteImage deletes an image
func (h *ContainerHandlers) DeleteImage(c *gin.Context) {
	if h.appManagerConn == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "App Manager not available")
		return
	}
	image := c.Param("image")

	client := apppb.NewAppManagerClient(h.appManagerConn)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()

	status, err := client.RemoveImage(ctx, &apppb.RemoveImageRequest{ImageId: image, Force: true})
	if err != nil || !status.Success {
		var msg string
		if err != nil {
			msg = err.Error()
		} else {
			msg = status.Message
		}
		Resp(c).FailMsg(CodeFileDeleteFailed, "Failed to delete image: "+msg)
		return
	}

	Resp(c).OK(gin.H{"message": "Image deleted"})
}
