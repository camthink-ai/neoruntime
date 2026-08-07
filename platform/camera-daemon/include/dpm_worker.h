/**
 * @file dpm_worker.h
 * @brief Dynamic Privacy Mask (DPM) worker — detection + semantic segmentation.
 *
 * DECOUPLED from the static privacy-mask blender. Runs one or more inference
 * sessions on a dedicated thread and publishes a per-frame **bytemask** — a
 * single frame-resolution silhouette mask aggregating every selected target:
 *   - person    -> linknet semantic segmentation -> real per-pixel silhouette
 *   - vehicle / face / license_plate -> detection bbox -> filled rect OR'd in
 * (no segmentation model exists for the latter three; a tight box is acceptable.)
 *
 * The pre-encoder frontend lambda (camera_daemon.cpp) consumes get_latest() and
 * bakes the bytemask onto the live encoder-input frame via HAL_DRAW_OPS — a pure
 * pixel-bake with its OWN style (mosaic/blur/overlay + dpm_color), completely
 * independent of the static medialib blender which keeps owning the static
 * polygons + the static color. Toggling one never disturbs the other; the two
 * never share state.
 *
 * ## Clean-capture input model (fix: inference no longer feeds on masked frames)
 *
 * The bake happens IN-PLACE on the shared DMA-BUF encoder-input frame, on the
 * GStreamer streaming thread, BEFORE FrameRouter routes it to subscribers. So a
 * normal downstream subscriber would always see an ALREADY-MASKED frame → the
 * mask would feed back into its own detection (the person it just hid). To break
 * that feedback loop the worker is NOT a FrameRouter subscriber. Instead the
 * frontend lambda calls offer_frame() SYNCHRONOUSLY on the streaming thread,
 * BEFORE the bake, while the frame is still clean. offer_frame() DSP-resizes
 * the clean frame into each model's ping-pong input buffer (the exact 4K→model
 * resize the inference already did — now relocated pre-bake), then signals the
 * worker. The worker runs inference on the buffered clean input and publishes.
 * This is the "Clean Frame Path" layer of the 3-layer design: inference + mask
 * state run only on clean data; the bake is purely a per-stream render stage.
 *
 * Concurrency (2-buffer ping-pong, drop-on-pending — race-free):
 *   - offer_frame (streaming thread): lock offer_mu_; if a capture is still
 *     pending (offer_seq_ != taken_seq_) the worker hasn't consumed the last
 *     capture yet → DROP (return), keeping no in-flight buffer for the streaming
 *     thread. Otherwise resize frame→session.input_fb[session.write_idx] for
 *     every session, flip each write_idx, ++offer_seq_, notify.
 *   - worker_loop (DPM thread): wait until offer_seq_ != taken_seq_ || !running;
 *     under offer_mu_ take each session's read_idx = write_idx^1 (the just-written
 *     buffer), set taken_seq_ = offer_seq_, unlock; run inference on those buffers.
 *   - Invariant: while the worker holds read_idx across inference, the next write
 *     goes to write_idx (= read_idx^1) — a DIFFERENT buffer — and further writes
 *     drop until the worker takes again. So no buffer is ever written while read.
 *
 * Architecture (see /root/.claude/plans/jiggly-napping-pretzel.md):
 *   streaming thread --offer_frame(clean frame)--> resize into ping-pong model
 *                                                            input buffers
 *                                                            (drop-on-pending)
 *     worker thread: wait -> take buffers -> for each active spec:
 *       tensor_from_frame -> run -> postproc
 *       is_seg  -> threshold linknet mask (cid==1||cid>=128) -> upscale -> OR into bytemask
 *       !is_seg -> keep_labels filter -> fill bbox rects -> OR into bytemask
 *     -> merge occupied bytemask into mosaic_cells (run-length merge, no coverage gap)
 *     -> DpmMaskState{mask, mosaic_cells} (mutex-protected shared_ptr)
 *   Frontend lambda: get_latest() -> overlay=one draw_mask(mask);
 *                    mosaic/blur=iterate mosaic_cells (camera_daemon.cpp)
 *
 * Inference NEVER runs on the streaming/frontend thread (would stall it). The
 * streaming thread does only the proven 4K→model DSP resize (relocated from the
 * worker); the frontend lambda does only an O(1) shared_ptr load + a single
 * draw_mask or bounded draw_mosaic calls. Upscaling the seg mask to frame
 * resolution happens on the worker thread, never the hot path.
 *
 * Reference: hal_v2/examples/ai_example_v2/ai_example_v2.cpp — stage1 detection
 * AND the linknet segmentation render path (cid==1||cid>=128 threshold), both
 * re-expressed via HAL_INFERENCE_OPS + HAL_POSTPROCESS_OPS + HAL_DSP_OPS +
 * HAL_FRAME_BUFFER_OPS.
 */

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include "common/hal_buffer.h"
#include "dsp/hal_dsp.h"
#include "media/hal_media.h"
#include "model/hal_inference.h"
#include "model/hal_postprocess.h"
}

