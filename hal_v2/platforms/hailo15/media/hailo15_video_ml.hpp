/**
 * @file hailo15_video_ml.hpp
 * @brief MediaLibrary profile updates for frontend streams (resolution / fps / format / pool).
 */
#pragma once

#include "hailo15_codec_ml.hpp"
#include "hailo15_common.hpp"
#include "hailo15_media_priv.hpp"
#include "hailo15_osd_ml.hpp"

#include "media/hal_media_internal.h"
#include "media/hal_video_internal.h"

#include <hailo/media_library/media_library.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>

namespace hailo15::video_ml
{

inline void clear_encoder_osd(config_profile_t &p, const std::string *stream_id)
{
    for (auto &kv : p.encoded_output_streams)
    {
        if (stream_id && kv.first != *stream_id)
        {
            continue;
        }
        /* Minimal safety: drop OSD overlays when geometry changes.
         * Full alignment with webserver would rescale / reposition overlays instead of clearing them. */
        kv.second.osd.image_overlays.clear();
        kv.second.osd.text_overlays.clear();
        kv.second.osd.datetime_overlays.clear();
    }
}

inline HailoFormat hal_pixel_to_hailo(HalPixelFormat f)
{
    switch (f)
    {
        case HAL_PIX_FMT_NV12:
            return HAILO_FORMAT_NV12;
        case HAL_PIX_FMT_GRAY8:
            return HAILO_FORMAT_GRAY8;
        case HAL_PIX_FMT_RGB24:
            return HAILO_FORMAT_RGB;
        case HAL_PIX_FMT_ARGB32:
            return HAILO_FORMAT_ARGB;
        default:
            return HAILO_FORMAT_NV12;
    }
}

inline void apply_profile_to_video_ctx(HalVideoContext *vc, const config_profile_t &prof, const char *stream_id)
{
    if (!vc || !stream_id)
    {
        return;
    }
    for (const auto &res : prof.application_settings.application_input_streams.resolutions)
    {
        if (res.stream_id == stream_id)
        {
            vc->config.width = res.dimensions.destination_width;
            vc->config.height = res.dimensions.destination_height;
            vc->config.framerate = res.framerate;
            vc->config.format = hailo_format_to_hal(prof.application_settings.application_input_streams.format);
            vc->config.pool_max_buffers = res.pool_max_buffers;
            return;
        }
    }
}

inline void apply_profile_to_codec_ctx(HalCodecContext *cc, const config_profile_t &prof)
{
    if (!cc)
    {
        return;
    }
    const std::string eid = cc->codec_name;
    auto it = prof.encoded_output_streams.find(eid);
    if (it != prof.encoded_output_streams.end())
    {
        void *mp = cc->config.media_ptr;
        hailo15::ml::fill_hal_codec_config(cc, eid, it->second, mp);
    }
}

inline void refresh_all_context_configs(Hailo15MediaPriv *priv, HalMediaContext *hm)
{
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp || !hm->video_ctx_list || !hm->codec_ctx_list)
    {
        return;
    }
    const config_profile_t &prof = prof_exp.value();
    const HalStatus pipe_st = priv->pipeline_started ? HAL_STATUS_RUNNING : HAL_STATUS_INITIALIZED;
    for (uint32_t i = 0; i < hm->video_ctx_list_count; i++)
    {
        auto *vc = static_cast<HalVideoContext *>(hm->video_ctx_list[i]);
        apply_profile_to_video_ctx(vc, prof, vc->video_name);
        vc->status = pipe_st;
    }
    for (uint32_t i = 0; i < hm->codec_ctx_list_count; i++)
    {
        auto *cc = static_cast<HalCodecContext *>(hm->codec_ctx_list[i]);
        apply_profile_to_codec_ctx(cc, prof);
        cc->status = pipe_st;
    }
}

/**
 * Updates one frontend output_resolution_t and optionally global input format, then set_override_parameters().
 */
