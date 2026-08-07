package factoryeeprom

import (
	"context"
	"os"
	"path/filepath"
	"testing"
)

func TestValidateValue(t *testing.T) {
	tests := []struct {
		name    string
		field   Field
		value   string
		want    string
		wantErr bool
	}{
		{name: "sn", field: FieldSN, value: " CT2026-000812 ", want: "CT2026-000812"},
		{name: "mac", field: FieldMAC, value: "00:11:22:33:44:55", want: "00:11:22:33:44:55"},
		{name: "batch length", field: FieldBatch, value: "20260724", want: "20260724"},
		{name: "batch too long", field: FieldBatch, value: "202607240", wantErr: true},
		{name: "unsafe chars", field: FieldSN, value: "SN 001", wantErr: true},
		{name: "bad mac", field: FieldMAC, value: "not-a-mac", wantErr: true},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := ValidateValue(tt.field, tt.value)
			if tt.wantErr {
				if err == nil {
					t.Fatal("expected error")
				}
				return
			}
			if err != nil {
				t.Fatalf("ValidateValue() error = %v", err)
			}
			if got != tt.want {
				t.Fatalf("ValidateValue() = %q, want %q", got, tt.want)
			}
		})
	}
}

func TestSetAndVerify(t *testing.T) {
	script := writeFakeFactoryScript(t)
	client := Client{ToolPath: script}

	info, err := client.SetAndVerify(context.Background(), "serial_number", "CT2026-000812")
	if err != nil {
		t.Fatalf("SetAndVerify() error = %v", err)
	}
	if info.SerialNumber != "CT2026-000812" {
		t.Fatalf("serial = %q", info.SerialNumber)
	}

	info, err = client.SetAndVerify(context.Background(), "MAC", "AA:BB:CC:DD:EE:FF")
	if err != nil {
		t.Fatalf("SetAndVerify(MAC) error = %v", err)
	}
	if !ValuesEqual(FieldMAC, "aa:bb:cc:dd:ee:ff", info.MACAddress) {
		t.Fatalf("mac = %q", info.MACAddress)
	}
}

func TestReadUnavailable(t *testing.T) {
	client := Client{ToolPath: filepath.Join(t.TempDir(), "missing")}
	info, err := client.Read(context.Background())
	if err == nil {
		t.Fatal("expected error")
	}
	if info.Available {
		t.Fatal("missing tool should not be available")
	}
}

func writeFakeFactoryScript(t *testing.T) string {
	t.Helper()

	dir := t.TempDir()
	stateDir := filepath.Join(dir, "state")
	if err := os.MkdirAll(stateDir, 0755); err != nil {
		t.Fatal(err)
	}
	script := filepath.Join(dir, "factory-eeprom.sh")
	body := `#!/bin/sh
set -eu
if [ "${1:-}" = "-d" ]; then
  shift 2
fi
cmd="${1:-}"
field="${2:-}"
value="${3:-}"
state="` + stateDir + `"
case "$cmd" in
  get)
    file="$state/$field"
    [ -f "$file" ] || exit 1
    cat "$file"
    ;;
  set)
    [ -n "$field" ] || exit 2
    printf '%s' "$value" > "$state/$field"
    ;;
  *)
    exit 2
    ;;
esac
`
	if err := os.WriteFile(script, []byte(body), 0755); err != nil {
		t.Fatal(err)
	}
	return script
}
