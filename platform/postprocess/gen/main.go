// Command gen renders the postprocess registry into the two checked-in
// artifacts every consumer reads instead of hand-maintained tables:
//
//	platform/postprocess/generated/postprocess_schema.json  (tooling/UI)
//	platform/ai-runtime/include/postprocess_schema.h        (ai-runtime C++)
//
// Both renders are pure functions of the registry — fixed order, no map
// iteration, LF endings — so regeneration is byte-deterministic and CI can
// fail on drift with a plain git diff.
package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"strconv"
	"strings"

	"aipc/platform/postprocess"
)

func main() {
	check := false
	for _, a := range os.Args[1:] {
		if a == "-check" {
			check = true
		} else {
			fmt.Fprintf(os.Stderr, "postprocess-schema: unknown flag %q\n", a)
			os.Exit(2)
		}
	}
	paths, err := resolveArtifactPaths()
	if err == nil {
		if check {
			err = checkArtifacts(paths)
		} else {
			err = writeArtifacts(paths)
		}
	}
	if err != nil {
		fmt.Fprintln(os.Stderr, "postprocess-schema:", err)
		os.Exit(1)
	}
}

// artifactPaths locates the two checked-in outputs relative to this source
// file — the generator runs from the module, never an installed location.
type artifactFiles struct {
	json   string
	header string
}

func resolveArtifactPaths() (artifactFiles, error) {
	_, thisFile, _, ok := runtime.Caller(0)
	if !ok {
		return artifactFiles{}, fmt.Errorf("cannot locate source path")
	}
	pkgDir := filepath.Dir(filepath.Dir(thisFile))
	return artifactFiles{
		json:   filepath.Join(pkgDir, "generated", "postprocess_schema.json"),
		header: filepath.Join(pkgDir, "..", "ai-runtime", "include", "postprocess_schema.h"),
	}, nil
}

func writeArtifacts(p artifactFiles) error {
	jsonBlob, err := renderJSON()
	if err != nil {
		return fmt.Errorf("render JSON: %w", err)
	}
	if err := os.MkdirAll(filepath.Dir(p.json), 0o755); err != nil {
		return fmt.Errorf("mkdir generated: %w", err)
	}
	if err := os.WriteFile(p.json, jsonBlob, 0o644); err != nil {
		return fmt.Errorf("write %s: %w", p.json, err)
	}

	if err := os.MkdirAll(filepath.Dir(p.header), 0o755); err != nil {
		return fmt.Errorf("mkdir include: %w", err)
	}
	if err := os.WriteFile(p.header, []byte(renderHeader()), 0o644); err != nil {
		return fmt.Errorf("write %s: %w", p.header, err)
	}
	return nil
}

// checkArtifacts renders both artifacts in memory and byte-compares them
// with the checked-in files — the drift guard. It never writes: a check
// that regenerated and diffed against HEAD would flag any legitimate
// uncommitted regen as drift, and a restore step would then discard it
// (review 2026-09-21). Failing here means exactly one thing: the files on
// disk do not match what the current registry renders.
func checkArtifacts(p artifactFiles) error {
	jsonBlob, err := renderJSON()
	if err != nil {
		return fmt.Errorf("render JSON: %w", err)
	}
	if err := compareArtifact(p.json, jsonBlob); err != nil {
		return err
	}
	return compareArtifact(p.header, []byte(renderHeader()))
}

func compareArtifact(path string, want []byte) error {
	got, err := os.ReadFile(path)
	if err != nil {
		return fmt.Errorf("%s: %w (run 'make postprocess-schema' to create it)", path, err)
	}
	if !bytes.Equal(got, want) {
		return fmt.Errorf("%s drifted from the registry (run 'make postprocess-schema' and commit the result)", path)
	}
	return nil
}

// schemaDoc is the JSON artifact's top level.
type schemaDoc struct {
	GeneratedFrom        string                    `json:"generated_from"`
	RuntimeTypes         []string                  `json:"runtime_types"`
	DetectionVariantKeys []string                  `json:"detection_variant_keys"`
	KeypointVariantKeys  []string                  `json:"keypoint_variant_keys"`
	ForbiddenVariantKeys []string                  `json:"forbidden_variant_keys"`
	Aliases              []postprocess.AliasRule   `json:"aliases"`
	Decoders             []postprocess.DecoderSpec `json:"decoders"`
}

