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
			"non-string function rejected",
			`{"backend_function":123,"iou_threshold":0.45,"detection_threshold":0.5,` +
				`"output_activation":"none","label_offset":1,"max_boxes":32,"labels":[]}`,
			"is not supported",
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
