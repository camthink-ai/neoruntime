#include "model_variant_validation.h"

#include "postprocess_schema.h"

#include <cctype>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace aipc::ai_runtime {
namespace {

// All membership tables — model types, detection backends, the closed
// detection variant schema, the keypoint informational key set, the loader
// control-key blacklist, the full decoder matrix with per-key shapes —
// come from the generated postprocess_schema.h, rendered from
// platform/postprocess (registry.go): the single source of truth shared
// with the Go side. Hand edits here have no effect; change the registry
// and run `make postprocess-schema`.
using postprocess_schema::kDecoders;
using postprocess_schema::kDecodersCount;
using postprocess_schema::kDetectionBackends;
using postprocess_schema::kDetectionBackendsCount;
using postprocess_schema::kDetectionSchemaList;
using postprocess_schema::kDetectionVariantKeys;
using postprocess_schema::kDetectionVariantKeysCount;
using postprocess_schema::kForbiddenVariantKeys;
using postprocess_schema::kForbiddenVariantKeysCount;
using postprocess_schema::kModelTypes;
using postprocess_schema::kModelTypesCount;

constexpr int kMaxJsonDepth = 32;  // flat schema; deeper nesting is malformed

// Comma-joined whitelist for refusal messages — joined from the generated
// table so a registry change flows into the error text without a second
// hand-maintained copy.
std::string join_detection_backends() {
    std::string out;
    for (std::size_t i = 0; i < kDetectionBackendsCount; ++i) {
        if (!out.empty()) out += ", ";
        out += kDetectionBackends[i];
    }
    return out;
}

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

// Loader control keys are refused wherever they appear STRUCTURALLY — as a
// top-level member or nested at any depth inside objects and arrays. The
// keypoint blacklist must be depth-independent because HAL's
// json_extract_string_best_effort substring-searches the raw JSON text, so
// {"extra":{"backend_lib_path":…}} reaches dlopen unless every layer
// refuses it (review 2026-09-21 P0). A key spelled inside a JSON string
// VALUE is not structural (its quotes are escaped as \" in the raw text,
// breaking the search pattern) and stays legal.
bool is_forbidden_key(const std::string& key) {
    for (std::size_t i = 0; i < kForbiddenVariantKeysCount; ++i)
        if (key == kForbiddenVariantKeys[i]) return true;
    return false;
}

// First structural hit wins; the refusal message names it.
void record_forbidden(std::string* forbidden, const std::string& key) {
    if (forbidden != nullptr && forbidden->empty()) *forbidden = key;
}

// Skips one JSON value of any type. str_out captures a string value's
// content (backend_function); forbidden collects the first loader control
// key found structurally inside objects/arrays. Returns false on any
// structural anomaly.
bool scan_value(Scan& s, int depth, std::string* str_out = nullptr,
                std::string* forbidden = nullptr) {
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
            std::string key;
            if (!scan_string(s, &key)) return false;
            if (is_forbidden_key(key)) record_forbidden(forbidden, key);
            skip_ws(s);
            if (!eat(s, ':')) return false;
            if (!scan_value(s, depth + 1, nullptr, forbidden)) return false;
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
            if (!scan_value(s, depth + 1, nullptr, forbidden)) return false;
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

// Shape of one top-level value, as the detection schema check needs it:
// which JSON type it is, whether a number token was integral, and whether
// an array held only strings.
struct MemberShape {
    bool is_string = false, is_number = false, is_array = false,
         is_object = false, is_bool = false, is_null = false;
    bool integral = true;           // number token without . e E
    bool array_all_strings = true;  // every element a JSON string
    std::string str;                // string content
};

using MemberShapes = std::unordered_map<std::string, MemberShape>;

// Parses a flat JSON object: fills keys with the top-level key set
// (duplicate keys refuse), shapes with each top-level value's shape, and
// forbidden with the first loader control key found structurally anywhere
// in the blob. Returns false on structural anomaly or trailing garbage
// after the closing brace.
bool parse_flat_object(const std::string& text,
                       std::unordered_set<std::string>* keys,
                       MemberShapes* shapes,
                       std::string* forbidden) {
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
        if (is_forbidden_key(key)) record_forbidden(forbidden, key);
        skip_ws(s);
        if (!eat(s, ':')) return false;
        skip_ws(s);
        if (s.p >= s.end) return false;
        MemberShape& m = (*shapes)[key];
        const char c = *s.p;
        if (c == '"') {
            m.is_string = true;
            if (!scan_string(s, &m.str)) return false;
        } else if (c == '{') {
            m.is_object = true;
            if (!scan_value(s, 1, nullptr, forbidden)) return false;
        } else if (c == '[') {
            m.is_array = true;
            ++s.p;
            skip_ws(s);
            if (!eat(s, ']')) {
                for (;;) {
                    skip_ws(s);
                    if (s.p < s.end && *s.p == '"') {
                        if (!scan_string(s, nullptr)) return false;
                    } else {
                        m.array_all_strings = false;
                        if (!scan_value(s, 1, nullptr, forbidden)) return false;
                    }
                    skip_ws(s);
                    if (eat(s, ',')) continue;
                    if (!eat(s, ']')) return false;
                    break;
                }
            }
        } else if (c == 't' || c == 'f') {
            if (!eat_literal(s, "true") && !eat_literal(s, "false"))
                return false;
            m.is_bool = true;
        } else if (c == 'n') {
            if (!eat_literal(s, "null")) return false;
            m.is_null = true;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            m.is_number = true;
            const char* num_begin = s.p;
            if (!scan_value(s, 1)) return false;
            for (const char* q = num_begin; q != s.p; ++q) {
                if (*q == '.' || *q == 'e' || *q == 'E') {
                    m.integral = false;
                    break;
                }
            }
        } else {
            return false;
        }
        skip_ws(s);
        if (eat(s, ',')) continue;
        if (!eat(s, '}')) return false;
        break;
    }
    skip_ws(s);
    return s.p == s.end;  // trailing garbage refuses
}

// Human phrase for a registry type, for refusal messages.
std::string shape_phrase(const char* type) {
    const std::string t = type;
    if (t == "string_list") return "an array of strings";
    if (t == "integer") return "an integer";
    return "a " + t;
}

// Value-shape check for a successfully parsed detection blob, read from the
// generated decoder matrix — the same registry params the Go boundary
// derives its shape table from. The closed key set alone does not close the
// loader-key vector: a legal key with a container value (labels as an
// object) smuggles forbidden keys structurally into the raw text HAL
// substring-searches.
std::string detection_shape_error(const MemberShapes& shapes) {
    for (std::size_t d = 0; d < kDecodersCount; ++d) {
        if (std::string(kDecoders[d].model_type) != "detection") continue;
        for (std::size_t i = 0; i < kDecoders[d].params_count; ++i) {
            const auto& p = kDecoders[d].params[i];
            const auto it = shapes.find(p.key);
            if (it == shapes.end()) continue;  // missing keys: checked later
            const MemberShape& m = it->second;
            const std::string t = p.type;
            bool ok = true;
            if (t == "string") ok = m.is_string;
            else if (t == "number") ok = m.is_number;
            else if (t == "integer") ok = m.is_number && m.integral;
            else if (t == "string_list") ok = m.is_array && m.array_all_strings;
            else if (t == "boolean") ok = m.is_bool;
            if (!ok)
                return "model_variant JSON key '" + std::string(p.key) +
                       "' must be " + shape_phrase(p.type);
        }
        // All four detection decoders share the seven-key shape table
        // (hailoYoloParams); the first one covers the contract.
        return "";
    }
    return "";
}

// Closed-schema check for a successfully parsed detection blob: exactly the
// seven known keys (unknown first — an injected loader key like
// backend_lib_path is the security-relevant failure — then missing), the
// per-key value shapes, then the backend_function whitelist.
std::string detection_blob_error(const std::unordered_set<std::string>& keys,
                                 const MemberShapes& shapes) {
    for (const auto& key : keys) {
        bool known = false;
        for (std::size_t i = 0; i < kDetectionVariantKeysCount; ++i)
            if (key == kDetectionVariantKeys[i]) { known = true; break; }
        if (!known)
            return "model_variant JSON contains unknown key '" + key +
                   "' — the detection variant schema is closed: " +
                   kDetectionSchemaList +
                   " (loader keys such as backend_lib_path or "
                   "backend_config_path are never accepted)";
    }
    for (std::size_t i = 0; i < kDetectionVariantKeysCount; ++i) {
        if (keys.count(kDetectionVariantKeys[i]) == 0)
            return std::string("model_variant JSON is missing required key '") +
                   kDetectionVariantKeys[i] + "' (closed detection schema: " +
                   kDetectionSchemaList + ")";
    }
    const std::string shape_error = detection_shape_error(shapes);
    if (!shape_error.empty()) return shape_error;
    std::string backend_function;
    const auto it = shapes.find("backend_function");
    if (it != shapes.end()) backend_function = it->second.str;
    if (!is_known_detection_backend(backend_function))
        return "model_variant backend_function '" + backend_function +
               "' is not a known detection backend — known: " +
               join_detection_backends();
    return "";
}

}  // namespace

