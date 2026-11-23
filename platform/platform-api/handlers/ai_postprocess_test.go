package handlers

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
)

// Registration-time guard tests. The load-time composition tests
// (variant composition, materialization, smoke-test tensor sizing) moved to
// platform/modelload/runtime_test.go along with the code they cover.

func TestCopyFile(t *testing.T) {
	dir := t.TempDir()
	src := filepath.Join(dir, "src.hef")
	dst := filepath.Join(dir, "dst.hef")
	if err := os.WriteFile(src, []byte("src-body"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := copyFile(src, dst); err != nil {
		t.Fatalf("copyFile: %v", err)
	}
	body, err := os.ReadFile(dst)
	if err != nil || string(body) != "src-body" {
		t.Fatalf("copy mismatch: %q err=%v", body, err)
	}
	// Overwrite an existing dst with new content.
	if err := os.WriteFile(src, []byte("src-body-2"), 0644); err != nil {
		t.Fatal(err)
	}
	if err := copyFile(src, dst); err != nil {
		t.Fatalf("copyFile overwrite: %v", err)
	}
	body, _ = os.ReadFile(dst)
	if string(body) != "src-body-2" {
		t.Fatalf("overwrite mismatch: %q", body)
	}
}

// completeVariantJSON is a schema-valid custom variant blob — the bar every
// user-supplied `{…}` variant must clear before it may be stored.
func completeVariantJSON(fn string) string {
	return `{"backend_function":"` + fn + `","iou_threshold":0.45,"detection_threshold":0.5,` +
		`"output_activation":"none","label_offset":1,"max_boxes":32,"labels":["fire","smoke"]}`
}

func TestValidateDetectionVariant(t *testing.T) {
	tests := []struct {
		name    string
		variant string
		wantErr string // empty means must pass
	}{
		{"empty variant", "", ""},
		{"plugin basename passes through", "hailo_yolov8s", ""},
		{"complete n function", completeVariantJSON("hailo_yolov8n"), ""},
		{"complete s function", completeVariantJSON("hailo_yolov8s"), ""},
		{"complete m function", completeVariantJSON("hailo_yolov8m"), ""},
		{
			"invalid json",
			`{"backend_function":`,
			"not valid JSON",
		},
		{
			"missing keys",
			`{"backend_function":"hailo_yolov8n","detection_threshold":0.5}`,
			"missing required key(s): iou_threshold, output_activation, label_offset, max_boxes, labels",
		},
		{
			"generic function rejected",
			completeVariantJSON("yolov8s"),
			`backend_function "yolov8s" is not supported`,
		},
		{
			// A non-string backend_function now trips the value-type check
			// (more precise) before the whitelist lookup does.
			"non-string function rejected",
			`{"backend_function":123,"iou_threshold":0.45,"detection_threshold":0.5,` +
				`"output_activation":"none","label_offset":1,"max_boxes":32,"labels":[]}`,
			`variant key "backend_function" must be a string`,
		},
		{
			// The REST surface must stay as closed as the AMPK package
			// boundary: HAL reads backend_lib_path from this same config JSON
			// and dlopens whatever it names, and a `{…}` variant travels to
			// the runtime verbatim.
			"backend_lib_path rejected",
			strings.TrimSuffix(completeVariantJSON("hailo_yolov8n"), "}") +
				`,"backend_lib_path":"/tmp/evil.so"}`,
			"unsupported key(s): backend_lib_path",
		},
		{
			"backend_config_path rejected",
			strings.TrimSuffix(completeVariantJSON("hailo_yolov8n"), "}") +
				`,"backend_config_path":"/tmp/evil.json"}`,
			"unsupported key(s): backend_config_path",
		},
		{
			"arbitrary extra key rejected",
			strings.TrimSuffix(completeVariantJSON("hailo_yolov8s"), "}") +
				`,"zsl":true}`,
			"unsupported key(s): zsl",
		},
		{
			"multiple unknown keys listed sorted",
			strings.TrimSuffix(completeVariantJSON("hailo_yolov8m"), "}") +
				`,"zz_extra":1,"aa_extra":2}`,
			"unsupported key(s): aa_extra, zz_extra",
		},
		{
			// The value-type half of the P0: labels is a legal KEY, so the
			// key-set check passes — but an object value smuggles loader keys
			// structurally into the blob HAL substring-searches.
			"labels as object rejected",
			strings.Replace(completeVariantJSON("hailo_yolov8n"),
				`"labels":["fire","smoke"]`,
				`"labels":{"backend_lib_path":"/tmp/evil.so"}`, 1),
			`variant key "labels" must be an array of strings`,
		},
		{
			"labels array with non-string element rejected",
			strings.Replace(completeVariantJSON("hailo_yolov8n"),
				`"labels":["fire","smoke"]`, `"labels":["fire",7]`, 1),
			`variant key "labels" must contain only strings`,
		},
		{
			"threshold as string rejected",
			strings.Replace(completeVariantJSON("hailo_yolov8n"),
				`"detection_threshold":0.5`, `"detection_threshold":"0.5"`, 1),
			`variant key "detection_threshold" must be a number`,
		},
		{
			"max_boxes as fraction rejected",
			strings.Replace(completeVariantJSON("hailo_yolov8n"),
				`"max_boxes":32`, `"max_boxes":32.5`, 1),
			`variant key "max_boxes" must be an integer`,
		},
		{
			"label_offset as string rejected",
			strings.Replace(completeVariantJSON("hailo_yolov8s"),
				`"label_offset":1`, `"label_offset":"1"`, 1),
			`variant key "label_offset" must be an integer`,
		},
		{
			"output_activation as number rejected",
			strings.Replace(completeVariantJSON("hailo_yolov8s"),
				`"output_activation":"none"`, `"output_activation":0`, 1),
			`variant key "output_activation" must be a string`,
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := validateDetectionVariant(tt.variant)
			if tt.wantErr == "" {
				if err != nil {
					t.Fatalf("validateDetectionVariant(%q) = %v, want nil", tt.variant, err)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tt.wantErr) {
				t.Fatalf("validateDetectionVariant(%q) = %v, want error containing %q", tt.variant, err, tt.wantErr)
			}
		})
	}
}

// TestValidatePostprocessProfile pins the write-boundary semantics: only a
// detection row in platform mode with a present-but-wrong postprocess_profile
// is rejected. Non-detection models, raw output mode, and absent keys keep
// the legacy silent-default semantics — a model whose profile is "not set"
// is not a typo.
func TestValidatePostprocessProfile(t *testing.T) {
	tests := []struct {
		name       string
		modelType  string
		outputMode string
		cfg        map[string]interface{}
		wantErr    string // empty means must pass
	}{
		{"non-detection ignores profile", "classification", "platform",
			map[string]interface{}{"postprocess_profile": "yolov8x_640_640"}, ""},
		{"raw output ignores profile", "detection", "raw",
			map[string]interface{}{"postprocess_profile": "yolov8x_640_640"}, ""},
		{"absent key falls back to default", "detection", "platform",
			map[string]interface{}{}, ""},
		{"valid profile passes", "detection", "platform",
			map[string]interface{}{"postprocess_profile": "hailo_yolov8s_384_640"}, ""},
		{"empty output mode resolves to platform", "detection", "",
			map[string]interface{}{"postprocess_profile": "yolov8x_640_640"}, "postprocess_profile"},
		{"unknown profile rejected", "detection", "platform",
			map[string]interface{}{"postprocess_profile": "yolov8x_640_640"}, "postprocess_profile"},
		{"non-string value rejected", "detection", "platform",
			map[string]interface{}{"postprocess_profile": 42}, "postprocess_profile"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := validatePostprocessProfile(tt.modelType, tt.outputMode, tt.cfg)
			if tt.wantErr == "" {
				if err != nil {
					t.Fatalf("validatePostprocessProfile(%q, %q, %v) = %v, want nil", tt.modelType, tt.outputMode, tt.cfg, err)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tt.wantErr) {
				t.Fatalf("validatePostprocessProfile(%q, %q, %v) = %v, want error containing %q", tt.modelType, tt.outputMode, tt.cfg, err, tt.wantErr)
			}
		})
	}
}

// The keypoint dialect is open by design (legacy opaque channel — HAL owns
// content validation), with exactly one hard refusal: the loader control
// keys the postprocess layer dlopens from. Everything else a hand-written
// pose blob may carry passes.
func TestValidateKeypointVariant(t *testing.T) {
	// Shape of the vendor's own example blob
	// (hal_v2/examples/ai_example_v2/data/yolov8_pose_native_post.example.json).
	example := `{"native_yolov8_pose":true,"score_threshold":0.6,"keypoint_threshold":0.5,` +
		`"yolov8_pose_network_width":640,"yolov8_pose_network_height":640}`
	tests := []struct {
		name    string
		variant string
		wantErr string // empty means must pass
	}{
		{"empty variant passes", "", ""},
		{"vendor example blob passes", example, ""},
		{"leading whitespace tolerated", "  " + example, ""},
		{
			// Open dialect: unknown ordinary keys travel opaquely to HAL.
			"unknown ordinary key passes",
			`{"native_yolov8_pose":true,"some_future_key":7}`,
			"",
		},
		{
			"backend_lib_path rejected",
			`{"native_yolov8_pose":true,"backend_lib_path":"/tmp/evil.so"}`,
			`variant key "backend_lib_path" is never accepted`,
		},
		{
			// The P0 regression: the old check only scanned top-level keys,
			// but HAL substring-searches the raw JSON, so a loader key nested
			// inside an ordinary value reached dlopen through every layer.
			"nested backend_lib_path rejected",
			`{"native_yolov8_pose":true,"extra":{"backend_lib_path":"/tmp/evil.so"}}`,
			`variant key "extra.backend_lib_path" is never accepted`,
		},
		{
			"nested backend_config_path rejected",
			`{"meta":{"backend_config_path":"/tmp/evil.json"}}`,
			`variant key "meta.backend_config_path" is never accepted`,
		},
		{
			// Arrays are containers too — depth alone is not the defense,
			// structure is.
			"loader key inside array element rejected",
			`{"native_yolov8_pose":true,"presets":[{"backend_lib_path":"/tmp/e.so"}]}`,
			`variant key "presets[0].backend_lib_path" is never accepted`,
		},
		{
			"deeply nested loader key rejected",
			`{"a":{"b":{"c":{"backend_lib_path":"/tmp/evil.so"}}}}`,
			`variant key "a.b.c.backend_lib_path" is never accepted`,
		},
		{
			// The escaped-quote analysis: a loader key spelled inside a
			// STRING value cannot match HAL's raw search (quotes are escaped
			// as \" inside the JSON text), so it is not a smuggling channel
			// and must not be refused.
			"loader-like text inside a string value passes",
			`{"note":"set backend_lib_path to your plugin"}`,
			"",
		},
		{
			"backend_config_path rejected",
			`{"native_yolov8_pose":true,"backend_config_path":"/tmp/evil.json"}`,
			`variant key "backend_config_path" is never accepted`,
		},
		{
			// Same message shape the runtime's own validator answers with.
			"bare decoder name rejected",
			"yolov8s_pose",
			"bare decoder names are rejected",
		},
		{
			"invalid json rejected",
			`{"native_yolov8_pose":`,
			"not valid JSON",
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := validateKeypointVariant(tt.variant)
			if tt.wantErr == "" {
				if err != nil {
					t.Fatalf("validateKeypointVariant(%q) = %v, want nil", tt.variant, err)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tt.wantErr) {
				t.Fatalf("validateKeypointVariant(%q) = %v, want error containing %q", tt.variant, err, tt.wantErr)
			}
		})
	}
}

func TestValidateVariantJSONDispatch(t *testing.T) {
	// detection keeps its closed set; keypoint its open dialect with the
	// loader blacklist; every other type runs the same loader-key refusal
	// (review 2026-09-24 P0 — the pass-through let loader keys through for
	// types with no schema at this boundary).
	if err := validateVariantJSON("detection", `{"backend_function":"hailo_yolov8n"}`); err == nil {
		t.Error("detection partial blob must be rejected by the closed-set check")
	}
	if err := validateVariantJSON("keypoint", `{"backend_lib_path":"/tmp/evil.so"}`); err == nil {
		t.Error("keypoint loader key must be rejected")
	}
	if err := validateVariantJSON("keypoint", `{"whatever":1}`); err != nil {
		t.Errorf("keypoint unknown ordinary key must pass (opaque channel): %v", err)
	}
	// Alias spellings route to the same dialects.
	if err := validateVariantJSON("landmarks", "yolov8s_pose"); err == nil {
		t.Error("landmarks alias must reach the keypoint bare-name rejection")
	}
	if err := validateVariantJSON("yolo", `{"backend_lib_path":"/tmp/evil.so"}`); err == nil {
		t.Error("yolo alias must reach the detection closed set")
	}
	if err := validateVariantJSON("classification", `{"backend_lib_path":"/tmp/evil.so"}`); err == nil {
		t.Error("classification loader key must be rejected by the default-branch walk")
	}
}

// The default branch of validateVariantJSON covers every model type that has
// no dialect of its own: the only refusal is the any-depth loader-key walk,
// applied per type (review 2026-09-24 P0 — these types used to pass
// unchecked all the way to the HAL plugin loader's config surface).
func TestValidateVariantJSONDefaultBranchLoaderKeys(t *testing.T) {
	otherTypes := []string{
		"segmentation", "classification", "clip", "embedding", "depth",
		"monocular_depth", "scdepth", "ocr_detection", "ocr_recognition",
	}
	tests := []struct {
		name    string
		variant string
		wantErr string // empty means must pass
	}{
		{"empty variant passes", "", ""},
		{"bare name passes (no dialect to select)", "some_decoder", ""},
		{"ordinary blob passes", `{"threshold":0.3,"prompts":["a cat"]}`, ""},
		{
			// Not a smuggling channel: quotes are escaped inside a JSON
			// string value, so HAL's raw search cannot match it.
			"loader-like text inside a string value passes",
			`{"note":"set backend_lib_path to your plugin"}`,
			"",
		},
		{
			"root backend_lib_path rejected",
			`{"threshold":0.3,"backend_lib_path":"/tmp/evil.so"}`,
			`variant key "backend_lib_path" is never accepted`,
		},
		{
			"root backend_config_path rejected",
			`{"backend_config_path":"/tmp/evil.json"}`,
			`variant key "backend_config_path" is never accepted`,
		},
		{
			"nested object loader key rejected",
			`{"extra":{"backend_lib_path":"/tmp/evil.so"}}`,
			`variant key "extra.backend_lib_path" is never accepted`,
		},
		{
			"loader key inside array element rejected",
			`{"presets":[{"backend_config_path":"/tmp/evil.json"}]}`,
			`variant key "presets[0].backend_config_path" is never accepted`,
		},
		{
			"deeply nested loader key rejected",
			`{"a":{"b":{"c":{"backend_lib_path":"/tmp/evil.so"}}}}`,
			`variant key "a.b.c.backend_lib_path" is never accepted`,
		},
		{
			// Fail-closed, mirroring the keypoint surface: a malformed
			// object is refused instead of silently stored.
			"invalid json rejected",
			`{"threshold":`,
			"not valid JSON",
		},
	}
	for _, modelType := range otherTypes {
		for _, tt := range tests {
			t.Run(modelType+"/"+tt.name, func(t *testing.T) {
				err := validateVariantJSON(modelType, tt.variant)
				if tt.wantErr == "" {
					if err != nil {
						t.Fatalf("validateVariantJSON(%s, %q) = %v, want nil", modelType, tt.variant, err)
					}
					return
				}
				if err == nil || !strings.Contains(err.Error(), tt.wantErr) {
					t.Fatalf("validateVariantJSON(%s, %q) = %v, want error containing %q", modelType, tt.variant, err, tt.wantErr)
				}
			})
		}
	}
}

// Keypoint rows join detection in guarding the postprocess_profile key: a
// typo'd pose profile would silently register the facial decoder.
func TestValidatePostprocessProfileKeypoint(t *testing.T) {
	tests := []struct {
		name       string
		modelType  string
		outputMode string
		cfg        map[string]interface{}
		wantErr    string // empty means must pass
	}{
		{"absent key falls back to facial", "keypoint", "platform",
			map[string]interface{}{}, ""},
		{"facial profile passes", "keypoint", "platform",
			map[string]interface{}{"postprocess_profile": "facial_landmarks"}, ""},
		{"pose profile passes", "keypoint", "platform",
			map[string]interface{}{"postprocess_profile": "yolov8_pose"}, ""},
		{"landmarks alias passes", "landmarks", "platform",
			map[string]interface{}{"postprocess_profile": "yolov8_pose"}, ""},
		{"raw output ignores profile", "keypoint", "raw",
			map[string]interface{}{"postprocess_profile": "openpose"}, ""},
		{"unknown profile rejected", "keypoint", "platform",
			map[string]interface{}{"postprocess_profile": "openpose"}, "openpose"},
		{
			// Detection basenames and keypoint profiles share the config key
			// but not the value space — crossing them is a typo, not a
			// passthrough.
			"detection basename rejected for keypoint",
			"keypoint", "platform",
			map[string]interface{}{"postprocess_profile": "hailo_yolov8n_384_640"},
			"hailo_yolov8n_384_640",
		},
		{"keypoint profile rejected for detection", "detection", "platform",
			map[string]interface{}{"postprocess_profile": "yolov8_pose"}, "yolov8_pose"},
		{"non-string value rejected", "keypoint", "platform",
			map[string]interface{}{"postprocess_profile": true}, "postprocess_profile"},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			err := validatePostprocessProfile(tt.modelType, tt.outputMode, tt.cfg)
			if tt.wantErr == "" {
				if err != nil {
					t.Fatalf("validatePostprocessProfile(%q, %q, %v) = %v, want nil", tt.modelType, tt.outputMode, tt.cfg, err)
				}
				return
			}
			if err == nil || !strings.Contains(err.Error(), tt.wantErr) {
				t.Fatalf("validatePostprocessProfile(%q, %q, %v) = %v, want error containing %q", tt.modelType, tt.outputMode, tt.cfg, err, tt.wantErr)
			}
		})
	}
}
