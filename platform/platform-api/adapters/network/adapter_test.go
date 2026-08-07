package network

import (
	"context"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// newAdapter builds an Adapter whose networkDir and interfacesPath both live
// under a temp dir, optionally seeding existing file content.
func newAdapter(t *testing.T, seedNetwork, seedInterfaces string) (*Adapter, string, string) {
	t.Helper()
	dir := t.TempDir()
	netDir := filepath.Join(dir, "net")
	if err := os.MkdirAll(netDir, 0755); err != nil {
		t.Fatalf("mkdir netdir: %v", err)
	}
	ifaces := filepath.Join(dir, "interfaces")
	if seedInterfaces != "" {
		if err := os.WriteFile(ifaces, []byte(seedInterfaces), 0644); err != nil {
			t.Fatalf("seed interfaces: %v", err)
		}
	}
	netPath := filepath.Join(netDir, "10-eth0.network")
	if seedNetwork != "" {
		if err := os.WriteFile(netPath, []byte(seedNetwork), 0644); err != nil {
			t.Fatalf("seed .network: %v", err)
		}
	}
	return New(netDir, ifaces), netPath, ifaces
}

func TestValidate(t *testing.T) {
	a, _, _ := newAdapter(t, "", "")
	ctx := context.Background()
	cases := []struct {
		name    string
		json    string
		wantErr error
	}{
		{"dhcp ok", `{"interface":"eth0","mode":"dhcp"}`, nil},
		{"static ok", `{"mode":"static","ip_address":"10.0.0.5","subnet_mask":"255.255.255.0"}`, nil},
		{"static with gw+dns", `{"mode":"static","ip_address":"10.0.0.5","subnet_mask":"255.255.255.0","gateway":"10.0.0.1","dns1":"8.8.8.8","dns2":"1.1.1.1"}`, nil},
		{"bad mode", `{"mode":"bridge"}`, ErrInvalidMode},
		{"static missing ip", `{"mode":"static","subnet_mask":"255.255.255.0"}`, ErrMissingField},
		{"static bad ip", `{"mode":"static","ip_address":"not-ip","subnet_mask":"255.255.255.0"}`, ErrBadIP},
		{"static bad mask", `{"mode":"static","ip_address":"10.0.0.5","subnet_mask":"nope"}`, ErrBadIP},
		{"static bad gw", `{"mode":"static","ip_address":"10.0.0.5","subnet_mask":"255.255.255.0","gateway":"x"}`, ErrBadIP},
		{"bad dns1", `{"mode":"dhcp","dns1":"x"}`, ErrBadIP},
		{"bad dns2", `{"mode":"dhcp","dns2":"x"}`, ErrBadIP},
		{"bad json", `{not-json}`, ErrInvalidJSON},
	}
	for _, tc := range cases {
		err := a.Validate(ctx, "eth0", tc.json)
		if tc.wantErr == nil {
			if err != nil {
				t.Errorf("%s: want nil, got %v", tc.name, err)
			}
			continue
		}
		if !errors.Is(err, tc.wantErr) {
			t.Errorf("%s: want %v, got %v", tc.name, tc.wantErr, err)
		}
	}
}

func TestBackup_PreservesBoth(t *testing.T) {
	a, _, _ := newAdapter(t, "[Match]\nName=eth0\n", "auto lo\n")
	bs, err := a.Backup(context.Background(), "eth0")
	if err != nil {
		t.Fatalf("backup: %v", err)
	}
	b := bs.(backupState)
	if string(b.networkBytes) != "[Match]\nName=eth0\n" {
		t.Fatalf("networkBytes = %q", string(b.networkBytes))
	}
	if string(b.interfacesBytes) != "auto lo\n" {
		t.Fatalf("interfacesBytes = %q", string(b.interfacesBytes))
	}
}

func TestBackup_MissingFilesAreNil(t *testing.T) {
	a, _, _ := newAdapter(t, "", "")
	bs, err := a.Backup(context.Background(), "eth0")
	if err != nil {
		t.Fatalf("backup: %v", err)
	}
	b := bs.(backupState)
	if b.networkBytes != nil || b.interfacesBytes != nil {
		t.Fatalf("want nil slices, got network=%q interfaces=%q", string(b.networkBytes), string(b.interfacesBytes))
	}
}

func TestRender_DHCP(t *testing.T) {
	a, _, _ := newAdapter(t, "", "")
	r, err := a.Render(context.Background(), "eth0", `{"mode":"dhcp"}`)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	rd := r.(rendered)
	if rd.iface != "eth0" {
		t.Fatalf("iface = %q", rd.iface)
	}
	net := string(rd.networkBytes)
	if !strings.Contains(net, "Name=eth0") || !strings.Contains(net, "DHCP=yes") {
		t.Fatalf("network bytes = %q", net)
	}
	iface := string(rd.interfacesBytes)
	if !strings.Contains(iface, "iface eth0 inet dhcp") {
		t.Fatalf("interfaces bytes = %q", iface)
	}
}

func TestRender_StaticFallsBackToKey(t *testing.T) {
	a, _, _ := newAdapter(t, "", "")
	// interface field empty in JSON → falls back to key "eth0".
	r, err := a.Render(context.Background(), "eth0", `{"mode":"static","ip_address":"10.0.0.5","subnet_mask":"255.255.255.0","gateway":"10.0.0.1","dns1":"8.8.8.8"}`)
	if err != nil {
		t.Fatalf("render: %v", err)
	}
	rd := r.(rendered)
	net := string(rd.networkBytes)
	if !strings.Contains(net, "Address=10.0.0.5/24") || !strings.Contains(net, "Gateway=10.0.0.1") || !strings.Contains(net, "DNS=8.8.8.8") {
		t.Fatalf("network bytes = %q", net)
	}
	iface := string(rd.interfacesBytes)
	if !strings.Contains(iface, "iface eth0 inet static") || !strings.Contains(iface, "address 10.0.0.5") || !strings.Contains(iface, "dns-nameservers 8.8.8.8") {
		t.Fatalf("interfaces bytes = %q", iface)
	}
}

func TestRender_BadJSON(t *testing.T) {
	a, _, _ := newAdapter(t, "", "")
	if _, err := a.Render(context.Background(), "eth0", `{bad}`); !errors.Is(err, ErrInvalidJSON) {
		t.Fatalf("want ErrInvalidJSON, got %v", err)
	}
}

func TestApply_WritesBothFiles(t *testing.T) {
	a, netPath, ifaces := newAdapter(t, "", "")
	r := rendered{
		networkBytes:    []byte("# net\n[Match]\nName=eth0\n[Network]\nDHCP=yes\n"),
		interfacesBytes: []byte("# ifaces\nauto lo\niface lo inet loopback\n"),
		iface:           "eth0",
	}
	if err := a.Apply(context.Background(), "eth0", r); err != nil {
		t.Fatalf("apply: %v", err)
	}
	if got, _ := os.ReadFile(netPath); string(got) != string(r.networkBytes) {
		t.Fatalf(".network = %q", string(got))
	}
	if got, _ := os.ReadFile(ifaces); string(got) != string(r.interfacesBytes) {
		t.Fatalf("interfaces = %q", string(got))
	}
}

func TestApply_BadRenderedType(t *testing.T) {
	a, _, _ := newAdapter(t, "", "")
	if err := a.Apply(context.Background(), "eth0", 123); err == nil {
		t.Fatal("apply with wrong type want error")
	}
}

func TestVerify_Match(t *testing.T) {
	a, netPath, _ := newAdapter(t, "", "")
	_ = os.WriteFile(netPath, []byte("# Network configuration managed by AIPC Platform\n[Match]\nName=eth0\n\n[Network]\nDHCP=yes\n"), 0644)
	desired := `{"interface":"eth0","mode":"dhcp"}`
	if err := a.Verify(context.Background(), "eth0", desired); err != nil {
		t.Fatalf("verify: %v", err)
	}
}

func TestVerify_Mismatch(t *testing.T) {
	a, netPath, _ := newAdapter(t, "", "")
	_ = os.WriteFile(netPath, []byte("totally different\n"), 0644)
	desired := `{"interface":"eth0","mode":"dhcp"}`
	if err := a.Verify(context.Background(), "eth0", desired); err == nil {
		t.Fatal("verify with mismatched content want error")
	}
}

func TestVerify_MissingFile(t *testing.T) {
	a, _, _ := newAdapter(t, "", "")
	if err := a.Verify(context.Background(), "eth0", `{"interface":"eth0","mode":"dhcp"}`); err == nil {
		t.Fatal("verify with missing file want error")
	}
}

func TestVerify_BadJSON(t *testing.T) {
	a, _, _ := newAdapter(t, "", "")
	if err := a.Verify(context.Background(), "eth0", `{bad}`); !errors.Is(err, ErrInvalidJSON) {
		t.Fatalf("want ErrInvalidJSON, got %v", err)
	}
}

func TestRestore_RevertsBothFiles(t *testing.T) {
	a, netPath, ifaces := newAdapter(t, "", "")
	// Apply wrote new content; restore must put the backup back.
	_ = os.WriteFile(netPath, []byte("NEW"), 0644)
	_ = os.WriteFile(ifaces, []byte("NEW"), 0644)
	bs := backupState{networkBytes: []byte("OLDNET"), interfacesBytes: []byte("OLDIFACES")}
	if err := a.Restore(context.Background(), "eth0", bs); err != nil {
		t.Fatalf("restore: %v", err)
	}
	if got, _ := os.ReadFile(netPath); string(got) != "OLDNET" {
		t.Fatalf(".network after restore = %q", string(got))
	}
	if got, _ := os.ReadFile(ifaces); string(got) != "OLDIFACES" {
		t.Fatalf("interfaces after restore = %q", string(got))
	}
}

func TestRestore_RemovesCreatedFiles(t *testing.T) {
	a, netPath, ifaces := newAdapter(t, "", "")
	// Apply created both files; backup had none → restore removes them.
	_ = os.WriteFile(netPath, []byte("NEW"), 0644)
	_ = os.WriteFile(ifaces, []byte("NEW"), 0644)
	bs := backupState{} // both nil
	if err := a.Restore(context.Background(), "eth0", bs); err != nil {
		t.Fatalf("restore: %v", err)
	}
	if _, err := os.Stat(netPath); !os.IsNotExist(err) {
		t.Fatalf("want .network removed, got %v", err)
	}
	if _, err := os.Stat(ifaces); !os.IsNotExist(err) {
		t.Fatalf("want interfaces removed, got %v", err)
	}
}

func TestRestore_BadBackupType(t *testing.T) {
	a, _, _ := newAdapter(t, "", "")
	if err := a.Restore(context.Background(), "eth0", "nope"); err == nil {
		t.Fatal("restore with bad backup type want error")
	}
}

func TestNewReturnsAdapter(t *testing.T) {
	if a := New("", ""); a == nil {
		t.Fatal("New returned nil")
	}
}

func TestMaskToPrefix(t *testing.T) {
	cases := []struct {
		mask string
		want int
	}{
		{"255.255.255.0", 24},
		{"255.255.0.0", 16},
		{"255.0.0.0", 8},
		{"255.255.255.255", 32},
		{"0.0.0.0", 0},
		{"not-a-mask", 0},
		{"", 0},
	}
	for _, tc := range cases {
		if got := maskToPrefix(tc.mask); got != tc.want {
			t.Errorf("maskToPrefix(%q) = %d, want %d", tc.mask, got, tc.want)
		}
	}
}
