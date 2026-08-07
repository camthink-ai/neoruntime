/**
 * @file hailo15_osd_impl.cpp
 * @brief Hailo-15 HAL OSD implementation (FROM_MEDIA encoder-scoped overlays).
 *
 * Delegates to the MediaLibrary osd::Configurer (osd::Blender) obtained from
 * each encoder via encoder->get_osd_blender().  HAL <-> ML type conversions
 * use the helpers in hailo15_osd_ml.hpp.
 */

#include "hailo15_common.hpp"
#include "hailo15_media_priv.hpp"
#include "hailo15_osd_ml.hpp"

#include "common/hal_log.h"
#include "media/hal_osd.h"
#include "media/hal_codec_internal.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

/* -------------------------------------------------------------------
 * OSD context helper — resolves codec_ctx to blender + stream id.
 * ------------------------------------------------------------------- */

struct OsdContext
{
    Hailo15MediaPriv *priv;
    std::string stream_id;
    std::shared_ptr<osd::Blender> blender;
};

int resolve_osd_context(void *codec_ctx, OsdContext *out)
{
    if (!codec_ctx || !out)
    {
        return HAL_ERR_INVALID_ARG;
    }

    auto *cc = static_cast<HalCodecContext *>(codec_ctx);
    if (cc->config.type != HAL_CODEC_TYPE_FROM_MEDIA || !cc->config.media_ptr)
    {
        HAL_LOG_ERROR("Hailo15 OSD: codec context is not FROM_MEDIA type");
        return HAL_ERR_NOT_SUPPORTED;
    }

    Hailo15MediaPriv *priv = hailo15_media_priv_from_hal(cc->config.media_ptr);
    if (!priv || !priv->media_lib)
    {
        HAL_LOG_ERROR("Hailo15 OSD: media priv or media_lib is null");
        return HAL_ERR_NOT_INITIALIZED;
    }

    std::string stream_id(cc->codec_name);
    auto enc_it = priv->media_lib->m_encoders.find(stream_id);
    if (enc_it == priv->media_lib->m_encoders.end())
    {
        HAL_LOG_ERROR("Hailo15 OSD: encoder '%s' not found", cc->codec_name);
        return HAL_ERR_NOT_FOUND;
    }

    auto blender = enc_it->second->get_osd_blender();
    if (!blender)
    {
        HAL_LOG_ERROR("Hailo15 OSD: get_osd_blender() returned null for '%s'", cc->codec_name);
        return HAL_ERR_NOT_INITIALIZED;
    }

    out->priv = priv;
    out->stream_id = std::move(stream_id);
    out->blender = std::move(blender);
    return HAL_OK;
}

bool readable_image_file(const char *path)
{
    if (!path || path[0] == '\0')
    {
        return false;
    }
    struct stat st {};
    return (::stat(path, &st) == 0) && S_ISREG(st.st_mode) && (::access(path, R_OK) == 0);
}

static int copy_custom_overlay_data(OsdContext &ctx, const HalOsdCustomOverlay &h)
{
    if (h.data == nullptr || h.data_size == 0)
    {
        return HAL_OK; /* metadata-only update */
    }
    auto exp = ctx.blender->get_overlay(std::string(h.base.id));
    if (!exp.has_value())
    {
        return HAL_ERR_NOT_FOUND;
    }
    auto custom = std::static_pointer_cast<osd::CustomOverlay>(exp.value());
    if (!custom)
    {
        return HAL_ERROR;
    }
    auto buf = custom->get_buffer();
    if (!buf)
    {
        return HAL_ERR_NOT_INITIALIZED;
    }

    const uint32_t plane_count = (h.format == HAL_OSD_CUSTOM_FMT_A420) ? 4u : 1u;
    size_t need = 0;
    for (uint32_t i = 0; i < plane_count; i++)
    {
        need += (size_t)buf->get_plane_size(i);
    }
    if (need == 0 || (size_t)h.data_size != need)
    {
        HAL_LOG_ERROR("Hailo15 OSD custom overlay '%s': data_size=%u expected=%zu (format=%s)",
                      h.base.id, (unsigned)h.data_size, need,
                      (h.format == HAL_OSD_CUSTOM_FMT_A420) ? "A420" : "ARGB");
        return HAL_ERR_INVALID_SIZE;
    }

    const uint8_t *src = static_cast<const uint8_t *>(h.data);
    size_t off = 0;
    for (uint32_t i = 0; i < plane_count; i++)
    {
        void *dst = buf->get_plane_ptr(i);
        const size_t sz = (size_t)buf->get_plane_size(i);
        if (dst == nullptr || sz == 0)
        {
            return HAL_ERROR;
        }
        std::memcpy(dst, src + off, sz);
        off += sz;
    }
    return HAL_OK;
}

} // namespace

