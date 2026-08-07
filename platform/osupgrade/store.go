package osupgrade

import (
	"encoding/json"
	"errors"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"time"
)

const DefaultRoot = "/data/aipc-os-upgrade"

type State string

const (
	StateIdle           State = "idle"
	StateUploading      State = "uploading"
	StateValidating     State = "validating"
	StateReady          State = "ready"
	StateInstalling     State = "installing"
	StateInstalled      State = "installed"
	StateAwaitingReboot State = "awaiting_reboot"
	StateRebooting      State = "rebooting"
	StateVerifying      State = "verifying"
	StateSuccess        State = "success"
	StateRollback       State = "rollback"
	StateFailed         State = "failed"
	StateCancelled      State = "cancelled"
)

type UpgradeStrategy string

const (
	UpgradeStrategyStandard UpgradeStrategy = "standard"
	UpgradeStrategyFull     UpgradeStrategy = "full"
)

type UpgradeStrategyOption struct {
	Strategy  UpgradeStrategy `json:"strategy"`
	Modes     []string        `json:"modes"`
	Supported bool            `json:"supported"`
	Reason    string          `json:"reason,omitempty"`
}

type UpdateModeOption struct {
	Mode          string `json:"mode"`
	Supported     bool   `json:"supported"`
	Default       bool   `json:"default,omitempty"`
	Reason        string `json:"reason,omitempty"`
	Warning       string `json:"warning,omitempty"`
	WarningCode   string `json:"warning_code,omitempty"`
	WarningCopy   string `json:"warning_copy,omitempty"`
	WarningTarget string `json:"warning_target,omitempty"`
}

type Job struct {
	ID                          string                  `json:"job_id"`
	State                       State                   `json:"status"`
	Progress                    int                     `json:"progress"`
	Message                     string                  `json:"message,omitempty"`
	Error                       string                  `json:"error,omitempty"`
	FileName                    string                  `json:"file_name,omitempty"`
	PackagePath                 string                  `json:"-"`
	Size                        int64                   `json:"file_size,omitempty"`
	SHA256                      string                  `json:"sha256,omitempty"`
	CurrentVersion              string                  `json:"current_version,omitempty"`
	TargetVersion               string                  `json:"target_version,omitempty"`
	BuildTime                   string                  `json:"build_time,omitempty"`
	Machine                     string                  `json:"machine,omitempty"`
	Product                     string                  `json:"product,omitempty"`
	HardwareVersion             string                  `json:"hardware_version,omitempty"`
	CurrentCopy                 string                  `json:"current_copy,omitempty"`
	TargetCopy                  string                  `json:"target_copy,omitempty"`
	PreviousCopy                string                  `json:"previous_copy,omitempty"`
	UpgradeMode                 string                  `json:"upgrade_mode,omitempty"`
	UpgradeStrategy             string                  `json:"upgrade_strategy,omitempty"`
	UpdateMode                  string                  `json:"update_mode,omitempty"`
	AvailableUpdateModes        []string                `json:"available_update_modes,omitempty"`
	AvailableUpdateModeOptions  []UpdateModeOption      `json:"available_update_mode_options,omitempty"`
	AvailableUpgradeStrategies  []UpgradeStrategyOption `json:"available_upgrade_strategies,omitempty"`
	SupportsStandardUpgrade     bool                    `json:"supports_standard_upgrade"`
	SupportsFullUpgrade         bool                    `json:"supports_full_upgrade"`
	RecoverySource              string                  `json:"recovery_source,omitempty"`
	RecoveryVersion             string                  `json:"recovery_version,omitempty"`
	SecureBootKeyID             string                  `json:"secure_boot_key_id,omitempty"`
	AppVersion                  string                  `json:"app_version,omitempty"`
	CompatLevel                 int                     `json:"compat_level,omitempty"`
	DataSchema                  int                     `json:"data_schema,omitempty"`
	CompatibilityValid          bool                    `json:"compatibility_valid"`
	RollbackSupported           bool                    `json:"rollback_supported"`
	ServiceInterruptionRequired bool                    `json:"service_interruption_required"`
	DowngradeAllowed            bool                    `json:"downgrade_allowed"`
	SignatureValid              bool                    `json:"signature_valid"`
	RebootRequired              bool                    `json:"reboot_required"`
	CreatedAt                   time.Time               `json:"created_at"`
	UpdatedAt                   time.Time               `json:"updated_at"`
}

