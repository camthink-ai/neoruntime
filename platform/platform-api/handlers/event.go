package handlers

import (
	"context"
	"encoding/json"
	"time"

	"github.com/gin-gonic/gin"

	eventpb "aipc/platform/event-bus/proto"
)

// Event Bus handlers

func (h *APIHandlers) ListTopics(c *gin.Context) {
	if h.grpcClients.EventBus == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Event Bus not available")
		return
	}

	client := eventpb.NewEventBusClient(h.grpcClients.EventBus)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	resp, err := client.ListTopics(ctx, &eventpb.Empty{})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(resp)
}

func (h *APIHandlers) PublishEvent(c *gin.Context) {
	if h.grpcClients.EventBus == nil {
		Resp(c).FailMsg(CodeServiceUnavailable, "Event Bus not available")
		return
	}

	var req struct {
		Topic   string                 `json:"topic"`
		Payload map[string]interface{} `json:"payload"`
	}

	if err := c.ShouldBindJSON(&req); err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid request body: "+err.Error())
		return
	}

	if req.Topic == "" {
		Resp(c).FailMsg(CodeInvalidRequest, "Topic is required")
		return
	}

	// Convert payload to JSON bytes
	payloadBytes, err := json.Marshal(req.Payload)
	if err != nil {
		Resp(c).FailMsg(CodeInvalidRequest, "Invalid payload: "+err.Error())
		return
	}

	client := eventpb.NewEventBusClient(h.grpcClients.EventBus)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()

	event := &eventpb.Event{
		Topic:       req.Topic,
		TimestampNs: uint64(time.Now().UnixNano()),
		Payload:     payloadBytes,
		PayloadType: "json",
	}

	resp, err := client.Publish(ctx, &eventpb.PublishRequest{
		Event: event,
	})
	if err != nil {
		Resp(c).FailMsg(CodeServiceError, err.Error())
		return
	}

	Resp(c).OK(resp)
}
