package secrets

import (
	"os"

	"gopkg.in/yaml.v3"

	"aipc/platform/common/logger"
	"aipc/platform/platform-api/internal/atomicfile"
)

// MigratePlaintextPassword reads the platform-api config at configPath and, if
// its auth.password is a non-empty plaintext (not already a bcrypt hash),
// bcrypt-hashes the value and writes the file back atomically. It is the
// one-time upgrade that turns a legacy plaintext password on disk into a bcrypt
// hash so the static-storage threat (config dump, backup) no longer exposes it.
//
// It is idempotent (once auth.password starts with "$2" it is a no-op) and
// non-fatal: every failure path (unreadable file, malformed YAML, hash or write
// error) logs a warning and returns the original password value, never blocking
// boot — the in-memory config keeps the plaintext and VerifyPassword still
// matches it at runtime via the plaintext branch.
//
// The return value is the password the caller should keep in memory for this
// boot: the env override verbatim, the freshly-written hash, or the original
// value when nothing changed. envPassword, when non-empty, marks the operator's
// AIPC_AUTH_PASSWORD env var as authoritative for this boot (it is reapplied on
// every restart anyway, so hashing it to disk would be discarded); the yaml is
// left untouched and the env value is returned as-is.
func MigratePlaintextPassword(configPath, envPassword string) (finalPassword string, err error) {
	if envPassword != "" {
		return envPassword, nil
	}

	data, err := os.ReadFile(configPath)
	if err != nil {
		logger.Warn("secrets: migrate skipped, cannot read %s: %v", configPath, err)
		return "", err
	}

	var cfg map[string]interface{}
	if err := yaml.Unmarshal(data, &cfg); err != nil {
		logger.Warn("secrets: migrate skipped, cannot parse %s: %v", configPath, err)
		return "", err
	}

	authSection := stringMap(cfg["auth"])
	if authSection == nil {
		// No auth section to migrate; nothing to keep in memory either.
		return "", nil
	}

	current, _ := authSection["password"].(string)
	if current == "" || IsBcryptHash(current) {
		// Empty or already hashed: nothing to do, keep the value as-is.
		return current, nil
	}

	hashed, err := HashPassword(current)
	if err != nil {
		logger.Warn("secrets: migrate skipped, hash failed: %v", err)
		return current, nil
	}

	authSection["password"] = hashed
	cfg["auth"] = authSection // propagate rebuilt map (stringMap may have copied it)
	out, err := yaml.Marshal(cfg)
	if err != nil {
		logger.Warn("secrets: migrate skipped, marshal failed: %v", err)
		return current, nil
	}
	if err := atomicfile.Write(configPath, out, 0644); err != nil {
		logger.Warn("secrets: migrate skipped, write failed: %v", err)
		return current, nil
	}

	logger.Info("secrets: migrated auth.password to bcrypt hash in %s", configPath)
	return hashed, nil
}

// stringMap extracts a map[string]interface{} from v, tolerating the
// map[interface{}]interface{} shape some YAML parsers yield for nested maps.
// It returns nil when v is neither. When given the interface-keyed shape it
// rebuilds a string-keyed copy so callers can mutate freely and write it back.
func stringMap(v interface{}) map[string]interface{} {
	if m, ok := v.(map[string]interface{}); ok {
		return m
	}
	if inner, ok := v.(map[interface{}]interface{}); ok {
		out := make(map[string]interface{}, len(inner))
		for k, val := range inner {
			if ks, ok := k.(string); ok {
				out[ks] = val
			}
		}
		return out
	}
	return nil
}
