package manifest

import (
	"encoding/json"
	"os"
	"reflect"
	"strings"
	"testing"

	"gopkg.in/yaml.v3"
)

func TestExpandEnvRefs(t *testing.T) {
	tests := []struct {
		name  string
		input string
		setup func() // set env vars before test
		clean func() // unset env vars after test
		want  string
	}{
		{
			name:  "no_refs",
			input: "plain-value",
			want:  "plain-value",
		},
		{
			name:  "single_ref_resolved",
			input: "${HOME}",
			setup: func() { os.Setenv("HOME", "/root") },
			clean: func() { os.Unsetenv("HOME") },
			want:  "/root",
		},
		{
			name:  "single_ref_unresolved",
			input: "${AIPC_TOKEN_KEY}",
			setup: func() { os.Unsetenv("AIPC_TOKEN_KEY") },
			want:  "${AIPC_TOKEN_KEY}", // left intact
		},
		{
			name:  "ref_resolved_token",
			input: "${AIPC_TOKEN_KEY}",
			setup: func() { os.Setenv("AIPC_TOKEN_KEY", "aipc-secure-token-secret") },
			clean: func() { os.Unsetenv("AIPC_TOKEN_KEY") },
			want:  "aipc-secure-token-secret",
		},
		{
			name:  "mixed_literal_and_ref",
			input: "prefix-${HOSTNAME}-suffix",
			setup: func() { os.Setenv("HOSTNAME", "hailo15") },
			clean: func() { os.Unsetenv("HOSTNAME") },
			want:  "prefix-hailo15-suffix",
		},
		{
			name:  "multiple_refs",
			input: "${A}/${B}",
			setup: func() { os.Setenv("A", "1"); os.Setenv("B", "2") },
			clean: func() { os.Unsetenv("A"); os.Unsetenv("B") },
			want:  "1/2",
		},
		{
			name:  "empty_string",
			input: "",
			want:  "",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			if tt.setup != nil {
				tt.setup()
			}
			defer func() {
				if tt.clean != nil {
					tt.clean()
				}
			}()
			got := ExpandEnvRefs(tt.input)
			if got != tt.want {
				t.Errorf("ExpandEnvRefs(%q) = %q, want %q", tt.input, got, tt.want)
			}
		})
	}
}

func TestToContainerEnvExpansion(t *testing.T) {
	os.Setenv("AIPC_TOKEN_KEY", "test-token-123")
	defer os.Unsetenv("AIPC_TOKEN_KEY")

	m := &AppManifest{
		Spec: Spec{
			Env: []EnvVar{
				{Name: "PLAIN", Value: "hello"},
				{Name: "TOKEN", Value: "${AIPC_TOKEN_KEY}"},
				{Name: "MISSING", Value: "${NONEXISTENT_VAR_XYZ}"},
			},
		},
	}

	env := m.ToContainerEnv()
	want := []string{
		"PLAIN=hello",
		"TOKEN=test-token-123",
		"MISSING=${NONEXISTENT_VAR_XYZ}",
	}
	if len(env) != len(want) {
		t.Fatalf("ToContainerEnv() len = %d, want %d", len(env), len(want))
	}
	for i, got := range env {
		if got != want[i] {
			t.Errorf("env[%d] = %q, want %q", i, got, want[i])
		}
	}
}