func (j Job) Terminal() bool {
	return j.State == StateSuccess || j.State == StateFailed ||
		j.State == StateCancelled
}

func ParseUpgradeStrategy(value string) (UpgradeStrategy, error) {
	switch strings.ToLower(strings.TrimSpace(value)) {
	case "":
		return "", nil
	case string(UpgradeStrategyStandard):
		return UpgradeStrategyStandard, nil
	case string(UpgradeStrategyFull):
		return UpgradeStrategyFull, nil
	default:
		return "", fmt.Errorf("unknown OS upgrade strategy %q", value)
	}
}

func (j Job) EffectiveUpgradeStrategy() UpgradeStrategy {
	if strategy, err := ParseUpgradeStrategy(j.UpgradeStrategy); err == nil && strategy != "" {
		return strategy
	}
	if j.SupportsStandardUpgrade {
		return UpgradeStrategyStandard
	}
	return UpgradeStrategyFull
}

func (j Job) SupportsUpgradeStrategy(strategy UpgradeStrategy) bool {
	switch strategy {
	case UpgradeStrategyStandard:
		return j.SupportsStandardUpgrade
	case UpgradeStrategyFull:
		return j.SupportsFullUpgrade || (!j.SupportsStandardUpgrade && !j.SupportsFullUpgrade)
	default:
		return false
	}
}

func (j Job) CheckUpgradeStrategy(strategy UpgradeStrategy) error {
	switch strategy {
	case UpgradeStrategyStandard:
		if !j.SupportsUpgradeStrategy(strategy) {
			return fmt.Errorf("OS package or recovery does not support standard upgrade mode")
		}
	case UpgradeStrategyFull:
		if !j.SupportsUpgradeStrategy(strategy) {
			return fmt.Errorf("OS package or recovery does not support full upgrade mode")
		}
	default:
		return fmt.Errorf("unknown OS upgrade strategy %q", strategy)
	}
	return nil
}

func ParseUpdateMode(value string) (string, error) {
	mode := strings.TrimSpace(value)
	if mode == "" {
		return "", nil
	}
	for index := 0; index < len(mode); index++ {
		value := mode[index]
		if value >= 'a' && value <= 'z' ||
			value >= 'A' && value <= 'Z' ||
			value >= '0' && value <= '9' ||
			value == '_' || value == '-' || value == '.' {
			continue
		}
		return "", fmt.Errorf("invalid OS update mode %q", mode)
	}
	return mode, nil
}

func (j Job) CheckUpdateMode(targetCopy, mode string) error {
	mode, err := ParseUpdateMode(mode)
	if err != nil {
		return err
	}
	if mode == "" {
		return fmt.Errorf("OS update mode is empty")
	}
	if len(j.AvailableUpdateModeOptions) > 0 {
		for _, option := range j.AvailableUpdateModeOptions {
			if option.Mode != mode {
				continue
			}
			return nil
		}
		return fmt.Errorf("OS package does not define stable/%s", mode)
	}
	if !stringInSlice(j.AvailableUpdateModes, mode) {
		return fmt.Errorf("OS package does not define stable/%s", mode)
	}
	return nil
}

func (j Job) SWUpdateMode(targetCopy string) (string, error) {
	target := strings.ToLower(strings.TrimSpace(targetCopy))
	if target != "a" && target != "b" {
		return "", fmt.Errorf("invalid target copy %q", targetCopy)
	}
	if mode, err := ParseUpdateMode(j.UpdateMode); err != nil {
		return "", err
	} else if mode != "" {
		if err := j.CheckUpdateMode(target, mode); err != nil {
			return "", err
		}
		return mode, nil
	}
	strategy := j.EffectiveUpgradeStrategy()
	if err := j.CheckUpgradeStrategy(strategy); err != nil {
		return "", err
	}
	mode := "copy-" + target
	return mode, nil
}

func (j Job) SWUpdateSelection(targetCopy string) (string, error) {
	mode, err := j.SWUpdateMode(targetCopy)
	if err != nil {
		return "", err
	}
	return "stable," + mode, nil
}

