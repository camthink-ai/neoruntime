package modelload

import (
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"reflect"
	"strings"
	"testing"

	inferencepb "aipc/platform/ai-runtime/proto"
	"aipc/platform/common/constants"
	"aipc/platform/platform-api/model"
)

// newPostprocessTestEnv points the platform root at a temp dir so
// materialized runtime copies land in a scratch models tree.
func newPostprocessTestEnv(t *testing.T) (string, func()) {
	t.Helper()
	oldRoot := constants.RootPath()
	root := t.TempDir()
	constants.SetRootPath(root)
	return root, func() { constants.SetRootPath(oldRoot) }
}

func writeHEF(t *testing.T, path, body string) {
	t.Helper()
	if err := os.MkdirAll(filepath.Dir(path), 0755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	if err := os.WriteFile(path, []byte(body), 0644); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
}

func TestDetectionPostprocessProfile(t *testing.T) {
	tests := []struct {
		name    string
		cfg     string
		want    string
		wantErr bool
	}{
		{"empty config", "", model.DefaultDetectionProfile, false},
		{"valid s profile", `{"postprocess_profile":"hailo_yolov8s_384_640"}`, "hailo_yolov8s_384_640", false},
		{"unknown profile", `{"postprocess_profile":"yolov8x_640_640"}`, "", true},
		{"invalid json", "{not json", model.DefaultDetectionProfile, false},
		{"wrong value type", `{"postprocess_profile":42}`, "", true},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			m := &model.AIModel{Config: tt.cfg}
			got, err := DetectionPostprocessProfile(m)
			if tt.wantErr {
				if err == nil {
					t.Fatalf("DetectionPostprocessProfile() = %q, want error", got)
				}
				return
			}
			if err != nil {
				t.Fatalf("DetectionPostprocessProfile() unexpected error: %v", err)
			}
			if got != tt.want {
				t.Fatalf("DetectionPostprocessProfile() = %q, want %q", got, tt.want)
			}
		})
	}
}

// defaultVariantLabels mirrors the plugin's compiled-in table shape: index 0
// is a placeholder so labels[N] names class_id N.
var defaultVariantLabels = []interface{}{"unlabeled", "person", "vehicle", "face", "license_plate"}

func TestDetectionVariantJSON(t *testing.T) {
	verbatim := `{"backend_function":"hailo_yolov8s","label_offset":0}`
	tests := []struct {
		name     string
		m        *model.AIModel
		want     map[string]interface{} // used unless passthru
		passthru bool                   // expect exact passthrough of m.Variant
	}{
		{
			name: "default profile composes full schema-valid blob",
			m:    &model.AIModel{ModelType: "detection", Threshold: 0.4, MaxDetections: 32},
			want: map[string]interface{}{
				"backend_function":    "hailo_yolov8n",
				"iou_threshold":       0.45,
				"detection_threshold": 0.4,
				"output_activation":   "none",
				"label_offset":        float64(1),
				"max_boxes":           float64(32),
				"labels":              defaultVariantLabels,
			},
		},
		{
			name: "explicit s profile with labels and nms from config",
			m: &model.AIModel{
				ModelType: "detection",
				Config:    `{"postprocess_profile":"hailo_yolov8s_384_640","labels":"fire,smoke","nms_threshold":0.6}`,
			},
			want: map[string]interface{}{
				"backend_function":    "hailo_yolov8s",
				"iou_threshold":       0.6,
				"detection_threshold": 0.25,
				"output_activation":   "none",
				"label_offset":        float64(1),
				"max_boxes":           float64(64),
				"labels":              []interface{}{"unlabeled", "fire", "smoke"},
			},
		},
		{
			name: "legacy zero tuning values without config fall back to schema defaults",
			m:    &model.AIModel{ModelType: "detection"},
			want: map[string]interface{}{
				"backend_function":    "hailo_yolov8n",
				"iou_threshold":       0.45,
				"detection_threshold": 0.25,
				"output_activation":   "none",
				"label_offset":        float64(1),
				"max_boxes":           float64(64),
				"labels":              defaultVariantLabels,
			},
		},
		{
			name: "explicit zero confidence and nms thresholds are preserved",
			m: &model.AIModel{
				ModelType: "detection",
				Threshold: 0,
				Config:    `{"threshold":0,"nms_threshold":0}`,
			},
			want: map[string]interface{}{
				"backend_function":    "hailo_yolov8n",
				"iou_threshold":       float64(0),
				"detection_threshold": float64(0),
				"output_activation":   "none",
				"label_offset":        float64(1),
				"max_boxes":           float64(64),
				"labels":              defaultVariantLabels,
			},
		},
		{
			name:     "json variant passes through verbatim",
			m:        &model.AIModel{ModelType: "detection", Variant: verbatim},
			passthru: true,
		},
		{
			name:     "non-detection variant unchanged",
			m:        &model.AIModel{ModelType: "classification", Variant: "resnet18"},
			passthru: true,
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got, err := DetectionVariantJSON(tt.m)
			if err != nil {
				t.Fatalf("DetectionVariantJSON() unexpected error: %v", err)
			}
			if tt.passthru {
				if got != tt.m.Variant {
					t.Fatalf("DetectionVariantJSON() = %q, want passthrough %q", got, tt.m.Variant)
				}
				return
			}
			var parsed map[string]interface{}
			if err := json.Unmarshal([]byte(got), &parsed); err != nil {
				t.Fatalf("composed variant is not valid JSON: %v (%q)", err, got)
			}
			if !reflect.DeepEqual(parsed, tt.want) {
				t.Fatalf("DetectionVariantJSON() = %v, want %v", parsed, tt.want)
			}
		})
	}
}

