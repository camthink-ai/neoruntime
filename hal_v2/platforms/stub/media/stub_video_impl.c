/**
 * @file stub_video_impl.c
 * @brief Stub platform — HAL_VIDEO_OPS.
 */

#include "media/hal_video.h"
#include "media/hal_video_internal.h"

#include <stddef.h>
#include <string.h>

static int stub_video_init(const HalVideoConfig *config, void **video_ctx_return)
{
    if (!config || !video_ctx_return)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (config->type == HAL_VIDEO_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    *video_ctx_return = NULL;
    return HAL_ERR_NOT_IMPLEMENTED;
}

static int stub_video_deinit(void *video_ctx)
{
    if (!video_ctx)
    {
        return HAL_ERR_INVALID_ARG;
    }
    const HalVideoContext *vc = (const HalVideoContext *)video_ctx;
    if (vc->config.type == HAL_VIDEO_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    return HAL_ERR_NOT_IMPLEMENTED;
}

static int stub_video_start(void *video_ctx)
{
    (void)video_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_video_stop(void *video_ctx)
{
    (void)video_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static HalStatus stub_video_get_status(void *video_ctx)
{
    if (!video_ctx)
    {
        return HAL_STATUS_UNINITIALIZED;
    }
    return HAL_STATUS_UNINITIALIZED;
}

static int stub_video_get_current_config(void *video_ctx, HalVideoConfig *config)
{
    if (!video_ctx || !config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    memset(config, 0, sizeof(*config));
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_video_dynamic_change_resolution(void *video_ctx, uint32_t width, uint32_t height)
{
    (void)video_ctx;
    (void)width;
    (void)height;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_video_dynamic_change_framerate(void *video_ctx, uint32_t framerate)
{
    (void)video_ctx;
    (void)framerate;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_video_dynamic_change_format(void *video_ctx, HalPixelFormat format)
{
    (void)video_ctx;
    (void)format;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_video_dynamic_change_pool_max_buffers(void *video_ctx, uint32_t pool_max_buffers)
{
    (void)video_ctx;
    (void)pool_max_buffers;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_video_subscribe_stream(void *video_ctx, const char *stream_name, HalVideoFrameCallback callback,
                                       void *userdata)
{
    (void)video_ctx;
    (void)stream_name;
    (void)callback;
    (void)userdata;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_video_unsubscribe_stream(void *video_ctx, const char *stream_name)
{
    (void)video_ctx;
    (void)stream_name;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_video_release_frame(void *video_ctx, HalFrameBuffer *frame)
{
    (void)video_ctx;
    (void)frame;
    return HAL_OK;
}

static const char *stub_video_get_version(void)
{
    return "HAL-VIDEO stub 2.1.0 (platform stub)";
}

static int stub_video_get_sensor_module_info(void *video_ctx, uint32_t sensor_index, HalVideoSensorModuleInfo *info)
{
    (void)video_ctx;
    (void)sensor_index;
    if (!info)
    {
        return HAL_ERR_INVALID_ARG;
    }
    memset(info, 0, sizeof(*info));
    info->i2c_bus = -1;
    info->sensor_pixel_format = -1;
    return HAL_ERR_NOT_SUPPORTED;
}

/* Multi-stage snapshot — static simulation (M2). */
static int stub_video_request_snapshot(void *video_ctx, const char *stage)
{
    (void)video_ctx;
    (void)stage;
    return HAL_ERR_NOT_SUPPORTED; /* stub: no pipeline stages */
}

static int stub_video_list_snapshot_stages(void *video_ctx, char *buf, size_t buf_len)
{
    (void)video_ctx;
    if (!buf || buf_len == 0)
    {
        return HAL_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    return HAL_OK;
}

HalVideoOps HAL_VIDEO_OPS = {
    .init = stub_video_init,
    .deinit = stub_video_deinit,
    .start = stub_video_start,
    .stop = stub_video_stop,
    .get_status = stub_video_get_status,
    .get_current_config = stub_video_get_current_config,
    .dynamic_change_resolution = stub_video_dynamic_change_resolution,
    .dynamic_change_framerate = stub_video_dynamic_change_framerate,
    .dynamic_change_format = stub_video_dynamic_change_format,
    .dynamic_change_pool_max_buffers = stub_video_dynamic_change_pool_max_buffers,
    .subscribe_stream = stub_video_subscribe_stream,
    .unsubscribe_stream = stub_video_unsubscribe_stream,
    .release_frame = stub_video_release_frame,
    .get_version = stub_video_get_version,
    .get_sensor_module_info = stub_video_get_sensor_module_info,
    .request_snapshot = stub_video_request_snapshot,
    .list_snapshot_stages = stub_video_list_snapshot_stages,
};
