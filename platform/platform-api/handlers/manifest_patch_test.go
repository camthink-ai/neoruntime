package handlers

import (
	"bytes"
	"encoding/json"
	"net/http/httptest"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/gin-gonic/gin"

	"aipc/platform/common/constants"
)

const patchTestManifest = `apiVersion: v1
kind: Application
# hand-edited manifest
metadata:
  id: patch-app
  name: Patch App # original name
  version: 1.0.0
spec:
  image: docker.io/library/alpine:latest
  unknown_future_field: keep-me
`

const patchTestMultiContainer = `apiVersion: v1
kind: Application
metadata:
  id: multi-app
  name: Multi
  version: 1.0.0
spec:
  image: docker.io/library/alpine:latest
  containers:
    - name: sidecar
      image: docker.io/library/busybox:latest
`

// newPatchTestEnv points the platform root at a temp dir and returns the
// manifest dir plus a restore func.
func newPatchTestEnv(t *testing.T, manifest string) (string, func()) {
	t.Helper()
	oldRoot := constants.RootPath()
	root := t.TempDir()
	constants.SetRootPath(root)
	dir := filepath.Join(root, "apps", "staging", "1700000000_deadbeef")
	if err := os.MkdirAll(dir, 0755); err != nil {
		t.Fatalf("MkdirAll: %v", err)
	}
	if err := os.WriteFile(filepath.Join(dir, "app.yaml"), []byte(manifest), 0644); err != nil {
		t.Fatalf("WriteFile: %v", err)
	}
	return dir, func() { constants.SetRootPath(oldRoot) }
}

// callPatch issues a patch request body and returns the recorder.
func callPatch(t *testing.T, h *APIHandlers, body string) *httptest.ResponseRecorder {
	t.Helper()
	gin.SetMode(gin.TestMode)
	w := httptest.NewRecorder()
	c, _ := gin.CreateTestContext(w)
	c.Request = httptest.NewRequest("PATCH", "/api/v1/apps/manifest", strings.NewReader(body))
	c.Request.Header.Set("Content-Type", "application/json")
	h.PatchManifest(c)
	return w
}

func patchCode(t *testing.T, w *httptest.ResponseRecorder) int {
	t.Helper()
	var resp struct {
		Code int `json:"code"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatalf("decode response %q: %v", w.Body.String(), err)
	}
	return resp.Code
}

func quoteJSON(s string) string {
	b, _ := json.Marshal(s)
	return string(b)
}

func TestPatchManifestHappyPath(t *testing.T) {
	dir, restore := newPatchTestEnv(t, patchTestManifest)
	defer restore()
	h := &APIHandlers{}

	path := filepath.Join(dir, "app.yaml")
	w := callPatch(t, h, `{
		"manifest_path": `+quoteJSON(path)+`,
		"fields": {
			"metadata.name": "New: \"Name\"",
			"spec.autostart": true,
			"spec.permissions.inference.models": ["clip_vit_b_32"]
		}
	}`)

	if code := patchCode(t, w); code != CodeSuccess {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeSuccess, w.Body.String())
	}

	// Disk: value changed, comment and unknown field preserved.
	onDisk, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	s := string(onDisk)
	if !strings.Contains(s, "# hand-edited manifest") {
		t.Errorf("document comment lost:\n%s", s)
	}
	if !strings.Contains(s, "keep-me") {
		t.Errorf("unknown field lost:\n%s", s)
	}
	if !strings.Contains(s, "autostart: true") {
		t.Errorf("autostart not written:\n%s", s)
	}
	if !strings.Contains(s, "clip_vit_b_32") {
		t.Errorf("models not written:\n%s", s)
	}

	// The response carries the full manifest back.
	var resp struct {
		Data struct {
			Manifest struct {
				Metadata struct {
					Name string `json:"name"`
				} `json:"metadata"`
			} `json:"manifest"`
		} `json:"data"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatalf("decode success body: %v", err)
	}
	if resp.Data.Manifest.Metadata.Name != `New: "Name"` {
		t.Errorf("response manifest name = %q", resp.Data.Manifest.Metadata.Name)
	}

	// No temp file left behind.
	if _, err := os.Stat(path + ".tmp"); !os.IsNotExist(err) {
		t.Errorf("temp file left behind: %v", err)
	}
}

func TestPatchManifestWhitelist(t *testing.T) {
	dir, restore := newPatchTestEnv(t, patchTestManifest)
	defer restore()
	h := &APIHandlers{}
	path := filepath.Join(dir, "app.yaml")

	tests := []struct {
		name  string
		field string
	}{
		{"metadata_id", "metadata.id"},
		{"spec_image", "spec.image"},
		{"spec_models_subpath", "spec.models.detector.id"},
		{"random", "metadata.evolved"},
	}
	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			body, _ := json.Marshal(ManifestPatchRequest{
				ManifestPath: path,
				Fields:       map[string]json.RawMessage{tc.field: json.RawMessage(`"x"`)},
			})
			w := callPatch(t, h, string(body))
			if code := patchCode(t, w); code != CodeInvalidParameter {
				t.Fatalf("code = %d, want %d; body: %s", code, CodeInvalidParameter, w.Body.String())
			}
			// A rejected field rejects the whole request: the disk file must
			// still match the original byte for byte.
			after, _ := os.ReadFile(path)
			if string(after) != patchTestManifest {
				t.Fatalf("disk file changed on rejected patch")
			}
		})
	}
}