/* ====================================================================
 * Add overlays
 * ==================================================================== */

static int hailo15_osd_add_image(void *codec_ctx, const HalOsdImageOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (!readable_image_file(overlay->image_path))
    {
        HAL_LOG_ERROR("Hailo15 OSD add_image: invalid image_path='%s'", overlay->image_path);
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        /* Defensive: clamp overlay coords into the pre-rotation DSP blend
         * frame so a portrait rotation can't push an image outside the blend
         * height and wedge the encoded stream. See clamp_osd_for_rotation(). */
        HalOsdImageOverlay adjusted = *overlay;
        auto lay_it = ctx.priv->osd_layout_by_encoder.find(ctx.stream_id);
        if (lay_it != ctx.priv->osd_layout_by_encoder.end())
        {
            HalRotationAngle rot = static_cast<HalRotationAngle>(lay_it->second.rotation);
            float ox = adjusted.base.x;
            float oy = adjusted.base.y;
            if (hailo15::osd_ml::clamp_osd_for_rotation(ox, oy, adjusted.width, adjusted.height,
                                                        lay_it->second.width, lay_it->second.height, rot))
            {
                HAL_LOG_WARNING("Hailo15 OSD: image '%s' origin (%.4f,%.4f) clamped to (%.4f,%.4f) "
                                "to fit pre-rotation blend frame under rotation %d (out %ux%u); "
                                "unclamped portrait OSD can wedge the encoded stream",
                                adjusted.base.id, adjusted.base.x, adjusted.base.y, ox, oy,
                                rot, lay_it->second.width, lay_it->second.height);
                adjusted.base.x = ox;
                adjusted.base.y = oy;
            }
        }
        osd::ImageOverlay ml = hailo15::osd_ml::hal_to_ml_image(adjusted);
        media_library_return ret = ctx.blender->add_overlay(ml);
        if (ret != MEDIA_LIBRARY_SUCCESS)
        {
            return hailo15_ml_err(ret);
        }
        ret = ctx.blender->set_overlay_enabled(std::string(overlay->base.id), overlay->base.enabled);
        return hailo15_ml_err(ret);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD add_image exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_add_text(void *codec_ctx, const HalOsdTextOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        /* Defensive: clamp overlay origin into the pre-rotation DSP blend
         * frame under portrait rotation; an out-of-bounds y wedges the stream. */
        HalOsdTextOverlay adjusted = *overlay;
        auto lay_it = ctx.priv->osd_layout_by_encoder.find(ctx.stream_id);
        if (lay_it != ctx.priv->osd_layout_by_encoder.end())
        {
            HalRotationAngle rot = static_cast<HalRotationAngle>(lay_it->second.rotation);
            float ox = adjusted.base.x;
            float oy = adjusted.base.y;
            if (hailo15::osd_ml::clamp_osd_for_rotation(ox, oy, 0.0f, 0.0f,
                                                        lay_it->second.width, lay_it->second.height, rot))
            {
                HAL_LOG_WARNING("Hailo15 OSD: text '%s' origin (%.4f,%.4f) clamped to (%.4f,%.4f) "
                                "to fit pre-rotation blend frame under rotation %d (out %ux%u); "
                                "unclamped portrait OSD can wedge the encoded stream",
                                adjusted.base.id, adjusted.base.x, adjusted.base.y, ox, oy,
                                rot, lay_it->second.width, lay_it->second.height);
                adjusted.base.x = ox;
                adjusted.base.y = oy;
            }
        }
        osd::TextOverlay ml = hailo15::osd_ml::hal_to_ml_text(adjusted);
        media_library_return ret = ctx.blender->add_overlay(ml);
        if (ret != MEDIA_LIBRARY_SUCCESS)
        {
            return hailo15_ml_err(ret);
        }
        ret = ctx.blender->set_overlay_enabled(std::string(overlay->base.id), overlay->base.enabled);
        return hailo15_ml_err(ret);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD add_text exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_add_datetime(void *codec_ctx, const HalOsdDateTimeOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        /* Defensive: clamp overlay origin into the pre-rotation DSP blend
         * frame under portrait rotation; an out-of-bounds y wedges the stream. */
        HalOsdDateTimeOverlay adjusted = *overlay;
        auto lay_it = ctx.priv->osd_layout_by_encoder.find(ctx.stream_id);
        if (lay_it != ctx.priv->osd_layout_by_encoder.end())
        {
            HalRotationAngle rot = static_cast<HalRotationAngle>(lay_it->second.rotation);
            float ox = adjusted.text.base.x;
            float oy = adjusted.text.base.y;
            if (hailo15::osd_ml::clamp_osd_for_rotation(ox, oy, 0.0f, 0.0f,
                                                        lay_it->second.width, lay_it->second.height, rot))
            {
                HAL_LOG_WARNING("Hailo15 OSD: datetime '%s' origin (%.4f,%.4f) clamped to (%.4f,%.4f) "
                                "to fit pre-rotation blend frame under rotation %d (out %ux%u); "
                                "unclamped portrait OSD can wedge the encoded stream",
                                adjusted.text.base.id, adjusted.text.base.x, adjusted.text.base.y, ox, oy,
                                rot, lay_it->second.width, lay_it->second.height);
                adjusted.text.base.x = ox;
                adjusted.text.base.y = oy;
            }
        }
        osd::DateTimeOverlay ml = hailo15::osd_ml::hal_to_ml_datetime(adjusted);
        media_library_return ret = ctx.blender->add_overlay(ml);
        if (ret != MEDIA_LIBRARY_SUCCESS)
        {
            return hailo15_ml_err(ret);
        }
        ret = ctx.blender->set_overlay_enabled(std::string(overlay->text.base.id), overlay->text.base.enabled);
        return hailo15_ml_err(ret);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD add_datetime exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_add_custom(void *codec_ctx, const HalOsdCustomOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        osd::CustomOverlay ml = hailo15::osd_ml::hal_to_ml_custom(*overlay);
        media_library_return ret = ctx.blender->add_overlay(ml);
        if (ret != MEDIA_LIBRARY_SUCCESS)
        {
            return hailo15_ml_err(ret);
        }
        int cr = copy_custom_overlay_data(ctx, *overlay);
        if (cr != HAL_OK)
        {
            return cr;
        }
        ret = ctx.blender->set_overlay_enabled(std::string(overlay->base.id), overlay->base.enabled);
        return hailo15_ml_err(ret);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD add_custom exception: %s", e.what());
        return HAL_ERROR;
    }
}

/* ====================================================================
 * Set (update) overlays
 * ==================================================================== */

static int hailo15_osd_set_image(void *codec_ctx, const HalOsdImageOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (!readable_image_file(overlay->image_path))
    {
        HAL_LOG_ERROR("Hailo15 OSD set_image: invalid image_path='%s'", overlay->image_path);
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        osd::ImageOverlay ml = hailo15::osd_ml::hal_to_ml_image(*overlay);
        media_library_return ret = ctx.blender->set_overlay(ml);
        return hailo15_ml_err(ret);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD set_image exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_set_text(void *codec_ctx, const HalOsdTextOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        osd::TextOverlay ml = hailo15::osd_ml::hal_to_ml_text(*overlay);
        media_library_return ret = ctx.blender->set_overlay(ml);
        return hailo15_ml_err(ret);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD set_text exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_set_datetime(void *codec_ctx, const HalOsdDateTimeOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        osd::DateTimeOverlay ml = hailo15::osd_ml::hal_to_ml_datetime(*overlay);
        media_library_return ret = ctx.blender->set_overlay(ml);
        return hailo15_ml_err(ret);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD set_datetime exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_set_custom(void *codec_ctx, const HalOsdCustomOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        osd::CustomOverlay ml = hailo15::osd_ml::hal_to_ml_custom(*overlay);
        media_library_return ret = ctx.blender->set_overlay(ml);
        int rr = hailo15_ml_err(ret);
        if (rr != HAL_OK)
        {
            return rr;
        }
        int cr = copy_custom_overlay_data(ctx, *overlay);
        if (cr != HAL_OK)
        {
            return cr;
        }
        return HAL_OK;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD set_custom exception: %s", e.what());
        return HAL_ERROR;
    }
}

/* ====================================================================
 * Remove / enable-disable
 * ==================================================================== */

static int hailo15_osd_remove(void *codec_ctx, const char *overlay_id)
{
    if (!overlay_id)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        media_library_return ret = ctx.blender->remove_overlay(std::string(overlay_id));
        return hailo15_ml_err(ret);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD remove exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_set_enabled(void *codec_ctx, const char *overlay_id, bool enabled)
{
    if (!overlay_id)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        media_library_return ret = ctx.blender->set_overlay_enabled(std::string(overlay_id), enabled);
        return hailo15_ml_err(ret);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD set_enabled exception: %s", e.what());
        return HAL_ERROR;
    }
}

/* ====================================================================
 * Query overlays
 * ==================================================================== */

static int hailo15_osd_get_overlays(void *codec_ctx, HalOsdOverlay *overlays, uint32_t *count)
{
    if (!count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        /* Do not hold priv->mutex across MediaLibrary calls. */
        auto prof_exp = ctx.priv->media_lib->get_current_profile();
        if (!prof_exp)
        {
            HAL_LOG_ERROR("Hailo15 OSD get_overlays: get_current_profile failed");
            return HAL_ERROR;
        }
        const config_profile_t &prof = prof_exp.value();
        auto stream_it = prof.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == prof.encoded_output_streams.end())
        {
            HAL_LOG_ERROR("Hailo15 OSD get_overlays: stream '%s' not found in profile", ctx.stream_id.c_str());
            return HAL_ERR_NOT_FOUND;
        }

        const config_stream_osd_t &osd = stream_it->second.osd;
        const uint32_t n_image = static_cast<uint32_t>(osd.image_overlays.size());
        const uint32_t n_text = static_cast<uint32_t>(osd.text_overlays.size());
        const uint32_t n_datetime = static_cast<uint32_t>(osd.datetime_overlays.size());
        const uint32_t total = n_image + n_text + n_datetime;

        /* Query-only: caller wants to know the required count. */
        if (!overlays || *count < total)
        {
            *count = total;
            return overlays ? HAL_ERR_INSUFFICIENT_BUFFER : HAL_OK;
        }

        uint32_t idx = 0;

        for (uint32_t i = 0; i < n_image; ++i)
        {
            if (!osd.image_overlays[i])
            {
                continue;
            }
            overlays[idx].type = HAL_OSD_OVERLAY_IMAGE;
            hailo15::osd_ml::ml_to_hal_image(*osd.image_overlays[i], &overlays[idx].data.image);
            ++idx;
        }

        for (uint32_t i = 0; i < n_text; ++i)
        {
            if (!osd.text_overlays[i])
            {
                continue;
            }
            overlays[idx].type = HAL_OSD_OVERLAY_TEXT;
            hailo15::osd_ml::ml_to_hal_text(*osd.text_overlays[i], &overlays[idx].data.text);
            ++idx;
        }

        for (uint32_t i = 0; i < n_datetime; ++i)
        {
            if (!osd.datetime_overlays[i])
            {
                continue;
            }
            overlays[idx].type = HAL_OSD_OVERLAY_DATETIME;
            hailo15::osd_ml::ml_to_hal_datetime(*osd.datetime_overlays[i], &overlays[idx].data.datetime);
            ++idx;
        }

        *count = idx;
        return HAL_OK;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD get_overlays exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_get_overlay(void *codec_ctx, const char *overlay_id, HalOsdOverlay *overlay)
{
    if (!overlay_id || !overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        /* Do not hold priv->mutex across MediaLibrary calls. */
        auto prof_exp = ctx.priv->media_lib->get_current_profile();
        if (!prof_exp)
        {
            return HAL_ERROR;
        }
        const config_profile_t &prof = prof_exp.value();
        auto stream_it = prof.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == prof.encoded_output_streams.end())
        {
            return HAL_ERR_NOT_FOUND;
        }

        const config_stream_osd_t &osd = stream_it->second.osd;
        const std::string id(overlay_id);

        /* Search image overlays. */
        for (const auto &ptr : osd.image_overlays)
        {
            if (ptr && ptr->id == id)
            {
                overlay->type = HAL_OSD_OVERLAY_IMAGE;
                hailo15::osd_ml::ml_to_hal_image(*ptr, &overlay->data.image);
                return HAL_OK;
            }
        }

        /* Search text overlays. */
        for (const auto &ptr : osd.text_overlays)
        {
            if (ptr && ptr->id == id)
            {
                overlay->type = HAL_OSD_OVERLAY_TEXT;
                hailo15::osd_ml::ml_to_hal_text(*ptr, &overlay->data.text);
                return HAL_OK;
            }
        }

        /* Search datetime overlays. */
        for (const auto &ptr : osd.datetime_overlays)
        {
            if (ptr && ptr->id == id)
            {
                overlay->type = HAL_OSD_OVERLAY_DATETIME;
                hailo15::osd_ml::ml_to_hal_datetime(*ptr, &overlay->data.datetime);
                return HAL_OK;
            }
        }

        return HAL_ERR_NOT_FOUND;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD get_overlay exception: %s", e.what());
        return HAL_ERROR;
    }
}

/* ====================================================================
 * Clear all overlays
 * ==================================================================== */

static int hailo15_osd_clear(void *codec_ctx)
{
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        /* Do not hold priv->mutex across MediaLibrary calls. */
        auto prof_exp = ctx.priv->media_lib->get_current_profile();
        if (!prof_exp)
        {
            return HAL_ERROR;
        }
        const config_profile_t &prof = prof_exp.value();
        auto stream_it = prof.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == prof.encoded_output_streams.end())
        {
            return HAL_ERR_NOT_FOUND;
        }

        /* Collect all overlay ids from the profile's OSD config. */
        std::vector<std::string> ids;
        const config_stream_osd_t &osd = stream_it->second.osd;
        for (const auto &ptr : osd.image_overlays)
        {
            if (ptr)
            {
                ids.push_back(ptr->id);
            }
        }
        for (const auto &ptr : osd.text_overlays)
        {
            if (ptr)
            {
                ids.push_back(ptr->id);
            }
        }
        for (const auto &ptr : osd.datetime_overlays)
        {
            if (ptr)
            {
                ids.push_back(ptr->id);
            }
        }

        /* Remove each overlay via the blender (updates both repository and profile). */
        for (const auto &id : ids)
        {
            ctx.blender->remove_overlay(id);
        }

        return HAL_OK;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD clear exception: %s", e.what());
        return HAL_ERROR;
    }
}

/* ====================================================================
 * Version
 * ==================================================================== */

static const char *hailo15_osd_get_version(void)
{
    return "Hailo15 HAL-OSD 2.0.0";
}

/* ====================================================================
 * OSD ops table
 * ==================================================================== */

extern "C" {

HalOsdOps HAL_OSD_OPS = {
    .add_image_overlay    = hailo15_osd_add_image,
    .add_text_overlay     = hailo15_osd_add_text,
    .add_datetime_overlay = hailo15_osd_add_datetime,
    .add_custom_overlay   = hailo15_osd_add_custom,

    .set_image_overlay    = hailo15_osd_set_image,
    .set_text_overlay     = hailo15_osd_set_text,
    .set_datetime_overlay = hailo15_osd_set_datetime,
    .set_custom_overlay   = hailo15_osd_set_custom,

    .remove_overlay       = hailo15_osd_remove,
    .set_overlay_enabled  = hailo15_osd_set_enabled,

    .get_overlays         = hailo15_osd_get_overlays,
    .get_overlay          = hailo15_osd_get_overlay,
    .clear_overlays       = hailo15_osd_clear,

    .get_version          = hailo15_osd_get_version,
};

} // extern "C"
