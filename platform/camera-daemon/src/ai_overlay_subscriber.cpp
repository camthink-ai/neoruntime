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
#include <algorithm>
#include <strings.h>

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

void AiOverlaySubscriber::update_config(bool draw_labels, bool draw_confidence,
                                        uint32_t box_thickness, bool enable_face_blur) {
    {
        std::lock_guard<std::mutex> lock(config_mu_);
        config_.draw_labels = draw_labels;
        config_.draw_confidence = draw_confidence;
        config_.box_thickness = box_thickness;
        config_.enable_face_blur = enable_face_blur;
    }

    HAL_LOG_INFO("AiOverlaySubscriber: config updated (labels=%s, confidence=%s, thickness=%u, face_blur=%s)",
                 draw_labels ? "on" : "off",
                 draw_confidence ? "on" : "off",
                 box_thickness,
                 enable_face_blur ? "on" : "off");
}

void AiOverlaySubscriber::update_strict(bool strict_frame_lock, uint32_t strict_wait_cap_ms) {
    {
        std::lock_guard<std::mutex> lock(config_mu_);
        config_.strict_frame_lock = strict_frame_lock;
        config_.strict_wait_cap_ms = strict_wait_cap_ms;
    }
    if (strict_frame_lock) {
        // Release any frame currently gated mid-wait so toggling strict
        // never parks the streaming thread for a full cap.
        std::lock_guard<std::mutex> lock(results_mu_);
        result_cv_.notify_all();
    }
    HAL_LOG_INFO("AiOverlaySubscriber: strict mode %s (wait_cap=%u ms)",
                 strict_frame_lock ? "ON" : "OFF", strict_wait_cap_ms);
}

bool AiOverlaySubscriber::strict_gate_active(const std::string& display_stream) {
    // Identity predicate, not !strict_cross_fed: the auto-generated map
    // (every stream → primary encoder) makes the primary stream cross-fed
    // while still carrying the identity entry that strict lock needs.
    std::lock_guard<std::mutex> lock(config_mu_);
    if (!config_.strict_frame_lock) return false;
    auto it = config_.stream_map.find(display_stream);
    return it != config_.stream_map.end() && it->second == display_stream;
}

bool AiOverlaySubscriber::is_bake_target(const std::string& stream) {
    // The bake set is the stream_map ∪ bindings VALUE set: identity
    // entries (D→D) and cross-fed entries (I→D) both bake their display
    // stream. A key mapped to a foreign display is an inference SOURCE —
    // the clean feed the split exists to provide — and must never carry
    // baked pixels or a set baked flag.
    std::lock_guard<std::mutex> lock(config_mu_);
    for (const auto& [infer_stream, display_stream] : config_.stream_map) {
        if (display_stream == stream) return true;
    }
    for (const auto& [infer_stream, display_stream] : config_.bindings) {
        if (display_stream == stream) return true;
    }
    return false;
}

// Fallback TTL when nothing resolves a stream's validity window (see
// resolve_result_ttl_ms) — roughly two frame periods at 30fps.
static constexpr uint32_t RESULT_TTL_FALLBACK_MS = 500;

// Per-event wire override ceiling: a "result_ttl_ms" metadata value above
// this is treated as malformed (unset). Results are refreshed per frame;
// ten minutes of validity from a single event is far past any real use.
static constexpr uint32_t RESULT_TTL_MAX_MS = 600000;

// Face-blur label match: exact "face" in any case ("FACE", "Face").
// Substrings like "facial" or "face_mask" do not match — only a detector that
// labels the region class itself as face gets blurred.
static bool is_face_label(const char* label) {
    return label != nullptr && strcasecmp(label, "face") == 0;
}

size_t collect_face_mosaic_rects(const HalPostprocessResult& result,
                                 uint32_t frame_width, uint32_t frame_height,
                                 uint32_t block_size,
                                 HalDrawMosaic* out, size_t cap) {
    if (out == nullptr || cap == 0) return 0;
    if (result.type != HAL_POST_TYPE_DETECTION) return 0;
    if (frame_width == 0 || frame_height == 0) return 0;

    // num_detections comes from the event payload; parse_json_result clamps
    // writes but not the count, so bound reads to the array too.
    const auto& det = result.result.detection;
    uint32_t n = std::min(det.num_detections, (uint32_t)HAL_MAX_DETECTIONS);

    size_t count = 0;
    for (uint32_t i = 0; i < n && count < cap; i++) {
        const auto& d = det.detections[i];
        if (!is_face_label(d.label)) continue;

        // Reuse hal_bbox_to_rect for the normalized→pixel mapping: sanitize,
        // order, scale, and clip to frame bounds in one place.
        HalDrawRect rect{};
        hal_bbox_to_rect(&d.bbox, frame_width, frame_height, &rect);
        if (rect.width <= 0 || rect.height <= 0) continue;  // fully outside

        HalDrawMosaic mosaic{};
        mosaic.x = rect.x;
        mosaic.y = rect.y;
        mosaic.width = rect.width;
        mosaic.height = rect.height;
        mosaic.block_size = static_cast<int32_t>(block_size);  // 0 = blur
        out[count++] = mosaic;
    }
    return count;
}

// Draws face mosaics for a (possibly fresh) result using the snapshotted
// config. Shared by the unified draw_result path and the primitives fallback.
static void draw_face_mosaics(const HalPostprocessResult& result, HalFrameBuffer* frame,
                              const HalDrawOps* ops,
                              uint32_t block_size) {
    HalDrawMosaic mosaics[HAL_MAX_DETECTIONS];
    size_t n = collect_face_mosaic_rects(result, frame->width, frame->height,
                                         block_size, mosaics, HAL_MAX_DETECTIONS);
    for (size_t i = 0; i < n; i++) {
        ops->draw_mosaic(frame, &mosaics[i]);
    }
}

