#include "model_variant_validation.h"

#include <cctype>
#include <cstring>
#include <unordered_set>

namespace aipc::ai_runtime {
namespace {

// Type strings init_post_process (model_manager.cpp) dispatches on, verbatim.
// NOTE: "landmarks" is plural-only there — a singular "landmark" would fall
// through to the silent detection default, which is exactly what this
// validator exists to catch at the boundary.
const char* const kKnownModelTypes[] = {
    "detection",     "yolo",           "landmarks",       "keypoint",
    "segmentation",  "classification", "clip",            "embedding",
    "depth",         "monocular_depth", "scdepth",        "ocr_detection",
    "ocr_recognition",
};

// Detection backend_function whitelist. MUST stay in sync with platform-api
// DetectionPostprocessProfiles (models/model_types.go) and the REST
// validator's LookupDetectionBackendFunction (handlers/ai_postprocess.go).
const char* const kDetectionBackends[] = {
    "hailo_yolov8n", "hailo_yolov8s", "hailo_yolov8m", "yolov5m_vehicles",
};

// The closed detection variant schema — exactly these keys, no others, no
// nesting at the top level (mirrors handlers/ai_postprocess.go
// customVariantKeys and the composed blob in model_manager.cpp).
const char* const kDetectionVariantKeys[] = {
    "backend_function", "iou_threshold",    "detection_threshold",
    "output_activation", "label_offset",    "max_boxes",
    "labels",
};

constexpr const char* kDetectionSchemaList =
    "backend_function, iou_threshold, detection_threshold, "
    "output_activation, label_offset, max_boxes, labels";

constexpr int kMaxJsonDepth = 32;  // flat schema; deeper nesting is malformed

std::string lowercased(std::string s) {
    for (char& c : s)
        c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string trimmed(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// ── Minimal fail-closed JSON structural scanner ─────────────────────────────
// ai-runtime links no JSON library, and only two facts about the blob matter
// here: the set of top-level keys and the backend_function string. Anything
// structurally suspicious is refused rather than guessed at (a wrong guess
// here means either a refused-good or accepted-bad variant at registration).

struct Scan {
    const char* p;
    const char* end;
};

void skip_ws(Scan& s) {
    while (s.p < s.end && (*s.p == ' ' || *s.p == '\t' || *s.p == '\n' ||
                           *s.p == '\r'))
        ++s.p;
}

bool eat(Scan& s, char c) {
    if (s.p < s.end && *s.p == c) {
        ++s.p;
        return true;
    }
    return false;
}

bool eat_literal(Scan& s, const char* lit) {
    const size_t n = std::strlen(lit);
    if (static_cast<size_t>(s.end - s.p) < n) return false;
    if (std::strncmp(s.p, lit, n) != 0) return false;
    s.p += n;
    return true;
}

// Scans a JSON string starting at the opening quote; when out is non-null it
// receives the unescaped content. Returns false on unterminated strings,
// invalid escapes or raw control characters (strict JSON).
bool scan_string(Scan& s, std::string* out) {
    if (!eat(s, '"')) return false;
    if (out) out->clear();
    for (;;) {
        if (s.p >= s.end) return false;  // unterminated
        char c = *s.p++;
        if (c == '"') return true;
        if (c == '\\') {
            if (s.p >= s.end) return false;
            char e = *s.p++;
            switch (e) {
                case '"': case '\\': case '/':
                    if (out) *out += e;
                    break;
                case 'b': if (out) *out += '\b'; break;
                case 'f': if (out) *out += '\f'; break;
                case 'n': if (out) *out += '\n'; break;
                case 'r': if (out) *out += '\r'; break;
                case 't': if (out) *out += '\t'; break;
                case 'u':
                    // Only structural validity matters; \uXXXX cannot occur
                    // in a whitelisted backend_function, so it collapses.
                    if (s.end - s.p < 4) return false;
                    for (int i = 0; i < 4; ++i, ++s.p)
                        if (!std::isxdigit(static_cast<unsigned char>(*s.p)))
                            return false;
                    if (out) *out += '?';
                    break;
                default:
                    return false;
            }
        } else if (static_cast<unsigned char>(c) < 0x20) {
            return false;  // raw control character
        } else if (out) {
            *out += c;
        }
    }
}

// Skips one JSON value of any type (strings are the only values whose
// content matters, and only when str_out captures backend_function).
// Returns false on any structural anomaly.
bool scan_value(Scan& s, int depth, std::string* str_out = nullptr) {
    if (depth > kMaxJsonDepth) return false;
    skip_ws(s);
    if (s.p >= s.end) return false;
    const char c = *s.p;
    if (c == '"') return scan_string(s, str_out);
    if (c == '{') {
        ++s.p;
        skip_ws(s);
        if (eat(s, '}')) return true;
        for (;;) {
            skip_ws(s);
            if (!scan_string(s, nullptr)) return false;
            skip_ws(s);
            if (!eat(s, ':')) return false;
            if (!scan_value(s, depth + 1)) return false;
            skip_ws(s);
            if (eat(s, ',')) continue;
            return eat(s, '}');
        }
    }
    if (c == '[') {
        ++s.p;
        skip_ws(s);
        if (eat(s, ']')) return true;
        for (;;) {
            if (!scan_value(s, depth + 1)) return false;
            skip_ws(s);
            if (eat(s, ',')) continue;
            return eat(s, ']');
        }
    }
    if (c == 't') return eat_literal(s, "true");
    if (c == 'f') return eat_literal(s, "false");
    if (c == 'n') return eat_literal(s, "null");
    if (c == '-' || (c >= '0' && c <= '9')) {
        bool digits = false;
        eat(s, '-');
        while (s.p < s.end && std::isdigit(static_cast<unsigned char>(*s.p))) {
            ++s.p;
            digits = true;
        }
        if (!digits) return false;
        if (eat(s, '.')) {
            bool frac = false;
            while (s.p < s.end &&
                   std::isdigit(static_cast<unsigned char>(*s.p))) {
                ++s.p;
                frac = true;
            }
            if (!frac) return false;
        }
        if (s.p < s.end && (*s.p == 'e' || *s.p == 'E')) {
            ++s.p;
            eat(s, '+') || eat(s, '-');
            bool exp_digits = false;
            while (s.p < s.end &&
                   std::isdigit(static_cast<unsigned char>(*s.p))) {
                ++s.p;
                exp_digits = true;
            }
            if (!exp_digits) return false;
        }
        return true;
    }
    return false;
}

// Parses a flat JSON object: fills keys with the top-level key set (duplicate
// keys refuse) and, when present, backend_function's string value (a
// non-string backend_function refuses). Returns false on structural anomaly
// or trailing garbage after the closing brace.
bool parse_flat_object(const std::string& text,
                       std::unordered_set<std::string>* keys,
                       std::string* backend_function) {
    Scan s{text.data(), text.data() + text.size()};
    skip_ws(s);
    if (!eat(s, '{')) return false;
    skip_ws(s);
    if (eat(s, '}')) return true;  // {} — the schema check reports the gaps
    for (;;) {
        skip_ws(s);
        std::string key;
        if (!scan_string(s, &key)) return false;
        if (!keys->insert(key).second) return false;  // duplicate key
        skip_ws(s);
        if (!eat(s, ':')) return false;
        if (key == "backend_function") {
            skip_ws(s);
            // Must be a string; anything else cannot name a dlsym symbol.
            if (s.p >= s.end || *s.p != '"') return false;
            if (!scan_string(s, backend_function)) return false;
        } else {
            if (!scan_value(s, 1)) return false;
        }
        skip_ws(s);
        if (eat(s, ',')) continue;
        if (!eat(s, '}')) return false;
        break;
    }
    skip_ws(s);
    return s.p == s.end;  // trailing garbage refuses
}

// Closed-schema check for a successfully parsed detection blob: exactly the
// seven known keys (unknown first — an injected loader key like
// backend_lib_path is the security-relevant failure — then missing), then
// the backend_function whitelist.
std::string detection_blob_error(const std::unordered_set<std::string>& keys,
                                 const std::string& backend_function) {
    for (const auto& key : keys) {
        bool known = false;
        for (const char* k : kDetectionVariantKeys)
            if (key == k) { known = true; break; }
        if (!known)
            return "model_variant JSON contains unknown key '" + key +
                   "' — the detection variant schema is closed: " +
                   kDetectionSchemaList +
                   " (loader keys such as backend_lib_path or "
                   "backend_config_path are never accepted)";
    }
    for (const char* k : kDetectionVariantKeys) {
        if (keys.count(k) == 0)
            return std::string("model_variant JSON is missing required key '") +
                   k + "' (closed detection schema: " + kDetectionSchemaList +
                   ")";
    }
    if (!is_known_detection_backend(backend_function))
        return "model_variant backend_function '" + backend_function +
               "' is not a known detection backend — known: "
               "hailo_yolov8n, hailo_yolov8s, hailo_yolov8m, yolov5m_vehicles";
    return "";
}

}  // namespace

bool is_known_model_type(const std::string& model_type) {
    const std::string t = lowercased(model_type);
    for (const char* k : kKnownModelTypes)
        if (t == k) return true;
    return false;
}

bool is_known_detection_backend(const std::string& name) {
    for (const char* k : kDetectionBackends)
        if (name == k) return true;
    return false;
}

std::string validate_model_variant(const std::string& model_type,
                                   const std::string& variant) {
    const std::string t = lowercased(model_type);
    const std::string v = trimmed(variant);
    if (v.empty()) return "";  // no variant: per-type defaults apply

    if (t == "detection" || t == "yolo") {
        if (v.front() == '{') {
            std::unordered_set<std::string> keys;
            std::string backend_function;
            if (!parse_flat_object(v, &keys, &backend_function))
                return "model_variant is not a valid flat JSON object — the "
                       "detection variant schema has exactly 7 top-level "
                       "keys: " + std::string(kDetectionSchemaList);
            return detection_blob_error(keys, backend_function);
        }
        if (!is_known_detection_backend(v))
            return "Invalid model_variant '" + v + "' for model_type '" + t +
                   "': a bare variant must name a known backend_function "
                   "(hailo_yolov8n, hailo_yolov8s, hailo_yolov8m, "
                   "yolov5m_vehicles); anything else must be a full JSON "
                   "config with the keys: " + std::string(kDetectionSchemaList);
        return "";
    }

    if (t == "landmarks" || t == "keypoint") {
        if (v.front() == '{') return "";  // blob: HAL/plugin validates content
        return "Invalid model_variant '" + v + "' for model_type '" + t +
               "': keypoint/landmarks variants must be a full JSON config "
               "blob — a bare backend_function name cannot select the "
               "COCO-17 pose decoder and the facial-landmarks default would "
               "silently be kept";
    }

    // Other families: pass through. Their variants are config blobs whose
    // content HAL and the plugin validate; restricting them here would break
    // forward compatibility for no silent-no-op gain.
    return "";
}

}  // namespace aipc::ai_runtime
