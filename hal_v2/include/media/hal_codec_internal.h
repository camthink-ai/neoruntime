/**
 * @file hal_codec_internal.h
 * @brief HAL Codec Internal - Codec context structure for platform implementations.
 *
 * This header is used by platform adapters only (e.g. hailo15_codec_impl.cpp).
 * Application code should use hal_codec.h instead.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../common/hal_common.h"
#include "../common/hal_buffer.h"
#include "hal_codec.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Internal codec context holding encoder state.
 *
 * For HW / SOFT types: allocated by init(), freed by deinit().
 * For FROM_MEDIA type: allocated by media->get_codec_list(), freed by media->deinit().
 */
typedef struct {
    HalStatus               status;             /* current encoder status */
    HalCodecConfig          config;             /* current encoder configuration */

    int                     codec_fd;           /* device file descriptor (-1 for FROM_MEDIA) */
    char                    codec_name[64];     /* human-readable encoder name (e.g. "encoder_0") */

    HalCodecFrameCallback   frame_callback;     /* registered push-mode callback (NULL if none) */
    void                   *userdata;           /* opaque pointer passed to frame_callback */

    void                   *priv;               /* platform-private data (e.g. Hailo15CodecPriv) */
} HalCodecContext;

#ifdef __cplusplus
}
#endif
