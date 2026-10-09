package model

import (
	"fmt"
	"math"

	"aipc/platform/postprocess"
)

// ValidateModelConfig checks a request's config map against the type's
// field schema, then against the decoder registry's parameter table,
// BEFORE the row is persisted. It closes the "stored-but-never-applied"
// gap: until now an out-of-range threshold or a typo'd select value was
// stored verbatim and only silently reinterpreted at load time (omitted
// from the composed blob, or defaulted by the decoder) — the write
// boundary answered "accepted" for values the pipeline would never honor.
//
// Semantics:
//   - Keys with a field definition are validated for type, range,
//     integrality (step-1 number fields), and select membership. Select
//     membership is mode-independent: a typo'd postprocess_profile is
//     invalid stored data even in raw output mode, where it lies dormant
//     until the mode flips.
//   - Keys without a field definition but with a registry parameter
//     (e.g. keypoint yolov8_pose_network_width/height, which ride config
//     into the composed pose blob) are validated against the param's type
//     and range.
//   - Unknown keys pass: the config map is deliberately opaque
//     forward-compatible metadata (AMPK packages carry vendor keys the
//     platform does not consume).
//
// Empty maps and unknown model types are not this validator's refusal —
// callers already gate those separately.
func ValidateModelConfig(modelType string, cfg map[string]interface{}) error {
	if len(cfg) == 0 {
		return nil
	}
	resolved := ResolveModelType(modelType)
	if resolved == "" {
		return nil
	}
	td := GetModelTypeDef(resolved)
	if td == nil {
		return nil
	}
	fields := make(map[string]ModelFieldDef, len(td.Fields))
	for _, f := range td.Fields {
		fields[f.Key] = f
	}
	for key, value := range cfg {
		if f, ok := fields[key]; ok {
			if err := validateFieldValue(f, value); err != nil {
				return err
			}
			continue
		}
		if err := validateRegistryParam(resolved, key, value); err != nil {
			return err
		}
	}
	return nil
}

func validateFieldValue(f ModelFieldDef, value interface{}) error {
	switch f.Type {
	case FieldTypeNumber:
		v, ok := value.(float64)
		if !ok {
			return fmt.Errorf("config field %q must be a number", f.Key)
		}
		if f.Min != nil && v < *f.Min {
			return fmt.Errorf("config field %q must be >= %v, got %v", f.Key, *f.Min, v)
		}
		if f.Max != nil && v > *f.Max {
			return fmt.Errorf("config field %q must be <= %v, got %v", f.Key, *f.Max, v)
		}
		// A step-1 field is the schema's spelling of "integer knob".
		if f.Step != nil && *f.Step == 1 && v != math.Trunc(v) {
			return fmt.Errorf("config field %q must be an integer, got %v", f.Key, v)
		}
	case FieldTypeText:
		if _, ok := value.(string); !ok {
			return fmt.Errorf("config field %q must be a string", f.Key)
		}
	case FieldTypeSelect:
		s, ok := value.(string)
		if !ok {
			return fmt.Errorf("config field %q must be a string", f.Key)
		}
		for _, opt := range f.Options {
			if opt.Value == s {
				return nil
			}
		}
		return fmt.Errorf("config field %q has unsupported value %q", f.Key, s)
	case FieldTypeBoolean:
		if _, ok := value.(bool); !ok {
			return fmt.Errorf("config field %q must be a boolean", f.Key)
		}
	}
	return nil
}

// validateRegistryParam applies the decoder registry's parameter contract
// to a config key that has no field definition. Only keys the decoders
// actually read from config reach here (today: the keypoint pose network
// dimensions); everything else is unknown and passes.
func validateRegistryParam(resolvedType, key string, value interface{}) error {
	var param *postprocess.ParamDef
	for _, d := range postprocess.DecodersForType(resolvedType) {
		for i := range d.Params {
			if d.Params[i].Key == key {
				param = &d.Params[i]
				break
			}
		}
		if param != nil {
			break
		}
	}
	if param == nil {
		return nil // unknown key: opaque forward-compatible metadata
	}
	switch param.Type {
	case "number", "integer":
		v, ok := value.(float64)
		if !ok {
			article := "a"
			if param.Type == "integer" {
				article = "an"
			}
			return fmt.Errorf("config key %q must be %s %s", key, article, param.Type)
		}
		if param.Type == "integer" && v != math.Trunc(v) {
			return fmt.Errorf("config key %q must be an integer, got %v", key, v)
		}
		if param.Min != nil && v < *param.Min {
			return fmt.Errorf("config key %q must be >= %v, got %v", key, *param.Min, v)
		}
		if param.Max != nil && v > *param.Max {
			return fmt.Errorf("config key %q must be <= %v, got %v", key, *param.Max, v)
		}
	case "string":
		if _, ok := value.(string); !ok {
			return fmt.Errorf("config key %q must be a string", key)
		}
	case "string_list":
		list, ok := value.([]interface{})
		if !ok {
			return fmt.Errorf("config key %q must be an array of strings", key)
		}
		for _, item := range list {
			if _, ok := item.(string); !ok {
				return fmt.Errorf("config key %q must contain only strings", key)
			}
		}
	case "boolean":
		if _, ok := value.(bool); !ok {
			return fmt.Errorf("config key %q must be a boolean", key)
		}
	}
	return nil
}
