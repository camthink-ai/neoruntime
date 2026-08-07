package secrets

import "testing"

func TestIsBcryptHash(t *testing.T) {
	cases := []struct {
		in   string
		want bool
	}{
		{"$2a$10$abcdefghijklmnopqrstuv12345678901234567890123456789012345678", true},
		{"$2b$12$abcdefghijklmnopqrstuv12345678901234567890123456789012345678", true},
		{"$2y$10$abcdefghijklmnopqrstuv12345678901234567890123456789012345678", true},
		{"admin", false},
		{"", false},
		{"$2$short", false},
		{"$2a$short", false}, // plausible prefix but too short to be a real hash
	}
	for _, c := range cases {
		if got := IsBcryptHash(c.in); got != c.want {
			t.Errorf("IsBcryptHash(%q) = %v, want %v", c.in, got, c.want)
		}
	}
}

func TestHashVerifyRoundtrip(t *testing.T) {
	plain := "hunter2blue"
	hashed, err := HashPassword(plain)
	if err != nil {
		t.Fatalf("HashPassword: %v", err)
	}
	if !IsBcryptHash(hashed) {
		t.Errorf("hashed value is not a bcrypt hash: %q", hashed)
	}
	if !VerifyPassword(plain, hashed) {
		t.Errorf("VerifyPassword(plain, hashed) = false, want true")
	}
	if VerifyPassword("wrong", hashed) {
		t.Errorf("VerifyPassword(wrong, hashed) = true, want false")
	}
}

func TestVerifyPassword_PlaintextFallback(t *testing.T) {
	if !VerifyPassword("admin", "admin") {
		t.Error(`VerifyPassword("admin","admin") = false, want true`)
	}
	if VerifyPassword("admin", "Admin") {
		t.Error(`VerifyPassword("admin","Admin") = true, want false`)
	}
	if VerifyPassword("admin", "") {
		t.Error(`VerifyPassword("admin","") = true, want false`)
	}
}
