/**
 * @file encoder_manager.cpp
 * @brief Encoder Manager Implementation (v2: void* codec_ctx)
 */

#include "../include/encoder_manager.h"

#include <atomic>

extern "C" {
    #include "hal_log.h"
}

EncoderManager::EncoderManager(HalCodecOps* ops) : ops_(ops) {}

EncoderManager::~EncoderManager() {
    destroy_all();
}

bool EncoderManager::subscribe_output(void* ctx, const std::string& stream_name) {
    if (!ops_->subscribe) return true;
    int ret = ops_->subscribe(ctx, codec_output_callback, this);
    if (ret < 0) {
        HAL_LOG_ERROR("EncoderManager: subscribe failed for %s: %d",
                     stream_name.c_str(), ret);
        return false;
    }
    return true;
}

bool EncoderManager::create(const std::string& stream_name,
                            const HalCodecConfig& config) {
    if (!ops_) return false;

    std::lock_guard<std::mutex> lock(mu_);
    if (encoders_.count(stream_name)) {
        HAL_LOG_WARNING("EncoderManager: Encoder already exists for %s",
                       stream_name.c_str());
        return true;
    }

    void* ctx = nullptr;
    int ret = ops_->init(&config, &ctx);
    if (ret < 0 || !ctx) {
        HAL_LOG_ERROR("EncoderManager: init failed for %s: %d",
                     stream_name.c_str(), ret);
        return false;
    }

    if (!subscribe_output(ctx, stream_name)) {
        ops_->deinit(ctx);
        return false;
    }

    EncoderSlot slot;
    slot.codec_ctx = ctx;
    slot.stream_name = stream_name;
    slot.running = false;
    slot.created_at = std::chrono::steady_clock::now();

    encoders_[stream_name] = slot;
    handle_to_stream_[ctx] = stream_name;

    HAL_LOG_INFO("EncoderManager: Created encoder for %s ctx=%p",
                 stream_name.c_str(), ctx);
    return true;
}

bool EncoderManager::create_from_context(const std::string& stream_name, void* codec_ctx) {
    if (!ops_ || !codec_ctx) return false;

    std::lock_guard<std::mutex> lock(mu_);
    if (encoders_.count(stream_name)) {
        HAL_LOG_WARNING("EncoderManager: Encoder already exists for %s",
                       stream_name.c_str());
        return true;
    }

    if (!subscribe_output(codec_ctx, stream_name)) {
        return false;
    }

    EncoderSlot slot;
    slot.codec_ctx = codec_ctx;
    slot.stream_name = stream_name;
    slot.running = true;   // FROM_MEDIA encoders are already running
    slot.from_media = true;
    slot.created_at = std::chrono::steady_clock::now();

    encoders_[stream_name] = slot;
    handle_to_stream_[codec_ctx] = stream_name;

    HAL_LOG_INFO("EncoderManager: Created FROM_MEDIA encoder for %s ctx=%p",
                 stream_name.c_str(), codec_ctx);
    return true;
}

int EncoderManager::encode_frame(const std::string& stream_name,
                                 HalFrameBuffer* frame) {
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end() || !it->second.running) return -1;
        if (!ops_->input_frame) return -1;
        ctx = it->second.codec_ctx;
    }
    int ret = ops_->input_frame(ctx, frame);
    if (ret != 0) {
        static int err_count = 0;
        if (++err_count <= 5) {
            HAL_LOG_ERROR("EncoderManager: input_frame(%s) failed: %d",
                         stream_name.c_str(), ret);
        }
    }
    return ret;
}

int EncoderManager::set_bitrate(const std::string& stream_name, uint32_t bitrate_bps) {
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) return -1;
        ctx = it->second.codec_ctx;
    }
    if (!ops_->dynamic_change_config) return -1;
    HalCodecConfig cfg{};
    cfg.bitrate = bitrate_bps;
    return ops_->dynamic_change_config(ctx, &cfg);
}

int EncoderManager::set_framerate(const std::string& stream_name, uint32_t fps) {
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) return -1;
        ctx = it->second.codec_ctx;
    }
    if (!ops_->dynamic_change_config) return -1;
    HalCodecConfig cfg{};
    cfg.framerate = fps;
    return ops_->dynamic_change_config(ctx, &cfg);
}

