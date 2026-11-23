package postprocess

// Registry data. Every fact below traces to either the vendor postprocess
// library 1.12.1 source (see proposal §3.1) or on-device verification; where
// neither covers a key, Effect is EffectAdvisory as the conservative call.

// RuntimeTypes lists the model_type strings ai-runtime's gRPC RegisterModel
// accepts. Verbatim mirror of the known-type table in
// platform/ai-runtime/src/model_variant_validation.cpp — an unknown type here
// is a silent fallthrough to the detection default in init_post_process, so
// membership changes must be paired with that switch and a regeneration.
// Note "genai" exists only in the Go wizard table (no runtime postprocess
// path) and is deliberately absent.
var RuntimeTypes = []string{
	"detection", "yolo", "landmarks", "keypoint", "segmentation",
	"classification", "clip", "embedding", "depth", "monocular_depth",
	"scdepth", "ocr_detection", "ocr_recognition",
}

// DetectionVariantKeys is the closed key set of the detection variant JSON
// dialect. The vendor plugin's schema requires all of them up front and the
// REST/gRPC surfaces accept exactly these — mirrors
// handlers.customVariantKeys and the generated C++ table.
var DetectionVariantKeys = []string{
	"backend_function",
	"iou_threshold",
	"detection_threshold",
	"output_activation",
	"label_offset",
	"max_boxes",
	"labels",
}

// KeypointVariantKeys is the informational key set of the keypoint variant
// JSON dialect: the create-time decoder flag plus every key the HAL accept
// layer recognizes for the keypoint family. The dialect is NOT closed —
// unrecognized keys pass through opaquely to HAL (legacy opaque channel) —
// so this set drives documentation and badges, not rejection.
var KeypointVariantKeys = []string{
	"native_yolov8_pose",
	"score_threshold",
	"confidence_threshold",
	"keypoint_threshold",
	"iou_threshold",
	"num_keypoints",
	"yolov8_pose_network_width",
	"yolov8_pose_network_height",
}

// ForbiddenVariantKeys are control keys the HAL postprocess layer reads to
// dlopen arbitrary shared objects. They are rejected on every variant
// surface (REST detection closed set, REST keypoint blacklist, gRPC
// validator) regardless of model type.
var ForbiddenVariantKeys = []string{
	"backend_lib_path",
	"backend_config_path",
}

// Aliases maps same-knob spellings across namespaces.
var Aliases = []AliasRule{
	// The detect threshold appears as detection_threshold in the detection
	// blob dialect and confidence_threshold in HAL's scalar sync tables.
	{Canonical: "confidence_threshold", Aliases: []string{"detection_threshold"}},
	// The wizard's max_detections field composes into the blob's max_boxes.
	{Canonical: "max_boxes", Aliases: []string{"max_detections"}},
}

// hailoYoloParams builds the seven-key detection blob parameter table. The
// parameterized plugin entries (hailo_yolov8n/s/m) consume exactly three of
// the seven beyond the routing key; the fixed-name entries consume none at
// all (device-verified 2026-09-02: baked label table, unread knobs), so every
// non-routing effect degrades to advisory there.
func hailoYoloParams(backend string, parameterized bool) []ParamDef {
	effect := func(consumed Effect) Effect {
		if parameterized {
			return consumed
		}
		return EffectAdvisory
	}
	return []ParamDef{
		{Key: "backend_function", Type: "string", Default: backend,
			Mutability: MutabilityCreateOnly, Effect: EffectConsumed},
		{Key: "labels", Type: "string_list",
			Mutability: MutabilityCreateOnly, Effect: effect(EffectConsumed)},
		{Key: "detection_threshold", Type: "number", Min: fptr(0), Max: fptr(1), Default: 0.25,
			Mutability: MutabilityHotUpdate, Effect: effect(EffectConsumed)},
		{Key: "max_boxes", Type: "integer", Min: fptr(1), Max: fptr(999), Default: 64,
			Mutability: MutabilityCreateOnly, Effect: effect(EffectConsumed)},
		{Key: "iou_threshold", Type: "number", Min: fptr(0), Max: fptr(1), Default: 0.45,
			Mutability: MutabilityHotUpdate, Effect: EffectAdvisory},
		{Key: "output_activation", Type: "string", Default: "none",
			Mutability: MutabilityCreateOnly, Effect: EffectAdvisory},
		{Key: "label_offset", Type: "integer", Default: 1,
			Mutability: MutabilityCreateOnly, Effect: EffectAdvisory},
	}
}

// fptr keeps numeric range literals to one line in the tables below.
func fptr(v float64) *float64 { return &v }

