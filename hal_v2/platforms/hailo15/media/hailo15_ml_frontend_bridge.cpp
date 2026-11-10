/**
 * @file hailo15_ml_frontend_bridge.cpp
 */

#include "hailo15_ml_frontend_bridge.hpp"
#include "hailo15_common.hpp"
#include "hailo15_media_priv.hpp"

#include "common/hal_log.h"

#include <hailo/media_library/media_library.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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
        /* Restrict the comparison to blocks intersecting the configured ROI
         * (all-zero ROI = full frame). The trigger ratio counts ROI blocks
         * only, so motion outside the ROI cannot fire callbacks. */
        uint32_t bx0 = 0, bx1 = gw, by0 = 0, by1 = gh;
        if (p->motion_roi_w > 0 && p->motion_roi_h > 0)
        {
            const uint32_t rx1 = std::min(p->motion_roi_x + p->motion_roi_w, fw);
            const uint32_t ry1 = std::min(p->motion_roi_y + p->motion_roi_h, fh);
            bx0 = std::min(p->motion_roi_x / 16, gw);
            bx1 = std::min((rx1 + 15) / 16, gw);
            by0 = std::min(p->motion_roi_y / 16, gh);
            by1 = std::min((ry1 + 15) / 16, gh);
        }
        uint32_t changed = 0, total = 0;
        const int diff = p->motion_diff_level;
        for (uint32_t bj = by0; bj < by1; ++bj)
        {
            for (uint32_t bi = bx0; bi < bx1; ++bi)
            {
                const int d = (int)grid[(size_t)bj * gw + bi] - (int)p->motion_prev_grid[(size_t)bj * gw + bi];
                if (d > diff || d < -diff)
                {
                    ++changed;
                }
                ++total;
            }
        }
        const float ratio = total ? (float)changed / (float)total : 0.0f;
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

namespace
{

/* Alarm window for uninterrupted auto-feed failures. Two seconds covers the
 * transient misses a reinit ramp-up produces (~a few frames at 30fps) while
 * still latching long before the daemon-side 10s encoded-gap stall monitor
 * would, so the FEED_STALL_ALARM names the cause (encoder input rejected)
 * ahead of the symptom (no encoded packets). */
constexpr uint32_t kFeedStallWindowMs = 2000;

/* Debug injection: set AIPC_HAL_INJECT_FEED_FAIL=<stream_id> (e.g. "sink1")
 * to force add_buffer failures for that one stream. Optional "@<seconds>"
 * suffix delays onset (e.g. "sink1@30" fails starting 30s after HAL load) so
 * the stream first produces packets — exercising the daemon's encoded-gap
 * rebuild path exactly like a real mid-stream caps/layout fault. The stream
 * really starves while active — test-only, never ship enabled. */
struct FeedInjectPlan
{
    const char *sid = nullptr;   /* nullptr: injection disabled */
    uint64_t after_ms = 0;
    std::chrono::steady_clock::time_point loaded_at{};
};

bool feed_fail_inject_active(const std::string &sid)
{
    static const FeedInjectPlan plan = []() {
        FeedInjectPlan p;
        p.loaded_at = std::chrono::steady_clock::now();
        const char *raw = std::getenv("AIPC_HAL_INJECT_FEED_FAIL");
        if (raw == nullptr || raw[0] == '\0')
        {
            return p;
        }
        /* Split "sink1@30" -> sid "sink1", onset 30s (default 0 = immediate). */
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s", raw);
        uint64_t delay_s = 0;
        char *at = std::strchr(buf, '@');
        if (at != nullptr)
        {
            *at = '\0';
            delay_s = static_cast<uint64_t>(std::strtoull(at + 1, nullptr, 10));
        }
        static char sid_buf[64];
        std::snprintf(sid_buf, sizeof(sid_buf), "%s", buf);
        p.sid = sid_buf;
        p.after_ms = delay_s * 1000;
        return p;
    }();
    if (plan.sid == nullptr)
    {
        return false;
    }
    if (sid != plan.sid)
    {
        return false;
    }
    if (plan.after_ms == 0)
    {
        return true;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - plan.loaded_at).count() >=
           static_cast<int64_t>(plan.after_ms);
}

/* Steady-clock milliseconds for the streak window math. */
uint64_t feed_steady_ms()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
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
        /* Feed-health entries are created here (connect is single-threaded);
         * the callback below only touches the atomics inside them. */
        p->feed_health.try_emplace(sid, std::make_unique<Hailo15FeedHealth>());
        p->encoder_feed_suspended.try_emplace(sid, false);
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
                /* Feed-health entry is prebuilt at connect; lock-free atomics
                 * only on this GStreamer streaming thread. */
                auto fh_it = p->feed_health.find(sid);
                Hailo15FeedHealth *fh = (fh_it != p->feed_health.end()) ? fh_it->second.get() : nullptr;

