package secrets

import (
	"os"
	"path/filepath"
	"testing"

	"gopkg.in/yaml.v3"
)

func writeTestConfig(t *testing.T, dir, body string) string {
	t.Helper()
	p := filepath.Join(dir, "platform-api.yaml")
	if err := os.WriteFile(p, []byte(body), 0644); err != nil {
		t.Fatalf("write config: %v", err)
	}
	return p
}

func TestMigrate_PlaintextToHash(t *testing.T) {
	dir := t.TempDir()
	p := writeTestConfig(t, dir, "auth:\n  username: admin\n  password: hunter2blue\n")

	final, err := MigratePlaintextPassword(p, "")
	if err != nil {
		t.Fatalf("Migrate: %v", err)
	}
	if !IsBcryptHash(final) {
		t.Errorf("final = %q, want bcrypt hash", final)
	}
	if !VerifyPassword("hunter2blue", final) {
		t.Error("final hash does not verify against original plaintext")
	}

	// The file on disk must now hold the hash too.
	data, _ := os.ReadFile(p)
	var cfg map[string]interface{}
	if err := yaml.Unmarshal(data, &cfg); err != nil {
		t.Fatalf("reparse: %v", err)
	}
	auth := cfg["auth"].(map[string]interface{})
	if got, _ := auth["password"].(string); !IsBcryptHash(got) {
		t.Errorf("on-disk password = %q, want bcrypt hash", got)
	}
}

func TestMigrate_Idempotent(t *testing.T) {
	dir := t.TempDir()
	p := writeTestConfig(t, dir, "auth:\n  password: hunter2blue\n")

	if _, err := MigratePlaintextPassword(p, ""); err != nil {
		t.Fatalf("first migrate: %v", err)
	}
	first, _ := os.ReadFile(p)

	if _, err := MigratePlaintextPassword(p, ""); err != nil {
		t.Fatalf("second migrate: %v", err)
	}
	second, _ := os.ReadFile(p)

	if string(first) != string(second) {
		t.Error("second migrate run changed the file (not idempotent)")
	}
}

func TestMigrate_SkipWhenEnvSet(t *testing.T) {
	dir := t.TempDir()
	original := "auth:\n  password: hunter2blue\n"
	p := writeTestConfig(t, dir, original)

	final, err := MigratePlaintextPassword(p, "envpass123")
	if err != nil {
		t.Fatalf("Migrate: %v", err)
	}
	if final != "envpass123" {
		t.Errorf("final = %q, want env value", final)
	}
	if got, _ := os.ReadFile(p); string(got) != original {
		t.Error("env-set migrate mutated the yaml (should be untouched)")
	}
}

func TestMigrate_SkipWhenAlreadyHashed(t *testing.T) {
	dir := t.TempDir()
	hashed, _ := HashPassword("already-hashed")
	original := "auth:\n  password: " + hashed + "\n"
	p := writeTestConfig(t, dir, original)

	final, err := MigratePlaintextPassword(p, "")
	if err != nil {
		t.Fatalf("Migrate: %v", err)
	}
	if final != hashed {
		t.Errorf("final = %q, want unchanged hash", final)
	}
	if got, _ := os.ReadFile(p); string(got) != original {
		t.Error("already-hashed migrate mutated the yaml")
	}
}

func TestMigrate_FailureNonBlocking(t *testing.T) {
	// Missing file: returns an error + empty final, does not panic.
	final, err := MigratePlaintextPassword(filepath.Join(t.TempDir(), "nope.yaml"), "")
	if err == nil {
		t.Error("missing file: expected error, got nil")
	}
	if final != "" {
		t.Errorf("final = %q, want empty on failure", final)
	}

	// Malformed yaml: "a: b: c" is a classic yaml syntax error.
	dir := t.TempDir()
	p := writeTestConfig(t, dir, "a: b: c\n")
	final2, err2 := MigratePlaintextPassword(p, "")
	if err2 == nil {
		t.Error("malformed yaml: expected error, got nil")
	}
	if final2 != "" {
		t.Errorf("final = %q, want empty on parse failure", final2)
	}
}

func TestMigrate_NoAuthSection(t *testing.T) {
	dir := t.TempDir()
	p := writeTestConfig(t, dir, "service:\n  port: 8080\n")
	final, err := MigratePlaintextPassword(p, "")
	if err != nil {
		t.Fatalf("Migrate: %v", err)
	}
	if final != "" {
		t.Errorf("final = %q, want empty when no auth section", final)
	}
}
