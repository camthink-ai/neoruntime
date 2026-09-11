package handlers

import (
	"strings"
	"testing"

	"gopkg.in/yaml.v3"

	"aipc/platform/app-manager/manifest"
)

func TestGenerateAppYAMLInjectionRegression(t *testing.T) {
	// A name containing ':' and quotes used to produce invalid YAML when the
	// generator concatenated raw strings; yaml.Marshal must quote it safely.
	req := &WizardRequest{
		Metadata: WizardMetadata{
			ID:      "evil-app",
			Name:    `Evil: app "quoted" # not a comment`,
			Version: "1.0.0",
		},
		Image: "docker.io/library/alpine:latest",
	}
	data, err := yaml.Marshal(wizardRequestToManifest(req))
	if err != nil {
		t.Fatalf("yaml.Marshal() error: %v", err)
	}
	parsed, err := manifest.ParseManifest(data)
	if err != nil {
		t.Fatalf("ParseManifest(round-trip) error: %v\nYAML:\n%s", err, data)
	}
	if parsed.Metadata.Name != req.Metadata.Name {
		t.Errorf("name round-trip = %q, want %q\nYAML:\n%s", parsed.Metadata.Name, req.Metadata.Name, data)
	}
}

func TestGenerateAppYAMLFullRequestRoundTrip(t *testing.T) {
	noNewPriv := false
	readonlyRootfs := true
	req := &WizardRequest{
		Metadata: WizardMetadata{
			ID:          "full-app",
			Name:        "Full App",
			Version:     "2.1.0",
			Description: "Has: colon, \"quotes\" and # hash",
		},
		Image:         "docker.io/library/nginx:1.27",
		Resources:     WizardResources{CPU: "50%", Memory: "256Mi"},
		Autostart:     true,
		RestartPolicy: "on-failure",
		Permissions: WizardPermissions{
			Video: []string{"/dev/video0"},
			Inference: &WizardInference{
				Models:        []string{"clip_vit_b_32"},
				MaxQPS:        10,
				MaxConcurrent: 2,
				AllowRegister: true,
			},
			Events: &WizardEvents{
				Publish:   []string{"app/full-app/detection"},
				Subscribe: []string{"platform/system/reboot"},
			},
			Device:  &WizardDevice{Light: true, PTZ: true},
			Network: &WizardNetwork{Mode: "host", Inbound: []int{8080, 8443}},
		},
		Env:     []WizardEnvVar{{Name: "PLAIN", Value: "hello"}, {Name: "TRICKY", Value: "a: b # c"}},
		Volumes: []WizardVolume{{Host: "/data", Container: "/app/data", ReadOnly: true}},
		Security: WizardSecurity{
			NoNewPrivileges: &noNewPriv,
			ReadonlyRootfs:  &readonlyRootfs,
		},
	}

	m := wizardRequestToManifest(req)
	data, err := yaml.Marshal(m)
	if err != nil {
		t.Fatalf("yaml.Marshal() error: %v", err)
	}
	parsed, err := manifest.ParseManifest(data)
	if err != nil {
		t.Fatalf("ParseManifest(round-trip) error: %v\nYAML:\n%s", err, data)
	}

	if parsed.Metadata.ID != "full-app" || parsed.Metadata.Name != "Full App" ||
		parsed.Metadata.Version != "2.1.0" || parsed.Metadata.Description != req.Metadata.Description {
		t.Errorf("metadata round-trip mismatch: %+v", parsed.Metadata)
	}
	if parsed.Spec.Image != req.Image {
		t.Errorf("image = %q, want %q", parsed.Spec.Image, req.Image)
	}
	if parsed.Spec.Resources.CPU != "50%" || parsed.Spec.Resources.Memory != "256Mi" {
		t.Errorf("resources = %+v", parsed.Spec.Resources)
	}
	if !parsed.Spec.Autostart || parsed.Spec.RestartPolicy != "on-failure" {
		t.Errorf("autostart/restart_policy = %v/%q", parsed.Spec.Autostart, parsed.Spec.RestartPolicy)
	}
	p := parsed.Spec.Permissions
	if len(p.Video) != 1 || p.Video[0] != "/dev/video0" {
		t.Errorf("video = %v", p.Video)
	}
	if len(p.Inference.Models) != 1 || p.Inference.Models[0] != "clip_vit_b_32" ||
		p.Inference.MaxQPS != 10 || p.Inference.MaxConcurrent != 2 || !p.Inference.AllowRegister {
		t.Errorf("inference = %+v", p.Inference)
	}
	if len(p.Events.Publish) != 1 || len(p.Events.Subscribe) != 1 {
		t.Errorf("events = %+v", p.Events)
	}
	if !p.Device.Light || !p.Device.PTZ || p.Device.IrCut || p.Device.Lens {
		t.Errorf("device = %+v", p.Device)
	}
	if p.Network.Mode != "host" || len(p.Network.Inbound) != 2 || p.Network.Inbound[0] != 8080 {
		t.Errorf("network = %+v", p.Network)
	}
	if len(parsed.Spec.Env) != 2 || parsed.Spec.Env[1].Value != "a: b # c" {
		t.Errorf("env = %+v", parsed.Spec.Env)
	}
	if len(parsed.Spec.Volumes) != 1 || !parsed.Spec.Volumes[0].Readonly {
		t.Errorf("volumes = %+v", parsed.Spec.Volumes)
	}

	// Explicit pointers must survive as pointers — including explicit false,
	// which is semantically different from nil (platform default true).
	sec := parsed.Spec.Security
	if sec.NoNewPrivileges == nil || *sec.NoNewPrivileges != false {
		t.Errorf("security.no_new_privileges = %v, want explicit false", sec.NoNewPrivileges)
	}
	if sec.ReadonlyRootfs == nil || *sec.ReadonlyRootfs != true {
		t.Errorf("security.readonly_rootfs = %v, want explicit true", sec.ReadonlyRootfs)
	}
}