int EncoderManager::set_gop(const std::string& stream_name, uint32_t gop) {
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) return -1;
        ctx = it->second.codec_ctx;
    }
    if (!ops_->dynamic_change_config) return -1;
    // Use intra_pic_rate for the keyframe interval (I-frame distance).
    // gop_size in HalCodecConfig maps to Hantro gop_config.gop_size which
    // controls B-frame hierarchy (valid: 1-8), NOT the I-frame interval.
    // Setting gop_size=30 would break the encoder.
    HalCodecConfig cfg{};
    cfg.intra_pic_rate = gop;
    return ops_->dynamic_change_config(ctx, &cfg);
}

int EncoderManager::force_keyframe(const std::string& stream_name) {
    // v2 FROM_MEDIA encoders don't expose a direct force_keyframe API.
    // The encoder generates IDR frames based on its GOP/interval settings.
    // Log only once to avoid flooding at 30fps when platform-api keeps requesting.
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true)) {
        HAL_LOG_WARNING("EncoderManager: force_keyframe not directly available in v2 for %s "
                        "(encoder generates IDR frames based on GOP config)",
                        stream_name.c_str());
    }
    return 0;
}

int EncoderManager::reconfigure(const std::string& stream_name,
                                const HalCodecConfig& new_config,
                                uint32_t* interrupt_ms) {
    auto start_time = std::chrono::steady_clock::now();

    bool was_running = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) {
            HAL_LOG_ERROR("EncoderManager: reconfigure - encoder not found: %s", stream_name.c_str());
            return -1;
        }
        was_running = it->second.running;
    }

    HAL_LOG_INFO("EncoderManager: Reconfiguring encoder %s (destroy+create, was_running=%d)",
                 stream_name.c_str(), was_running);

    destroy(stream_name);

    if (!create(stream_name, new_config)) {
        HAL_LOG_ERROR("EncoderManager: create failed during reconfigure for %s", stream_name.c_str());
        return -1;
    }

    if (was_running) {
        if (!start(stream_name)) {
            HAL_LOG_ERROR("EncoderManager: start failed during reconfigure for %s", stream_name.c_str());
            return -1;
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    if (interrupt_ms) {
        *interrupt_ms = static_cast<uint32_t>(duration_ms.count());
    }

    HAL_LOG_INFO("EncoderManager: Reconfigured encoder %s via destroy+create (interrupt: %lums)",
                 stream_name.c_str(), interrupt_ms ? *interrupt_ms : 0);
    return 0;
}

void* EncoderManager::get_codec_ctx(const std::string& stream_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_.find(stream_name);
    if (it == encoders_.end()) return nullptr;
    return it->second.codec_ctx;
}

void EncoderManager::codec_output_callback(void* codec_ctx,
                                            HalPacketBuffer* packet,
                                            void* userdata) {
    auto* self = static_cast<EncoderManager*>(userdata);
    self->on_packet(codec_ctx, packet);
}

void EncoderManager::on_packet(void* codec_ctx, const HalPacketBuffer* packet) {
    std::string stream_name;
    bool log_first = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto hit = handle_to_stream_.find(codec_ctx);
        if (hit == handle_to_stream_.end()) return;
        stream_name = hit->second;

        // Stamp frame-health signal and emit the first-packet log once per slot
        // (NOT once per process — the old static-atomic suppressed first-packet
        // logs for every stream after the very first one).
        auto eit = encoders_.find(stream_name);
        if (eit != encoders_.end()) {
            auto& slot = eit->second;
            slot.last_packet_time = std::chrono::steady_clock::now();
            if (!slot.logged_first_packet) {
                slot.logged_first_packet = true;
                log_first = true;
            }
        }
    }

    if (log_first) {
        HAL_LOG_INFO("EncoderManager: First encoded packet for %s (size=%u)",
                    stream_name.c_str(), packet ? packet->size : 0);
    }

    if (output_fn_) {
        output_fn_(stream_name, packet);
    }

    if (ops_->release_packet) {
        ops_->release_packet(codec_ctx, const_cast<HalPacketBuffer*>(packet));
    }
}

// === Shared methods ===

