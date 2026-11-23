package postprocess

import (
	"reflect"
	"testing"
)

// The registry is the single source of truth for two generated artifacts
// (schema JSON + the ai-runtime C++ header) and for the model package's
// profile tables. These tests pin the invariants a regeneration must keep.

func TestDetectionVariantKeysClosedSet(t *testing.T) {
	want := []string{
		"backend_function",
		"iou_threshold",
		"detection_threshold",
		"output_activation",
		"label_offset",
		"max_boxes",
		"labels",
	}
	if !reflect.DeepEqual(DetectionVariantKeys, want) {
		t.Fatalf("DetectionVariantKeys = %v, want %v (must match the vendor plugin's closed schema verbatim)", DetectionVariantKeys, want)
	}
}

func TestKeypointVariantKeys(t *testing.T) {
	want := []string{
		"native_yolov8_pose",
		"score_threshold",
		"confidence_threshold",
		"keypoint_threshold",
		"iou_threshold",
		"num_keypoints",
		"yolov8_pose_network_width",
		"yolov8_pose_network_height",
	}
	if !reflect.DeepEqual(KeypointVariantKeys, want) {
		t.Fatalf("KeypointVariantKeys = %v, want %v", KeypointVariantKeys, want)
	}
}

func TestForbiddenVariantKeys(t *testing.T) {
	want := []string{"backend_lib_path", "backend_config_path"}
	if !reflect.DeepEqual(ForbiddenVariantKeys, want) {
		t.Fatalf("ForbiddenVariantKeys = %v, want %v", ForbiddenVariantKeys, want)
	}
}

func TestRuntimeTypesMatchRuntime(t *testing.T) {
	// Verbatim match with the ai-runtime known-type table
	// (model_variant_validation.cpp). A change here must be paired with an
	// init_post_process change and a schema regeneration.
	want := []string{
		"detection", "yolo", "landmarks", "keypoint", "segmentation",
		"classification", "clip", "embedding", "depth", "monocular_depth",
		"scdepth", "ocr_detection", "ocr_recognition",
	}
	if !reflect.DeepEqual(RuntimeTypes, want) {
		t.Fatalf("RuntimeTypes = %v, want %v", RuntimeTypes, want)
	}
}

func TestParamDefsAnnotated(t *testing.T) {
	// "integer" is number with an integrality contract (REST/C++ value-shape
	// checks and ValidateModelConfig all enforce it); the wizard's step-1
	// number fields are its schema spelling.
	validTypes := map[string]bool{"number": true, "integer": true, "string": true, "boolean": true, "string_list": true}
	for _, d := range Decoders {
		for _, p := range d.Params {
			if !validTypes[p.Type] {
				t.Errorf("%s/%s param %s: invalid type %q", d.ModelType, d.Decoder, p.Key, p.Type)
			}
			switch p.Mutability {
			case MutabilityCreateOnly, MutabilityHotUpdate, MutabilityRequiresReload:
			default:
				t.Errorf("%s/%s param %s: missing/invalid mutability %q", d.ModelType, d.Decoder, p.Key, p.Mutability)
			}
			switch p.Effect {
			case EffectConsumed, EffectAdvisory, EffectMetadata:
			default:
				t.Errorf("%s/%s param %s: missing/invalid effect %q", d.ModelType, d.Decoder, p.Key, p.Effect)
			}
			if p.Min != nil && p.Max != nil && *p.Min > *p.Max {
				t.Errorf("%s/%s param %s: min %v > max %v", d.ModelType, d.Decoder, p.Key, *p.Min, *p.Max)
			}
		}
	}
}

func TestDetectionDecodersCoverBlobSchema(t *testing.T) {
	dets := DecodersForType("detection")
	if len(dets) != 4 {
		t.Fatalf("detection decoders = %d, want 4", len(dets))
	}
	for _, d := range dets {
		if d.Basename == "" {
			t.Errorf("detection decoder %s: missing basename", d.Decoder)
		}
		if d.Selection != SelectionBackendFunction {
			t.Errorf("detection decoder %s: Selection = %q, want %q", d.Decoder, d.Selection, SelectionBackendFunction)
		}
		if len(d.Params) != len(DetectionVariantKeys) {
			t.Errorf("detection decoder %s: %d params, want the %d blob keys", d.Decoder, len(d.Params), len(DetectionVariantKeys))
		}
	}
}

