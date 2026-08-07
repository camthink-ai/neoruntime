/**
 * @file hailo15_default_medialib.hpp
 * @brief Materialize the compiled-in default media-library config at runtime.
 *
 * The Hailo media library requires each profile to carry a readable on-disk config_file
 * (it reads + flattens it at init), so a config cannot live purely in memory. To keep the
 * HAL self-contained, a build-time generator (gen_default_medialib_bundle.py) bakes the SDK
 * webserver config for a fixed set of profiles into the .so as @ref kHailo15DefaultMedialibBundle,
 * with every internal reference already rewritten to point under a scratch root.
 *
 * hailo15_materialize_default_medialib_config() extracts those files to the scratch root and
 * returns the (self-contained) container JSON, which can be fed straight to
 * MediaLibrary::initialize(). Runtime-only data files (sensor calibration, HEF networks, EIS
 * calibration) are intentionally left pointing at their on-device paths.
 */
#pragma once

#include <string>

namespace hailo15
{
/**
 * Extract the compiled-in default medialib config to its scratch dir and return the
 * self-contained container JSON.
 *
 * @param container_json_out  Receives the container JSON string (profiles reference the
 *                            extracted scratch files).
 * @param err_out             Optional human-readable error message on failure.
 * @return true on success, false on failure.
 */
bool materialize_default_medialib_config(std::string &container_json_out, std::string *err_out = nullptr);
} // namespace hailo15
