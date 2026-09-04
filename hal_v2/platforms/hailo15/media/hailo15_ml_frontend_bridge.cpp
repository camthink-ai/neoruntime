/**
 * @file hailo15_ml_frontend_bridge.cpp
 */

#include "hailo15_ml_frontend_bridge.hpp"
#include "hailo15_common.hpp"
#include "hailo15_media_priv.hpp"

#include "common/hal_log.h"

#include <hailo/media_library/media_library.hpp>

namespace
{

class FrontendCallbackGuard
{
public:
    explicit FrontendCallbackGuard(Hailo15MediaPriv *p) : p_(p)
    {
        if (!p_)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(p_->callback_lifecycle_mu);
        if (p_->callbacks_quiescing)
        {
            return;
        }
        ++p_->frontend_callbacks_inflight;
        entered_ = true;
    }

    ~FrontendCallbackGuard()
    {
        if (!entered_)
        {
            return;
        }
        std::lock_guard<std::mutex> lock(p_->callback_lifecycle_mu);
        if (--p_->frontend_callbacks_inflight == 0)
        {
            p_->callback_lifecycle_cv.notify_all();
        }
    }

    explicit operator bool() const
    {
        return entered_;
    }

private:
    Hailo15MediaPriv *p_;
    bool entered_{false};
};

} // namespace

namespace
{

/* HAL frame-difference motion engine (the medialib module never runs — stock
 * profiles ship it disabled with an empty analysis stream_id). Called from the
 * frontend bridge on the smallest output stream; downsampled block-mean luma
 * keeps the cost at ~w*h/256 compares per frame. Returns true on a state
 * TRANSITION (caller fires the subscriber outside the lock). */
bool hailo15_motion_detect_update(Hailo15MediaPriv *p, const std::string &sid,
                                  const HailoMediaLibraryBufferPtr &buf, bool &detected_out)
{
    detected_out = p->motion_last_state;
    if (!p->motion_engine_enabled || !p->motion_cb || sid != p->motion_analysis_sid)
    {
        return false;
    }
    if (!buf)
    {
        return false;
    }
    const uint32_t fw = buf->buffer_data ? buf->buffer_data->width : 0U;
    const uint32_t fh = buf->buffer_data ? buf->buffer_data->height : 0U;
    if (fw == 0 || fh == 0)
    {
        return false;
    }
    const uint32_t gw = fw / 16;   /* 16x16 blocks */
    const uint32_t gh = fh / 16;
    if (gw < 2 || gh < 2)
    {
        return false;
    }
    /* Plane 0 access via the buffer's dma-buf-mapped memory. */
    const uint32_t stride = fw;
    uint8_t *base = static_cast<uint8_t *>(buf->get_plane_ptr(0));
    if (!base)
    {
        return false;
    }
    const uint8_t *y = base;

    std::vector<uint8_t> grid(static_cast<size_t>(gw) * gh);
    for (uint32_t bj = 0; bj < gh; ++bj)
    {
        for (uint32_t bi = 0; bi < gw; ++bi)
        {
            uint32_t acc = 0;
            const uint8_t *row = y + (size_t)(bj * 16) * stride + bi * 16;
            for (uint32_t j = 0; j < 16; j += 2)   /* sample every 2nd row/pixel */
            {
                const uint8_t *r = row + (size_t)j * stride;
                for (uint32_t i = 0; i < 16; i += 2)
                {
                    acc += r[i];
                }
            }
            grid[(size_t)bj * gw + bi] = static_cast<uint8_t>(acc / 64);
        }
    }

    bool detected = p->motion_last_state;
    if (p->motion_prev_grid.size() == grid.size() && p->motion_grid_w == gw)
    {
        uint32_t changed = 0;
        const int diff = p->motion_diff_level;
        for (size_t i = 0; i < grid.size(); ++i)
        {
            const int d = (int)grid[i] - (int)p->motion_prev_grid[i];
            if (d > diff || d < -diff)
            {
                ++changed;
            }
        }
        const float ratio = (float)changed / (float)grid.size();
        detected = ratio > p->motion_threshold;
    }
    p->motion_prev_grid = std::move(grid);
    p->motion_grid_w = gw;
    p->motion_grid_h = gh;

    if (detected != p->motion_last_state)
    {
        p->motion_last_state = detected;
        detected_out = detected;
        return true;
    }
    return false;
}

} // namespace

