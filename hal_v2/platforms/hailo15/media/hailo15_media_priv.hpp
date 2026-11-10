/**
 * @file hailo15_media_priv.hpp
 * @brief Shared MediaLibrary state for Hailo-15 HAL media / video / codec adapters.
 */
#pragma once

#include <hailo/media_library/media_library.hpp>

#include "media/hal_codec_internal.h"
#include "media/hal_media_internal.h"
#include "media/hal_osd.h"
#include "media/hal_video_internal.h"

#include "hailo15_feed_health.hpp"

#include <map>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

/** Tracks per-encoder-stream OSD layout dimensions/rotation for rescaling on changes. */
struct OsdLayoutState
{
    uint32_t width{0};
    uint32_t height{0};
    int rotation{0}; /**< HalRotationAngle cast to int */
};

struct Hailo15MediaPriv
{
    std::recursive_mutex mutex;
    MediaLibraryPtr media_lib;
    HalMediaContext *hal_media_ctx{nullptr};

    std::string stored_config_json;
    std::string stored_config_path;
    /** Effective default backup dir after init (HAL backup_folder_path if set, else JSON field); restored after backup_current_profile(). */
    std::string medialib_default_backup_folder;
    HalMediaImageConfig image_overrides{};
    std::string encoder_overrides_json;

    /** Last JSON string for get_current_profile_json (active profile only). */
    std::string current_profile_json;
    /** Last value string returned by get_config_field (owned by the impl). */
    std::string current_config_field_value;

    std::vector<std::string> profile_names;
    /** Authored iq_settings.grayscale.enabled per profile, snapshotted once at first
     *  init from the pristine (pre-override) SDK state. The live value is unsafe
     *  afterwards: set_override_parameters() replaces the stored profile_by_name
     *  entries with toggled values (see profile_authored_grayscale in
     *  hailo15_media_impl.cpp). Survives media_lib reinits; deliberately NOT
     *  rebuilt from patched json. */
    std::map<std::string, bool> authored_profile_grayscale;
    std::vector<std::string> frontend_stream_ids;
    std::vector<std::string> encoder_stream_ids;
    /** Snapshot after last successful build; used to detect profile-only updates vs stream layout changes. */
    std::vector<std::string> last_frontend_stream_ids;
    std::vector<std::string> last_encoder_stream_ids;

    bool pipeline_started{false};
    /** Global default: when true, frontend output buffers are also passed to same-id encoder (see set_encoder_auto_feed). */
    bool encoder_auto_feed_default{true};
    /** Per-stream override of auto-feed behavior (see set_encoder_auto_feed_for_stream). */
    std::map<std::string, bool> encoder_auto_feed_by_stream;
    /** Transient: per-stream encoder feeding is suspended during a resolution change
     * (set by apply_frontend_stream_override around set_override_parameters). Checked by BOTH
     * the auto-feed frontend bridge AND the manual input_frame() codec op, so feeding is paused
     * regardless of whether the app uses auto_feed or feeds frames itself — prevents the
     * encoder from receiving a stride-mismatched buffer mid-reconfigure (VCEnc -3). */
    std::map<std::string, bool> encoder_feed_suspended;

    uint64_t frame_seq{0};
    uint64_t packet_seq{0};

    /**
     * Leaf lock guarding hm->video_ctx_list / hm->codec_ctx_list (+counts and
     * the by_stream maps) against get_codec_names() snapshots. destroy_contexts()
     * and build_contexts() mutate the lists under it; it is NEVER held across
     * MediaLibrary calls (build_contexts takes it only after its
     * get_current_profile() returns) and never nests with mutex, so no
     * lock-order interaction exists. Exists because daemon-side codec-name
     * probes (zombie heal / profile-switch reconcile) cannot hold the daemon
     * serialization locks without an AB-BA against switch_profile_internal —
     * the snapshot op lets them read names race-free instead.
     */
    std::mutex ctx_list_mu;

    std::map<std::string, HalVideoContext *> video_by_stream;
    std::map<std::string, HalCodecContext *> codec_by_stream;

    std::map<std::string, std::pair<HalVideoFrameCallback, void *>> video_subscribers;
    std::map<std::string, std::pair<HalCodecFrameCallback, void *>> codec_packet_subscribers;

    /** FROM_MEDIA packet callbacks currently running, per encoder id. The
     *  dispatch lambda copies the subscriber under `mutex` and invokes it
     *  AFTER unlocking, so hailo15_codec_unsubscribe() must wait the eid's
     *  count back to zero before returning — a late callback would reach
     *  the manager after it dropped the ctx's release-only bookkeeping and
     *  leak the packet buffer. Counted per eid, not globally: unsubscribe
     *  must only wait out ITS stream's callbacks; a sibling stream that is
     *  actively producing (or wedged) must not turn an unrelated teardown
     *  into a spurious timeout. Guarded by packet_cb_mu; dispatchers nest
     *  mutex -> packet_cb_mu, the unsubscribe waiter takes packet_cb_mu
     *  alone after releasing `mutex`, so the orderings never conflict. */
    std::map<std::string, int> packet_cb_in_flight;
    std::mutex packet_cb_mu;
    std::condition_variable packet_cb_cv;