// spec.models is patchable as a whole map: set → clear-with-null round trip,
// with the response echoing the dependency map and the disk file losing the
// models subtree entirely on clear.
func TestPatchManifestSpecModelsMap(t *testing.T) {
	dir, restore := newPatchTestEnv(t, patchTestManifest)
	defer restore()
	h := &APIHandlers{}
	path := filepath.Join(dir, "app.yaml")

	w := callPatch(t, h, `{
		"manifest_path": `+quoteJSON(path)+`,
		"fields": {
			"spec.models": {
				"detector": {"id": "yolov8s-640", "path": "/opt/models/yolov8s.bin", "required": true}
			}
		}
	}`)
	if code := patchCode(t, w); code != CodeSuccess {
		t.Fatalf("set code = %d, want %d; body: %s", code, CodeSuccess, w.Body.String())
	}

	var resp struct {
		Data struct {
			Manifest struct {
				Spec struct {
					Models map[string]struct {
						ID       string `json:"id"`
						Path     string `json:"path"`
						Required bool   `json:"required"`
					} `json:"models"`
				} `json:"spec"`
			} `json:"manifest"`
		} `json:"data"`
	}
	if err := json.Unmarshal(w.Body.Bytes(), &resp); err != nil {
		t.Fatalf("decode success body: %v", err)
	}
	if got := resp.Data.Manifest.Spec.Models["detector"]; got.ID != "yolov8s-640" || got.Path != "/opt/models/yolov8s.bin" || !got.Required {
		t.Errorf("response models[detector] = %+v", got)
	}

	onDisk, err := os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile: %v", err)
	}
	// jsonToNode deliberately keeps JSON scalar quoting (type fidelity), so
	// patch-written map keys/values land double-quoted — valid YAML that
	// round-trips identically.
	for _, want := range []string{"models:", `"detector":`, `"id": "yolov8s-640"`, `"path": "/opt/models/yolov8s.bin"`, `"required": true`} {
		if !strings.Contains(string(onDisk), want) {
			t.Errorf("disk missing %q:\n%s", want, onDisk)
		}
	}

	w = callPatch(t, h, `{
		"manifest_path": `+quoteJSON(path)+`,
		"fields": {"spec.models": null}
	}`)
	if code := patchCode(t, w); code != CodeSuccess {
		t.Fatalf("clear code = %d, want %d; body: %s", code, CodeSuccess, w.Body.String())
	}
	onDisk, err = os.ReadFile(path)
	if err != nil {
		t.Fatalf("ReadFile after clear: %v", err)
	}
	if strings.Contains(string(onDisk), "models") {
		t.Errorf("clear should remove the models subtree:\n%s", onDisk)
	}
	if !strings.Contains(string(onDisk), "keep-me") {
		t.Errorf("unknown sibling lost on clear:\n%s", onDisk)
	}
}

func TestPatchManifestPathSafety(t *testing.T) {
	_, restore := newPatchTestEnv(t, patchTestManifest)
	defer restore()
	h := &APIHandlers{}

	for _, p := range []string{
		"../../etc/passwd",
		"/etc/passwd",
		"/data/aipc/apps/manifests/../../../etc/passwd",
		"/data/aipc/apps/manifests2/evil/app.yaml",
	} {
		req := ManifestPatchRequest{
			ManifestPath: p,
			Fields:       map[string]json.RawMessage{"metadata.name": json.RawMessage(`"x"`)},
		}
		body, _ := json.Marshal(req)
		w := callPatch(t, h, string(body))
		if code := patchCode(t, w); code != CodeInvalidParameter {
			t.Errorf("path %q: code = %d, want 1004; body: %s", p, code, w.Body.String())
		}
	}

	// Direct unit check of the strict staging helper.
	root := constants.RootPath()
	validDir := filepath.Join(root, "apps", "staging", "1700000000_cafebabe")
	if err := os.MkdirAll(validDir, 0755); err != nil {
		t.Fatal(err)
	}
	valid := filepath.Join(validDir, "app.yaml")
	if err := os.WriteFile(valid, []byte(patchTestManifest), 0644); err != nil {
		t.Fatal(err)
	}
	if _, err := safeStagingManifest(valid); err != nil {
		t.Errorf("valid staging path rejected: %v", err)
	}
	for _, invalid := range []string{
		filepath.Join(root, "apps", "manifests", "app", "app.yaml"),
		filepath.Join(root, "apps", "staging", "bad-token", "app.yaml"),
		filepath.Join(validDir, "nested", "app.yaml"),
		filepath.Join(validDir, "other.yaml"),
	} {
		if _, err := safeStagingManifest(invalid); err == nil {
			t.Errorf("unsafe staging path accepted: %s", invalid)
		}
	}
}

