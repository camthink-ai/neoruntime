/**
 * @file ai_overlay_subscriber.cpp
 * @brief AI Overlay Subscriber — subscribes to Event Bus for inference results
 *        and draws AI overlays on video frames.
 */

#include "../include/ai_overlay_subscriber.h"

#include <cstring>
#include <cstdlib>
#include <sstream>
#include <cmath>

extern "C" {
    #include "hal_log.h"
}

#ifdef HAS_GRPC
#include <grpcpp/grpcpp.h>
#include <grpcpp/create_channel_posix.h>
#include "event.grpc.pb.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

AiOverlaySubscriber::AiOverlaySubscriber(const AiOverlayConfig& config)
    : config_(config) {
    memset(&default_draw_cfg_, 0, sizeof(default_draw_cfg_));
    hal_draw_config_init_default(&default_draw_cfg_);
}

AiOverlaySubscriber::~AiOverlaySubscriber() {
    stop();
}

bool AiOverlaySubscriber::start() {
    if (!config_.enabled) return true;
#ifndef HAS_GRPC
    HAL_LOG_WARNING("AiOverlaySubscriber: built without gRPC, Event Bus subscription unavailable");
    return true;
#endif
    if (running_.load()) return true;

    running_.store(true);
    subscriber_thread_ = std::thread(&AiOverlaySubscriber::subscriber_loop, this);

    HAL_LOG_INFO("AiOverlaySubscriber: started, endpoint=%s, prefix=%s",
                 config_.event_bus_endpoint.c_str(),
                 config_.topic_prefix.c_str());
    return true;
}

void AiOverlaySubscriber::stop() {
    if (!running_.load()) return;
    running_.store(false);

    {
        std::lock_guard<std::mutex> lock(ctx_mu_);
        if (active_ctx_) {
#ifdef HAS_GRPC
            static_cast<grpc::ClientContext*>(active_ctx_)->TryCancel();
#endif
            active_ctx_ = nullptr;
        }
    }

    if (subscriber_thread_.joinable()) {
        subscriber_thread_.join();
    }
    HAL_LOG_INFO("AiOverlaySubscriber: stopped");
}

void AiOverlaySubscriber::update_config(bool draw_labels, bool draw_confidence, uint32_t box_thickness) {
    {
        std::lock_guard<std::mutex> lock(config_mu_);
        config_.draw_labels = draw_labels;
        config_.draw_confidence = draw_confidence;
        config_.box_thickness = box_thickness;
    }

    HAL_LOG_INFO("AiOverlaySubscriber: config updated (labels=%s, confidence=%s, thickness=%u)",
                 draw_labels ? "on" : "off",
                 draw_confidence ? "on" : "off",
                 box_thickness);
}

static constexpr auto RESULT_TTL = std::chrono::milliseconds(500);

void AiOverlaySubscriber::apply_overlay(const std::string& stream_name,
                                        HalFrameBuffer* frame) {
    if (!config_.enabled || !frame) return;

    // Snapshot mutable config fields under lock
    bool draw_labels, draw_confidence;
    uint32_t box_thickness;
    {
        std::lock_guard<std::mutex> lock(config_mu_);
        draw_labels = config_.draw_labels;
        draw_confidence = config_.draw_confidence;
        box_thickness = config_.box_thickness;
    }

    std::lock_guard<std::mutex> lock(results_mu_);

    auto it = results_.find(stream_name);
    if (it == results_.end() || !it->second.valid) {
        for (auto& [infer_stream, display_stream] : config_.stream_map) {
            if (display_stream == stream_name) {
                it = results_.find(infer_stream);
                if (it != results_.end() && it->second.valid) break;
                it = results_.end();
            }
        }
    }
    if (it == results_.end() || !it->second.valid) return;

    StreamResult& sr = it->second;

    auto now = std::chrono::steady_clock::now();
    auto age = std::chrono::duration_cast<std::chrono::milliseconds>(now - sr.last_update_time);
    if (age > RESULT_TTL) {
        sr.valid = false;
        return;
    }

    if (!config_.draw_ops) {
        static bool warned = false;
        if (!warned) {
            HAL_LOG_WARNING("AiOverlaySubscriber: draw_ops is NULL — HAL draw library not loaded");
            warned = true;
        }
        return;
    }

    if (config_.draw_ops->draw_result) {
        // Update draw_cfg with snapshotted values
        sr.draw_cfg.draw_detection_labels = draw_labels;
        sr.draw_cfg.draw_detection_confidence = draw_confidence;
        sr.draw_cfg.default_box_thickness = static_cast<int32_t>(box_thickness);
        config_.draw_ops->draw_result(&sr.result, frame, &sr.draw_cfg);
    } else {
        draw_with_primitives(sr.result, frame, draw_labels, draw_confidence, box_thickness);
    }
}

