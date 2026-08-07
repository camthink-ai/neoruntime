/**
 * @file stub_osd_impl.c
 * @brief Stub platform — HAL_OSD_OPS.
 */

#include "media/hal_osd.h"

#include <stddef.h>

static int stub_osd_add_image_overlay(void *codec_ctx, const HalOsdImageOverlay *overlay)
{
    (void)codec_ctx;
    (void)overlay;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_osd_add_text_overlay(void *codec_ctx, const HalOsdTextOverlay *overlay)
{
    (void)codec_ctx;
    (void)overlay;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_osd_add_datetime_overlay(void *codec_ctx, const HalOsdDateTimeOverlay *overlay)
{
    (void)codec_ctx;
    (void)overlay;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_osd_add_custom_overlay(void *codec_ctx, const HalOsdCustomOverlay *overlay)
{
    (void)codec_ctx;
    (void)overlay;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_osd_set_image_overlay(void *codec_ctx, const HalOsdImageOverlay *overlay)
{
    (void)codec_ctx;
    (void)overlay;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_osd_set_text_overlay(void *codec_ctx, const HalOsdTextOverlay *overlay)
{
    (void)codec_ctx;
    (void)overlay;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_osd_set_datetime_overlay(void *codec_ctx, const HalOsdDateTimeOverlay *overlay)
{
    (void)codec_ctx;
    (void)overlay;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_osd_set_custom_overlay(void *codec_ctx, const HalOsdCustomOverlay *overlay)
{
    (void)codec_ctx;
    (void)overlay;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_osd_remove_overlay(void *codec_ctx, const char *overlay_id)
{
    (void)codec_ctx;
    (void)overlay_id;
    return HAL_ERR_NOT_FOUND;
}

static int stub_osd_set_overlay_enabled(void *codec_ctx, const char *overlay_id, bool enabled)
{
    (void)codec_ctx;
    (void)overlay_id;
    (void)enabled;
    return HAL_ERR_NOT_FOUND;
}

static int stub_osd_get_overlays(void *codec_ctx, HalOsdOverlay *overlays, uint32_t *count)
{
    (void)codec_ctx;
    (void)overlays;
    if (!count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *count = 0;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_osd_get_overlay(void *codec_ctx, const char *overlay_id, HalOsdOverlay *overlay)
{
    (void)codec_ctx;
    (void)overlay_id;
    (void)overlay;
    return HAL_ERR_NOT_FOUND;
}

static int stub_osd_clear_overlays(void *codec_ctx)
{
    (void)codec_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_osd_get_version(void)
{
    return "HAL-OSD stub 2.0.0 (platform stub)";
}

HalOsdOps HAL_OSD_OPS = {
    .add_image_overlay = stub_osd_add_image_overlay,
    .add_text_overlay = stub_osd_add_text_overlay,
    .add_datetime_overlay = stub_osd_add_datetime_overlay,
    .add_custom_overlay = stub_osd_add_custom_overlay,
    .set_image_overlay = stub_osd_set_image_overlay,
    .set_text_overlay = stub_osd_set_text_overlay,
    .set_datetime_overlay = stub_osd_set_datetime_overlay,
    .set_custom_overlay = stub_osd_set_custom_overlay,
    .remove_overlay = stub_osd_remove_overlay,
    .set_overlay_enabled = stub_osd_set_overlay_enabled,
    .get_overlays = stub_osd_get_overlays,
    .get_overlay = stub_osd_get_overlay,
    .clear_overlays = stub_osd_clear_overlays,
    .get_version = stub_osd_get_version,
};