func BuildUpgradeStrategyOptions(
	layoutMode string,
	result *ValidationResult,
	recovery *RecoveryBundle,
) []UpgradeStrategyOption {
	standardModes := []string{"copy-a", "copy-b"}
	fullModes := []string{"copy-a", "copy-b"}
	if layoutMode == LayoutSingle {
		standardModes = []string{"copy-a"}
		fullModes = []string{"copy-a"}
	}
	return []UpgradeStrategyOption{
		buildUpgradeStrategyOption(UpgradeStrategyStandard, standardModes, result, recovery, layoutMode),
		buildUpgradeStrategyOption(UpgradeStrategyFull, fullModes, result, recovery, layoutMode),
	}
}

func BuildUpdateModeOptions(
	layoutMode, targetCopy string,
	result *ValidationResult,
	recovery *RecoveryBundle,
) []UpdateModeOption {
	if result == nil {
		return nil
	}
	defaultMode := "copy-" + strings.ToLower(strings.TrimSpace(targetCopy))
	if layoutMode == LayoutSingle {
		defaultMode = "copy-a"
	}
	options := make([]UpdateModeOption, 0, len(result.UpdateModes))
	for _, mode := range result.UpdateModes {
		warning := updateModeWarning(layoutMode, mode, recovery)
		option := UpdateModeOption{
			Mode:          mode,
			Supported:     true,
			Default:       mode == defaultMode,
			WarningCode:   warning.Code,
			WarningCopy:   warning.Copy,
			WarningTarget: warning.Target,
		}
		options = append(options, option)
	}
	return options
}

type updateModeWarningInfo struct {
	Code   string
	Copy   string
	Target string
}

func updateModeWarning(layoutMode, mode string, recovery *RecoveryBundle) updateModeWarningInfo {
	switch {
	case strings.HasPrefix(mode, "init-partitions-"):
		return updateModeWarningInfo{Code: "init_partitions"}
	case mode == "init-scu-bl":
		return updateModeWarningInfo{Code: "boot_chain"}
	case mode == "copy-a" || mode == "copy-b":
		return updateModeWarningInfo{}
	case layoutMode == LayoutSingle && recovery != nil && !recovery.SupportsLocalUpdateMode(mode):
		return updateModeWarningInfo{Code: "recovery_unverified"}
	default:
		return updateModeWarningInfo{Code: "custom"}
	}
}

func UpgradeStrategyOptionSupported(options []UpgradeStrategyOption, strategy UpgradeStrategy) bool {
	for _, option := range options {
		if option.Strategy == strategy {
			return option.Supported
		}
	}
	return false
}

func buildUpgradeStrategyOption(
	strategy UpgradeStrategy,
	modes []string,
	result *ValidationResult,
	recovery *RecoveryBundle,
	layoutMode string,
) UpgradeStrategyOption {
	option := UpgradeStrategyOption{
		Strategy: strategy,
		Modes:    append([]string(nil), modes...),
	}
	if result == nil {
		option.Reason = "package validation result is unavailable"
		return option
	}
	if reason := missingUpdateModesReason(result.UpdateModes, modes); reason != "" {
		option.Reason = reason
		return option
	}
	if layoutMode == LayoutSingle {
		localMode := modes[0]
		if recovery == nil {
			option.Reason = "target package recovery is unavailable"
			return option
		}
		if !recovery.SupportsLocalUpdateMode(localMode) {
			option.Reason = fmt.Sprintf("target package recovery does not support local update mode %s", localMode)
			return option
		}
	}
	option.Supported = true
	return option
}

func missingUpdateModesReason(available []string, required []string) string {
	var missing []string
	for _, mode := range required {
		if !stringInSlice(available, mode) {
			missing = append(missing, "stable/"+mode)
		}
	}
	if len(missing) == 0 {
		return ""
	}
	if len(missing) == 1 {
		return "missing update mode " + missing[0]
	}
	return "missing update modes " + strings.Join(missing, ", ")
}

type Store struct {
	Root string
}

func NewStore(root string) *Store {
	if strings.TrimSpace(root) == "" {
		root = DefaultRoot
	}
	return &Store{Root: filepath.Clean(root)}
}

