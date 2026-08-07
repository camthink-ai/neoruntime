package model

import "strings"

// FieldType defines the UI input type for a model configuration field.
type FieldType string

const (
	FieldTypeNumber  FieldType = "number"
	FieldTypeText    FieldType = "text"
	FieldTypeSelect  FieldType = "select"
	FieldTypeBoolean FieldType = "boolean"
)

// FieldOption defines a selectable option for FieldTypeSelect fields.
type FieldOption struct {
	Value string `json:"value"`
	Label string `json:"label"`
}

// ModelFieldDef describes a single configuration field for a model type.
// The frontend renders these dynamically — no hardcoded form fields needed.
type ModelFieldDef struct {
	Key      string        `json:"key"`
	Type     FieldType     `json:"type"`
	Required bool          `json:"required"`
	Default  interface{}   `json:"default"`
	Min      *float64      `json:"min,omitempty"`
	Max      *float64      `json:"max,omitempty"`
	Step     *float64      `json:"step,omitempty"`
	Options  []FieldOption `json:"options,omitempty"`
}

// ModelTypeDef describes a supported model postprocess type.
// Mirrors HAL enum HalPostprocessType in hal_v2/include/model/hal_postprocess.h.
type ModelTypeDef struct {
	ID      string          `json:"id"`
	Label   string          `json:"label"`
	Fields  []ModelFieldDef `json:"fields"`
	Aliases []string        `json:"aliases,omitempty"`
}

// FileFormat describes a supported model file format.
type FileFormat struct {
	Extension string `json:"extension"`
	MIMEType  string `json:"mime_type"`
	Label     string `json:"label"`
}

// Helper constructors for common field types
func numField(key string, def, min, max, step float64) ModelFieldDef {
	return ModelFieldDef{
		Key: key, Type: FieldTypeNumber, Required: false,
		Default: def, Min: &min, Max: &max, Step: &step,
	}
}

func reqNumField(key string, def, min, max, step float64) ModelFieldDef {
	return ModelFieldDef{
		Key: key, Type: FieldTypeNumber, Required: true,
		Default: def, Min: &min, Max: &max, Step: &step,
	}
}

func boolField(key string, def bool) ModelFieldDef {
	return ModelFieldDef{
		Key: key, Type: FieldTypeBoolean, Required: false, Default: def,
	}
}

func selectField(key string, def string, opts []FieldOption) ModelFieldDef {
	return ModelFieldDef{
		Key: key, Type: FieldTypeSelect, Required: false, Default: def, Options: opts,
	}
}

// SupportedModelTypes is the canonical list of model types.
// Single source of truth for Go layer, derived from HAL HalPostprocessType enum.
var SupportedModelTypes = []ModelTypeDef{
	{
		ID: "detection", Label: "Object Detection",
		Aliases: []string{"yolo"},
		Fields: []ModelFieldDef{
			reqNumField("threshold", 0.25, 0, 1, 0.01),
			reqNumField("max_detections", 64, 1, 999, 1),
			numField("nms_threshold", 0.45, 0, 1, 0.01),
		},
	},
	{
		ID: "classification", Label: "Image Classification",
		Fields: []ModelFieldDef{
			numField("threshold", 0.25, 0, 1, 0.01),
			numField("top_k", 5, 1, 100, 1),
		},
	},
	{
		ID: "segmentation", Label: "Semantic Segmentation",
		Fields: []ModelFieldDef{
			numField("threshold", 0.25, 0, 1, 0.01),
		},
	},
	{
		ID: "keypoint", Label: "Keypoint Detection",
		Aliases: []string{"landmarks", "landmark"},
		Fields: []ModelFieldDef{
			numField("threshold", 0.25, 0, 1, 0.01),
			numField("keypoint_threshold", 0.25, 0, 1, 0.01),
			numField("num_keypoints", 0, 0, 200, 1),
		},
	},
	{
		ID: "clip", Label: "CLIP Zero-Shot",
		Fields: []ModelFieldDef{
			numField("score_threshold", 0.0, 0, 1, 0.01),
			numField("top_k", 1, 1, 20, 1),
			selectField("match_policy", "any", []FieldOption{
				{Value: "any", Label: "Any Match"},
				{Value: "all", Label: "All Must Match"},
			}),
		},
	},
	{
		ID: "embedding", Label: "Feature Embedding",
		Fields: []ModelFieldDef{
			boolField("normalize", true),
		},
	},
	{
		ID: "ocr_detection", Label: "OCR Text Detection",
		Fields: []ModelFieldDef{
			numField("threshold", 0.25, 0, 1, 0.01),
			numField("max_detections", 64, 1, 999, 1),
		},
	},
	{
		ID: "ocr_recognition", Label: "OCR Text Recognition",
		Fields: []ModelFieldDef{},
	},
	{
		ID: "depth", Label: "Depth Estimation",
		Fields: []ModelFieldDef{},
	},
	{
		ID: "genai", Label: "Generative AI",
		Aliases: []string{"vlm", "llm"},
		Fields: []ModelFieldDef{
			numField("max_context_length", 2048, 256, 8192, 256),
			numField("temperature", 0.7, 0, 2, 0.1),
		},
	},
}