void AiOverlaySubscriber::draw_with_primitives(const HalPostprocessResult& result,
                                                HalFrameBuffer* frame,
                                                bool draw_labels,
                                                bool draw_confidence,
                                                uint32_t box_thickness) {
    const auto* ops = config_.draw_ops;
    if (!ops || !frame) return;

    // ── Detection: bounding boxes + labels ──────────────────────────────────
    if (result.type == HAL_POST_TYPE_DETECTION) {
        auto& det = result.result.detection;
        for (uint32_t i = 0; i < det.num_detections; i++) {
            const auto& d = det.detections[i];

            if (ops->draw_rect) {
                HalDrawRect rect{};
                rect.x = d.bbox.x;
                rect.y = d.bbox.y;
                rect.width = d.bbox.w;
                rect.height = d.bbox.h;
                rect.color = {0, 255, 0, 255};
                rect.thickness = box_thickness;
                ops->draw_rect(frame, &rect);
            }

            if ((draw_labels || draw_confidence) && ops->draw_text) {
                char text_buf[HAL_MAX_TEXT_LEN] = {};
                if (draw_labels && draw_confidence) {
                    snprintf(text_buf, sizeof(text_buf), "%s %.0f%%",
                             d.label, d.confidence * 100.0f);
                } else if (draw_labels) {
                    snprintf(text_buf, sizeof(text_buf), "%s", d.label);
                } else {
                    snprintf(text_buf, sizeof(text_buf), "%.0f%%", d.confidence * 100.0f);
                }

                HalDrawText txt{};
                txt.x = d.bbox.x;
                txt.y = d.bbox.y;
                strncpy(txt.text, text_buf, sizeof(txt.text) - 1);
                txt.color = {255, 255, 255, 255};
                txt.font_scale = 0.6f;
                txt.thickness = 1;
                ops->draw_text(frame, &txt);
            }
        }
        return;
    }

    // ── Classification: top-3 labels drawn at top-left corner ───────────────
    if (result.type == HAL_POST_TYPE_CLASSIFICATION && ops->draw_text) {
        auto& cls = result.result.classification;
        uint32_t max_draw = std::min(cls.num_classes, (uint32_t)3);
        for (uint32_t i = 0; i < max_draw; i++) {
            const auto& c = cls.classes[i];
            if (c.confidence < 0.05f) continue;  // skip near-zero classes

            char text_buf[HAL_MAX_TEXT_LEN] = {};
            if (draw_labels && draw_confidence) {
                snprintf(text_buf, sizeof(text_buf), "%s %.0f%%",
                         c.label, c.confidence * 100.0f);
            } else if (draw_labels) {
                snprintf(text_buf, sizeof(text_buf), "%s", c.label);
            } else {
                snprintf(text_buf, sizeof(text_buf), "%.0f%%", c.confidence * 100.0f);
            }

            HalDrawText txt{};
            // Stack labels vertically: 0.04 per line in normalised coords
            txt.x = 0.01f;
            txt.y = 0.04f + i * 0.06f;
            strncpy(txt.text, text_buf, sizeof(txt.text) - 1);
            txt.color = {255, 255, 255, 255};  // white
            txt.font_scale = 0.7f;
            txt.thickness = 2;
            ops->draw_text(frame, &txt);
        }
        return;
    }

    // ── Keypoint / Pose: skeleton dots + links ───────────────────────────────
    if (result.type == HAL_POST_TYPE_KEYPOINT) {
        auto& kp = result.result.keypoint;

        for (uint32_t oi = 0; oi < kp.num_objects; oi++) {
            const auto& obj = kp.objects[oi];

            // Draw skeleton links first (under dots)
            if (ops->draw_line) {
                for (uint32_t li = 0; li < kp.num_links; li++) {
                    const auto& lnk = kp.links[li];
                    int32_t a = lnk.from_idx;
                    int32_t b = lnk.to_idx;
                    if (a < 0 || b < 0
                        || (uint32_t)a >= obj.num_keypoints
                        || (uint32_t)b >= obj.num_keypoints) continue;

                    const auto& pa = obj.keypoints[a];
                    const auto& pb = obj.keypoints[b];
                    if (pa.x < 0.0f || pa.y < 0.0f ||  // invisible keypoint
                        pb.x < 0.0f || pb.y < 0.0f) continue;

                    HalDrawLine line{};
                    line.x1 = pa.x;  line.y1 = pa.y;
                    line.x2 = pb.x;  line.y2 = pb.y;
                    // Use link color if defined, else default cyan
                    line.color = (lnk.color.r || lnk.color.g || lnk.color.b)
                        ? lnk.color : HalColor{0, 255, 255, 255};
                    line.thickness = (lnk.thickness > 0.0f) ? (uint32_t)lnk.thickness : 2;
                    ops->draw_line(frame, &line);
                }
            }

            // Draw keypoint dots
            if (ops->draw_circle) {
                for (uint32_t ki = 0; ki < obj.num_keypoints; ki++) {
                    const auto& pt = obj.keypoints[ki];
                    if (pt.x < 0.0f || pt.y < 0.0f) continue;  // invisible

                    HalDrawCircle circle{};
                    circle.x = pt.x;
                    circle.y = pt.y;
                    circle.radius = 3;
                    circle.color = {0, 255, 0, 255};  // green dots
                    circle.thickness = -1;             // -1 = filled
                    ops->draw_circle(frame, &circle);
                }
            }
        }
        return;
    }
}