func renderJSON() ([]byte, error) {
	doc := schemaDoc{
		GeneratedFrom:        "platform/postprocess/registry.go",
		RuntimeTypes:         postprocess.RuntimeTypes,
		DetectionVariantKeys: postprocess.DetectionVariantKeys,
		KeypointVariantKeys:  postprocess.KeypointVariantKeys,
		ForbiddenVariantKeys: postprocess.ForbiddenVariantKeys,
		Aliases:              postprocess.Aliases,
		Decoders:             postprocess.Decoders,
	}
	blob, err := json.MarshalIndent(doc, "", "  ")
	if err != nil {
		return nil, err
	}
	return append(blob, '\n'), nil
}

func renderHeader() string {
	var b strings.Builder
	w := func(format string, args ...interface{}) { fmt.Fprintf(&b, format, args...) }

	w("// GENERATED FILE — DO NOT EDIT.\n")
	w("// Single source of truth: platform/postprocess (registry.go).\n")
	w("// Regenerate with: make postprocess-schema  (go generate ./platform/postprocess)\n")
	w("// CI fails when this file drifts from the registry.\n")
	w("#ifndef AI_RUNTIME_POSTPROCESS_SCHEMA_H_\n")
	w("#define AI_RUNTIME_POSTPROCESS_SCHEMA_H_\n\n")
	w("#include <cstddef>\n\n")
	w("namespace aipc::ai_runtime::postprocess_schema {\n\n")

	w("// Model type strings the gRPC RegisterModel validator accepts, verbatim\n")
	w("// from the registry's RuntimeTypes. An unknown type falls through to the\n")
	w("// silent detection default in init_post_process — membership changes are\n")
	w("// paired with that switch and a regeneration.\n")
	writeStringArray(w, "kModelTypes", postprocess.RuntimeTypes)
	writeCount(w, "kModelTypes")

	detection := postprocess.DecodersForType("detection")
	backends := make([]string, 0, len(detection))
	for _, d := range detection {
		backends = append(backends, d.Decoder)
	}
	w("\n// Detection backend_function whitelist (parameterized plugin entries plus\n")
	w("// the fixed-name customer entry).\n")
	writeStringArray(w, "kDetectionBackends", backends)
	writeCount(w, "kDetectionBackends")

	w("\n// The closed detection variant key schema.\n")
	writeStringArray(w, "kDetectionVariantKeys", postprocess.DetectionVariantKeys)
	writeCount(w, "kDetectionVariantKeys")
	w("inline constexpr const char* kDetectionSchemaList = \"%s\";\n",
		strings.Join(postprocess.DetectionVariantKeys, ", "))

	w("\n// Informational keypoint key set — the dialect itself is OPEN (legacy\n")
	w("// opaque channel; HAL validates content); only the loader keys in\n")
	w("// kForbiddenVariantKeys are refused on every surface.\n")
	writeStringArray(w, "kKeypointVariantKeys", postprocess.KeypointVariantKeys)
	writeCount(w, "kKeypointVariantKeys")

	w("\n// Loader control keys the HAL postprocess layer dlopens from — rejected on\n")
	w("// every variant surface regardless of model type.\n")
	writeStringArray(w, "kForbiddenVariantKeys", postprocess.ForbiddenVariantKeys)
	writeCount(w, "kForbiddenVariantKeys")

	w("\n// Full decoder matrix. Numeric min/max/default are rendered as strings —\n")
	w("// C++ consumers need membership checks, not arithmetic; the JSON artifact\n")
	w("// carries the exact typed values for tooling.\n")
	w("struct ParamSpec {\n")
	w("    const char* key;\n")
	w("    const char* type;\n")
	w("    const char* min;            // \"\" = unbounded\n")
	w("    const char* max;            // \"\" = unbounded\n")
	w("    const char* default_value;  // \"\" = none\n")
	w("    const char* mutability;\n")
	w("    const char* effect;\n")
	w("    const char* dialect_key;    // \"\" = same spelling as key\n")
	w("    const char* note;\n")
	w("};\n\n")
	w("struct HardcodedSpec {\n")
	w("    const char* key;\n")
	w("    const char* value;\n")
	w("    const char* note;\n")
	w("};\n\n")
	w("struct DecoderSpec {\n")
	w("    const char* model_type;\n")
	w("    const char* decoder;\n")
	w("    const char* label;\n")
	w("    const char* select_value;  // \"\" = implicit single-decoder default\n")
	w("    const char* basename;      // detection plugin basenames only, else \"\"\n")
	w("    const char* selection;     // routing mechanism, see the registry\n")
	w("    bool custom;\n")
	w("    const ParamSpec* params;\n")
	w("    std::size_t params_count;\n")
	w("    const HardcodedSpec* hardcoded;\n")
	w("    std::size_t hardcoded_count;\n")
	w("};\n\n")

	for _, d := range postprocess.Decoders {
		if len(d.Params) == 0 {
			continue
		}
		name := paramsArrayName(d)
		w("inline constexpr ParamSpec %s[] = {\n", name)
		for _, p := range d.Params {
			w("    {\"%s\", \"%s\", \"%s\", \"%s\", %s, \"%s\", \"%s\", \"%s\", \"%s\"},\n",
				cppString(p.Key), cppString(p.Type),
				cppNumberPtr(p.Min), cppNumberPtr(p.Max),
				cppDefault(p.Default),
				cppString(string(p.Mutability)), cppString(string(p.Effect)),
				cppString(p.DialectKey), cppString(p.Note))
		}
		w("};\n\n")
	}
	for _, d := range postprocess.Decoders {
		if len(d.Hardcoded) == 0 {
			continue
		}
		name := hardcodedArrayName(d)
		w("inline constexpr HardcodedSpec %s[] = {\n", name)
		for _, h := range d.Hardcoded {
			w("    {\"%s\", \"%s\", \"%s\"},\n", cppString(h.Key), cppString(h.Value), cppString(h.Note))
		}
		w("};\n\n")
	}

	w("inline constexpr DecoderSpec kDecoders[] = {\n")
	for _, d := range postprocess.Decoders {
		w("    {\"%s\", \"%s\", \"%s\", \"%s\", \"%s\", \"%s\", %s, %s, %d, %s, %d},\n",
			cppString(d.ModelType), cppString(d.Decoder), cppString(d.Label),
			cppString(d.SelectValue), cppString(d.Basename), cppString(d.Selection),
			cppBool(d.Custom),
			paramsRef(d), len(d.Params),
			hardcodedRef(d), len(d.Hardcoded))
	}
	w("};\n")
	writeCount(w, "kDecoders")

	w("\n}  // namespace aipc::ai_runtime::postprocess_schema\n\n")
	w("#endif  // AI_RUNTIME_POSTPROCESS_SCHEMA_H_\n")
	return b.String()
}

