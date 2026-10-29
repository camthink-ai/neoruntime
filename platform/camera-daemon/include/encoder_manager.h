/**
 * @file encoder_manager.h
 * @brief Encoder Manager - Per-stream HAL Codec encoder management
 *
 * Uses HAL Codec push mode (subscribe) for encoded output.
 * void* codec_ctx, HalCodecConfig, HalPacketBuffer.
 */

#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cstdint>

extern "C" {
    #include "hal_codec.h"
    #include "hal_buffer.h"
}

/** Callback for encoded packet output (to RTSP/HLS/file/gRPC) */
using PacketOutputFn = std::function<void(const std::string& stream_name,
                                           const HalPacketBuffer* packet)>;

class EncoderManager {
public:
    explicit EncoderManager(HalCodecOps* ops);
    ~EncoderManager();

    EncoderManager(const EncoderManager&) = delete;
    EncoderManager& operator=(const EncoderManager&) = delete;

    /** Create encoder for a stream */
    bool create(const std::string& stream_name,
                const HalCodecConfig& config);

    /**
     * @brief Create encoder from pre-existing FROM_MEDIA context
     * @param stream_name Stream identifier
     * @param codec_ctx Pre-created codec context from get_codec_list()
     * @return true on success
     *
     * Does NOT call HAL_CODEC_OPS.init() — context is already created
     * by the media pipeline. Registers output callback via subscribe().
     */
    bool create_from_context(const std::string& stream_name, void* codec_ctx);

    /**
     * Tear down one encoder. Returns false when the bounded drain of
     * in-flight force_keyframe() calls timed out: the HAL teardown was
     * ABORTED (ctx leaked on purpose — freeing it would race the wedged
     * call) and the slot is parked as a closing tombstone; callers must
     * abort the pipeline teardown they were about to perform and surface
     * the failure. A later destroy() of the same name retries the parked
     * teardown and reclaims the ctx once the call finally returns — unless
     * another thread already owns that retry (teardown_in_progress), in
     * which case this call also returns false without touching the ctx.
     */
    bool destroy(const std::string& stream_name);

    /**
     * destroy() every registered encoder. Returns false if ANY teardown
     * aborted on drain timeout (see destroy()). Shutdown paths may ignore
     * the result; rebuild paths must not proceed on false.
     */
    bool destroy_all();

    /**
     * Gate keyframe requests during pipeline rebuilds. MediaLibrary frees
     * borrowed FROM_MEDIA contexts on reinit while encoders_ still maps the
     * stale pointers (cleared later by resync/discard_from_media); callers
     * must set this across every stop->start media reinit window so
     * force_keyframe() returns -1 instead of touching freed contexts.
     */
    void set_rebuild_active(bool active) { rebuild_active_.store(active); }
    bool is_rebuild_active() const { return rebuild_active_.load(); }

    /**
     * Bounded wait for in-flight force_keyframe() HAL calls to return.
     * Teardown paths that recycle borrowed FROM_MEDIA contexts (rebuild
     * windows, destroy) drain first so a call that already captured the ctx
     * pointer is not still inside the HAL when the ctx goes away. Returns
     * false on timeout — the caller MUST abort the teardown it was about to
     * run: medialib's force_idr has no deadline, and recycling a ctx the
     * call still occupies is a use-after-free, not an acceptable fallback.
     */
    bool wait_keyframe_calls_drained(int timeout_ms = 2000);

    /**
     * Forget locally registered FROM_MEDIA contexts without calling HAL.
     *
     * MediaLibrary may have already destroyed/replaced these borrowed
     * HalCodecContext objects during a pipeline reconfiguration. Dereferencing
     * them to unsubscribe would be a use-after-free.
     */
    void discard_from_media();

    /**
     * HAL codec output callback body (reached via codec_output_callback).
     * Public so tests can drive parked-ctx packet-release behavior.
     */
    void on_packet(void* codec_ctx, const HalPacketBuffer* packet);

    bool start(const std::string& stream_name);
    bool stop(const std::string& stream_name);

    /** Feed one frame to encoder (zero-copy for DMA-BUF) */
    int encode_frame(const std::string& stream_name, HalFrameBuffer* frame);

    /** Runtime parameter adjustment */
    int set_bitrate(const std::string& stream_name, uint32_t bitrate_bps);
    int set_framerate(const std::string& stream_name, uint32_t fps);
    int set_gop(const std::string& stream_name, uint32_t gop);
    int force_keyframe(const std::string& stream_name);