void EncoderManager::destroy(const std::string& stream_name) {
    void* ctx = nullptr;
    bool was_running = false;
    bool is_from_media = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) return;

        ctx = it->second.codec_ctx;
        was_running = it->second.running;
        is_from_media = it->second.from_media;

        handle_to_stream_.erase(ctx);
        encoders_.erase(it);
    }

    // FROM_MEDIA contexts are owned by media pipeline — skip start/stop/deinit
    if (!is_from_media) {
        if (was_running && ops_->stop) {
            ops_->stop(ctx);
        }
        if (ops_->unsubscribe) {
            ops_->unsubscribe(ctx, codec_output_callback);
        }
        if (ops_->deinit) {
            ops_->deinit(ctx);
        }
    } else {
        if (ops_->unsubscribe) {
            ops_->unsubscribe(ctx, codec_output_callback);
        }
    }

    HAL_LOG_INFO("EncoderManager: Destroyed encoder for %s (from_media=%d)",
                 stream_name.c_str(), is_from_media);
}

void EncoderManager::destroy_all() {
    std::vector<std::string> names;
    {
        std::lock_guard<std::mutex> lock(mu_);
        for (auto& [name, _] : encoders_) {
            names.push_back(name);
        }
    }
    for (auto& name : names) {
        destroy(name);
    }
}

void EncoderManager::discard_from_media() {
    std::lock_guard<std::mutex> lock(mu_);
    size_t discarded = 0;
    for (auto it = encoders_.begin(); it != encoders_.end(); ) {
        if (!it->second.from_media) {
            ++it;
            continue;
        }

        handle_to_stream_.erase(it->second.codec_ctx);
        it = encoders_.erase(it);
        ++discarded;
    }
    HAL_LOG_INFO("EncoderManager: Discarded %zu stale FROM_MEDIA context(s) without HAL access",
                 discarded);
}

bool EncoderManager::start(const std::string& stream_name) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_.find(stream_name);
    if (it == encoders_.end()) return false;

    auto& slot = it->second;
    if (slot.running) return true;

    if (ops_->start) {
        int ret = ops_->start(slot.codec_ctx);
        if (ret < 0) {
            HAL_LOG_ERROR("EncoderManager: start failed for %s: %d",
                         stream_name.c_str(), ret);
            return false;
        }
    }

    slot.running = true;
    HAL_LOG_INFO("EncoderManager: Started encoder %s", stream_name.c_str());
    return true;
}

bool EncoderManager::stop(const std::string& stream_name) {
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) return false;

        auto& slot = it->second;
        if (!slot.running) return true;

        ctx = slot.codec_ctx;
        slot.running = false;
    }

    if (ops_->stop) {
        ops_->stop(ctx);
    }
    HAL_LOG_INFO("EncoderManager: Stopped encoder %s", stream_name.c_str());
    return true;
}

void EncoderManager::set_output_callback(PacketOutputFn fn) {
    output_fn_ = std::move(fn);
}

bool EncoderManager::has_encoder(const std::string& stream_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    return encoders_.count(stream_name) > 0;
}

uint64_t EncoderManager::ms_since_last_packet(const std::string& stream_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_.find(stream_name);
    if (it == encoders_.end()) return UINT64_MAX;  // no encoder
    if (it->second.last_packet_time == std::chrono::steady_clock::time_point{}) {
        return UINT64_MAX;                          // never received a packet
    }
    auto delta = std::chrono::steady_clock::now() - it->second.last_packet_time;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
}

bool EncoderManager::seen_first_packet(const std::string& stream_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_.find(stream_name);
    if (it == encoders_.end()) return false;
    return it->second.last_packet_time != std::chrono::steady_clock::time_point{};
}

bool EncoderManager::is_stream_stalled(const std::string& stream_name,
                                       uint64_t threshold_ms,
                                       uint64_t grace_ms) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_.find(stream_name);
    if (it == encoders_.end()) return true;  // missing encoder counts as stalled

    const auto& slot = it->second;
    auto now = std::chrono::steady_clock::now();

    // Startup grace: a freshly-created encoder has not had a chance to emit yet.
    // Suppress stall verdicts during pipeline bring-up (incl. post-switch verify)
    // so we don't false-alarm a healthy but slow-to-first-frame encoder.
    auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - slot.created_at).count();
    if (static_cast<uint64_t>(age_ms) < grace_ms) return false;

    if (slot.last_packet_time == std::chrono::steady_clock::time_point{}) {
        return true;  // grace elapsed and still no first packet
    }
    auto gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - slot.last_packet_time).count();
    return static_cast<uint64_t>(gap_ms) > threshold_ms;
}