func TestRuntimeRegistration(t *testing.T) {
	root, restore := newPostprocessTestEnv(t)
	defer restore()

	blob := filepath.Join(root, "models", "blobs", "deadbeef.hef")
	writeHEF(t, blob, "blob-bytes")

	builtin := filepath.Join(root, "models", "detection", "hailo_yolov8n_384_640.hef")
	writeHEF(t, builtin, "builtin-bytes")

	t.Run("non-detection passes through", func(t *testing.T) {
		m := &model.AIModel{ModelID: "cls1", ModelType: "classification", FilePath: blob, Variant: "resnet18"}
		path, variant, grpcType, err := RuntimeRegistration(m)
		if err != nil {
			t.Fatalf("RuntimeRegistration: %v", err)
		}
		if path != blob || variant != "resnet18" || grpcType != "classification" {
			t.Fatalf("got (%q, %q, %q), want passthrough (%q, %q, %q)", path, variant, grpcType, blob, "resnet18", "classification")
		}
	})

	t.Run("raw mode skips postprocess session", func(t *testing.T) {
		m := &model.AIModel{ModelID: "raw_det", ModelType: "detection", OutputMode: "raw", FilePath: blob, Variant: "whatever"}
		path, variant, grpcType, err := RuntimeRegistration(m)
		if err != nil {
			t.Fatalf("RuntimeRegistration: %v", err)
		}
		if path != blob || variant != "" || grpcType != "" {
			t.Fatalf("got (%q, %q, %q), want (%q, \"\", \"\") for raw mode", path, variant, grpcType, blob)
		}
	})

	t.Run("builtin profile basename passes path through", func(t *testing.T) {
		m := &model.AIModel{ModelID: "det_builtin", ModelType: "detection", FilePath: builtin, Threshold: 0.3}
		path, variant, _, err := RuntimeRegistration(m)
		if err != nil {
			t.Fatalf("RuntimeRegistration: %v", err)
		}
		if path != builtin {
			t.Fatalf("path = %q, want builtin path %q unchanged", path, builtin)
		}
		var parsed map[string]interface{}
		if err := json.Unmarshal([]byte(variant), &parsed); err != nil {
			t.Fatalf("variant not JSON: %v", err)
		}
		if parsed["backend_function"] != "hailo_yolov8n" {
			t.Fatalf("backend_function = %v, want hailo_yolov8n", parsed["backend_function"])
		}
	})

	t.Run("blob is materialized under profile name", func(t *testing.T) {
		m := &model.AIModel{
			ModelID: "fire_smoke", ModelType: "detection",
			FilePath: blob, Threshold: 0.4, MaxDetections: 32,
		}
		path, variant, _, err := RuntimeRegistration(m)
		if err != nil {
			t.Fatalf("RuntimeRegistration: %v", err)
		}
		want := filepath.Join(root, "models", "runtime", "fire_smoke", "hailo_yolov8n_384_640.hef")
		if path != want {
			t.Fatalf("path = %q, want %q", path, want)
		}
		srcStat, err := os.Stat(blob)
		if err != nil {
			t.Fatal(err)
		}
		dstStat, err := os.Stat(path)
		if err != nil {
			t.Fatal(err)
		}
		if !os.SameFile(srcStat, dstStat) {
			t.Fatal("runtime copy is not a link to the blob (different inodes)")
		}
		var parsed map[string]interface{}
		if err := json.Unmarshal([]byte(variant), &parsed); err != nil {
			t.Fatalf("variant not JSON: %v", err)
		}
		if parsed["detection_threshold"] != 0.4 || parsed["max_boxes"] != float64(32) {
			t.Fatalf("tuning not carried into variant: %v", parsed)
		}
	})

	t.Run("stored profile selects matching basename", func(t *testing.T) {
		m := &model.AIModel{
			ModelID: "fire_smoke", ModelType: "detection", FilePath: blob,
			Config: `{"postprocess_profile":"hailo_yolov8s_384_640"}`,
		}
		path, _, _, err := RuntimeRegistration(m)
		if err != nil {
			t.Fatalf("RuntimeRegistration: %v", err)
		}
		if !strings.HasSuffix(path, "hailo_yolov8s_384_640.hef") {
			t.Fatalf("path = %q, want hailo_yolov8s_384_640.hef suffix", path)
		}
	})

	t.Run("unknown postprocess_profile fails load", func(t *testing.T) {
		m := &model.AIModel{
			ModelID: "typo_profile", ModelType: "detection", FilePath: blob,
			Config: `{"postprocess_profile":"yolov8x_640_640"}`,
		}
		if _, _, _, err := RuntimeRegistration(m); err == nil {
			t.Fatal("RuntimeRegistration: want error for unknown postprocess_profile, got nil")
		}
	})

	t.Run("stale runtime copy is replaced atomically", func(t *testing.T) {
		dir := filepath.Join(root, "models", "runtime", "stale_model")
		stale := filepath.Join(dir, "hailo_yolov8n_384_640.hef")
		writeHEF(t, stale, "stale-bytes")

		m := &model.AIModel{ModelID: "stale_model", ModelType: "detection", FilePath: blob}
		path, _, _, err := RuntimeRegistration(m)
		if err != nil {
			t.Fatalf("RuntimeRegistration: %v", err)
		}
		body, err := os.ReadFile(path)
		if err != nil {
			t.Fatal(err)
		}
		if string(body) != "blob-bytes" {
			t.Fatalf("runtime copy body = %q, want fresh %q", body, "blob-bytes")
		}
		entries, err := os.ReadDir(dir)
		if err != nil {
			t.Fatal(err)
		}
		for _, e := range entries {
			if strings.Contains(e.Name(), ".tmp-") {
				t.Fatalf("staged temp file left behind: %s", e.Name())
			}
		}
	})

	t.Run("unsafe model id rejected", func(t *testing.T) {
		for _, id := range []string{"../evil", "a/b", ".hidden", "a b", ""} {
			m := &model.AIModel{ModelID: id, ModelType: "detection", FilePath: blob}
			if _, _, _, err := RuntimeRegistration(m); err == nil {
				t.Errorf("model id %q: expected error, got none", id)
			}
		}
	})
}

