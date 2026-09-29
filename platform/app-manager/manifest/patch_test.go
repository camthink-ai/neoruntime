package manifest

import (
	"strings"
	"testing"

	"gopkg.in/yaml.v3"
)

// patchSrc is a representative hand-written manifest: comments, unknown
// fields and non-canonical formatting that must all survive a patch.
const patchSrc = `apiVersion: v1
kind: Application
# platform manifest — hand edited
metadata:
  id: test-app # directory-bound
  name: Test App
  version: 1.0.0
  unknown_meta: keep-me
spec:
  image: docker.io/library/alpine:latest
  autostart: true
  env:
    - name: KEEP
      value: me
  unknown_spec:
    nested: true
`

func mustPatch(t *testing.T, src string, ops ...FieldPatch) []byte {
	t.Helper()
	out, err := PatchDocument([]byte(src), ops)
	if err != nil {
		t.Fatalf("PatchDocument() error: %v", err)
	}
	return out
}

// 1. No-op round-trip keeps comments and unknown fields.
func TestPatchDocumentNoOpPreservesCommentsAndUnknownFields(t *testing.T) {
	out := mustPatch(t, patchSrc)
	s := string(out)
	for _, want := range []string{
		"# platform manifest — hand edited",
		"# directory-bound",
		"unknown_meta: keep-me",
		"nested: true",
		"id: test-app",
	} {
		if !strings.Contains(s, want) {
			t.Errorf("output missing %q:\n%s", want, s)
		}
	}
	if strings.Contains(s, "\t") {
		t.Errorf("output contains tab indentation:\n%s", s)
	}
	if _, err := ParseManifest(out); err != nil {
		t.Fatalf("ParseManifest(no-op) error: %v\n%s", err, out)
	}
}

// 2. Injection regression: a hostile name round-trips exactly once through
// ParseManifest — the R3 guarantee the wizard hand-builder broke.
func TestPatchDocumentEvilNameRoundTrip(t *testing.T) {
	out := mustPatch(t, patchSrc, FieldPatch{
		Path:  "metadata.name",
		Value: []byte(`"Evil: \"app\" # not a comment"`),
	})
	parsed, err := ParseManifest(out)
	if err != nil {
		t.Fatalf("ParseManifest() error: %v\n%s", err, out)
	}
	if want := `Evil: "app" # not a comment`; parsed.Metadata.Name != want {
		t.Errorf("name = %q, want %q\n%s", parsed.Metadata.Name, want, out)
	}
	// The rest of the document is untouched.
	if !strings.Contains(string(out), "unknown_meta: keep-me") {
		t.Errorf("sibling field lost:\n%s", out)
	}
}

// 3. Missing intermediate mappings are created on demand.
func TestPatchDocumentCreatesIntermediateMappings(t *testing.T) {
	src := `apiVersion: v1
kind: Application
metadata:
  id: bare-app
  name: Bare
  version: 1.0.0
spec:
  image: alpine:latest
`
	out := mustPatch(t, src, FieldPatch{
		Path:  "spec.permissions.inference.models",
		Value: []byte(`["clip_vit_b_32"]`),
	})
	parsed, err := ParseManifest(out)
	if err != nil {
		t.Fatalf("ParseManifest() error: %v\n%s", err, out)
	}
	if got := parsed.Spec.Permissions.Inference.Models; len(got) != 1 || got[0] != "clip_vit_b_32" {
		t.Errorf("inference.models = %v\n%s", got, out)
	}
	if !strings.Contains(string(out), "permissions:") || !strings.Contains(string(out), "inference:") {
		t.Errorf("intermediate keys not emitted:\n%s", out)
	}
}

// 4. Sequence replacement; JSON strings that look numeric stay strings.
func TestPatchDocumentSequenceReplacementKeepsStringyNumbers(t *testing.T) {
	out := mustPatch(t, patchSrc, FieldPatch{
		Path:  "spec.env",
		Value: []byte(`[{"name":"PORT","value":"123"},{"name":"CODE","value":"007"}]`),
	})
	parsed, err := ParseManifest(out)
	if err != nil {
		t.Fatalf("ParseManifest() error: %v\n%s", err, out)
	}
	if len(parsed.Spec.Env) != 2 {
		t.Fatalf("env = %+v\n%s", parsed.Spec.Env, out)
	}
	for i, want := range []string{"123", "007"} {
		if parsed.Spec.Env[i].Value != want {
			t.Errorf("env[%d].value = %q, want string %q\n%s", i, parsed.Spec.Env[i].Value, want, out)
		}
	}
	if !strings.Contains(string(out), `"123"`) {
		t.Errorf("stringy number lost its quotes:\n%s", out)
	}
}