bool is_known_model_type(const std::string& model_type) {
    const std::string t = lowercased(model_type);
    for (std::size_t i = 0; i < kModelTypesCount; ++i)
        if (t == kModelTypes[i]) return true;
    return false;
}

bool is_known_detection_backend(const std::string& name) {
    for (std::size_t i = 0; i < kDetectionBackendsCount; ++i)
        if (name == kDetectionBackends[i]) return true;
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
            MemberShapes shapes;
            std::string forbidden;
            if (!parse_flat_object(v, &keys, &shapes, &forbidden))
                return "model_variant is not a valid flat JSON object — the "
                       "detection variant schema has exactly 7 top-level "
                       "keys: " + std::string(kDetectionSchemaList);
            return detection_blob_error(keys, shapes);
        }
        if (!is_known_detection_backend(v))
            return "Invalid model_variant '" + v + "' for model_type '" + t +
                   "': a bare variant must name a known backend_function (" +
                   join_detection_backends() +
                   "); anything else must be a full JSON "
                   "config with the keys: " + std::string(kDetectionSchemaList);
        return "";
    }

    if (t == "landmarks" || t == "keypoint") {
        if (v.front() == '{') {
            // Open dialect: unknown ordinary keys travel opaquely to HAL
            // (the legacy keypoint channel — HAL validates content). The
            // one hard refusal is the loader control-key blacklist at ANY
            // structural depth, mirroring the REST validator
            // (handlers/ai_postprocess.go).
            std::unordered_set<std::string> keys;
            MemberShapes shapes;
            std::string forbidden;
            if (!parse_flat_object(v, &keys, &shapes, &forbidden))
                return "model_variant is not a valid flat JSON object — "
                       "keypoint variants are an opaque config blob handed "
                       "to HAL verbatim";
            if (!forbidden.empty())
                return "model_variant JSON key '" + forbidden +
                       "' is never accepted (loader control key)";
            return "";
        }
        return "Invalid model_variant '" + v + "' for model_type '" + t +
               "': keypoint/landmarks variants must be a full JSON config "
               "blob — a bare backend_function name cannot select the "
               "COCO-17 pose decoder and the facial-landmarks default would "
               "silently be kept";
    }

    // Other families have no dialect of their own here, but the same
    // any-depth loader-key refusal applies (review 2026-09-24 P0: the
    // pass-through let backend_lib_path / backend_config_path through for
    // types whose variants had no schema at this boundary, while REST
    // already refused them). Bare names still pass — these types have no
    // decoder-selection semantics and init_post_process drops their
    // variants; the guard exists so a future routing change cannot reopen
    // the dlopen vector silently. Malformed `{`-prefixed blobs fail closed,
    // matching REST's validateLoaderKeysOnly.
    if (v.front() == '{') {
        std::unordered_set<std::string> keys;
        MemberShapes shapes;
        std::string forbidden;
        if (!parse_flat_object(v, &keys, &shapes, &forbidden))
            return "model_variant is not a valid flat JSON object — a "
                   "`{`-prefixed variant for this model_type must be valid "
                   "JSON, and loader control keys are refused at any depth";
        if (!forbidden.empty())
            return "model_variant JSON key '" + forbidden +
                   "' is never accepted (loader control key)";
    }
    return "";
}

}  // namespace aipc::ai_runtime
