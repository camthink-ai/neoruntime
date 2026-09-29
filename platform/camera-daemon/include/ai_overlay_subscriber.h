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
#include <vector>
#include <utility>
#include <mutex>
#include <atomic>
#include <thread>
#include <memory>
#include <map>
#include <chrono>
#include <condition_variable>

extern "C" {
    #include "hal_postprocess.h"
    #include "hal_draw.h"
    #include "hal_buffer.h"
}

// One zone polygon from an annotate(polygons=...) event. Points are the
// payload's normalized [0,1] coords kept verbatim — conversion to pixels
// happens at draw time, when the frame size is known. color/thickness ride
// as optional pass-throughs (has_* marks whether the payload carried them).
struct OverlayPolygon {
    std::vector<std::pair<float, float>> points;  // normalized x,y
    std::string label;
    bool     closed = true;
    uint32_t color = 0;          // ARGB, when has_color
    int32_t  thickness = 0;
    bool     has_color = false;
    bool     has_thickness = false;
};

struct AiOverlayConfig {
    bool        enabled = false;
    std::string event_bus_endpoint = "unix:///run/aipc/event-bus.sock";
    std::string topic_prefix       = "inference/";

    bool     draw_detections  = true;
    bool     draw_labels      = true;
    bool     draw_confidence  = true;
    bool     draw_landmarks   = true;
    bool     enable_face_blur = false;
    uint32_t face_blur_block_size = 8;   // mosaic cell size (px); 0 = blur
    uint32_t box_thickness    = 2;

    // Result validity window. 0 = derive: per-result ttl (event metadata
    // "result_ttl_ms") > stream override (stream_result_ttls) > global
    // result_ttl_ms > round(2000/fps) from stream_fps > 500ms fallback.
    uint32_t result_ttl_ms = 0;
    std::unordered_map<std::string, uint32_t> stream_result_ttls;  // keyed like stream_map
    std::unordered_map<std::string, uint32_t> stream_fps;          // display name -> fps

    std::unordered_map<std::string, std::string> stream_map;

    // Behavior decoupling (output-isolation scope cut). A bare subscribe()
    // must not change the video: platform result events ("ai-runtime" /
    // "auto-infer" publishers) draw only when their infer stream is bound
    // here (infer stream -> display stream, same direction as stream_map)
    // or the legacy switch is on. App events (SDK publish, source=app_id)
    // are always admitted. Static daemon config only — never an RPC knob.
    bool legacy_auto_bind = false;
    std::map<std::string, std::string> bindings;

    // Strict frame-lock (P1-6): when enabled and the display stream's
    // inference feed is itself (stream_map[D] == D), the bake waits —
    // bounded by strict_wait_cap_ms — for the result belonging to the
    // exact frame before drawing, so boxes and picture are the same
    // frame. The lock covers the PLATFORM result only; app layers
    // (SDK annotate) draw alongside it — TTL-governed, never suppressed,
    // and also alone when the gate SKIPs or the locked result aged out.
    // Wait expiry degrades to the preview semantics (freshest TTL-valid
    // result) with a counter + rate-limited warning; the encoder is
    // never stalled past the cap. 0 cap = derive: 2x frame period of the
    // display stream, clamped to [1,500] ms.
    bool     strict_frame_lock = false;
    uint32_t strict_wait_cap_ms = 0;

    const HalDrawOps* draw_ops = nullptr;
};

// Collects pixel-space mosaic rects for detections labeled "face"
// (case-insensitive). Normalized bboxes are scaled to the frame and clipped to
// its bounds (hal_bbox_to_rect semantics); degenerate rects are skipped.
// Returns the number of rects written to out, at most cap. Free function so
// the geometry is unit-testable without a live subscriber or frame.
size_t collect_face_mosaic_rects(const HalPostprocessResult& result,
                                 uint32_t frame_width, uint32_t frame_height,
                                 uint32_t block_size,
                                 HalDrawMosaic* out, size_t cap);

// Parses the payload's "polygons": [...] array into out (replacing its
// contents). Returns true when the payload carried the key (out then holds
// the parsed list, empty included — an empty list clears the sidecar).
// Absent key -> out untouched, returns false. Malformed entries are
// skipped, not fatal — a wrong polygon must not kill the boxes channel.
bool parse_overlay_polygons(const std::string& payload,
                            std::vector<OverlayPolygon>* out);

