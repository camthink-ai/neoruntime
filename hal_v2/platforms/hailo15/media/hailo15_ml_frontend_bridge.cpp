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
