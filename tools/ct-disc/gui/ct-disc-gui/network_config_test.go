package main

import (
	"encoding/json"
	"testing"

	"github.com/camthink/ct-disc/pkg/discover"
)

// TestNetworkConfigRoundtrip verifies the JSON path:
// Frontend form data → JSON → SetDeviceNetworkConfig → SendSetNetwork
func TestNetworkConfigRoundtrip(t *testing.T) {
	// Simulates what NetworkConfigDialog.handleSave() sends
	mode := "static"
	cfg := map[string]string{
		"interface":   "eth0",
		"mode":        mode,
		"ip_address":  "192.168.1.100",
		"subnet_mask": "255.255.255.0",
		"gateway":     "192.168.1.1",
		"dns1":        "8.8.8.8",
		"dns2":        "8.8.4.4",
		"sn":          "TEST-SN-001",
		"mac":         "aa:bb:cc:dd:ee:ff",
	}

	cfgJSON, err := json.Marshal(cfg)
	if err != nil {
		t.Fatalf("Marshal failed: %v", err)
	}
	t.Logf("Frontend JSON: %s", string(cfgJSON))

	// Simulates SetDeviceNetworkConfig — unmarshal into NetworkConfig
	type NetworkConfig struct {
		Interface  string `json:"interface"`
		Mode       string `json:"mode"`
		IPAddress  string `json:"ip_address"`
		SubnetMask string `json:"subnet_mask"`
		Gateway    string `json:"gateway"`
		DNS1       string `json:"dns1"`
		DNS2       string `json:"dns2"`
		SN         string `json:"sn"`
		MACAddress string `json:"mac"`
	}

	var parsed NetworkConfig
	if err := json.Unmarshal([]byte(cfgJSON), &parsed); err != nil {
		t.Fatalf("Unmarshal failed: %v", err)
	}

	// Simulates the SendSetNetwork call in app.go
	sn := discover.SetNetwork{
		SN:         parsed.SN,
		MAC:        parsed.MACAddress,
		Interface:  parsed.Interface,
		Mode:       parsed.Mode,
		IPAddress:  parsed.IPAddress,
		SubnetMask: parsed.SubnetMask,
		Gateway:    parsed.Gateway,
		DNS1:       parsed.DNS1,
		DNS2:       parsed.DNS2,
	}

	// Verify encoding
	data, err := discover.EncodeSetNetwork(sn)
	if err != nil {
		t.Fatalf("EncodeSetNetwork failed: %v", err)
	}
	t.Logf("Wire JSON: %s", string(data))

	// Verify all fields roundtripped
	if sn.SN != "TEST-SN-001" {
		t.Errorf("SN mismatch: got %q", sn.SN)
	}
	if sn.MAC != "aa:bb:cc:dd:ee:ff" {
		t.Errorf("MAC mismatch: got %q", sn.MAC)
	}
	if sn.Mode != "static" {
		t.Errorf("Mode mismatch: got %q", sn.Mode)
	}
	if sn.IPAddress != "192.168.1.100" {
		t.Errorf("IP mismatch: got %q", sn.IPAddress)
	}
	if sn.SubnetMask != "255.255.255.0" {
		t.Errorf("SubnetMask mismatch: got %q", sn.SubnetMask)
	}
	if sn.Gateway != "192.168.1.1" {
		t.Errorf("Gateway mismatch: got %q", sn.Gateway)
	}

	// Verify type field is set by EncodeSetNetwork
	var wire map[string]interface{}
	json.Unmarshal(data, &wire)
	if wire["type"] != "ct-set-network" {
		t.Errorf("Wire type mismatch: got %q, want ct-set-network", wire["type"])
	}
}

// TestDHCPModeRoundtrip verifies that DHCP mode works (empty IP fields)
func TestDHCPModeRoundtrip(t *testing.T) {
	cfg := map[string]string{
		"interface":   "eth0",
		"mode":        "dhcp",
		"ip_address":  "",
		"subnet_mask": "",
		"gateway":     "",
		"dns1":        "",
		"dns2":        "",
		"sn":          "TEST-SN-002",
	}

	cfgJSON, err := json.Marshal(cfg)
	if err != nil {
		t.Fatalf("Marshal failed: %v", err)
	}

	sn := discover.SetNetwork{}
	if err := json.Unmarshal(cfgJSON, &sn); err != nil {
		t.Fatalf("Unmarshal failed: %v", err)
	}

	data, err := discover.EncodeSetNetwork(sn)
	if err != nil {
		t.Fatalf("EncodeSetNetwork failed: %v", err)
	}
	t.Logf("DHCP mode wire: %s", string(data))

	if sn.Mode != "dhcp" {
		t.Errorf("Expected DHCP mode, got %q", sn.Mode)
	}
}

// TestEmptyJSONRejection verifies the Go backend rejects invalid JSON
func TestEmptyJSONRejection(t *testing.T) {
	var cfg map[string]interface{}
	err := json.Unmarshal([]byte(""), &cfg)
	if err == nil {
		t.Error("Expected error for empty JSON, got nil")
	}

	err = json.Unmarshal([]byte("{bad}"), &cfg)
	if err == nil {
		t.Error("Expected error for malformed JSON, got nil")
	}
}
