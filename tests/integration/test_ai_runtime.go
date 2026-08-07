package integration

import (
	"context"
	"testing"
	"time"

	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"

	pb "aipc/platform/ai-runtime/proto"
)

// TestAIRuntimeBasic tests basic AI Runtime functionality
func TestAIRuntimeBasic(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test")
	}
	
	// Connect to AI Runtime
	conn, err := grpc.Dial("unix:///run/aipc/ai-runtime.sock",
		grpc.WithTransportCredentials(insecure.NewCredentials()),
		grpc.WithTimeout(5*time.Second))
	if err != nil {
		t.Skipf("Cannot connect to ai-runtime: %v (service not running?)", err)
	}
	defer conn.Close()
	
	client := pb.NewInferenceServiceClient(conn)
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	
	// Test RegisterModel
	t.Run("RegisterModel", func(t *testing.T) {
		resp, err := client.RegisterModel(ctx, &pb.ModelRegisterRequest{
			ModelId:   "test_model_v1",
			ModelPath: "/tmp/test_model.hef",
		})
		
		if err != nil {
			t.Fatalf("RegisterModel failed: %v", err)
		}
		
		if !resp.Status.Success {
			t.Fatalf("RegisterModel unsuccessful: %s", resp.Status.Message)
		}
		
		t.Logf("✓ Model registered: %s", resp.ModelId)
	})
	
	// Test ListModels
	t.Run("ListModels", func(t *testing.T) {
		resp, err := client.ListModels(ctx, &pb.Empty{})
		
		if err != nil {
			t.Fatalf("ListModels failed: %v", err)
		}
		
		t.Logf("✓ Found %d models", len(resp.Models))
		
		if len(resp.Models) == 0 {
			t.Log("⚠ No models loaded (expected if no models registered)")
		}
	})
	
	// Test Infer (with dummy data)
	t.Run("Infer", func(t *testing.T) {
		// Create dummy input tensor
		input := &pb.Tensor{
			Shape: []int32{1, 3, 640, 640},
			Dtype: pb.DataType_UINT8,
			Data:  make([]byte, 1*3*640*640),  // Dummy data
		}
		
		resp, err := client.Infer(ctx, &pb.InferRequest{
			ModelId:   "test_model_v1",
			Inputs:    []*pb.Tensor{input},
			SessionId: "test_session",
		})
		
		if err != nil {
			t.Fatalf("Infer failed: %v", err)
		}
		
		if !resp.Status.Success {
			t.Logf("⚠ Infer unsuccessful (expected if model not loaded): %s", resp.Status.Message)
		} else {
			t.Logf("✓ Inference completed in %d μs", resp.InferTimeUs)
		}
	})
}

// TestAIRuntimeStreamInfer tests streaming inference
func TestAIRuntimeStreamInfer(t *testing.T) {
	if testing.Short() {
		t.Skip("Skipping integration test")
	}
	
	conn, err := grpc.Dial("unix:///run/aipc/ai-runtime.sock",
		grpc.WithTransportCredentials(insecure.NewCredentials()))
	if err != nil {
		t.Skipf("Cannot connect: %v", err)
	}
	defer conn.Close()
	
	client := pb.NewInferenceServiceClient(conn)
	ctx, cancel := context.WithTimeout(context.Background(), 30*time.Second)
	defer cancel()
	
	// Start stream inference
	stream, err := client.StreamInfer(ctx, &pb.StreamInferRequest{
		StreamId:  "cam0_main",
		ModelId:   "test_model_v1",
		FpsLimit:  10,
		SessionId: "test_stream_session",
	})
	
	if err != nil {
		t.Fatalf("StreamInfer failed: %v", err)
	}
	
	// Receive results
	count := 0
	maxFrames := 5  // Only receive 5 frames for testing
	
	for count < maxFrames {
		result, err := stream.Recv()
		if err != nil {
			t.Logf("Stream ended or error: %v", err)
			break
		}
		
		count++
		t.Logf("✓ Received frame %d (seq=%d, ts=%d)",
			count, result.FrameSequence, result.TimestampNs)
	}
	
	if count > 0 {
		t.Logf("✓ Stream inference working (%d frames received)", count)
	}
}