inline int apply_frontend_stream_override(const MediaLibraryPtr &ml, const std::string &stream_id,
                                          const std::optional<std::pair<uint32_t, uint32_t>> &resolution,
                                          const std::optional<uint32_t> &framerate,
                                          const std::optional<HalPixelFormat> &format,
                                          const std::optional<uint32_t> &pool_max,
                                          Hailo15MediaPriv *priv_for_osd = nullptr)
{
    auto prof_exp = ml->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    config_profile_t p = prof_exp.value();
    bool found = false;
    uint32_t new_w = 0U;
    uint32_t new_h = 0U;
    for (auto &res : p.application_settings.application_input_streams.resolutions)
    {
        if (res.stream_id != stream_id)
        {
            continue;
        }
        found = true;
        if (resolution.has_value())
        {
            const auto [w, h] = resolution.value();
            if (w > 0U && h > 0U)
            {
                res.dimensions.destination_width = w;
                res.dimensions.destination_height = h;
                new_w = w;
                new_h = h;
            }
        }
        if (framerate.has_value() && framerate.value() > 0U)
        {
            res.framerate = framerate.value();
        }
        if (pool_max.has_value())
        {
            res.pool_max_buffers = pool_max.value();
        }
        break;
    }
    if (!found)
    {
        return HAL_ERR_INVALID_STATE;
    }

    /* Keep encoder input dimensions in sync for matching stream id (typical 1:1 sinkX mapping),
     * and capture the old geometry so OSD/privacy-mask can be rescaled to the new resolution. */
    uint32_t old_enc_w = 0U;
    uint32_t old_enc_h = 0U;
    if (new_w > 0U && new_h > 0U)
    {
        for (auto &kv : p.encoded_output_streams)
        {
            if (kv.first == stream_id)
            {
                std::visit(
                    [&](auto &enc) {
                        old_enc_w = enc.input_stream.width;
                        old_enc_h = enc.input_stream.height;
                        enc.input_stream.width = new_w;
                        enc.input_stream.height = new_h;
                    },
                    kv.second.encoding);
            }
        }
        /* Preserve OSD + static privacy mask across the resolution change by rescaling (relative
         * overlay positions and image dims auto-adapt; absolute font/line/outline and privacy-mask
         * vertices are rescaled). set_override_parameters() does NOT reconfigure the blenders, so
         * they are pushed again below via configure_osd()/configure_privacy_mask(). */
        hailo15::osd_ml::rescale_stream_osd_and_masking(p, stream_id, old_enc_w, old_enc_h, new_w, new_h);
        if (priv_for_osd)
        {
            std::lock_guard<std::recursive_mutex> lock(priv_for_osd->mutex);
            priv_for_osd->osd_layout_by_encoder[stream_id] =
                OsdLayoutState{new_w, new_h, static_cast<int>(HAL_ROTATION_ANGLE_0)};
        }
    }

    if (format.has_value())
    {
        p.application_settings.application_input_streams.format = hal_pixel_to_hailo(format.value());
    }

    /* On a resolution change, suspend encoder feeding for this stream around
     * set_override_parameters(). While the pipeline is stopping/restarting the encoder is
     * reconfigured to the new geometry; if it receives a buffer whose stride doesn't match the
     * new geometry -> update_stride() fails (VCEnc -3, silent) -> the encoder stalls and never
     * releases its buffers -> the frontend multi_resize_output pool can't drain -> "Timeout
     * waiting for used buffers" + "auto_feed add_buffer failed". The suspend flag is checked by
     * BOTH the auto-feed frontend bridge AND the manual input_frame() codec op, so feeding is
     * paused whether the app uses auto_feed or feeds frames itself. Held suspended across the
     * set_override_parameters() call and a short post-switch window (see below), then cleared. */
    const bool feed_suspend = (priv_for_osd != nullptr) && (new_w > 0U) && (new_h > 0U);
    if (feed_suspend)
    {
        std::lock_guard<std::recursive_mutex> lock(priv_for_osd->mutex);
        priv_for_osd->encoder_feed_suspended[stream_id] = true;
    }
    media_library_return r = ml->set_override_parameters(p);

    if (feed_suspend)
    {
        std::lock_guard<std::recursive_mutex> lock(priv_for_osd->mutex);
        priv_for_osd->encoder_feed_suspended[stream_id] = false;
    }

    if (r != MEDIA_LIBRARY_SUCCESS)
    {
        return hailo15_ml_err(r);
    }

    /* set_override_parameters() does NOT reconfigure the blenders (the encoder instance
     * persists). For the static privacy mask this is fixable: the privacy-mask blender is public
     * and takes the struct directly, so re-push the rescaled config (vertices are absolute px).
     *
     * OSD, however, cannot be cleanly re-applied from the HAL in this medialib version:
     * configure_osd() is private, osd::Blender::configure() needs an internally-serialized
     * string, and add_overlay() expects osd::ImageOverlay (a different type than the profile's
     * global ImageOverlay). So the OSD blender keeps its existing overlays across the change.
     * The overlay x/y and image width/height are relative (0..1), so they track the new frame on
     * their own; only font_size/line_thickness/outline_size (rescaled in the profile above) are
     * not pushed to the persisted blender. (A full MediaLibrary reinit would re-create the
     * encoder and pick up the rescaled OSD config.) */
    if (new_w > 0U && new_h > 0U)
    {
        auto enc_it = ml->m_encoders.find(stream_id);
        auto eos_it = p.encoded_output_streams.find(stream_id);
        if (enc_it != ml->m_encoders.end() && enc_it->second &&
            eos_it != p.encoded_output_streams.end())
        {
            auto pm_blender = enc_it->second->get_privacy_mask_blender();
            if (pm_blender)
            {
                (void)pm_blender->configure(std::make_unique<privacy_mask_config_t>(eos_it->second.masking));
            }
        }
    }

    return HAL_OK;
}

} // namespace hailo15::video_ml