// Renders the polygon sidecar (zone overlays) after the primary result pass.
// Points arrive normalized [0,1]; scale against the live frame here — where
// the frame size is finally known. Labels ride at the first vertex in PIXEL
// space (draw_text takes pixel coords, unlike the normalized floats the
// classification fallback above passes). thickness -1 ("filled" on hardware
// ops) renders as a 1px outline on the CPU path — clamped, not dropped.
static void draw_result_polygons(const std::vector<OverlayPolygon>& polygons,
                                 HalFrameBuffer* frame,
                                 const HalDrawOps* ops,
                                 bool draw_labels) {
    if (ops == nullptr || frame == nullptr) return;

    int32_t xs[HAL_MAX_POLYGON_POINTS];
    int32_t ys[HAL_MAX_POLYGON_POINTS];

    for (const auto& poly : polygons) {
        size_t n = std::min(poly.points.size(), (size_t)HAL_MAX_POLYGON_POINTS);
        if (n < 2) continue;  // two distinct vertices minimum

        for (size_t i = 0; i < n; i++) {
            // sanitize clamps NaN / out-of-range to [0,1]; set_nv12_pixel
            // clips the final pixels, so edge coords stay safe.
            float nx = hal_bbox_sanitize_norm(poly.points[i].first);
            float ny = hal_bbox_sanitize_norm(poly.points[i].second);
            xs[i] = (int32_t)(nx * (float)frame->width);
            ys[i] = (int32_t)(ny * (float)frame->height);
        }

        HalColor color{0, 255, 255, 255};  // default cyan, opaque
        if (poly.has_color) {
            color.r = (uint8_t)((poly.color >> 16) & 0xFF);  // ARGB wire order
            color.g = (uint8_t)((poly.color >> 8) & 0xFF);
            color.b = (uint8_t)(poly.color & 0xFF);
            color.a = (uint8_t)((poly.color >> 24) & 0xFF);
        }
        int32_t thickness = poly.has_thickness ? poly.thickness : 2;
        if (thickness < 1) thickness = 1;

        if (poly.closed && ops->draw_polygon) {
            HalDrawPolygon dp{};
            dp.num_points = (uint32_t)n;
            memcpy(dp.points_x, xs, n * sizeof(xs[0]));
            memcpy(dp.points_y, ys, n * sizeof(ys[0]));
            dp.color = color;
            dp.thickness = thickness;
            ops->draw_polygon(frame, &dp);  // CPU impl closes the loop itself
        } else if (ops->draw_line) {
            // Open polygon (or no draw_polygon op): segments via draw_line —
            // cpu_draw_polygon always closes, so closed=false must not use it.
            size_t segs = poly.closed ? n : n - 1;
            for (size_t i = 0; i < segs; i++) {
                size_t j = (i + 1) % n;
                HalDrawLine line{};
                line.x1 = xs[i]; line.y1 = ys[i];
                line.x2 = xs[j]; line.y2 = ys[j];
                line.color = color;
                line.thickness = (uint32_t)thickness;
                ops->draw_line(frame, &line);
            }
        }

        if (draw_labels && !poly.label.empty() && ops->draw_text) {
            HalDrawText txt{};
            txt.x = xs[0];
            txt.y = ys[0];
            strncpy(txt.text, poly.label.c_str(), sizeof(txt.text) - 1);
            txt.color = {255, 255, 255, 255};
            txt.font_scale = 0.6f;
            txt.thickness = 1;
            ops->draw_text(frame, &txt);
        }
    }
}

// Stream generation seed (file-local; the epoch map itself is private to
// the subscriber). Seeded lazily from CLOCK_MONOTONIC us — never 0, so an
// app event with no epoch metadata is distinguishable from a stale one.
// Must run under results_mu_.
static uint64_t lazy_stream_epoch(
    std::unordered_map<std::string, uint64_t>& epochs,
    const std::string& stream) {
    auto it = epochs.find(stream);
    if (it != epochs.end()) return it->second;
    uint64_t seed = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (seed == 0) seed = 1;  // 0 reserved: "unset" on the app-event side
    epochs.emplace(stream, seed);
    return seed;
}

// Every results_ key routed onto this display stream: itself plus every
// infer stream mapped (stream_map) or bound (bindings) onto it, deduped —
// an identity entry D→D must not count its layers twice. Shared by the
// bake site's layer collection and snapshot_stream_stats' aggregation.
// stream_map/bindings are immutable after construction, so reading them
// under results_mu_ (the caller) is safe.
static std::vector<std::string> routed_keys(const std::string& stream_name,
                                            const AiOverlayConfig& cfg) {
    std::vector<std::string> keys{stream_name};
    auto add_key = [&](const std::string& k) {
        if (k != stream_name &&
            std::find(keys.begin(), keys.end(), k) == keys.end()) {
            keys.push_back(k);
        }
    };
    for (const auto& [infer_stream, display_stream] : cfg.stream_map) {
        if (display_stream == stream_name) add_key(infer_stream);
    }
    for (const auto& [infer_stream, display_stream] : cfg.bindings) {
        if (display_stream == stream_name) add_key(infer_stream);
    }
    return keys;
}

// draw_ops stays null when the HAL draw library is not loaded; warn once
// per process instead of per frame — bake skips are counted before this
// check, so the drop surface stays observable.
static void warn_no_draw_ops() {
    static bool warned = false;
    if (!warned) {
        HAL_LOG_WARNING("AiOverlaySubscriber: draw_ops is NULL — HAL draw library not loaded");
        warned = true;
    }
}

