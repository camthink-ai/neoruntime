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
#include <functional>
#include <mutex>
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

    void destroy(const std::string& stream_name);
    void destroy_all();

    /**
     * Forget locally registered FROM_MEDIA contexts without calling HAL.
     *
     * MediaLibrary may have already destroyed/replaced these borrowed
     * HalCodecContext objects during a pipeline reconfiguration. Dereferencing
     * them to unsubscribe would be a use-after-free.
     */
    void discard_from_media();

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
        // Frame-health signals (steady_clock — monotonic, immune to NTP jumps).
        std::chrono::steady_clock::time_point created_at;
        std::chrono::steady_clock::time_point last_packet_time;  // epoch == never received
        uint64_t packet_sequence = 0;
        bool        logged_first_packet = false;
    };

    HalCodecOps* ops_;

    bool subscribe_output(void* ctx, const std::string& stream_name);
    mutable std::mutex mu_;
    std::unordered_map<std::string, EncoderSlot> encoders_;
    PacketOutputFn output_fn_;

    // Reverse map: codec_ctx → stream_name (for callback routing)
    std::unordered_map<void*, std::string> handle_to_stream_;

    /** HAL Codec push mode callback */
    static void codec_output_callback(void* codec_ctx,
                                       HalPacketBuffer* packet,
                                       void* userdata);
    void on_packet(void* codec_ctx, const HalPacketBuffer* packet);
};
