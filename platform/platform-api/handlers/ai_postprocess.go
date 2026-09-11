package handlers

import (
	"encoding/json"
	"fmt"
	"slices"
	"strings"

	"aipc/platform/platform-api/model"
)

// Detection-model postprocess registration guards.
//
// The load-time half of this plumbing — materializing detection models under
// a plugin-recognized basename and composing the variant JSON — lives in
// platform/modelload, shared by platform-api's LoadModel and app-manager's
// PreloadModels so both register models identically. What stays here are the
// checks applied when a model row is written, before anything loads.

// customVariantKeys are the keys the vendor plugin's JSON schema requires —
// a partial blob is rejected at load time with a bare "required" error, so
// user-supplied variant JSON must carry all of them up front. They are also
// the complete set the REST surface accepts: the schema is closed.
var customVariantKeys = []string{
	"backend_function",
	"iou_threshold",
	"detection_threshold",
	"output_activation",
	"label_offset",
	"max_boxes",
	"labels",
}

// validateDetectionVariant guards the advanced `{…}` variant escape hatch
// before it is stored: every schema key must be present, no other key may
// appear, and backend_function must name one of the verified postprocess
// functions (the generic single-argument ones hardcode a 0.4 threshold and
// COCO labels, so a model pointed at them would silently detect with the
// wrong settings). The closed key set is what aligns this REST path with the
// AMPK package boundary: the HAL postprocess layer reads backend_lib_path /
// backend_config_path from this same config JSON and dlopens whatever they
// name, and a `{...}` variant travels to the runtime verbatim — an open
// schema here would reopen the arbitrary-dlopen vector the AMPK importer
// explicitly refuses (storage/modelpackage.go). Variants that are not JSON
// objects — plugin basenames, keypoint names, empty — are left to their
// existing handling and pass unchanged.
func validateDetectionVariant(variant string) error {
	trimmed := strings.TrimSpace(variant)
	if !strings.HasPrefix(trimmed, "{") {
		return nil
	}
	var cfg map[string]interface{}
	if err := json.Unmarshal([]byte(trimmed), &cfg); err != nil {
		return fmt.Errorf("variant is not valid JSON: %w", err)
	}
	var missing []string
	for _, key := range customVariantKeys {
		if _, ok := cfg[key]; !ok {
			missing = append(missing, key)
		}
	}
	if len(missing) > 0 {
		return fmt.Errorf("variant JSON is missing required key(s): %s — the postprocess plugin requires all of: %s",
			strings.Join(missing, ", "), strings.Join(customVariantKeys, ", "))
	}
	var unknown []string
	for key := range cfg {
		if !slices.Contains(customVariantKeys, key) {
			unknown = append(unknown, key)
		}
	}
	if len(unknown) > 0 {
		slices.Sort(unknown)
		return fmt.Errorf("variant JSON contains unsupported key(s): %s — allowed keys are exactly: %s (loader keys such as backend_lib_path/backend_config_path are never accepted here)",
			strings.Join(unknown, ", "), strings.Join(customVariantKeys, ", "))
	}
	fn, _ := cfg["backend_function"].(string)
	if _, ok := model.LookupDetectionBackendFunction(fn); !ok {
		fns := make([]string, 0, len(model.DetectionPostprocessProfiles))
		for _, p := range model.DetectionPostprocessProfiles {
			fns = append(fns, p.BackendFunction)
		}
		return fmt.Errorf("variant backend_function %q is not supported — supported functions: %s",
			fn, strings.Join(fns, ", "))
	}
	return nil
}

// validatePostprocessProfile guards the postprocess_profile config key at the
// write boundary, mirroring the load-time check in
// modelload.DetectionPostprocessProfile: a detection row in platform mode runs
// the vendor postprocess plugin, which selects its function from a closed set
// of verified profile basenames. A typo'd profile would otherwise be silently
// defaulted at load time and mismatch the model to the wrong materialization
// basename and backend function with no diagnostic, so a present-but-unknown
// value is rejected here while the row is still being written. Raw output mode
// never composes a profile (the consumer owns decoding) and non-detection
// models have no profile semantics; an absent key falls back to the default
// profile, mirroring legacy rows.
func validatePostprocessProfile(modelType, outputMode string, cfg map[string]interface{}) error {
	if model.ResolveModelType(modelType) != "detection" {
		return nil
	}
	if mode, ok := model.ResolveOutputMode(outputMode); !ok || mode != model.OutputModePlatform {
		return nil
	}
	raw, ok := cfg["postprocess_profile"]
	if !ok {
		return nil
	}
	name, isStr := raw.(string)
	if !isStr {
		return fmt.Errorf("postprocess_profile must be a string, got %T (supported: %s)", raw, supportedPostprocessBasenames())
	}
	if _, ok := model.LookupDetectionProfile(name); !ok {
		return fmt.Errorf("postprocess_profile %q is not supported (supported: %s)", name, supportedPostprocessBasenames())
	}
	return nil
}

// supportedPostprocessBasenames lists the verified plugin profile basenames
// for validation error messages, derived from the same table the load-time
// check consults.
func supportedPostprocessBasenames() string {
	names := make([]string, 0, len(model.DetectionPostprocessProfiles))
	for _, p := range model.DetectionPostprocessProfiles {
		names = append(names, p.Basename)
	}
	return strings.Join(names, ", ")
}

// validateOutputModeForModel is the server-side guard behind the wizard's
// disabled radio card: the platform postprocess path only decodes NMS-layer
// HEFs, so a feature-map HEF registered as platform+detection would load and
// then fail postprocess on every frame. Raw mode is always fine — the
// consumer owns decoding. Empty vstream info (legacy rows, model_path
// registrations without a prior parse) skips the cross-check rather than
// failing closed on metadata the request never carried.
func validateOutputModeForModel(outputMode, modelType, vstreamInfo string) error {
	if outputMode != model.OutputModePlatform {
		return nil
	}
	if model.ResolveModelType(modelType) != "detection" {
		return nil
	}
	if model.ClassifyOutputFormat(vstreamInfo) != model.OutputFormatFeatureMap {
		return nil
	}
	return fmt.Errorf("this HEF outputs raw feature maps (no NMS layer compiled in), so the platform postprocess cannot decode it — choose raw output mode")
}