/**
 * @brief Immutable bytemask snapshot published by the worker, consumed by the
 *        frontend draw lambda. `mask` is a frame_w*frame_h byte array (0 or 255)
 *        — the unified silhouette of every selected target (person seg + det
 *        bboxes OR'd together). The frontend bakes it in one draw_mask (overlay)
 *        or iterates the worker-precomputed mosaic_cells (mosaic/blur).
 */
struct DpmMaskState {
    uint32_t frame_w = 0;  // bytemask width  (== main-stream frame width)
    uint32_t frame_h = 0;  // bytemask height (== main-stream frame height)
    std::vector<uint8_t> mask;  // frame_w*frame_h, 0 or 255 (unified silhouette)

    /** One occupied mosaic cell (frame pixels). The worker merges the bytemask
     *  into the minimum set of rectangles (run-length merge) so the frontend
     *  mosaic/blur path makes a bounded number of draw_mosaic calls instead of
     *  per-pixel work, with NO coverage gap. */
    struct Cell {
        int32_t x = 0;
        int32_t y = 0;
        uint32_t w = 0;
        uint32_t h = 0;
    };
    std::vector<Cell> mosaic_cells;

    int64_t ts_ns = 0;
    bool restricted = false;  // thermal/NPU restriction — frontend should skip drawing
};

/**
 * @brief Owns inference sessions and a worker thread. Multi-spec: one session per
 *        DetectorSpec (e.g. linknet seg for person, coco det for vehicle, plate,
 *        face). Per-spec init is GRACEFUL — a missing/unsupported HEF is skipped
 *        with a warning; start() succeeds even with zero loaded models (idle
 *        mode: worker runs, no inference, publishes no mask — used when DPM is
 *        enabled with an empty label set). If the person seg HEF is absent, a
 *        coco detection spec with keep_labels={person} serves as a bbox fallback.
 *
 * Thread model:
 *   - start()/stop() called from the RPC thread (set_privacy_mask_config).
 *   - offer_frame() called on the GStreamer streaming thread (clean frame, pre-bake).
 *   - worker_loop() runs on the dedicated DPM thread (all HAL inference calls).
 *   - get_latest() called on the frontend thread (mutex-protected shared_ptr load).
 */
class DpmWorker {
public:
    /** @brief One model to load. A segmentation spec (is_seg=true) writes a real
     *         silhouette into the bytemask; a detection spec fills bbox rects. */
    struct DetectorSpec {
        std::string name;                     // telemetry label, e.g. "person_seg"/"coco"/"plate"/"face"
        std::string hef;                      // absolute path to .hef
        std::string post_json;                // optional vendor postproc config JSON path
        std::vector<std::string> keep_labels; // post-detection label allowlist (empty = keep all)
        bool is_seg = false;                  // true -> semantic segmentation (linknet), false -> detection
        // tiny_yolov4 license-plate detector emits a raw uint16 YOLO grid (NO
        // on-chip NMS) that the HAL NMS postproc cannot decode. When true,
        // init_sessions skips postproc creation and run_detector decodes the raw
        // output tensors itself (decode_plate_grid), bypassing the postproc path
        // entirely. Decoder constants' source of truth:
        // ne503-aipc-apps/showcases/parking-lot/parking_lot/config.py:135-140.
        bool is_grid_det = false;
        // Plan-A person gate: when a segmentation spec is loaded AND this label is
        // non-empty, detections matching `gate_label` are written to a separate gate
        // mask (the union of their boxes) instead of the final bytemask. The worker
        // then ANDs the seg silhouette with that gate mask — keeping linknet's
        // contour precision *inside* person boxes and dropping the ~47% of generic-
        // foreground false positives *outside* them. Empty -> det bbox fills the
        // final mask directly (also the graceful fallback when no seg spec is loaded).
        std::string gate_label;
    };