// Appends " #<id>" to label when track_id >= 0, truncating to cap.
// Detection labels carry the track suffix this way instead of widening the
// HAL struct. No-op for negative ids (the "untracked" convention).
void apply_track_id_label(char* label, size_t cap, int32_t track_id);

// True when topic is the session-end broadcast for this subscriber's
// prefix ("<prefix>session/end") — the P2-13 overlay sweep trigger.
// Exact match: the topic names no stream, the affected session rides in
// the event metadata ("session_id"). Reserved name: a result topic whose
// model+stream literally spell "session/end" would collide, publishers
// must not use those names.
bool is_overlay_session_end_topic(const std::string& topic,
                                  const std::string& topic_prefix);

// Resolves the effective result TTL (ms): per-result ttl > stream override >
// global cfg ttl > 2x frame period from cfg.stream_fps > 500ms fallback.
uint32_t resolve_result_ttl_ms(uint32_t per_result_ttl,
                               const std::string& display_stream,
                               const AiOverlayConfig& cfg);

// Parses a "result_ttl_ms" event-metadata value: positive decimal ms,
// capped at RESULT_TTL_MAX_MS. Malformed input — non-numeric, negative,
// zero, overflow — returns 0 (unset), never a wrapped or truncated value:
// the metadata is wire input and must not be trusted (a bare strtoul
// turns "-1" into ~4.29e9, a layer that never expires).
uint32_t parse_result_ttl_ms(const std::string& raw);

// Builds the per-frame HAL draw config from the HAL-sanctioned defaults
// (hal_draw_config_init_default) plus the overlay's runtime knobs (labels,
// confidence, box thickness). The defaults seed everything — draw_detections,
// box/text colors, keypoint/segmentation/ocr toggles — so the unified
// draw_result path never sees a zeroed config that silently disables the
// whole detection branch. draw_detections is forced on: this renderer is
// only reached when a detection-style result should be drawn.
HalDrawConfig compose_draw_config(const HalDrawConfig& base,
                                  bool draw_labels,
                                  bool draw_confidence,
                                  uint32_t box_thickness);

// Strict-gate verdict for one display frame (P1-6). Pure sequence
// comparison, no timing — the caller owns the wait, the cap, and the
// degrade on expiry.
enum class StrictGateDecision {
    SKIP,          // no result for the stream at all: draw nothing, no wait
    WAIT,          // stored result is OLDER than the frame: caller may wait
    LOCK_FRESH,    // stored result is this exact frame: draw it, no TTL check
    LOCK_NEWEST,   // stored result is NEWER (the wait overtook): draw it
};

// latest_result_seq is the stream's newest stored result sequence,
// has_valid_result whether any result is stored, frame_sequence the
// sequence of the frame about to be encoded.
StrictGateDecision strict_gate_decision(uint64_t latest_result_seq,
                                        bool has_valid_result,
                                        uint32_t frame_sequence);

// Effective strict wait cap (ms): configured value clamped to [1,500]
// > round((2000 + fps/2) / fps) of the display stream — two frame
// periods, floored at 1 — > 66ms fallback. Mirrors the
// resolve_result_ttl_ms chain.
uint32_t resolve_strict_wait_cap_ms(uint32_t configured_cap_ms,
                                    const std::string& display_stream,
                                    const AiOverlayConfig& cfg);

// True when a foreign infer stream targets display_stream (some
// stream_map entry maps infer != display onto it). The shared HAL
// counter ticks once per frontend callback, so cross-stream sequences
// can never match — strict lock is not offered for such displays.
// Config-level diagnosis (log-once); the runtime identity check is
// stream_map.count(D) && stream_map[D] == D.
bool strict_cross_fed(const std::string& display_stream,
                      const AiOverlayConfig& cfg);

// Parses the result payload's "frame_sequence": N into out_seq.
// Returns false when the key is absent or its value non-numeric;
// out_seq is untouched on false.
bool parse_frame_sequence(const std::string& payload, uint64_t* out_seq);

