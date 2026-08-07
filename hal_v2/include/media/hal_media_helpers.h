/**
 * @file hal_media_helpers.h
 * @brief Small helpers for selecting FROM_MEDIA video branches (e.g. dual-stream record + inference).
 *
 * Typical pattern (see `hal/test_example/record_and_inference`): one high-res frontend for display/encode and one
 * frontend at **exact** neural network input width/height so the ISP outputs native model resolution without CPU resize.
 *
 * Requires `HalVideoContext` from `hal_video_internal.h` (same as returned by HAL_MEDIA_OPS.get_video_list).
 */
#pragma once

#include <stdint.h>
#include "hal_video_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @return Index into @p video_list, or `UINT32_MAX` if no stream matches. */
static inline uint32_t hal_media_find_video_index_exact(void **video_list, uint32_t video_count, uint32_t width,
                                                         uint32_t height)
{
    if (!video_list || video_count == 0)
        return UINT32_MAX;
    for (uint32_t i = 0; i < video_count; i++)
    {
        HalVideoContext *v = (HalVideoContext *)video_list[i];
        if (v && v->config.width == width && v->config.height == height)
            return i;
    }
    return UINT32_MAX;
}

/**
 * Prefer a stream matching (@p width, @p height) whose index is not @p avoid_index (e.g. the preview/RTP branch).
 * If only @p avoid_index matches, returns that index so single-stream exact-size still works.
 */
static inline uint32_t hal_media_find_video_index_exact_prefer_not(void **video_list, uint32_t video_count,
                                                                   uint32_t width, uint32_t height,
                                                                   uint32_t avoid_index)
{
    uint32_t any = UINT32_MAX;
    if (!video_list || video_count == 0)
        return UINT32_MAX;
    for (uint32_t i = 0; i < video_count; i++)
    {
        HalVideoContext *v = (HalVideoContext *)video_list[i];
        if (!v || v->config.width != width || v->config.height != height)
            continue;
        if (any == UINT32_MAX)
            any = i;
        if (i != avoid_index)
            return i;
    }
    return any;
}

#ifdef __cplusplus
}
#endif
