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
 *
 * @param keep_suspended  When true, the per-stream feed suspend flag set before
 *                        the vendor call is NOT cleared after it returns — the
 *                        caller (try_light_reconfigure) manages the suspend
 *                        window for ALL streams and clears everything via its
 *                        FeedResumeGuard. This prevents the inner per-stream
 *                        clear from re-opening the feed while a subsequent
 *                        iteration's vendor stop/start (which stops the ENTIRE
 *                        pipeline) is still in flight. Default false preserves
 *                        the standalone-call behavior (suspend → vendor → resume).
 */
inline int apply_frontend_stream_override(const MediaLibraryPtr &ml, const std::string &stream_id,
                                          const std::optional<std::pair<uint32_t, uint32_t>> &resolution,
                                          const std::optional<uint32_t> &framerate,
                                          const std::optional<HalPixelFormat> &format,
                                          const std::optional<uint32_t> &pool_max,
                                          Hailo15MediaPriv *priv_for_osd = nullptr,
                                          bool keep_suspended = false)
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
            uint32_t w = resolution.value().first;
            uint32_t h = resolution.value().second;
            if (w > 0U && h > 0U)
            {
                /* Landscape guard (Pass B convention): config dimensions are
                 * ALWAYS landscape; medialib swaps internally for 90/270.
                 * Without this clamp the light path can write portrait dims
                 * into the live profile while the heavy path (generate_config)
                 * forces landscape for the same request — asymmetric configs
                 * and the double-swap FEED_STALL signature. */
                if (w < h) { std::swap(w, h); }
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
         * vertices are rescaled). Since medialib 1.13 the blenders read OSD/masking per frame from
         * the profile, so applying the rescaled profile below is sufficient. */
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

    /* On ANY light-path change (resolution OR framerate), suspend encoder
     * feeding for this stream around set_override_parameters(). While the
     * pipeline is stopping/restarting the encoder is reconfigured; if it
     * receives a buffer whose stride or timing doesn't match the new state
     * -> update_stride() fails (VCEnc -3, silent) -> the encoder stalls and
     * never releases its buffers -> the frontend multi_resize_output pool
     * can't drain -> "Timeout waiting for used buffers" + "auto_feed
     * add_buffer failed". The suspend flag is checked by BOTH the auto-feed
     * frontend bridge AND the manual input_frame() codec op, so feeding is
     * paused whether the app uses auto_feed or feeds frames itself.
     *
     * Previously this only engaged for resolution changes (new_w>0 && new_h>0);
     * a pure-fps light-path change left feeding live during the vendor's
     * internal stop/start window — exactly the race the old UAF hit. */
    const bool feed_suspend = (priv_for_osd != nullptr) &&
        (resolution.has_value() || framerate.has_value());
    if (feed_suspend)
    {
        std::lock_guard<std::recursive_mutex> lock(priv_for_osd->mutex);
        priv_for_osd->encoder_feed_suspended[stream_id] = true;
    }
    media_library_return r = ml->set_override_parameters(p);

    /* Clear the suspend flag ONLY when the caller isn't managing the
     * lifecycle. In try_light_reconfigure's batch loop, an early clear
     * here re-opens this stream's feed while the NEXT iteration's vendor
     * stop/start (which stops the entire pipeline, not just one stream)
     * is still tearing down — the rapid-switch SEGV race. keep_suspended
     * defers the clear to the caller's FeedResumeGuard. */
    if (feed_suspend && !keep_suspended)
    {
        std::lock_guard<std::recursive_mutex> lock(priv_for_osd->mutex);
        priv_for_osd->encoder_feed_suspended[stream_id] = false;
    }

    if (r != MEDIA_LIBRARY_SUCCESS)
    {
        return hailo15_ml_err(r);
    }

    /* Since medialib 1.13 the OSD/privacy-mask blenders read their config from the
     * profile attached to each encoded buffer, so set_override_parameters() above
     * is all that is needed for the rescaled OSD + masking to take effect — no
     * blender re-push (the pre-1.13 workaround) is required any more. */

    return HAL_OK;
}

} // namespace hailo15::video_ml