// Frame-binding slack (frames): an app layer bound to frame N draws while
// the bake site's current frame is <= N + kFrameBindSlack, then expires —
// commands bound to a frame the pipeline has already passed stop drawing
// instead of haunting the stream forever. The same slack bounds the
// ingest-side late-command judgement (frame_sequence far past the last
// frame the bake site saw = dead generation, rejected).
constexpr uint64_t kFrameBindSlack = 2;
// How many display frames behind the anchor a bound command may arrive
// before the ingest gate calls it dead: covers inference latency (a
// result riding a subscribe() iteration is a few frames old on arrival)
// with margin, while still rejecting genuinely stale generations.
constexpr uint64_t kFrameBindPastFrames = 10;

class AiOverlaySubscriber {
public:
    explicit AiOverlaySubscriber(const AiOverlayConfig& config);
    ~AiOverlaySubscriber();

    AiOverlaySubscriber(const AiOverlaySubscriber&) = delete;
    AiOverlaySubscriber& operator=(const AiOverlaySubscriber&) = delete;

    bool start();
    void stop();

    void apply_overlay(const std::string& stream_name, HalFrameBuffer* frame,
                       uint64_t current_seq = 0);

    bool is_running() const { return running_.load(); }

    void update_config(bool draw_labels, bool draw_confidence, uint32_t box_thickness,
                       bool enable_face_blur);

    // Hot-path control for strict mode (UpdateAiOverlay RPC / reload).
    void update_strict(bool strict_frame_lock, uint32_t strict_wait_cap_ms);

    // True when strict gating applies to this display stream: strict
    // enabled in config and the stream's inference feed is itself.
    bool strict_gate_active(const std::string& display_stream);

    // True when this stream is an overlay bake target — it appears as a
    // stream_map OR bindings VALUE (identity entries D→D and cross-fed
    // entries I→D both count). Streams that are only inference SOURCES
    // (key mapped to a foreign display, e.g. a clean inference feed) are
    // not bake targets: apply_overlay never draws on them and the
    // frame-metadata baked flag stays clear.
    bool is_bake_target(const std::string& stream);

    // P2-13 session lifecycle -------------------------------------------------
    //
    // An app session spans capture, inference, overlay and output; the
    // zone-polygon sidecar is the one overlay state that outlives its
    // publisher (boxes expire via TTL, polygons do not). Polygon writes
    // tagged with metadata "session_id" belong to that session: a
    // session/end event (or an explicit call) sweeps them from every
    // stream in one pass. An untagged write resets the tag — operator-
    // side writes are never swept by a dying app session.

    // Handles one event-bus event (topic + metadata + payload + source):
    // the same body the subscriber loop runs per event, split out so the
    // tag-and-sweep semantics are testable without a live bus. source is
    // the event-bus Event.source: "" (test convention), "ai-runtime" and
    // "auto-infer" are platform publishers — admitted only when bound
    // (config.bindings) or legacy_auto_bind is on; anything else is an app
    // (SDK publish, source=app_id) and is always stored as its own layer.
    void handle_event(const std::string& topic,
                      const std::map<std::string, std::string>& metadata,
                      const std::string& payload,
                      const std::string& source = "");

    // Stream restart hook (reconfigure / transform full reinit): bumps the
    // stream's epoch and purges every layer held for it — results bound to
    // the old generation must not survive into the new one. App events
    // tagged with a stale epoch are rejected from then on.
    void note_stream_restart(const std::string& stream);

    // Effective frame-bind window for a display stream, in HAL-counter
    // steps: kFrameBindSlack display frames converted via the stream's
    // learned steps-per-frame EMA (unlearned → 1 step/frame). Caller must
    // hold results_mu_. The past window for the ingest gate is
    // kFrameBindPastFrames frames in the same conversion.
    uint64_t bind_slack_steps(const std::string& stream) const;
    uint64_t bind_past_steps(const std::string& stream) const;

    // Clears the polygon sidecar of every stream whose latest polygons
    // write was tagged with session_id (primary results are untouched —
    // they carry their own TTL). Returns the number of streams swept;
    // idempotent; an empty session_id is a no-op.
    size_t handle_session_end(const std::string& session_id);

