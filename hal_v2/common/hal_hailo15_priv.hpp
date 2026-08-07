/**
 * @file hal_hailo15_priv.hpp
 * @brief Internal Hailo15 helpers shared across modules.
 *
 * This header is private to the HAL implementation. It must not be included by
 * public API headers.
 */

#pragma once

#include <memory>

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
#include <hailo_postprocess_tools/objects/hailo_objects.hpp>
#endif

namespace hal_v2::hailo15
{

struct TensorPriv
{
    std::shared_ptr<void> holder;

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
    HailoROIPtr roi;
#endif
};

} // namespace hal_v2::hailo15

