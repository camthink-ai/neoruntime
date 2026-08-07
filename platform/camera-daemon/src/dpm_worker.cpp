/**
 * @file dpm_worker.cpp
 * @brief Dynamic Privacy Mask (DPM) worker — detection + semantic segmentation.
 *
 * Runs on a dedicated thread (never the streaming/frontend thread). The frontend
 * frame callback (camera_daemon.cpp) calls offer_frame() on the GStreamer
 * streaming thread, BEFORE the per-stream bake, while the frame is still CLEAN.
 * offer_frame() DSP-resizes the clean frame into each model's ping-pong input
 * buffer (drop-on-pending) and wakes the worker. The worker runs every loaded
 * model on its buffered clean input and OR's the result into a single
 * frame-resolution bytemask:
 *   - segmentation specs (linknet person/foreground) -> real per-pixel silhouette
 *   - detection specs (yolov8n COCO for vehicle, plate, face) -> filled bbox rects
 * The frontend callback then calls get_latest() per stream and bakes the bytemask
 * onto the live pre-encode frame via HAL_DRAW_OPS — overlay = one draw_mask(mask);
 * mosaic/blur = iterate the worker-precomputed mosaic_cells. Independent of the
 * static medialib blender, and — critically — inference never feeds on a frame
 * it has already masked (clean-capture breaks the feedback loop).
 *
 * Reference: hal_v2/examples/ai_example_v2/ai_example_v2.cpp — stage1 detection
 * AND the linknet segmentation render path (cid==1||cid>=128 threshold), both
 * re-expressed via HAL_INFERENCE_OPS + HAL_POSTPROCESS_OPS + HAL_DSP_OPS +
 * HAL_FRAME_BUFFER_OPS.
 */

#include "dpm_worker.h"