void AiOverlaySubscriber::apply_overlay(const std::string& stream_name,
                                        HalFrameBuffer* frame,
                                        uint64_t current_seq) {
    // Anchor the ingest-side late-command judgement BEFORE any gate: the
    // bake site saw this frame even when the overlay is disabled or the
    // frame ships clean, and app commands must be judged against the
    // freshest anchor, not the last one that drew. current_seq == 0 means
    // "no sequence authority here" (tests, odd call sites) — no anchor.
    if (frame != nullptr && current_seq > 0) {
        std::lock_guard<std::mutex> anchor_lock(results_mu_);
        auto prev = last_seen_seq_.find(stream_name);
        if (prev != last_seen_seq_.end() && prev->second > 0 &&
            current_seq > prev->second && current_seq - prev->second < 1024) {
            // Learn the counter distance between this stream's consecutive
            // frames: the shared HAL counter advances once per frontend
            // callback across ALL streams, so this distance (not 1) is how
            // many steps one display frame is worth here.
            const double delta =
                static_cast<double>(current_seq - prev->second);
            auto ema = steps_per_frame_.find(stream_name);
            if (ema == steps_per_frame_.end())
                steps_per_frame_[stream_name] = delta;
            else
                ema->second = 0.7 * ema->second + 0.3 * delta;
        }
        last_seen_seq_[stream_name] = current_seq;
    }

    if (!config_.enabled || !frame) return;

    // Snapshot mutable config fields under lock
    bool draw_labels, draw_confidence, enable_face_blur;
    uint32_t box_thickness, face_blur_block_size, strict_wait_cap_ms;
    {
        std::lock_guard<std::mutex> lock(config_mu_);
        draw_labels = config_.draw_labels;
        draw_confidence = config_.draw_confidence;
        box_thickness = config_.box_thickness;
        enable_face_blur = config_.enable_face_blur;
        face_blur_block_size = config_.face_blur_block_size;
        strict_wait_cap_ms = config_.strict_wait_cap_ms;
    }

    // Strict gating (P1-6) applies only to identity-fed display streams
    // (stream_map[D] == D): the frame waits, bounded, for ITS OWN result.
    const bool strict = strict_gate_active(stream_name);

    // unique_lock: the strict path's bounded wait must release results_mu_
    // so subscriber_loop can store the incoming result and wake it.
    std::unique_lock<std::mutex> lock(results_mu_);

    // One drawable layer = exactly one renderer call + its polygons (+ face
    // mosaics on the unified path). Runs under results_mu_, reading the
    // layer's StreamResult in place.
    auto draw_layer = [&](const StreamResult& sr) {
        if (config_.draw_ops->draw_result) {
            // Seed from the HAL-sanctioned defaults (constructor's
            // default_draw_cfg_) so draw_detections/colors/other toggles are
            // on, then overlay the runtime knobs. Per-stream configs are no
            // longer cached: the old zero-initialized draw_cfg{} silently
            // disabled the whole detection branch in the HAL draw.
            HalDrawConfig cfg = compose_draw_config(default_draw_cfg_, draw_labels,
                                                    draw_confidence, box_thickness);
            config_.draw_ops->draw_result(&sr.result, frame, &cfg);

            // Face blur rides on top of the unified draw pass so
            // boxes/labels stay under HAL control; the mosaic covers them.
            if (enable_face_blur && config_.draw_ops->draw_mosaic) {
                draw_face_mosaics(sr.result, frame, config_.draw_ops,
                                  face_blur_block_size);
            }
        } else {
            draw_with_primitives(sr.result, frame, draw_labels, draw_confidence,
                                 box_thickness, enable_face_blur,
                                 face_blur_block_size);
        }

        // Zone polygons ride on every path, independent of which result
        // renderer ran above (and of whether a primary result exists).
        draw_result_polygons(sr.polygons, frame, config_.draw_ops, draw_labels);
    };

    // Collect the drawable layers routed onto this display stream, split
    // app/platform, applying the TTL and frame-binding windows. Shared by
    // the strict branch (app half only — plat_out may be null) and the
    // multi-layer path below: one set of expiry rules, two compositions.
    // App layers come back newest-first; platform layers keep storage
    // order. `now` is refreshed by the caller after any bounded wait (the
    // strict gate parks) so ages are measured against post-wait time.
    auto now = std::chrono::steady_clock::now();
    auto collect_fresh_layers = [&](std::vector<Layer*>& app_out,
                                    std::vector<Layer*>* plat_out,
                                    bool* have_unbound_app) {
        if (have_unbound_app) *have_unbound_app = false;
        for (const std::string& key : routed_keys(stream_name, config_)) {
            auto lit = results_.find(key);
            if (lit == results_.end()) continue;
            for (Layer& l : lit->second) {
                if (!l.sr.valid) continue;
                const uint32_t ttl_ms =
                    resolve_result_ttl_ms(l.sr.ttl_ms, stream_name, config_);
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 now - l.sr.last_update_time);
                if (age > std::chrono::milliseconds(ttl_ms)) {
                    l.sr.valid = false;  // expired: drops out until re-seeded
                    continue;
                }
                // Frame-bound layer: draws only while the bake site is at or
                // before bound_seq + slack — past that the command is from a
                // dead frame generation. Validity is untouched here: binding
                // expiry governs drawing, TTL governs storage. The slack is
                // in HAL-counter steps scaled to display frames (see
                // bind_slack_steps): a shared multi-stream counter makes raw
                // steps sub-frame.
                if (l.bound && current_seq != 0 &&
                    current_seq > l.bound_seq + bind_slack_steps(stream_name)) {
                    continue;
                }
                if (l.is_app) {
                    app_out.push_back(&l);
                    if (have_unbound_app && !l.bound) *have_unbound_app = true;
                } else if (plat_out) {
                    plat_out->push_back(&l);
                }
            }
        }
        std::sort(app_out.begin(), app_out.end(),
                  [](const Layer* a, const Layer* b) {
                      return a->sr.last_update_time > b->sr.last_update_time;
                  });
    };

    bool skip_ttl = false;
    if (strict) {
        // Strict frame-locks the stream's own platform result (the gate
        // below); app layers are not part of that lock — they are
        // command-driven and TTL-governed — so they draw in BOTH shapes:
        // on top of the locked platform layer, or alone when the gate
        // SKIPs / the locked result aged out. Strict never suppresses the
        // locked platform layer (the frame-sync guarantee is the point of
        // the mode) and never admits cross-fed platform layers (the
        // multi-layer path's plat half stays closed here).
        Layer* gated = strict_gate(lock, stream_name, frame, strict_wait_cap_ms,
                                   &skip_ttl);
        now = std::chrono::steady_clock::now();  // post-wait age base
        StreamResult* locked_sr = nullptr;
        if (gated != nullptr) {
            StreamResult& sr = gated->sr;
            if (!skip_ttl) {
                const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now - sr.last_update_time);
                // Validity window is per-stream resolvable: event override >
                // stream override > global config > 2x frame period >
                // fallback; stream_name is the display stream. LOCK_* draws
                // skip it: the result belongs to this exact frame, so its
                // age is the inference latency, not staleness.
                const uint32_t ttl_ms =
                    resolve_result_ttl_ms(sr.ttl_ms, stream_name, config_);
                if (age > std::chrono::milliseconds(ttl_ms)) {
                    sr.valid = false;  // degraded: app half may still draw
                } else {
                    locked_sr = &sr;
                }
            } else {
                locked_sr = &sr;
            }
        }
        std::vector<Layer*> app_layers;
        collect_fresh_layers(app_layers, nullptr, nullptr);
        if (locked_sr == nullptr && app_layers.empty()) {
            // Bake skip #1: strict SKIP / degraded with nothing else to
            // draw — the frame ships clean. Strict SKIPs are also tallied
            // in strict_skips; bake_skips is the layer-neutral observable
            // the encode side reconciles against.
            ++stream_stats_[stream_name].bake_skips;
            return;
        }
        if (!config_.draw_ops) {
            warn_no_draw_ops();
            return;
        }
        if (locked_sr != nullptr) draw_layer(*locked_sr);
        for (const Layer* l : app_layers) draw_layer(l->sr);
        return;
    }

    if (!is_bake_target(stream_name)) {
        // Source-only streams (key mapped to a foreign display — the clean
        // inference feed) and unmapped streams never draw. Without this
        // gate the layer collection below would hit the stream's OWN
        // inference results and bake them into the feed that exists to be
        // clean.
        return;
    }

    // Collect every drawable layer routed onto this display stream.
    // Fresh unbound app layers suppress the platform layers — the app owns
    // the picture while it is actively annotating; bound app layers stack
    // instead (they decorate a specific frame, not the stream).
    std::vector<Layer*> app_fresh, plat_fresh;
    bool have_fresh_app_suppressor = false;
    collect_fresh_layers(app_fresh, &plat_fresh, &have_fresh_app_suppressor);

    if (app_fresh.empty() && plat_fresh.empty()) {
        // Bake skip #1 (layer path): nothing drawable — the frame ships
        // clean.
        ++stream_stats_[stream_name].bake_skips;
        return;
    }
    if (!config_.draw_ops) {
        // Counted above when nothing was drawable; a null renderer with
        // drawable layers is a setup fault, not a skip — warn once.
        warn_no_draw_ops();
        return;
    }

    // Fixed draw order: app layers newest-first, then platform layers in
    // storage order (suppressed while a fresh unbound app layer owns the
    // stream).
    for (const Layer* l : app_fresh) draw_layer(l->sr);
    if (!have_fresh_app_suppressor) {
        for (const Layer* l : plat_fresh) draw_layer(l->sr);
    }
}