// ─── gRPC channel creation ──────────────────────────────────────────────────

#ifdef HAS_GRPC
static std::string extract_unix_path(const std::string& ep) {
    if (ep.rfind("unix:///", 0) == 0) return ep.substr(7);
    if (ep.rfind("unix:", 0) == 0)    return ep.substr(5);
    if (!ep.empty() && ep[0] == '/')  return ep;
    return {};
}

static std::shared_ptr<grpc::Channel> connect_channel(const std::string& endpoint) {
    std::string upath = extract_unix_path(endpoint);
    if (!upath.empty()) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return nullptr;
        struct sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, upath.c_str(), sizeof(sa.sun_path) - 1);
        if (::connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
            close(fd);
            return nullptr;
        }
        return grpc::CreateInsecureChannelFromFd("event-bus", fd);
    }

    std::string addr = endpoint;
    if (addr.rfind("ipv4:", 0) == 0) addr = addr.substr(5);
    auto colon = addr.rfind(':');
    if (colon == std::string::npos) return nullptr;
    std::string host = addr.substr(0, colon);
    int port = std::atoi(addr.substr(colon + 1).c_str());
    if (host.empty() || port <= 0) return nullptr;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        close(fd);
        return nullptr;
    }
    if (::connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        close(fd);
        return nullptr;
    }
    return grpc::CreateInsecureChannelFromFd("event-bus", fd);
}
#endif

// ─── JSON helpers ───────────────────────────────────────────────────────────