    // Zone polygons currently held for a stream (0 = none) — the sweep's
    // observable, for tests and diagnostics. Sums across the stream's
    // layers (each layer owns its own polygon sidecar).
    size_t polygon_count(const std::string& stream_id) const;

    // Per-stream bake/strict observability for GetStreamStatus. Zeroes for
    // a stream that has never been through the bake site. The layer and
    // ingest fields aggregate every results_ key routed onto the display
    // stream (identity entry + stream_map cross-feeds), same as the bake
    // site's own layer collection.
    struct OverlayStreamStats {
        uint64_t bake_skips;      // shipped clean: no result / TTL expired
        uint64_t strict_locked;   // LOCK_FRESH + LOCK_NEWEST draws
        uint64_t strict_degraded; // cap expired or predicted hopeless
        uint64_t strict_skips;    // strict SKIP (no result for the stream)
        uint64_t stream_epoch;          // stream generation (restart bumps)
        uint64_t overlay_layer_count;   // layers currently held
        uint64_t overlay_late_commands; // app cmds rejected: frame past last-seen
        uint64_t overlay_epoch_rejects; // app cmds rejected: stale epoch
        uint64_t overlay_no_binding_drops; // platform events dropped (unbound)
    };
    void snapshot_stream_stats(const std::string& stream_name, OverlayStreamStats* out);

private:
    struct StreamResult {
        HalPostprocessResult result{};
        std::vector<OverlayPolygon> polygons;  // zone sidecar, independent channel
        std::string       polygons_session;    // owning session of the last polygons write ("" = untagged, never swept)
        uint32_t          ttl_ms = 0;           // from last event's metadata, 0 = unset
        uint64_t          last_frame_seq = 0;   // frame the result belongs to
        uint32_t          interval_ema_ms = 0;  // EMA of result inter-arrival
        std::chrono::steady_clock::time_point last_update_time;
        bool              valid = false;
    };

    // One drawable result layer per (session_id, source_id) identity.
    // Layers stack: each platform model and each app session draws over
    // the same frame instead of fighting for a single slot. Same identity
    // replaces its layer wholesale; the session sweep erases app layers
    // and clears session-tagged polygon writes on the survivors.
    struct Layer {
        StreamResult sr;             // primary result + polygon sidecar
        std::string  session_id;     // app session that created the layer ("" = platform)
        std::string  source_id;      // identity: app source (app_id) or "src/model_id"
        bool         is_app = false; // app layers: always admitted, suppress platform on draw
        bool         bound = false;  // app layer carried metadata frame_sequence
        uint64_t     bound_seq = 0;  // the frame it was bound to
        uint64_t     epoch = 0;      // stream epoch at ingest
    };

    void subscriber_loop();

    void draw_with_primitives(const HalPostprocessResult& result, HalFrameBuffer* frame,
                              bool draw_labels, bool draw_confidence, uint32_t box_thickness,
                              bool enable_face_blur, uint32_t face_blur_block_size);
    bool parse_json_result(const std::string& payload, const std::string& stream_id,
                           HalPostprocessResult* out);

    // Strict-gate core (P1-6). Runs under results_mu_ (passed as a
    // unique_lock so the bounded wait can release it). Locks the stream's
    // FIRST non-app layer — strict's single-source frame lock covers the
    // platform result only; app layers are NOT gated (they are
    // command-driven, TTL-governed) and draw alongside whatever the gate
    // returned. Returns the Layer to draw — null means no platform result
    // and the app half draws alone — and sets *skip_ttl when the layer's
    // result belongs to this exact frame (LOCK_*: the validity window
    // does not apply there). The wait can grow/erase the layer vector, so
    // the caller must re-find the layer after each wake, never hold the
    // pointer across one.
    Layer* strict_gate(std::unique_lock<std::mutex>& lock,
                       const std::string& stream_name,
                       HalFrameBuffer* frame,
                       uint32_t configured_cap_ms,
                       bool* skip_ttl);

    // Milestone logging (every 300 gated frames) on EVERY gate path —
    // a healthy all-locked run must be as observable as a degrading one.
    void log_strict_stats(const std::string& stream_name);