AiOverlaySubscriber::Layer* AiOverlaySubscriber::strict_gate(
        std::unique_lock<std::mutex>& lock,
        const std::string& stream_name,
        HalFrameBuffer* frame,
        uint32_t configured_cap_ms,
        bool* skip_ttl) {
    *skip_ttl = false;

    // Per-stream accounting entry (under results_mu_). References into
    // stream_stats_ survive rehash and concurrent inserts while parked in
    // the wait below — the same guarantee the layer lookup relies on.
    StreamStats& st = stream_stats_[stream_name];

    // Identity feed only — no cross-fed fallback: a foreign stream's
    // sequences live in a different counter space and can never match.
    // Strict locks the stream's FIRST non-app layer (storage order) and
    // does not join the multi-layer draw — single-source semantics.
    auto find_layer = [&]() -> Layer* {
        auto it = results_.find(stream_name);
        if (it == results_.end()) return nullptr;
        for (Layer& l : it->second) {
            if (!l.is_app) return &l;
        }
        return nullptr;
    };

    Layer* layer = find_layer();
    if (layer == nullptr) {
        ++st.strict_skips;
        ++st.gated_frame_count;
        log_strict_stats(stream_name);
        return nullptr;  // no platform result ever stored: ship clean, O(1)
    }

    const uint32_t frame_seq = frame->sequence;
    const uint32_t cap_ms =
        resolve_strict_wait_cap_ms(configured_cap_ms, stream_name, config_);
    StrictGateDecision d =
        strict_gate_decision(layer->sr.last_frame_seq, layer->sr.valid, frame_seq);

    bool degraded = false;
    if (d == StrictGateDecision::WAIT) {
        const auto wait_start = std::chrono::steady_clock::now();
        // Only wait when the predictor says the result is plausibly close:
        // expected wait = EMA of result interval − time since last result.
        // No EMA yet (startup) or no valid result = blind stall → degrade
        // immediately; one result interval is enough to seed the EMA.
        int64_t expected_ms = (int64_t)cap_ms + 1;  // hopeless by default
        if (layer->sr.valid && layer->sr.interval_ema_ms > 0) {
            auto since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                wait_start - layer->sr.last_update_time).count();
            expected_ms = (int64_t)layer->sr.interval_ema_ms - since_ms;
        }
        if (expected_ms < (int64_t)cap_ms / 2) {
            const auto deadline = wait_start + std::chrono::milliseconds(cap_ms);
            do {
                // Releases results_mu_ while parked; subscriber_loop's
                // notify_all on every stored result wakes us as soon as
                // this frame's own result lands. The release means the
                // layer vector may have grown or been erased (sweep,
                // restart purge) — re-find the layer every wake and never
                // hold the pointer across one. Gone = ship clean.
                result_cv_.wait_until(lock, deadline);
                layer = find_layer();
                if (layer == nullptr) {
                    d = StrictGateDecision::SKIP;
                    break;
                }
                d = strict_gate_decision(layer->sr.last_frame_seq,
                                         layer->sr.valid, frame_seq);
            } while (d == StrictGateDecision::WAIT &&
                     std::chrono::steady_clock::now() < deadline);
        }
        if (d == StrictGateDecision::WAIT) {
            degraded = true;  // cap expired or predicted hopeless
        }
        st.strict_wait_total_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - wait_start).count();
        ++st.strict_wait_samples;
    }
    ++st.gated_frame_count;

    if (d == StrictGateDecision::SKIP) {
        ++st.strict_skips;
        log_strict_stats(stream_name);
        return nullptr;
    }
    if (!degraded) {
        // LOCK_FRESH / LOCK_NEWEST: the result is this frame's (or the
        // wait overtook it) — the validity window does not apply.
        ++st.strict_locked;
        *skip_ttl = true;
        log_strict_stats(stream_name);
        return layer;
    }

    // Degraded to preview semantics: draw the freshest stored result and
    // let the caller's TTL check decide (decision was WAIT ⇒ valid).
    // The frame ships clean if the result ages out there.
    ++st.strict_degraded;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_degrade_log_time_ >= std::chrono::seconds(1)) {
        last_degrade_log_time_ = now;
        HAL_LOG_WARNING(
            "AiOverlaySubscriber: strict gate DEGRADE stream=%s frame_seq=%u "
            "have_seq=%llu cap=%u ms (locked=%llu degraded=%llu skipped=%llu)",
            stream_name.c_str(), frame_seq,
            (unsigned long long)layer->sr.last_frame_seq, cap_ms,
            (unsigned long long)st.strict_locked,
            (unsigned long long)st.strict_degraded,
            (unsigned long long)st.strict_skips);
    }
    log_strict_stats(stream_name);
    return layer;
}

void AiOverlaySubscriber::log_strict_stats(const std::string& stream_name)
{
    // Every-path milestone: without this on the SKIP/LOCK exits a fully
    // healthy run (all locked, nothing degraded) stays silent, and its
    // avg wait — the acceptance number for "encode latency = inference
    // latency + 1 frame" — is unobservable from the journal.
    auto stats_it = stream_stats_.find(stream_name);
    if (stats_it == stream_stats_.end()) {
        return;
    }
    const StreamStats& st = stats_it->second;
    if (st.gated_frame_count % 300 != 0) {
        return;
    }
    const double avg_wait_ms =
        st.strict_wait_samples
            ? (double)st.strict_wait_total_us / st.strict_wait_samples / 1000.0
            : 0.0;
    HAL_LOG_INFO(
        "AiOverlaySubscriber: strict gate stream=%s frames=%llu locked=%llu "
        "degraded=%llu skipped=%llu avg_wait=%.1f ms",
        stream_name.c_str(), (unsigned long long)st.gated_frame_count,
        (unsigned long long)st.strict_locked,
        (unsigned long long)st.strict_degraded,
        (unsigned long long)st.strict_skips, avg_wait_ms);
}