// SupportedFormats lists accepted model file formats for the current platform.
var SupportedFormats = []FileFormat{
	{Extension: ".hef", MIMEType: "application/octet-stream", Label: "Hailo HEF"},
}

// ResolveModelType normalizes aliases to canonical ID.
func ResolveModelType(raw string) string {
	low := strings.ToLower(strings.TrimSpace(raw))
	for _, t := range SupportedModelTypes {
		if t.ID == low {
			return t.ID
		}
		for _, a := range t.Aliases {
			if a == low {
				return t.ID
			}
		}
	}
	return ""
}

// GetModelTypeDef returns the ModelTypeDef for a canonical ID, or nil.
func GetModelTypeDef(id string) *ModelTypeDef {
	for i := range SupportedModelTypes {
		if SupportedModelTypes[i].ID == id {
			return &SupportedModelTypes[i]
		}
	}
	return nil
}

// GetFieldDefaults returns a map of key→default for a given model type.
func GetFieldDefaults(typeID string) map[string]interface{} {
	td := GetModelTypeDef(typeID)
	if td == nil {
		return nil
	}
	defaults := make(map[string]interface{}, len(td.Fields))
	for _, f := range td.Fields {
		if f.Default != nil {
			defaults[f.Key] = f.Default
		}
	}
	return defaults
}

// GuessModelType attempts to infer model type from network name heuristics.
func GuessModelType(networkName string) string {
	n := strings.ToLower(networkName)
	switch {
	// Specific patterns first (before generic "det")
	case strings.Contains(n, "ocr_det"):
		return "ocr_detection"
	case strings.Contains(n, "ocr_rec") || strings.Contains(n, "recognition"):
		return "ocr_recognition"
	case strings.Contains(n, "lprnet") || strings.Contains(n, "license_plate"):
		return "ocr_recognition"
	// Generic patterns
	case strings.Contains(n, "yolo") || strings.Contains(n, "det"):
		return "detection"
	case strings.Contains(n, "cls") || strings.Contains(n, "class") || strings.Contains(n, "vit"):
		return "classification"
	case strings.Contains(n, "seg") || strings.Contains(n, "linknet"):
		return "segmentation"
	case strings.Contains(n, "pose") || strings.Contains(n, "keypoint") || strings.Contains(n, "landmark") || strings.Contains(n, "face"):
		return "keypoint"
	case strings.Contains(n, "clip"):
		return "clip"
	case strings.Contains(n, "embed"):
		return "embedding"
	case strings.Contains(n, "depth") || strings.Contains(n, "scdepth"):
		return "depth"
	case strings.Contains(n, "qwen") || strings.Contains(n, "genai") || strings.Contains(n, "vlm") || strings.Contains(n, "llm"):
		return "genai"
	default:
		return "detection"
	}
}
