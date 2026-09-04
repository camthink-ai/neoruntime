/**
 * @file hailo15_media_priv.hpp
 * @brief Shared MediaLibrary state for Hailo-15 HAL media / video / codec adapters.
 */
#pragma once

#include <hailo/media_library/media_library.hpp>

#include "media/hal_codec_internal.h"
#include "media/hal_media_internal.h"
#include "media/hal_video_internal.h"

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

    std::map<std::string, HalVideoContext *> video_by_stream;
    std::map<std::string, HalCodecContext *> codec_by_stream;

    std::map<std::string, std::pair<HalVideoFrameCallback, void *>> video_subscribers;
    std::map<std::string, std::pair<HalCodecFrameCallback, void *>> codec_packet_subscribers;

    /** Per-encoder OSD layout state for font-size rescaling on resolution/rotation changes. */
    std::map<std::string, OsdLayoutState> osd_layout_by_encoder;

    /** Patched encoder dimensions from camera-daemon overrides (stream_id → {width, height}).
     *  Cached at init time so rotation swaps use the correct (patched) dimensions
     *  instead of the sensor resolution stored in the medialib profile. */
    std::map<std::string, std::pair<uint32_t, uint32_t>> encoder_patched_dims;

    /** Privacy mask original polygons in normalized space (unrotated/unflipped).
     *  Keyed by mask id; values are list of (x,y) points in [0..1]. */
    std::map<std::string, std::vector<std::pair<float, float>>> privacy_mask_original;
    /** Last display-space polygons received from user (normalized in current displayed frame). */
    std::map<std::string, std::vector<std::pair<float, float>>> privacy_mask_last_display;

    bool callbacks_registered{false};

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

    /* Diagnostics: per-stream counters for troubleshooting buffer pool exhaustion. */
    std::map<std::string, size_t> enc_pkt_count;
    std::map<std::string, size_t> feed_err_count;

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
