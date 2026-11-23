package model

import (
	"slices"
	"testing"

	"aipc/platform/postprocess"
)

func TestLookupDetectionProfile(t *testing.T) {
	p, ok := LookupDetectionProfile("hailo_yolov8s_384_640")
	if !ok || p.BackendFunction != "hailo_yolov8s" {
		t.Fatalf("lookup s profile = %+v ok=%v", p, ok)
	}
	// Device-verified 2026-09-02: the parking-lot model maps 1:1 (its
	// backend function is the network name itself).
	p, ok = LookupDetectionProfile("yolov5m_vehicles")
	if !ok || p.BackendFunction != "yolov5m_vehicles" {
		t.Fatalf("lookup vehicles profile = %+v ok=%v", p, ok)
	}
	if _, ok := LookupDetectionProfile(DefaultDetectionProfile); !ok {
		t.Fatalf("default profile %q missing from table", DefaultDetectionProfile)
	}
	if _, ok := LookupDetectionProfile("yolov8x_640_640"); ok {
		t.Fatal("unknown profile resolved")
	}
}

func TestDetectionFieldsIncludePostprocessControls(t *testing.T) {
	td := GetModelTypeDef("detection")
	if td == nil {
		t.Fatal("detection type def missing")
	}

	var profile *ModelFieldDef
	var labels *ModelFieldDef
	for i := range td.Fields {
		switch td.Fields[i].Key {
		case "postprocess_profile":
			profile = &td.Fields[i]
		case "labels":
			labels = &td.Fields[i]
		}
	}
	if profile == nil {
		t.Fatal("postprocess_profile field missing from detection schema")
	}
	if profile.Type != FieldTypeSelect {
		t.Fatalf("postprocess_profile type = %q, want select", profile.Type)
	}
	if profile.Default != DefaultDetectionProfile {
		t.Fatalf("postprocess_profile default = %v, want %q", profile.Default, DefaultDetectionProfile)
	}
	if len(profile.Options) != len(DetectionPostprocessProfiles) {
		t.Fatalf("postprocess_profile options = %d, want %d", len(profile.Options), len(DetectionPostprocessProfiles))
	}
	for _, opt := range profile.Options {
		p, ok := LookupDetectionProfile(opt.Value)
		if !ok {
			t.Fatalf("option %q not present in DetectionPostprocessProfiles", opt.Value)
		}
		if opt.Custom != p.Custom {
			t.Fatalf("option %q custom = %v, want %v (flag must propagate)", opt.Value, opt.Custom, p.Custom)
		}
	}

	// The customer-specific basename is the only custom entry: it stays
	// selectable (load-time resolution is unchanged) but the generic UI
	// hides it unless it is already the active value.
	customCount := 0
	for _, opt := range profile.Options {
		if opt.Custom {
			customCount++
			if opt.Value != "yolov5m_vehicles" {
				t.Fatalf("unexpected custom option %q", opt.Value)
			}
		}
	}
	if customCount != 1 {
		t.Fatalf("custom options = %d, want exactly 1 (yolov5m_vehicles)", customCount)
	}

	if labels == nil {
		t.Fatal("labels field missing from detection schema")
	}
	if labels.Type != FieldTypeText {
		t.Fatalf("labels type = %q, want text", labels.Type)
	}
	// Device A/B 2026-09-20 (93.72, hailo_yolov8n_384_640): changing the
	// labels config relabeled live post_result output (person/face →
	// zzz_foo/yyy_bar), so for the parameterized decoders the key is
	// consumed — the form must not badge it metadata/advisory. (Effect's
	// zero value is consumed.) The fixed-name entries (yolov5m_vehicles)
	// bake their table; that per-decoder split lives in the registry rows.
	if labels.Effect == postprocess.EffectAdvisory || labels.Effect == postprocess.EffectMetadata {
		t.Fatalf("labels effect = %q, want consumed (device A/B 2026-09-20)", labels.Effect)
	}
}

func TestGetFieldDefaultsIncludesPostprocessControls(t *testing.T) {
	defaults := GetFieldDefaults("detection")
	if defaults == nil {
		t.Fatal("no defaults for detection")
	}
	if defaults["postprocess_profile"] != DefaultDetectionProfile {
		t.Fatalf("postprocess_profile default = %v, want %q", defaults["postprocess_profile"], DefaultDetectionProfile)
	}
	if _, ok := defaults["labels"]; !ok {
		t.Fatal("labels key missing from detection defaults")
	}
}