// TestMaterializeReplacesWithoutClobber pins the cross-process safety of the
// tmp+rename publish: platform-api LoadModel and app-manager PreloadModels can
// materialize the same model concurrently, and the previous remove-then-link /
// truncate-copy sequence could clobber the shared inode (the CAS blob itself)
// of a hardlink another process just published. Replacing dst must swap the
// directory entry only — every other link to the old inode keeps its bytes,
// and the source's bytes are never touched.
func TestMaterializeReplacesWithoutClobber(t *testing.T) {
	dir := t.TempDir()
	src := filepath.Join(dir, "blob.hef")
	writeHEF(t, src, "src-bytes")

	// A previously published runtime copy, hardlinked from another blob.
	oldBlob := filepath.Join(dir, "old-blob.hef")
	writeHEF(t, oldBlob, "old-bytes")
	dst := filepath.Join(dir, "runtime.hef")
	if err := os.Link(oldBlob, dst); err != nil {
		t.Fatal(err)
	}

	if err := materialize(src, dst); err != nil {
		t.Fatalf("materialize: %v", err)
	}

	dstBody, err := os.ReadFile(dst)
	if err != nil {
		t.Fatal(err)
	}
	if string(dstBody) != "src-bytes" {
		t.Fatalf("dst body = %q, want %q", dstBody, "src-bytes")
	}
	// The old inode survives intact through its other link.
	oldBody, err := os.ReadFile(oldBlob)
	if err != nil {
		t.Fatal(err)
	}
	if string(oldBody) != "old-bytes" {
		t.Fatalf("old inode clobbered via its other link: %q, want %q", oldBody, "old-bytes")
	}
	// The source is untouched.
	srcBody, err := os.ReadFile(src)
	if err != nil {
		t.Fatal(err)
	}
	if string(srcBody) != "src-bytes" {
		t.Fatalf("source blob rewritten: %q", srcBody)
	}
	// No staged temp files remain next to dst.
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		if strings.Contains(e.Name(), ".tmp-") {
			t.Fatalf("staged temp file left behind: %s", e.Name())
		}
	}
}

