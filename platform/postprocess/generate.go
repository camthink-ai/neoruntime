package postprocess

// Regenerates the checked-in schema artifacts consumed by ai-runtime and any
// schema-driven tooling:
//
//	platform/postprocess/generated/postprocess_schema.json
//	platform/ai-runtime/include/postprocess_schema.h
//
// `make postprocess-schema` runs this; CI fails when either artifact drifts
// from the registry (make postprocess-schema-check).
//go:generate go run ./gen
