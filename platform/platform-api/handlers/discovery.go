package handlers

import (
	"context"
	"net/http"
	"time"

	"github.com/gin-gonic/gin"
	"google.golang.org/grpc"

	pb "aipc/platform/device-discovery/proto"
)

type DiscoveryHandler struct {
	client pb.DiscoveryServiceClient
}

func NewDiscoveryHandler(conn *grpc.ClientConn) *DiscoveryHandler {
	return &DiscoveryHandler{
		client: pb.NewDiscoveryServiceClient(conn),
	}
}

func (h *DiscoveryHandler) ListDevices(c *gin.Context) {
	ctx, cancel := context.WithTimeout(c.Request.Context(), 5*time.Second)
	defer cancel()

	product := c.Query("product")
	var status pb.DeviceStatus
	switch c.Query("status") {
	case "offline":
		status = pb.DeviceStatus_DEVICE_OFFLINE
	case "unreachable":
		status = pb.DeviceStatus_DEVICE_UNREACHABLE
	default:
		status = pb.DeviceStatus_DEVICE_ONLINE
	}

	resp, err := h.client.ListDevices(ctx, &pb.ListDevicesRequest{
		Product: product,
		Status:  status,
	})
	if err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"code": 502, "message": err.Error()})
		return
	}
	c.JSON(http.StatusOK, gin.H{"code": 0, "data": resp.Devices})
}

func (h *DiscoveryHandler) GetDevice(c *gin.Context) {
	ctx, cancel := context.WithTimeout(c.Request.Context(), 5*time.Second)
	defer cancel()

	sn := c.Param("sn")
	resp, err := h.client.GetDevice(ctx, &pb.GetDeviceRequest{SerialNumber: sn})
	if err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"code": 502, "message": err.Error()})
		return
	}
	if resp == nil {
		c.JSON(http.StatusNotFound, gin.H{"code": 404, "message": "device not found"})
		return
	}
	c.JSON(http.StatusOK, gin.H{"code": 0, "data": resp})
}

func (h *DiscoveryHandler) TriggerScan(c *gin.Context) {
	ctx, cancel := context.WithTimeout(c.Request.Context(), 30*time.Second)
	defer cancel()

	resp, err := h.client.TriggerScan(ctx, &pb.TriggerScanRequest{TimeoutSeconds: 10})
	if err != nil {
		c.JSON(http.StatusBadGateway, gin.H{"code": 502, "message": err.Error()})
		return
	}
	c.JSON(http.StatusOK, gin.H{"code": 0, "data": gin.H{"found_count": resp.FoundCount, "new_devices": resp.NewDevices}})
}
