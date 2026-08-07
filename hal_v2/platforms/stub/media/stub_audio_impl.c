/**
 * @file stub_audio_impl.c
 * @brief Stub platform — HAL_AUDIO_OPS.
 */

#include "media/hal_audio.h"
#include "media/hal_audio_internal.h"

#include <stddef.h>
#include <string.h>

static int stub_audio_init(const HalAudioConfig *config, void **audio_ctx_return)
{
    if (!config || !audio_ctx_return)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *audio_ctx_return = NULL;
    return HAL_ERR_NOT_IMPLEMENTED;
}

static int stub_audio_deinit(void *audio_ctx)
{
    (void)audio_ctx;
    return HAL_ERR_NOT_IMPLEMENTED;
}

static int stub_audio_start(void *audio_ctx)
{
    (void)audio_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_audio_stop(void *audio_ctx)
{
    (void)audio_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_audio_get_status(void *audio_ctx)
{
    (void)audio_ctx;
    return HAL_STATUS_UNINITIALIZED;
}

static int stub_audio_get_current_config(void *audio_ctx, HalAudioConfig *config)
{
    (void)audio_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_audio_subscribe_pcm(void *audio_ctx, HalAudioFrameCallback callback, void *userdata)
{
    (void)audio_ctx;
    (void)callback;
    (void)userdata;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_audio_unsubscribe_pcm(void *audio_ctx, HalAudioFrameCallback callback)
{
    (void)audio_ctx;
    (void)callback;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_audio_subscribe_packet(void *audio_ctx, HalAudioPacketCallback callback, void *userdata)
{
    (void)audio_ctx;
    (void)callback;
    (void)userdata;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_audio_unsubscribe_packet(void *audio_ctx, HalAudioPacketCallback callback)
{
    (void)audio_ctx;
    (void)callback;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_audio_release_frame(void *audio_ctx, HalAudioBuffer *frame)
{
    (void)audio_ctx;
    (void)frame;
    return HAL_OK;
}

static int stub_audio_release_packet(void *audio_ctx, HalPacketBuffer *packet)
{
    (void)audio_ctx;
    (void)packet;
    return HAL_OK;
}

static int stub_audio_list_devices_impl(HalAudioDeviceInfo *devices,
                                        uint32_t max_devices,
                                        uint32_t *count_out,
                                        const char *desc)
{
    if (!devices || !count_out || max_devices == 0)
    {
        return HAL_ERR_INVALID_ARG;
    }
    strncpy(devices[0].name, "stub0", sizeof(devices[0].name) - 1);
    strncpy(devices[0].description, desc, sizeof(devices[0].description) - 1);
    *count_out = 1;
    return HAL_OK;
}

static int stub_audio_list_devices(HalAudioDeviceInfo *devices, uint32_t max_devices, uint32_t *count_out)
{
    return stub_audio_list_devices_impl(devices, max_devices, count_out, "Stub capture device");
}

static int stub_audio_list_playback_devices(HalAudioDeviceInfo *devices,
                                            uint32_t max_devices,
                                            uint32_t *count_out)
{
    return stub_audio_list_devices_impl(devices, max_devices, count_out, "Stub playback device");
}

static int stub_audio_write_pcm(void *audio_ctx, const HalAudioBuffer *frame)
{
    (void)audio_ctx;
    (void)frame;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_audio_dynamic_change_config(void *audio_ctx, const HalAudioConfig *config)
{
    (void)audio_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_audio_get_version(void)
{
    return "Stub HAL-AUDIO 2.0.0";
}

HalAudioOps HAL_AUDIO_OPS = {
    .init = stub_audio_init,
    .deinit = stub_audio_deinit,
    .start = stub_audio_start,
    .stop = stub_audio_stop,
    .get_status = stub_audio_get_status,
    .get_current_config = stub_audio_get_current_config,
    .subscribe_pcm = stub_audio_subscribe_pcm,
    .unsubscribe_pcm = stub_audio_unsubscribe_pcm,
    .subscribe_packet = stub_audio_subscribe_packet,
    .unsubscribe_packet = stub_audio_unsubscribe_packet,
    .release_frame = stub_audio_release_frame,
    .release_packet = stub_audio_release_packet,
    .list_devices = stub_audio_list_devices,
    .list_playback_devices = stub_audio_list_playback_devices,
    .write_pcm = stub_audio_write_pcm,
    .dynamic_change_config = stub_audio_dynamic_change_config,
    .get_version = stub_audio_get_version,
};
