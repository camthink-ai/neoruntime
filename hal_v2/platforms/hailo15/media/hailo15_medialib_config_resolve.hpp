/**
 * @file hailo15_medialib_config_resolve.hpp
 * @brief Resolve medialib JSON for HAL_CODEC_TYPE_HW and HAL_VIDEO_TYPE_CSI (V1-aligned rules).
 */
#pragma once

#include "hailo15_hal_video_codec_ext.h"
#include "common/hal_common.h"
#include "media/hal_codec.h"
#include "media/hal_video.h"

#include <string>

namespace hailo15::cfg
{

bool json_is_full_medialib(const std::string &raw);

/** Codec HW: path → json → embedded default + HalCodecConfig patch. */
int resolve_hw_encoder_json(const HalCodecConfig *cfg, const Hailo15HalCodecPrivExt *ext, std::string *out_json,
                            HalCodecConfig *effective_out);

/** CSI mode A: tiered config strings → frontend schema JSON. */
int resolve_csi_frontend_json(const HalVideoConfig *cfg, const Hailo15HalVideoPrivExt *ext, std::string *out_json,
                              HalVideoConfig *effective_out);

/** CSI mode B: tiered config → string for MediaLibrary::initialize. */
int resolve_csi_medialib_json(const HalVideoConfig *cfg, const Hailo15HalVideoPrivExt *ext, std::string *out_json,
                              HalVideoConfig *effective_out);

/** Patch frontend JSON dimensions / fps / pool from HalVideoConfig non-zero fields. */
int apply_hal_video_to_frontend_json_string(std::string *json_io, const HalVideoConfig *patch);

} // namespace hailo15::cfg
