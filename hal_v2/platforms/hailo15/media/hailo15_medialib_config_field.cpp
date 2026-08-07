/**
 * @file hailo15_medialib_config_field.cpp
 * @brief Implementation of hailo15_patch_profile_field().
 *
 * Strategy: serialise the config_profile_t to nlohmann::json (to_json, from libmedialib),
 * resolve the dotted field path inside that JSON (stripping a conceptual "frontend." /
 * "encoder." namespace and auto-discovering the real parent section from the config content),
 * set the typed value, then deserialise back (from_json).
 */
#include "hailo15_medialib_config_field.hpp"

#include "common/hal_log.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::string trim(const std::string &s)
{
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos)
        return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

bool iequals(const std::string &a, const std::string &b)
{
    if (a.size() != b.size())
        return false;
    return std::equal(a.begin(), a.end(), b.begin(), b.end(),
                      [](char ca, char cb) { return std::tolower(static_cast<unsigned char>(ca)) ==
                                             std::tolower(static_cast<unsigned char>(cb)); });
}

std::vector<std::string> split_path(const std::string &field_path)
{
    std::vector<std::string> segments;
    std::string token;
    for (char c : field_path)
    {
        if (c == '.')
        {
            if (!token.empty())
            {
                segments.push_back(token);
                token.clear();
            }
        }
        else
        {
            token.push_back(c);
        }
    }
    if (!token.empty())
        segments.push_back(token);
    return segments;
}

// Strip a leading conceptual namespace token ("frontend" / "encoder") if present.
// These scope a field to a config section but are never real profile JSON keys.
std::vector<std::string> normalize_segments(std::vector<std::string> segments)
{
    static const char *const namespace_tokens[] = {"frontend", "encoder"};
    if (!segments.empty())
    {
        for (const char *ns : namespace_tokens)
        {
            if (iequals(segments.front(), ns))
            {
                segments.erase(segments.begin());
                break;
            }
        }
    }
    return segments;
}

// Read-only navigation following a list of keys; returns pointer to the value or nullptr.
const nlohmann::json *navigate(const nlohmann::json &node, const std::vector<std::string> &keys)
{
    const nlohmann::json *cur = &node;
    for (const auto &key : keys)
    {
        if (!cur->is_object() || !cur->contains(key))
            return nullptr;
        cur = &cur->at(key);
    }
    return cur;
}

// Try direct navigation from the root; returns the full key path on success, else empty.
std::vector<std::string> resolve_direct(const nlohmann::json &root, const std::vector<std::string> &segments)
{
    if (navigate(root, segments) != nullptr)
        return segments;
    return {};
}

// Recursive descent: find the first node from which the full segment chain matches
// consecutively; returns the full root-relative key path to the leaf, else empty.
std::vector<std::string> resolve_recursive(const nlohmann::json &node,
                                           const std::vector<std::string> &segments,
                                           std::vector<std::string> &current_path)
{
    if (navigate(node, segments) != nullptr)
    {
        std::vector<std::string> full_path = current_path;
        full_path.insert(full_path.end(), segments.begin(), segments.end());
        return full_path;
    }
    if (node.is_object())
    {
        for (auto it = node.begin(); it != node.end(); ++it)
        {
            current_path.push_back(it.key());
            auto found = resolve_recursive(it.value(), segments, current_path);
            if (!found.empty())
                return found;
            current_path.pop_back();
        }
    }
    return {};
}

// Convert a string-encoded value to a JSON value of the requested type.
nlohmann::json parse_typed_value(HalConfigFieldType type, const std::string &value)
{
    const std::string trimmed = trim(value);
    switch (type)
    {
    case HAL_CONFIG_FIELD_BOOL:
        if (iequals(trimmed, "true") || trimmed == "1")
            return true;
        if (iequals(trimmed, "false") || trimmed == "0")
            return false;
        throw std::invalid_argument("Invalid boolean value '" + value + "' (expected true/false/1/0)");
    case HAL_CONFIG_FIELD_INT32:
    {
        errno = 0;
        char *end = nullptr;
        long long v = std::strtoll(trimmed.c_str(), &end, 10);
        if (errno != 0 || end == trimmed.c_str() || *end != '\0')
            throw std::invalid_argument("Invalid int32 value '" + value + "'");
        return static_cast<int32_t>(v);
    }
    case HAL_CONFIG_FIELD_UINT32:
    {
        errno = 0;
        char *end = nullptr;
        unsigned long long v = std::strtoull(trimmed.c_str(), &end, 10);
        if (errno != 0 || end == trimmed.c_str() || *end != '\0')
            throw std::invalid_argument("Invalid uint32 value '" + value + "'");
        return static_cast<uint32_t>(v);
    }
    case HAL_CONFIG_FIELD_FLOAT64:
    {
        errno = 0;
        char *end = nullptr;
        double v = std::strtod(trimmed.c_str(), &end);
        if (errno != 0 || end == trimmed.c_str() || *end != '\0')
            throw std::invalid_argument("Invalid float64 value '" + value + "'");
        return v;
    }
    case HAL_CONFIG_FIELD_STRING:
        return value;
    default:
        throw std::invalid_argument("Unsupported config field type");
    }
}

