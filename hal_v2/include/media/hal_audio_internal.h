/**
 * @file hal_audio_internal.h
 * @brief HAL Audio internal context (platform adapters only).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../common/hal_common.h"
#include "hal_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    HalStatus               status;
    HalAudioConfig          config;

    HalAudioFrameCallback   pcm_callback;
    void                   *pcm_userdata;

    HalAudioPacketCallback  packet_callback;
    void                   *packet_userdata;

    void                   *priv;
} HalAudioContext;

#ifdef __cplusplus
}
#endif
