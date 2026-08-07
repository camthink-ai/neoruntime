package handler

import (
	"context"

	"aipc/platform/device-discovery/discovery"
	pb "aipc/platform/device-discovery/proto"
)

type Handler struct {
	pb.UnimplementedDiscoveryServiceServer
	registry *discovery.Registry
}

func NewHandler(registry *discovery.Registry) *Handler {
	return &Handler{registry: registry}
}

func (h *Handler) ListDevices(ctx context.Context, req *pb.ListDevicesRequest) (*pb.ListDevicesResponse, error) {
	devices := h.registry.List(req.Product, req.Status)
	return &pb.ListDevicesResponse{Devices: devices}, nil
}

func (h *Handler) GetDevice(ctx context.Context, req *pb.GetDeviceRequest) (*pb.DiscoveredDevice, error) {
	// Try MAC first (more reliable hardware identifier), then fallback to serial number
	if req.MacAddress != "" {
		if dev, ok := h.registry.GetByMAC(req.MacAddress); ok {
			return dev, nil
		}
	}
	if req.SerialNumber != "" {
		if dev, ok := h.registry.Get(req.SerialNumber); ok {
			return dev, nil
		}
	}
	return nil, nil
}

func (h *Handler) TriggerScan(ctx context.Context, req *pb.TriggerScanRequest) (*pb.TriggerScanResponse, error) {
	return &pb.TriggerScanResponse{FoundCount: 0}, nil
}

func (h *Handler) WatchDevices(req *pb.WatchDevicesRequest, stream pb.DiscoveryService_WatchDevicesServer) error {
	<-stream.Context().Done()
	return nil
}