void AiOverlaySubscriber::snapshot_stream_stats(const std::string& stream_name,
                                                OverlayStreamStats* out)
{
    out->bake_skips = 0;
    out->strict_locked = 0;
    out->strict_degraded = 0;
    out->strict_skips = 0;
    out->stream_epoch = 0;
    out->overlay_layer_count = 0;
    out->overlay_late_commands = 0;
    out->overlay_epoch_rejects = 0;
    out->overlay_no_binding_drops = 0;

    std::lock_guard<std::mutex> lock(results_mu_);
    // Seed/answer the epoch even for a stream that never went through the
    // bake site — an app needs the CURRENT epoch to tag its commands with,
    // before anything has drawn.
    out->stream_epoch = lazy_stream_epoch(stream_epochs_, stream_name);

    // Layer/ingest counters aggregate every results_ key routed onto this
    // display stream (identity entry + cross-feeds + bindings) — the same
    // key set the bake site draws from.
    for (const std::string& key : routed_keys(stream_name, config_)) {
        auto lit = results_.find(key);
        if (lit != results_.end()) out->overlay_layer_count += lit->second.size();
        auto sit = stream_stats_.find(key);
        if (sit == stream_stats_.end()) continue;
        out->overlay_late_commands    += sit->second.late_commands;
        out->overlay_epoch_rejects    += sit->second.epoch_rejects;
        out->overlay_no_binding_drops += sit->second.no_binding_drops;
    }

    // Bake/strict counters live on the display stream's own entry only.
    auto it = stream_stats_.find(stream_name);
    if (it == stream_stats_.end()) {
        return;  // never through the bake site: bake/strict stay zero
    }
    out->bake_skips = it->second.bake_skips;
    out->strict_locked = it->second.strict_locked;
    out->strict_degraded = it->second.strict_degraded;
    out->strict_skips = it->second.strict_skips;
}

size_t AiOverlaySubscriber::handle_session_end(const std::string& session_id) {
    if (session_id.empty()) return 0;
    size_t swept = 0;
    {
        std::lock_guard<std::mutex> lock(results_mu_);
        for (auto& kv : results_) {
            std::vector<Layer>& layers = kv.second;
            for (auto it = layers.begin(); it != layers.end();) {
                // Rule 2 first, and exclusively: an erased app layer must
                // not also count (or clear) its polygons via rule 1 — one
                // layer, one sweep tick.
                if (it->is_app && it->session_id == session_id) {
                    it = layers.erase(it);
                    ++swept;
                    continue;
                }
                // Rule 1: owner match only — an untagged (operator-side)
                // write or a different session's write survives this
                // session's end. The !empty() guard makes a repeat sweep
                // a no-op.
                if (!it->sr.polygons.empty() &&
                    it->sr.polygons_session == session_id) {
                    it->sr.polygons.clear();
                    ++swept;
                }
                ++it;
            }
        }
        result_cv_.notify_all();
    }
    return swept;
}

size_t AiOverlaySubscriber::polygon_count(const std::string& stream_id) const {
    std::lock_guard<std::mutex> lock(results_mu_);
    auto it = results_.find(stream_id);
    if (it == results_.end()) return 0;
    size_t n = 0;
    for (const auto& layer : it->second) n += layer.sr.polygons.size();
    return n;
}

uint64_t AiOverlaySubscriber::bind_slack_steps(const std::string& stream) const {
    auto it = steps_per_frame_.find(stream);
    const uint64_t spf = it == steps_per_frame_.end() ? 1 : std::max<uint64_t>(1, static_cast<uint64_t>(it->second + 0.5));
    return kFrameBindSlack * spf;
}

uint64_t AiOverlaySubscriber::bind_past_steps(const std::string& stream) const {
    auto it = steps_per_frame_.find(stream);
    const uint64_t spf = it == steps_per_frame_.end() ? 1 : std::max<uint64_t>(1, static_cast<uint64_t>(it->second + 0.5));
    return kFrameBindPastFrames * spf;
}

void AiOverlaySubscriber::note_stream_restart(const std::string& stream) {
    std::lock_guard<std::mutex> lock(results_mu_);
    // Ensure seeded, then bump: results and commands of the old generation
    // die together — every layer held for the stream is purged, and app
    // events still tagged with the old epoch are rejected from now on.
    lazy_stream_epoch(stream_epochs_, stream);
    ++stream_epochs_[stream];
    results_.erase(stream);
    // The late-command anchor belongs to the same generation: a pipeline
    // rebuild can reset the HAL counter to small values while a surviving
    // stale anchor would (a) wrongly admit nothing and (b) expire every
    // newly bound layer against the wrong window. Dropping it returns the
    // stream to the no-authority state until the next bake re-seeds it.
    last_seen_seq_.erase(stream);
    // The frame pace is equally generation-bound: the rebuilt pipeline may
    // run a different stream set, so the old steps-per-frame EMA would
    // mis-scale the bind window until re-learned.
    steps_per_frame_.erase(stream);
    result_cv_.notify_all();
}

// Defined below (JSON helpers section) but used by handle_event's payload
// stream_id fallback for metadata-less publishers (auto-infer).
static bool json_find_string(const std::string& json, const std::string& key,
                             std::string& out);