func TestKeypointDecoders(t *testing.T) {
	kps := DecodersForType("keypoint")
	if len(kps) != 2 {
		t.Fatalf("keypoint decoders = %d, want 2 (facial + pose)", len(kps))
	}
	facial, ok := LookupSelectValue("keypoint", "facial_landmarks")
	if !ok {
		t.Fatal("keypoint decoder for select value facial_landmarks not found")
	}
	if len(facial.Params) != 0 {
		t.Errorf("facial decoder has %d params, want 0 (mediapipe reads no configuration)", len(facial.Params))
	}
	if facial.Selection != SelectionEmptyVariant {
		t.Errorf("facial Selection = %q, want %q", facial.Selection, SelectionEmptyVariant)
	}
	pose, ok := LookupSelectValue("keypoint", "yolov8_pose")
	if !ok {
		t.Fatal("keypoint decoder for select value yolov8_pose not found")
	}
	if pose.Selection != "flag:native_yolov8_pose" {
		t.Errorf("pose Selection = %q, want flag:native_yolov8_pose", pose.Selection)
	}
	keys := map[string]bool{}
	for _, p := range pose.Params {
		keys[p.Key] = true
	}
	for _, k := range KeypointVariantKeys {
		if !keys[k] {
			t.Errorf("pose decoder params missing key %s", k)
		}
	}
	// num_keypoints is advisory for pose: the built-in decoder hardcodes 17.
	for _, p := range pose.Params {
		if p.Key == "num_keypoints" && p.Effect != EffectAdvisory {
			t.Errorf("pose num_keypoints effect = %q, want advisory (COCO-17 hardcoded)", p.Effect)
		}
		if (p.Key == "yolov8_pose_network_width" || p.Key == "yolov8_pose_network_height") && p.Mutability != MutabilityCreateOnly {
			t.Errorf("pose %s mutability = %q, want create_only (read at create, absent from both hot-update key tables)", p.Key, p.Mutability)
		}
	}
}

func TestSelectValuesUniquePerType(t *testing.T) {
	seen := map[string]string{}
	for _, d := range Decoders {
		if d.SelectValue == "" {
			continue
		}
		key := d.ModelType + "/" + d.SelectValue
		if prev, dup := seen[key]; dup {
			t.Errorf("duplicate SelectValue %s for %s (also %s)", d.SelectValue, d.ModelType, prev)
		}
		seen[key] = d.Decoder
	}
}

func TestLookupDecoder(t *testing.T) {
	if _, ok := LookupDecoder("detection", "hailo_yolov8n"); !ok {
		t.Error("LookupDecoder(detection, hailo_yolov8n) = false")
	}
	if _, ok := LookupDecoder("detection", "hailo_yolov9x"); ok {
		t.Error("LookupDecoder(detection, hailo_yolov9x) = true, want false")
	}
	if _, ok := LookupDecoder("keypoint", "native_yolov8_pose"); !ok {
		t.Error("LookupDecoder(keypoint, native_yolov8_pose) = false")
	}
}

// The fixed-name entry consumes nothing beyond its routing key
// (device-verified 2026-09-02: baked "car" label table, unread knobs), so
// every non-routing param must read advisory — the generated artifacts and
// the wizard's effect badges inherit this row (review 2026-09-21).
func TestFixedNameDecoderParamsAllAdvisory(t *testing.T) {
	d, ok := LookupDecoder("detection", "yolov5m_vehicles")
	if !ok {
		t.Fatal("fixed-name decoder row missing")
	}
	for _, p := range d.Params {
		want := EffectConsumed
		if p.Key != "backend_function" {
			want = EffectAdvisory
		}
		if p.Effect != want {
			t.Errorf("%s param %s effect = %q, want %q", d.Decoder, p.Key, p.Effect, want)
		}
	}
}