int hailo15_connect_media_priv_frontend(Hailo15MediaPriv *p)
{
    if (!p || !p->media_lib || !p->media_lib->m_frontend)
    {
        return HAL_ERR_INVALID_STATE;
    }
    FrontendCallbacksMap fe_map;
    for (const auto &sid : p->frontend_stream_ids)
    {
        fe_map[sid] = [p, sid](HailoMediaLibraryBufferPtr buf, uint32_t sz) {
            (void)sz;
            if (!buf)
            {
                return;
            }
            FrontendCallbackGuard callback_guard(p);
            if (!callback_guard)
            {
                return;
            }
            (void)buf->sync_start();
            /* Snapshot callback state under lock, then release before invoking
             * user code or MediaLibrary calls.  Holding p->mutex across user
             * callbacks or add_buffer() risks deadlock if those call paths
             * try to lock p->mutex again (e.g. via stop_pipeline). */
            HalVideoFrameCallback cb = nullptr;
            void *cb_ud = nullptr;
            HalVideoContext *vctx = nullptr;
            bool do_auto_feed = false;
            uint64_t seq = 0;
            {
                /* Never hold p->mutex across user callbacks or MediaLibrary calls. */
                std::lock_guard<std::recursive_mutex> lock(p->mutex);
                p->frame_seq++;
                seq = p->frame_seq;

                const auto vsub = p->video_subscribers.find(sid);
                if (vsub != p->video_subscribers.end())
                {
                    cb = vsub->second.first;
                    cb_ud = vsub->second.second;
                }


                const auto vs_it = p->video_by_stream.find(sid);
                if (vs_it != p->video_by_stream.end())
                {
                    vctx = vs_it->second;
                }

                do_auto_feed = p->encoder_auto_feed_default;
                const auto af_it = p->encoder_auto_feed_by_stream.find(sid);
                if (af_it != p->encoder_auto_feed_by_stream.end())
                {
                    do_auto_feed = af_it->second;
                }
                /* Transient suspend during a resolution change (see apply_frontend_stream_override).
                 * While suspended: (a) skip auto-forwarding to the encoder (avoids a
                 * stride-mismatched buffer mid-reconfigure -> VCEnc -3); (b) stop delivering new
                 * frames to the user callback, so the app can't grab/hold a fresh buffer during the
                 * switch window; (c) manual input_frame() is gated the same way. */
                const auto sus_it = p->encoder_feed_suspended.find(sid);
                const bool suspended = (sus_it != p->encoder_feed_suspended.end() && sus_it->second);
                if (suspended)
                {
                    do_auto_feed = false;
                    cb = nullptr; /* don't deliver new frames to the user during the switch */
                }
            }

            if (cb)
            {
                HalFrameBuffer frame{};
                hailo15_fill_frame_from_buffer(buf, &frame);
                frame.sequence = static_cast<uint32_t>(seq);
                /* Upper layer may attach AI results to frame->priv->ml_buf->m_analytics_metadata
                 * here (HalMediaOps.attach_frame_analytics); the blender consumes it during
                 * add_buffer() below. The HAL does not auto-attach — the upper layer decides. */
                cb(vctx, &frame, cb_ud);
            }

            /* Motion events: HAL frame-difference engine on the analysis stream,
             * fired on state transitions only. */
            {
                HalMotionCallback mcb = nullptr;
                void *mcb_ud = nullptr;
                bool fire = false;
                bool detected = false;
                {
                    std::lock_guard<std::recursive_mutex> lock(p->mutex);
                    fire = hailo15_motion_detect_update(p, sid, buf, detected);
                    if (fire)
                    {
                        mcb = p->motion_cb;
                        mcb_ud = p->motion_cb_user;
                    }
                }
                if (fire)
                {
                    mcb(p->hal_media_ctx, detected, seq, buf->pts, mcb_ud);
                }
            }

            if (do_auto_feed)
            {
                auto enc_it = p->media_lib->m_encoders.find(sid);
                if (enc_it != p->media_lib->m_encoders.end() && enc_it->second)
                {
                    media_library_return mr = enc_it->second->add_buffer(buf);
                    if (mr != MEDIA_LIBRARY_SUCCESS && p->feed_err_count[sid]++ < 5)
                    {
                        HAL_LOG_ERROR("hailo15_media: auto_feed add_buffer('%s') failed: %d",
                                      sid.c_str(), static_cast<int>(mr));
                    }
                }
                else if (p->feed_err_count[sid]++ < 5)
                {
                    HAL_LOG_ERROR("hailo15_media: auto_feed: no encoder for '%s' (encoders=%zu)",
                                  sid.c_str(), p->media_lib->m_encoders.size());
                }
            }
            (void)buf->sync_end();
        };
    }
    media_library_return r = p->media_lib->subscribe_to_frontend_output(fe_map);
    return hailo15_ml_err(r);
}

int hailo15_connect_csi_medialib_frontend(const MediaLibraryPtr &media_lib,
                                            const std::vector<std::string> &frontend_stream_ids,
                                            std::recursive_mutex &mutex, uint64_t *frame_seq,
                                            std::map<std::string, std::pair<HalVideoFrameCallback, void *>> &video_subscribers,
                                            HalVideoContext *single_vc)
{
    if (!media_lib || !media_lib->m_frontend || !frame_seq)
    {
        return HAL_ERR_INVALID_STATE;
    }
    FrontendCallbacksMap fe_map;
    for (const auto &sid : frontend_stream_ids)
    {
        fe_map[sid] = [&mutex, frame_seq, &video_subscribers, single_vc, sid](HailoMediaLibraryBufferPtr buf,
                                                                            uint32_t sz) {
            (void)sz;
            if (!buf)
            {
                return;
            }
            (void)buf->sync_start();
            HalVideoFrameCallback cb = nullptr;
            void *cb_ud = nullptr;
            uint64_t seq = 0;
            {
                std::lock_guard<std::recursive_mutex> lock(mutex);
                (*frame_seq)++;
                seq = *frame_seq;
                const auto vsub = video_subscribers.find(sid);
                if (vsub != video_subscribers.end())
                {
                    cb = vsub->second.first;
                    cb_ud = vsub->second.second;
                }
            }

            if (cb)
            {
                HalFrameBuffer frame{};
                hailo15_fill_frame_from_buffer(buf, &frame);
                frame.sequence = static_cast<uint32_t>(seq);
                cb(single_vc, &frame, cb_ud);
            }
            (void)buf->sync_end();
        };
    }
    media_library_return r = media_lib->subscribe_to_frontend_output(fe_map);
    return hailo15_ml_err(r);
}
