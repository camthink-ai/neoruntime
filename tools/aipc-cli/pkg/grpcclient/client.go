/*
Package grpcclient provides gRPC clients for AIPC platform services.
*/
package grpcclient

import (
	"context"
	"fmt"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	apppb "aipc/platform/app-manager/proto"
	inferencepb "aipc/platform/ai-runtime/proto"
	devicepb "aipc/platform/device-control/proto"
	eventpb "aipc/platform/event-bus/proto"
)

// Config holds gRPC client configuration
type Config struct {
	// Service endpoints (Unix socket or TCP address)
	AppManagerAddr    string
	AIRuntimeAddr     string
	EventBusAddr      string
	DeviceControlAddr string

	// Connection settings
	Timeout time.Duration
}

// DefaultConfig returns default configuration
func DefaultConfig() *Config {
	return &Config{
		AppManagerAddr:    "unix:///var/run/aipc/app-manager.sock",
		AIRuntimeAddr:     "unix:///var/run/aipc/ai-runtime.sock",
		EventBusAddr:      "unix:///var/run/aipc/event-bus.sock",
		DeviceControlAddr: "unix:///var/run/aipc/device-control.sock",
		Timeout:           30 * time.Second,
	}
}

// Client wraps all gRPC service clients
type Client struct {
	config *Config

	// Connections
	appConn    *grpc.ClientConn
	aiConn     *grpc.ClientConn
	eventConn  *grpc.ClientConn
	deviceConn *grpc.ClientConn

	// Service clients
	AppManager    apppb.AppManagerClient
	AIRuntime     inferencepb.InferenceServiceClient
	EventBus      eventpb.EventBusClient
	DeviceControl devicepb.DeviceControlClient
}

// NewClient creates a new gRPC client
func NewClient(config *Config) (*Client, error) {
	if config == nil {
		config = DefaultConfig()
	}

	c := &Client{
		config: config,
	}

	return c, nil
}

// ConnectAppManager connects to the app-manager service
func (c *Client) ConnectAppManager() error {
	if c.appConn != nil {
		return nil // Already connected
	}

	ctx, cancel := context.WithTimeout(context.Background(), c.config.Timeout)
	defer cancel()

	conn, err := grpc.DialContext(ctx, c.config.AppManagerAddr,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithBlock(),
	)
	if err != nil {
		return fmt.Errorf("failed to connect to app-manager: %w", err)
	}

	c.appConn = conn
	c.AppManager = apppb.NewAppManagerClient(conn)
	return nil
}

// ConnectEventBus connects to the event-bus service
func (c *Client) ConnectEventBus() error {
	if c.eventConn != nil {
		return nil // Already connected
	}

	ctx, cancel := context.WithTimeout(context.Background(), c.config.Timeout)
	defer cancel()

	conn, err := grpc.DialContext(ctx, c.config.EventBusAddr,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithBlock(),
	)
	if err != nil {
		return fmt.Errorf("failed to connect to event-bus: %w", err)
	}

	c.eventConn = conn
	c.EventBus = eventpb.NewEventBusClient(conn)
	return nil
}

// ConnectAIRuntime connects to the ai-runtime service
func (c *Client) ConnectAIRuntime() error {
	if c.aiConn != nil {
		return nil // Already connected
	}

	ctx, cancel := context.WithTimeout(context.Background(), c.config.Timeout)
	defer cancel()

	conn, err := grpc.DialContext(ctx, c.config.AIRuntimeAddr,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithBlock(),
	)
	if err != nil {
		return fmt.Errorf("failed to connect to ai-runtime: %w", err)
	}

	c.aiConn = conn
	c.AIRuntime = inferencepb.NewInferenceServiceClient(conn)
	return nil
}

// ConnectDeviceControl connects to the device-control service
func (c *Client) ConnectDeviceControl() error {
	if c.deviceConn != nil {
		return nil // Already connected
	}

	ctx, cancel := context.WithTimeout(context.Background(), c.config.Timeout)
	defer cancel()

	conn, err := grpc.DialContext(ctx, c.config.DeviceControlAddr,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithBlock(),
	)
	if err != nil {
		return fmt.Errorf("failed to connect to device-control: %w", err)
	}

	c.deviceConn = conn
	c.DeviceControl = devicepb.NewDeviceControlClient(conn)
	return nil
}

// Close closes all connections
func (c *Client) Close() {
	if c.appConn != nil {
		c.appConn.Close()
	}
	if c.aiConn != nil {
		c.aiConn.Close()
	}
	if c.eventConn != nil {
		c.eventConn.Close()
	}
	if c.deviceConn != nil {
		c.deviceConn.Close()
	}
}

// GetAppManagerAddr returns the app-manager address
func (c *Client) GetAppManagerAddr() string {
	return c.config.AppManagerAddr
}

// SetAppManagerAddr sets the app-manager address
func (c *Client) SetAppManagerAddr(addr string) {
	c.config.AppManagerAddr = addr
}

// GetEventBusAddr returns the event-bus address
func (c *Client) GetEventBusAddr() string {
	return c.config.EventBusAddr
}

// SetEventBusAddr sets the event-bus address
func (c *Client) SetEventBusAddr(addr string) {
	c.config.EventBusAddr = addr
}
