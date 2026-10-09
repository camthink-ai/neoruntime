package handlers

import (
	"encoding/json"
	"fmt"
	"math"
	"slices"
	"strings"

	"aipc/platform/platform-api/model"
	"aipc/platform/postprocess"
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
// Single-sourced from the postprocess registry; the gRPC RegisterModel
// surface mirrors this closed set (plus the backend_function whitelist
// below) in the generated ai-runtime/include/postprocess_schema.h — extend
// the registry and regenerate, not this list.
var customVariantKeys = postprocess.DetectionVariantKeys

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
// detectionVariantValueTypes is the per-key value shape of the closed
// detection blob, derived from the decoder registry (the same params render
// into the generated C++ header, so both sides share one source). The key
// set alone does not close the loader-key vector: HAL substring-searches
// the raw JSON for backend_lib_path/backend_function
// (json_extract_string_best_effort), so a forbidden key hidden INSIDE a
// legal top-level value — e.g. labels as an object — would reach dlopen.
// (A forbidden key inside a JSON *string* value cannot match: quotes in
// string values are escaped as \", which breaks HAL's "key" search pattern;
// only structural nesting can smuggle it.) Shapes: string / number /
// integer (rejects fractions) / string_list (array of strings).
var detectionVariantValueTypes = func() map[string]string {
	shapes := map[string]string{}
	for _, d := range postprocess.Decoders {
		if d.ModelType != "detection" {
			continue
		}
		// All four detection decoders share the same seven-key shape
		// (hailoYoloParams); first hit wins, later ones cannot disagree
		// without failing the registry's own consistency tests.
		for _, p := range d.Params {
			shapes[p.Key] = p.Type
		}
		return shapes
	}
	return shapes
}()

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
	for _, key := range customVariantKeys {
		if err := validateVariantValueType(key, cfg[key], detectionVariantValueTypes[key]); err != nil {
			return err
		}
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

// validateVariantValueType checks one blob value against a shape from
// detectionVariantValueTypes: string / number (finite, optionally integral)
// / string_list (array of strings). A value of the wrong shape is not just
// malformed — through HAL's raw-substring reads it is the nested loader-key
// smuggling channel, so this belongs at every write boundary.
func validateVariantValueType(key string, value interface{}, want string) error {
	switch want {
	case "string":
		if _, ok := value.(string); !ok {
			return fmt.Errorf("variant key %q must be a string", key)
		}
	case "number", "integer":
		v, ok := value.(float64)
		if !ok {
			article := "a"
			if want == "integer" {
				article = "an"
			}
			return fmt.Errorf("variant key %q must be %s %s", key, article, want)
		}
		if want == "integer" && v != math.Trunc(v) {
			return fmt.Errorf("variant key %q must be an integer, got %v", key, v)
		}
	case "string_list":
		list, ok := value.([]interface{})
		if !ok {
			return fmt.Errorf("variant key %q must be an array of strings", key)
		}
		for _, item := range list {
			if _, ok := item.(string); !ok {
				return fmt.Errorf("variant key %q must contain only strings", key)
			}
		}
	case "boolean":
		if _, ok := value.(bool); !ok {
			return fmt.Errorf("variant key %q must be a boolean", key)
		}
	}
	return nil
}

// validateKeypointVariant guards the keypoint `{…}` variant escape hatch
// before it is stored. Unlike detection the keypoint dialect is deliberately
// OPEN: the blob travels opaquely to HAL, which owns content validation
// (proposal §6 compat matrix — legacy opaque channel), so unknown keys pass.
// The one hard refusal is the loader control keys the HAL postprocess layer
// dlopens from (backend_lib_path / backend_config_path): the keypoint surface
// had no such guard before, which was the same arbitrary-dlopen vector the
// detection closed set blocks. Bare decoder names are rejected with the same
// message shape the runtime's validator uses — pick a postprocess_profile
// instead and let the load-time composer build the blob.
func validateKeypointVariant(variant string) error {
	trimmed := strings.TrimSpace(variant)
	if trimmed == "" {
		return nil
	}
	if !strings.HasPrefix(trimmed, "{") {
		return fmt.Errorf("keypoint variant must be a JSON object (or empty to follow the postprocess_profile) — bare decoder names are rejected by the runtime")
	}
	var cfg map[string]interface{}
	if err := json.Unmarshal([]byte(trimmed), &cfg); err != nil {
		return fmt.Errorf("variant is not valid JSON: %w", err)
	}
	if key := findForbiddenVariantKey("", cfg); key != "" {
		return fmt.Errorf("variant key %q is never accepted (loader control key)", key)
	}
	return nil
}

// findForbiddenVariantKey walks a decoded variant blob — objects AND arrays,
// at any depth — looking for a loader control key (backend_lib_path /
// backend_config_path). The top-level-only check that used to live here was
// bypassable: HAL's json_extract_string_best_effort substring-searches the
// raw JSON, so {"extra":{"backend_lib_path":"/tmp/evil.so"}} reached dlopen
// despite passing every layer (review 2026-09-21 P0). path prefixes the
// reported location for diagnostics.
func findForbiddenVariantKey(path string, node interface{}) string {
	switch v := node.(type) {
	case map[string]interface{}:
		for _, key := range postprocess.ForbiddenVariantKeys {
			if _, ok := v[key]; ok {
				return joinVariantPath(path, key)
			}
		}
		for key, child := range v {
			if found := findForbiddenVariantKey(joinVariantPath(path, key), child); found != "" {
				return found
			}
		}
	case []interface{}:
		for i, child := range v {
			if found := findForbiddenVariantKey(fmt.Sprintf("%s[%d]", path, i), child); found != "" {
				return found
			}
		}
	}
	return ""
}

func joinVariantPath(path, key string) string {
	if path == "" {
		return key
	}
	return path + "." + key
}

// validateVariantJSON dispatches variant validation by model type: detection
// keeps its closed seven-key schema, keypoint gets the open dialect with the
// loader-key blacklist, and every other type runs the same any-depth
// loader-key refusal on `{`-prefixed blobs (review 2026-09-24 P0: the
// previous pass-through let backend_lib_path / backend_config_path through
// for types whose variants had no schema at this boundary). Bare names and
// empty variants still pass — other types have no decoder-selection
// semantics here.
func validateVariantJSON(modelType, variant string) error {
	switch model.ResolveModelType(modelType) {
	case "detection":
		return validateDetectionVariant(variant)
	case "keypoint":
		return validateKeypointVariant(variant)
	default:
		return validateLoaderKeysOnly(variant)
	}
}

// validateLoaderKeysOnly applies the any-depth loader-key refusal to a
// variant blob without imposing any other schema: empty and bare
// non-object names pass (no dialect to enforce), while a `{`-prefixed blob
// must parse as JSON (fail-closed, mirroring the keypoint surface — a
// malformed object is refused, not silently stored) and must not carry a
// loader control key at any depth.
func validateLoaderKeysOnly(variant string) error {
	trimmed := strings.TrimSpace(variant)
	if trimmed == "" || !strings.HasPrefix(trimmed, "{") {
		return nil
	}
	var cfg map[string]interface{}
	if err := json.Unmarshal([]byte(trimmed), &cfg); err != nil {
		return fmt.Errorf("variant is not valid JSON: %w", err)
	}
	if key := findForbiddenVariantKey("", cfg); key != "" {
		return fmt.Errorf("variant key %q is never accepted (loader control key)", key)
	}
	return nil
}

// validatePostprocessProfile guards the postprocess_profile config key at the
// write boundary, mirroring the load-time checks in
// modelload.DetectionPostprocessProfile / KeypointPostprocessProfile: a
// platform-mode detection row runs the vendor postprocess plugin (closed set
// of verified profile basenames) and a platform-mode keypoint row gets its
// blob composed per profile. A typo'd profile would otherwise be silently
// defaulted at load time — for a pose model that means the facial decoder
// with no diagnostic. Raw output mode never composes a profile (the consumer
// owns decoding) and other model types have no profile semantics; an absent
// key falls back to the type's default, mirroring legacy rows.
func validatePostprocessProfile(modelType, outputMode string, cfg map[string]interface{}) error {
	resolved := model.ResolveModelType(modelType)
	if resolved != "detection" && resolved != "keypoint" {
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
		return fmt.Errorf("postprocess_profile must be a string, got %T", raw)
	}
	if resolved == "detection" {
		if _, ok := model.LookupDetectionProfile(name); !ok {
			return fmt.Errorf("postprocess_profile %q is not supported (supported: %s)", name, supportedPostprocessBasenames())
		}
		return nil
	}
	if _, ok := model.LookupKeypointProfile(name); !ok {
		return fmt.Errorf("postprocess_profile %q is not supported for keypoint models (supported: %s)", name, supportedKeypointProfileValues())
	}
	return nil
}

// supportedKeypointProfileValues lists the keypoint decoder profiles for
// validation error messages, derived from the same table the load-time check
// consults.
func supportedKeypointProfileValues() string {
	names := make([]string, 0, len(model.KeypointPostprocessProfiles))
	for _, p := range model.KeypointPostprocessProfiles {
		names = append(names, p.Value)
	}
	return strings.Join(names, ", ")
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