// TestMaterializeCopyFallback exercises the copy branch used when a hardlink
// cannot be staged (source on a foreign filesystem). Cross-device links are
// hard to stage portably, so the fallback is driven through copyInto directly.
func TestMaterializeCopyFallback(t *testing.T) {
	dir := t.TempDir()
	src := filepath.Join(dir, "src.hef")
	writeHEF(t, src, "copy-me")
	tmp := filepath.Join(dir, "staged")
	if err := copyInto(src, tmp); err != nil {
		t.Fatalf("copyInto: %v", err)
	}
	body, err := os.ReadFile(tmp)
	if err != nil || string(body) != "copy-me" {
		t.Fatalf("copy mismatch: %q err=%v", body, err)
	}
}

// TestMaterializeSurvivesStaleStagedHardlink pins the device incident: an
// attempt killed between link and rename leaves a staged temp that is itself a
// hardlink to the stored blob. The next attempt must publish through its own
// unique temp name and leave the blob's bytes intact — the old pid-only temp
// name let the retry collide, fall back to the truncating copy, and zero the
// shared inode (blob, runtime copy and stale temps collapsing to one 0-byte
// file).
func TestMaterializeSurvivesStaleStagedHardlink(t *testing.T) {
	dir := t.TempDir()
	src := filepath.Join(dir, "blob.hef")
	writeHEF(t, src, "blob-bytes")
	dst := filepath.Join(dir, "runtime.hef")

	// A crashed earlier attempt left this behind: a hardlink to the blob.
	stale := fmt.Sprintf("%s.tmp-%d", dst, os.Getpid())
	if err := os.Link(src, stale); err != nil {
		t.Fatal(err)
	}

	if err := materialize(src, dst); err != nil {
		t.Fatalf("materialize: %v", err)
	}
	dstBody, err := os.ReadFile(dst)
	if err != nil || string(dstBody) != "blob-bytes" {
		t.Fatalf("dst body = %q err=%v, want published blob bytes", dstBody, err)
	}
	srcBody, err := os.ReadFile(src)
	if err != nil || string(srcBody) != "blob-bytes" {
		t.Fatalf("stored blob destroyed via stale staged hardlink: %q err=%v", srcBody, err)
	}
}

// TestMaterializeIdempotentRerunLeavesNoDebris pins the heal-flow shape: load
// composes the registration twice back-to-back (stale-check plus the real
// registration), so the second materialize finds dst already hardlinked to the
// stored blob. Re-staging would hit rename()'s same-inode no-op and strand a
// temp name per heal; the short-circuit must leave nothing behind.
func TestMaterializeIdempotentRerunLeavesNoDebris(t *testing.T) {
	dir := t.TempDir()
	src := filepath.Join(dir, "blob.hef")
	writeHEF(t, src, "blob-bytes")
	dst := filepath.Join(dir, "runtime.hef")

	for i := 0; i < 2; i++ {
		if err := materialize(src, dst); err != nil {
			t.Fatalf("materialize pass %d: %v", i+1, err)
		}
	}
	dstBody, err := os.ReadFile(dst)
	if err != nil || string(dstBody) != "blob-bytes" {
		t.Fatalf("dst body = %q err=%v, want published blob bytes", dstBody, err)
	}
	srcBody, err := os.ReadFile(src)
	if err != nil || string(srcBody) != "blob-bytes" {
		t.Fatalf("stored blob rewritten: %q err=%v", srcBody, err)
	}
	entries, err := os.ReadDir(dir)
	if err != nil {
		t.Fatal(err)
	}
	for _, e := range entries {
		if strings.Contains(e.Name(), ".tmp-") {
			t.Fatalf("idempotent re-run stranded staging debris: %s", e.Name())
		}
	}
}

