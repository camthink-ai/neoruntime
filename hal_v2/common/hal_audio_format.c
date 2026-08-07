/**
 * @file hal_audio_format.c
 * @brief Audio format helpers for HAL audio module.
 */

#include "media/hal_audio.h"

uint32_t hal_audio_sample_format_bytes(HalAudioSampleFormat fmt)
{
    switch (fmt)
    {
        case HAL_AUDIO_SAMPLE_FMT_S16LE:
            return 2u;
        case HAL_AUDIO_SAMPLE_FMT_S32LE:
            return 4u;
        case HAL_AUDIO_SAMPLE_FMT_F32LE:
            return 4u;
        default:
            return 0u;
    }
}

const char *hal_audio_sample_format_to_string(HalAudioSampleFormat fmt)
{
    switch (fmt)
    {
        case HAL_AUDIO_SAMPLE_FMT_S16LE:
            return "S16LE";
        case HAL_AUDIO_SAMPLE_FMT_S32LE:
            return "S32LE";
        case HAL_AUDIO_SAMPLE_FMT_F32LE:
            return "F32LE";
        default:
            return "UNKNOWN";
    }
}
