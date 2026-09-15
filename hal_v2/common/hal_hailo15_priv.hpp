/**
 * @file hal_hailo15_priv.hpp
 * @brief Internal Hailo15 helpers shared across modules.
 *
 * This header is private to the HAL implementation. It must not be included by
 * public API headers.
 */

#pragma once

#include <memory>

#include "model/hal_inference.h" /* HalDmaFrameDesc (public dma-bind contract) */

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
#include <hailo_postprocess_tools/objects/hailo_objects.hpp>
#endif

namespace hal_v2::hailo15
{

struct TensorPriv
{
    std::shared_ptr<void> holder;

    /* Non-NULL only on tensors produced by HAL_INFERENCE_OPS.bind_dma_frame():
     * owned copy of the caller's frame descriptor; the dmabuf fds themselves
     * stay caller-owned (borrowed). Deleted by free_tensor(). */
    HalDmaFrameDesc *dma_frame = nullptr;

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
    HailoROIPtr roi;
#endif
};

} // namespace hal_v2::hailo15

