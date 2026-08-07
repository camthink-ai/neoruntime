/**
 * @file hal_video_internal.h
 * @brief HAL Video Internal - Video context structure for platform implementations.
 *
 * This header is used by platform adapters only (e.g. hailo15_video_impl.cpp).
 * Application code should use hal_video.h instead.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../common/hal_common.h"
#include "../common/hal_buffer.h"
#include "hal_video.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Internal video context holding device state.
 *
 * For CSI / UVC types: allocated by init(), freed by deinit().
 * For FROM_MEDIA type: allocated by media->get_video_list(), freed by media->deinit().
 */
typedef struct {
    HalStatus               status;             /* current device status */
    HalVideoConfig          config;             /* current video configuration */

    int                     video_fd;           /* device file descriptor (-1 for FROM_MEDIA) */
    char                    video_name[64];     /* human-readable device name */

    HalVideoFrameCallback   frame_callback;     /* registered push-mode callback (NULL if none) */
    void                   *userdata;           /* opaque pointer passed to frame_callback */

    void                   *priv;               /* platform-private data (e.g. Hailo15VideoPriv) */
} HalVideoContext;

#ifdef __cplusplus
}
#endif
