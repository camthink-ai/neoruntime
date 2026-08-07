package discover

import (
	"reflect"
	"testing"
)

func TestRegistryListSortsDevicesByIPv4(t *testing.T) {
	registry := NewRegistry()
	for _, announce := range []Announce{
		{Product: "NE503", SN: "sn-10", MAC: "00:00:00:00:00:10", IP: "192.168.1.10"},
		{Product: "NE503", SN: "sn-bad", MAC: "00:00:00:00:00:ff", IP: "pending"},
		{Product: "NE503", SN: "sn-2", MAC: "00:00:00:00:00:02", IP: "192.168.1.2"},
		{Product: "NE503", SN: "sn-1", MAC: "00:00:00:00:00:01", IP: "10.0.0.1"},
	} {
		registry.Update(announce)
	}

	got := deviceIPs(registry.List())
	want := []string{"10.0.0.1", "192.168.1.2", "192.168.1.10", "pending"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("List() IP order = %v, want %v", got, want)
	}
}

func TestRegistryFilterKeepsIPv4SortOrder(t *testing.T) {
	registry := NewRegistry()
	for _, announce := range []Announce{
		{Product: "Other", SN: "sn-other", MAC: "00:00:00:00:00:99", IP: "192.168.1.1"},
		{Product: "NE503", SN: "sn-20", MAC: "00:00:00:00:00:20", IP: "192.168.1.20"},
		{Product: "NE503", SN: "sn-3", MAC: "00:00:00:00:00:03", IP: "192.168.1.3"},
	} {
		registry.Update(announce)
	}

	got := deviceIPs(registry.Filter("NE503", "", ""))
	want := []string{"192.168.1.3", "192.168.1.20"}
	if !reflect.DeepEqual(got, want) {
		t.Fatalf("Filter() IP order = %v, want %v", got, want)
	}
}

func deviceIPs(devices []*DeviceInfo) []string {
	ips := make([]string, len(devices))
	for i, device := range devices {
		ips[i] = device.Announce.IP
	}
	return ips
}