func TestValidateModels(t *testing.T) {
	tests := []struct {
		name    string
		alias   string
		mapping ModelMapping
		wantErr string // empty = expect success
	}{
		{
			name:    "valid_mapping",
			alias:   "clip",
			mapping: ModelMapping{ID: "clip_vit_b_32", Required: true},
		},
		{
			name:    "alias_starts_with_digit",
			alias:   "1clip",
			mapping: ModelMapping{ID: "clip_vit_b_32"},
			wantErr: "must match",
		},
		{
			name:    "alias_contains_dash",
			alias:   "my-model",
			mapping: ModelMapping{ID: "clip_vit_b_32"},
			wantErr: "must match",
		},
		{
			name:    "alias_contains_space",
			alias:   "my model",
			mapping: ModelMapping{ID: "clip_vit_b_32"},
			wantErr: "must match",
		},
		{
			name:    "alias_reserved_app_id",
			alias:   "APP_ID",
			mapping: ModelMapping{ID: "clip_vit_b_32"},
			wantErr: "reserved",
		},
		{
			name:    "alias_reserved_host_prefix",
			alias:   "HOST_PREFIX",
			mapping: ModelMapping{ID: "clip_vit_b_32"},
			wantErr: "reserved",
		},
		{
			name:    "alias_reserved_app_role",
			alias:   "APP_ROLE",
			mapping: ModelMapping{ID: "clip_vit_b_32"},
			wantErr: "reserved",
		},
		{
			name:    "alias_reserved_container_name",
			alias:   "CONTAINER_NAME",
			mapping: ModelMapping{ID: "clip_vit_b_32"},
			wantErr: "reserved",
		},
		{
			name:    "id_missing",
			alias:   "clip",
			mapping: ModelMapping{},
			wantErr: "spec.models.clip.id is required",
		},
		{
			name:    "path_relative",
			alias:   "clip",
			mapping: ModelMapping{ID: "clip_vit_b_32", Path: "models/clip.bin"},
			wantErr: "absolute container path",
		},
		{
			name:    "path_parent_escape",
			alias:   "clip",
			mapping: ModelMapping{ID: "clip_vit_b_32", Path: "/app/../etc/clip.bin"},
			wantErr: "absolute container path",
		},
		{
			name:    "path_redundant_separator",
			alias:   "clip",
			mapping: ModelMapping{ID: "clip_vit_b_32", Path: "/app//models/clip.bin"},
			wantErr: "absolute container path",
		},
		{
			name:    "path_directory_root",
			alias:   "clip",
			mapping: ModelMapping{ID: "clip_vit_b_32", Path: "/"},
			wantErr: "absolute container path",
		},
		{
			name:    "path_bare_hef",
			alias:   "clip",
			mapping: ModelMapping{ID: "clip_vit_b_32", Path: "/app/models/clip.hef"},
			wantErr: "must be a .bin AMPK model package",
		},
		{
			name:    "path_without_extension",
			alias:   "clip",
			mapping: ModelMapping{ID: "clip_vit_b_32", Path: "/app/models/clip"},
			wantErr: "must be a .bin AMPK model package",
		},
		{
			name:    "bundled_bin_package_accepted",
			alias:   "clip",
			mapping: ModelMapping{ID: "clip_vit_b_32", Path: "/app/models/clip.bin", Required: true},
			wantErr: "",
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			m := &AppManifest{
				Spec: Spec{
					Models: map[string]ModelMapping{tt.alias: tt.mapping},
				},
			}
			err := m.ValidateModels()
			if tt.wantErr == "" {
				if err != nil {
					t.Fatalf("ValidateModels() unexpected error: %v", err)
				}
			} else {
				if err == nil {
					t.Fatalf("ValidateModels() expected error containing %q, got nil", tt.wantErr)
				}
				if !strings.Contains(err.Error(), tt.wantErr) {
					t.Errorf("ValidateModels() error = %q, want substring %q", err.Error(), tt.wantErr)
				}
			}
		})
	}
}

func TestValidateModelsChainedFromValidate(t *testing.T) {
	data := []byte(`
apiVersion: v1
kind: Application
metadata:
  id: test-app
  name: Test App
  version: 1.0.0
spec:
  image: docker.io/library/alpine:latest
  models:
    detector:
      id: yolo_v5
      path: /app/models/../models/yolo.bin
`)
	_, err := ParseManifest(data)
	if err == nil {
		t.Fatal("ParseManifest() expected error for unclean models path, got nil")
	}
	if !strings.Contains(err.Error(), "models validation failed") {
		t.Errorf("ParseManifest() error = %q, want 'models validation failed' in chain", err.Error())
	}
}

