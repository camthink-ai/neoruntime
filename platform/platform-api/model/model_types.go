package model

import (
	"strings"

	"aipc/platform/postprocess"
)

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
//
// Effect (empty = postprocess.EffectConsumed) tells the UI whether filling
// the field changes decoder behavior: advisory fields render a "no effect"
// badge, metadata fields a "recorded only" one. Profiles, when non-empty,
// restricts the field to rows whose postprocess_profile is in the list — the
// schema stays one table but inactive-profile knobs neither render nor
// validate.
type ModelFieldDef struct {
	Key      string             `json:"key"`
	Type     FieldType          `json:"type"`
	Required bool               `json:"required"`
	Default  interface{}        `json:"default"`
	Min      *float64           `json:"min,omitempty"`
	Max      *float64           `json:"max,omitempty"`
	Step     *float64           `json:"step,omitempty"`
	Options  []FieldOption      `json:"options,omitempty"`
	Effect   postprocess.Effect `json:"effect,omitempty"`
	Profiles []string           `json:"profiles,omitempty"`
}

// ModelTypeDef describes a supported model postprocess type.
// Mirrors HAL enum HalPostprocessType in hal_v2/include/model/hal_postprocess.h.
//
// ProfileParamEffects (multi-decoder types only) overrides the Fields'
// Effect per postprocess_profile value, keyed by the profile's select
// value and the FIELD key (the registry's blob-dialect key is bridged;
// see applyRegistryParamEffects). It is what lets the page say "under
// yolov5m_vehicles this threshold does nothing" without a second
// hand-maintained effect table.
type ModelTypeDef struct {
	ID                  string                                   `json:"id"`
	Label               string                                   `json:"label"`
	Fields              []ModelFieldDef                          `json:"fields"`
	Aliases             []string                                 `json:"aliases,omitempty"`
	ProfileParamEffects map[string]map[string]postprocess.Effect `json:"profile_param_effects,omitempty"`
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

// profiledNumField is a numField visible only under the listed
// postprocess_profile values (see ModelFieldDef.Profiles).
func profiledNumField(key string, def, min, max, step float64, profiles ...string) ModelFieldDef {
	f := numField(key, def, min, max, step)
	f.Profiles = profiles
	return f
}

// withEffect overrides a constructor's default effect annotation.
func withEffect(f ModelFieldDef, effect postprocess.Effect) ModelFieldDef {
	f.Effect = effect
	return f
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
// Derived from the postprocess registry (platform/postprocess) — the single
// source of truth; the gRPC-side mirror is the generated
// ai-runtime/include/postprocess_schema.h. Extend the registry and
// regenerate, never this table.
//
// The Custom entry (yolov5m_vehicles) is a customer-trained parking-lot
// model: RGB888 1920x1080 in, one class. Unlike the yolov8 profiles its NMS
// tensor is named yolov5m_vehicles/yolov5_nms_postprocess, so only the
// composed variant's backend_function routes it — the plugin's default
// selection never matches (fire-smoke signature). Device-verified
// 2026-09-02: output decodes exactly like the hand-decoded NMS blob, but
// the label table is baked ("car") and ignores the JSON labels. Custom: the
// wizard suggests it from the parsed vstream info and the dropdown only
// surfaces it then (or when updating such a row) — it is invisible to users
// without this deployment's HEF.
var DetectionPostprocessProfiles = detectionProfilesFromRegistry()

func detectionProfilesFromRegistry() []DetectionPostprocessProfile {
	decoders := postprocess.DecodersForType("detection")
	out := make([]DetectionPostprocessProfile, 0, len(decoders))
	for _, d := range decoders {
		// Registry order == the hand-maintained order above it: parameterized
		// n/s/m first, the custom fixed-name entry last.
		out = append(out, DetectionPostprocessProfile{
			Basename:        d.Basename,
			BackendFunction: d.Decoder,
			Label:           d.Label,
			Custom:          d.Custom,
		})
	}
	return out
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

// KeypointPostprocessProfile couples a postprocess_profile value with the
// HAL decoder it activates. Unlike detection, keypoint decoders are selected
// inside the variant blob (empty blob = facial default; the create-time
// native_yolov8_pose flag = built-in pose decoder), so the profile mainly
// tells modelload which blob to compose and the wizard which knobs exist.
type KeypointPostprocessProfile struct {
	Value   string // postprocess_profile select value
	Decoder string // HAL decoder id (registry Decoder)
	Label   string // human-readable label for the wizard dropdown
}

const (
	// KeypointProfileFacial is the zero-config default: libmediapipe facial
	// landmarks, selected by an empty variant blob — identical to keypoint
	// rows stored before profiles existed (missing key == this value).
	KeypointProfileFacial = "facial_landmarks"
	// KeypointProfilePose activates HAL's built-in YOLOv8 pose decoder via a
	// create-time variant blob (native_yolov8_pose:true + thresholds).
	KeypointProfilePose = "yolov8_pose"
)

// DefaultKeypointProfile matches the pre-profile behavior byte-for-byte: no
// variant blob, mediapipe facial decoder.
const DefaultKeypointProfile = KeypointProfileFacial

// KeypointPostprocessProfiles is derived from the postprocess registry —
// same single-source rule as the detection table.
var KeypointPostprocessProfiles = keypointProfilesFromRegistry()

func keypointProfilesFromRegistry() []KeypointPostprocessProfile {
	decoders := postprocess.DecodersForType("keypoint")
	out := make([]KeypointPostprocessProfile, 0, len(decoders))
	for _, d := range decoders {
		out = append(out, KeypointPostprocessProfile{
			Value:   d.SelectValue,
			Decoder: d.Decoder,
			Label:   d.Label,
		})
	}
	return out
}

// LookupKeypointProfile returns the profile a postprocess_profile value
// points at; ok is false for values outside the two verified decoders.
func LookupKeypointProfile(value string) (KeypointPostprocessProfile, bool) {
	for _, p := range KeypointPostprocessProfiles {
		if p.Value == value {
			return p, true
		}
	}
	return KeypointPostprocessProfile{}, false
}

func keypointProfileOptions() []FieldOption {
	opts := make([]FieldOption, 0, len(KeypointPostprocessProfiles))
	for _, p := range KeypointPostprocessProfiles {
		opts = append(opts, FieldOption{Value: p.Value, Label: p.Label})
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
			// Advisory: it composes into the blob's iou_threshold, but the
			// parameterized plugin functions never read it (device-verified
			// 2026-09-02; registry rows agree).
			withEffect(numField("nms_threshold", 0.45, 0, 1, 0.01), postprocess.EffectAdvisory),
			// Drives the runtime materialization basename and the composed
			// variant's backend_function (see handlers/ai_postprocess.go).
			selectField("postprocess_profile", DefaultDetectionProfile, detectionProfileOptions()),
			// Consumed by the parameterized decoders (hailo_yolov8n/s/m):
			// device A/B 2026-09-20 — relabeling live post_result output.
			// The fixed-name entries (yolov5m_vehicles) bake their table and
			// ignore it; that per-decoder split lives in the registry rows
			// (labelsConsumed), not this coarse single-value annotation.
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
		// Two decoders with disjoint knob sets: mediapipe facial landmarks
		// (default, zero configuration — 468 points, presence 0.5, 192x192
		// all compile-time fixed) and HAL's built-in YOLOv8 pose (COCO-17
		// hardcoded; thresholds live-update, network size is create-only).
		// num_keypoints is deliberately absent from the schema: both decoders
		// hardcode their topology and the key has no effect on either.
		ID: "keypoint", Label: "Keypoint Detection",
		Aliases: []string{"landmarks", "landmark"},
		Fields: []ModelFieldDef{
			// Missing key == facial_landmarks: existing rows keep their exact
			// pre-profile behavior (empty variant blob, mediapipe decoder).
			selectField("postprocess_profile", DefaultKeypointProfile, keypointProfileOptions()),
			// Pose-only (composes into the blob's score_threshold). Min 0.01:
			// HAL treats score_threshold < 1e-6 as unset and silently falls
			// back to 0.6 — the form must not offer values that read as "let
			// HAL decide".
			profiledNumField("threshold", 0.25, 0.01, 1, 0.01, KeypointProfilePose),
			profiledNumField("keypoint_threshold", 0.25, 0, 1, 0.01, KeypointProfilePose),
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
		// monocular_depth/scdepth are the runtime's separate known-type
		// spellings (registry RuntimeTypes); for the wizard they are the
		// same knob-free depth family.
		ID: "depth", Label: "Depth Estimation",
		Aliases: []string{"monocular_depth", "scdepth"},
		Fields:  []ModelFieldDef{},
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

// LoadProbeWorthy reports whether a model type follows the platform
// postprocess path whose failure modes are only observable through
// post_result — the load-time smoke probe (one zero-input infer, expect a
// non-empty post_result) exists for exactly these types. It replaces the
// hardcoded `== "detection"` checks at the probe call sites so keypoint
// models get the same fail-loud guarantee.
func LoadProbeWorthy(typeID string) bool {
	switch ResolveModelType(typeID) {
	case "detection", "keypoint":
		return true
	}
	return false
}

// GuessModelType attempts to infer model type from network name heuristics.
func GuessModelType(networkName string) string {
	n := strings.ToLower(networkName)
	switch {
	// Specific patterns first (before generic "yolo"/"det") — pose networks
	// ship as yolov8*_pose, so keypoint identity must outrank the yolo prefix
	// or the wizard suggests detection for a pose HEF. "face" is NOT part of
	// the hoisted set: face_detection/face_detector are detection networks,
	// and a bare face token outranking det sends them at the facial-landmarks
	// decoder (review 2026-09-21). "clip" joins the hoisted set for the same
	// reason against a generic token: CLIP encoders ship as clip_vit_b_32_*,
	// and the "vit" token in the classification case below would otherwise
	// swallow every ViT-named CLIP network (found on-device 2026-09-26).
	case strings.Contains(n, "ocr_det"):
		return "ocr_detection"
	case strings.Contains(n, "ocr_rec") || strings.Contains(n, "recognition"):
		return "ocr_recognition"
	case strings.Contains(n, "lprnet") || strings.Contains(n, "license_plate"):
		return "ocr_recognition"
	case strings.Contains(n, "pose") || strings.Contains(n, "keypoint") || strings.Contains(n, "landmark"):
		return "keypoint"
	case strings.Contains(n, "clip"):
		return "clip"
	// Generic patterns
	case strings.Contains(n, "yolo") || strings.Contains(n, "det"):
		return "detection"
	case strings.Contains(n, "cls") || strings.Contains(n, "class") || strings.Contains(n, "vit"):
		return "classification"
	case strings.Contains(n, "seg") || strings.Contains(n, "linknet"):
		return "segmentation"
	// "face" stays late (its pre-hoist position): by the time a name reaches
	// this case it carries no det/yolo token, so face_mesh and unnamed
	// mediapipe-style face networks still suggest keypoint while
	// face_detection already matched detection above.
	case strings.Contains(n, "face"):
		return "keypoint"
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

// paramKeyToFieldKey bridges the registry's variant-blob dialect to the
// config field dialect for effect projection ONLY (presentation). The two
// namespaces spell the same knob differently; validation walks each
// namespace independently and never consults this table.
var paramKeyToFieldKey = map[string]map[string]string{
	"detection": {
		"detection_threshold": "threshold",
		"max_boxes":           "max_detections",
		"iou_threshold":       "nms_threshold",
		"labels":              "labels",
	},
	"keypoint": {
		"score_threshold":    "threshold",
		"keypoint_threshold": "keypoint_threshold",
	},
}

// applyRegistryParamEffects projects the decoder registry's per-decoder
// effect truth onto the wizard schema, so a page badge answers "does THIS
// knob do anything under the SELECTED decoder" instead of a single
// worst-case annotation:
//   - single-decoder types: the decoder's params ARE the type's effect
//     truth — field effects are overwritten from the registry (that is how
//     segmentation/clip/ocr knobs carry their advisory truth, which the
//     hand-written field table predates).
//   - multi-decoder types (detection's four plugin entries, keypoint's
//     facial/pose pair): per-profile overrides land in ProfileParamEffects
//     keyed by profile select value; the field-level annotation stays as
//     the default profile's truth.
//
// Runs once at package init; the registry is immutable after load.
func applyRegistryParamEffects() {
	for i := range SupportedModelTypes {
		td := &SupportedModelTypes[i]
		decoders := postprocess.DecodersForType(td.ID)
		if len(decoders) == 0 {
			continue
		}
		bridge := paramKeyToFieldKey[td.ID]
		fieldKey := func(paramKey string) string {
			if k, ok := bridge[paramKey]; ok {
				return k
			}
			return paramKey
		}
		if len(decoders) == 1 {
			for _, p := range decoders[0].Params {
				fk := fieldKey(p.Key)
				for j := range td.Fields {
					if td.Fields[j].Key == fk {
						td.Fields[j].Effect = p.Effect
					}
				}
			}
			continue
		}
		overrides := map[string]map[string]postprocess.Effect{}
		for _, d := range decoders {
			profileKey := d.SelectValue
			if profileKey == "" {
				profileKey = d.Decoder
			}
			m := map[string]postprocess.Effect{}
			for _, p := range d.Params {
				m[fieldKey(p.Key)] = p.Effect
			}
			if len(m) > 0 {
				overrides[profileKey] = m
			}
		}
		if len(overrides) > 0 {
			td.ProfileParamEffects = overrides
		}
	}
}

func init() {
	applyRegistryParamEffects()
}