static bool json_find_number(const std::string& json, const std::string& key, double& out) {
    std::string pattern = "\"" + key + "\":";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) return false;
    pos += pattern.size();
    while (pos < json.size() && json[pos] == ' ') pos++;
    char* end = nullptr;
    out = strtod(json.c_str() + pos, &end);
    return end != json.c_str() + pos;
}

static bool json_find_string(const std::string& json, const std::string& key, std::string& out) {
    std::string pattern = "\"" + key + "\":\"";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) return false;
    pos += pattern.size();
    auto end = json.find('"', pos);
    if (end == std::string::npos) return false;
    out = json.substr(pos, end - pos);
    return true;
}

// ─── Background subscriber thread ───────────────────────────────────────────

void AiOverlaySubscriber::subscriber_loop() {
#ifndef HAS_GRPC
    return;
#endif
#ifdef HAS_GRPC
    while (running_.load()) {
        auto channel = connect_channel(config_.event_bus_endpoint);
        if (!channel) {
            HAL_LOG_WARNING("AiOverlaySubscriber: connect to %s failed, retrying...",
                            config_.event_bus_endpoint.c_str());
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        auto stub = aipc::event::EventBus::NewStub(channel);
        if (!stub) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        aipc::event::SubscribeRequest sub_req;
        sub_req.set_topic(config_.topic_prefix + "**");
        sub_req.set_subscriber_id("camera-daemon-overlay");
        sub_req.set_queue_size(8);
        sub_req.set_drop_old(true);

        auto ctx = std::make_unique<grpc::ClientContext>();
        {
            std::lock_guard<std::mutex> lock(ctx_mu_);
            active_ctx_ = ctx.get();
        }

        auto reader = stub->Subscribe(ctx.get(), sub_req);
        if (!reader) {
            {
                std::lock_guard<std::mutex> lock(ctx_mu_);
                active_ctx_ = nullptr;
            }
            std::this_thread::sleep_for(std::chrono::seconds(3));
            continue;
        }

        HAL_LOG_INFO("AiOverlaySubscriber: subscribed to %s**",
                     config_.topic_prefix.c_str());

        aipc::event::Event event;
        bool first_event = true;
        while (running_.load() && reader->Read(&event)) {
            if (first_event) {
                HAL_LOG_INFO("AiOverlaySubscriber: first event received on topic=%s",
                             event.topic().c_str());
                first_event = false;
            }

            std::string stream_id;
            if (event.metadata().count("stream_id")) {
                stream_id = event.metadata().at("stream_id");
            }
            if (stream_id.empty()) continue;

            std::string payload(event.payload().begin(), event.payload().end());

            HalPostprocessResult result{};
            if (!parse_json_result(payload, stream_id, &result)) continue;

            {
                std::lock_guard<std::mutex> lock(results_mu_);
                auto& sr = results_[stream_id];
                sr.result = result;
                sr.last_update_time = std::chrono::steady_clock::now();
                sr.valid = true;
            }
        }

        HAL_LOG_WARNING("AiOverlaySubscriber: Read() loop exited (running=%d)",
                         running_.load() ? 1 : 0);

        {
            std::lock_guard<std::mutex> lock(ctx_mu_);
            active_ctx_ = nullptr;
        }

        grpc::Status status = reader->Finish();
        if (!status.ok() && running_.load()) {
            HAL_LOG_WARNING("AiOverlaySubscriber: stream ended: %s",
                           status.error_message().c_str());
        }

        if (running_.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
        }
    }
#endif
}

// ─── JSON parse into result types ───────────────────────────────────────────

bool AiOverlaySubscriber::parse_json_result(const std::string& payload,
                                            const std::string& stream_id,
                                            HalPostprocessResult* out) {
    memset(out, 0, sizeof(*out));
    out->type = HAL_POST_TYPE_DETECTION;

    // ── Classification: { "classifications": [ {class_id, label, confidence}, ... ] } ──
    {
        auto cls_pos = payload.find("\"classifications\":[");
        if (cls_pos != std::string::npos) {
            out->type = HAL_POST_TYPE_CLASSIFICATION;
            auto& cls = out->result.classification;
            cls.num_classes = 0;
            cls.top1_class_id = -1;
            cls_pos = payload.find('[', cls_pos);

            for (uint32_t i = 0; i < HAL_MAX_CLASSES; i++) {
                auto obj_start = payload.find('{', cls_pos);
                auto obj_end   = payload.find('}', obj_start);
                if (obj_start == std::string::npos || obj_end == std::string::npos) break;

                std::string obj = payload.substr(obj_start, obj_end - obj_start + 1);

                double class_id = 0, conf = 0;
                std::string label;
                json_find_number(obj, "class_id", class_id);
                json_find_number(obj, "confidence", conf);
                json_find_string(obj, "label", label);

                cls.classes[i].class_id    = static_cast<int32_t>(class_id);
                cls.classes[i].confidence  = static_cast<float>(conf);
                strncpy(cls.classes[i].label, label.c_str(), sizeof(cls.classes[i].label) - 1);

                if (i == 0) cls.top1_class_id = static_cast<int32_t>(class_id);
                cls.num_classes++;
                cls_pos = obj_end + 1;
            }
            return true;
        }
    }

    // ── Keypoint: { "landmarks": [ { "type": "...", "points": [...] } ] } ──
    {
        auto lm_pos = payload.find("\"landmarks\":[");
        if (lm_pos != std::string::npos) {
            out->type = HAL_POST_TYPE_KEYPOINT;
            auto& kp = out->result.keypoint;
            kp.num_objects = 0;
            kp.num_links   = 0;
            lm_pos = payload.find('[', lm_pos);

            // Parse each landmark set (one per detected face/body)
            for (uint32_t oi = 0; oi < HAL_MAX_DETECTIONS; oi++) {
                auto obj_start = payload.find('{', lm_pos);
                if (obj_start == std::string::npos) break;

                // Find the matching close brace for this object (simple depth=1 scan)
                size_t depth = 0;
                size_t obj_end = obj_start;
                for (size_t p = obj_start; p < payload.size(); p++) {
                    if (payload[p] == '{') depth++;
                    else if (payload[p] == '}') { depth--; if (depth == 0) { obj_end = p; break; } }
                }
                if (obj_end == obj_start) break;

                std::string obj = payload.substr(obj_start, obj_end - obj_start + 1);

                // Parse "points": [ {x, y, confidence}, ... ]
                auto pts_pos = obj.find("\"points\":[");
                if (pts_pos == std::string::npos) { lm_pos = obj_end + 1; continue; }
                pts_pos = obj.find('[', pts_pos);

                auto& kobj = kp.objects[oi];
                kobj.num_keypoints = 0;
                kobj.confidence    = 1.0f;
                kobj.class_id      = 0;
                kobj.track_id      = -1;

                for (uint32_t ki = 0; ki < HAL_MAX_KEYPOINTS; ki++) {
                    auto pt_start = obj.find('{', pts_pos);
                    auto pt_end   = obj.find('}', pt_start);
                    if (pt_start == std::string::npos || pt_end == std::string::npos) break;
                    // Stop if we left the points array (hit parent '}')
                    if (pt_start > obj_end) break;

                    std::string pt = obj.substr(pt_start, pt_end - pt_start + 1);
                    double x = -1, y = -1, conf = 1;
                    json_find_number(pt, "x", x);
                    json_find_number(pt, "y", y);
                    json_find_number(pt, "confidence", conf);

                    // Invisible keypoints are encoded as negative coords by model-showcase
                    kobj.keypoints[ki].x = static_cast<float>(x);
                    kobj.keypoints[ki].y = static_cast<float>(y);

                    kobj.num_keypoints++;
                    pts_pos = pt_end + 1;
                }

                if (kobj.num_keypoints > 0) kp.num_objects++;
                lm_pos = obj_end + 1;
            }
            return true;
        }
    }

    // ── OCR: { "ocr_lines": [ {text, confidence, bbox}, ... ] } ────────────
    {
        auto ocr_pos = payload.find("\"ocr_lines\":[");
        if (ocr_pos != std::string::npos) {
            out->type = HAL_POST_TYPE_OCR_RECOGNITION;
            auto& ocr = out->result.ocr;
            ocr.num_lines = 0;
            ocr_pos = payload.find('[', ocr_pos);

            for (uint32_t i = 0; i < HAL_MAX_OCR_LINES; i++) {
                auto obj_start = payload.find('{', ocr_pos);
                auto obj_end   = payload.find('}', obj_start);
                if (obj_start == std::string::npos || obj_end == std::string::npos) break;

                std::string obj = payload.substr(obj_start, obj_end - obj_start + 1);

                double conf = 0;
                std::string text;
                json_find_number(obj, "confidence", conf);
                json_find_string(obj, "text", text);

                ocr.lines[i].confidence = static_cast<float>(conf);
                strncpy(ocr.lines[i].text, text.c_str(), sizeof(ocr.lines[i].text) - 1);

                auto bbox_pos = obj.find("\"bbox\":[");
                if (bbox_pos != std::string::npos) {
                    bbox_pos = obj.find('[', bbox_pos);
                    float vals[4] = {};
                    const char* p = obj.c_str() + bbox_pos + 1;
                    for (int v = 0; v < 4; v++) {
                        char* ep = nullptr;
                        vals[v] = strtof(p, &ep);
                        p = ep;
                        while (*p == ',' || *p == ' ') p++;
                    }
                    ocr.lines[i].bbox.x = vals[0];
                    ocr.lines[i].bbox.y = vals[1];
                    ocr.lines[i].bbox.w = vals[2];
                    ocr.lines[i].bbox.h = vals[3];
                }

                ocr.num_lines++;
                ocr_pos = obj_end + 1;
            }
            return true;
        }
    }

    // ── Detection (default): { "num_detections": N, "detections": [...] } ──
    {
        double num_det = 0;
        json_find_number(payload, "num_detections", num_det);

        auto& det = out->result.detection;
        det.num_detections = static_cast<uint32_t>(num_det);

        auto det_pos = payload.find("\"detections\":[");
        if (det_pos == std::string::npos) return true;  // empty detections payload
        det_pos = payload.find('[', det_pos);

        for (uint32_t i = 0; i < det.num_detections && i < HAL_MAX_DETECTIONS; i++) {
            auto obj_start = payload.find('{', det_pos);
            auto obj_end   = payload.find('}', obj_start);
            if (obj_start == std::string::npos || obj_end == std::string::npos) break;

            std::string obj = payload.substr(obj_start, obj_end - obj_start + 1);

            double class_id = 0, conf = 0;
            std::string label;
            json_find_number(obj, "class_id", class_id);
            json_find_number(obj, "confidence", conf);
            json_find_string(obj, "label", label);

            det.detections[i].class_id   = static_cast<int32_t>(class_id);
            det.detections[i].confidence = static_cast<float>(conf);
            strncpy(det.detections[i].label, label.c_str(), sizeof(det.detections[i].label) - 1);

            auto bbox_pos = obj.find("\"bbox\":[");
            if (bbox_pos != std::string::npos) {
                bbox_pos = obj.find('[', bbox_pos);
                float vals[4] = {};
                const char* p = obj.c_str() + bbox_pos + 1;
                for (int v = 0; v < 4; v++) {
                    char* ep = nullptr;
                    vals[v] = strtof(p, &ep);
                    p = ep;
                    while (*p == ',' || *p == ' ') p++;
                }
                det.detections[i].bbox.x = vals[0];
                det.detections[i].bbox.y = vals[1];
                det.detections[i].bbox.w = vals[2];
                det.detections[i].bbox.h = vals[3];
            }

            det_pos = obj_end + 1;
        }
    }

    return true;
}
