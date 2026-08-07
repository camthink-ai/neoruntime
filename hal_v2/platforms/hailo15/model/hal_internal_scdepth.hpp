/**
 * @file hal_internal_scdepth.hpp
 * @brief Built-in SCDepthV3-style monocular depth decode (no vendor depth .so).
 *
 * Matches hailo-apps @c postprocess/cpp/depth_estimation.cpp and @c cpp/depth_estimation_mono/mono_depth_estimation.cpp
 * (tensor @c scdepthv3/conv31, inverse-depth to meters).
 */
#pragma once

#include "model/hal_postprocess.h"

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)

#include <hailo_postprocess_tools/objects/hailo_objects.hpp>

namespace hal_v2::internal_scdepth
{

/** Decode depth logits tensor on @p roi into dense @p out.depth_m (meters). */
int run(const HalPostprocessConfig &cfg, HailoROIPtr roi, HalDepthResult *out);

} // namespace hal_v2::internal_scdepth

#endif /* HAL_HAVE_HAILO_POSTPROCESS_TOOLS */
