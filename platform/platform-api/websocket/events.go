package websocket

import (
	"context"
	"encoding/json"
	"net/http"
	"sync"
	"time"

	"aipc/platform/common/logger"
	eventpb "aipc/platform/event-bus/proto"
	"github.com/gorilla/websocket"
	"google.golang.org/grpc"
)

// Client represents a WebSocket client
type Client struct {
	conn *websocket.Conn
	send chan []byte
}

// EventStream manages WebSocket connections for event streaming
type EventStream struct {
	eventBusConn *grpc.ClientConn
	clients      map[*Client]bool
	clientsMutex sync.RWMutex
	register     chan *Client
	unregister   chan *Client
	stopCh       chan struct{}
}

// Upgrader for WebSocket connections
var upgrader = websocket.Upgrader{
	ReadBufferSize:  1024,
	WriteBufferSize: 1024,
	CheckOrigin: func(r *http.Request) bool {
		return true
	},
}

// NewEventStream creates a new event stream manager
func NewEventStream(eventBusConn *grpc.ClientConn) *EventStream {
	return &EventStream{
		eventBusConn: eventBusConn,
		clients:      make(map[*Client]bool),
		register:     make(chan *Client),
		unregister:   make(chan *Client),
		stopCh:       make(chan struct{}),
	}
}

// HandleWebSocket handles WebSocket connections for event streaming
func (es *EventStream) HandleWebSocket(w http.ResponseWriter, r *http.Request) {
	conn, err := upgrader.Upgrade(w, r, nil)
	if err != nil {
		logger.Error("Failed to upgrade WebSocket connection: %v", err)
		return
	}

	client := &Client{
		conn: conn,
		send: make(chan []byte, 256),
	}

	es.register <- client

	go es.writePump(client)
	go es.readPump(client)
}

// Start starts the event stream manager
func (es *EventStream) Start(ctx context.Context, topic string) {
	if es.eventBusConn != nil {
		go es.subscribeToEvents(ctx, topic)
	}
	go es.run()
}

// Stop stops the event stream manager
func (es *EventStream) Stop() {
	close(es.stopCh)

	es.clientsMutex.Lock()
	for client := range es.clients {
		close(client.send)
		client.conn.Close()
	}
	es.clients = make(map[*Client]bool)
	es.clientsMutex.Unlock()
}

// run handles client registration/unregistration
func (es *EventStream) run() {
	for {
		select {
		case client := <-es.register:
			es.clientsMutex.Lock()
			es.clients[client] = true
			es.clientsMutex.Unlock()
			logger.Info("WebSocket client connected, total: %d", len(es.clients))

		case client := <-es.unregister:
			es.clientsMutex.Lock()
			if _, ok := es.clients[client]; ok {
				delete(es.clients, client)
				close(client.send)
				client.conn.Close()
			}
			es.clientsMutex.Unlock()
			logger.Info("WebSocket client disconnected, total: %d", len(es.clients))

		case <-es.stopCh:
			return
		}
	}
}

// broadcast sends message to all connected clients
func (es *EventStream) broadcast(message []byte) {
	es.clientsMutex.RLock()
	defer es.clientsMutex.RUnlock()

	for client := range es.clients {
		select {
		case client.send <- message:
		default:
			// Client buffer full, skip
			logger.Warn("Client buffer full, dropping message")
		}
	}
}

// subscribeToEvents subscribes to event bus and broadcasts to WebSocket clients
func (es *EventStream) subscribeToEvents(ctx context.Context, topic string) {
	if es.eventBusConn == nil {
		logger.Error("Event bus connection is nil")
		return
	}

	logger.Info("Starting event subscription for topic: %s", topic)
	client := eventpb.NewEventBusClient(es.eventBusConn)

	for {
		select {
		case <-ctx.Done():
			logger.Info("Context done, stopping subscription")
			return
		case <-es.stopCh:
			logger.Info("Stop signal received, stopping subscription")
			return
		default:
		}

		logger.Info("Subscribing to event bus...")
		stream, err := client.Subscribe(ctx, &eventpb.SubscribeRequest{
			Topic:        topic,
			SubscriberId: "websocket-stream",
		})
		if err != nil {
			logger.Error("Failed to subscribe to event bus: %v", err)
			time.Sleep(2 * time.Second)
			continue
		}
		logger.Info("Successfully subscribed to event bus")

		es.receiveEvents(ctx, stream)
	}
}

func (es *EventStream) receiveEvents(ctx context.Context, stream eventpb.EventBus_SubscribeClient) {
	logger.Info("Starting to receive events...")
	for {
		// Recv() is blocking, so we need to handle context cancellation separately
		event, err := stream.Recv()
		if err != nil {
			// Check if context was cancelled
			select {
			case <-ctx.Done():
				logger.Info("Context cancelled, stopping event receiver")
				return
			case <-es.stopCh:
				logger.Info("Stop signal received, stopping event receiver")
				return
			default:
			}
			logger.Error("Error receiving event: %v", err)
			return // Return to reconnect
		}

		logger.Info("Received event: topic=%s, id=%s, timestamp_ns=%d", event.Topic, event.EventId, event.TimestampNs)

		eventJSON := map[string]interface{}{
			"topic":        event.Topic,
			"payload":      string(event.Payload),
			"payload_type": event.PayloadType,
			"timestamp_ns": event.TimestampNs,
			"source":       event.Source,
			"event_id":     event.EventId,
		}

		jsonData, err := json.Marshal(eventJSON)
		if err != nil {
			logger.Error("Failed to marshal event: %v", err)
			continue
		}

		es.broadcast(jsonData)
		logger.Info("Broadcasted event to %d clients", len(es.clients))
	}
}

// writePump pumps messages to the WebSocket connection
func (es *EventStream) writePump(client *Client) {
	ticker := time.NewTicker(30 * time.Second)
	defer func() {
		ticker.Stop()
		es.unregister <- client
	}()

	for {
		select {
		case <-es.stopCh:
			return

		case message, ok := <-client.send:
			client.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
			if !ok {
				client.conn.WriteMessage(websocket.CloseMessage, []byte{})
				return
			}

			if err := client.conn.WriteMessage(websocket.TextMessage, message); err != nil {
				logger.Error("Failed to write message: %v", err)
				return
			}

		case <-ticker.C:
			client.conn.SetWriteDeadline(time.Now().Add(10 * time.Second))
			if err := client.conn.WriteMessage(websocket.PingMessage, nil); err != nil {
				logger.Warn("Failed to send ping: %v", err)
				return
			}
		}
	}
}

// readPump pumps messages from the WebSocket connection
func (es *EventStream) readPump(client *Client) {
	defer func() {
		es.unregister <- client
	}()

	client.conn.SetReadDeadline(time.Now().Add(60 * time.Second))
	client.conn.SetPongHandler(func(string) error {
		client.conn.SetReadDeadline(time.Now().Add(60 * time.Second))
		return nil
	})

	for {
		_, _, err := client.conn.ReadMessage()
		if err != nil {
			if websocket.IsUnexpectedCloseError(err, websocket.CloseGoingAway, websocket.CloseAbnormalClosure) {
				logger.Error("WebSocket error: %v", err)
			}
			break
		}
	}
}