func (s *Store) Init() error {
	for _, dir := range []string{s.IncomingDir(), s.PackagesDir(), s.JobsDir()} {
		if err := os.MkdirAll(dir, 0750); err != nil {
			return err
		}
	}
	return nil
}

func (s *Store) IncomingDir() string { return filepath.Join(s.Root, "incoming") }
func (s *Store) PackagesDir() string { return filepath.Join(s.Root, "packages") }
func (s *Store) JobsDir() string     { return filepath.Join(s.Root, "jobs") }
func (s *Store) ActivePath() string  { return filepath.Join(s.Root, "active-job") }
func (s *Store) LockPath() string    { return filepath.Join(s.Root, "install.lock") }
func (s *Store) JobDir(id string) string {
	return filepath.Join(s.JobsDir(), id)
}
func (s *Store) StatusPath(id string) string {
	return filepath.Join(s.JobDir(id), "status.json")
}
func (s *Store) LogPath(id string) string {
	return filepath.Join(s.JobDir(id), "swupdate.log")
}
func (s *Store) RecoveryCandidateDir(id string) string {
	return filepath.Join(s.JobDir(id), "target-recovery")
}
func (s *Store) IncomingPath(id string) string {
	return filepath.Join(s.IncomingDir(), id+".part")
}
func (s *Store) PackagePath(id string) string {
	return filepath.Join(s.PackagesDir(), id+".swu")
}

func validateID(id string) error {
	if id == "" || strings.ContainsAny(id, `/\`) || id == "." || id == ".." {
		return errors.New("invalid job id")
	}
	return nil
}

func (s *Store) Save(job *Job) error {
	if err := validateID(job.ID); err != nil {
		return err
	}
	if err := s.Init(); err != nil {
		return err
	}
	if err := os.MkdirAll(s.JobDir(job.ID), 0750); err != nil {
		return err
	}
	job.UpdatedAt = time.Now().UTC()
	if job.CreatedAt.IsZero() {
		job.CreatedAt = job.UpdatedAt
	}
	if job.PackagePath == "" {
		job.PackagePath = s.PackagePath(job.ID)
	}
	data, err := json.MarshalIndent(job, "", "  ")
	if err != nil {
		return err
	}
	return atomicWrite(s.StatusPath(job.ID), data, 0640)
}

func (s *Store) Load(id string) (*Job, error) {
	if err := validateID(id); err != nil {
		return nil, err
	}
	data, err := os.ReadFile(s.StatusPath(id))
	if err != nil {
		return nil, err
	}
	var job Job
	if err := json.Unmarshal(data, &job); err != nil {
		return nil, err
	}
	job.PackagePath = s.PackagePath(job.ID)
	return &job, nil
}

func (s *Store) SetActive(id string) error {
	if err := validateID(id); err != nil {
		return err
	}
	return atomicWrite(s.ActivePath(), []byte(id+"\n"), 0640)
}

func (s *Store) Active() (*Job, error) {
	data, err := os.ReadFile(s.ActivePath())
	if err != nil {
		return nil, err
	}
	return s.Load(strings.TrimSpace(string(data)))
}

func (s *Store) RemovePackage(job *Job) error {
	if !job.Terminal() {
		return fmt.Errorf("cannot remove package while job is %s", job.State)
	}
	if err := os.Remove(s.PackagePath(job.ID)); err != nil && !os.IsNotExist(err) {
		return err
	}
	if data, err := os.ReadFile(s.ActivePath()); err == nil &&
		strings.TrimSpace(string(data)) == job.ID {
		if err := os.Remove(s.ActivePath()); err != nil && !os.IsNotExist(err) {
			return err
		}
	}
	return nil
}

func atomicWrite(path string, data []byte, mode os.FileMode) error {
	dir := filepath.Dir(path)
	if err := os.MkdirAll(dir, 0750); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(dir, ".tmp-*")
	if err != nil {
		return err
	}
	name := tmp.Name()
	defer os.Remove(name)
	if err := tmp.Chmod(mode); err != nil {
		tmp.Close()
		return err
	}
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if err := os.Rename(name, path); err != nil {
		return err
	}
	if d, err := os.Open(dir); err == nil {
		_ = d.Sync()
		_ = d.Close()
	}
	return nil
}