// The profile table must be derived from the postprocess registry — the S2
// invariant that makes the registry the single source of truth.
func TestDetectionProfilesDerivedFromRegistry(t *testing.T) {
	dcs := postprocess.DecodersForType("detection")
	if len(dcs) != len(DetectionPostprocessProfiles) {
		t.Fatalf("detection profiles = %d, registry decoders = %d", len(DetectionPostprocessProfiles), len(dcs))
	}
	for i, p := range DetectionPostprocessProfiles {
		d := dcs[i]
		if p.Basename != d.Basename || p.BackendFunction != d.Decoder ||
			p.Label != d.Label || p.Custom != d.Custom {
			t.Errorf("profile[%d] = %+v, want %+v derived from registry decoder %+v",
				i, p, DetectionPostprocessProfile{Basename: d.Basename, BackendFunction: d.Decoder, Label: d.Label, Custom: d.Custom}, d)
		}
	}
}

func TestKeypointFieldsIncludePostprocessControls(t *testing.T) {
	td := GetModelTypeDef("keypoint")
	if td == nil {
		t.Fatal("keypoint type def missing")
	}

	var profile *ModelFieldDef
	numFields := map[string]*ModelFieldDef{}
	for i := range td.Fields {
		f := &td.Fields[i]
		switch f.Key {
		case "postprocess_profile":
			profile = f
		case "threshold", "keypoint_threshold":
			numFields[f.Key] = f
		case "num_keypoints":
			t.Fatal("num_keypoints must not appear in the form schema (hardcoded 468 facial / 17 pose)")
		}
	}
	if profile == nil {
		t.Fatal("postprocess_profile field missing from keypoint schema")
	}
	if profile.Type != FieldTypeSelect {
		t.Fatalf("postprocess_profile type = %q, want select", profile.Type)
	}
	if profile.Default != DefaultKeypointProfile {
		t.Fatalf("postprocess_profile default = %v, want %q", profile.Default, DefaultKeypointProfile)
	}
	if len(profile.Profiles) != 0 {
		t.Fatalf("postprocess_profile must render for every profile, got Profiles=%v", profile.Profiles)
	}
	if len(profile.Options) != len(postprocess.DecodersForType("keypoint")) {
		t.Fatalf("postprocess_profile options = %d, want %d", len(profile.Options), len(postprocess.DecodersForType("keypoint")))
	}
	for _, opt := range profile.Options {
		if _, ok := LookupKeypointProfile(opt.Value); !ok {
			t.Fatalf("option %q not present in KeypointPostprocessProfiles", opt.Value)
		}
	}
	if _, ok := LookupKeypointProfile("hailo_yolov8s_384_640"); ok {
		t.Fatal("detection basename leaked into keypoint profiles")
	}

	// Both numeric knobs exist only under the pose profile; the facial
	// decoder reads no configuration at all.
	for _, key := range []string{"threshold", "keypoint_threshold"} {
		f, ok := numFields[key]
		if !ok {
			t.Fatalf("%s field missing from keypoint schema", key)
		}
		if !slices.Equal(f.Profiles, []string{KeypointProfilePose}) {
			t.Fatalf("%s Profiles = %v, want [%s]", key, f.Profiles, KeypointProfilePose)
		}
	}

	// HAL silently falls back to 0.6 when score_threshold < 1e-6, so the
	// form must not offer 0.
	if th := numFields["threshold"]; th == nil || th.Min == nil || *th.Min != 0.01 {
		t.Fatalf("threshold min must be 0.01 (HAL treats <1e-6 as unset and falls back to 0.6), got %+v", numFields["threshold"])
	}
}

func TestGetFieldDefaultsKeypoint(t *testing.T) {
	defaults := GetFieldDefaults("keypoint")
	if defaults == nil {
		t.Fatal("no defaults for keypoint")
	}
	if defaults["postprocess_profile"] != DefaultKeypointProfile {
		t.Fatalf("postprocess_profile default = %v, want %q", defaults["postprocess_profile"], DefaultKeypointProfile)
	}
	if _, ok := defaults["num_keypoints"]; ok {
		t.Fatal("num_keypoints must not be seeded into new keypoint rows")
	}
}

func TestLookupKeypointProfile(t *testing.T) {
	if p, ok := LookupKeypointProfile(KeypointProfileFacial); !ok || p.Decoder != "facial_landmarks_nv12" {
		t.Fatalf("facial lookup = %+v ok=%v", p, ok)
	}
	if p, ok := LookupKeypointProfile(KeypointProfilePose); !ok || p.Decoder != "native_yolov8_pose" {
		t.Fatalf("pose lookup = %+v ok=%v", p, ok)
	}
	if _, ok := LookupKeypointProfile("openpose"); ok {
		t.Fatal("unknown keypoint profile resolved")
	}
}

func TestLoadProbeWorthy(t *testing.T) {
	for _, id := range []string{"detection", "yolo", "keypoint", "landmarks"} {
		if !LoadProbeWorthy(id) {
			t.Errorf("LoadProbeWorthy(%q) = false, want true", id)
		}
	}
	for _, id := range []string{"classification", "clip", "segmentation", "genai", ""} {
		if LoadProbeWorthy(id) {
			t.Errorf("LoadProbeWorthy(%q) = true, want false", id)
		}
	}
}

