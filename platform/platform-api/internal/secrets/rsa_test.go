package secrets

import (
	"crypto/rand"
	"crypto/rsa"
	"encoding/base64"
	"testing"
)

func TestDecryptPassword_Roundtrip(t *testing.T) {
	priv, err := rsa.GenerateKey(rand.Reader, 2048)
	if err != nil {
		t.Fatalf("GenerateKey: %v", err)
	}
	plain := "hunter2blue"
	ct, err := rsa.EncryptPKCS1v15(rand.Reader, &priv.PublicKey, []byte(plain))
	if err != nil {
		t.Fatalf("EncryptPKCS1v15: %v", err)
	}
	encoded := base64.StdEncoding.EncodeToString(ct)
	if got := DecryptPassword(encoded, priv); got != plain {
		t.Errorf("DecryptPassword = %q, want %q", got, plain)
	}
}

func TestDecryptPassword_Fallback(t *testing.T) {
	priv, _ := rsa.GenerateKey(rand.Reader, 2048)
	cases := []string{
		"not-base64!!!", // invalid base64
		base64.StdEncoding.EncodeToString([]byte("too-short-not-rsa")), // valid base64, wrong ciphertext length
		"plaintext-admin", // a literal plaintext password (no encryption attempted)
	}
	for _, in := range cases {
		if got := DecryptPassword(in, priv); got != in {
			t.Errorf("DecryptPassword(%q) = %q, want original (fallback)", in, got)
		}
	}
	// nil key -> always passthrough, regardless of input.
	if got := DecryptPassword("anything", nil); got != "anything" {
		t.Errorf("DecryptPassword with nil key = %q, want passthrough", got)
	}
}