func TestParseManifestRejectsLegacyModelType(t *testing.T) {
	// A leftover spec.models.<alias>.type key must fail parsing outright:
	// yaml.Unmarshal is not strict, so without the probe the key would be
	// silently swallowed and the model would install with different
	// postprocess configuration than its author wrote.
	base := `
apiVersion: v1
kind: Application
metadata:
  id: test-app
  name: Test App
  version: 1.0.0
spec:
  image: docker.io/library/alpine:latest
  models:
    detector:
      id: yolo_v5
`
	t.Run("type_with_path", func(t *testing.T) {
		_, err := ParseManifest([]byte(base + "      path: /app/models/yolo.bin\n      type: detection\n"))
		if err == nil {
			t.Fatal("ParseManifest() expected error for legacy type key, got nil")
		}
		if !strings.Contains(err.Error(), "spec.models.detector.type is no longer supported") {
			t.Errorf("ParseManifest() error = %q, want 'type is no longer supported'", err.Error())
		}
	})

	t.Run("type_without_path", func(t *testing.T) {
		_, err := ParseManifest([]byte(base + "      type: clip\n"))
		if err == nil {
			t.Fatal("ParseManifest() expected error for legacy type key, got nil")
		}
		if !strings.Contains(err.Error(), "spec.models.detector.type is no longer supported") {
			t.Errorf("ParseManifest() error = %q, want 'type is no longer supported'", err.Error())
		}
	})

	t.Run("no_type_key_parses", func(t *testing.T) {
		if _, err := ParseManifest([]byte(base + "      path: /app/models/yolo.bin\n")); err != nil {
			t.Fatalf("ParseManifest() unexpected error: %v", err)
		}
	})
}

func TestModelEnvVars(t *testing.T) {
	tests := []struct {
		name   string
		models map[string]ModelMapping
		want   []string
	}{
		{
			name:   "nil_models",
			models: nil,
			want:   nil,
		},
		{
			name:   "empty_models",
			models: map[string]ModelMapping{},
			want:   nil,
		},
		{
			name: "single_model",
			models: map[string]ModelMapping{
				"clip": {ID: "clip_vit_b_32"},
			},
			// Alias case is preserved: apps read os.environ["AIPC_MODEL_clip"].
			want: []string{"AIPC_MODEL_clip=clip_vit_b_32"},
		},
		{
			name: "sorted_by_alias",
			models: map[string]ModelMapping{
				"zeta":  {ID: "model_z"},
				"alpha": {ID: "model_a"},
				"mid":   {ID: "model_m"},
			},
			want: []string{
				"AIPC_MODEL_alpha=model_a",
				"AIPC_MODEL_mid=model_m",
				"AIPC_MODEL_zeta=model_z",
			},
		},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			m := &AppManifest{Spec: Spec{Models: tt.models}}
			got := m.ModelEnvVars()
			if !reflect.DeepEqual(got, tt.want) {
				t.Errorf("ModelEnvVars() = %v, want %v", got, tt.want)
			}
		})
	}
}

func TestMergeModelPermissions(t *testing.T) {
	t.Run("appends_missing_ids_order_stable", func(t *testing.T) {
		m := &AppManifest{
			Spec: Spec{
				Permissions: Permissions{
					Inference: InferencePerms{Models: []string{"existing_a"}},
				},
				Models: map[string]ModelMapping{
					"zeta":  {ID: "model_z"},
					"alpha": {ID: "model_a"},
				},
			},
		}
		m.MergeModelPermissions()
		want := []string{"existing_a", "model_a", "model_z"}
		if !reflect.DeepEqual(m.Spec.Permissions.Inference.Models, want) {
			t.Errorf("models = %v, want %v", m.Spec.Permissions.Inference.Models, want)
		}
	})

	t.Run("dedup_against_existing_and_within_models", func(t *testing.T) {
		m := &AppManifest{
			Spec: Spec{
				Permissions: Permissions{
					Inference: InferencePerms{Models: []string{"shared"}},
				},
				Models: map[string]ModelMapping{
					"one":   {ID: "shared"},
					"two":   {ID: "shared"},
					"three": {ID: "unique"},
				},
			},
		}
		m.MergeModelPermissions()
		want := []string{"shared", "unique"}
		if !reflect.DeepEqual(m.Spec.Permissions.Inference.Models, want) {
			t.Errorf("models = %v, want %v", m.Spec.Permissions.Inference.Models, want)
		}
	})

	t.Run("idempotent", func(t *testing.T) {
		m := &AppManifest{
			Spec: Spec{
				Models: map[string]ModelMapping{
					"clip": {ID: "clip_vit_b_32"},
				},
			},
		}
		m.MergeModelPermissions()
		first := append([]string(nil), m.Spec.Permissions.Inference.Models...)
		m.MergeModelPermissions()
		if !reflect.DeepEqual(m.Spec.Permissions.Inference.Models, first) {
			t.Errorf("second merge changed list: %v != %v", m.Spec.Permissions.Inference.Models, first)
		}
	})

	t.Run("no_models_noop", func(t *testing.T) {
		m := &AppManifest{
			Spec: Spec{
				Permissions: Permissions{
					Inference: InferencePerms{Models: []string{"keep_me"}},
				},
			},
		}
		m.MergeModelPermissions()
		want := []string{"keep_me"}
		if !reflect.DeepEqual(m.Spec.Permissions.Inference.Models, want) {
			t.Errorf("models = %v, want %v (must be untouched)", m.Spec.Permissions.Inference.Models, want)
		}
	})
}