// Keypoint identity must win over the generic yolo/det prefix: the pose
// networks ship as yolov8*_pose, and a detection suggestion sends the wizard
// down the wrong profile path (P1 step-1 acceptance). "face" is deliberately
// NOT hoisted: face_detection/face_detector are detection networks and must
// keep the detection suggestion — only the landmark-ish spellings
// (face_landmarks_lite, face_mesh) land on keypoint via the late case.
func TestGuessModelTypeKeypointPrecedence(t *testing.T) {
	cases := map[string]string{
		"yolov8s_pose":          "keypoint",
		"yolov8n_pose":          "keypoint",
		"face_landmarks_lite":   "keypoint",
		"face_mesh":             "keypoint",
		"facial_landmarks":      "keypoint",
		"yolov8n":               "detection",
		"hailo_yolov8n_384_640": "detection",
		"yolov5m_vehicles":      "detection",
		// Conflict names (review 2026-09-21): a bare "face" token must not
		// outrank the det/yolo prefixes.
		"face_detection": "detection",
		"face_detector":  "detection",
		"yolov5m_face":   "detection",
		"retinaface_det": "detection",
		// Names with no det/yolo token keep the pre-hoist suggestion
		// (late "face" case): keypoint.
		"face_box_reg": "keypoint",
	}
	for name, want := range cases {
		if got := GuessModelType(name); got != want {
			t.Errorf("GuessModelType(%q) = %q, want %q", name, got, want)
		}
	}
}

// CLIP identity must win over the generic classification tokens: the vision
// encoders ship as clip_vit_b_32_*, and the "vit" token in the classification
// case would otherwise suggest classification for every ViT-named CLIP
// network (found on-device 2026-09-26: clip_vit_b_32_image_encoder parsed as
// classification). Pure ViT classifiers keep the classification suggestion.
func TestGuessModelTypeClipPrecedence(t *testing.T) {
	cases := map[string]string{
		"clip_vit_b_32_image_encoder": "clip",
		"clip_vit_l_14":               "clip",
		"clip_text_encoder":           "clip",
		// Controls: without the clip token the generic vit/cls tokens win.
		"vit_b_32":         "classification",
		"mobilenet_v2_cls": "classification",
	}
	for name, want := range cases {
		if got := GuessModelType(name); got != want {
			t.Errorf("GuessModelType(%q) = %q, want %q", name, got, want)
		}
	}
}

// RuntimeTypes is the registry's own list of gRPC-known type strings; the Go
// wizard table must cover it (via ids or aliases) and, minus genai (no
// runtime postprocess path), must not miss entries either.
func TestRuntimeTypesConsistency(t *testing.T) {
	goNames := map[string]bool{}
	for _, td := range SupportedModelTypes {
		goNames[td.ID] = true
		for _, a := range td.Aliases {
			goNames[a] = true
		}
	}
	for _, rt := range postprocess.RuntimeTypes {
		if !goNames[rt] {
			t.Errorf("RuntimeTypes entry %q not covered by the Go wizard table (add it as an id or alias)", rt)
		}
	}
	for _, td := range SupportedModelTypes {
		if td.ID == "genai" {
			continue
		}
		if !slices.Contains(postprocess.RuntimeTypes, td.ID) {
			t.Errorf("Go model type %q missing from RuntimeTypes (unknown types silently fall to the detection default in init_post_process)", td.ID)
		}
	}
}

// The field-level annotation is the DEFAULT profile's truth; per-decoder
// divergence must surface through ProfileParamEffects keyed by the profile
// select value — that is the P1-d fix: yolov5m_vehicles bakes its label
// table and reads none of the knobs, so threshold/max_detections/labels are
// advisory THERE while staying consumed under the parameterized entries.
func TestProfileParamEffectsDetection(t *testing.T) {
	td := GetModelTypeDef("detection")
	if td == nil {
		t.Fatal("detection type def missing")
	}
	if len(td.ProfileParamEffects) != len(DetectionPostprocessProfiles) {
		t.Fatalf("profile_param_effects entries = %d, want %d (one per decoder)", len(td.ProfileParamEffects), len(DetectionPostprocessProfiles))
	}

	def, ok := td.ProfileParamEffects[DefaultDetectionProfile]
	if !ok {
		t.Fatalf("default profile %q missing from profile_param_effects", DefaultDetectionProfile)
	}
	for key, want := range map[string]postprocess.Effect{
		"threshold":        postprocess.EffectConsumed,
		"max_detections":   postprocess.EffectConsumed,
		"nms_threshold":    postprocess.EffectAdvisory,
		"labels":           postprocess.EffectConsumed,
		"backend_function": postprocess.EffectConsumed,
	} {
		if def[key] != want {
			t.Errorf("default profile %s effect = %q, want %q", key, def[key], want)
		}
	}

	vehicles, ok := td.ProfileParamEffects["yolov5m_vehicles"]
	if !ok {
		t.Fatal("yolov5m_vehicles missing from profile_param_effects")
	}
	for key, want := range map[string]postprocess.Effect{
		"threshold":      postprocess.EffectAdvisory,
		"max_detections": postprocess.EffectAdvisory,
		"labels":         postprocess.EffectAdvisory,
	} {
		if vehicles[key] != want {
			t.Errorf("yolov5m_vehicles %s effect = %q, want %q (fixed-name entry reads none of the knobs)", key, vehicles[key], want)
		}
	}
}

