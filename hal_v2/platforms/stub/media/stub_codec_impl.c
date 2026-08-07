/**
 * @file stub_codec_impl.c
 * @brief Stub platform — HAL_CODEC_OPS.
 */

#include "media/hal_codec.h"
#include "media/hal_codec_internal.h"

#include <stddef.h>
#include <string.h>

static int stub_codec_init(const HalCodecConfig *config, void **codec_ctx_return)
{
    if (!config || !codec_ctx_return)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (config->type == HAL_CODEC_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    *codec_ctx_return = NULL;
    return HAL_ERR_NOT_IMPLEMENTED;
}

static int stub_codec_deinit(void *codec_ctx)
{
    if (!codec_ctx)
    {
        return HAL_ERR_INVALID_ARG;
    }
    const HalCodecContext *cc = (const HalCodecContext *)codec_ctx;
    if (cc->config.type == HAL_CODEC_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    return HAL_ERR_NOT_IMPLEMENTED;
}

static int stub_codec_start(void *codec_ctx)
{
    (void)codec_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_codec_stop(void *codec_ctx)
{
    (void)codec_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_codec_input_frame(void *codec_ctx, HalFrameBuffer *frame)
{
    (void)codec_ctx;
    (void)frame;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_codec_subscribe(void *codec_ctx, HalCodecFrameCallback callback, void *userdata)
{
    (void)codec_ctx;
    (void)callback;
    (void)userdata;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_codec_unsubscribe(void *codec_ctx, HalCodecFrameCallback callback)
{
    (void)codec_ctx;
    (void)callback;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_codec_release_packet(void *codec_ctx, HalPacketBuffer *packet)
{
    (void)codec_ctx;
    (void)packet;
    return HAL_OK;
}

static int stub_codec_get_status(void *codec_ctx)
{
    (void)codec_ctx;
    return (int)HAL_STATUS_UNINITIALIZED;
}

static int stub_codec_get_current_config(void *codec_ctx, HalCodecConfig *config)
{
    if (!codec_ctx || !config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_codec_dynamic_change_config(void *codec_ctx, const HalCodecConfig *config)
{
    (void)codec_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_codec_get_version(void)
{
    return "HAL-CODEC stub 2.0.0 (platform stub)";
}

HalCodecOps HAL_CODEC_OPS = {
    .init = stub_codec_init,
    .deinit = stub_codec_deinit,
    .start = stub_codec_start,
    .stop = stub_codec_stop,
    .input_frame = stub_codec_input_frame,
    .subscribe = stub_codec_subscribe,
    .unsubscribe = stub_codec_unsubscribe,
    .release_packet = stub_codec_release_packet,
    .get_status = stub_codec_get_status,
    .get_current_config = stub_codec_get_current_config,
    .dynamic_change_config = stub_codec_dynamic_change_config,
    .get_version = stub_codec_get_version,
    .init_from_context = NULL,     /* not supported by stub */
    .deinit_from_context = NULL,   /* not supported by stub */
};