// Decoders is the decoder capability matrix, machine-readable.
var Decoders = []DecoderSpec{
	// --- detection: vendor libyolo_hailortpp plugin -----------------------
	{
		ModelType: "detection", Decoder: "hailo_yolov8n", Label: "YOLOv8n 384x640 (default)",
		SelectValue: "hailo_yolov8n_384_640", Basename: "hailo_yolov8n_384_640",
		Selection: SelectionBackendFunction,
		Params:    hailoYoloParams("hailo_yolov8n", true),
		Hardcoded: []HardcodedDef{
			{Key: "output_tensor", Value: "hailo_yolov8n_384_640/yolov8_nms_postprocess"},
			{Key: "labels", Value: "COCO 80", Note: "compiled-in table; the labels key only relabels for consumers"},
			{Key: "filter_by_score", Value: "true"},
		},
	},
	{
		ModelType: "detection", Decoder: "hailo_yolov8s", Label: "YOLOv8s 384x640",
		SelectValue: "hailo_yolov8s_384_640", Basename: "hailo_yolov8s_384_640",
		Selection: SelectionBackendFunction,
		Params:    hailoYoloParams("hailo_yolov8s", true),
		Hardcoded: []HardcodedDef{
			{Key: "output_tensor", Value: "hailo_yolov8s_384_640/yolov8_nms_postprocess"},
			{Key: "labels", Value: "COCO 80", Note: "compiled-in table; the labels key only relabels for consumers"},
			{Key: "filter_by_score", Value: "true"},
		},
	},
	{
		ModelType: "detection", Decoder: "hailo_yolov8m", Label: "YOLOv8m 384x640",
		SelectValue: "hailo_yolov8m_384_640", Basename: "hailo_yolov8m_384_640",
		Selection: SelectionBackendFunction,
		Params:    hailoYoloParams("hailo_yolov8m", true),
		Hardcoded: []HardcodedDef{
			{Key: "output_tensor", Value: "hailo_yolov8m_384_640/yolov8_nms_postprocess"},
			{Key: "labels", Value: "COCO 80", Note: "compiled-in table; the labels key only relabels for consumers"},
			{Key: "filter_by_score", Value: "true"},
		},
	},
	{
		// Fixed-name entry: only the routing key has effect; the label table
		// is baked ("car") and every other blob key is accepted but unread.
		// Device-verified 2026-09-02 (yolov5m_vehicles).
		ModelType: "detection", Decoder: "yolov5m_vehicles", Label: "YOLOv5m Vehicles 1920x1080",
		SelectValue: "yolov5m_vehicles", Basename: "yolov5m_vehicles",
		Selection: SelectionBackendFunction, Custom: true,
		Params: hailoYoloParams("yolov5m_vehicles", false),
		Hardcoded: []HardcodedDef{
			{Key: "output_tensor", Value: "yolov5m_vehicles/yolov5_nms_postprocess"},
			{Key: "labels", Value: `"car" (single class, baked)`},
		},
	},

	// --- keypoint: mediapipe facial (default) vs built-in COCO-17 pose ----
	{
		// libmediapipe_post.so facial_landmarks_nv12. Reads no configuration
		// at all — every accepted key is an accept-layer phantom.
		ModelType: "keypoint", Decoder: "facial_landmarks_nv12", Label: "Face Landmarks (default)",
		SelectValue: "facial_landmarks",
		Selection:   SelectionEmptyVariant,
		Hardcoded: []HardcodedDef{
			{Key: "num_keypoints", Value: "468"},
			{Key: "presence_threshold", Value: "0.5"},
			{Key: "input_normalization", Value: "192x192"},
			{Key: "output_tensors", Value: "face_landmarks_lite/conv22, face_landmarks_lite/conv25"},
		},
	},
	{
		// Built-in HAL decoder, selected by the create-time
		// native_yolov8_pose flag in the variant blob — the flag must be
		// present at registration; update_postprocess_config cannot flip it.
		ModelType: "keypoint", Decoder: "native_yolov8_pose", Label: "YOLOv8 Pose (COCO-17)",
		SelectValue: "yolov8_pose",
		Selection:   SelectionFlagPrefix + "native_yolov8_pose",
		Params: []ParamDef{
			{Key: "native_yolov8_pose", Type: "boolean", Default: true,
				Mutability: MutabilityCreateOnly, Effect: EffectConsumed},
			{Key: "score_threshold", Type: "number", Min: fptr(0.01), Max: fptr(1), Default: 0.25,
				Mutability: MutabilityHotUpdate, Effect: EffectConsumed,
				Note: "takes precedence over confidence_threshold; <1e-6 silently falls back to 0.6 in the decoder"},
			{Key: "keypoint_threshold", Type: "number", Min: fptr(0), Max: fptr(1), Default: 0.25,
				Mutability: MutabilityHotUpdate, Effect: EffectConsumed},
			{Key: "confidence_threshold", Type: "number", Min: fptr(0), Max: fptr(1), Default: 0.25,
				Mutability: MutabilityHotUpdate, Effect: EffectConsumed,
				Note: "only used when score_threshold is absent"},
			{Key: "iou_threshold", Type: "number", Min: fptr(0), Max: fptr(1), Default: 0.7,
				Mutability: MutabilityHotUpdate, Effect: EffectConsumed,
				Note: "adopted only when 0 < v < 1"},
			{Key: "yolov8_pose_network_width", Type: "integer",
				Min: fptr(16), Max: fptr(4096), Default: 640,
				Mutability: MutabilityCreateOnly, Effect: EffectConsumed,
				Note: "read once at create; absent from both hot-update key tables"},
			{Key: "yolov8_pose_network_height", Type: "integer",
				Min: fptr(16), Max: fptr(4096), Default: 640,
				Mutability: MutabilityCreateOnly, Effect: EffectConsumed,
				Note: "read once at create; absent from both hot-update key tables"},
			{Key: "num_keypoints", Type: "number",
				Mutability: MutabilityHotUpdate, Effect: EffectAdvisory,
				Note: "decoder hardcodes COCO-17; this key changes nothing"},
		},
		Hardcoded: []HardcodedDef{
			{Key: "num_keypoints", Value: "17 (COCO-17 topology)"},
		},
	},

	// --- remaining matrix rows (proposal §3.1), effect-conservative ------
	{
		ModelType: "segmentation", Decoder: "linknet", Label: "LinkNet Segmentation",
		Selection: SelectionEmptyVariant,
		Params: []ParamDef{
			{Key: "threshold", Type: "number", Min: fptr(0), Max: fptr(1), Default: 0.25,
				Mutability: MutabilityHotUpdate, Effect: EffectAdvisory},
		},
		Hardcoded: []HardcodedDef{
			{Key: "mask_threshold", Value: "0.5"},
			{Key: "output_layout", Value: "single channel, one class per output tensor"},
		},
	},
	{
		ModelType: "clip", Decoder: "clip_zeroshot", Label: "CLIP Zero-Shot Classification",
		Selection: SelectionEmptyVariant,
		Params: []ParamDef{
			{Key: "score_threshold", Type: "number", Min: fptr(0), Max: fptr(1),
				Mutability: MutabilityHotUpdate, Effect: EffectAdvisory},
			{Key: "top_k", Type: "number", Min: fptr(1), Max: fptr(100), Default: 5,
				Mutability: MutabilityHotUpdate, Effect: EffectAdvisory},
		},
		Hardcoded: []HardcodedDef{
			{Key: "logit_scale", Value: "100"},
			{Key: "prompt_prefix", Value: `"A photo of "`},
			{Key: "top_k", Value: "not implemented (returns all scores)"},
			{Key: "prompts_channel", Value: "ZeroMQ runtime messages, not the config file"},
		},
	},
	{
		ModelType: "embedding", Decoder: "clipgen", Label: "CLIP Image Embedding",
		Selection: SelectionEmptyVariant,
		Hardcoded: []HardcodedDef{
			{Key: "embedding_dims", Value: "640 / 512 / 768 (per model)"},
			{Key: "output_layout", Value: "single tensor, 1-D assumed"},
		},
	},
	{
		ModelType: "ocr_detection", Decoder: "paddle_det", Label: "PaddleOCR Text Detection",
		Selection: SelectionEmptyVariant,
		Params: []ParamDef{
			{Key: "threshold", Type: "number", Min: fptr(0), Max: fptr(1), Default: 0.25,
				Mutability: MutabilityHotUpdate, Effect: EffectAdvisory},
			{Key: "max_detections", Type: "number", Min: fptr(1), Max: fptr(999), Default: 64,
				Mutability: MutabilityCreateOnly, Effect: EffectAdvisory},
		},
	},
	{
		ModelType: "ocr_recognition", Decoder: "paddle_rec", Label: "PaddleOCR Text Recognition",
		Selection: SelectionEmptyVariant,
		Hardcoded: []HardcodedDef{
			{Key: "charset", Value: "compiled-in table"},
		},
	},
	{
		ModelType: "depth", Decoder: "monocular_depth", Label: "Monocular Depth",
		Selection: SelectionEmptyVariant,
	},
	{
		ModelType: "scdepth", Decoder: "scdepth", Label: "SCDepth",
		Selection: SelectionEmptyVariant,
		Params: []ParamDef{
			{Key: "scdepth_output_name", Type: "string",
				Mutability: MutabilityCreateOnly, Effect: EffectConsumed},
			{Key: "depth_float32", Type: "boolean",
				Mutability: MutabilityCreateOnly, Effect: EffectConsumed},
		},
	},
}

// DecodersForType returns the decoders registered under a model type.
func DecodersForType(modelType string) []DecoderSpec {
	var out []DecoderSpec
	for _, d := range Decoders {
		if d.ModelType == modelType {
			out = append(out, d)
		}
	}
	return out
}

// LookupDecoder finds a decoder spec by type and decoder name.
func LookupDecoder(modelType, decoder string) (DecoderSpec, bool) {
	for _, d := range Decoders {
		if d.ModelType == modelType && d.Decoder == decoder {
			return d, true
		}
	}
	return DecoderSpec{}, false
}

// LookupSelectValue finds the decoder a postprocess_profile select value
// points at (detection basenames and keypoint decoder ids share the key).
func LookupSelectValue(modelType, selectValue string) (DecoderSpec, bool) {
	for _, d := range Decoders {
		if d.ModelType == modelType && d.SelectValue == selectValue {
			return d, true
		}
	}
	return DecoderSpec{}, false
}