// TestCopyIntoRefusesExistingTarget pins the structural guard: copyInto must
// never open an existing path — O_TRUNC on a target that happens to be a
// hardlink to the source is exactly the blob-destroying write.
func TestCopyIntoRefusesExistingTarget(t *testing.T) {
	dir := t.TempDir()
	src := filepath.Join(dir, "blob.hef")
	writeHEF(t, src, "blob-bytes")
	existing := filepath.Join(dir, "existing.hef")
	if err := os.Link(src, existing); err != nil {
		t.Fatal(err)
	}

	if err := copyInto(src, existing); err == nil {
		t.Fatal("copyInto must refuse to open an existing target")
	}
	srcBody, err := os.ReadFile(src)
	if err != nil || string(srcBody) != "blob-bytes" {
		t.Fatalf("shared inode truncated through copy target: %q err=%v", srcBody, err)
	}
}

func TestRemoveRuntimeCopy(t *testing.T) {
	root, restore := newPostprocessTestEnv(t)
	defer restore()

	dir := filepath.Join(root, "models", "runtime", "fire_smoke")
	writeHEF(t, filepath.Join(dir, "hailo_yolov8n_384_640.hef"), "x")

	RemoveRuntimeCopy("fire_smoke")
	if _, err := os.Stat(dir); !os.IsNotExist(err) {
		t.Fatalf("runtime dir still exists after RemoveRuntimeCopy (err=%v)", err)
	}

	// Unknown ids and ids unsafe as path segments must be no-ops, not panics.
	RemoveRuntimeCopy("never-existed")
	RemoveRuntimeCopy("../evil")
	if _, err := os.Stat(filepath.Join(root, "models")); err != nil {
		t.Fatalf("models root disturbed by unsafe-id call: %v", err)
	}
}

func TestZeroInputTensor(t *testing.T) {
	spec := &inferencepb.TensorSpec{Shape: []int32{1, 384, 640, 2}, Dtype: inferencepb.DataType_UINT8}
	tensor, err := zeroInputTensor(spec)
	if err != nil {
		t.Fatalf("zeroInputTensor: %v", err)
	}
	if want := 1 * 384 * 640 * 2; len(tensor.Data) != want {
		t.Fatalf("data len = %d, want %d", len(tensor.Data), want)
	}
	if tensor.Dtype != spec.Dtype || !reflect.DeepEqual(tensor.Shape, spec.Shape) {
		t.Fatalf("tensor spec not mirrored: %+v", tensor)
	}

	// NV12 inputs report an RGB-like shape (features=3) whose product is not
	// the host frame size — the runtime's byte_size is authoritative there.
	nv12 := &inferencepb.TensorSpec{
		Shape:    []int32{1, 384, 640, 3},
		Dtype:    inferencepb.DataType_UINT8,
		ByteSize: 384 * 640 * 3 / 2,
	}
	tensor, err = zeroInputTensor(nv12)
	if err != nil {
		t.Fatalf("zeroInputTensor nv12: %v", err)
	}
	if want := 384 * 640 * 3 / 2; len(tensor.Data) != want {
		t.Fatalf("nv12 data len = %d, want byte_size %d", len(tensor.Data), want)
	}

	floatSpec := &inferencepb.TensorSpec{Shape: []int32{1, 10}, Dtype: inferencepb.DataType_FLOAT32}
	tensor, err = zeroInputTensor(floatSpec)
	if err != nil {
		t.Fatalf("zeroInputTensor float: %v", err)
	}
	if want := 1 * 10 * 4; len(tensor.Data) != want {
		t.Fatalf("float data len = %d, want %d", len(tensor.Data), want)
	}

	bad := &inferencepb.TensorSpec{Shape: []int32{1, 0, 640}, Dtype: inferencepb.DataType_UINT8}
	if _, err := zeroInputTensor(bad); err == nil {
		t.Fatal("expected error for non-positive shape dim")
	}
}
