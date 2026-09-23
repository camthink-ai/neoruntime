package handlers

import "strings"

// loadErrorExplanation maps a raw ai-runtime error signature to an
// actionable explanation. The runtime reports failures in C++ terms
// ("No tensor with name ..."); REST/CLI users need the cause and the
// fix, not the internals. Matching is substring + case-insensitive so
// wrapped errors ("postprocess smoke test failed: ...") still match.
type loadErrorExplanation struct {
	signature   string
	explanation string
}

var loadErrorExplanations = []loadErrorExplanation{
	{
		// Confirmed on device: an HEF compiled with a different
		// postprocess configuration than the profile selected at import.
		signature: "no tensor with name",
		explanation: "the model does not match its configured postprocess profile " +
			"(the HEF lacks the output tensors the profile expects). Re-compile the HEF " +
			"with the matching postprocess, or change the model's postprocess profile",
	},
}

// humanizeLoadError builds the error.detail string for a failed model
// load: the matched explanation first, the raw runtime error demoted
// after it so it stays available for debugging. Unmatched errors pass
// through unchanged — a wrong guess would hide the real cause.
func humanizeLoadError(err error) string {
	if err == nil {
		return ""
	}
	raw := err.Error()
	lower := strings.ToLower(raw)
	for _, e := range loadErrorExplanations {
		if strings.Contains(lower, e.signature) {
			return e.explanation + " (runtime error: " + raw + ")"
		}
	}
	return raw
}
