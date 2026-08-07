package containerd

import (
	"encoding/json"
	"os"
	"path/filepath"
	"testing"

	"github.com/opencontainers/runtime-spec/specs-go"
)

// TestLoadSeccompProfile tests loading a seccomp profile from file
func TestLoadSeccompProfile(t *testing.T) {
	// Create a temporary seccomp profile file
	tmpDir := t.TempDir()
	profilePath := filepath.Join(tmpDir, "test-seccomp.json")

	// Create a valid seccomp profile
	profile := specs.LinuxSeccomp{
		DefaultAction: specs.ActErrno,
		Architectures: []specs.Arch{specs.ArchX86_64, specs.ArchAARCH64},
		Syscalls: []specs.LinuxSyscall{
			{
				Names:  []string{"read", "write", "open", "close"},
				Action: specs.ActAllow,
			},
		},
	}

	profileData, err := json.Marshal(profile)
	if err != nil {
		t.Fatalf("Failed to marshal profile: %v", err)
	}

	if err := os.WriteFile(profilePath, profileData, 0644); err != nil {
		t.Fatalf("Failed to write profile file: %v", err)
	}

	// Create runtime instance
	client := &Client{} // Minimal client for testing
	runtime := NewRuntime(client, "/tmp/instances")

	// Test loading the profile
	opts, err := runtime.loadSeccompProfile(profilePath)
	if err != nil {
		t.Fatalf("Failed to load seccomp profile: %v", err)
	}

	if opts == nil {
		t.Fatal("loadSeccompProfile returned nil opts")
	}

	// Verify the profile can be applied to a spec
	// (This would require a full containerd client, so we'll just verify it doesn't error)
	t.Logf("Seccomp profile loaded successfully from: %s", profilePath)
}

// TestLoadSeccompProfileNotFound tests handling of missing profile file
func TestLoadSeccompProfileNotFound(t *testing.T) {
	client := &Client{}
	runtime := NewRuntime(client, "/tmp/instances")

	_, err := runtime.loadSeccompProfile("/nonexistent/path/seccomp.json")
	if err == nil {
		t.Fatal("Expected error for nonexistent profile file")
	}
	t.Logf("Correctly returned error for missing file: %v", err)
}

// TestValidateSeccompProfile tests seccomp profile validation
func TestValidateSeccompProfile(t *testing.T) {
	tests := []struct {
		name    string
		profile *specs.LinuxSeccomp
		wantErr bool
	}{
		{
			name: "valid profile",
			profile: &specs.LinuxSeccomp{
				DefaultAction: specs.ActErrno,
				Architectures: []specs.Arch{specs.ArchX86_64},
				Syscalls: []specs.LinuxSyscall{
					{
						Names:  []string{"read", "write"},
						Action: specs.ActAllow,
					},
				},
			},
			wantErr: false,
		},
		{
			name:    "nil profile",
			profile: nil,
			wantErr: true,
		},
		{
			name: "invalid default action",
			profile: &specs.LinuxSeccomp{
				DefaultAction: specs.LinuxSeccompAction("INVALID"),
			},
			wantErr: true,
		},
		{
			name: "invalid architecture",
			profile: &specs.LinuxSeccomp{
				DefaultAction: specs.ActErrno,
				Architectures: []specs.Arch{specs.Arch("INVALID")},
			},
			wantErr: true,
		},
		{
			name: "syscall without names or numbers",
			profile: &specs.LinuxSeccomp{
				DefaultAction: specs.ActErrno,
				Syscalls: []specs.LinuxSyscall{
					{
						Action: specs.ActAllow,
					},
				},
			},
			wantErr: true,
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := validateSeccompProfile(tt.profile)
			if (err != nil) != tt.wantErr {
				t.Errorf("validateSeccompProfile() error = %v, wantErr %v", err, tt.wantErr)
			}
		})
	}
}