    struct Config {
        // HAL ops tables (resolved by HalLoader from the monolithic libaipc_hal.so).
        // (Clean-capture means the worker no longer needs a FrameRouter reference:
        // the frontend lambda calls offer_frame() directly on the streaming thread.)
        HalInferenceOps* infer_ops = nullptr;
        HalPostprocessOps* post_ops = nullptr;
        HalDspOps* dsp_ops = nullptr;
        HalFrameBufferOps* fb_ops = nullptr;

        // Specs to run. Only the labels the user selected should map to specs
        // (built by camera_daemon start_dpm_worker) so NPU is not wasted on unused models.
        // EMPTY is valid → idle worker (DPM armed, no targets, publishes no mask).
        std::vector<DetectorSpec> detectors;

        // Behaviour.
        uint32_t max_rois = 32;          // detection max_detections cap (NMS bound)
        uint32_t target_fps = 12;        // clean-capture throttle
        uint32_t stale_ms = 500;         // clear mask after this long w/o detection (hysteresis)
        uint32_t mosaic_stride = 16;     // mosaic cell side (px); larger = fewer cells, coarser
        uint32_t max_mosaic_cells = 256; // safety cap on merged mosaic rects (run-merge keeps it far lower)
    };

    DpmWorker();
    ~DpmWorker();

    DpmWorker(const DpmWorker&) = delete;
    DpmWorker& operator=(const DpmWorker&) = delete;

    /** @brief Create sessions + start worker. Empty detectors = idle mode (worker
     *         runs, no inference) — still returns true so DPM can be "armed" with
     *         no targets selected. Fails only if the HAL context can't init. */
    bool start(const Config& cfg);

    /** @brief Stop worker + drain any in-flight offer_frame() + destroy sessions. Idempotent. */
    void stop();

    bool is_running() const { return running_.load(std::memory_order_acquire); }

    /**
     * @brief Clean-capture entry (streaming thread, called BEFORE the per-stream
     *        bake). DSP-resizes the CLEAN frame into each model's ping-pong input
     *        buffer and wakes the worker. Self-throttled to target_fps; drops
     *        (no-op) if a capture is still pending (worker hasn't consumed the
     *        last) or if the worker is stopped. Never blocks the streaming thread
     *        beyond the bounded resize(s).
     */
    void offer_frame(const HalFrameBuffer* frame);

    /**
     * @brief Latest bytemask snapshot (or nullptr if none). Hot path — called from
     *        the frontend frame callback. Caller keeps the shared_ptr alive across
     *        the draw calls.
     */
    std::shared_ptr<const DpmMaskState> get_latest() const;

private:
    /** @brief One loaded model (spec + live HAL handles + ping-pong clean-capture
     *         input buffers at model-input dims). */
    struct DetectorSession {
        DetectorSpec spec;
        HalInferenceSession* session = nullptr;
        HalPostprocessSession* post = nullptr;
        // Ping-pong model-input DMABUFs at model-input dims (format = input_format:
        // NV12 for NV12-native models, RGB24 for packed-RGB models like the plate
        // tiny_yolov4). offer_frame() (streaming thread) writes [write_idx];
        // worker_loop (DPM thread) reads [read_idx].
        HalFrameBuffer* input_fb[2] = {nullptr, nullptr};
        // Ping-pong NV12 STAGING buffers, ONLY for RGB-input sessions (input_format
        // != NV12). DSP resize needs matching src/dst formats, but the clean main
        // frame is NV12 while the model wants RGB24 — so offer_frame() resizes
        // NV12 frame -> staging_nv12 (NV12->NV12), then convert_format staging_nv12
        // -> input_fb (NV12->RGB24). Null for NV12-native sessions (no conversion).
        // Pattern lifted from hal_v2/examples/lpr_example_v2 (the proven RGB-model
        // feeding path). NV12 requires even W/H; allocated at model dims.
        HalFrameBuffer* staging_nv12[2] = {nullptr, nullptr};
        uint32_t input_w = 0;
        uint32_t input_h = 0;
        HalPixelFormat input_format = HAL_PIX_FMT_NV12;
        uint32_t num_outputs = 0;
        int write_idx = 0;  // next buffer offer_frame() fills (streaming thread)
        int read_idx = 0;   // buffer worker_loop is consuming (DPM thread)
    };