    AiOverlayConfig config_;

    // Protects mutable config fields (draw_labels, draw_confidence, box_thickness,
    // enable_face_blur)
    mutable std::mutex config_mu_;

    HalDrawConfig default_draw_cfg_{};

    std::atomic<bool> running_{false};
    std::thread       subscriber_thread_;

    mutable std::mutex ctx_mu_;
    void*              active_ctx_ = nullptr;

    mutable std::mutex results_mu_;
    // Per infer-stream stack of layers (key = stream as resolved from the
    // event; display-side lookups walk stream_map). Storage order is
    // arrival order; draw order is fixed (app newest-first, then platform).
    std::unordered_map<std::string, std::vector<Layer>> results_;

    // Stream generation counters (under results_mu_). Seeded lazily from
    // CLOCK_MONOTONIC us — never 0, so an unset epoch in an app event is
    // distinguishable — and bumped by note_stream_restart; daemon restart
    // reseeds for free. App events tagged with a stale epoch are rejected.
    std::unordered_map<std::string, uint64_t> stream_epochs_;

    // Last frame sequence each bake site reported (under results_mu_),
    // refreshed by every apply_overlay(current_seq > 0). The sequence is
    // the HAL frame's own counter (frame->sequence — the shared
    // media-context counter the FD publisher and ai-runtime re-export),
    // NOT the frame_router per-dispatch count: app commands carry the SDK
    // frame_sequence sourced from the same HAL counter, so the
    // ingest-side late-command judgement (cmd_seq vs this anchor) and the
    // draw-side binding window (current_seq vs bound_seq) must stay in
    // that one counter space. A per-stream router count diverges from it
    // on any multi-stream deployment (shared counter ticks once per
    // frontend callback for ALL streams) and would reject every bound
    // annotation as a late command. Cleared by note_stream_restart — a
    // pipeline rebuild can reset the HAL counter, and a stale anchor
    // would expire every newly bound layer against the wrong window.
    std::unordered_map<std::string, uint64_t> last_seen_seq_;

    // EMA of the HAL-counter distance between consecutive baked frames of
    // each display stream (under results_mu_). The shared media-context
    // counter ticks once per frontend callback for ALL streams, so on a
    // main@30+sub@30+third@15 device a main frame spans ~2.5 counter
    // steps — a frame-bind window expressed in raw steps (kFrameBindSlack
    // alone) would be sub-frame there and every latency-carrying bound
    // annotation would expire before drawing. The effective window is
    // kFrameBindSlack display frames converted to steps via this EMA;
    // 0 = not learned yet (single-frame history) → 1 step per frame.
    // Cleared together with the anchor by note_stream_restart: a rebuilt
    // pipeline re-learns its pace.
    std::unordered_map<std::string, double> steps_per_frame_;

    // Signalled whenever a result lands in subscriber_loop — the strict
    // gate waits on it (with results_mu_) and wakes as soon as the
    // frame's own result arrives, instead of polling.
    std::condition_variable result_cv_;

    // Per-stream bake/strict accounting (under results_mu_). Strict
    // counters tick in strict_gate; bake_skips tick at the two
    // ship-clean sites in apply_overlay (no result, TTL expired). Kept
    // per stream so GetStreamStatus and the milestone log report the
    // stream they actually belong to.
    struct StreamStats {
        uint64_t bake_skips = 0;
        uint64_t strict_locked = 0;   // LOCK_FRESH + LOCK_NEWEST draws
        uint64_t strict_degraded = 0; // cap expired or predicted hopeless
        uint64_t strict_skips = 0;    // SKIP (no result for the stream)
        uint64_t strict_wait_total_us = 0;
        uint32_t strict_wait_samples = 0;
        uint64_t gated_frame_count = 0; // frames through the strict gate
        uint64_t late_commands = 0;       // app events rejected: frame past last-seen
        uint64_t epoch_rejects = 0;       // app events rejected: stale stream_epoch
        uint64_t no_binding_drops = 0;    // platform events dropped (unbound, no legacy)
    };
    std::unordered_map<std::string, StreamStats> stream_stats_;
    std::chrono::steady_clock::time_point last_degrade_log_time_;
};
