/**
 * @file stub_media_impl.c
 * @brief Stub platform — HAL_MEDIA_OPS (host / CI; no pipeline).
 */

#include "media/hal_media.h"

#include <stddef.h>
#include <string.h>

static int stub_media_init(const HalMediaConfig *config, void **media_ctx_return)
{
    (void)config;
    if (!media_ctx_return)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *media_ctx_return = NULL;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_deinit(void *media_ctx)
{
    (void)media_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_start(void *media_ctx)
{
    (void)media_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_stop(void *media_ctx)
{
    (void)media_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_get_status(void *media_ctx)
{
    (void)media_ctx;
    return (int)HAL_STATUS_UNINITIALIZED;
}

static int stub_media_get_current_profile(void *media_ctx, char **profile_name)
{
    (void)media_ctx;
    if (!profile_name)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *profile_name = NULL;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_get_profile_list(void *media_ctx, char **profile_list, uint32_t *profile_list_count)
{
    (void)media_ctx;
    if (!profile_list_count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    (void)profile_list;
    *profile_list_count = 0;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_switch_profile(void *media_ctx, const char *profile_name, bool force_recycle)
{
    (void)media_ctx;
    (void)profile_name;
    (void)force_recycle;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_get_video_list(void *media_ctx, void **video_list, uint32_t *video_list_count)
{
    (void)media_ctx;
    if (!video_list || !video_list_count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *video_list = NULL;
    *video_list_count = 0;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_get_codec_list(void *media_ctx, void **codec_list, uint32_t *codec_list_count)
{
    (void)media_ctx;
    if (!codec_list || !codec_list_count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *codec_list = NULL;
    *codec_list_count = 0;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_get_current_config(void *media_ctx, HalMediaConfig *config)
{
    (void)media_ctx;
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_get_current_profile_json(void *media_ctx, const char **json_out)
{
    (void)media_ctx;
    if (!json_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *json_out = NULL;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_dynamic_change_image_config(void *media_ctx, const HalMediaImageConfig *config)
{
    (void)media_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_add_video_stream(void *media_ctx, const HalMediaAddVideoConfig *config)
{
    (void)media_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_add_codec_stream(void *media_ctx, const HalMediaAddCodecConfig *config)
{
    (void)media_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_remove_video_stream(void *media_ctx, const HalMediaRemoveVideoConfig *config)
{
    (void)media_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_remove_codec_stream(void *media_ctx, const HalMediaRemoveCodecConfig *config)
{
    (void)media_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_set_encoder_auto_feed(void *media_ctx, bool enable)
{
    (void)media_ctx;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_get_encoder_auto_feed(void *media_ctx, bool *enable_out)
{
    (void)media_ctx;
    if (!enable_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *enable_out = false;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_set_encoder_auto_feed_for_stream(void *media_ctx, const char *stream_id, bool enable)
{
    (void)media_ctx;
    (void)stream_id;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_get_encoder_auto_feed_for_stream(void *media_ctx, const char *stream_id, bool *enable_out)
{
    (void)media_ctx;
    (void)stream_id;
    if (!enable_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *enable_out = false;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_backup_current_profile(void *media_ctx, const char *path)
{
    (void)media_ctx;
    (void)path;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_set_config_field(void *media_ctx, const char *field_path,
                                        HalConfigFieldType field_type, const char *field_value)
{
    (void)media_ctx;
    (void)field_path;
    (void)field_type;
    (void)field_value;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_get_config_field(void *media_ctx, const char *field_path,
                                       HalConfigFieldType *type_out, const char **value_out)
{
    (void)media_ctx;
    (void)field_path;
    if (type_out)
    {
        *type_out = HAL_CONFIG_FIELD_STRING;
    }
    if (value_out)
    {
        *value_out = NULL;
    }
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_media_override_stream_params(void *media_ctx, const HalStreamOverrideBatch *batch)
{
    (void)media_ctx;
    (void)batch;
    return 0; /* stub: success */
}

static int stub_media_reconfigure_pipeline(void *media_ctx, const HalPipelineReconfig *reconfig)
{
    (void)media_ctx;
    (void)reconfig;
    return 0; /* stub: success */
}

/* Thermal throttling — static simulation (M1). */
static HalThrottlingCallback g_stub_throttling_cb;
static void *g_stub_throttling_user;

static int stub_media_subscribe_throttling(void *media_ctx, HalThrottlingCallback callback, void *userdata)
{
    (void)media_ctx;
    if (!callback)
    {
        return HAL_ERR_INVALID_ARG;
    }
    g_stub_throttling_cb = callback;
    g_stub_throttling_user = userdata;
    return HAL_OK;
}

static int stub_media_unsubscribe_throttling(void *media_ctx)
{
    (void)media_ctx;
    g_stub_throttling_cb = NULL;
    g_stub_throttling_user = NULL;
    return HAL_OK;
}

static int stub_media_get_throttling_state(void *media_ctx, HalThrottlingState *state_out)
{
    (void)media_ctx;
    if (!state_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *state_out = HAL_THROTTLING_FULL_PERFORMANCE; /* simulated: never restricted */
    return HAL_OK;
}

static const char *stub_media_get_version(void)
{
    return "HAL-MEDIA stub 2.2.0 (platform stub)";
}

/* Motion detection — static simulation (M2). */
static HalMotionConfig g_stub_motion_cfg;

static int stub_media_set_motion_config(void *media_ctx, const HalMotionConfig *config)
{
    (void)media_ctx;
    if (!config || config->threshold < 0.0f || config->threshold > 1.0f)
    {
        return HAL_ERR_INVALID_ARG;
    }
    g_stub_motion_cfg = *config;
    return HAL_OK;
}

static int stub_media_get_motion_config(void *media_ctx, HalMotionConfig *config)
{
    (void)media_ctx;
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *config = g_stub_motion_cfg;
    return HAL_OK;
}

static int stub_media_subscribe_motion(void *media_ctx, HalMotionCallback callback, void *userdata)
{
    (void)media_ctx;
    (void)userdata;
    if (!callback)
    {
        return HAL_ERR_INVALID_ARG;
    }
    return HAL_OK; /* stub: accepted, no frames are generated */
}

static int stub_media_unsubscribe_motion(void *media_ctx)
{
    (void)media_ctx;
    return HAL_OK;
}

HalMediaOps HAL_MEDIA_OPS = {
    .init = stub_media_init,
    .deinit = stub_media_deinit,
    .start = stub_media_start,
    .stop = stub_media_stop,
    .get_status = stub_media_get_status,
    .get_current_profile = stub_media_get_current_profile,
    .get_profile_list = stub_media_get_profile_list,
    .switch_profile = stub_media_switch_profile,
    .get_video_list = stub_media_get_video_list,
    .get_codec_list = stub_media_get_codec_list,
    .get_current_config = stub_media_get_current_config,
    .get_current_profile_json = stub_media_get_current_profile_json,
    .backup_current_profile = stub_media_backup_current_profile,
    .set_config_field = stub_media_set_config_field,
    .get_config_field = stub_media_get_config_field,
    .dynamic_change_image_config = stub_media_dynamic_change_image_config,
    .add_video_stream = stub_media_add_video_stream,
    .add_codec_stream = stub_media_add_codec_stream,
    .remove_video_stream = stub_media_remove_video_stream,
    .remove_codec_stream = stub_media_remove_codec_stream,
    .set_encoder_auto_feed = stub_media_set_encoder_auto_feed,
    .get_encoder_auto_feed = stub_media_get_encoder_auto_feed,
    .set_encoder_auto_feed_for_stream = stub_media_set_encoder_auto_feed_for_stream,
    .get_encoder_auto_feed_for_stream = stub_media_get_encoder_auto_feed_for_stream,
    .override_stream_params = stub_media_override_stream_params,
    .reconfigure_pipeline = stub_media_reconfigure_pipeline,

    .get_version = stub_media_get_version,
    .subscribe_throttling = stub_media_subscribe_throttling,
    .unsubscribe_throttling = stub_media_unsubscribe_throttling,
    .get_throttling_state = stub_media_get_throttling_state,
    .set_motion_config = stub_media_set_motion_config,
    .get_motion_config = stub_media_get_motion_config,
    .subscribe_motion = stub_media_subscribe_motion,
    .unsubscribe_motion = stub_media_unsubscribe_motion,
};