func TestPatchManifestMultiContainerRejected(t *testing.T) {
	dir, restore := newPatchTestEnv(t, patchTestMultiContainer)
	defer restore()
	h := &APIHandlers{}
	path := filepath.Join(dir, "app.yaml")

	body, _ := json.Marshal(ManifestPatchRequest{
		ManifestPath: path,
		Fields:       map[string]json.RawMessage{"metadata.name": json.RawMessage(`"x"`)},
	})
	w := callPatch(t, h, string(body))
	if code := patchCode(t, w); code != CodeInvalidRequest {
		t.Fatalf("code = %d, want %d; body: %s", code, CodeInvalidRequest, w.Body.String())
	}
	after, _ := os.ReadFile(path)
	if string(after) != patchTestMultiContainer {
		t.Fatal("multi-container manifest modified despite rejection")
	}
}

func TestPatchManifestInvalidEditLeavesFileUntouched(t *testing.T) {
	dir, restore := newPatchTestEnv(t, patchTestManifest)
	defer restore()
	h := &APIHandlers{}
	path := filepath.Join(dir, "app.yaml")

	// A structurally valid patch whose result fails manifest validation:
	// metadata.name cannot be empty.
	body, _ := json.Marshal(ManifestPatchRequest{
		ManifestPath: path,
		Fields:       map[string]json.RawMessage{"metadata.name": json.RawMessage(`""`)},
	})
	w := callPatch(t, h, string(body))
	if code := patchCode(t, w); code == CodeSuccess {
		t.Fatalf("empty name patch unexpectedly succeeded: %s", w.Body.String())
	}
	after, _ := os.ReadFile(path)
	if string(after) != patchTestManifest {
		t.Fatal("disk file changed despite invalid patch")
	}
}

func TestPatchManifestMissingInputs(t *testing.T) {
	_, restore := newPatchTestEnv(t, patchTestManifest)
	defer restore()
	h := &APIHandlers{}

	// No fields.
	w := callPatch(t, h, `{"manifest_path": "/whatever"}`)
	if code := patchCode(t, w); code != CodeMissingParameter {
		t.Errorf("no fields: code = %d, want 1003", code)
	}
	// No manifest_path.
	w = callPatch(t, h, `{"fields": {"metadata.name": "x"}}`)
	if code := patchCode(t, w); code != CodeMissingParameter {
		t.Errorf("no path: code = %d, want 1003", code)
	}
	// Malformed JSON.
	w = callPatch(t, h, `{broken`)
	if code := patchCode(t, w); code != CodeInvalidRequest {
		t.Errorf("broken json: code = %d, want 1001", code)
	}
	// Valid shape but nonexistent file.
	ghost := filepath.Join(constants.RootPath(), "apps", "staging", "1700000000_feedface", "app.yaml")
	if err := os.MkdirAll(filepath.Dir(ghost), 0755); err != nil {
		t.Fatal(err)
	}
	w = callPatch(t, h, `{"manifest_path": `+quoteJSON(ghost)+`,"fields":{"metadata.name":"x"}}`)
	if code := patchCode(t, w); code != CodeInvalidParameter {
		t.Errorf("missing staging artifact: code = %d, want 1004", code)
	}
}

// Deterministic output: identical requests must produce byte-identical files.
func TestPatchManifestDeterministicOutput(t *testing.T) {
	dir, restore := newPatchTestEnv(t, patchTestManifest)
	defer restore()
	h := &APIHandlers{}
	path := filepath.Join(dir, "app.yaml")

	body, _ := json.Marshal(ManifestPatchRequest{
		ManifestPath: path,
		Fields: map[string]json.RawMessage{
			"metadata.name":                     json.RawMessage(`"A"`),
			"spec.permissions.inference.models": json.RawMessage(`["m1","m2"]`),
			"spec.autostart":                    json.RawMessage(`true`),
		},
	})
	if code := patchCode(t, callPatch(t, h, string(body))); code != CodeSuccess {
		t.Fatalf("first patch failed")
	}
	first, _ := os.ReadFile(path)

	// Reset to the original and patch again.
	os.WriteFile(path, []byte(patchTestManifest), 0644)
	if code := patchCode(t, callPatch(t, h, string(body))); code != CodeSuccess {
		t.Fatalf("second patch failed")
	}
	second, _ := os.ReadFile(path)

	if !bytes.Equal(first, second) {
		t.Errorf("patch output not deterministic:\nfirst:\n%s\nsecond:\n%s", first, second)
	}
}