func TestModelDependencyIDs(t *testing.T) {
	m := &AppManifest{
		Spec: Spec{
			Models: map[string]ModelMapping{
				"zeta":  {ID: "shared"},
				"alpha": {ID: "shared"},
				"mid":   {ID: "unique"},
			},
		},
	}
	want := []string{"shared", "unique"}
	got := m.ModelDependencyIDs()
	if !reflect.DeepEqual(got, want) {
		t.Errorf("ModelDependencyIDs() = %v, want %v", got, want)
	}

	empty := (&AppManifest{}).ModelDependencyIDs()
	if empty != nil {
		t.Errorf("ModelDependencyIDs() on no models = %v, want nil", empty)
	}
}

const parseManifestModelsYAML = `
apiVersion: v1
kind: Application
metadata:
  id: shelf-ops
  name: Shelf Ops
  version: 0.3.0
spec:
  image: docker.io/aipc/shelf-ops:0.3.0
  permissions:
    inference:
      models:
        - clip_vit_b_32
  models:
    clip:
      id: clip_vit_b_32
      required: true
    detector:
      id: yolo_world_540
`

func TestParseManifestModels(t *testing.T) {
	t.Run("merges_declared_ids_into_permissions", func(t *testing.T) {
		m, err := ParseManifest([]byte(parseManifestModelsYAML))
		if err != nil {
			t.Fatalf("ParseManifest() error: %v", err)
		}
		want := []string{"clip_vit_b_32", "yolo_world_540"}
		if !reflect.DeepEqual(m.Spec.Permissions.Inference.Models, want) {
			t.Errorf("permissions.inference.models = %v, want %v", m.Spec.Permissions.Inference.Models, want)
		}
		if m.Spec.Models["clip"].Required != true {
			t.Errorf("models.clip.required = %v, want true", m.Spec.Models["clip"].Required)
		}
		if m.Spec.Models["detector"].Required != false {
			t.Errorf("models.detector.required = %v, want false (default)", m.Spec.Models["detector"].Required)
		}
	})

	t.Run("no_models_backward_compatible", func(t *testing.T) {
		data := strings.Replace(parseManifestModelsYAML, "  models:\n    clip:\n      id: clip_vit_b_32\n      required: true\n    detector:\n      id: yolo_world_540\n", "", 1)
		m, err := ParseManifest([]byte(data))
		if err != nil {
			t.Fatalf("ParseManifest() error: %v", err)
		}
		want := []string{"clip_vit_b_32"}
		if !reflect.DeepEqual(m.Spec.Permissions.Inference.Models, want) {
			t.Errorf("permissions.inference.models = %v, want %v (declared list untouched)", m.Spec.Permissions.Inference.Models, want)
		}
		if len(m.Spec.Models) != 0 {
			t.Errorf("models = %v, want empty", m.Spec.Models)
		}
		if envs := m.ModelEnvVars(); envs != nil {
			t.Errorf("ModelEnvVars() = %v, want nil", envs)
		}
	})

	t.Run("invalid_alias_rejected", func(t *testing.T) {
		data := []byte(`
apiVersion: v1
kind: Application
metadata:
  id: bad-app
  name: Bad App
  version: 1.0.0
spec:
  image: docker.io/library/alpine:latest
  models:
    1nvalid:
      id: some_model
`)
		_, err := ParseManifest(data)
		if err == nil {
			t.Fatal("ParseManifest() expected error for invalid alias, got nil")
		}
	})

	t.Run("deterministic_across_parses", func(t *testing.T) {
		var first []string
		for i := 0; i < 20; i++ {
			m, err := ParseManifest([]byte(parseManifestModelsYAML))
			if err != nil {
				t.Fatalf("ParseManifest() error: %v", err)
			}
			if i == 0 {
				first = m.Spec.Permissions.Inference.Models
			} else if !reflect.DeepEqual(m.Spec.Permissions.Inference.Models, first) {
				t.Fatalf("nondeterministic merge order: %v != %v", m.Spec.Permissions.Inference.Models, first)
			}
		}
	})
}

