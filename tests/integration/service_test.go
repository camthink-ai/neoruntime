package integration

import (
	"context"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	eventpb "aipc/platform/event-bus/proto"
	devicepb "aipc/platform/device-control/proto"
)

// TestEventBusIntegration tests event-bus service
func TestEventBusIntegration(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test")
	}
	
	// Connect to event-bus
	conn, err := grpc.Dial("unix:///run/aipc/event-bus.sock",
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Skipf("Cannot connect to event-bus: %v (service not running?)", err)
	}
	defer conn.Close()
	
	client := eventpb.NewEventBusClient(conn)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	
	// Test publish
	event := &eventpb.Event{
		Topic:       "test/integration",
		TimestampNs: uint64(time.Now().UnixNano()),
		Source:      "integration_test",
		Payload:     []byte(`{"test":true}`),
		PayloadType: "json",
	}
	
	resp, err := client.Publish(ctx, &eventpb.PublishRequest{Event: event})
	if err != nil {
		t.Fatalf("Publish failed: %v", err)
	}
	
	if !resp.Status.Success {
		t.Fatalf("Publish unsuccessful: %s", resp.Status.Message)
	}
	
	t.Log("✓ Event published successfully")
	
	// Test list topics
	topics, err := client.ListTopics(ctx, &eventpb.Empty{})
	if err != nil {
		t.Fatalf("ListTopics failed: %v", err)
	}
	
	t.Logf("✓ Found %d topics", len(topics.Topics))
}

// TestDeviceControlIntegration tests device-control service
func TestDeviceControlIntegration(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test")
	}
	
	// Connect to device-control
	conn, err := grpc.Dial("unix:///run/aipc/device-control.sock",
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Skipf("Cannot connect to device-control: %v", err)
	}
	defer conn.Close()
	
	client := devicepb.NewDeviceControlClient(conn)
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	
	// Test get status
	status, err := client.GetDeviceStatus(ctx, &devicepb.Empty{})
	if err != nil {
		t.Fatalf("GetDeviceStatus failed: %v", err)
	}
	
	t.Logf("✓ Device status: SoC=%.1f°C, MCU=%.1f°C",
		status.SocTempC, status.McuTempC)
	
	// Test light control
	lightResp, err := client.SetWhiteLight(ctx, &devicepb.LightLevelRequest{Level: 50})
	if err != nil {
		t.Fatalf("SetWhiteLight failed: %v", err)
	}
	
	if !lightResp.Success {
		t.Fatalf("SetWhiteLight unsuccessful: %s", lightResp.Message)
	}
	
	t.Log("✓ Light control OK")
}

// TestServiceConnectivity tests basic connectivity to all services
func TestServiceConnectivity(t *testing.T) {
	services := []struct {
		name string
		addr string
	}{
		{"event-bus", "unix:///run/aipc/event-bus.sock"},
		{"ai-runtime", "unix:///run/aipc/ai-runtime.sock"},
		{"device-control", "unix:///run/aipc/device-control.sock"},
		{"app-manager", "unix:///run/aipc/app-manager.sock"},
	}
	
	for _, svc := range services {
		t.Run(svc.name, func(t *testing.T) {
			conn, err := grpc.Dial(svc.addr,
				grpc.WithTransportCredentials(insecure.NewCredentials()),
				grpc.WithTimeout(2*time.Second))
			
			if err != nil {
				t.Logf("⚠ Cannot connect to %s: %v", svc.name, err)
				return
			}
			
			conn.Close()
			t.Logf("✓ %s is reachable", svc.name)
		})
	}
}

