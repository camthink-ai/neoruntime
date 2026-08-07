/**
 * @file hal_media_internal.h
 * @brief HAL Media Internal - Media context structure for platform implementations.
 *
 * This header is used by platform adapters only (e.g. hailo15_media_impl.cpp).
 * Application code should use hal_media.h instead.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../common/hal_common.h"
#include "../common/hal_buffer.h"
#include "hal_media.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Internal media context holding pipeline state.
 *
 * Platform implementations allocate this in init() and free it in deinit().
 * The video_ctx_list and codec_ctx_list arrays are populated when the
 * pipeline is initialized, corresponding to the frontend output streams
 * and encoder instances respectively.
 */
typedef struct {
    HalStatus       status;                 /* current pipeline status */
    HalMediaConfig  config;                 /* active configuration snapshot */

    void          **video_ctx_list;         /* array of HalVideoContext pointers (FROM_MEDIA type) */
    void          **codec_ctx_list;         /* array of HalCodecContext pointers (FROM_MEDIA type) */
    uint32_t        video_ctx_list_count;   /* number of video contexts */
    uint32_t        codec_ctx_list_count;   /* number of codec contexts */

    void           *priv;                   /* platform-private data (e.g. Hailo15MediaPriv) */
} HalMediaContext;

#ifdef __cplusplus
}
#endif
