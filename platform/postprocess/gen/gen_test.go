package main

import (
	"bytes"
	"encoding/json"
	"strings"
	"testing"

	"aipc/platform/postprocess"
)

// The artifacts are checked in and CI regenerates + diffs them — a render
// that flip-flops between runs would make that check unusable noise. Both
// renders must be pure functions of the registry.
func TestRenderDeterministic(t *testing.T) {
	jsonA, err := renderJSON()
	if err != nil {
		t.Fatalf("renderJSON() #1 error: %v", err)
	}
	jsonB, err := renderJSON()
	if err != nil {
		t.Fatalf("renderJSON() #2 error: %v", err)
	}
	if !bytes.Equal(jsonA, jsonB) {
		t.Error("renderJSON() not byte-deterministic across two calls")
	}

	if a, b := renderHeader(), renderHeader(); a != b {
		t.Error("renderHeader() not byte-deterministic across two calls")
	}
}

// The C++ header must carry the registry tables the S6 validator iterates —
// every RuntimeTypes string verbatim, the forbidden loader keys, and the
// decoder matrix with per-decoder param arrays.
func TestRenderHeaderCarriesRegistryTables(t *testing.T) {
	h := renderHeader()

	for _, want := range []string{
		"namespace aipc::ai_runtime::postprocess_schema",
		"kModelTypes",
		"kDetectionBackends",
		"kDetectionVariantKeys",
		"kKeypointVariantKeys",
		"kForbiddenVariantKeys",
		"kDecoders",
		"struct ParamSpec",
		"struct DecoderSpec",
	} {
		if !strings.Contains(h, want) {
			t.Errorf("header missing %q", want)
		}
	}

	// Every runtime type string appears as a literal — the gRPC validator's
	// membership table is only as complete as this.
	for _, s := range postprocess.RuntimeTypes {
		if !strings.Contains(h, `"`+s+`"`) {
			t.Errorf("header missing RuntimeTypes entry %q", s)
		}
	}

	for _, k := range postprocess.ForbiddenVariantKeys {
		if !strings.Contains(h, `"`+k+`"`) {
			t.Errorf("header missing forbidden key %q", k)
		}
	}

	// Each decoder with params gets a named array the DecoderSpec rows point at.
	for _, d := range postprocess.Decoders {
		if !strings.Contains(h, `"`+d.Decoder+"\"") {
			t.Errorf("header missing decoder %q", d.Decoder)
		}
		if len(d.Params) > 0 && !strings.Contains(h, paramsArrayName(d)+"[]") {
			t.Errorf("header missing param array %s[] for %s", paramsArrayName(d), d.Decoder)
		}
	}

	if !strings.HasSuffix(h, "#endif  // AI_RUNTIME_POSTPROCESS_SCHEMA_H_\n") {
		t.Error("header missing include guard suffix")
	}
	if strings.Contains(h, "\r") {
		t.Error("header must use LF endings only")
	}
}

// The JSON artifact feeds schema-driven tooling: it must round-trip the
// registry's key sets exactly, with a trailing newline for text hygiene.
func TestRenderJSONMirrorsRegistry(t *testing.T) {
	blob, err := renderJSON()
	if err != nil {
		t.Fatalf("renderJSON() error: %v", err)
	}
	if !bytes.HasSuffix(blob, []byte("}\n")) {
		t.Error("JSON artifact must end with a trailing LF")
	}

	var doc struct {
		RuntimeTypes         []string          `json:"runtime_types"`
		DetectionVariantKeys []string          `json:"detection_variant_keys"`
		KeypointVariantKeys  []string          `json:"keypoint_variant_keys"`
		ForbiddenVariantKeys []string          `json:"forbidden_variant_keys"`
		Decoders             []json.RawMessage `json:"decoders"`
	}
	if err := json.Unmarshal(blob, &doc); err != nil {
		t.Fatalf("artifact is not valid JSON: %v", err)
	}
	if len(doc.RuntimeTypes) != len(postprocess.RuntimeTypes) {
		t.Errorf("runtime_types = %d entries, want %d", len(doc.RuntimeTypes), len(postprocess.RuntimeTypes))
	}
	if len(doc.DetectionVariantKeys) != len(postprocess.DetectionVariantKeys) {
		t.Errorf("detection_variant_keys = %d entries, want %d",
			len(doc.DetectionVariantKeys), len(postprocess.DetectionVariantKeys))
	}
	if len(doc.KeypointVariantKeys) != len(postprocess.KeypointVariantKeys) {
		t.Errorf("keypoint_variant_keys = %d entries, want %d",
			len(doc.KeypointVariantKeys), len(postprocess.KeypointVariantKeys))
	}
	if len(doc.ForbiddenVariantKeys) != len(postprocess.ForbiddenVariantKeys) {
		t.Errorf("forbidden_variant_keys = %d entries, want %d",
			len(doc.ForbiddenVariantKeys), len(postprocess.ForbiddenVariantKeys))
	}
	if len(doc.Decoders) != len(postprocess.Decoders) {
		t.Errorf("decoders = %d entries, want %d", len(doc.Decoders), len(postprocess.Decoders))
	}
}
