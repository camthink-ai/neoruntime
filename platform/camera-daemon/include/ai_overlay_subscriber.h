/**
 * @file ai_overlay_subscriber.h
 * @brief AI Overlay Subscriber — receives inference results from Event Bus
 *        and draws AI overlays (detection boxes, landmarks, etc.) on video
 *        frames before encoding.
 *
 * Uses HalDrawOps + HalPostprocessResult.
 */

#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>
#include <chrono>

extern "C" {
    #include "hal_postprocess.h"
    #include "hal_draw.h"
    #include "hal_buffer.h"
}

struct AiOverlayConfig {
    bool        enabled = false;
    std::string event_bus_endpoint = "unix:///run/aipc/event-bus.sock";
    std::string topic_prefix       = "inference/";

    bool     draw_detections  = true;
    bool     draw_labels      = true;
    bool     draw_confidence  = true;
    bool     draw_landmarks   = true;
    bool     enable_face_blur = false;
    uint32_t box_thickness    = 2;

    std::unordered_map<std::string, std::string> stream_map;

    const HalDrawOps* draw_ops = nullptr;
};

class AiOverlaySubscriber {
public:
    explicit AiOverlaySubscriber(const AiOverlayConfig& config);
    ~AiOverlaySubscriber();

    AiOverlaySubscriber(const AiOverlaySubscriber&) = delete;
    AiOverlaySubscriber& operator=(const AiOverlaySubscriber&) = delete;

    bool start();
    void stop();

    void apply_overlay(const std::string& stream_name, HalFrameBuffer* frame);

    bool is_running() const { return running_.load(); }

    void update_config(bool draw_labels, bool draw_confidence, uint32_t box_thickness);

private:
    struct StreamResult {
        HalPostprocessResult result{};
        HalDrawConfig        draw_cfg{};
        uint64_t          last_frame_seq = 0;
        std::chrono::steady_clock::time_point last_update_time;
        bool              valid = false;
    };

    void subscriber_loop();

    void draw_with_primitives(const HalPostprocessResult& result, HalFrameBuffer* frame,
                              bool draw_labels, bool draw_confidence, uint32_t box_thickness);
    bool parse_json_result(const std::string& payload, const std::string& stream_id,
                           HalPostprocessResult* out);

    AiOverlayConfig config_;

    // Protects mutable config fields (draw_labels, draw_confidence, box_thickness)
    mutable std::mutex config_mu_;

    HalDrawConfig default_draw_cfg_{};

    std::atomic<bool> running_{false};
    std::thread       subscriber_thread_;

    mutable std::mutex ctx_mu_;
    void*              active_ctx_ = nullptr;

    mutable std::mutex results_mu_;
    std::unordered_map<std::string, StreamResult> results_;
};
