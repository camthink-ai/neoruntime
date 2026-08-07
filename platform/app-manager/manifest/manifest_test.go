package manifest

import (
	"os"
	"testing"
)

func TestExpandEnvRefs(t *testing.T) {
	tests := []struct {
		name  string
		input string
		setup func() // set env vars before test
		clean func() // unset env vars after test
		want  string
	}{
		{
			name:  "no_refs",
			input: "plain-value",
			want:  "plain-value",
		},
		{
			name:  "single_ref_resolved",
			input: "${HOME}",
			setup: func() { os.Setenv("HOME", "/root") },
			clean: func() { os.Unsetenv("HOME") },
			want:  "/root",
		},
		{
			name:  "single_ref_unresolved",
			input: "${AIPC_TOKEN_KEY}",
			setup: func() { os.Unsetenv("AIPC_TOKEN_KEY") },
			want:  "${AIPC_TOKEN_KEY}", // left intact
		},
		{
			name:  "ref_resolved_token",
			input: "${AIPC_TOKEN_KEY}",
			setup: func() { os.Setenv("AIPC_TOKEN_KEY", "aipc-secure-token-secret") },
			clean: func() { os.Unsetenv("AIPC_TOKEN_KEY") },
			want:  "aipc-secure-token-secret",
		},
		{
			name:  "mixed_literal_and_ref",
			input: "prefix-${HOSTNAME}-suffix",
			setup: func() { os.Setenv("HOSTNAME", "hailo15") },
			clean: func() { os.Unsetenv("HOSTNAME") },
			want:  "prefix-hailo15-suffix",
		},
		{
			name:  "multiple_refs",
			input: "${A}/${B}",
			setup: func() { os.Setenv("A", "1"); os.Setenv("B", "2") },
			clean: func() { os.Unsetenv("A"); os.Unsetenv("B") },
			want:  "1/2",
		},
		{
			name:  "empty_string",
			input: "",
			want:  "",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if tt.setup != nil {
				tt.setup()
			}
			defer func() {
				if tt.clean != nil {
					tt.clean()
				}
			}()
			got := ExpandEnvRefs(tt.input)
			if got != tt.want {
				t.Errorf("ExpandEnvRefs(%q) = %q, want %q", tt.input, got, tt.want)
			}
		})
	}
}

func TestToContainerEnvExpansion(t *testing.T) {
	os.Setenv("AIPC_TOKEN_KEY", "test-token-123")
	defer os.Unsetenv("AIPC_TOKEN_KEY")

	m := &AppManifest{
		Spec: Spec{
			Env: []EnvVar{
				{Name: "PLAIN", Value: "hello"},
				{Name: "TOKEN", Value: "${AIPC_TOKEN_KEY}"},
				{Name: "MISSING", Value: "${NONEXISTENT_VAR_XYZ}"},
			},
		},
	}

	env := m.ToContainerEnv()
	want := []string{
		"PLAIN=hello",
		"TOKEN=test-token-123",
		"MISSING=${NONEXISTENT_VAR_XYZ}",
	}
	if len(env) != len(want) {
		t.Fatalf("ToContainerEnv() len = %d, want %d", len(env), len(want))
	}
	for i, got := range env {
		if got != want[i] {
			t.Errorf("env[%d] = %q, want %q", i, got, want[i])
		}
	}
}