void AiOverlaySubscriber::handle_event(
    const std::string& topic,
    const std::map<std::string, std::string>& metadata,
    const std::string& payload,
    const std::string& source) {

    // P2-13: the session-end broadcast carries no stream_id — it must be
    // handled before the stream gate below or the skip would drop it.
    if (is_overlay_session_end_topic(topic, config_.topic_prefix)) {
        auto sit = metadata.find("session_id");
        if (sit == metadata.end() || sit->second.empty()) {
            HAL_LOG_WARNING("AiOverlaySubscriber: session/end event without "
                            "session_id metadata, ignored");
            return;
        }
        const size_t swept = handle_session_end(sit->second);
        HAL_LOG_INFO("AiOverlaySubscriber: session '%s' ended, %zu layer(s) "
                     "swept (app layers erased, session polygons cleared)",
                     sit->second.c_str(), swept);
        return;
    }

    std::string stream_id;
    {
        auto it = metadata.find("stream_id");
        if (it != metadata.end())
            stream_id = it->second;
    }
    // auto-infer publishes topic inference/<stream> with NO metadata at
    // all — its stream_id rides in the payload. One string scan, skipped
    // whenever metadata already answered.
    if (stream_id.empty()) {
        json_find_string(payload, "stream_id", stream_id);
    }
    if (stream_id.empty()) return;

    // ── Admission (behavior decoupling) ──
    // Platform publishers: the empty source (test convention) plus the two
    // daemon services. A bare subscribe() must not change the video, so
    // platform results are stored ONLY when the stream is explicitly bound
    // (config.bindings) or the legacy switch is on. Anything else is an app
    // (SDK publish, source=app_id) and is always admitted — an app's own
    // annotate channel must never be crowded out by this gate.
    const bool is_platform =
        source.empty() || source == "ai-runtime" || source == "auto-infer";
    const bool is_app = !is_platform;
    if (is_platform && !config_.legacy_auto_bind &&
        config_.bindings.count(stream_id) == 0) {
        std::lock_guard<std::mutex> lock(results_mu_);
        ++stream_stats_[stream_id].no_binding_drops;
        return;
    }

    // Primary-result discriminator: a payload carrying any result key
    // replaces the stream's primary result. A polygons-only payload
    // (or a ttl-only refresh) carries none, so the boxes from the
    // last detections event stand — zones and boxes are independent
    // channels. ai-runtime's result payloads always carry
    // frame_sequence (StreamInfer attaches it), which also covers
    // the empty-detections frame: the publisher omits
    // "detections":[] when N=0, so without the frame_sequence term
    // an object-free frame would read as "not a primary result" and
    // the strict gate could never lock on an empty scene.
    const bool has_primary =
        payload.find("\"num_detections\":")      != std::string::npos ||
        payload.find("\"detections\":[")         != std::string::npos ||
        payload.find("\"classifications\":[")    != std::string::npos ||
        payload.find("\"landmarks\":[")          != std::string::npos ||
        payload.find("\"ocr_lines\":[")          != std::string::npos ||
        payload.find("\"num_keypoint_objects\"") != std::string::npos ||
        payload.find("\"frame_sequence\":")      != std::string::npos;

    HalPostprocessResult result{};
    const bool have_result = has_primary &&
                             parse_json_result(payload, stream_id, &result);

    std::vector<OverlayPolygon> polys;
    const bool have_polys = parse_overlay_polygons(payload, &polys);

    // Session tag of THIS event's polygons write; read once so the
    // lock section only touches plain locals.
    std::string polygons_session;
    if (have_polys) {
        auto sit = metadata.find("session_id");
        if (sit != metadata.end())
            polygons_session = sit->second;
    }

    // Per-event override; absent metadata resets it — the override
    // described THAT event, it must not stick to later ones. Malformed
    // values parse as unset (0), never a wrapped huge TTL.
    uint32_t ttl_ms = 0;
    {
        auto it = metadata.find("result_ttl_ms");
        if (it != metadata.end()) {
            ttl_ms = parse_result_ttl_ms(it->second);
        }
    }

    // ── Layer identity ──
    // App: (metadata session_id or "", the app source itself) — the
    // session key lets the sweep erase the app's layers wholesale.
    // Platform: ("", source, or "source/model_id") — session stays empty so
    // a session sweep never erases platform layers, and a model-tagged feed
    // is its own layer so multiple models stack instead of replacing.
    std::string layer_session;
    std::string source_id = source;
    if (is_app) {
        auto sit = metadata.find("session_id");
        if (sit != metadata.end()) layer_session = sit->second;
    } else {
        auto mit = metadata.find("model_id");
        if (mit != metadata.end() && !mit->second.empty())
            source_id = source + "/" + mit->second;
    }

    // Frame-binding value, read once: the ingest late-command guard and the
    // layer's bound_seq share it. Binding params ride metadata, never the
    // payload — the has_primary payload scan above must keep its semantics.
    uint64_t cmd_seq = 0;
    const bool have_cmd_seq = metadata.count("frame_sequence") != 0;
    if (have_cmd_seq) {
        auto it = metadata.find("frame_sequence");
        cmd_seq = strtoull(it->second.c_str(), nullptr, 10);
    }

    {
        std::lock_guard<std::mutex> lock(results_mu_);

        // App-only ingest guards: platform feeds are daemon-fed and never
        // stale by construction.
        if (is_app) {
            // Stale epoch = command from before a restart/reconfigure —
            // its frame generation is gone; drawing it would decorate the
            // wrong picture.
            auto eit = metadata.find("stream_epoch");
            if (eit != metadata.end()) {
                const uint64_t ev_epoch =
                    strtoull(eit->second.c_str(), nullptr, 10);
                if (ev_epoch != lazy_stream_epoch(stream_epochs_, stream_id)) {
                    ++stream_stats_[stream_id].epoch_rejects;
                    return;
                }
            }
            // frame_sequence far past the last frame the bake site saw =
            // a command for a dead frame generation (sequence restarted
            // or raced ahead). Judged only when an anchor exists — before
            // the first bake the daemon has no authority to call it late.
            // The window is symmetric in display frames: too-future
            // commands (beyond the bind slack) and long-dead commands
            // (older than kFrameBindPastFrames frames of latency budget)
            // are both rejected, so late_commands keeps meaning "the
            // pipeline is nowhere near that frame" while results that
            // merely carry a few frames of inference latency pass.
            if (have_cmd_seq) {
                auto anc = last_seen_seq_.find(stream_id);
                if (anc != last_seen_seq_.end() && anc->second > 0 &&
                    (cmd_seq > anc->second + bind_slack_steps(stream_id) ||
                     cmd_seq + bind_past_steps(stream_id) < anc->second)) {
                    ++stream_stats_[stream_id].late_commands;
                    return;
                }
            }
        }

        // Same identity replaces its layer wholesale; different identities
        // stack. The interval EMA carries across a replace (measured
        // against the old layer's last_update_time) so the strict
        // predictor does not restart from zero on every event.
        std::vector<Layer>& stack = results_[stream_id];
        Layer* layer = nullptr;
        for (Layer& l : stack) {
            if (l.session_id == layer_session && l.source_id == source_id) {
                layer = &l;
                break;
            }
        }
        if (layer == nullptr) {
            stack.emplace_back();
            layer = &stack.back();
            layer->session_id = layer_session;
            layer->source_id = source_id;
        }
        StreamResult& sr = layer->sr;

        const auto now = std::chrono::steady_clock::now();
        // Strict-mode predictor input: EMA of result inter-arrival.
        // Only real results tick it; a ≥2s gap (stream restarted,
        // long stall) resets it so a stale cadence never parks the
        // gate. First sample seeds; g is floored at 1ms.
        if (have_result && sr.valid) {
            uint64_t g = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                             now - sr.last_update_time).count();
            if (g < 2000) {
                sr.interval_ema_ms = sr.interval_ema_ms
                                         ? (sr.interval_ema_ms * 7 + g * 3) / 10
                                         : (g > 0 ? (uint32_t)g : 1);
            } else {
                sr.interval_ema_ms = 0;
            }
        }
        if (have_result) sr.result = result;
        if (have_polys)  sr.polygons = std::move(polys);
        // P2-13: latest write owns the sidecar's session tag; an
        // untagged write resets it (never swept by a session end).
        if (have_polys)  sr.polygons_session = std::move(polygons_session);
        sr.ttl_ms = ttl_ms;
        // Only daemon-fed StreamInfer results carry frame_sequence in the
        // payload; annotate events without it leave the stale seq, so the
        // strict gate degrades them instead of locking a mismatch.
        uint64_t seq = 0;
        if (have_result && parse_frame_sequence(payload, &seq)) {
            sr.last_frame_seq = seq;
        }
        sr.last_update_time = now;
        sr.valid = true;

        // Layer bookkeeping (per event, so a replace also refreshes it).
        layer->is_app = is_app;
        layer->epoch = lazy_stream_epoch(stream_epochs_, stream_id);
        // Metadata frame_sequence binds an app layer to that frame: it
        // draws only within the kFrameBindSlack window around it. Absent
        // metadata = an unbound, free-running layer.
        layer->bound = false;
        layer->bound_seq = 0;
        if (is_app && have_cmd_seq) {
            layer->bound = true;
            layer->bound_seq = cmd_seq;
        }
        // Wake any strict gate parked on this frame's result.
        result_cv_.notify_all();
    }
}