                media_library_return mr = media_library_return::MEDIA_LIBRARY_SUCCESS;
                auto enc_it = p->media_lib->m_encoders.find(sid);
                /* Debug injection (AIPC_HAL_INJECT_FEED_FAIL[=@delay_s]). */
                const bool injected = feed_fail_inject_active(sid);
                if (!injected && enc_it != p->media_lib->m_encoders.end() && enc_it->second)
                {
                    mr = enc_it->second->add_buffer(buf);
                }
                else if (!injected)
                {
                    /* Frames arrive but the encoder object is gone — same
                     * user-visible starvation as a rejected buffer. Sentinel
                     * -1 marks "no encoder" in the health snapshot. */
                    mr = media_library_return::MEDIA_LIBRARY_ERROR;
                    if (fh)
                    {
                        const bool alarm = feed_health_record_failure(*fh, -1, feed_steady_ms(),
                                                                      kFeedStallWindowMs);
                        /* Early-failure diagnostics keyed on the CURRENT streak
                         * (consec), not the lifetime total — a second fault in
                         * the same process must still get its first-3 logs. */
                        if (alarm || fh->consecutive_failures.load(std::memory_order_relaxed) <= 3)
                        {
                            HAL_LOG_ERROR("hailo15_media: auto_feed: no encoder for '%s' (encoders=%zu)%s",
                                          sid.c_str(), p->media_lib->m_encoders.size(),
                                          alarm ? " [FEED_STALL]" : "");
                        }
                    }
                    fh = nullptr; /* already accounted above */
                }
                else
                {
                    /* Injected failure: synthesize the vendor error code used
                     * by a rejected push, without calling add_buffer. */
                    mr = media_library_return::MEDIA_LIBRARY_ERROR;
                }

                if (fh)
                {
                    if (mr != media_library_return::MEDIA_LIBRARY_SUCCESS)
                    {
                        const uint64_t now_ms = feed_steady_ms();
                        const uint32_t consec =
                            fh->consecutive_failures.load(std::memory_order_relaxed) + 1;
                        const bool alarm = feed_health_record_failure(
                            *fh, static_cast<int32_t>(mr), now_ms, kFeedStallWindowMs);
                        if (alarm)
                        {
                            HAL_LOG_ERROR("hailo15_media: FEED_STALL_ALARM stream='%s' "
                                          "consec=%u window_ms=%u vendor_ret=%d "
                                          "(encoder input rejected persistently — suspected "
                                          "caps/layout mismatch)",
                                          sid.c_str(), consec, kFeedStallWindowMs,
                                          static_cast<int>(mr));
                        }
                        else if (consec <= 3)
                        {
                            HAL_LOG_ERROR("hailo15_media: auto_feed add_buffer('%s') failed: %d",
                                          sid.c_str(), static_cast<int>(mr));
                        }
                    }
                    else
                    {
                        const bool was_latched = feed_health_record_success(*fh);
                        if (was_latched)
                        {
                            HAL_LOG_INFO("hailo15_media: FEED_RECOVERED stream='%s' "
                                         "(auto-feed accepted again)",
                                         sid.c_str());
                        }
                    }
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