// 5. Type fidelity: JSON true is a YAML bool, JSON "true" is a quoted string.
func TestPatchDocumentTypeFidelity(t *testing.T) {
	boolNode, err := jsonToNode([]byte(`true`))
	if err != nil {
		t.Fatalf("jsonToNode(true) error: %v", err)
	}
	if boolNode.Tag != "!!bool" || boolNode.Value != "true" {
		t.Errorf("JSON true → tag %s value %q, want !!bool/true", boolNode.Tag, boolNode.Value)
	}

	strNode, err := jsonToNode([]byte(`"true"`))
	if err != nil {
		t.Fatalf("jsonToNode(\"true\") error: %v", err)
	}
	if strNode.Tag != "!!str" || strNode.Value != "true" {
		t.Errorf("JSON \"true\" → tag %s value %q, want !!str/true", strNode.Tag, strNode.Value)
	}
	if strNode.Style&yaml.DoubleQuotedStyle == 0 {
		t.Errorf("JSON \"true\" should keep double quotes so it re-encodes as a string")
	}

	// End to end: autostart=false as JSON bool, version as JSON number-like
	// string "2.0" must not become float 2.
	out := mustPatch(t, patchSrc,
		FieldPatch{Path: "spec.autostart", Value: []byte(`false`)},
		FieldPatch{Path: "metadata.version", Value: []byte(`"2.0"`)},
	)
	parsed, err := ParseManifest(out)
	if err != nil {
		t.Fatalf("ParseManifest() error: %v\n%s", err, out)
	}
	if parsed.Spec.Autostart {
		t.Errorf("autostart still true after JSON false patch:\n%s", out)
	}
	if parsed.Metadata.Version != "2.0" {
		t.Errorf("version = %q, want string \"2.0\"\n%s", parsed.Metadata.Version, out)
	}
}

// 6. List-of-maps replacement (spec.env) with nested map values and tricky
// scalars.
func TestPatchDocumentListOfMapsWithTrickyValues(t *testing.T) {
	out := mustPatch(t, patchSrc, FieldPatch{
		Path:  "spec.env",
		Value: []byte(`[{"name":"GREETING","value":"hello: world # fine"},{"name":"EMPTY","value":""}]`),
	})
	parsed, err := ParseManifest(out)
	if err != nil {
		t.Fatalf("ParseManifest() error: %v\n%s", err, out)
	}
	if parsed.Spec.Env[0].Value != "hello: world # fine" {
		t.Errorf("env[0].value = %q\n%s", parsed.Spec.Env[0].Value, out)
	}
	if parsed.Spec.Env[1].Value != "" {
		t.Errorf("env[1].value = %q, want empty string\n%s", parsed.Spec.Env[1].Value, out)
	}
}

// 7. Unknown sibling fields and their subtree survive a targeted patch.
func TestPatchDocumentUnknownSiblingsUntouched(t *testing.T) {
	out := mustPatch(t, patchSrc, FieldPatch{
		Path:  "metadata.description",
		Value: []byte(`"added by patch"`),
	})
	s := string(out)
	if !strings.Contains(s, "unknown_meta: keep-me") || !strings.Contains(s, "nested: true") {
		t.Errorf("unknown siblings lost:\n%s", s)
	}
	if !strings.Contains(s, "image: docker.io/library/alpine:latest") {
		t.Errorf("spec.image changed:\n%s", s)
	}
}

// 8. Paths that traverse a scalar or a sequence fail loudly.
func TestPatchDocumentBadParents(t *testing.T) {
	cases := []struct {
		name string
		path string
		want string
	}{
		{"through_scalar", "metadata.id.foo", "cannot descend"},
		{"through_sequence", "spec.env.0.name", "cannot descend"},
		{"empty_path", "", "empty patch path"},
		{"dotted_hole", "spec..image", "empty segment"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			_, err := PatchDocument([]byte(patchSrc), []FieldPatch{{Path: tc.path, Value: []byte(`1`)}})
			if err == nil {
				t.Fatalf("expected error containing %q, got nil", tc.want)
			}
			if !strings.Contains(err.Error(), tc.want) {
				t.Errorf("error = %q, want substring %q", err.Error(), tc.want)
			}
		})
	}
}