// Keypoint's two profiles diverge too: facial consumes nothing (no params at
// all — hence no entry), pose consumes both thresholds. The field-level
// annotation stays consumed because both numeric fields only render under
// pose (Profiles == [yolov8_pose]).
func TestProfileParamEffectsKeypoint(t *testing.T) {
	td := GetModelTypeDef("keypoint")
	if td == nil {
		t.Fatal("keypoint type def missing")
	}
	if _, ok := td.ProfileParamEffects[KeypointProfileFacial]; ok {
		t.Fatalf("facial profile must have no overrides (decoder reads no config), got %v", td.ProfileParamEffects[KeypointProfileFacial])
	}
	pose, ok := td.ProfileParamEffects[KeypointProfilePose]
	if !ok {
		t.Fatalf("pose profile %q missing from profile_param_effects", KeypointProfilePose)
	}
	for key, want := range map[string]postprocess.Effect{
		"threshold":          postprocess.EffectConsumed,
		"keypoint_threshold": postprocess.EffectConsumed,
	} {
		if pose[key] != want {
			t.Errorf("pose profile %s effect = %q, want %q", key, pose[key], want)
		}
	}
}

// Single-decoder types take the registry row as their effect truth: the
// hand-written field table predates the registry and had no way to know the
// linknet decoder ignores the threshold knob.
func TestSingleDecoderEffectOverwrite(t *testing.T) {
	td := GetModelTypeDef("segmentation")
	if td == nil {
		t.Fatal("segmentation type def missing")
	}
	for _, f := range td.Fields {
		if f.Key == "threshold" && f.Effect != postprocess.EffectAdvisory {
			t.Fatalf("segmentation threshold effect = %q, want advisory (linknet ignores the knob)", f.Effect)
		}
	}
}

// ValidateModelConfig is the write-boundary gate (P1-c): what gets persisted
// must be what the pipeline would honor, instead of storing garbage that is
// silently dropped from the composed blob at load time.
func TestValidateModelConfig(t *testing.T) {
	cases := []struct {
		name      string
		modelType string
		cfg       map[string]interface{}
		wantErr   bool
	}{
		{"empty map passes", "detection", map[string]interface{}{}, false},
		{"unknown type passes", "no_such_type", map[string]interface{}{"threshold": 9}, false},
		{"unknown key passes (opaque vendor metadata)", "detection", map[string]interface{}{"vendor_extra": "x"}, false},
		{"threshold 0 is legal (schema min)", "detection", map[string]interface{}{"threshold": 0.0}, false},
		{"threshold above max rejected", "detection", map[string]interface{}{"threshold": 1.5}, true},
		{"threshold as string rejected", "detection", map[string]interface{}{"threshold": "0.5"}, true},
		{"max_detections fractional rejected", "detection", map[string]interface{}{"max_detections": 32.5}, true},
		{"labels as number rejected", "detection", map[string]interface{}{"labels": 80}, true},
		{"typo'd profile rejected even off-mode", "detection", map[string]interface{}{"postprocess_profile": "yolov8x_640_640"}, true},
		{"keypoint dims fractional rejected", "keypoint", map[string]interface{}{"yolov8_pose_network_width": 640.9}, true},
		{"keypoint dims integer passes", "keypoint", map[string]interface{}{"yolov8_pose_network_width": 640.0}, false},
		{"keypoint dims below min rejected", "keypoint", map[string]interface{}{"yolov8_pose_network_height": 8}, true},
		{"keypoint dims above max rejected", "keypoint", map[string]interface{}{"yolov8_pose_network_width": 8192}, true},
		{"pose threshold below HAL floor rejected", "keypoint", map[string]interface{}{"threshold": 0.001}, true},
	}
	for _, tc := range cases {
		err := ValidateModelConfig(tc.modelType, tc.cfg)
		if (err != nil) != tc.wantErr {
			t.Errorf("%s: ValidateModelConfig(%q, %v) err = %v, wantErr %v", tc.name, tc.modelType, tc.cfg, err, tc.wantErr)
		}
	}
}