func TestMarshalOmitEmpty(t *testing.T) {
	t.Run("yaml_minimal_spec_has_no_empty_blocks", func(t *testing.T) {
		m := AppManifest{
			APIVersion: "v1",
			Kind:       "Application",
			Metadata: Metadata{
				ID:      "minimal-app",
				Name:    "Minimal App",
				Version: "1.0.0",
			},
			Spec: Spec{Image: "docker.io/library/alpine:latest"},
		}
		out, err := yaml.Marshal(&m)
		if err != nil {
			t.Fatalf("yaml.Marshal() error: %v", err)
		}
		s := string(out)
		for _, banned := range []string{"permissions:", "resources:", "healthcheck:", "auto_restart:", "models:", "containers:", "volumes:", "env:", "dev:"} {
			if strings.Contains(s, banned) {
				t.Errorf("minimal spec yaml contains %q:\n%s", banned, s)
			}
		}
	})

	t.Run("yaml_models_emitted_when_declared", func(t *testing.T) {
		m := AppManifest{
			APIVersion: "v1",
			Kind:       "Application",
			Metadata:   Metadata{ID: "app", Name: "App", Version: "1.0.0"},
			Spec: Spec{
				Image: "docker.io/library/alpine:latest",
				Models: map[string]ModelMapping{
					"clip": {ID: "clip_vit_b_32", Required: true},
				},
			},
		}
		out, err := yaml.Marshal(&m)
		if err != nil {
			t.Fatalf("yaml.Marshal() error: %v", err)
		}
		s := string(out)
		for _, want := range []string{"models:", "clip:", "id: clip_vit_b_32", "required: true"} {
			if !strings.Contains(s, want) {
				t.Errorf("yaml output missing %q:\n%s", want, s)
			}
		}
		// Round-trip: marshaled output must parse back to the same mapping.
		parsed, err := ParseManifest(out)
		if err != nil {
			t.Fatalf("ParseManifest(marshaled) error: %v", err)
		}
		if parsed.Spec.Models["clip"].ID != "clip_vit_b_32" || !parsed.Spec.Models["clip"].Required {
			t.Errorf("round-trip models = %+v", parsed.Spec.Models)
		}
	})

	t.Run("json_models_key_omitted_when_absent", func(t *testing.T) {
		m := AppManifest{
			APIVersion: "v1",
			Kind:       "Application",
			Metadata:   Metadata{ID: "app", Name: "App", Version: "1.0.0"},
			Spec:       Spec{Image: "docker.io/library/alpine:latest"},
		}
		out, err := json.Marshal(&m)
		if err != nil {
			t.Fatalf("json.Marshal() error: %v", err)
		}
		// Check spec.models specifically — the literal "models" also appears
		// under permissions.inference.models, so decode instead of substring.
		var decoded map[string]any
		if err := json.Unmarshal(out, &decoded); err != nil {
			t.Fatalf("json.Unmarshal() error: %v", err)
		}
		spec, ok := decoded["spec"].(map[string]any)
		if !ok {
			t.Fatalf("json output missing spec object:\n%s", out)
		}
		if _, present := spec["models"]; present {
			t.Errorf("json spec should omit empty models:\n%s", out)
		}
		if _, present := spec["image"]; !present {
			t.Errorf("json spec missing image:\n%s", out)
		}
		if decoded["apiVersion"] != "v1" {
			t.Errorf("json output missing mirrored tag name apiVersion:\n%s", out)
		}
	})

	t.Run("json_models_present_when_set", func(t *testing.T) {
		m := AppManifest{
			APIVersion: "v1",
			Kind:       "Application",
			Metadata:   Metadata{ID: "app", Name: "App", Version: "1.0.0"},
			Spec: Spec{
				Image: "docker.io/library/alpine:latest",
				Models: map[string]ModelMapping{
					"clip": {ID: "clip_vit_b_32"},
				},
			},
		}
		out, err := json.Marshal(&m)
		if err != nil {
			t.Fatalf("json.Marshal() error: %v", err)
		}
		if !strings.Contains(string(out), `"models":{"clip":{"id":"clip_vit_b_32"}}`) {
			t.Errorf("json output missing models mapping:\n%s", out)
		}
	})
}

