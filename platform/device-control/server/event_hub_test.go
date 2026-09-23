package main

import (
	"testing"

	pb "aipc/platform/device-control/proto"
)

func TestLightChanged(t *testing.T) {
	tests := []struct {
		name string
		last int64
		now  int64
		want bool
	}{
		{name: "first sample", last: -1, now: 0, want: true},
		{name: "unchanged", last: 500, now: 500, want: false},
		{name: "below both thresholds", last: 500, now: 524, want: false},
		{name: "relative threshold", last: 500, now: 525, want: true},
		{name: "absolute threshold", last: 2000, now: 1950, want: true},
		{name: "zero baseline below absolute threshold", last: 0, now: 49, want: false},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if got := lightChanged(tt.last, tt.now); got != tt.want {
				t.Fatalf("lightChanged(%d, %d) = %v, want %v", tt.last, tt.now, got, tt.want)
			}
		})
	}
}

func TestPublishDeviceEventDoesNotBlockOnSlowSubscriber(t *testing.T) {
	slow := make(chan *pb.DeviceEvent, 1)
	fast := make(chan *pb.DeviceEvent, 1)
	queued := &pb.DeviceEvent{TimestampNs: 1}
	slow <- queued

	server := &DeviceControlServer{
		eventSubs: map[chan *pb.DeviceEvent]struct{}{
			slow: {},
			fast: {},
		},
	}
	event := &pb.DeviceEvent{
		Type:        pb.DeviceEvent_LIGHT_SENSOR_CHANGE,
		TimestampNs: 2,
		Data:        &pb.DeviceEvent_LightSensorValue{LightSensorValue: 123},
	}

	server.publishDeviceEvent(event)

	if got := <-fast; got != event {
		t.Fatalf("fast subscriber got %p, want %p", got, event)
	}
	if got := <-slow; got != queued {
		t.Fatalf("slow subscriber queue was overwritten: got %p, want %p", got, queued)
	}
	select {
	case got := <-slow:
		t.Fatalf("slow subscriber unexpectedly received dropped event: %+v", got)
	default:
	}
}