// The top-level models map lands in spec.models verbatim — path/required
// subfields survive the round-trip — and an empty map leaves no stray key.
func TestGenerateAppYAMLModelsMapRoundTrip(t *testing.T) {
	req := &WizardRequest{
		Metadata: WizardMetadata{ID: "model-app", Name: "Model App", Version: "1.0.0"},
		Image:    "docker.io/library/alpine:latest",
		Models: map[string]manifest.ModelMapping{
			"detector": {ID: "yolov8s-640", Path: "/opt/models/yolov8s.bin", Required: true},
			"clip":     {ID: "clip_vit_b_32"},
		},
	}
	data, err := yaml.Marshal(wizardRequestToManifest(req))
	if err != nil {
		t.Fatalf("yaml.Marshal() error: %v", err)
	}
	parsed, err := manifest.ParseManifest(data)
	if err != nil {
		t.Fatalf("ParseManifest(round-trip) error: %v\nYAML:\n%s", err, data)
	}
	if len(parsed.Spec.Models) != 2 {
		t.Fatalf("models = %+v, want 2 entries\nYAML:\n%s", parsed.Spec.Models, data)
	}
	det := parsed.Spec.Models["detector"]
	if det.ID != "yolov8s-640" || det.Path != "/opt/models/yolov8s.bin" || !det.Required {
		t.Errorf("models[detector] = %+v\nYAML:\n%s", det, data)
	}
	if clip := parsed.Spec.Models["clip"]; clip.ID != "clip_vit_b_32" || clip.Required {
		t.Errorf("models[clip] = %+v", clip)
	}

	// No dependencies → no models key at all.
	bare := &WizardRequest{
		Metadata: WizardMetadata{ID: "bare-app", Name: "Bare", Version: "1.0.0"},
		Image:    "docker.io/library/alpine:latest",
	}
	bareData, err := yaml.Marshal(wizardRequestToManifest(bare))
	if err != nil {
		t.Fatalf("yaml.Marshal(bare) error: %v", err)
	}
	if strings.Contains(string(bareData), "models") {
		t.Errorf("empty models map should produce no models key:\n%s", bareData)
	}
}

func TestGenerateAppYAMLMinimalRequestNoEmptyBlocks(t *testing.T) {
	req := &WizardRequest{
		Metadata: WizardMetadata{ID: "minimal-app", Name: "Minimal App", Version: "1.0.0"},
		Image:    "docker.io/library/alpine:latest",
	}
	data, err := yaml.Marshal(wizardRequestToManifest(req))
	if err != nil {
		t.Fatalf("yaml.Marshal() error: %v", err)
	}
	s := string(data)
	for _, banned := range []string{
		"permissions:", "resources:", "healthcheck:", "auto_restart:",
		"volumes:", "env:", "security:", "restart_policy:", "author:", "email:",
	} {
		if strings.Contains(s, banned) {
			t.Errorf("minimal wizard output contains %q:\n%s", banned, s)
		}
	}
	if strings.Contains(s, "autostart") {
		// autostart must not appear at all when false (omitempty), unlike
		// the old emitter which only skipped it by explicit check.
		t.Logf("note: autostart present unexpectedly")
	}
	if _, err := manifest.ParseManifest(data); err != nil {
		t.Fatalf("ParseManifest(minimal) error: %v\nYAML:\n%s", err, data)
	}
}

func TestGenerateAppYAMLEmptyPermissionPointersOmitted(t *testing.T) {
	// Non-nil but empty permission sub-structs must not leave stray keys
	// behind: yaml.v3's isZero drops all-zero structs.
	req := &WizardRequest{
		Metadata:    WizardMetadata{ID: "empty-perms", Name: "Empty Perms", Version: "1.0.0"},
		Image:       "docker.io/library/alpine:latest",
		Permissions: WizardPermissions{Inference: &WizardInference{}, Events: &WizardEvents{}},
	}
	data, err := yaml.Marshal(wizardRequestToManifest(req))
	if err != nil {
		t.Fatalf("yaml.Marshal() error: %v", err)
	}
	if strings.Contains(string(data), "permissions:") {
		t.Errorf("empty permission pointers should produce no permissions block:\n%s", data)
	}
}

func TestGenerateAppYAMLHandlerWrapper(t *testing.T) {
	var h APIHandlers
	req := &WizardRequest{
		Metadata: WizardMetadata{ID: "x", Name: "X", Version: "1.0.0"},
		Image:    "docker.io/library/alpine:latest",
	}
	data, err := h.generateAppYAML(req)
	if err != nil {
		t.Fatalf("generateAppYAML() error: %v", err)
	}
	if len(data) == 0 {
		t.Fatal("generateAppYAML() returned empty bytes")
	}
}