extern "C" {
#include "hal_log.h"
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace {

// Minimum detection confidence to keep as a mask ROI.
constexpr float kDetConfFloor = 0.30f;

// linknet_mbv1_ss_dpm outputs either a class-id mask (0=bg, 1=fg) or a uint8
// probability-like mask (0..255, fg near 255). Foreground = cid==1 OR cid>=128.
// (Verbatim semantics from ai_example_v2.cpp:1525-1529.)
constexpr uint8_t kSegFgClassId = 1;
constexpr uint8_t kSegProbFloor = 128;

int64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

const char* pixfmt_name(HalPixelFormat fmt) {
    switch (fmt) {
        case HAL_PIX_FMT_RGB24: return "RGB24";
        case HAL_PIX_FMT_BGR24: return "BGR24";
        case HAL_PIX_FMT_GRAY8: return "GRAY8";
        case HAL_PIX_FMT_NV12:
        default: return "NV12";
    }
}

// Extract model input H/W and the host pixel format from get_model_info. DPM
// models in the field may be compiled for either NV12 blobs (W*H*3/2) or packed
// RGB tensors (W*H*3). The clean-capture buffer must match that byte size before
// tensor_from_frame() hands it to HailoRT.
bool model_input_info(HalInferenceOps* ops, HalInferenceSession* s,
                      uint32_t& w, uint32_t& h, HalPixelFormat& fmt) {
    if (!ops || !s) return false;
    HalModelInfo mi{};
    if (ops->get_model_info(s, &mi) != 0 || mi.num_inputs == 0) return false;
    for (uint32_t i = 0; i < mi.num_inputs; ++i) {
        const HalModelTensorInfo& in = mi.inputs[i];
        if (in.ndim >= 4) {
            const int32_t hh = in.shape[1];
            const int32_t ww = in.shape[2];
            if (hh > 0 && ww > 0) {
                w = static_cast<uint32_t>(ww);
                h = static_cast<uint32_t>(hh);
                const uint64_t pixels = static_cast<uint64_t>(w) * h;
                const uint64_t bytes = static_cast<uint64_t>(in.byte_size);
                if (in.is_nv12 || bytes == pixels * 3ULL / 2ULL) {
                    fmt = HAL_PIX_FMT_NV12;
                } else if (bytes == pixels * 3ULL) {
                    fmt = HAL_PIX_FMT_RGB24;
                } else if (bytes == pixels) {
                    fmt = HAL_PIX_FMT_GRAY8;
                } else {
                    HAL_LOG_WARNING("DPM: unsupported model input byte_size=%u for %ux%u",
                                    in.byte_size, w, h);
                    return false;
                }
                return true;
            }
        }
    }
    return false;
}

// True if any byte of the bytemask is set (early-exit on first non-zero).
bool mask_nonempty(const std::vector<uint8_t>& m) {
    return std::find_if(m.begin(), m.end(),
                        [](uint8_t v) { return v != 0; }) != m.end();
}

// Scan the bytemask into occupied cells via run-length MERGE so EVERY occupied
// pixel ends up inside some emitted rect — NO coverage gap. (The previous
// implementation capped at max_cells and silently truncated, leaving parts of a
// silhouette unmasked — a privacy hole for large/tall subjects.)
//
// Two passes (cell coords, stride-coarsened):
//   1. per cell-row: collapse consecutive occupied cells into horizontal runs;
//   2. greedy vertical merge: a run in row R absorbs into a rect whose bottom is
//      R-1 and whose x-range touches it; else it starts a new rect.
// Coverage is complete (every run lands in exactly one rect). A generous
// max_cells backstop only triggers for pathologically fragmented masks and is
// logged (never silent). overlay mode ignores the result and draws the full
// bytemask in one draw_mask.
void build_mosaic_cells(const std::vector<uint8_t>& mask, uint32_t fw, uint32_t fh,
                        uint32_t stride, size_t max_cells,
                        std::vector<DpmMaskState::Cell>& out) {
    out.clear();
    if (fw == 0 || fh == 0 || stride == 0) return;
    const uint32_t cw = (fw + stride - 1) / stride;
    const uint32_t ch = (fh + stride - 1) / stride;

    // Step 1: per cell-row horizontal runs (cell coords; x1 inclusive).
    struct Run { uint32_t row; uint32_t x0; uint32_t x1; };
    std::vector<Run> runs;
    runs.reserve(static_cast<size_t>(ch) * 4);
    for (uint32_t cy = 0; cy < ch; ++cy) {
        const uint32_t y0 = cy * stride;
        const uint32_t y1 = std::min(y0 + stride, fh);
        uint32_t run_x0 = 0;
        bool in_run = false;
        for (uint32_t cx = 0; cx < cw; ++cx) {
            const uint32_t xx0 = cx * stride;
            const uint32_t xx1 = std::min(xx0 + stride, fw);
            bool occupied = false;
            for (uint32_t yy = y0; yy < y1 && !occupied; ++yy) {
                const uint8_t* row = &mask[static_cast<size_t>(yy) * fw];
                for (uint32_t xx = xx0; xx < xx1; ++xx) {
                    if (row[xx]) { occupied = true; break; }
                }
            }
            if (occupied) {
                if (!in_run) { run_x0 = cx; in_run = true; }
            } else if (in_run) {
                runs.push_back({cy, run_x0, cx - 1});
                in_run = false;
            }
        }
        if (in_run) runs.push_back({cy, run_x0, cw - 1});
    }
    if (runs.empty()) return;

    // Step 2: greedy downward merge into rects (cell coords; x1 inclusive).
    struct Rect { uint32_t row0; uint32_t row1; uint32_t x0; uint32_t x1; };
    std::vector<Rect> rects;
    rects.reserve(runs.size());
    for (const auto& r : runs) {
        bool merged = false;
        for (auto& rc : rects) {
            if (rc.row1 + 1 == r.row &&
                r.x0 <= rc.x1 + 1 && r.x1 + 1 >= rc.x0) {
                rc.row1 = r.row;
                rc.x0 = std::min(rc.x0, r.x0);
                rc.x1 = std::max(rc.x1, r.x1);
                merged = true;
                break;
            }
        }
        if (!merged) rects.push_back({r.row, r.row, r.x0, r.x1});
    }

    // Step 3: emit pixel Cells (inclusive cell coords → pixel rects, clamped).
    out.reserve(rects.size());
    for (const auto& rc : rects) {
        if (out.size() >= max_cells) {
            HAL_LOG_WARNING("DPM: mosaic cells capped at %zu (mask very fragmented)",
                            max_cells);
            break;
        }
        DpmMaskState::Cell c;
        c.x = static_cast<int32_t>(rc.x0 * stride);
        c.y = static_cast<int32_t>(rc.row0 * stride);
        const uint32_t x_end = std::min((rc.x1 + 1) * stride, fw);
        const uint32_t y_end = std::min((rc.row1 + 1) * stride, fh);
        c.w = x_end - static_cast<uint32_t>(c.x);
        c.h = y_end - static_cast<uint32_t>(c.y);
        out.push_back(c);
    }
}

// ---------------------------------------------------------------------------
// tiny_yolov4 license-plate grid decoder (raw uint16 YOLO grid, NO on-chip NMS)
// ---------------------------------------------------------------------------
// tiny_yolov4_license_plates.hef emits two raw uint16 YOLO grids that the HAL
// NMS postproc cannot decode. This is a faithful C++ port of the proven Python
// parse_yolo_grid (ne503-aipc-apps/showcases/parking-lot/parking_lot/postprocess.py:238 and
// ne503-aipc-apps/showcases/model-showcase/main.py:2927) — identical anchors, thresholds, NMS.
// Source of truth for anchors: ne503-aipc-apps/showcases/parking-lot/parking_lot/config.py:135-140.
struct Yv4GridSpec {
    uint32_t grid_h;
    uint32_t grid_w;
    int anchors[3][2];  // 3 anchors (aw, ah) in input pixels
};
constexpr uint32_t kYv4InputSide = 416;      // model input H/W (px)
constexpr float kYv4ObjFloor = 0.30f;        // objectness gate (Python: obj<0.3 skip)
constexpr float kYv4ConfFloor = 0.20f;       // obj*cls gate (Python: conf<0.2 skip)
constexpr float kYv4NmsThreshold = 0.45f;    // greedy NMS IoU (Python: >0.45 suppress)
constexpr size_t kYv4MaxKeep = 32;           // top-N after NMS (Python: keep[:32])
// Two output grids. 13x13 needs 13*13*3*6 = 3042 uint16 elements; 26x26 needs
// 26*26*3*6 = 12168. The 4x size gap makes largest-fit matching unambiguous.
const Yv4GridSpec kYv4Grids[2] = {
    {13, 13, {{81, 82}, {135, 169}, {344, 319}}},
    {26, 26, {{23, 27}, {37, 58}, {81, 82}}},
};

// IoU of two axis-aligned boxes in normalized [0..1] coords (x,y,w,h).
float bbox_iou_norm(float ax, float ay, float aw, float ah,
                    float bx, float by, float bw, float bh) {
    const float ix0 = std::max(ax, bx);
    const float iy0 = std::max(ay, by);
    const float ix1 = std::min(ax + aw, bx + bw);
    const float iy1 = std::min(ay + ah, by + bh);
    const float iw = std::max(0.0f, ix1 - ix0);
    const float ih = std::max(0.0f, iy1 - iy0);
    const float inter = iw * ih;
    const float uni = aw * ah + bw * bh - inter;
    return uni > 0.0f ? inter / uni : 0.0f;
}

// Fill a normalized bbox (x,y,w,h in [0..1]) as a solid 255 rect into the
// frame-resolution bytemask, clamped to frame bounds. Shared by the postproc
// detection path and the grid path (DRY: was inlined in run_detector).
void fill_bbox_rect(std::vector<uint8_t>& mask, uint32_t fw, uint32_t fh,
                    float nx, float ny, float nw, float nh) {
    int32_t x0 = static_cast<int32_t>(nx * static_cast<float>(fw));
    int32_t y0 = static_cast<int32_t>(ny * static_cast<float>(fh));
    int32_t x1 = static_cast<int32_t>((nx + nw) * static_cast<float>(fw));
    int32_t y1 = static_cast<int32_t>((ny + nh) * static_cast<float>(fh));
    x0 = std::max(0, std::min(x0, static_cast<int32_t>(fw)));
    y0 = std::max(0, std::min(y0, static_cast<int32_t>(fh)));
    x1 = std::max(0, std::min(x1, static_cast<int32_t>(fw)));
    y1 = std::max(0, std::min(y1, static_cast<int32_t>(fh)));
    for (int32_t yy = y0; yy < y1; ++yy) {
        uint8_t* row = &mask[static_cast<size_t>(yy) * fw];
        for (int32_t xx = x0; xx < x1; ++xx) row[xx] = 255;
    }
}

// Decode tiny_yolov4 uint16 grid outputs into normalized bboxes and OR them as
// filled rects into the bytemask. Returns the number of boxes drawn.
//
// Each output tensor's raw bytes are read as little-endian uint16 (byte_size/2
// elements) — equivalent to numpy's arr.view(np.uint16), regardless of whether
// HAL labels the dtype uint8 or uint16. Each tensor is matched to the grid with
// the LARGEST element count that fits (more robust than the Python, which
// assumes raw[0]=13x13; here output ORDER does not matter). Layout is C-order
// [grid_h, grid_w, 3 anchors, 6 vals]; flat idx [gy,gx,a,v] = ((gy*gw+gx)*3+a)*6+v.
int decode_plate_grid(const HalTensor* outs, uint32_t num_outputs,
                      std::vector<uint8_t>& mask, uint32_t fw, uint32_t fh) {
    struct Box { float x, y, w, h, conf; };
    std::vector<Box> dets;

    // Diagnostics (throttled ~every 3s at 10Hz): output element counts, how many
    // grids matched, and the peak objectness / confidence across all cells. If
    // peak_obj ~= 0 the input pixels are garbage (color/feeding bug); if grids
    // matched but peak_obj < floor the thresholds or input are off; if peak_obj
    // >= floor the decoder is working and 0 boxes just means no plate in frame.
    int grids_matched = 0;
    float peak_obj = 0.0f;
    float peak_conf = 0.0f;
    uint32_t cells_above_obj = 0;
    char elems_desc[64] = {0};

    for (uint32_t t = 0; t < num_outputs; ++t) {
        if (!outs[t].data || outs[t].byte_size < 12) continue;  // need >= 1 anchor cell
        const size_t elems = static_cast<size_t>(outs[t].byte_size) / 2;  // uint16 count
        if (t == 0)
            std::snprintf(elems_desc, sizeof(elems_desc), "o0=%zu", elems);
        // Largest-fit grid match (13x13=3042, 26x26=12168 — unambiguous).
        const Yv4GridSpec* g = nullptr;
        size_t best_need = 0;
        for (const auto& cand : kYv4Grids) {
            const size_t need =
                static_cast<size_t>(cand.grid_h) * cand.grid_w * 3 * 6;
            if (need <= elems && need > best_need) {
                best_need = need;
                g = &cand;
            }
        }
        if (!g) continue;
        ++grids_matched;
        const uint32_t gh = g->grid_h;
        const uint32_t gw = g->grid_w;
        const uint16_t* p = static_cast<const uint16_t*>(outs[t].data);

        for (uint32_t gy = 0; gy < gh; ++gy) {
            for (uint32_t gx = 0; gx < gw; ++gx) {
                for (int a = 0; a < 3; ++a) {
                    const size_t base = ((static_cast<size_t>(gy) * gw + gx) * 3 + a) * 6;
                    if (base + 5 >= elems) break;  // buffer bound (shouldn't trigger)
                    const float tx = p[base + 0] / 65535.0f;
                    const float ty = p[base + 1] / 65535.0f;
                    const float tw = p[base + 2] / 65535.0f;
                    const float th = p[base + 3] / 65535.0f;
                    const float obj = p[base + 4] / 65535.0f;
                    const float cls = p[base + 5] / 65535.0f;
                    if (obj > peak_obj) peak_obj = obj;
                    const float conf = obj * cls;
                    if (conf > peak_conf) peak_conf = conf;
                    if (obj < kYv4ObjFloor) continue;
                    ++cells_above_obj;
                    const float aw = static_cast<float>(g->anchors[a][0]);
                    const float ah = static_cast<float>(g->anchors[a][1]);
                    const float cx = (static_cast<float>(gx) + tx) / static_cast<float>(gw);
                    const float cy = (static_cast<float>(gy) + ty) / static_cast<float>(gh);
                    const float w = aw * tw / static_cast<float>(kYv4InputSide);
                    const float h = ah * th / static_cast<float>(kYv4InputSide);
                    if (conf < kYv4ConfFloor) continue;
                    dets.push_back({cx - w * 0.5f, cy - h * 0.5f, w, h, conf});
                }
            }
        }
    }

    // Health heartbeat (~every 10s at 10Hz): the only signal that distinguishes
    // "decoder alive, no plate in frame" from "decoder stalled". peak_obj~=0 would
    // mean garbage input (color/feeding bug); grids matched but peak_obj<floor
    // means thresholds/input are off; peak_obj>=floor means the decoder is working
    // and 0 boxes just means no plate in frame.
    {
        static uint32_t s_diag = 0;
        if ((s_diag++ % 100u) == 0u) {
            HAL_LOG_INFO("DPM[plate]: grid diag %s grids=%d peak_obj=%.3f peak_conf=%.3f cells_obj=%u cand=%zu",
                         elems_desc, grids_matched, peak_obj, peak_conf,
                         cells_above_obj, dets.size());
        }
    }

    // Greedy NMS by confidence (desc), IoU>kYv4NmsThreshold suppresses. Top-32.
    std::sort(dets.begin(), dets.end(),
              [](const Box& a, const Box& b) { return a.conf > b.conf; });
    std::vector<Box> kept;
    kept.reserve(std::min(dets.size(), kYv4MaxKeep));
    for (const auto& d : dets) {
        bool suppressed = false;
        for (const auto& k : kept) {
            if (bbox_iou_norm(d.x, d.y, d.w, d.h, k.x, k.y, k.w, k.h) > kYv4NmsThreshold) {
                suppressed = true;
                break;
            }
        }
        if (suppressed) continue;
        kept.push_back(d);
        if (kept.size() >= kYv4MaxKeep) break;
    }
    for (const auto& k : kept) fill_bbox_rect(mask, fw, fh, k.x, k.y, k.w, k.h);
    return static_cast<int>(kept.size());
}

}  // namespace

DpmWorker::DpmWorker() = default;

DpmWorker::~DpmWorker() { stop(); }

bool DpmWorker::start(const Config& cfg) {
    if (running_.load(std::memory_order_acquire)) return true;

    if (!cfg.infer_ops || !cfg.post_ops || !cfg.dsp_ops || !cfg.fb_ops) {
        return false;
    }

    cfg_ = cfg;

    if (!init_sessions()) {
        destroy_sessions();
        return false;
    }

    running_.store(true, std::memory_order_release);
    worker_ = std::thread(&DpmWorker::worker_loop, this);
    return true;
}

void DpmWorker::stop() {
    if (!running_.load(std::memory_order_acquire)) return;
    running_.store(false, std::memory_order_release);
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    // Drain any in-flight offer_frame(): acquiring offer_mu_ waits for a resize
    // currently under way on the streaming thread to finish BEFORE we destroy the
    // buffers it wrote. Any offer_frame() that runs after this point checks
    // running_ (==false) under offer_mu_ and returns without touching buffers.
    {
        std::lock_guard<std::mutex> lk(offer_mu_);
    }

    destroy_sessions();

    {
        std::lock_guard<std::mutex> lk(latest_mu_);
        latest_.reset();
    }
    last_nonempty_.reset();
    last_det_ns_ = 0;
    cycle_cnt_ = 0;
    offer_seq_ = taken_seq_ = 0;
    cap_w_ = cap_h_ = 0;
    last_offer_ns_.store(0, std::memory_order_relaxed);
}

bool DpmWorker::init_sessions() {
    infer_ops_ = cfg_.infer_ops;
    post_ops_ = cfg_.post_ops;
    dsp_ops_ = cfg_.dsp_ops;
    fb_ops_ = cfg_.fb_ops;

    // DSP context (clean-frame → model-input resize, on the streaming thread).
    HalDspConfig dcfg{};
    dcfg.device_priority = 0;
    if (dsp_ops_->init(&dcfg, &dsp_ctx_) != 0 || !dsp_ctx_) return false;

    // Graceful per-spec init: a missing/unsupported HEF is logged + skipped; the
    // worker still ships the models that DID load. start() succeeds even if every
    // spec fails (idle mode) — DPM can be "armed" with no targets selected.
    for (const auto& spec : cfg_.detectors) {
        DetectorSession ds;
        ds.spec = spec;

        HalInferenceConfig icfg{};
        std::snprintf(icfg.model_path, sizeof(icfg.model_path), "%s",
                      spec.hef.c_str());
        icfg.batch_size = 1;
        icfg.timeout_ms = 1000;
        icfg.use_dma = true;
        icfg.platform_config = nullptr;
        icfg.platform_data = nullptr;
        icfg.runtime = nullptr;
        icfg.preprocess = {};
        icfg.preprocess.color = HAL_PREPROCESS_COLOR_NONE;
        ds.session = infer_ops_->create(&icfg);
        if (!ds.session) {
            HAL_LOG_WARNING("DPM[%s]: HEF load failed (%s) — skipping model",
                            spec.name.c_str(), spec.hef.c_str());
            continue;
        }

        HalModelInfo minfo{};
        if (infer_ops_->get_model_info(ds.session, &minfo) != 0 ||
            minfo.num_outputs == 0) {
            HAL_LOG_WARNING("DPM[%s]: get_model_info failed — skipping",
                            spec.name.c_str());
            infer_ops_->destroy(ds.session);
            continue;
        }
        ds.num_outputs = std::min<uint32_t>(minfo.num_outputs, HAL_MAX_TENSORS);

        if (!model_input_info(infer_ops_, ds.session, ds.input_w, ds.input_h, ds.input_format)) {
            HAL_LOG_WARNING("DPM[%s]: input dims/format unknown — skipping",
                            spec.name.c_str());
            infer_ops_->destroy(ds.session);
            continue;
        }

        // TWO model-input DMABUFs (ping-pong). offer_frame() (streaming
        // thread) writes [write_idx]; worker_loop (DPM thread) reads [read_idx].
        // pool_max_buffers is left at 0 → the HAL default (8): we only HOLD 2
        // ping-pong buffers long-term, but MediaLibraryBufferPool::init() rejects
        // a 1-buffer pool (returns MEDIA_LIBRARY_BUFFER_ALLOCATION_ERROR →
        // HAL_ERR_NO_MEM, observed rc=-2809). The proven ai_example_v2 path also
        // relies on the default pool size, so we match it.
        HalFrameBufferRequest req{};
        req.width = ds.input_w;
        req.height = ds.input_h;
        req.format = ds.input_format;
        req.mem_type = HAL_MEM_DMABUF;
        req.zero_initialize = false;
        req.priv = nullptr;
        bool alloc_ok = true;
        int alloc_rc = 0;
        for (int b = 0; b < 2; ++b) {
            alloc_rc = fb_ops_->request_frame_buffer(&req, &ds.input_fb[b]);
            if (alloc_rc != 0 || !ds.input_fb[b]) {
                alloc_ok = false;
                break;
            }
        }
        if (!alloc_ok) {
            HAL_LOG_WARNING("DPM[%s]: input buffer alloc failed (rc=%d, %ux%u %s DMABUF) — skipping",
                            spec.name.c_str(), alloc_rc, ds.input_w, ds.input_h,
                            pixfmt_name(ds.input_format));
            for (int b = 0; b < 2; ++b) {
                if (ds.input_fb[b]) { fb_ops_->release_frame_buffer(ds.input_fb[b]); ds.input_fb[b] = nullptr; }
            }
            infer_ops_->destroy(ds.session);
            continue;
        }

        // RGB-input sessions (e.g. plate tiny_yolov4, input_format=RGB24) cannot
        // be fed directly from the NV12 clean main frame — DSP resize requires
        // matching src/dst formats. Allocate a ping-pong NV12 STAGING buffer at
        // model dims; offer_frame() resizes NV12 frame -> staging_nv12, then
        // convert_format staging_nv12 -> input_fb (NV12->RGB24). NV12 needs even
        // W/H; the plate model is 416x416 (even). If alloc fails the session can
        // never receive a frame, so skip it wholesale (mirrors input_fb handling).
        if (ds.input_format != HAL_PIX_FMT_NV12) {
            if ((ds.input_w & 1u) || (ds.input_h & 1u)) {
                HAL_LOG_WARNING("DPM[%s]: RGB model with odd input dims %ux%u — NV12 staging needs even dims, skipping",
                                spec.name.c_str(), ds.input_w, ds.input_h);
                for (int b = 0; b < 2; ++b) {
                    if (ds.input_fb[b]) { fb_ops_->release_frame_buffer(ds.input_fb[b]); ds.input_fb[b] = nullptr; }
                }
                infer_ops_->destroy(ds.session);
                continue;
            }
            HalFrameBufferRequest sreq{};
            sreq.width = ds.input_w;
            sreq.height = ds.input_h;
            sreq.format = HAL_PIX_FMT_NV12;
            sreq.mem_type = HAL_MEM_DMABUF;
            sreq.zero_initialize = false;
            sreq.priv = nullptr;
            bool salloc_ok = true;
            int salloc_rc = 0;
            for (int b = 0; b < 2; ++b) {
                salloc_rc = fb_ops_->request_frame_buffer(&sreq, &ds.staging_nv12[b]);
                if (salloc_rc != 0 || !ds.staging_nv12[b]) {
                    salloc_ok = false;
                    break;
                }
            }
            if (!salloc_ok) {
                HAL_LOG_WARNING("DPM[%s]: NV12 staging buffer alloc failed (rc=%d, %ux%u) — skipping",
                                spec.name.c_str(), salloc_rc, ds.input_w, ds.input_h);
                for (int b = 0; b < 2; ++b) {
                    if (ds.input_fb[b]) { fb_ops_->release_frame_buffer(ds.input_fb[b]); ds.input_fb[b] = nullptr; }
                    if (ds.staging_nv12[b]) { fb_ops_->release_frame_buffer(ds.staging_nv12[b]); ds.staging_nv12[b] = nullptr; }
                }
                infer_ops_->destroy(ds.session);
                continue;
            }
        }

        // tiny_yolov4 grid-det specs decode raw uint16 output tensors themselves
        // (decode_plate_grid) — NO postproc session is created, ds.post stays null.
        // destroy_sessions already null-guards ds.post, so this is safe.
        if (spec.is_grid_det) {
            HAL_LOG_INFO("DPM[%s]: loaded (grid det, %s, input=%ux%u %s) — raw uint16 grid decode, no postproc",
                         spec.name.c_str(), spec.hef.c_str(),
                         ds.input_w, ds.input_h, pixfmt_name(ds.input_format));
            detectors_.push_back(std::move(ds));
            continue;
        }

        // Postproc session: SEGMENTATION for linknet (linknet_post is the default
        // Hailo-15 plugin for the SEG type), DETECTION otherwise. spec.post_json is
        // a FILE PATH (not inline JSON), so it MUST go to config_file — the HAL
        // reads the file into merged_vendor_json (hailo15_postprocess_impl.cpp:1053),
        // which populates p->labels for the d.label fallback. Putting a path in
        // config_json is silently ignored (it requires a '{' JSON-object prefix,
        // see str_has_json_object_prefix @ :1076) → labels stay empty → d.label
        // empty → keep_labels string-match never fires. Optional for both: when
        // null, vendor defaults apply.
        HalPostprocessConfig pcfg{};
        if (spec.is_seg) {
            pcfg.type = HAL_POST_TYPE_SEGMENTATION;
            pcfg.config.segmentation.confidence_threshold = 0.0f;  // threshold is cid-based (kSegFgClassId/kSegProbFloor)
            pcfg.config.segmentation.output_width = 0;   // 0 = same as model input
            pcfg.config.segmentation.output_height = 0;
            pcfg.config.segmentation.labels_file = nullptr;
            pcfg.config.segmentation.config_json = nullptr;
            pcfg.config.segmentation.config_file =
                spec.post_json.empty() ? nullptr : spec.post_json.c_str();
        } else {
            pcfg.type = HAL_POST_TYPE_DETECTION;
            pcfg.config.detection.confidence_threshold = kDetConfFloor;
            pcfg.config.detection.nms_threshold = 0.45f;
            pcfg.config.detection.max_detections = cfg_.max_rois;
            pcfg.config.detection.class_ids_filter = nullptr;
            pcfg.config.detection.num_class_ids_filter = 0;
            pcfg.config.detection.labels_file = nullptr;
            pcfg.config.detection.config_json = nullptr;
            pcfg.config.detection.config_file =
                spec.post_json.empty() ? nullptr : spec.post_json.c_str();
        }
        ds.post = post_ops_->create(&pcfg);
        if (!ds.post) {
            HAL_LOG_WARNING("DPM[%s]: postproc create failed — skipping",
                            spec.name.c_str());
            for (int b = 0; b < 2; ++b) {
                if (ds.input_fb[b]) { fb_ops_->release_frame_buffer(ds.input_fb[b]); ds.input_fb[b] = nullptr; }
                if (ds.staging_nv12[b]) { fb_ops_->release_frame_buffer(ds.staging_nv12[b]); ds.staging_nv12[b] = nullptr; }
            }
            infer_ops_->destroy(ds.session);
            continue;
        }

        HAL_LOG_INFO("DPM[%s]: loaded (%s, %s, input=%ux%u %s, keep=%zu labels)",
                     spec.name.c_str(), spec.hef.c_str(),
                     spec.is_seg ? "seg" : "det",
                     ds.input_w, ds.input_h, pixfmt_name(ds.input_format),
                     spec.keep_labels.size());
        detectors_.push_back(std::move(ds));
    }

    // Plan-A person gate: if a segmentation spec (linknet person_seg) loaded
    // successfully, mark every *filtered* detection spec that keeps "person" with
    // gate_label="person". Such specs stay alive — at runtime their person boxes
    // are written to a separate gate mask and ANDed against the seg silhouette,
    // confining linknet's contour to inside person boxes and dropping the generic-
    // foreground false positives (~47% of the frame) outside them. vehicle/face/
    // plate labels in the same spec still fill the final bytemask directly. When
    // no seg spec loaded, no gate_label is set, so person falls back to a pure bbox
    // (graceful). No spec is dropped here.
    bool seg_loaded = false;
    for (const auto& ds : detectors_) if (ds.spec.is_seg) { seg_loaded = true; break; }
    if (seg_loaded) {
        for (auto& ds : detectors_) {
            if (ds.spec.is_seg) continue;
            const auto& kl = ds.spec.keep_labels;
            const bool keeps_person =
                std::find(kl.begin(), kl.end(), std::string("person")) != kl.end();
            // keep-all specs (empty keep_labels, e.g. plate/face) cannot reliably
            // emit a "person" label, so they are not promoted to gate specs.
            if (keeps_person) {
                ds.spec.gate_label = "person";
                HAL_LOG_INFO("DPM[%s]: person boxes gate person_seg silhouette",
                             ds.spec.name.c_str());
            }
        }
    }

    if (detectors_.empty()) {
        // Idle mode: DPM armed with no targets. Valid state — worker runs, no
        // inference, publishes no mask (get_latest() == nullptr → no bake).
        HAL_LOG_INFO("DPM: idle mode (no detectors loaded) — worker runs, no inference");
    }
    return true;
}

void DpmWorker::destroy_sessions() {
    for (auto& ds : detectors_) {
        if (ds.post && post_ops_) { post_ops_->destroy(ds.post); ds.post = nullptr; }
        if (ds.session && infer_ops_) { infer_ops_->destroy(ds.session); ds.session = nullptr; }
        for (int b = 0; b < 2; ++b) {
            if (ds.input_fb[b] && fb_ops_) { fb_ops_->release_frame_buffer(ds.input_fb[b]); ds.input_fb[b] = nullptr; }
            if (ds.staging_nv12[b] && fb_ops_) { fb_ops_->release_frame_buffer(ds.staging_nv12[b]); ds.staging_nv12[b] = nullptr; }
        }
    }
    detectors_.clear();
    if (dsp_ctx_ && dsp_ops_) { dsp_ops_->deinit(dsp_ctx_); dsp_ctx_ = nullptr; }
}

void DpmWorker::offer_frame(const HalFrameBuffer* frame) {
    // Runs on the GStreamer streaming thread, BEFORE the per-stream bake, so the
    // frame is still CLEAN (inference must not feed on its own mask output).
    if (!running_.load(std::memory_order_acquire) || !frame) return;

    // Throttle (lock-free fast path): skip captures closer than 1/target_fps.
    const int64_t now = now_ns();
    const int64_t min_interval_ns =
        1000000000LL / std::max<uint32_t>(1u, cfg_.target_fps);
    if (now - last_offer_ns_.load(std::memory_order_relaxed) < min_interval_ns) return;

    std::lock_guard<std::mutex> lk(offer_mu_);
    if (!running_.load(std::memory_order_acquire)) return;

    // Drop-on-pending: if the worker hasn't consumed the last capture, keep it
    // (already the newest accepted) and drop THIS frame. Under steady 12fps
    // capture vs ~12fps worker this almost never drops; hysteresis covers gaps.
    if (offer_seq_ != taken_seq_) return;
    if (detectors_.empty()) return;  // idle: nothing to capture

    // Resize the CLEAN frame into every session's write buffer. This is the exact
    // 4K→model resize run_detector/run_segmenter did before — relocated pre-bake
    // onto the streaming thread so inference only ever sees clean pixels.
    for (auto& ds : detectors_) {
        if (!ds.input_fb[ds.write_idx] || !dsp_ctx_) continue;
        // RGB-input sessions: the NV12 clean frame can't resize directly into the
        // RGB24 model input (DSP requires matching src/dst formats). Route through
        // the NV12 staging buffer — resize NV12->NV12 staging, then convert_format
        // NV12 staging->RGB24 input_fb. NV12-native sessions skip staging and
        // resize straight into input_fb (staging_nv12 is null).
        HalFrameBuffer* const resize_dst = ds.staging_nv12[ds.write_idx]
                                               ? ds.staging_nv12[ds.write_idx]
                                               : ds.input_fb[ds.write_idx];
        HalDspResizeParams rp{};
        rp.src = frame;
        rp.dst = resize_dst;
        rp.interpolation = HAL_DSP_INTERPOLATION_BILINEAR;
        if (dsp_ops_->resize(dsp_ctx_, &rp) != 0) {
            // DSP contention with the encoder (shared DSP). Drop this whole
            // capture WITHOUT advancing the seq; hysteresis holds the last mask.
            HAL_LOG_WARNING("DPM: clean-capture resize failed (DSP contention?) — frame dropped");
            return;
        }
        if (ds.staging_nv12[ds.write_idx]) {
            HalDspConvertFormatParams cfp{};
            cfp.src = ds.staging_nv12[ds.write_idx];
            cfp.dst = ds.input_fb[ds.write_idx];
            if (dsp_ops_->convert_format(dsp_ctx_, &cfp) != 0) {
                HAL_LOG_WARNING("DPM[%s]: NV12->RGB convert_format failed — frame dropped",
                                ds.spec.name.c_str());
                return;
            }
        }
    }

    cap_w_ = frame->width;
    cap_h_ = frame->height;
    // Flip each write_idx so the worker reads the just-written buffer (=write^1)
    // and the next offer writes the OTHER buffer — the ping-pong invariant.
    for (auto& ds : detectors_) ds.write_idx ^= 1;
    ++offer_seq_;
    last_offer_ns_.store(now, std::memory_order_relaxed);  // stamp only on acceptance
    cv_.notify_one();
}

void DpmWorker::worker_loop() {
    while (running_.load(std::memory_order_acquire)) {
        uint32_t fw = 0;
        uint32_t fh = 0;
        {
            std::unique_lock<std::mutex> lk(offer_mu_);
            cv_.wait(lk, [this] {
                return !running_.load(std::memory_order_acquire) ||
                       offer_seq_ != taken_seq_;
            });
            if (!running_.load(std::memory_order_acquire)) break;
            if (offer_seq_ == taken_seq_) continue;  // spurious wakeup
            // Take the capture: each session's read buffer is the one just written
            // (write_idx was flipped AFTER the write → read_idx = write_idx^1).
            for (auto& ds : detectors_) ds.read_idx = ds.write_idx ^ 1;
            taken_seq_ = offer_seq_;
            fw = cap_w_;
            fh = cap_h_;
        }

        if (fw == 0 || fh == 0) continue;

        auto state = std::make_shared<DpmMaskState>();
        const int64_t now = now_ns();
        state->ts_ns = now;
        state->frame_w = fw;
        state->frame_h = fh;
        // Frame-resolution bytemask (0 or 255), unified silhouette of every
        // selected target (person seg + det bboxes OR'd together). For 4K main
        // this is ~8MB/cycle; glibc reuses the arena block after warmup so the
        // steady-state cost is a memset, not a real alloc. Built on the worker
        // thread only — the frontend never touches it until published.
        state->mask.assign(static_cast<size_t>(fw) * fh, 0);

        const int64_t t_start = now_ns();
        bool any_ok = false;

        // Plan-A gate buffers (worker thread only). seg_accum collects the linknet
        // foreground silhouette; person_gate collects the union of person detection
        // boxes. Pre-sized only when needed. Per-frame ~8MB each on 4K main, but
        // glibc reuses the arena block after warmup — steady-state cost is memset.
        bool have_seg = false;
        bool need_gate = false;
        for (const auto& ds : detectors_) {
            if (ds.spec.is_seg) have_seg = true;
            else if (!ds.spec.gate_label.empty()) need_gate = true;
        }
        std::vector<uint8_t> seg_accum;
        std::vector<uint8_t> person_gate;
        if (have_seg) seg_accum.assign(static_cast<size_t>(fw) * fh, 0);
        if (need_gate) person_gate.assign(static_cast<size_t>(fw) * fh, 0);

        for (auto& ds : detectors_) {
            if (ds.spec.is_seg) {
                if (run_segmenter(ds, seg_accum, fw, fh)) any_ok = true;
            } else {
                std::vector<uint8_t>* gate =
                    (!ds.spec.gate_label.empty()) ? &person_gate : nullptr;
                if (run_detector(ds, state->mask, gate, fw, fh)) any_ok = true;
            }
        }

        // Compose Plan-A: keep linknet's silhouette only where a person box exists.
        // seg_accum && person_gate -> final bytemask. If seg ran but no person was
        // detected this frame (empty gate), the silhouette contributes nothing (no
        // ungated background leaks through). Non-gate det bboxes (vehicle/face/
        // plate) were already OR'd into state->mask above. If seg loaded but its
        // person detector failed to load (no gate_label anywhere), fall back to an
        // ungated silhouette — over-broad, but still masks the person.
        if (have_seg) {
            const size_t n = static_cast<size_t>(fw) * fh;
            if (need_gate) {
                for (size_t i = 0; i < n; ++i)
                    if (seg_accum[i] && person_gate[i]) state->mask[i] = 255;
            } else {
                for (size_t i = 0; i < n; ++i)
                    if (seg_accum[i]) state->mask[i] = 255;
            }
        }

        const int64_t t_done = now_ns();

        const bool nonempty = mask_nonempty(state->mask);
        if (!any_ok || !nonempty) {
            if ((cycle_cnt_++ % 10u) == 0u) {
                HAL_LOG_INFO("DPM cycle #%llu: no-mask any_ok=%d dets=%zu ms=%lld",
                             static_cast<unsigned long long>(cycle_cnt_),
                             static_cast<int>(any_ok), detectors_.size(),
                             static_cast<long long>((t_done - t_start) / 1000000LL));
            }
            publish(nullptr);  // hysteresis may keep last mask briefly
            continue;
        }

        // Merge occupied bytemask into mosaic_cells (mosaic/blur frontend path).
        // Run-length merge guarantees full coverage (no truncation gap). overlay
        // mode ignores this and draws the full bytemask in one draw_mask.
        build_mosaic_cells(state->mask, fw, fh, cfg_.mosaic_stride,
                           cfg_.max_mosaic_cells, state->mosaic_cells);

        last_det_ns_ = now;
        publish(state);

        if ((cycle_cnt_++ % 10u) == 0u) {
            HAL_LOG_INFO("DPM cycle #%llu: cells=%zu dets=%zu ms=%lld",
                         static_cast<unsigned long long>(cycle_cnt_),
                         state->mosaic_cells.size(), detectors_.size(),
                         static_cast<long long>((t_done - t_start) / 1000000LL));
        }
    }
}

bool DpmWorker::run_detector(DetectorSession& ds, std::vector<uint8_t>& mask,
                             std::vector<uint8_t>* gate_mask,
                             uint32_t fw, uint32_t fh) {
    // Inference runs on the session's captured CLEAN input buffer (ds.read_idx),
    // already resized by offer_frame() from the pre-bake frame. tensor_from_frame
    // packs that buffer into the model's expected input blob.
    HalFrameBuffer* in_fb = ds.input_fb[ds.read_idx];
    if (!in_fb) return false;

    HalTensor in{};
    if (infer_ops_->tensor_from_frame(in_fb, &in) != 0) return false;

    HalTensor outs[HAL_MAX_TENSORS];
    std::memset(outs, 0, sizeof(outs));
    int run_rc = infer_ops_->run(ds.session, &in, 1, outs, ds.num_outputs);
    // tensor_from_frame allocates a host buffer (TensorPriv holding a shared_ptr
    // to the memcpy'd model input blob); run() has now copied it to the NPU, so free it
    // on every path.
    infer_ops_->free_tensor(&in);
    if (run_rc != 0) {
        HAL_LOG_WARNING("DPM[%s]: infer run rc=%d", ds.spec.name.c_str(), run_rc);
        return false;
    }

    // tiny_yolov4 license-plate model: raw uint16 YOLO grid (NO on-chip NMS),
    // undecodable by the HAL NMS postproc. Decode the raw output tensors here and
    // OR the boxes into the bytemask, bypassing postproc entirely. Faithful port
    // of apps/.../parse_yolo_grid (see decode_plate_grid). The plate spec has no
    // gate_label, so boxes fill `mask` (the final bytemask) directly.
    if (ds.spec.is_grid_det) {
        const int drawn = decode_plate_grid(outs, ds.num_outputs, mask, fw, fh);
        for (uint32_t i = 0; i < ds.num_outputs; ++i) {
            if (outs[i].data) infer_ops_->free_tensor(&outs[i]);
        }
        if (drawn > 0) HAL_LOG_INFO("DPM[%s]: grid decode drew %d box(es)", ds.spec.name.c_str(), drawn);
        return true;  // inference succeeded this cycle (drawn may be 0 = no plate)
    }

    HalPostprocessResult pr{};
    int rc = post_ops_->run(ds.post, outs, ds.num_outputs, &pr);
    // Free run() output tensors (impl may have allocated them).
    for (uint32_t i = 0; i < ds.num_outputs; ++i) {
        if (outs[i].data) infer_ops_->free_tensor(&outs[i]);
    }
    if (rc != 0) return false;

    // OR each kept detection's bbox (normalized [0..1]) as a filled rect into
    // the frame-resolution bytemask. vehicle/face/plate have no segmentation
    // model, so a tight box is the best available silhouette.
    const auto& dr = pr.result.detection;
    const auto& keep = ds.spec.keep_labels;
    for (uint32_t i = 0; i < dr.num_detections && i < HAL_MAX_DETECTIONS; ++i) {
        const HalDetection& d = dr.detections[i];
        if (d.confidence < kDetConfFloor) continue;
        // keep_labels empty => keep all detections (e.g. plate/face models where
        // every detection is already the target). Otherwise string-match against
        // the postproc-emitted label (hailo yolov8 json populates d.label).
        if (!keep.empty()) {
            bool allow = false;
            for (const auto& L : keep) {
                if (L == d.label) { allow = true; break; }
            }
            if (!allow) continue;
        }
        // Plan-A gate routing: a detection matching this spec's gate_label writes
        // its box into the gate mask (ANDed against the seg silhouette by the
        // worker) rather than the final bytemask. All other kept labels fill the
        // final bytemask directly.
        const bool routed_to_gate = (gate_mask != nullptr)
            && !ds.spec.gate_label.empty()
            && (d.label == ds.spec.gate_label);
        std::vector<uint8_t>& dst = routed_to_gate ? *gate_mask : mask;

        // Shared fill helper (same one the grid path uses): clamps the normalized
        // box to frame bounds and fills it solid 255.
        fill_bbox_rect(dst, fw, fh, d.bbox.x, d.bbox.y, d.bbox.w, d.bbox.h);
    }

    post_ops_->free_result(&pr);
    return true;
}

bool DpmWorker::run_segmenter(DetectorSession& ds, std::vector<uint8_t>& mask,
                              uint32_t fw, uint32_t fh) {
    HalFrameBuffer* in_fb = ds.input_fb[ds.read_idx];
    if (!in_fb) return false;

    HalTensor in{};
    if (infer_ops_->tensor_from_frame(in_fb, &in) != 0) return false;

    HalTensor outs[HAL_MAX_TENSORS];
    std::memset(outs, 0, sizeof(outs));
    int run_rc = infer_ops_->run(ds.session, &in, 1, outs, ds.num_outputs);
    infer_ops_->free_tensor(&in);
    if (run_rc != 0) {
        HAL_LOG_WARNING("DPM[%s]: infer run rc=%d", ds.spec.name.c_str(), run_rc);
        return false;
    }

    HalPostprocessResult pr{};
    int rc = post_ops_->run(ds.post, outs, ds.num_outputs, &pr);
    for (uint32_t i = 0; i < ds.num_outputs; ++i) {
        if (outs[i].data) infer_ops_->free_tensor(&outs[i]);
    }
    if (rc != 0) return false;

    if (pr.type != HAL_POST_TYPE_SEGMENTATION || !pr.result.segmentation.mask_data) {
        post_ops_->free_result(&pr);
        HAL_LOG_WARNING("DPM[%s]: postproc returned no segmentation mask",
                        ds.spec.name.c_str());
        return false;
    }

    const auto& seg = pr.result.segmentation;
    const uint32_t mw = seg.width;
    const uint32_t mh = seg.height;
    if (mw == 0 || mh == 0) {
        post_ops_->free_result(&pr);
        return false;
    }

    // Nearest-neighbor upscale + threshold into the frame-res bytemask. Only
    // sets 255 (OR), so this composes with run_detector's bbox rects. Upscaling
    // runs on the worker thread, never the hot path. Threshold semantics are
    // verbatim from ai_example_v2.cpp:1525-1529 (cid==1 || cid>=128).
    for (uint32_t y = 0; y < fh; ++y) {
        const uint32_t my = (y * mh) / fh;
        uint8_t* dst_row = &mask[static_cast<size_t>(y) * fw];
        const uint8_t* src_row = &seg.mask_data[static_cast<size_t>(my) * mw];
        for (uint32_t x = 0; x < fw; ++x) {
            const uint32_t mx = (x * mw) / fw;
            const uint8_t cid = src_row[mx];
            if (cid == kSegFgClassId || cid >= kSegProbFloor) dst_row[x] = 255;
        }
    }

    post_ops_->free_result(&pr);
    return true;
}

void DpmWorker::publish(std::shared_ptr<DpmMaskState> state) {
    const bool nonempty = state && mask_nonempty(state->mask);
    std::shared_ptr<const DpmMaskState> to_publish;
    if (nonempty) {
        last_nonempty_ = state;
        to_publish = state;
    } else {
        // Temporal hysteresis: keep the last non-empty mask briefly so a single
        // dropped detection doesn't flicker the mask off. Clear after stale_ms.
        const int64_t now = now_ns();
        if (last_nonempty_ &&
            (now - last_det_ns_) <=
                static_cast<int64_t>(cfg_.stale_ms) * 1000000LL) {
            to_publish = last_nonempty_;
        } else {
            last_nonempty_.reset();
            to_publish = nullptr;
        }
    }
    {
        std::lock_guard<std::mutex> lk(latest_mu_);
        latest_ = to_publish;
    }
}

std::shared_ptr<const DpmMaskState> DpmWorker::get_latest() const {
    std::lock_guard<std::mutex> lk(latest_mu_);
    return latest_;
}