// Resolve a dotted field path inside a profile JSON. Returns the full root-relative key
// path to the leaf, or an empty vector if not found. Shared by patch and read.
std::vector<std::string> resolve_field(const nlohmann::json &root, const std::string &field_path)
{
    auto segments = normalize_segments(split_path(field_path));
    if (segments.empty())
        return {};
    std::vector<std::string> resolved = resolve_direct(root, segments);
    if (resolved.empty())
    {
        std::vector<std::string> current_path;
        resolved = resolve_recursive(root, segments, current_path);
    }
    return resolved;
}

// Classify a scalar JSON value into a HalConfigFieldType.
HalConfigFieldType classify_value(const nlohmann::json &v)
{
    if (v.is_boolean())
        return HAL_CONFIG_FIELD_BOOL;
    if (v.is_string())
        return HAL_CONFIG_FIELD_STRING;
    if (v.is_number_float())
        return HAL_CONFIG_FIELD_FLOAT64;
    if (v.is_number_integer() && v.get<int64_t>() < 0)
        return HAL_CONFIG_FIELD_INT32;
    return HAL_CONFIG_FIELD_UINT32;
}

// Stringify a scalar JSON value (booleans -> "true"/"false", strings verbatim, numbers minimal).
std::string stringify_value(const nlohmann::json &v)
{
    if (v.is_boolean())
        return v.get<bool>() ? "true" : "false";
    if (v.is_string())
        return v.get<std::string>();
    return v.dump();
}
} // namespace

bool hailo15_patch_profile_field(config_profile_t &profile,
                                 const std::string &field_path,
                                 HalConfigFieldType type,
                                 const std::string &value_str,
                                 std::string *err_out)
{
    try
    {
        nlohmann::json patched = nlohmann::json(profile); // to_json (libmedialib)

        std::vector<std::string> resolved = resolve_field(patched, field_path);
        if (resolved.empty())
            throw std::invalid_argument("Empty or unresolvable config field path '" + field_path + "'");

        nlohmann::json value = parse_typed_value(type, value_str);

        nlohmann::json *cur = &patched;
        for (size_t i = 0; i + 1 < resolved.size(); ++i)
            cur = &cur->at(resolved[i]);
        if (!cur->is_object())
            throw std::invalid_argument("Parent of config field '" + field_path + "' is not an object");
        (*cur)[resolved.back()] = value;

        profile = patched.get<config_profile_t>(); // from_json (libmedialib)
        return true;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_WARNING("hailo15_media: set_config_field: patch failed for '%s': %s",
                        field_path.c_str(), e.what());
        if (err_out)
            *err_out = e.what();
        return false;
    }
}

bool hailo15_read_profile_field(const std::string &profile_json_str,
                                const std::string &field_path,
                                HalConfigFieldType *type_out,
                                std::string &value_out,
                                std::string *err_out)
{
    try
    {
        nlohmann::json root = nlohmann::json::parse(profile_json_str);

        std::vector<std::string> resolved = resolve_field(root, field_path);
        if (resolved.empty())
            throw std::invalid_argument("Empty or unresolvable config field path '" + field_path + "'");

        const nlohmann::json *cur = &root;
        for (const auto &key : resolved)
            cur = &cur->at(key);

        if (cur->is_object() || cur->is_array() || cur->is_null())
            throw std::invalid_argument("Config field '" + field_path + "' is not a scalar");

        if (type_out)
            *type_out = classify_value(*cur);
        value_out = stringify_value(*cur);
        return true;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_WARNING("hailo15_media: get_config_field: read failed for '%s': %s",
                        field_path.c_str(), e.what());
        if (err_out)
            *err_out = e.what();
        return false;
    }
}
