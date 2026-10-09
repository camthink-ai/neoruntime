package postprocess

// Mutability declares when a parameter change takes effect. Only
// MutabilityHotUpdate keys can be changed through the runtime's
// apply_config_json path without re-registering the model.
type Mutability string

const (
	// MutabilityCreateOnly is read once when the postprocess session is
	// created (model registration); later changes require a reload.
	MutabilityCreateOnly Mutability = "create_only"
	// MutabilityHotUpdate keys are present in HAL's apply_config_json key
	// tables and take effect on the next frame.
	MutabilityHotUpdate Mutability = "hot_update"
	// MutabilityRequiresReload is accepted for bookkeeping but the decoder
	// reads it only at create time through a non-hot channel.
	MutabilityRequiresReload Mutability = "requires_reload"
)

// Effect declares whether a parameter actually changes decoder behavior.
type Effect string

const (
	// EffectConsumed: the decoder reads the key and behavior changes.
	EffectConsumed Effect = "consumed"
	// EffectAdvisory: the accept layer parses the key without error but the
	// decoder never reads it — filling it changes nothing. The UI must show
	// a "no effect" badge.
	EffectAdvisory Effect = "advisory"
	// EffectMetadata: recorded for consumers (e.g. class-id ↔ label mapping)
	// but not read by the decoder itself.
	EffectMetadata Effect = "metadata"
)

// Selection strategies: how a decoder is chosen inside the variant dialect.
const (
	// SelectionEmptyVariant: the runtime's empty-variant per-type default.
	SelectionEmptyVariant = ""
	// SelectionBackendFunction: the blob's backend_function string routes to
	// the plugin function (detection dialect).
	SelectionBackendFunction = "backend_function"
	// SelectionFlagPrefix: "flag:<json-key>" — a boolean key in the JSON blob
	// flips the decoder at create time (keypoint pose dialect).
	SelectionFlagPrefix = "flag:"
)

// ParamDef describes one configuration parameter of a decoder in the
// platform namespace. DialectKey names the key's spelling inside the variant
// JSON dialect when it differs from Key; empty means identical.
type ParamDef struct {
	Key        string     `json:"key"`
	Type       string     `json:"type"` // number | string | boolean | string_list
	Min        *float64   `json:"min,omitempty"`
	Max        *float64   `json:"max,omitempty"`
	Default    any        `json:"default,omitempty"`
	Mutability Mutability `json:"mutability"`
	Effect     Effect     `json:"effect"`
	DialectKey string     `json:"dialect_key,omitempty"`
	Note       string     `json:"note,omitempty"`
}

// HardcodedDef records a value the decoder fixes at compile time. No
// configuration path can change it; the UI shows it as a fact, never as a
// field.
type HardcodedDef struct {
	Key   string `json:"key"`
	Value string `json:"value"`
	Note  string `json:"note,omitempty"`
}

// DecoderSpec is one decoder under a model type. SelectValue is the value of
// the config.postprocess_profile key that selects this decoder in the wizard
// ("" = implicit single-decoder default). Basename is the HEF basename the
// vendor plugin recognizes (detection only). Selection documents the routing
// mechanism — see the Selection* constants.
type DecoderSpec struct {
	ModelType   string         `json:"model_type"`
	Decoder     string         `json:"decoder"`
	Label       string         `json:"label"`
	SelectValue string         `json:"select_value,omitempty"`
	Basename    string         `json:"basename,omitempty"`
	Selection   string         `json:"selection,omitempty"`
	Custom      bool           `json:"custom,omitempty"`
	Params      []ParamDef     `json:"params,omitempty"`
	Hardcoded   []HardcodedDef `json:"hardcoded,omitempty"`
}

// AliasRule records key spellings that denote the same knob across the
// form/DB-column/variant-dialect namespaces. The canonical spelling is what
// the unified document uses; aliases are accepted on read and folded.
type AliasRule struct {
	Canonical string   `json:"canonical"`
	Aliases   []string `json:"aliases"`
}
