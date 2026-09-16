package model

import "testing"

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