func writeStringArray(w func(string, ...interface{}), name string, items []string) {
	w("inline constexpr const char* const %s[] = {\n", name)
	for _, s := range items {
		w("    \"%s\",\n", cppString(s))
	}
	w("};\n")
}

func writeCount(w func(string, ...interface{}), name string) {
	w("inline constexpr std::size_t %sCount = sizeof(%s) / sizeof(%s[0]);\n", name, name, name)
}

func paramsArrayName(d postprocess.DecoderSpec) string {
	return "kParams_" + d.ModelType + "_" + d.Decoder
}

func hardcodedArrayName(d postprocess.DecoderSpec) string {
	return "kHardcoded_" + d.ModelType + "_" + d.Decoder
}

func paramsRef(d postprocess.DecoderSpec) string {
	if len(d.Params) == 0 {
		return "nullptr"
	}
	return paramsArrayName(d)
}

func hardcodedRef(d postprocess.DecoderSpec) string {
	if len(d.Hardcoded) == 0 {
		return "nullptr"
	}
	return hardcodedArrayName(d)
}

func cppBool(b bool) string {
	if b {
		return "true"
	}
	return "false"
}

func cppNumberPtr(v *float64) string {
	if v == nil {
		return ""
	}
	return strconv.FormatFloat(*v, 'g', -1, 64)
}

// cppDefault renders a heterogeneous registry default as a C++ string
// literal (or the empty literal when absent).
func cppDefault(v interface{}) string {
	switch t := v.(type) {
	case nil:
		return `""`
	case float64:
		return `"` + strconv.FormatFloat(t, 'g', -1, 64) + `"`
	case int:
		return `"` + strconv.Itoa(t) + `"`
	case bool:
		if t {
			return `"true"`
		}
		return `"false"`
	case string:
		return `"` + cppString(t) + `"`
	case []string:
		return `"` + cppString(strings.Join(t, ",")) + `"`
	default:
		return `""`
	}
}

// cppString escapes for a C++ string literal. Registry strings are plain
// ASCII identifiers/notes; the escapes cover what could ever appear.
func cppString(s string) string {
	r := strings.NewReplacer(`\`, `\\`, `"`, `\"`, "\n", `\n`)
	return r.Replace(s)
}
