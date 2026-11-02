// Package cellular is the Config Controller adapter for the cellular domain.
// It owns the user dial settings file (typically /data/aipc/etc/cellular.json)
// — APN/auth/usbnet/RAT preferences. Board topology stays in cellular-hw.yaml
// and is not projected here.
//
// SINGLE-KEY ("config"): desired value is the full JSON the handler already
// marshals (json.MarshalIndent + trailing newline). Render passes it through
// so Verify can byte-compare. Live effects (power, dialer, AT prefs) stay in
// the handler post-Apply, matching network/media/time.
package cellular

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"

	"aipc/platform/common/constants"
	"aipc/platform/platform-api/internal/atomicfile"
)

const keyConfig = "config"

var (
	ErrUnknownKey      = errors.New("cellular: unknown key")
	ErrInvalidJSON     = errors.New("cellular: desired is not valid JSON")
	ErrBadRenderedType = errors.New("cellular: rendered value is not JSON bytes")
	ErrBadBackupType   = errors.New("cellular: backup value is not file bytes")
)

// Adapter owns the cellular user-config JSON file.
type Adapter struct {
	configPath string
}

// New returns a cellular Adapter. configPath defaults to
// constants.ConfigPath()+"/cellular.json" when empty. Callers that resolve the
// board profile (paths.user_config) should pass that path so Register and the
// handler share one projection target — same pattern as device_info.
func New(configPath string) *Adapter {
	if configPath == "" {
		configPath = constants.ConfigPath() + "/cellular.json"
	}
	return &Adapter{configPath: configPath}
}

type backupState struct{ bytes []byte }

type rendered []byte

// dialConfig is the subset used for Validate (mirrors handlers.CellularConfig).
type dialConfig struct {
	Enabled        bool   `json:"enabled"`
	APN            string `json:"apn"`
	User           string `json:"user"`
	Password       string `json:"password"`
	Auth           string `json:"auth"`
	IPType         string `json:"ip_type"`
	PDPCID         int    `json:"pdp_cid"`
	CarrierProfile string `json:"carrier_profile"`
	Usbnet         string `json:"usbnet"`
	NetworkMode    string `json:"network_mode"`
	Roaming        *bool  `json:"roaming"`
	PIN            string `json:"pin"`
}

// Validate mirrors handlers.validateCellularConfig so Manager.Apply / Reconcile
// cannot persist enums the HTTP path would reject.
func (a *Adapter) Validate(_ context.Context, key, desiredJSON string) error {
	if key != keyConfig {
		return ErrUnknownKey
	}
	var cfg dialConfig
	if err := json.Unmarshal([]byte(desiredJSON), &cfg); err != nil {
		return fmt.Errorf("%w: %v", ErrInvalidJSON, err)
	}
	return validateDialConfig(&cfg)
}

func validateDialConfig(cfg *dialConfig) error {
	switch cfg.Auth {
	case "none", "pap", "chap":
	default:
		return fmt.Errorf("cellular: auth must be none, pap, or chap")
	}
	switch cfg.IPType {
	case "ipv4", "ipv6", "ipv4v6":
	default:
		return fmt.Errorf("cellular: ip_type must be ipv4, ipv6, or ipv4v6")
	}
	switch cfg.CarrierProfile {
	case "generic", "verizon":
	default:
		return fmt.Errorf("cellular: carrier_profile must be generic or verizon")
	}
	switch cfg.Usbnet {
	case "qmi", "ecm", "mbim":
	default:
		return fmt.Errorf("cellular: usbnet must be qmi, ecm, or mbim")
	}
	mode := strings.ToLower(strings.TrimSpace(cfg.NetworkMode))
	if mode == "" {
		mode = "auto"
	}
	switch mode {
	case "auto", "lte", "wcdma":
	default:
		return fmt.Errorf("cellular: network_mode must be auto, lte, or wcdma")
	}
	if cfg.PDPCID < 1 || cfg.PDPCID > 16 {
		return fmt.Errorf("cellular: pdp_cid must be between 1 and 16")
	}
	pin := strings.TrimSpace(cfg.PIN)
	if len(pin) > 8 {
		return fmt.Errorf("cellular: pin must be at most 8 digits")
	}
	for _, r := range pin {
		if r < '0' || r > '9' {
			return fmt.Errorf("cellular: pin must be numeric")
		}
	}
	return nil
}

func (a *Adapter) Backup(_ context.Context, key string) (any, error) {
	if key != keyConfig {
		return nil, ErrUnknownKey
	}
	b, err := os.ReadFile(a.configPath)
	if err != nil {
		if os.IsNotExist(err) {
			return backupState{nil}, nil
		}
		return nil, err
	}
	return backupState{b}, nil
}

func (a *Adapter) Render(_ context.Context, key, desiredJSON string) (any, error) {
	if key != keyConfig {
		return nil, ErrUnknownKey
	}
	return rendered([]byte(desiredJSON)), nil
}

func (a *Adapter) Apply(_ context.Context, key string, r any) error {
	if key != keyConfig {
		return ErrUnknownKey
	}
	rd, ok := r.(rendered)
	if !ok {
		return ErrBadRenderedType
	}
	if err := os.MkdirAll(filepath.Dir(a.configPath), 0755); err != nil {
		return err
	}
	return atomicfile.Write(a.configPath, []byte(rd), 0644)
}

func (a *Adapter) Verify(_ context.Context, key, desiredJSON string) error {
	if key != keyConfig {
		return ErrUnknownKey
	}
	got, err := os.ReadFile(a.configPath)
	if err != nil {
		return err
	}
	if string(got) != desiredJSON {
		return errors.New("cellular: file content does not match desired")
	}
	return nil
}

func (a *Adapter) Restore(_ context.Context, key string, backup any) error {
	if key != keyConfig {
		return ErrUnknownKey
	}
	bs, ok := backup.(backupState)
	if !ok {
		return ErrBadBackupType
	}
	if bs.bytes == nil {
		if err := os.Remove(a.configPath); err != nil && !os.IsNotExist(err) {
			return err
		}
		return nil
	}
	if err := os.MkdirAll(filepath.Dir(a.configPath), 0755); err != nil {
		return err
	}
	return atomicfile.Write(a.configPath, bs.bytes, 0644)
}

// Snapshot imports pre-existing live JSON into an empty desired-state store.
func (a *Adapter) Snapshot(_ context.Context, key string) (string, error) {
	if key != keyConfig {
		return "", ErrUnknownKey
	}
	b, err := os.ReadFile(a.configPath)
	if err != nil {
		return "", err
	}
	return string(b), nil
}

// ConfigPath returns the projected file path.
func (a *Adapter) ConfigPath() string { return a.configPath }