void AiOverlaySubscriber::draw_with_primitives(const HalPostprocessResult& result,
                                                HalFrameBuffer* frame,
                                                bool draw_labels,
                                                bool draw_confidence,
                                                uint32_t box_thickness,
                                                bool enable_face_blur,
                                                uint32_t face_blur_block_size) {
    const auto* ops = config_.draw_ops;
    if (!ops || !frame) return;

    // ── Detection: bounding boxes + labels ──────────────────────────────────
    if (result.type == HAL_POST_TYPE_DETECTION) {
        auto& det = result.result.detection;
        for (uint32_t i = 0; i < det.num_detections; i++) {
            const auto& d = det.detections[i];

            // bboxes are normalized [0,1] — scale to frame pixels with the
            // HAL helper (same semantics as the unified draw_result path).
            HalDrawRect rect{};
            if (ops->draw_rect) {
                hal_bbox_to_rect(&d.bbox, frame->width, frame->height, &rect);
                rect.color = {0, 255, 0, 255};
                rect.thickness = static_cast<int32_t>(box_thickness);
                ops->draw_rect(frame, &rect);
            } else {
                hal_bbox_to_rect(&d.bbox, frame->width, frame->height, &rect);
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
                txt.x = rect.x;
                txt.y = rect.y;
                strncpy(txt.text, text_buf, sizeof(txt.text) - 1);
                txt.color = {255, 255, 255, 255};
                txt.font_scale = 0.6f;
                txt.thickness = 1;
                ops->draw_text(frame, &txt);
            }
        }

        // Parity with the unified draw_result path: mosaic face detections on
        // top of the boxes/labels drawn above.
        if (enable_face_blur && ops->draw_mosaic) {
            draw_face_mosaics(result, frame, ops, face_blur_block_size);
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

// json_find_number cannot read booleans — strtod stops at 't'/'f' and reports
// a consumed-zero. Scan the literal itself ("closed":true / false).
static bool json_find_bool(const std::string& json, const std::string& key, bool& out) {
    std::string pattern = "\"" + key + "\":";
    auto pos = json.find(pattern);
    if (pos == std::string::npos) return false;
    pos += pattern.size();
    while (pos < json.size() && json[pos] == ' ') pos++;
    if (json.compare(pos, 4, "true") == 0) { out = true; return true; }
    if (json.compare(pos, 5, "false") == 0) { out = false; return true; }
    return false;
}

// ─── Overlay payload helpers (free functions, unit-testable) ────────────────

// Mirrors the SDK-side cap (_POLYGON_CAP): at most this many zone polygons
// per payload; extra entries are dropped rather than trusted.
static constexpr uint32_t OVERLAY_POLYGON_CAP = 16;

bool parse_overlay_polygons(const std::string& payload,
                            std::vector<OverlayPolygon>* out) {
    if (out == nullptr) return false;
    auto arr_pos = payload.find("\"polygons\":[");
    if (arr_pos == std::string::npos) return false;
    size_t p = payload.find('[', arr_pos);
    if (p == std::string::npos) return false;
    ++p;  // past the array's '['

    std::vector<OverlayPolygon> parsed;
    auto skip_sep = [&](size_t& i) {
        while (i < payload.size() &&
               (payload[i] == ' ' || payload[i] == '\t' ||
                payload[i] == '\n' || payload[i] == '\r' || payload[i] == ',')) ++i;
    };

    while (parsed.size() < OVERLAY_POLYGON_CAP) {
        size_t i = p;
        skip_sep(i);
        if (i >= payload.size()) break;
        if (payload[i] == ']') break;   // end of the polygons array
        if (payload[i] != '{') break;   // malformed tail — keep what we have

        // Polygon objects hold no nested braces (points are [[x,y],...]), so
        // the first '}' closes the object exactly.
        auto obj_end = payload.find('}', i);
        if (obj_end == std::string::npos) break;
        std::string obj = payload.substr(i, obj_end - i + 1);

        OverlayPolygon poly;
        auto pts_pos = obj.find("\"points\":[");
        if (pts_pos != std::string::npos) {
            // '"points":[' = 10 chars, so +10 lands exactly on the first
            // pair's '[' — the loop's own ++q steps into it. (An extra ++q
            // here made the first pair start on a digit, aborting the scan.)
            const char* q = obj.c_str() + pts_pos + 10;
            while (*q != '\0') {
                while (*q == ' ' || *q == ',') ++q;
                if (*q == ']') break;    // end of the points array
                if (*q != '[') break;    // not a pair — stop this polygon
                ++q;
                char* ep = nullptr;
                float x = strtof(q, &ep);
                if (ep == q) break;      // not a number
                q = ep;
                while (*q == ' ') ++q;
                if (*q != ',') break;
                ++q;
                while (*q == ' ') ++q;
                float y = strtof(q, &ep);
                if (ep == q) break;
                q = ep;
                while (*q == ' ') ++q;
                if (*q != ']') break;    // pair must close
                ++q;
                poly.points.emplace_back(x, y);
                if (poly.points.size() >= (size_t)HAL_MAX_POLYGON_POINTS) break;
            }
        }

        bool closed = true;  // absent "closed" keeps the default
        json_find_bool(obj, "closed", closed);
        poly.closed = closed;
        std::string label;
        if (json_find_string(obj, "label", label)) poly.label = label;

        double v = 0;
        if (json_find_number(obj, "color", v)) {
            poly.color = (uint32_t)(int64_t)v;
            poly.has_color = true;
        }
        if (json_find_number(obj, "thickness", v)) {
            poly.thickness = (int32_t)v;
            poly.has_thickness = true;
        }

        // Skip malformed entries (< 2 points) without failing the rest — a
        // wrong polygon must not kill the boxes channel.
        if (poly.points.size() >= 2) parsed.push_back(std::move(poly));

        p = obj_end + 1;
    }

    *out = std::move(parsed);
    return true;
}

void apply_track_id_label(char* label, size_t cap, int32_t track_id) {
    if (label == nullptr || cap == 0 || track_id < 0) return;

    char suffix[16];
    int sn = snprintf(suffix, sizeof(suffix), " #%d", (int)track_id);
    if (sn <= 0) return;
    size_t slen = ((size_t)sn < sizeof(suffix)) ? (size_t)sn : sizeof(suffix) - 1;
    if (slen + 1 > cap) return;  // suffix cannot fit at all — leave label as is

    // Truncate the label from the right when needed; the fresh suffix wins,
    // because the id is the newer information than the tail of the class name.
    size_t len = strnlen(label, cap);
    if (len + slen + 1 > cap) len = cap - slen - 1;
    memcpy(label + len, suffix, slen);
    label[len + slen] = '\0';
}

bool is_overlay_session_end_topic(const std::string& topic,
                                  const std::string& topic_prefix) {
    return topic == topic_prefix + "session/end";
}

uint32_t parse_result_ttl_ms(const std::string& raw) {
    // Signed parse + explicit range check: a bare strtoul wraps "-1" to
    // ~4.29e9 (a layer that never expires) and silently truncates values
    // beyond 32 bits. Out-of-range strtoll saturates at LLONG_MAX/LLONG_MIN,
    // which the range check rejects just the same.
    char* end = nullptr;
    const long long v = strtoll(raw.c_str(), &end, 10);
    if (end == raw.c_str() || *end != '\0' || v <= 0 ||
        v > static_cast<long long>(RESULT_TTL_MAX_MS)) {
        return 0;
    }
    return static_cast<uint32_t>(v);
}

uint32_t resolve_result_ttl_ms(uint32_t per_result_ttl,
                               const std::string& display_stream,
                               const AiOverlayConfig& cfg) {
    if (per_result_ttl > 0) return per_result_ttl;
    auto sit = cfg.stream_result_ttls.find(display_stream);
    if (sit != cfg.stream_result_ttls.end() && sit->second > 0) return sit->second;
    if (cfg.result_ttl_ms > 0) return cfg.result_ttl_ms;

    auto fit = cfg.stream_fps.find(display_stream);
    if (fit != cfg.stream_fps.end() && fit->second > 0) {
        // Two frame periods, rounded to nearest ms, floor 1ms (a 1000fps
        // stream must not get a zero window). 30fps -> 67ms, 15fps -> 133ms.
        uint64_t ms = (2000ull + fit->second / 2) / fit->second;
        return (uint32_t)std::max<uint64_t>(ms, 1);
    }
    return RESULT_TTL_FALLBACK_MS;
}

HalDrawConfig compose_draw_config(const HalDrawConfig& base,
                                  bool draw_labels,
                                  bool draw_confidence,
                                  uint32_t box_thickness) {
    HalDrawConfig cfg = base;
    // The overlay renderer is only invoked for a result that should be drawn,
    // so detections are unconditionally on — a zeroed base (degenerate
    // caller) must not silently disable the branch like the old per-stream
    // zero-init configs did.
    cfg.draw_detections = true;
    cfg.draw_detection_labels = draw_labels;
    cfg.draw_detection_confidence = draw_confidence;
    if (box_thickness > 0) {
        cfg.default_box_thickness = static_cast<int32_t>(box_thickness);
    }
    return cfg;
}

// Strict wait caps run on the streaming thread — hard ceiling regardless
// of configuration, plus the fallback when nothing derives a value.
static constexpr uint32_t STRICT_WAIT_CAP_MAX_MS = 500;
static constexpr uint32_t STRICT_WAIT_CAP_FALLBACK_MS = 66;  // ≈2 periods @30fps

StrictGateDecision strict_gate_decision(uint64_t latest_result_seq,
                                        bool has_valid_result,
                                        uint32_t frame_sequence) {
    if (!has_valid_result) return StrictGateDecision::SKIP;
    if (latest_result_seq == (uint64_t)frame_sequence) return StrictGateDecision::LOCK_FRESH;
    if (latest_result_seq > (uint64_t)frame_sequence) return StrictGateDecision::LOCK_NEWEST;
    return StrictGateDecision::WAIT;
}

uint32_t resolve_strict_wait_cap_ms(uint32_t configured_cap_ms,
                                    const std::string& display_stream,
                                    const AiOverlayConfig& cfg) {
    if (configured_cap_ms > 0) return std::min(configured_cap_ms, STRICT_WAIT_CAP_MAX_MS);

    auto fit = cfg.stream_fps.find(display_stream);
    if (fit != cfg.stream_fps.end() && fit->second > 0) {
        // Same derivation as the TTL chain: two frame periods rounded to
        // nearest ms, floored at 1ms, capped at the streaming-thread ceiling.
        uint64_t ms = (2000ull + fit->second / 2) / fit->second;
        ms = std::max<uint64_t>(ms, 1);
        return (uint32_t)std::min<uint64_t>(ms, STRICT_WAIT_CAP_MAX_MS);
    }
    return STRICT_WAIT_CAP_FALLBACK_MS;
}

bool strict_cross_fed(const std::string& display_stream, const AiOverlayConfig& cfg) {
    for (const auto& [infer_stream, display] : cfg.stream_map) {
        if (display == display_stream && infer_stream != display_stream) return true;
    }
    return false;
}

bool parse_frame_sequence(const std::string& payload, uint64_t* out_seq) {
    double v = 0.0;
    if (!json_find_number(payload, "frame_sequence", v)) return false;
    *out_seq = (uint64_t)v;
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

            const std::string payload(event.payload().begin(),
                                      event.payload().end());
            handle_event(event.topic(),
                         std::map<std::string, std::string>(
                             event.metadata().begin(), event.metadata().end()),
                         payload,
                         event.source());
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

            // track_id rides as a "#id" label suffix rather than widening the
            // HAL detection struct; absent or negative means untracked.
            double track_id = -1;
            json_find_number(obj, "track_id", track_id);
            apply_track_id_label(det.detections[i].label,
                                 sizeof(det.detections[i].label),
                                 static_cast<int32_t>(track_id));

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
