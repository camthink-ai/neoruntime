package server

import (
	"context"
	"encoding/json"
	"os"
	"path/filepath"
	"strings"
	"testing"

	inferencepb "aipc/platform/ai-runtime/proto"
	"aipc/platform/app-manager/manifest"
	"aipc/platform/platform-api/model"
)

// PreloadModels must register platform.db models through the same load-time
// composition platform-api's LoadModel uses — the reboot path this file pins.
// Before that sharing, preload handed the runtime the raw CAS blob path with
// an empty variant, silently degrading detection models to bare tensors.

// newPreloadEnv wires a validation server whose platform.db is seeded against
// a scratch root (rows may reference files materialized under it).
func newPreloadEnv(t *testing.T, client *stubInferenceClient, seed func(root string) map[string]model.AIModel) (*AppManagerServer, string) {
	t.Helper()
	root := withTempRoot(t)
	s := newValidationServer(client, true)
	if seed != nil {
		if rows := seed(root); len(rows) > 0 {
			s.db = newModelMetaDB(t, rows)
		}
	}
	return s, root
}

func writePreloadBlob(t *testing.T, root string) string {
	t.Helper()
	blob := filepath.Join(root, "models", "blobs", "deadbeef.hef")
	if err := os.MkdirAll(filepath.Dir(blob), 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	if err := os.WriteFile(blob, []byte("blob-bytes"), 0o644); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	return blob
}

func preloadManifest(ids ...string) *manifest.AppManifest {
	return &manifest.AppManifest{Spec: manifest.Spec{
		Permissions: manifest.Permissions{Inference: manifest.InferencePerms{Models: ids}},
	}}
}

func probeInputs(id string) *inferencepb.ModelInfo {
	return &inferencepb.ModelInfo{ModelId: id, Inputs: []*inferencepb.TensorSpec{{
		Shape: []int32{1, 384, 640, 3}, Dtype: inferencepb.DataType_UINT8, ByteSize: 384 * 640 * 3 / 2,
	}}}
}

func TestPreloadModelsComposesDetectionRegistration(t *testing.T) {
	client := &stubInferenceClient{infos: map[string]*inferencepb.ModelInfo{
		"fire_smoke": probeInputs("fire_smoke"),
	}}
	s, root := newPreloadEnv(t, client, func(root string) map[string]model.AIModel {
		return map[string]model.AIModel{
			"fire_smoke": {ModelType: "detection", FilePath: writePreloadBlob(t, root), Threshold: 0.4, MaxDetections: 32},
		}
	})

	s.PreloadModels(context.Background(), "app-x", preloadManifest("fire_smoke"))

	if len(client.registrations) != 1 {
		t.Fatalf("registrations = %+v, want exactly one", client.registrations)
	}
	reg := client.registrations[0]
	wantPath := filepath.Join(root, "models", "runtime", "fire_smoke", "hailo_yolov8n_384_640.hef")
	if reg.ModelId != "fire_smoke" || reg.OwnerId != "app-x" || reg.ModelType != "detection" || reg.Transient {
		t.Errorf("registration = %+v, want fire_smoke/app-x/detection/non-transient", reg)
	}
	if reg.ModelPath != wantPath {
		t.Errorf("path = %q, want materialized %q", reg.ModelPath, wantPath)
	}
	if body, err := os.ReadFile(wantPath); err != nil || string(body) != "blob-bytes" {
		t.Errorf("materialized copy wrong: %q err=%v", body, err)
	}
	var variant map[string]interface{}
	if err := json.Unmarshal([]byte(reg.ModelVariant), &variant); err != nil {
		t.Fatalf("variant is not JSON: %v (%q)", err, reg.ModelVariant)
	}
	if variant["backend_function"] != "hailo_yolov8n" || variant["detection_threshold"] != 0.4 || variant["max_boxes"] != float64(32) {
		t.Errorf("variant tuning not carried: %v", variant)
	}
	// Fresh registration: the load smoke probe ran and passed, no rollback.
	if len(client.inferCalls) != 1 || client.inferCalls[0] != "fire_smoke" {
		t.Errorf("inferCalls = %v, want one fire_smoke probe", client.inferCalls)
	}
	if len(client.unregistered) != 0 {
		t.Errorf("unregistered = %+v, want none on probe success", client.unregistered)
	}
}

func TestPreloadModelsRawModelSkipsComposition(t *testing.T) {
	client := &stubInferenceClient{}
	var blob string
	s, root := newPreloadEnv(t, client, func(root string) map[string]model.AIModel {
		blob = writePreloadBlob(t, root)
		return map[string]model.AIModel{
			"raw_det": {ModelType: "detection", OutputMode: "raw", FilePath: blob, Variant: "stale-junk"},
		}
	})

	s.PreloadModels(context.Background(), "app-x", preloadManifest("raw_det"))

	if len(client.registrations) != 1 {
		t.Fatalf("registrations = %+v, want one", client.registrations)
	}
	reg := client.registrations[0]
	if reg.ModelPath != blob || reg.ModelType != "" || reg.ModelVariant != "" {
		t.Errorf("registration = %+v, want raw passthrough (path=%q, empty type/variant)", reg, blob)
	}
	if _, err := os.Stat(filepath.Join(root, "models", "runtime")); !os.IsNotExist(err) {
		t.Errorf("raw model must not be materialized (stat err=%v)", err)
	}
	if len(client.inferCalls) != 0 || len(client.unregistered) != 0 {
		t.Errorf("inferCalls=%v unregistered=%v, want neither (no tensor info configured)", client.inferCalls, client.unregistered)
	}
}

func TestPreloadModelsNonDetectionPassesThrough(t *testing.T) {
	// Probe inputs plus a poison Infer failure prove the smoke test never
	// runs for non-detection models.
	client := &stubInferenceClient{
		infos:     map[string]*inferencepb.ModelInfo{"cls1": probeInputs("cls1")},
		inferFail: "probe must not run for classification models",
	}
	var blob string
	s, _ := newPreloadEnv(t, client, func(root string) map[string]model.AIModel {
		blob = writePreloadBlob(t, root)
		return map[string]model.AIModel{
			"cls1": {ModelType: "classification", FilePath: blob, Variant: "resnet18"},
		}
	})

	s.PreloadModels(context.Background(), "app-x", preloadManifest("cls1"))

	if len(client.registrations) != 1 {
		t.Fatalf("registrations = %+v, want one", client.registrations)
	}
	reg := client.registrations[0]
	if reg.ModelPath != blob || reg.ModelType != "classification" || reg.ModelVariant != "resnet18" {
		t.Errorf("registration = %+v, want passthrough (path=%q classification/resnet18)", reg, blob)
	}
	if len(client.inferCalls) != 0 {
		t.Errorf("inferCalls = %v, want none for non-detection models", client.inferCalls)
	}
	if len(client.unregistered) != 0 {
		t.Errorf("unregistered = %+v, want none", client.unregistered)
	}
}

func TestPreloadModelsSmokeFailureRollsBackFreshRegistration(t *testing.T) {
	client := &stubInferenceClient{
		infos:     map[string]*inferencepb.ModelInfo{"fire_smoke": probeInputs("fire_smoke")},
		inferFail: "init_post_process failed: no NMS tensor for backend function",
	}
	s, _ := newPreloadEnv(t, client, func(root string) map[string]model.AIModel {
		return map[string]model.AIModel{
			"fire_smoke": {ModelType: "detection", FilePath: writePreloadBlob(t, root)},
		}
	})

	s.PreloadModels(context.Background(), "app-x", preloadManifest("fire_smoke"))

	// The registration was attempted, probed, found broken, and rolled back.
	if len(client.registrations) != 1 || client.registrations[0].ModelId != "fire_smoke" {
		t.Fatalf("registrations = %+v, want the attempted fire_smoke registration", client.registrations)
	}
	if len(client.inferCalls) != 1 || client.inferCalls[0] != "fire_smoke" {
		t.Errorf("inferCalls = %v, want one fire_smoke probe", client.inferCalls)
	}
	if len(client.unregistered) != 1 || client.unregistered[0].ModelId != "fire_smoke" || client.unregistered[0].OwnerId != "app-x" {
		t.Errorf("unregistered = %+v, want fire_smoke rolled back for owner app-x", client.unregistered)
	}
}

func TestPreloadModelsPreexistingRegistrationIsNotProbed(t *testing.T) {
	// The model is already live in the runtime (someone else's registration):
	// preload only adds this app as a co-owner. Even a poison Infer failure
	// must not probe or roll back a registration this process did not create.
	client := &stubInferenceClient{
		models:    []*inferencepb.ModelInfo{loadedModel("fire_smoke")},
		infos:     map[string]*inferencepb.ModelInfo{"fire_smoke": probeInputs("fire_smoke")},
		inferFail: "preexisting models must not be probed",
	}
	s, _ := newPreloadEnv(t, client, func(root string) map[string]model.AIModel {
		return map[string]model.AIModel{
			"fire_smoke": {ModelType: "detection", FilePath: writePreloadBlob(t, root)},
		}
	})

	s.PreloadModels(context.Background(), "app-x", preloadManifest("fire_smoke"))

	if len(client.registrations) != 1 || client.registrations[0].OwnerId != "app-x" {
		t.Fatalf("registrations = %+v, want the co-ownership registration for app-x", client.registrations)
	}
	if len(client.inferCalls) != 0 {
		t.Errorf("inferCalls = %v, want none for preexisting models", client.inferCalls)
	}
	if len(client.unregistered) != 0 {
		t.Errorf("unregistered = %+v, want none for preexisting models", client.unregistered)
	}
}

func TestPreloadModelsCompositionFailureSkipsRegistration(t *testing.T) {
	client := &stubInferenceClient{}
	s, _ := newPreloadEnv(t, client, func(root string) map[string]model.AIModel {
		return map[string]model.AIModel{
			"gone_det": {ModelType: "detection", FilePath: filepath.Join(root, "models", "blobs", "missing.hef")},
		}
	})

	s.PreloadModels(context.Background(), "app-x", preloadManifest("gone_det"))

	if len(client.registrations) != 0 {
		t.Errorf("registrations = %+v, want none when materialization fails", client.registrations)
	}
	if len(client.inferCalls) != 0 || len(client.unregistered) != 0 {
		t.Errorf("inferCalls=%v unregistered=%v, want neither", client.inferCalls, client.unregistered)
	}
}

func TestPreloadModelsRestoresRawBundledModelWithOptIn(t *testing.T) {
	// Reboot restore of a bundled output_mode=raw package: the sidecar's
	// empty ModelType must reach the runtime together with the explicit
	// raw_output_only opt-in, or the transient gate rejects the restore and
	// the app's model silently never comes back after a reboot.
	client := &stubInferenceClient{}
	s, root := newPreloadEnv(t, client, nil)

	aliasDir := filepath.Join(root, "app-models", "app-x", "rawdet")
	if err := os.MkdirAll(aliasDir, 0o755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	if err := os.WriteFile(filepath.Join(aliasDir, "raw.hef"), []byte("hef-bytes"), 0o644); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	sidecar, err := json.Marshal(bundledRegistration{ModelID: "bundled_raw", HEF: "raw.hef", RawOutputOnly: true})
	if err != nil {
		t.Fatalf("Marshal: %v", err)
	}
	if err := os.WriteFile(filepath.Join(aliasDir, bundledRegistrationFile), sidecar, 0o644); err != nil {
		t.Fatalf("WriteFile sidecar: %v", err)
	}

	m := preloadManifest("bundled_raw")
	m.Spec.Models = map[string]manifest.ModelMapping{
		"rawdet": {ID: "bundled_raw", Path: "/app/models/raw.bin"},
	}
	s.PreloadModels(context.Background(), "app-x", m)

	if len(client.registrations) != 1 {
		t.Fatalf("registrations = %+v, want one bundled restore", client.registrations)
	}
	reg := client.registrations[0]
	if reg.ModelId != "bundled_raw" || reg.OwnerId != "app-x" || !reg.Transient {
		t.Errorf("registration = %+v, want bundled_raw/app-x/transient", reg)
	}
	if reg.ModelType != "" || !reg.RawOutputOnly {
		t.Errorf("registration = %+v, want empty type with raw_output_only=true", reg)
	}
}

func TestPreloadModelsBundledLogicalFailureHonorsRequired(t *testing.T) {
	setup := func(t *testing.T, required bool) (*AppManagerServer, *stubInferenceClient, *manifest.AppManifest) {
		t.Helper()
		client := &stubInferenceClient{regStatus: map[string]*inferencepb.Status{
			"bundled_det": {Success: false, Message: "model id collision: different configuration"},
		}}
		s, root := newPreloadEnv(t, client, nil)
		aliasDir := filepath.Join(root, "app-models", "app-x", "detector")
		if err := os.MkdirAll(aliasDir, 0o755); err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(aliasDir, "det.hef"), []byte("hef"), 0o644); err != nil {
			t.Fatal(err)
		}
		sidecar, err := json.Marshal(bundledRegistration{
			ModelID: "bundled_det", HEF: "det.hef", ModelType: "detection",
		})
		if err != nil {
			t.Fatal(err)
		}
		if err := os.WriteFile(filepath.Join(aliasDir, bundledRegistrationFile), sidecar, 0o644); err != nil {
			t.Fatal(err)
		}
		m := preloadManifest("bundled_det")
		m.Spec.Models = map[string]manifest.ModelMapping{
			"detector": {ID: "bundled_det", Path: "/app/models/det.bin", Required: required},
		}
		return s, client, m
	}

	t.Run("required_refusal_aborts_start_precondition", func(t *testing.T) {
		s, client, m := setup(t, true)
		err := s.PreloadModels(context.Background(), "app-x", m)
		if err == nil || !strings.Contains(err.Error(), "different configuration") {
			t.Fatalf("PreloadModels error = %v, want logical refusal", err)
		}
		if len(client.inferCalls) != 0 {
			t.Errorf("refused registration must not be smoke-probed, got %v", client.inferCalls)
		}
	})

	t.Run("optional_refusal_warns_but_does_not_abort", func(t *testing.T) {
		s, client, m := setup(t, false)
		if err := s.PreloadModels(context.Background(), "app-x", m); err != nil {
			t.Fatalf("optional refusal must not abort: %v", err)
		}
		if len(client.inferCalls) != 0 {
			t.Errorf("refused optional registration must not be probed, got %v", client.inferCalls)
		}
	})
}
