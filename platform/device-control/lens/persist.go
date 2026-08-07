package lens

import (
	"encoding/json"
	"os"
	"path/filepath"

	"aipc/platform/common/constants"
	"aipc/platform/common/logger"
)

// lensConfigFile is the device-side side-file that survives camera-daemon /
// device-control restarts (deploy.sh only removes *.yaml under etc, json is
// untouched). Uses constants.ConfigPath() so --prefix / SetRootPath is honored.
func lensConfigFile() string {
	return filepath.Join(constants.ConfigPath(), "lens_config.json")
}

// persistedLensConfig is the on-disk shape. has_iris_target gates iris_target
// so a config that never set an iris target is distinguishable from one that
// set it to 0.
//
// Zoom/focus limits are NOT persisted here. They are AF0832 mechanical-range
// calibration constants sourced from device-control.yaml (default_zoom_limit /
// default_focus_limit) and the only live writers (ZoomLimitSet/FocusLimitSet)
// carry no runtime override path that should outlive a restart. Persisting
// them once — e.g. a one-off verification PUT of placeholder 0/8000 — would
// poison every subsequent boot: ReplayPersistedConfig would read the stale
// side-file, see it differ from the yaml-calibrated c.cfg, and call
// ZoomLimitSet to overwrite the real range with the stale defaults. That exact
// regression broke GetLensStatus readback on 93.72 (limits 0/8000 instead of
// -3236/760 → the UI (pos-min)/range formula clamped negative positions to 0,
// pinning zoom at 1.0x and focus at 0%). Only iris_target needs to survive a
// restart: yaml ships no default for it and the HAL exposes no getter, so a
// runtime IrisTargetSet would otherwise be lost across a restart.
type persistedLensConfig struct {
	IrisTarget    uint16 `json:"iris_target"`
	HasIrisTarget bool   `json:"has_iris_target"`
}

// persistLensConfig writes the iris-target side-file atomically (tmp + rename).
// A failure is logged but not returned as fatal: persistence is best-effort —
// the live HAL state is already updated by the caller.
func persistLensConfig(irisTarget uint16, hasIris bool) error {
	path := lensConfigFile()
	data, err := json.MarshalIndent(persistedLensConfig{
		IrisTarget:    irisTarget,
		HasIrisTarget: hasIris,
	}, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		return err
	}
	tmp := path + ".tmp"
	if err := os.WriteFile(tmp, data, 0644); err != nil {
		return err
	}
	if err := os.Rename(tmp, path); err != nil {
		_ = os.Remove(tmp)
		return err
	}
	return nil
}

// loadLensConfig reads the side-file. A missing file is normal (INFO, returns
// ok=false); a corrupt file is WARN and also returns ok=false so the caller
// falls back to defaults rather than applying garbage.
func loadLensConfig() (irisTarget uint16, hasIris bool, ok bool) {
	data, err := os.ReadFile(lensConfigFile())
	if err != nil {
		if !os.IsNotExist(err) {
			logger.Warn("lens config read failed: %v", err)
		}
		return 0, false, false
	}
	var p persistedLensConfig
	if err := json.Unmarshal(data, &p); err != nil {
		logger.Warn("lens config corrupt, ignoring: %v", err)
		return 0, false, false
	}
	return p.IrisTarget, p.HasIrisTarget, true
}

// persist is the best-effort writeback called from IrisTargetSet after a
// successful HAL round-trip. A failure is logged but never fatal: the live
// HAL state is already correct; only the restart-survival side-file is at stake.
func (c *LensClient) persist() {
	if err := persistLensConfig(c.irisTarget, c.hasIrisTarget); err != nil {
		logger.Warn("lens config persist failed: %v", err)
	}
}

// ReplayPersistedConfig loads the side-file and, if an iris target was
// persisted, re-pushes it to the HAL. Zoom/focus limits are never replayed:
// they stay at the yaml-calibrated constants held in c.cfg, so a stale or
// poison side-file cannot overwrite the real mechanical range on boot.
func (c *LensClient) ReplayPersistedConfig() {
	irisTarget, hasIris, ok := loadLensConfig()
	if !ok {
		return
	}
	if hasIris {
		if err := c.IrisTargetSet(irisTarget); err != nil {
			logger.Warn("replay iris target failed: %v", err)
		}
	}
}
