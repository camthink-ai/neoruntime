/**
 * @file audio_service.h
 * @brief Audio capture/playback service wrapping HalAudioOps
 *
 * Manages audio HAL lifecycle: capture from microphone with encoding (AAC/G711),
 * playback to speaker, volume/mute control, device enumeration.
 * Encoded capture packets are forwarded to EncodedPublisher for distribution.
 */

#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <functional>

extern "C" {
#include "media/hal_audio.h"
}

class EncodedPublisher;

struct AudioCfg {
    bool        enabled = false;
    std::string audio_lib;
    std::string capture_device = "default";
    uint32_t    sample_rate = 48000;
    uint32_t    channels = 1;
    std::string codec = "aac";      // "aac", "g711a", "g711u", "pcm"
    uint32_t    bitrate = 128000;
    float       volume = 1.0;
    bool        mute = false;
};

class AudioService {
public:
    explicit AudioService(HalAudioOps* ops);
    ~AudioService();

    AudioService(const AudioService&) = delete;
    AudioService& operator=(const AudioService&) = delete;

    bool init(const AudioCfg& cfg);
    void deinit();

    // Capture
    bool start_capture();
    void stop_capture();
    bool is_capturing() const { return capturing_.load(); }

    // Playback
    bool start_playback(const std::string& device, uint32_t sample_rate,
                        uint32_t channels, const std::string& codec);
    void stop_playback();
    bool is_playing() const { return playing_.load(); }
    bool write_pcm(const uint8_t* data, size_t size);

    // Runtime controls
    bool set_volume(float volume);
    bool set_mute(bool mute);

    struct AudioStatus {
        bool   capturing = false;
        bool   playing = false;
        std::string device;
        uint32_t sample_rate = 0;
        uint32_t channels = 0;
        std::string codec;
        float  volume = 1.0f;
        bool   mute = false;
    };
    bool get_status(HalAudioIoDirection dir, AudioStatus& status);

    // Device enumeration
    struct DeviceInfo {
        std::string name;
        std::string id;
    };
    std::vector<DeviceInfo> list_capture_devices();
    std::vector<DeviceInfo> list_playback_devices();

    void set_encoded_publisher(EncodedPublisher* pub) { encoded_pub_ = pub; }

private:
    HalAudioOps*       ops_ = nullptr;
    EncodedPublisher*  encoded_pub_ = nullptr;
    // Full-duplex: capture and playback own independent HAL contexts (and thus
    // independent ALSA handles). Neither path tears the other down.
    void*              capture_ctx_ = nullptr;
    void*              playback_ctx_ = nullptr;
    std::mutex         capture_mu_;   // guards capture_ctx_ + cfg_
    std::mutex         playback_mu_;  // guards playback_ctx_ + pb_sample_rate_/pb_channels_
    std::atomic<bool>  capturing_{false};
    std::atomic<bool>  playing_{false};

    AudioCfg           cfg_;          // capture-only config (listen path)
    // Playback format stamped into each write_pcm() buffer; set by
    // start_playback(), read by write_pcm() under playback_mu_.
    uint32_t           pb_sample_rate_ = 0;
    uint32_t           pb_channels_ = 0;

    static void packet_callback(void* audio_ctx, HalPacketBuffer* packet, void* user_data);
    static void pcm_callback(void* audio_ctx, HalAudioBuffer* frame, void* user_data);
    void on_encoded_packet(const HalPacketBuffer* packet);
    void on_pcm_frame(const HalAudioBuffer* frame);

    // Helper used by start_capture(). Assumes capture_mu_ is held and ops_ non-null.
    HalAudioConfig build_capture_hal_cfg_() const;  // builds capture HalAudioConfig from cfg_
    bool start_capture_locked_();                    // subscribes callbacks + starts the HAL
};