func TestImageReferences(t *testing.T) {
	t.Run("single_container_returns_normalized_spec_image", func(t *testing.T) {
		m := &AppManifest{
			Metadata: Metadata{ID: "app", Name: "App", Version: "1.0.0"},
			Spec:     Spec{Image: "busybox:latest"},
		}
		got := m.ImageReferences()
		want := []string{"docker.io/library/busybox:latest"}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("ImageReferences() = %v, want %v", got, want)
		}
	})

	t.Run("multi_container_main_first_then_sorted_deduped", func(t *testing.T) {
		m := &AppManifest{
			Metadata: Metadata{ID: "app", Name: "App", Version: "1.0.0"},
			Spec: Spec{
				Containers: map[string]ContainerSpec{
					"worker": {Image: "docker.io/library/alpine:3.19", Role: "sub"},
					"main":   {Image: "busybox:latest", Role: "main"},
					"zleep":  {Image: "docker.io/library/busybox:latest", Role: "sub"},
					"noimg":  {Image: "", Role: "sub"},
				},
			},
		}
		got := m.ImageReferences()
		// main first, then remaining containers in sorted key order, empty
		// images skipped, duplicates collapsed after normalization.
		want := []string{
			"docker.io/library/busybox:latest",
			"docker.io/library/alpine:3.19",
		}
		if !reflect.DeepEqual(got, want) {
			t.Fatalf("ImageReferences() = %v, want %v", got, want)
		}
	})

	t.Run("no_image_and_no_containers_returns_empty", func(t *testing.T) {
		m := &AppManifest{
			Metadata: Metadata{ID: "app", Name: "App", Version: "1.0.0"},
		}
		if got := m.ImageReferences(); len(got) != 0 {
			t.Fatalf("ImageReferences() = %v, want empty", got)
		}
	})

	t.Run("single_container_with_empty_image_returns_empty", func(t *testing.T) {
		m := &AppManifest{
			Metadata: Metadata{ID: "app", Name: "App", Version: "1.0.0"},
		}
		if got := m.ImageReferences(); len(got) != 0 {
			t.Fatalf("ImageReferences() = %v, want empty", got)
		}
	})
}

func TestValidateModelsDuplicateIDPathContract(t *testing.T) {
	tests := []struct {
		name    string
		models  map[string]ModelMapping
		wantErr bool
	}{
		{
			name: "aliases_without_paths_share_runtime_id",
			models: map[string]ModelMapping{
				"primary": {ID: "shared"},
				"backup":  {ID: "shared", Required: true},
			},
		},
		{
			name: "aliases_with_same_path_share_bundled_source",
			models: map[string]ModelMapping{
				"primary": {ID: "shared", Path: "/app/models/shared.bin"},
				"backup":  {ID: "shared", Path: "/app/models/shared.bin"},
			},
		},
		{
			name: "different_paths_are_ambiguous",
			models: map[string]ModelMapping{
				"primary": {ID: "shared", Path: "/app/models/a.bin"},
				"backup":  {ID: "shared", Path: "/app/models/b.bin"},
			},
			wantErr: true,
		},
		{
			name: "path_and_no_path_are_ambiguous",
			models: map[string]ModelMapping{
				"primary": {ID: "shared", Path: "/app/models/shared.bin"},
				"backup":  {ID: "shared"},
			},
			wantErr: true,
		},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			m := &AppManifest{Spec: Spec{Models: tt.models}}
			err := m.ValidateModels()
			if tt.wantErr && (err == nil || !strings.Contains(err.Error(), "conflicting paths")) {
				t.Fatalf("ValidateModels error = %v, want conflicting paths", err)
			}
			if !tt.wantErr && err != nil {
				t.Fatalf("ValidateModels unexpected error: %v", err)
			}
		})
	}

	m := &AppManifest{Spec: Spec{Models: map[string]ModelMapping{
		"primary": {ID: "shared"},
		"backup":  {ID: "shared"},
	}}}
	if got := m.ModelEnvVars(); !reflect.DeepEqual(got, []string{"AIPC_MODEL_backup=shared", "AIPC_MODEL_primary=shared"}) {
		t.Fatalf("ModelEnvVars = %v, want both aliases", got)
	}
}