// 9. Multiple ops apply in order; a later op can target a subtree an
// earlier op created.
func TestPatchDocumentMultiOpChainedCreation(t *testing.T) {
	src := `apiVersion: v1
kind: Application
metadata:
  id: chain-app
  name: Chain
  version: 1.0.0
spec:
  image: alpine:latest
`
	out := mustPatch(t, src,
		FieldPatch{Path: "spec.permissions.inference.models", Value: []byte(`["clip_vit_b_32"]`)},
		FieldPatch{Path: "spec.permissions.inference.max_qps", Value: []byte(`5`)},
		FieldPatch{Path: "metadata.name", Value: []byte(`"Chain II"`)},
	)
	parsed, err := ParseManifest(out)
	if err != nil {
		t.Fatalf("ParseManifest() error: %v\n%s", err, out)
	}
	inf := parsed.Spec.Permissions.Inference
	if len(inf.Models) != 1 || inf.Models[0] != "clip_vit_b_32" {
		t.Errorf("models = %v\n%s", inf.Models, out)
	}
	if inf.MaxQPS != 5 {
		t.Errorf("max_qps = %d, want 5\n%s", inf.MaxQPS, out)
	}
	if parsed.Metadata.Name != "Chain II" {
		t.Errorf("name = %q\n%s", parsed.Metadata.Name, out)
	}
}

func TestPatchDocumentInvalidInputs(t *testing.T) {
	if _, err := PatchDocument([]byte("not: [valid"), nil); err == nil {
		t.Error("expected error for invalid YAML source")
	}
	if _, err := PatchDocument([]byte("- a\n- b\n"), nil); err == nil {
		t.Error("expected error for non-mapping root")
	}
	if _, err := PatchDocument([]byte(patchSrc), []FieldPatch{{Path: "metadata.name", Value: []byte(`{broken`)}}); err == nil {
		t.Error("expected error for invalid JSON value")
	}
}

// Replaced values keep their inline comment.
func TestPatchDocumentCarriesValueComments(t *testing.T) {
	src := `apiVersion: v1
kind: Application
metadata:
  id: cmt-app
  name: Old # keep this comment
  version: 1.0.0
spec:
  image: alpine:latest
`
	out := mustPatch(t, src, FieldPatch{Path: "metadata.name", Value: []byte(`"New"`)})
	if !strings.Contains(string(out), "# keep this comment") {
		t.Errorf("inline comment on replaced value lost:\n%s", out)
	}
	parsed, err := ParseManifest(out)
	if err != nil {
		t.Fatalf("ParseManifest() error: %v", err)
	}
	if parsed.Metadata.Name != "New" {
		t.Errorf("name = %q, want New\n%s", parsed.Metadata.Name, out)
	}
}

// A null value deletes the target key instead of writing a null scalar:
// clearing spec.models removes the subtree, clearing an absent key is a
// no-op, and siblings survive both.
func TestPatchDocumentNullDeletesKey(t *testing.T) {
	set := mustPatch(t, patchSrc,
		FieldPatch{Path: "spec.models", Value: []byte(`{"detector":{"id":"yolov8s-640","path":"/opt/models/yolov8s.bin","required":true}}`)})
	parsed, err := ParseManifest(set)
	if err != nil {
		t.Fatalf("ParseManifest(set) error: %v\n%s", err, set)
	}
	if m := parsed.Spec.Models["detector"]; m.ID != "yolov8s-640" || m.Path != "/opt/models/yolov8s.bin" || !m.Required {
		t.Fatalf("models round-trip lost subfields: %+v\n%s", m, set)
	}

	cleared := mustPatch(t, string(set), FieldPatch{Path: "spec.models", Value: []byte(`null`)})
	if strings.Contains(string(cleared), "models") {
		t.Errorf("null patch should delete spec.models:\n%s", cleared)
	}
	if !strings.Contains(string(cleared), "unknown_spec:") {
		t.Errorf("sibling subtree lost on delete:\n%s", cleared)
	}
	parsed, err = ParseManifest(cleared)
	if err != nil {
		t.Fatalf("ParseManifest(cleared) error: %v\n%s", err, cleared)
	}
	if len(parsed.Spec.Models) != 0 {
		t.Errorf("models = %+v after clear\n%s", parsed.Spec.Models, cleared)
	}

	// null on a key that never existed: no-op, nothing appended.
	noop := mustPatch(t, patchSrc, FieldPatch{Path: "spec.models", Value: []byte(`null`)})
	if strings.Contains(string(noop), "models") {
		t.Errorf("null patch on absent key created a node:\n%s", noop)
	}
}
