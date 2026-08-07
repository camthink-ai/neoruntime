// Package secrets holds the password-related crypto primitives shared by the
// login and change-password request paths: bcrypt hashing/verification, RSA
// decryption of client-encrypted passwords, timestamp-based replay protection,
// and the one-time plaintext→bcrypt migration of the auth config at boot.
package secrets

import (
	"crypto/subtle"
	"strings"

	"golang.org/x/crypto/bcrypt"
)

// bcryptCost is the cost factor used when hashing new passwords. Cost 10 keeps
// a single hash ~60ms on typical edge hardware (well under the login latency
// budget) while remaining expensive enough to deter offline brute force.
const bcryptCost = 10

// minBcryptHashLen is the shortest plausible bcrypt hash string; IsBcryptHash
// rejects anything shorter so truncated values are never mistaken for hashes.
const minBcryptHashLen = 50

// IsBcryptHash reports whether s looks like a bcrypt hash ($2a/$2b/$2y prefix).
// It is the idempotency gate for the startup plaintext→hash migration and the
// branch selector inside VerifyPassword: a stored value that is already hashed
// is never re-hashed, and is compared via bcrypt rather than constant-time
// string equality.
func IsBcryptHash(s string) bool {
	if len(s) < minBcryptHashLen {
		return false
	}
	return strings.HasPrefix(s, "$2a$") || strings.HasPrefix(s, "$2b$") || strings.HasPrefix(s, "$2y$")
}

// HashPassword returns a bcrypt hash of plain at the package cost.
func HashPassword(plain string) (string, error) {
	b, err := bcrypt.GenerateFromPassword([]byte(plain), bcryptCost)
	if err != nil {
		return "", err
	}
	return string(b), nil
}

// VerifyPassword normalizes the stored value and verifies plain against it.
//   - stored is a bcrypt hash  -> bcrypt.CompareHashAndPassword
//   - stored is plaintext/empty -> constant-time string equality (a legacy yaml
//     value, or an operator-set AIPC_AUTH_PASSWORD env override)
//
// A hash mismatch (or any bcrypt error) is normalized to false rather than
// returned as an error, so callers can write a plain `if !VerifyPassword(...)`.
// The plaintext branch uses subtle.ConstantTimeCompare to avoid a timing oracle
// on legacy values.
func VerifyPassword(plain, stored string) bool {
	if IsBcryptHash(stored) {
		return bcrypt.CompareHashAndPassword([]byte(stored), []byte(plain)) == nil
	}
	return subtle.ConstantTimeCompare([]byte(plain), []byte(stored)) == 1
}
