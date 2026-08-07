/**
 * @file hailo15_medialib_config_field.hpp
 * @brief Patch a single field of a media-library profile (config_profile_t) by dotted path.
 *
 * Lives in the HAL layer (hal_v2) on top of the unmodified Hailo media library.
 * The caller supplies a field path (e.g. "frontend.hailort.use-hailort-service"), a type
 * and a string-encoded value; this locates the field inside the profile's JSON form,
 * converts the value and writes it back, returning the modified profile.
 */
#pragma once

#include <string>

#include "media/hal_media.h"                               // HalConfigFieldType
#include <nlohmann/json.hpp>
#include <hailo/media_library/media_library_api_types.hpp> // config_profile_t

// Forward declarations: to_json/from_json for config_profile_t are defined inside
// libmedialib (config_type_conversions). Declaring them here lets this translation unit
// (de)serialise a config_profile_t to/from nlohmann::json without pulling in the private
// medialib header; the symbols are resolved from the linked libmedialib.
void to_json(nlohmann::json &j, const config_profile_t &profile);
void from_json(const nlohmann::json &j, config_profile_t &profile);

/**
 * Patch a single field of a media-library profile (in place) by dotted path.
 *
 * Path resolution (@p field_path is a dotted path, segments separated by '.'):
 *  - The actual location of the field is discovered from the real config content. A leading
 *    conceptual namespace token may be used to scope the field and is stripped before lookup:
 *      "frontend.<rest>"  -> frontend-config scoped (e.g. "frontend.hailort.use-hailort-service"
 *                            is auto-located under "application_settings").
 *      "encoder.<rest>"   -> encoder-config scoped.
 *  - An explicit profile path also works, e.g. "application_settings.hailort.use-hailort-service".
 *
 * @param profile     Profile to modify (serialised to JSON, patched, deserialised back).
 * @param field_path  Dotted field path (see resolution rules above).
 * @param type        Value type; controls how @p value_str is interpreted.
 * @param value_str   Value encoded as a string.
 * @param err_out     If non-NULL and the function fails, receives a human-readable message.
 * @return true on success, false on failure (unresolvable path or value that does not match type).
 */
bool hailo15_patch_profile_field(config_profile_t &profile,
                                 const std::string &field_path,
                                 HalConfigFieldType type,
                                 const std::string &value_str,
                                 std::string *err_out = nullptr);

/**
 * Read a single scalar field from a media-library profile JSON string by dotted path.
 *
 * Uses the same path resolution rules as hailo15_patch_profile_field(): a leading
 * "frontend." / "encoder." namespace is stripped and the actual parent section is
 * auto-discovered from the real config content (with a direct navigation tried first).
 *
 * @param profile_json_str  Full profile JSON (e.g. from MediaLibrary::get_current_profile_str()).
 * @param field_path        Dotted field path (see resolution rules above).
 * @param type_out          If non-NULL, receives the detected value type.
 * @param value_out         Receives the value encoded as a string.
 * @param err_out           If non-NULL and the function fails, receives a human-readable message.
 * @return true on success, false on failure (unresolvable path or the field is not a scalar).
 */
bool hailo15_read_profile_field(const std::string &profile_json_str,
                                const std::string &field_path,
                                HalConfigFieldType *type_out,
                                std::string &value_out,
                                std::string *err_out = nullptr);
