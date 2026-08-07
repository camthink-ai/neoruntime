/**
 * @file audio_service.cpp
 * @brief Audio capture/playback service implementation
 */

#include "../include/audio_service.h"
#include "../include/encoded_publisher.h"

extern "C" {
#include "hal_log.h"
#include "hal_buffer.h"
}

#include <cstring>

AudioService::AudioService(HalAudioOps* ops)
    : ops_(ops) {}

AudioService::~AudioService() {
    deinit();
}

bool AudioService::init(const AudioCfg& cfg) {
    if (!ops_ || !ops_->init) return false;

    std::lock_guard<std::mutex> lk(capture_mu_);
    cfg_ = cfg;

    HalAudioConfig hal_cfg = build_capture_hal_cfg_();

    int ret = ops_->init(&hal_cfg, &capture_ctx_);
    if (ret != 0) {
        HAL_LOG_ERROR("Audio HAL init failed: %d", ret);
        return false;
    }

    HAL_LOG_INFO("Audio service initialized: device=%s rate=%u ch=%u codec=%s",
                 cfg_.capture_device.c_str(), cfg_.sample_rate, cfg_.channels,
                 cfg_.codec.c_str());
    return true;
}

void AudioService::deinit() {
    stop_capture();
    stop_playback();

    // Each direction owns its context + lock; tear them down independently.
    {
        std::lock_guard<std::mutex> lk(capture_mu_);
        if (capture_ctx_ && ops_ && ops_->deinit) {
            ops_->deinit(capture_ctx_);
            capture_ctx_ = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lk(playback_mu_);
        if (playback_ctx_ && ops_ && ops_->deinit) {
            ops_->deinit(playback_ctx_);
            playback_ctx_ = nullptr;
        }
    }
}

bool AudioService::start_capture() {
    if (!ops_ || !capture_ctx_) return false;

    std::lock_guard<std::mutex> lk(capture_mu_);
    if (capturing_.load()) return true;

    if (!start_capture_locked_()) return false;

    capturing_.store(true);
    HAL_LOG_INFO("Audio capture started");
    return true;
}

HalAudioConfig AudioService::build_capture_hal_cfg_() const {
    HalAudioConfig hal_cfg = {};
    hal_cfg.type = HAL_AUDIO_TYPE_ALSA;
    hal_cfg.direction = HAL_AUDIO_IO_CAPTURE;
    strncpy(hal_cfg.device, cfg_.capture_device.c_str(),
            sizeof(hal_cfg.device) - 1);
    hal_cfg.sample_rate = cfg_.sample_rate;
    hal_cfg.channels = cfg_.channels;
    hal_cfg.sample_format = HAL_AUDIO_SAMPLE_FMT_S16LE;
    hal_cfg.volume = cfg_.volume;
    hal_cfg.mute = cfg_.mute;

    if (cfg_.codec == "aac") {
        hal_cfg.codec = HAL_AUDIO_CODEC_AAC;
        hal_cfg.bitrate = cfg_.bitrate;
    } else if (cfg_.codec == "g711a") {
        hal_cfg.codec = HAL_AUDIO_CODEC_G711A;
    } else if (cfg_.codec == "g711u") {
        hal_cfg.codec = HAL_AUDIO_CODEC_G711U;
    } else {
        hal_cfg.codec = HAL_AUDIO_CODEC_PCM;
    }
    return hal_cfg;
}

bool AudioService::start_capture_locked_() {
    // Assumes capture_mu_ is held, ops_ non-null, and capture_ctx_ valid.
    // Subscribes the capture callbacks then starts the HAL.
    bool sub_ok = false;
    if (ops_->subscribe_packet) {
        int rc = ops_->subscribe_packet(capture_ctx_, &AudioService::packet_callback, this);
        if (rc == 0) {
            HAL_LOG_INFO("Audio: subscribed encoded packet callback");
            sub_ok = true;
        } else {
            HAL_LOG_INFO("Audio: subscribe_packet returned %d, trying subscribe_pcm", rc);
        }
    }
    if (!sub_ok && ops_->subscribe_pcm) {
        int rc = ops_->subscribe_pcm(capture_ctx_, &AudioService::pcm_callback, this);
        if (rc == 0) {
            HAL_LOG_INFO("Audio: subscribed PCM callback");
            sub_ok = true;
        } else {
            HAL_LOG_WARNING("Audio: subscribe_pcm also failed: %d", rc);
        }
    }
    if (!sub_ok) {
        HAL_LOG_WARNING("Audio: no callback subscription succeeded");
    }

    int ret = ops_->start ? ops_->start(capture_ctx_) : -1;
    if (ret != 0) {
        HAL_LOG_ERROR("Audio capture start failed: %d", ret);
        return false;
    }
    return true;
}

void AudioService::stop_capture() {
    if (!capturing_.load()) return;

    std::lock_guard<std::mutex> lk(capture_mu_);
    if (!capturing_.load()) return;

    if (ops_ && capture_ctx_ && ops_->stop) {
        ops_->stop(capture_ctx_);
    }
    if (ops_ && capture_ctx_ && ops_->unsubscribe_packet) {
        ops_->unsubscribe_packet(capture_ctx_, &AudioService::packet_callback);
    }
    if (ops_ && capture_ctx_ && ops_->unsubscribe_pcm) {
        ops_->unsubscribe_pcm(capture_ctx_, &AudioService::pcm_callback);
    }

    capturing_.store(false);
    HAL_LOG_INFO("Audio capture stopped");
}

bool AudioService::start_playback(const std::string& device, uint32_t sample_rate,
                                   uint32_t channels, const std::string& codec) {
    if (!ops_ || !ops_->init) return false;

    std::lock_guard<std::mutex> lk(playback_mu_);
    if (playing_.load()) return true;

    HalAudioConfig hal_cfg = {};
    hal_cfg.type = HAL_AUDIO_TYPE_ALSA;
    hal_cfg.direction = HAL_AUDIO_IO_PLAYBACK;
    strncpy(hal_cfg.device, device.c_str(), sizeof(hal_cfg.device) - 1);
    hal_cfg.sample_rate = sample_rate;
    hal_cfg.channels = channels;
    hal_cfg.sample_format = HAL_AUDIO_SAMPLE_FMT_S16LE;

    if (codec == "aac") hal_cfg.codec = HAL_AUDIO_CODEC_AAC;
    else if (codec == "g711a") hal_cfg.codec = HAL_AUDIO_CODEC_G711A;
    else if (codec == "g711u") hal_cfg.codec = HAL_AUDIO_CODEC_G711U;
    else hal_cfg.codec = HAL_AUDIO_CODEC_PCM;

    // Full-duplex: the playback context is independent of capture. We do NOT
    // touch capture_ctx_ here, so the listen stream keeps flowing while the
    // speaker plays. Stash the opened format for write_pcm() to stamp buffers.
    pb_sample_rate_ = sample_rate;
    pb_channels_ = channels;

    int ret = ops_->init(&hal_cfg, &playback_ctx_);
    if (ret != 0) {
        HAL_LOG_ERROR("Audio playback init failed: %d", ret);
        return false;
    }

    if (ops_->start && playback_ctx_) {
        ops_->start(playback_ctx_);
    }

    playing_.store(true);
    HAL_LOG_INFO("Audio playback started: device=%s", device.c_str());
    return true;
}

void AudioService::stop_playback() {
    if (!playing_.load()) return;

    std::lock_guard<std::mutex> lk(playback_mu_);
    if (!playing_.load()) return;

    if (ops_ && playback_ctx_ && ops_->stop) {
        ops_->stop(playback_ctx_);
    }
    // Fully release the playback HAL context so a later write_pcm() cannot
    // touch a half-closed ALSA handle. start_playback() re-inits a fresh
    // context next time. capture_ctx_ is untouched (full-duplex: capture keeps
    // running across talk start/stop, so no restore step is needed).
    if (ops_ && playback_ctx_ && ops_->deinit) {
        ops_->deinit(playback_ctx_);
    }
    playback_ctx_ = nullptr;
    playing_.store(false);
    HAL_LOG_INFO("Audio playback stopped");
}

bool AudioService::write_pcm(const uint8_t* data, size_t size) {
    if (!ops_ || !ops_->write_pcm) return false;
    if (size == 0) return false;

    // S16LE = 2 bytes per sample. The HAL validates that buf.samples (frames
    // per channel), sample_rate, channels and format all match the device opened
    // in start_playback(); samples==0 is rejected outright, so it must be derived
    // from the byte size here (see hal_audio_test.cpp for the reference contract).
    constexpr uint32_t kBytesPerSample = 2;

    // Hold playback_mu_ across the playback_ctx_ check, the pb_* format read and
    // the HAL write. Without this, stop_playback() can close/free the ALSA handle
    // (priv->pcm) concurrently — the exact race that produced the snd_pcm_close /
    // write_pcm cores. snd_pcm_writei blocks ~one period (<=21ms), so teardown
    // waits at most one frame; acceptable for push-to-talk. This lock is
    // independent of capture_mu_, so the listen stream is never blocked by talk.
    std::lock_guard<std::mutex> lk(playback_mu_);
    if (!playback_ctx_) return false;              // torn down while waiting
    if (pb_channels_ == 0) return false;

    const uint32_t frame_bytes = pb_channels_ * kBytesPerSample;
    HalAudioBuffer buf = {};
    buf.data = const_cast<uint8_t*>(data);
    buf.size = static_cast<uint32_t>(size);
    buf.sample_rate = pb_sample_rate_;
    buf.channels = pb_channels_;
    buf.format = HAL_AUDIO_SAMPLE_FMT_S16LE;
    buf.samples = static_cast<uint32_t>(size) / frame_bytes;
    if (buf.samples == 0) return false;

    const int ret = ops_->write_pcm(playback_ctx_, &buf);
    if (ret != 0) {
        HAL_LOG_WARNING("audio write_pcm failed: ret=%d size=%u samples=%u %uch@%uHz",
                        ret, buf.size, buf.samples, buf.channels, buf.sample_rate);
    }
    return ret == 0;
}

bool AudioService::set_volume(float volume) {
    // Listen-path volume: applies to the capture context (cfg_ is capture config).
    std::lock_guard<std::mutex> lk(capture_mu_);
    if (!ops_ || !capture_ctx_ || !ops_->dynamic_change_config) return false;

    HalAudioConfig cfg = {};
    cfg.volume = volume;
    if (ops_->dynamic_change_config(capture_ctx_, &cfg) != 0) return false;
    cfg_.volume = volume;
    return true;
}

bool AudioService::set_mute(bool mute) {
    // Listen-path mute: gates the capture (mic) output. Full-duplex note: this
    // is NOT auto-toggled on talk anymore — the frontend no longer mutes on talk.
    std::lock_guard<std::mutex> lk(capture_mu_);
    if (!ops_ || !capture_ctx_ || !ops_->dynamic_change_config) return false;

    HalAudioConfig cfg = {};
    cfg.mute = mute;
    if (ops_->dynamic_change_config(capture_ctx_, &cfg) != 0) return false;
    cfg_.mute = mute;
    return true;
}

bool AudioService::get_status(HalAudioIoDirection, AudioStatus& status) {
    std::lock_guard<std::mutex> lk(capture_mu_);
    status.capturing = capturing_.load();
    status.playing = playing_.load();
    status.device = cfg_.capture_device;
    status.sample_rate = cfg_.sample_rate;
    status.channels = cfg_.channels;
    status.codec = cfg_.codec;
    status.volume = cfg_.volume;
    status.mute = cfg_.mute;
    return true;
}

std::vector<AudioService::DeviceInfo> AudioService::list_capture_devices() {
    std::vector<DeviceInfo> result;
    if (!ops_ || !ops_->list_devices) return result;

    HalAudioDeviceInfo devices[HAL_AUDIO_MAX_LIST_DEVICES] = {};
    uint32_t count = 0;
    ops_->list_devices(devices, HAL_AUDIO_MAX_LIST_DEVICES, &count);
    for (uint32_t i = 0; i < count; ++i) {
        result.push_back({devices[i].name, devices[i].description});
    }
    return result;
}

std::vector<AudioService::DeviceInfo> AudioService::list_playback_devices() {
    std::vector<DeviceInfo> result;
    if (!ops_ || !ops_->list_playback_devices) return result;

    HalAudioDeviceInfo devices[HAL_AUDIO_MAX_LIST_DEVICES] = {};
    uint32_t count = 0;
    ops_->list_playback_devices(devices, HAL_AUDIO_MAX_LIST_DEVICES, &count);
    for (uint32_t i = 0; i < count; ++i) {
        result.push_back({devices[i].name, devices[i].description});
    }
    return result;
}

void AudioService::packet_callback(void* audio_ctx, HalPacketBuffer* packet, void* user_data) {
    auto* self = static_cast<AudioService*>(user_data);
    if (self && packet) {
        self->on_encoded_packet(packet);
        // HAL owns the callback buffer. EncodedPublisher copies the payload
        // synchronously, so return it immediately after forwarding.
        if (self->ops_ && self->ops_->release_packet) {
            self->ops_->release_packet(audio_ctx, packet);
        }
    }
}

void AudioService::pcm_callback(void* audio_ctx, HalAudioBuffer* frame, void* user_data) {
    auto* self = static_cast<AudioService*>(user_data);
    if (self && frame) {
        self->on_pcm_frame(frame);
        // HAL allocates a private heap buffer for every PCM callback.
        // on_pcm_frame() copies it into EncodedPublisher before returning.
        if (self->ops_ && self->ops_->release_frame) {
            self->ops_->release_frame(audio_ctx, frame);
        }
    }
}

void AudioService::on_encoded_packet(const HalPacketBuffer* packet) {
    if (encoded_pub_) {
        encoded_pub_->on_packet("audio_capture", packet);
    }
}

void AudioService::on_pcm_frame(const HalAudioBuffer* frame) {
    if (!frame || !frame->data || frame->size == 0) return;
    // Wrap raw PCM as a HalPacketBuffer and forward to EncodedPublisher
    HalPacketBuffer pkt = {};
    pkt.data = const_cast<uint8_t*>(frame->data);
    pkt.size = frame->size;
    pkt.timestamp_ns = frame->timestamp_ns;
    pkt.type = HAL_PACKET_TYPE_PCM;
    if (encoded_pub_) {
        encoded_pub_->on_packet("audio_capture", &pkt);
    }
}