    /**
     * @brief Full encoder reconfiguration via dynamic_change_config
     */
    int reconfigure(const std::string& stream_name,
                    const HalCodecConfig& new_config,
                    uint32_t* interrupt_ms);

    /** Register encoded output callback */
    void set_output_callback(PacketOutputFn fn);

    bool has_encoder(const std::string& stream_name) const;

    /** Milliseconds since the last encoded packet (UINT64_MAX if no encoder or never received). */
    uint64_t ms_since_last_packet(const std::string& stream_name) const;

    /**
     * @brief True if the stream has stopped producing encoded frames.
     * @param threshold_ms Report stalled once this long passes without a packet.
     * @param grace_ms     Suppress the stall verdict for an encoder younger than
     *                     this, so freshly-created encoders get their first frames
     *                     out without false-alarming (esp. after a profile switch).
     */
    bool is_stream_stalled(const std::string& stream_name,
                           uint64_t threshold_ms,
                           uint64_t grace_ms) const;

    /** True if this stream has produced at least one encoded packet. */
    bool seen_first_packet(const std::string& stream_name) const;

    /** Monotonic encoded-packet counter for this stream (0 if missing). */
    uint64_t packet_sequence(const std::string& stream_name) const;

    /** Get codec context by stream name (nullptr if not found) */
    void* get_codec_ctx(const std::string& stream_name) const;

    /** Get codec ops pointer */
    HalCodecOps* ops() const { return ops_; }

private:
    struct EncoderSlot {
        void*       codec_ctx = nullptr;
        std::string stream_name;
        bool        running = false;
        bool        from_media = false;
        // Closing-tombstone only: a thread exclusively owns this teardown
        // (set atomically when the slot is parked or a retry claims it, so a
        // concurrent destroy() cannot run stop/unsubscribe/deinit on the
        // same ctx a second time — that is a double-free). Cleared when a
        // drain timeout hands the tombstone back for a later retry.
        bool        teardown_in_progress = false;
        // Frame-health signals (steady_clock — monotonic, immune to NTP jumps).
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point last_packet_time;  // epoch == never received
        uint64_t packet_sequence = 0;
        bool        logged_first_packet = false;
    };

    HalCodecOps* ops_;
    std::atomic<bool> rebuild_active_{false};

    bool subscribe_output(void* ctx, const std::string& stream_name);
    mutable std::mutex mu_;
    std::unordered_map<std::string, EncoderSlot> encoders_;
    /** Slots whose HAL teardown aborted on drain timeout (destroy() == false).
     *  Kept so a retry can reclaim the ctx once the wedged call returns; the
     *  entries are invisible to has_encoder()/force_keyframe()/create(). */
    std::unordered_map<std::string, EncoderSlot> closing_teardowns_;
    /** ctx pointers whose packets must be dropped-but-RELEASED: (a) parked
     *  closing tombstones above (subscription still live after a drain
     *  abort), and (b) pending creations — a running FROM_MEDIA encoder can
     *  deliver the moment subscribe() installs the callback, before the
     *  promotion critical section adds the routing maps. Every packet owns
     *  a shared buffer reference that must be returned. */
    std::unordered_set<void*> release_only_ctxs_;
    /** Names reserved by an in-flight create()/create_from_context() (mu_):
     *  the existence check and the final insert are separate critical
     *  sections around an unlocked HAL phase, so without the reservation two
     *  concurrent creates of one name would both subscribe and the later
     *  insert would overwrite (and leak) the earlier ctx. destroy()/destroy_
     *  all() wait on creating_cv_ for reservations to clear so they never
     *  report "all gone" while a ctx is about to be published. */
    std::unordered_set<std::string> creating_names_;
    /** Signaled when creating_names_ loses an entry (mu_ held). */
    std::condition_variable creating_cv_;
    /** True across destroy_all()'s snapshot+teardown window (mu_): new
     *  create reservations are rejected so no ctx can be published right
     *  after destroy_all() reported all-clear. */
    bool destroy_all_active_ = false;
    PacketOutputFn output_fn_;

    // Reverse map: codec_ctx → stream_name (for callback routing)
    std::unordered_map<void*, std::string> handle_to_stream_;

    // Guarded by mu_: force_keyframe() increments before dropping mu_ for
    // the HAL call and decrements after; destroy()/rebuilds drain on it.
    int keyframes_in_flight_ = 0;
    std::condition_variable keyframe_drained_cv_;

    /** HAL Codec push mode callback */
    static void codec_output_callback(void* codec_ctx,
                                       HalPacketBuffer* packet,
                                       void* userdata);
};
