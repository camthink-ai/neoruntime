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
// Custom marks deployment-specific entries (e.g. a customer-trained model
// verified against this plugin build): the standard UI hides them unless
// they are already the active value, so generic users never see them.
type FieldOption struct {
	Value  string `json:"value"`
	Label  string `json:"label"`
	Custom bool   `json:"custom,omitempty"`
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

func textField(key string, def string) ModelFieldDef {
	return ModelFieldDef{
		Key: key, Type: FieldTypeText, Required: false, Default: def,
	}
}

// DetectionPostprocessProfile couples a HEF basename the vendor postprocess
// plugin (libyolo_hailortpp_post.so) recognizes with the backend function it
// maps to. HAL rewrites the NMS tensor name to <HEF basename>/yolov8_nms_postprocess
// and the plugin selects its function by that basename, so models stored under
// any other name (e.g. the sha256 CAS blob name) fail postprocess on every
// frame and must be re-materialized under one of these names at load time.
type DetectionPostprocessProfile struct {
	Basename        string // HEF filename without extension
	BackendFunction string // plugin backend function for this basename
	Label           string // human-readable label for the wizard dropdown
	Custom          bool   // deployment-specific entry; UI hides it unless active
}

// DefaultDetectionProfile is the zero-config profile: the plugin's default
// postprocess function is bound to this basename.
const DefaultDetectionProfile = "hailo_yolov8n_384_640"

// DetectionPostprocessProfiles lists the basenames verified against the
// vendor plugin. Other compiled-in names map to generic single-argument
// functions with a hardcoded 0.4 threshold and are deliberately excluded.
var DetectionPostprocessProfiles = []DetectionPostprocessProfile{
	{Basename: "hailo_yolov8n_384_640", BackendFunction: "hailo_yolov8n", Label: "YOLOv8n 384x640 (default)"},
	{Basename: "hailo_yolov8s_384_640", BackendFunction: "hailo_yolov8s", Label: "YOLOv8s 384x640"},
	{Basename: "hailo_yolov8m_384_640", BackendFunction: "hailo_yolov8m", Label: "YOLOv8m 384x640"},
	// Customer-trained parking-lot model: RGB888 1920x1080 in, one class.
	// Unlike the yolov8 profiles its NMS tensor is named
	// yolov5m_vehicles/yolov5_nms_postprocess, so only the composed
	// variant's backend_function routes it — the plugin's default selection
	// never matches (fire-smoke signature). Device-verified 2026-09-02:
	// output decodes exactly like the hand-decoded NMS blob, but the label
	// table is baked ("car") and ignores the JSON labels.
	// Custom: the wizard suggests it from the parsed vstream info and the
	// dropdown only surfaces it then (or when updating such a row) — it is
	// invisible to users without this deployment's HEF.
	{Basename: "yolov5m_vehicles", BackendFunction: "yolov5m_vehicles", Label: "YOLOv5m Vehicles 1920x1080", Custom: true},
}

// LookupDetectionProfile returns the profile for a basename; ok is false for
// names the plugin does not handle usefully.
func LookupDetectionProfile(basename string) (DetectionPostprocessProfile, bool) {
	for _, p := range DetectionPostprocessProfiles {
		if p.Basename == basename {
			return p, true
		}
	}
	return DetectionPostprocessProfile{}, false
}

// LookupDetectionBackendFunction returns the profile a postprocess
// backend_function name belongs to; ok is false for names outside the
// verified set (including the generic single-argument functions, which
// hardcode a 0.4 threshold and COCO labels and are not usable).
func LookupDetectionBackendFunction(fn string) (DetectionPostprocessProfile, bool) {
	for _, p := range DetectionPostprocessProfiles {
		if p.BackendFunction == fn {
			return p, true
		}
	}
	return DetectionPostprocessProfile{}, false
}

func detectionProfileOptions() []FieldOption {
	opts := make([]FieldOption, 0, len(DetectionPostprocessProfiles))
	for _, p := range DetectionPostprocessProfiles {
		opts = append(opts, FieldOption{Value: p.Basename, Label: p.Label, Custom: p.Custom})
	}
	return opts
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
			// Drives the runtime materialization basename and the composed
			// variant's backend_function (see handlers/ai_postprocess.go).
			selectField("postprocess_profile", DefaultDetectionProfile, detectionProfileOptions()),
			// Metadata only: the plugin's label table is compiled in and cannot
			// be changed via JSON. Consumers map output class_id N (1-based)
			// to labels[N-1]; list classes in training order, no background.
			textField("labels", ""),
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

// PackageExtension is the import-only single-file container (AMPK layout, see
// storage/modelpackage.go): platform metadata JSON + the HEF, unpacked and
// staged as a plain .hef blob at parse time. Deliberately not part of
// SupportedFormats — it is a transport container, not a model binary.
const PackageExtension = ".bin"

// Output delivery modes — orthogonal to the semantic model type. The type
// answers "what do the outputs mean" (UI/metadata); the mode answers "how are
// they delivered": plugin-decoded structured results, or bare NPU tensors the
// consumer decodes itself.
const (
	OutputModePlatform = "platform" // platform postprocess decodes NMS blobs into structured results
	OutputModeRaw      = "raw"      // no postprocess session; Infer returns raw output tensors
)

// ResolveOutputMode normalizes a requested/stored output mode. Empty resolves
// to platform (rows written before the column existed, requests that omit it).
// ok is false for values outside the known set — API boundaries should reject
// those rather than silently coerce.
func ResolveOutputMode(raw string) (mode string, ok bool) {
	trimmed := strings.TrimSpace(raw)
	switch trimmed {
	case "":
		return OutputModePlatform, true
	case OutputModePlatform, OutputModeRaw:
		return trimmed, true
	default:
		return "", false
	}
}

// HEF output format classifications, derived from parse-hef vstream info.
// This — not the semantic model type — decides whether the platform
// postprocess path is even possible.
const (
	OutputFormatNMS        = "nms"         // NMS layer compiled in: fixed-format detections blob
	OutputFormatFeatureMap = "feature_map" // raw feature maps; only consumable in raw output mode
)

// ClassifyOutputFormat inspects parse-hef vstream info for an NMS-layer
// output (tensor names like <basename>/yolov8_nms_postprocess). Empty input
// returns "" (unknown) so legacy rows without vstream info skip
// cross-validation instead of failing open or closed.
func ClassifyOutputFormat(vstreamInfo string) string {
	if strings.TrimSpace(vstreamInfo) == "" {
		return ""
	}
	if strings.Contains(vstreamInfo, "_nms_postprocess") {
		return OutputFormatNMS
	}
	return OutputFormatFeatureMap
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