    void worker_loop();
    bool init_sessions();
    void destroy_sessions();

    // Run one detection spec on its captured clean input buffer. For each kept
    // detection, if `gate_mask` is non-null AND the detection label matches
    // `ds.spec.gate_label`, its filled bbox goes into *gate_mask (the person-gate
    // union, ANDed against the seg silhouette by the worker); otherwise it is OR'd
    // into `mask` (the final frame-resolution bytemask). fw/fh are the captured
    // clean-frame dims (== main-stream resolution).
    bool run_detector(DetectorSession& ds, std::vector<uint8_t>& mask,
                      std::vector<uint8_t>* gate_mask,
                      uint32_t fw, uint32_t fh);

    // Run one segmentation spec on its captured clean input buffer; threshold the
    // linknet class mask (cid==1 || cid>=128) and OR the nearest-neighbor-upscaled
    // silhouette into the frame-resolution bytemask. fw/fh are the captured dims.
    bool run_segmenter(DetectorSession& ds, std::vector<uint8_t>& mask,
                       uint32_t fw, uint32_t fh);

    // Publish a new state (shared_ptr store) and update hysteresis bookkeeping.
    void publish(std::shared_ptr<DpmMaskState> state);

    Config cfg_;

    // HAL ops (non-owning; lifetime = daemon lifetime).
    HalInferenceOps* infer_ops_ = nullptr;
    HalPostprocessOps* post_ops_ = nullptr;
    HalDspOps* dsp_ops_ = nullptr;
    HalFrameBufferOps* fb_ops_ = nullptr;

    // DSP context (clean-frame → model-input resize, on the streaming thread).
    void* dsp_ctx_ = nullptr;

    // Loaded models (subset of cfg_.detectors that initialized successfully).
    // Empty = idle mode (worker runs, no inference).
    std::vector<DetectorSession> detectors_;

    // Worker thread + clean-capture hand-off. offer_mu_ guards the ping-pong
    // buffers AND offer_seq_/taken_seq_ (the drop-on-pending latch). stop() locks
    // offer_mu_ to drain any in-flight offer_frame() before destroying buffers.
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex offer_mu_;
    std::condition_variable cv_;
    uint64_t offer_seq_ = 0;   // bumped each time offer_frame() captures a clean frame
    uint64_t taken_seq_ = 0;   // bumped to offer_seq_ when the worker takes a capture
    uint32_t cap_w_ = 0;       // captured clean-frame dims (set by offer_frame)
    uint32_t cap_h_ = 0;

    // offer_frame throttle (streaming thread, lock-free): skip if the last clean
    // capture was less than (1/target_fps) ago.
    std::atomic<int64_t> last_offer_ns_{0};

    // Published state (mutex-protected shared_ptr; get_latest is the frontend hot
    // path, ~30fps — lock cost negligible, avoids C++20-deprecated std::atomic_store).
    mutable std::shared_ptr<const DpmMaskState> latest_;
    mutable std::mutex latest_mu_;

    // Temporal hysteresis (worker-only): when detections vanish, keep publishing
    // the last non-empty state briefly so masks don't flicker off on a single
    // dropped detection; clear after stale_ms.
    std::shared_ptr<const DpmMaskState> last_nonempty_;
    int64_t last_det_ns_ = 0;

    // Telemetry (worker-only).
    uint64_t cycle_cnt_ = 0;
};
