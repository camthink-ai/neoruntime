// Package postprocess is the single source of truth for postprocess decoder
// capabilities across the platform: which decoders exist per model type, which
// configuration keys each decoder actually consumes versus merely accepts,
// which values are hardcoded, and which keys each variant dialect recognizes.
//
// It is a leaf package on purpose — it imports nothing else in this module —
// so the model package, the REST handlers, modelload and the schema generator
// can all depend on it without cycles.
//
// Generated artifacts (checked in, regenerated via `make postprocess-schema`):
//
//	platform/postprocess/generated/postprocess_schema.json
//	platform/ai-runtime/include/postprocess_schema.h
//
// The C++ side consumes the generated header instead of hand-maintained
// tables, so a schema change lands on both sides from one edit here and a
// regeneration; CI fails when either artifact drifts from the registry.
//
// Capability facts are verified against the vendor postprocess library
// baseline hailo-media-library 1.12.1 (see
// docs/architecture/unified-postprocess-config-proposal.md §3.1). A vendor
// library upgrade is the trigger to re-verify the consumed/hardcoded
// classifications. Where consumption is unverified, Effect is conservatively
// EffectAdvisory — an advisory badge in the UI costs nothing, a wrong
// "consumed" claim silently misleads.
//
// # Adding a model type
//
// RuntimeTypes must stay verbatim-identical to the ai-runtime known-type
// table (platform/ai-runtime/src/model_variant_validation.cpp). To add a
// type: (1) decide its RuntimeTypes membership here, (2) teach
// init_post_process in model_manager.cpp to act on it, (3) regenerate the
// schema. The Go-side consistency tests in
// platform/platform-api/model/model_types_postprocess_test.go fail when
// RuntimeTypes and the Go SupportedModelTypes drift apart in either
// direction.
package postprocess