    /** Per-encoder OSD layout state for font-size rescaling on resolution/rotation changes. */
    std::map<std::string, OsdLayoutState> osd_layout_by_encoder;

    /** OSD declarative-model shadow state (medialib >= 1.13): config_stream_osd_t has no
     *  enabled flag — presence in the overlay vectors == blended — so disable = remove from
     *  the profile and snapshot here; enable = convert the snapshot back into the profile.
     *  Keyed by encoder stream_id, then overlay id. Cleared by osd clear / layout changes. */
    std::map<std::string, std::map<std::string, HalOsdOverlay>> osd_disabled_by_stream;
    /** Guards osd_disabled_by_stream AND serializes OSD ops (profile fetch -> mutate ->
     *  set_override_parameters runs entirely under this lock). Never held together with
     *  `mutex` — the existing discipline forbids holding `mutex` across MediaLibrary
     *  calls, and OSD ops never acquire `mutex`. */
    std::mutex osd_state_mu;

    /** Privacy mask original polygons in normalized space (unrotated/unflipped).
     *  Keyed by mask id; values are list of (x,y) points in [0..1]. */
    std::map<std::string, std::vector<std::pair<float, float>>> privacy_mask_original;
    /** Last display-space polygons received from user (normalized in current displayed frame). */
    std::map<std::string, std::vector<std::pair<float, float>>> privacy_mask_last_display;

    bool callbacks_registered{false};

    /* Timestamp of the last completed light-path reconfigure. Rapid
     * consecutive vendor stop/start cycles (user clicking through resolution
     * options) race dataflow threads still recovering from the previous
     * teardown — the 2026-11-06 SEGV on 3 reconfigures in 6 seconds. The
     * light path checks this and falls back to full reinit within the
     * cooldown. Not thread-safe: written and read only from the HAL's
     * serialized layout-op paths. */
    std::chrono::steady_clock::time_point last_light_reconfigure_at{};

    /**
     * Frontend callback lifecycle barrier used during MediaLibrary teardown.
     * callbacks_quiescing prevents new callbacks from entering while
     * frontend_callbacks_inflight lets teardown wait for add_buffer() calls
     * already in progress before stopping the GStreamer pipeline.
     */
    std::mutex callback_lifecycle_mu;
    std::condition_variable callback_lifecycle_cv;
    bool callbacks_quiescing{false};
    uint32_t frontend_callbacks_inflight{0};

    /* Auto-feed failure streaks, updated lock-free from GStreamer streaming
     * threads. Entries are created once at frontend-connect time; afterwards
     * only the atomics inside each entry mutate (see hailo15_feed_health.hpp).
     * Keyed by frontend stream id ("sink0"...). Read via the
     * HAL_MEDIA_OPS.get_feed_health trailing op.
     *
     * THREAD-SAFETY INVARIANT: the map's STRUCTURE (insert/erase) may only
     * change while frontend callbacks are guaranteed stopped — i.e. inside
     * connect, which runs before subscribe or after reinit teardown. Never
     * insert entries from any other context: concurrent streaming threads
     * run find() on this map without a lock. */
    std::map<std::string, std::unique_ptr<Hailo15FeedHealth>> feed_health;

    /* Thermal throttling subscription (subscribe_throttling). Guarded by mutex. */
    HalThrottlingCallback throttling_cb{nullptr};
    void *throttling_cb_user{nullptr};

    /* Motion detection subscription (subscribe_motion). Guarded by mutex.
     * Events come from the HAL's own frame-difference engine (see
     * hailo15_motion_detect_update): the medialib module is effectively
     * unusable — every stock profile ships it disabled and its analysis
     * stream_id is never populated (output_frames lookup silently skips). */
    HalMotionCallback motion_cb{nullptr};
    void *motion_cb_user{nullptr};
    bool motion_last_state{false};
    bool motion_engine_enabled{false};
    float motion_threshold{0.05f};
    int motion_diff_level{24}; /* per-pixel delta 8..40 mapped from sensitivity */
    std::string motion_analysis_sid;
    std::vector<uint8_t> motion_prev_grid; /* downsampled luma of previous frame */
    uint32_t motion_grid_w{0};
    uint32_t motion_grid_h{0};
    /* Analysis ROI in pixels of the motion analysis stream (the smallest
     * output stream); all-zero = full frame. The frame-difference engine
     * only compares blocks intersecting this ROI. */
    uint32_t motion_roi_x{0}, motion_roi_y{0}, motion_roi_w{0}, motion_roi_h{0};
};

inline Hailo15MediaPriv *hailo15_media_priv_from_hal(void *media_ctx)
{
    if (!media_ctx)
    {
        return nullptr;
    }
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    return static_cast<Hailo15MediaPriv *>(hm->priv);
}
