package main

import (
	"os"
	"path/filepath"
	"testing"

	"aipc/platform/common/factoryeeprom"
	"aipc/platform/device-discovery/discovery"
)

func TestMatchesSetNetworkTargetMACIsAuthoritative(t *testing.T) {
	cfg := discovery.SetNetwork{
		SN:  "shared-cloned-serial",
		MAC: "02:00:00:00:00:01",
	}

	if matchesSetNetworkTarget(cfg, "shared-cloned-serial", "02:00:00:00:00:02") {
		t.Fatal("device with the same SN but a different MAC must not match")
	}
	if !matchesSetNetworkTarget(cfg, "different-serial", "02:00:00:00:00:01") {
		t.Fatal("matching MAC should select the target device")
	}
}

func TestMatchesSetNetworkTargetLegacySNFallback(t *testing.T) {
	cfg := discovery.SetNetwork{SN: "legacy-device"}

	if !matchesSetNetworkTarget(cfg, "legacy-device", "02:00:00:00:00:01") {
		t.Fatal("command without MAC should fall back to matching SN")
	}
	if matchesSetNetworkTarget(cfg, "other-device", "02:00:00:00:00:01") {
		t.Fatal("different SN must not match a legacy command")
	}
}

func TestMatchesSetNetworkTargetFailsClosed(t *testing.T) {
	tests := []struct {
		name  string
		cfg   discovery.SetNetwork
		mySN  string
		myMAC string
	}{
		{
			name:  "target MAC present but local MAC unavailable",
			cfg:   discovery.SetNetwork{SN: "same-sn", MAC: "02:00:00:00:00:01"},
			mySN:  "same-sn",
			myMAC: "",
		},
		{
			name:  "no target identity",
			cfg:   discovery.SetNetwork{},
			mySN:  "same-sn",
			myMAC: "02:00:00:00:00:01",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if matchesSetNetworkTarget(tt.cfg, tt.mySN, tt.myMAC) {
				t.Fatal("ambiguous target must not match")
			}
		})
	}
}

func TestResolveSNPrefersConfigThenFactoryThenVersion(t *testing.T) {
	script := writeDiscoveryFactoryScript(t, "CT2026-000812")
	t.Setenv(factoryeeprom.EnvToolPath, script)

	versionFile := filepath.Join(t.TempDir(), "VERSION")
	if err := os.WriteFile(versionFile, []byte("version=1.0.0\nserial=VERSION-SN\n"), 0644); err != nil {
		t.Fatal(err)
	}

	if got := resolveSN("CONFIG-SN", versionFile); got != "CONFIG-SN" {
		t.Fatalf("config SN = %q", got)
	}
	if got := resolveSN("", versionFile); got != "CT2026-000812" {
		t.Fatalf("factory SN = %q", got)
	}
}

func TestResolveSNFallsBackToVersionWhenFactoryUnavailable(t *testing.T) {
	t.Setenv(factoryeeprom.EnvToolPath, filepath.Join(t.TempDir(), "missing"))

	versionFile := filepath.Join(t.TempDir(), "VERSION")
	if err := os.WriteFile(versionFile, []byte("version=1.0.0\nserial=VERSION-SN\n"), 0644); err != nil {
		t.Fatal(err)
	}
	if got := resolveSN("", versionFile); got != "VERSION-SN" {
		t.Fatalf("version fallback SN = %q", got)
	}
}

func writeDiscoveryFactoryScript(t *testing.T, serial string) string {
	t.Helper()

	dir := t.TempDir()
	script := filepath.Join(dir, "factory-eeprom.sh")
	body := `#!/bin/sh
set -eu
if [ "${1:-}" = "-d" ]; then
  shift 2
fi
cmd="${1:-}"
field="${2:-}"
if [ "$cmd" = "get" ] && [ "$field" = "SN" ]; then
  printf '%s' "` + serial + `"
  exit 0
fi
exit 1
`
	if err := os.WriteFile(script, []byte(body), 0755); err != nil {
		t.Fatal(err)
	}
	return script
}
