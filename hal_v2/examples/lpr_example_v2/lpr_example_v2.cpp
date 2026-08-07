/**
 * @file lpr_example_v2.cpp
 * @brief License plate recognition: Tiny-YOLOv4 plate detection + LPRNet OCR (det = `HAL_POST_TYPE_DETECTION`, rec = `HAL_POST_TYPE_OCR_RECOGNITION`).
 *
 * Models: Hailo Model Zoo [license_plate_detection](https://github.com/hailo-ai/hailo_model_zoo/tree/master/hailo_models/license_plate_detection)
 * and [license_plate_recognition](https://github.com/hailo-ai/hailo_model_zoo/tree/master/hailo_models/license_plate_recognition).
 * Pipeline idea matches hailo-camera-apps `lpr_small_yolo_rgb.sh` (plate detect, crop, `libocr_post.so` on LPRNet).
 *
 * 1) Resize full frame to detector WxH; YOLO post from `--lp-det-post-file` (e.g. `libyolo_post.so` + `tiny_yolov4_license_plates`).
 * 2) For each plate (top `--max-plates`, gate `--min-det-confidence`): letterbox-crop to LPRNet WxH; OCR post on `--lp-rec-post-file`.
 * 3) Merge to `HAL_POST_TYPE_OCR_RECOGNITION` with plate bbox (normalized vs full frame) and decoded text.
 *
 * Usage:
 *   hal-lpr-example-v2 --media <medialib.json> --profile <name_or_idx> \\
 *     --lp-det-hef tiny_yolov4_license_plates.hef --lp-det-post-file yolov4_license_plate.json \\
 *     --lp-rec-hef lprnet.hef [--lp-rec-post-file lprnet.json] \\
 *     [--udp host:port] [--verbose] [--max-plates N] [--min-det-confidence x]
 *
 * Cross-build: source `../sdk_4.0.23/environment-setup-armv8a-poky-linux` then CMake (see `hal_v2/CMakeLists.txt` sysroot hints).
 */

#include "common/hal_log.h"
#include "common/hal_udp_stream.hpp"

#include "media/hal_codec_internal.h"
#include "media/hal_media.h"
#include "media/hal_video_internal.h"

#include "dsp/hal_dsp.h"

#include "model/hal_draw.h"
#include "model/hal_inference.h"
#include "model/hal_postprocess.h"

#include <atomic>
#include <algorithm>
#include <iomanip>
#include <cctype>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <memory>
#include <sstream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

static std::atomic<bool> g_stop{false};
static volatile sig_atomic_t g_sig = 0;
static void on_sig(int) { g_sig = 1; }

struct FrameJob
{
    void *video_ctx = nullptr;
    HalFrameBuffer *frame = nullptr;
};

struct SharedFrame
{
    void *video_ctx = nullptr;
    HalFrameBuffer fb{};
    std::atomic<int> refs{0};
};

static SharedFrame *sharedframe_from_callback(void *video_ctx, HalFrameBuffer *stack_frame, int initial_refs)
{
    if (!video_ctx || !stack_frame || initial_refs <= 0)
        return nullptr;
    auto *sf = new (std::nothrow) SharedFrame{};
    if (!sf)
        return nullptr;
    sf->video_ctx = video_ctx;
    sf->fb = *stack_frame;
    stack_frame->priv = nullptr;
    sf->refs.store(initial_refs, std::memory_order_release);
    return sf;
}

static void sharedframe_unref(SharedFrame *sf)
{
    if (!sf)
        return;
    const int prev = sf->refs.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1)
    {
        (void)HAL_VIDEO_OPS.release_frame(sf->video_ctx, &sf->fb);
        delete sf;
    }
}

/** Holds two @c HalPostprocessResult values; the HAL union’s largest arm is @c HalKeypointResult (multi‑MB). */
struct LprSharedResult
{
    std::mutex mu;
    std::condition_variable cv;
    HalPostprocessResult last{};
    uint64_t seq = 0;
    bool valid = false;
    HalPostprocessResult last_valid{};
    uint64_t last_valid_seq = 0;
    bool has_valid = false;
    std::chrono::steady_clock::time_point last_valid_tp{};
};

struct LprAppCtx
{
    void *media_ctx = nullptr;
    void *video_ctx = nullptr;
    void *codec_ctx = nullptr;
    void *dsp_ctx = nullptr;
    std::string stream_key;

    HalInferenceSession *det_infer = nullptr;
    HalPostprocessSession *det_post = nullptr;
    HalInferenceSession *rec_infer = nullptr;
    HalPostprocessSession *rec_post = nullptr;
    bool det_only = false;
    enum RecBackend { REC_BACKEND_LPRNET = 0, REC_BACKEND_PPOCRV5 = 1 };
    RecBackend rec_backend = REC_BACKEND_LPRNET;
    bool rec_local_decode = true; // LPRNet typically needs local CTC (PaddleOCR uses vendor OCR postprocess)

    // Simple-det temporal smoothing (best-effort).
    std::vector<HalDetection> det_last_boxes;
    std::chrono::steady_clock::time_point det_last_tp{};

    // Det-only local postprocess params (PaddleOCR-like defaults).
    float det_bin_thresh = 0.30f;
    float det_box_thresh = 0.15f;
    float det_unclip_ratio = 1.2f;
    float det_min_box_size = 2.0f; // in probmap pixels (after det output)
    uint32_t det_max_candidates = 100;

    // Recognition decode params (builtin, derived from hailo-apps ocr_postprocess.cpp idea).
    std::string rec_charset_path;
    // CTC blank/invalid token index. -1 means "use default (last class)".
    int rec_blank_index = -1;
    int rec_charset_index_offset = 0;
    bool rec_time_major = false;
    bool rec_logits_are_softmax = true; // our HEF output is ew_mult_softmax3 (u8)
    float rec_min_score_threshold = 0.55f;
    int rec_min_chars = 4;
    bool rec_force_nv12 = false;        // HEF is NV12 but shape may look like packed RGB
    HalDspScalingMode rec_scaling_mode = HAL_DSP_SCALING_STRETCH;
    bool rec_debug = false;
    // Packed RGB/BGR selection for NV12->packed conversion and DSP convert_format outputs.
    // Default is RGB to match Hailo Model Zoo lprnet input spec.
    enum PackedFormatMode { PACKED_RGB = 0, PACKED_BGR = 1, PACKED_AUTO = 2 };
    PackedFormatMode det_packed_mode = PACKED_RGB;
    PackedFormatMode rec_packed_mode = PACKED_RGB;

    uint32_t det_w = 0, det_h = 0;
    uint32_t rec_w = 0, rec_h = 0;
    uint32_t max_text_regions = 8;
    float min_det_confidence = 0.12f;
    bool verbose = false;

    std::mutex q_mu;
    std::condition_variable q_cv;
    std::deque<SharedFrame *> q;
    static constexpr size_t kQMax = 8;

    std::mutex preview_mu;
    std::condition_variable preview_cv;
    std::deque<SharedFrame *> preview_q;

    LprSharedResult result;
    HalDrawConfig draw_cfg{};

    HalUdpStream *udp = nullptr;

    std::thread ai_worker;
    std::thread preview_worker;
};

static void simple_det_connected_components_from_prob_u8(const uint8_t *prob_u8, uint32_t H, uint32_t W,
                                                        float bin_thresh, float box_thresh,
                                                        float unclip_ratio, float min_box_size_px,
                                                        uint32_t max_candidates, uint32_t min_area,
                                                        std::vector<HalDetection> &out)
{
    out.clear();
    if (!prob_u8 || H == 0 || W == 0)
        return;
    const uint8_t thr = (uint8_t)std::max(0, std::min(255, (int)std::lround(bin_thresh * 255.0f)));

    const uint32_t N = H * W;
    std::vector<int32_t> visited;
    visited.assign(N, 0);
    std::vector<uint32_t> q;
    q.reserve(4096);

    auto push_det = [&](uint32_t minx, uint32_t miny, uint32_t maxx, uint32_t maxy, uint32_t area, uint64_t sum_u8) {
        if (area < min_area)
            return;
        const float mean = (area > 0) ? (float)sum_u8 / (float)area : 0.0f;
        const float score = mean / 255.0f;
        if (score < box_thresh)
            return;
        const float bw = (float)(maxx - minx + 1U);
        const float bh = (float)(maxy - miny + 1U);
        if (bw < min_box_size_px || bh < min_box_size_px)
            return;

        // PaddleOCR/DBNet "unclip" expands polygons; we approximate by expanding the axis-aligned bbox.
        // This is a pragmatic improvement over a tight heatmap bbox without pulling in OpenCV contours.
        float expand = std::max(1.0f, unclip_ratio);
        const float dx = 0.5f * (expand - 1.0f) * bw;
        const float dy = 0.5f * (expand - 1.0f) * bh;
        int32_t ex0 = (int32_t)std::floor((float)minx - dx);
        int32_t ey0 = (int32_t)std::floor((float)miny - dy);
        int32_t ex1 = (int32_t)std::ceil((float)maxx + dx);
        int32_t ey1 = (int32_t)std::ceil((float)maxy + dy);
        ex0 = std::max<int32_t>(0, ex0);
        ey0 = std::max<int32_t>(0, ey0);
        ex1 = std::min<int32_t>((int32_t)W - 1, ex1);
        ey1 = std::min<int32_t>((int32_t)H - 1, ey1);
        if (ex1 <= ex0 || ey1 <= ey0)
            return;

        HalDetection d{};
        d.bbox.x = (float)ex0 / (float)W;
        d.bbox.y = (float)ey0 / (float)H;
        d.bbox.w = (float)(ex1 - ex0 + 1) / (float)W;
        d.bbox.h = (float)(ey1 - ey0 + 1) / (float)H;
        d.confidence = score;
        d.class_id = 0;
        d.track_id = -1;
        std::snprintf(d.label, sizeof(d.label), "%s", "text_region");
        out.push_back(d);
    };

    for (uint32_t y0 = 0; y0 < H; y0++)
    {
        for (uint32_t x0 = 0; x0 < W; x0++)
        {
            const uint32_t idx0 = y0 * W + x0;
            if (visited[idx0])
                continue;
            if (prob_u8[idx0] < thr)
            {
                visited[idx0] = 1;
                continue;
            }
            // BFS 4-neighborhood
            uint32_t minx = x0, miny = y0, maxx = x0, maxy = y0;
            uint32_t area = 0;
            uint64_t sum_u8 = 0;
            q.clear();
            q.push_back(idx0);
            visited[idx0] = 1;
            for (size_t qi = 0; qi < q.size(); qi++)
            {
                const uint32_t idx = q[qi];
                const uint32_t y = idx / W;
                const uint32_t x = idx % W;
                area++;
                sum_u8 += prob_u8[idx];
                if (x < minx) minx = x;
                if (y < miny) miny = y;
                if (x > maxx) maxx = x;
                if (y > maxy) maxy = y;
                auto try_push = [&](int32_t nx, int32_t ny) {
                    if (nx < 0 || ny < 0 || nx >= (int32_t)W || ny >= (int32_t)H)
                        return;
                    const uint32_t nidx = (uint32_t)ny * W + (uint32_t)nx;
                    if (visited[nidx])
                        return;
                    visited[nidx] = 1;
                    if (prob_u8[nidx] >= thr)
                        q.push_back(nidx);
                };
                try_push((int32_t)x - 1, (int32_t)y);
                try_push((int32_t)x + 1, (int32_t)y);
                try_push((int32_t)x, (int32_t)y - 1);
                try_push((int32_t)x, (int32_t)y + 1);
            }
            push_det(minx, miny, maxx, maxy, area, sum_u8);
            if (out.size() >= HAL_MAX_DETECTIONS)
                return;
        }
    }

    // Keep top candidates by confidence (PaddleOCR det_max_candidates behavior).
    if (max_candidates > 0 && out.size() > max_candidates)
    {
        std::partial_sort(out.begin(), out.begin() + (size_t)max_candidates, out.end(),
                          [](const HalDetection &a, const HalDetection &b) { return a.confidence > b.confidence; });
        out.resize((size_t)max_candidates);
    }
}

[[maybe_unused]] static void simple_det_bbox_from_prob_u8(const uint8_t *prob_u8, uint32_t H, uint32_t W,
                                        float bin_thresh, std::vector<HalDetection> &out)
{
    out.clear();
    if (!prob_u8 || H == 0 || W == 0)
        return;
    uint8_t maxv = 0;
    uint32_t maxx = 0, maxy = 0;
    for (uint32_t i = 0; i < H * W; i++)
        if (prob_u8[i] > maxv)
        {
            maxv = prob_u8[i];
            maxy = i / W;
            maxx = i % W;
        }

    uint8_t thr = (uint8_t)std::max(0, std::min(255, (int)std::lround(bin_thresh * 255.0f)));
    // If threshold is too aggressive (no pixels), adapt based on max activation.
    if (maxv > 0 && thr > maxv)
        thr = (uint8_t)std::max<uint8_t>(1, (uint8_t)((uint32_t)maxv * 7U / 10U)); // 0.7 * max

    bool any = false;
    uint32_t minx = W, miny = H, bx1 = 0, by1 = 0;
    for (uint32_t y = 0; y < H; y++)
    {
        const uint8_t *row = prob_u8 + (size_t)y * (size_t)W;
        for (uint32_t x = 0; x < W; x++)
        {
            if (row[x] >= thr)
            {
                any = true;
                if (x < minx) minx = x;
                if (y < miny) miny = y;
                if (x > bx1) bx1 = x;
                if (y > by1) by1 = y;
            }
        }
    }
    if (!any || minx >= W || miny >= H)
    {
        // Fallback: always return a fixed-size box around the max activation
        // (prevents long stretches of 0 boxes when the probmap is weak).
        if (maxv == 0)
            return;
        const uint32_t half_w = std::max(8U, W / 10U);
        const uint32_t half_h = std::max(4U, H / 10U);
        minx = (maxx > half_w) ? (maxx - half_w) : 0U;
        miny = (maxy > half_h) ? (maxy - half_h) : 0U;
        bx1 = std::min(W - 1U, maxx + half_w);
        by1 = std::min(H - 1U, maxy + half_h);
    }

    // Expand a bit (in probmap coordinates) to be robust.
    const uint32_t pad_x = std::max(1U, W / 100U);
    const uint32_t pad_y = std::max(1U, H / 100U);
    minx = (minx > pad_x) ? (minx - pad_x) : 0U;
    miny = (miny > pad_y) ? (miny - pad_y) : 0U;
    bx1 = std::min(W - 1U, bx1 + pad_x);
    by1 = std::min(H - 1U, by1 + pad_y);

    HalDetection d{};
    d.bbox.x = (float)minx / (float)W;
    d.bbox.y = (float)miny / (float)H;
    d.bbox.w = (float)(bx1 - minx + 1U) / (float)W;
    d.bbox.h = (float)(by1 - miny + 1U) / (float)H;
    d.confidence = 0.99f;
    d.class_id = 0;
    d.track_id = -1;
    std::snprintf(d.label, sizeof(d.label), "%s", "text_region");
    out.push_back(d);
}

static void enqueue_ai(LprAppCtx *ctx, SharedFrame *sf)
{
    if (!ctx || !sf)
        return;
    std::unique_lock<std::mutex> lk(ctx->q_mu);
    while (ctx->q.size() >= LprAppCtx::kQMax && !g_stop.load(std::memory_order_acquire))
    {
        SharedFrame *drop = ctx->q.front();
        ctx->q.pop_front();
        lk.unlock();
        sharedframe_unref(drop);
        lk.lock();
    }
    if (g_stop.load(std::memory_order_acquire))
    {
        lk.unlock();
        sharedframe_unref(sf);
        return;
    }
    ctx->q.push_back(sf);
    ctx->q_cv.notify_one();
}

static void enqueue_preview(LprAppCtx *ctx, SharedFrame *sf)
{
    if (!ctx || !sf)
        return;
    std::unique_lock<std::mutex> lk(ctx->preview_mu);
    while (ctx->preview_q.size() >= LprAppCtx::kQMax && !g_stop.load(std::memory_order_acquire))
    {
        SharedFrame *drop = ctx->preview_q.front();
        ctx->preview_q.pop_front();
        lk.unlock();
        sharedframe_unref(drop);
        lk.lock();
    }
    if (g_stop.load(std::memory_order_acquire))
    {
        lk.unlock();
        sharedframe_unref(sf);
        return;
    }
    ctx->preview_q.push_back(sf);
    ctx->preview_cv.notify_one();
}

static void video_cb_preview_and_ai(void *video_ctx, HalFrameBuffer *frame, void *userdata)
{
    auto *ctx = static_cast<LprAppCtx *>(userdata);
    if (!ctx || !frame)
        return;
    SharedFrame *sf = sharedframe_from_callback(video_ctx, frame, 2);
    if (!sf)
        return;
    enqueue_preview(ctx, sf);
    enqueue_ai(ctx, sf);
}

static void video_cb_preview_only(void *video_ctx, HalFrameBuffer *frame, void *userdata)
{
    auto *ctx = static_cast<LprAppCtx *>(userdata);
    if (!ctx || !frame)
        return;
    SharedFrame *sf = sharedframe_from_callback(video_ctx, frame, 1);
    if (!sf)
        return;
    enqueue_preview(ctx, sf);
}

static void video_cb_ai_only(void *video_ctx, HalFrameBuffer *frame, void *userdata)
{
    auto *ctx = static_cast<LprAppCtx *>(userdata);
    if (!ctx || !frame)
        return;
    SharedFrame *sf = sharedframe_from_callback(video_ctx, frame, 1);
    if (!sf)
        return;
    enqueue_ai(ctx, sf);
}

static void codec_cb(void *codec_ctx, HalPacketBuffer *packet, void *userdata)
{
    auto *ctx = static_cast<LprAppCtx *>(userdata);
    if (!ctx || !packet)
        return;
    if (ctx->udp && ctx->udp->ok() && packet->data && packet->size > 0)
        ctx->udp->push_annex_b(packet->data, packet->size, packet->timestamp_ns);
    (void)HAL_CODEC_OPS.release_packet(codec_ctx, packet);
}

static std::optional<std::pair<uint32_t, uint32_t>> model_input_hw(HalInferenceSession *s)
{
    if (!s)
        return std::nullopt;
    HalModelInfo mi{};
    if (HAL_INFERENCE_OPS.get_model_info(s, &mi) != HAL_OK || mi.num_inputs == 0)
        return std::nullopt;
    for (uint32_t i = 0; i < mi.num_inputs; i++)
    {
        const auto &in = mi.inputs[i];
        if (in.ndim >= 4)
        {
            const int32_t h = in.shape[1];
            const int32_t w = in.shape[2];
            if (w > 0 && h > 0)
                return std::make_pair((uint32_t)w, (uint32_t)h);
        }
    }
    return std::nullopt;
}

static const char *tensor_layout_to_str(HalTensorLayout v)
{
    switch (v)
    {
    case HAL_TENSOR_LAYOUT_UNKNOWN:
        return "UNKNOWN";
    case HAL_TENSOR_LAYOUT_NHWC:
        return "NHWC";
    case HAL_TENSOR_LAYOUT_NCHW:
        return "NCHW";
    case HAL_TENSOR_LAYOUT_NC:
        return "NC";
    case HAL_TENSOR_LAYOUT_NHW:
        return "NHW";
    case HAL_TENSOR_LAYOUT_CHW:
        return "CHW";
    case HAL_TENSOR_LAYOUT_HWC:
        return "HWC";
    default:
        return "UNKNOWN";
    }
}

static const char *dtype_to_str(HalDataType v)
{
    switch (v)
    {
    case HAL_DTYPE_UNKNOWN:
        return "UNKNOWN";
    case HAL_DTYPE_UINT8:
        return "UINT8";
    case HAL_DTYPE_INT8:
        return "INT8";
    case HAL_DTYPE_UINT16:
        return "UINT16";
    case HAL_DTYPE_INT16:
        return "INT16";
    case HAL_DTYPE_UINT32:
        return "UINT32";
    case HAL_DTYPE_INT32:
        return "INT32";
    case HAL_DTYPE_FLOAT32:
        return "FLOAT32";
    default:
        return "UNKNOWN";
    }
}

static void log_model_info(HalInferenceSession *s, const char *tag)
{
    if (!s)
        return;
    HalModelInfo mi{};
    if (HAL_INFERENCE_OPS.get_model_info(s, &mi) != HAL_OK)
        return;

    HAL_LOG_INFO("%s: model=\"%s\" runtime=\"%s\" net=\"%s\" group=\"%s\" num_inputs=%u num_outputs=%u",
                 tag ? tag : "model",
                 mi.name, mi.version, mi.network_name, mi.network_group_name,
                 (unsigned)mi.num_inputs, (unsigned)mi.num_outputs);

    for (uint32_t i = 0; i < mi.num_inputs && i < HAL_MAX_TENSORS; i++)
    {
        const auto &t = mi.inputs[i];
        HAL_LOG_INFO("%s: input[%u] name=\"%s\" ndim=%d layout=%s dtype=%s byte_size=%u shape=[%d,%d,%d,%d,%d,%d,%d,%d]",
                     tag ? tag : "model", (unsigned)i, t.name, (int)t.ndim,
                     tensor_layout_to_str(t.layout), dtype_to_str(t.dtype), (unsigned)t.byte_size,
                     (int)t.shape[0], (int)t.shape[1], (int)t.shape[2], (int)t.shape[3],
                     (int)t.shape[4], (int)t.shape[5], (int)t.shape[6], (int)t.shape[7]);
    }
    for (uint32_t i = 0; i < mi.num_outputs && i < HAL_MAX_TENSORS; i++)
    {
        const auto &t = mi.outputs[i];
        HAL_LOG_INFO("%s: output[%u] name=\"%s\" ndim=%d layout=%s dtype=%s byte_size=%u is_nms=%u shape=[%d,%d,%d,%d,%d,%d,%d,%d]",
                     tag ? tag : "model", (unsigned)i, t.name, (int)t.ndim,
                     tensor_layout_to_str(t.layout), dtype_to_str(t.dtype), (unsigned)t.byte_size,
                     (unsigned)t.is_nms,
                     (int)t.shape[0], (int)t.shape[1], (int)t.shape[2], (int)t.shape[3],
                     (int)t.shape[4], (int)t.shape[5], (int)t.shape[6], (int)t.shape[7]);
    }
}

static std::string to_lower_ascii(const char *s)
{
    if (!s)
        return {};
    std::string out;
    out.reserve(std::strlen(s));
    for (const char *p = s; *p; ++p)
        out.push_back((char)std::tolower((unsigned char)*p));
    return out;
}

static bool ascii_contains_ci(const std::string &haystack, const char *needle)
{
    if (!needle || !*needle)
        return true;
    std::string h;
    h.reserve(haystack.size());
    for (const char ch : haystack)
        h.push_back((char)std::tolower((unsigned char)ch));
    const std::string n = to_lower_ascii(needle);
    return h.find(n) != std::string::npos;
}

static int32_t model_input_channels(const HalModelTensorInfo &in)
{
    if (in.ndim <= 0)
        return 0;

    // Best-effort channel extraction for common RGB layouts.
    switch (in.layout)
    {
    case HAL_TENSOR_LAYOUT_NHWC:
        if (in.ndim >= 4)
            return in.shape[3];
        break;
    case HAL_TENSOR_LAYOUT_NCHW:
        if (in.ndim >= 4)
            return in.shape[1];
        break;
    case HAL_TENSOR_LAYOUT_HWC:
        if (in.ndim >= 3)
            return in.shape[2];
        break;
    case HAL_TENSOR_LAYOUT_CHW:
        if (in.ndim >= 3)
            return in.shape[0];
        break;
    default:
        break;
    }

    // Fallback: check plausible "channel indices" where channel dim could be 3.
    // This avoids needing reliable color-order metadata.
    if (in.ndim == 4)
    {
        if (in.shape[1] == 3)
            return 3;
        if (in.shape[3] == 3)
            return 3;
    }
    else if (in.ndim == 3)
    {
        if (in.shape[0] == 3)
            return 3;
        if (in.shape[2] == 3)
            return 3;
    }
    else if (in.ndim == 2)
    {
        if (in.shape[1] == 3)
            return 3;
    }
    return 0;
}

/**
 * Decide whether we should convert input NV12 frames into model-expected RGB/BGR.
 *
 * The example input is always NV12 (Y + UV planes). If the model expects a 3-channel packed tensor,
 * `tensor_from_frame()` can do NV12->RGB/BGR conversion via `HalPreprocessConfig.color`.
 *
 * Heuristic:
 * - If the first 3-channel input is detected and input tensor name contains "bgr", use NV12_TO_BGR.
 * - Otherwise use NV12_TO_RGB.
 */
static HalPreprocessColor detect_nv12_to_rgb_bgr_for_session(HalInferenceSession *s)
{
    if (!s)
        return HAL_PREPROCESS_COLOR_NONE;
    HalModelInfo mi{};
    if (HAL_INFERENCE_OPS.get_model_info(s, &mi) != HAL_OK || mi.num_inputs == 0)
        return HAL_PREPROCESS_COLOR_NONE;

    for (uint32_t i = 0; i < mi.num_inputs; i++)
    {
        const auto &in = mi.inputs[i];
        const int32_t channels = model_input_channels(in);
        if (channels != 3)
            continue;

        const std::string lname = to_lower_ascii(in.name);
        if (lname.find("bgr") != std::string::npos)
            return HAL_PREPROCESS_COLOR_NV12_TO_BGR;
        return HAL_PREPROCESS_COLOR_NV12_TO_RGB;
    }
    return HAL_PREPROCESS_COLOR_NONE;
}

static HalPixelFormat pick_rgb_or_bgr24_for_session(HalInferenceSession *s)
{
    const HalPreprocessColor c = detect_nv12_to_rgb_bgr_for_session(s);
    if (c == HAL_PREPROCESS_COLOR_NV12_TO_BGR)
        return HAL_PIX_FMT_BGR24;
    return HAL_PIX_FMT_RGB24;
}

static HalPreprocessColor pick_preprocess_color_for_mode(HalInferenceSession *s, bool force_none, LprAppCtx::PackedFormatMode mode)
{
    if (force_none || !s)
        return HAL_PREPROCESS_COLOR_NONE;
    if (mode == LprAppCtx::PACKED_BGR)
        return HAL_PREPROCESS_COLOR_NV12_TO_BGR;
    if (mode == LprAppCtx::PACKED_RGB)
        return HAL_PREPROCESS_COLOR_NV12_TO_RGB;
    return detect_nv12_to_rgb_bgr_for_session(s);
}

static HalPixelFormat pick_packed_pixfmt_for_mode(HalInferenceSession *s, LprAppCtx::PackedFormatMode mode)
{
    if (mode == LprAppCtx::PACKED_BGR)
        return HAL_PIX_FMT_BGR24;
    if (mode == LprAppCtx::PACKED_RGB)
        return HAL_PIX_FMT_RGB24;
    return pick_rgb_or_bgr24_for_session(s);
}

static uint32_t pick_video_index_closest(void **video_list, uint32_t count, uint32_t mw, uint32_t mh)
{
    if (!video_list || count == 0)
        return UINT32_MAX;
    uint32_t best = 0;
    uint64_t best_cost = UINT64_MAX;
    for (uint32_t i = 0; i < count; i++)
    {
        auto *v = static_cast<HalVideoContext *>(video_list[i]);
        if (!v)
            continue;
        const uint64_t dw = (v->config.width > mw) ? (v->config.width - mw) : (mw - v->config.width);
        const uint64_t dh = (v->config.height > mh) ? (v->config.height - mh) : (mh - v->config.height);
        const uint64_t cost = dw + dh;
        if (cost < best_cost)
        {
            best_cost = cost;
            best = i;
        }
    }
    return best;
}

static uint32_t pick_video_index_exact_or_closest(void **video_list, uint32_t count, uint32_t w, uint32_t h)
{
    if (!video_list || count == 0)
        return UINT32_MAX;
    for (uint32_t i = 0; i < count; i++)
    {
        auto *v = static_cast<HalVideoContext *>(video_list[i]);
        if (v && v->config.width == w && v->config.height == h)
            return i;
    }
    return pick_video_index_closest(video_list, count, w, h);
}

static uint32_t pick_video_index_exact_or_closest_prefer_not(void **video_list, uint32_t count, uint32_t w, uint32_t h,
                                                             uint32_t avoid_index)
{
    if (!video_list || count == 0)
        return UINT32_MAX;
    uint32_t any_exact = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++)
    {
        auto *v = static_cast<HalVideoContext *>(video_list[i]);
        if (!v || v->config.width != w || v->config.height != h)
            continue;
        if (any_exact == UINT32_MAX)
            any_exact = i;
        if (i != avoid_index)
            return i;
    }
    if (any_exact != UINT32_MAX)
        return any_exact;
    uint32_t best = UINT32_MAX;
    uint64_t best_cost = UINT64_MAX;
    uint32_t best_any = UINT32_MAX;
    uint64_t best_any_cost = UINT64_MAX;
    for (uint32_t i = 0; i < count; i++)
    {
        auto *v = static_cast<HalVideoContext *>(video_list[i]);
        if (!v)
            continue;
        const uint64_t dw = (v->config.width > w) ? (v->config.width - w) : (w - v->config.width);
        const uint64_t dh = (v->config.height > h) ? (v->config.height - h) : (h - v->config.height);
        const uint64_t cost = dw + dh;
        if (cost < best_any_cost)
        {
            best_any_cost = cost;
            best_any = i;
        }
        if (i != avoid_index && cost < best_cost)
        {
            best_cost = cost;
            best = i;
        }
    }
    return (best != UINT32_MAX) ? best : best_any;
}

static inline uint32_t ceil_div_u32(uint32_t a, uint32_t b) { return (a + b - 1U) / b; }
static inline uint32_t align_up_even_u32(uint32_t v) { return (v + 1U) & ~1U; }
static inline bool is_even_u32(uint32_t v) { return (v % 2U) == 0U; }

static int dsp_resize_chain(void *dsp_ctx, const HalFrameBuffer *src, HalFrameBuffer *dst,
                            HalDspInterpolation interpolation, std::vector<HalFrameBuffer *> &intermediates_out)
{
    intermediates_out.clear();
    if (!dsp_ctx || !src || !dst)
        return HAL_ERR_INVALID_ARG;
    constexpr uint32_t kMaxDownscale = 16U;
    const HalFrameBuffer *cur_src = src;
    while (cur_src->width > dst->width * kMaxDownscale || cur_src->height > dst->height * kMaxDownscale)
    {
        const uint32_t next_w = std::max(dst->width, ceil_div_u32(cur_src->width, kMaxDownscale));
        const uint32_t next_h = std::max(dst->height, ceil_div_u32(cur_src->height, kMaxDownscale));
        HalFrameBufferRequest req{};
        req.width = next_w;
        req.height = next_h;
        req.format = HAL_PIX_FMT_NV12;
        req.mem_type = HAL_MEM_DMABUF;
        req.zero_initialize = false;
        HalFrameBuffer *mid = nullptr;
        if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&req, &mid) != HAL_OK || !mid)
            goto fail;
        HalDspResizeParams rp{};
        rp.src = cur_src;
        rp.dst = mid;
        rp.interpolation = interpolation;
        if (HAL_DSP_OPS.resize(dsp_ctx, &rp) != HAL_OK)
        {
            (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(mid);
            goto fail;
        }
        intermediates_out.push_back(mid);
        cur_src = mid;
    }
    {
        HalDspResizeParams rp{};
        rp.src = cur_src;
        rp.dst = dst;
        rp.interpolation = interpolation;
        if (HAL_DSP_OPS.resize(dsp_ctx, &rp) != HAL_OK)
            goto fail;
    }
    return HAL_OK;
fail:
    for (auto *b : intermediates_out)
        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(b);
    intermediates_out.clear();
    return HAL_ERR_CHECK;
}

static void ml_clone_metadata_best_effort(const HalFrameBuffer *src, HalFrameBuffer *dst)
{
    if (!src || !dst)
        return;
    (void)HAL_FRAME_BUFFER_OPS.copy_metadata_from_frame_buffer(src, dst);
}

static inline uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo)
        return lo;
    if (v > hi)
        return hi;
    return v;
}

static inline float clamp_f(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static bool bbox_to_roi(float x0, float y0, float x1, float y1, uint32_t frame_w, uint32_t frame_h, HalDspRoi &roi_out)
{
    if (frame_w == 0 || frame_h == 0 || !(x1 > x0 && y1 > y0))
        return false;
    uint32_t sx = clamp_u32((uint32_t)std::floor(x0 * (float)frame_w), 0U, frame_w);
    uint32_t sy = clamp_u32((uint32_t)std::floor(y0 * (float)frame_h), 0U, frame_h);
    uint32_t ex = clamp_u32((uint32_t)std::ceil(x1 * (float)frame_w), 0U, frame_w);
    uint32_t ey = clamp_u32((uint32_t)std::ceil(y1 * (float)frame_h), 0U, frame_h);
    sx &= ~1U;
    sy &= ~1U;
    ex = (ex + 1U) & ~1U;
    ey = (ey + 1U) & ~1U;
    sx = clamp_u32(sx, 0U, frame_w);
    sy = clamp_u32(sy, 0U, frame_h);
    ex = clamp_u32(ex, 0U, frame_w);
    ey = clamp_u32(ey, 0U, frame_h);
    if (ex <= sx)
    {
        if (sx + 2U <= frame_w)
            ex = sx + 2U;
        else if (sx >= 2U)
            sx -= 2U, ex = sx + 2U;
    }
    if (ey <= sy)
    {
        if (sy + 2U <= frame_h)
            ey = sy + 2U;
        else if (sy >= 2U)
            sy -= 2U, ey = sy + 2U;
    }
    if (ex <= sx || ey <= sy)
        return false;
    roi_out.start_x = sx;
    roi_out.start_y = sy;
    roi_out.end_x = ex;
    roi_out.end_y = ey;
    return true;
}

/** Expand normalized bbox by @p margin_ratio (each side) for extra context, clamped to [0,1]. */
static bool det_bbox_to_roi_expanded(const HalBBox &bb, float margin_ratio, uint32_t fw, uint32_t fh, HalDspRoi &roi)
{
    const float mx = bb.w * margin_ratio;
    const float my = bb.h * margin_ratio;
    float x0 = bb.x - mx;
    float y0 = bb.y - my;
    float x1 = bb.x + bb.w + mx;
    float y1 = bb.y + bb.h + my;
    x0 = clamp_f(x0, 0.0f, 1.0f);
    y0 = clamp_f(y0, 0.0f, 1.0f);
    x1 = clamp_f(x1, 0.0f, 1.0f);
    y1 = clamp_f(y1, 0.0f, 1.0f);
    return bbox_to_roi(x0, y0, x1, y1, fw, fh, roi);
}

static bool build_nv12_inputs_from_frame(const HalFrameBuffer *frame, HalTensor *inputs, int &num_inputs_out)
{
    num_inputs_out = 0;
    if (!frame || !inputs)
        return false;
    if (frame->format != HAL_PIX_FMT_NV12 || frame->num_planes < 2 || !frame->planes[0] || !frame->planes[1])
        return false;
    HalTensor y{};
    y.data = (void *)frame->planes[0];
    y.ndim = 2;
    y.shape[0] = (int32_t)frame->height;
    y.shape[1] = (int32_t)frame->width;
    y.dtype = HAL_DTYPE_UINT8;
    y.byte_size = frame->sizes[0] ? frame->sizes[0] : (uint32_t)(frame->strides[0] * frame->height);
    y.dma_fd = frame->dma_fds[0];
    y.priv = frame->priv;
    HalTensor uv{};
    uv.data = (void *)frame->planes[1];
    uv.ndim = 2;
    uv.shape[0] = (int32_t)(frame->height / 2);
    uv.shape[1] = (int32_t)frame->width;
    uv.dtype = HAL_DTYPE_UINT8;
    uv.byte_size = frame->sizes[1] ? frame->sizes[1] : (uint32_t)(frame->strides[1] * (frame->height / 2));
    uv.dma_fd = frame->dma_fds[1];
    uv.priv = frame->priv;
    inputs[0] = y;
    inputs[1] = uv;
    num_inputs_out = 2;
    return true;
}

// Pack NV12 (two planes) into a single contiguous blob [Y | UV] for single-input NV12 HEFs.
static bool build_nv12_blob_tensor_from_frame(const HalFrameBuffer *frame, HalTensor &t_out, std::vector<uint8_t> &storage)
{
    std::memset(&t_out, 0, sizeof(t_out));
    if (!frame || frame->format != HAL_PIX_FMT_NV12 || frame->num_planes < 2 || !frame->planes[0] || !frame->planes[1])
        return false;
    if (frame->width == 0 || frame->height == 0)
        return false;

    const uint32_t w = frame->width;
    const uint32_t h = frame->height;
    const uint32_t y_stride = frame->strides[0] ? frame->strides[0] : w;
    const uint32_t uv_stride = frame->strides[1] ? frame->strides[1] : w;
    const uint32_t y_rows = h;
    const uint32_t uv_rows = h / 2U;
    const size_t y_bytes = (size_t)w * (size_t)y_rows;
    const size_t uv_bytes = (size_t)w * (size_t)uv_rows;
    const size_t total = y_bytes + uv_bytes;

    storage.resize(total);
    uint8_t *dst = storage.data();
    const uint8_t *y = (const uint8_t *)frame->planes[0];
    const uint8_t *uv = (const uint8_t *)frame->planes[1];

    // Copy Y
    for (uint32_t r = 0; r < y_rows; r++)
        std::memcpy(dst + (size_t)r * w, y + (size_t)r * y_stride, w);
    // Copy UV right after Y
    uint8_t *dst_uv = dst + y_bytes;
    for (uint32_t r = 0; r < uv_rows; r++)
        std::memcpy(dst_uv + (size_t)r * w, uv + (size_t)r * uv_stride, w);

    t_out.data = dst;
    t_out.ndim = 1;
    t_out.shape[0] = (int32_t)total;
    t_out.dtype = HAL_DTYPE_UINT8;
    t_out.byte_size = (uint32_t)total;
    t_out.dma_fd = -1;
    t_out.priv = nullptr;
    return true;
}

static bool build_rgb_tensor_from_frame(const HalFrameBuffer *frame, HalTensor &t_out)
{
    std::memset(&t_out, 0, sizeof(t_out));
    if (!frame || frame->num_planes < 1 || !frame->planes[0] || frame->width == 0 || frame->height == 0)
        return false;
    if (frame->format != HAL_PIX_FMT_RGB24 && frame->format != HAL_PIX_FMT_BGR24)
        return false;
    const uint32_t want = frame->width * frame->height * 3U;
    t_out.data = (void *)frame->planes[0];
    t_out.ndim = 1;
    t_out.shape[0] = (int32_t)want;
    t_out.dtype = HAL_DTYPE_UINT8;
    t_out.byte_size = want;
    t_out.dma_fd = frame->dma_fds[0];
    t_out.priv = frame->priv;
    return true;
}

static bool model_input_is_rgb_packed_single(const HalModelInfo &mi)
{
    if (mi.num_inputs != 1)
        return false;
    const auto &in = mi.inputs[0];
    if (in.ndim < 4)
        return false;
    const int32_t h = in.shape[1];
    const int32_t w = in.shape[2];
    if (h <= 0 || w <= 0)
        return false;
    const uint64_t expect = (uint64_t)w * (uint64_t)h * 3ULL;
    return (uint64_t)in.byte_size == expect;
}

static std::vector<std::string> load_charset_lines(const std::string &path)
{
    std::vector<std::string> out;
    auto load_default = [&]() {
        out.clear();
        out.emplace_back("blank");
        for (char c = '0'; c <= '9'; ++c)
            out.emplace_back(1, c);
        out.emplace_back(":");
        out.emplace_back(";");
        out.emplace_back("<");
        out.emplace_back("=");
        out.emplace_back(">");
        out.emplace_back("?");
        out.emplace_back("@");
        for (char c = 'A'; c <= 'Z'; ++c)
            out.emplace_back(1, c);
        out.emplace_back("[");
        out.emplace_back("\\");
        out.emplace_back("]");
        out.emplace_back("^");
        out.emplace_back("_");
        out.emplace_back("`");
        for (char c = 'a'; c <= 'z'; ++c)
            out.emplace_back(1, c);
        out.emplace_back("{");
        out.emplace_back("|");
        out.emplace_back("}");
        out.emplace_back("~");
        out.emplace_back("!");
        out.emplace_back("\"");
        out.emplace_back("#");
        out.emplace_back("$");
        out.emplace_back("%");
        out.emplace_back("&");
        out.emplace_back("'");
        out.emplace_back("(");
        out.emplace_back(")");
        out.emplace_back("*");
        out.emplace_back("+");
        out.emplace_back(",");
        out.emplace_back("-");
        out.emplace_back(".");
        out.emplace_back("/");
        out.emplace_back(" ");
        out.emplace_back(" "); // match hailo-apps default charset
    };

    if (path.empty())
    {
        load_default();
        return out;
    }
    std::ifstream in(path);
    if (!in.is_open())
    {
        load_default();
        return out;
    }
    std::string line;
    while (std::getline(in, line))
    {
        // keep empty lines as valid tokens? PaddleOCR dicts normally have no empties; skip empties.
        if (!line.empty() && (line.back() == '\r'))
            line.pop_back();
        if (line.empty())
            continue;

        // Guard: some resources (frequency dictionary) are "word <number>" per line.
        // If the suffix after the last space is all digits, keep only the word token.
        const auto sp = line.find_last_of(' ');
        if (sp != std::string::npos && sp + 1 < line.size())
        {
            bool all_digits = true;
            for (size_t i = sp + 1; i < line.size(); i++)
            {
                const char ch = line[i];
                if (ch < '0' || ch > '9')
                {
                    all_digits = false;
                    break;
                }
            }
            if (all_digits)
                line = line.substr(0, sp);
        }

        if (!line.empty())
            out.push_back(line);
    }
    if (out.empty())
    {
        load_default();
    }
    return out;
}

struct RecU8View
{
    const uint8_t *base = nullptr;
    uint32_t T = 0;
    uint32_t C = 0;
    bool time_major = false;     // false: NTC (T-major), true: NCT (C-major)
    uint32_t outer_stride = 0;   // bytes between consecutive outer indices (t or c)
};

static inline uint8_t rec_u8_at(const RecU8View &v, uint32_t t, uint32_t c)
{
    if (!v.base || v.T == 0 || v.C == 0)
        return 0;
    if (t >= v.T || c >= v.C)
        return 0;
    // NTC: [t][c] contiguous with optional padding per time-step row
    // NCT: [c][t] contiguous with optional padding per class column
    const size_t off = v.time_major ? ((size_t)c * (size_t)v.outer_stride + (size_t)t)
                                    : ((size_t)t * (size_t)v.outer_stride + (size_t)c);
    return v.base[off];
}

static std::string ctc_greedy_decode_u8_view(const RecU8View &v,
                                            int blank_index,
                                            const std::vector<std::string> &charset,
                                            float &out_conf)
{
    out_conf = 0.0f;
    if (!v.base || v.T == 0 || v.C == 0)
        return {};
    blank_index = std::max(0, std::min((int)v.C - 1, blank_index));

    std::string text;
    text.reserve(64);

    int prev = -1;
    double conf_sum = 0.0;
    uint32_t conf_cnt = 0;

    for (uint32_t t = 0; t < v.T; t++)
    {
        uint32_t best_c = 0;
        uint8_t best_v = 0;
        for (uint32_t c = 0; c < v.C; c++)
        {
            const uint8_t vv = rec_u8_at(v, t, c);
            if (vv > best_v)
            {
                best_v = vv;
                best_c = c;
            }
        }
        const int cur = (int)best_c;
        if (cur != blank_index && cur != prev)
        {
            if (!charset.empty() && (size_t)cur < charset.size())
                text += charset[(size_t)cur];
            else
                text += "?";
            conf_sum += (double)best_v / 255.0;
            conf_cnt++;
        }
        prev = cur;
    }
    out_conf = conf_cnt ? (float)(conf_sum / (double)conf_cnt) : 0.0f;
    return text;
}

static void rec_debug_dump_argmax_view_u8(const RecU8View &v_in,
                                          int blank_index, uint32_t head_steps, std::string &out)
{
    out.clear();
    if (!v_in.base || v_in.T == 0 || v_in.C == 0)
        return;
    blank_index = std::max(0, std::min((int)v_in.C - 1, blank_index));
    const uint32_t steps = std::min(head_steps, v_in.T);

    std::ostringstream oss;
    oss << "argmax_head(" << steps << "/" << v_in.T << ") blank=" << blank_index
        << " layout=" << (v_in.time_major ? "NCT" : "NTC")
        << " stride=" << v_in.outer_stride << " : ";
    for (uint32_t t = 0; t < steps; t++)
    {
        uint32_t best_c = 0;
        uint8_t best_v = 0;
        for (uint32_t c = 0; c < v_in.C; c++)
        {
            const uint8_t vv = rec_u8_at(v_in, t, c);
            if (vv > best_v)
            {
                best_v = vv;
                best_c = c;
            }
        }
        oss << t << "=" << best_c << "(" << std::fixed << std::setprecision(3) << ((float)best_v / 255.0f) << ")";
        if (t + 1 != steps)
            oss << ", ";
    }
    out = oss.str();
}

static bool infer_rec_T_C_from_output(const HalModelTensorInfo &oi, const HalTensor &tout,
                                     uint32_t &out_T, uint32_t &out_C)
{
    out_T = 0;
    out_C = 0;
    // Prefer HEF-provided shape. For FCR streams, HAL stores shape as NHWC-like:
    // [N=1, H=1, W=T, C=classes] or similar. We only need (T,C).
    if (oi.ndim >= 4 && oi.shape[0] > 0)
    {
        // If one dim is 1 and two dims are "small T" and "large C", choose accordingly.
        const int32_t d1 = oi.shape[1];
        const int32_t d2 = oi.shape[2];
        const int32_t d3 = oi.shape[3];
        // Common: [1,1,T,C]
        if (d1 == 1 && d2 > 0 && d3 > 0)
        {
            out_T = (uint32_t)d2;
            out_C = (uint32_t)d3;
            return true;
        }
        // Common NCHW-ish: [1,C,T,1] etc (rare); fall back to heuristic.
    }
    if (oi.ndim == 3 && oi.shape[0] == 1 && oi.shape[1] > 0 && oi.shape[2] > 0)
    {
        // Could be [1,T,C] or [1,C,T] - decide by magnitude.
        const uint32_t a = (uint32_t)oi.shape[1];
        const uint32_t b = (uint32_t)oi.shape[2];
        if (a <= b)
        {
            out_T = a;
            out_C = b;
        }
        else
        {
            out_T = b;
            out_C = a;
        }
        return true;
    }
    // Last resort (not recommended): derive C from byte_size/T but this can include padding.
    if (tout.byte_size > 0)
    {
        const uint32_t try_T = 40;
        if (try_T > 0 && (tout.byte_size % try_T == 0))
        {
            out_T = try_T;
            out_C = (uint32_t)(tout.byte_size / try_T);
            return true;
        }
    }
    return false;
}

static std::vector<std::string> lprnet_default_charset_11()
{
    // Align with TAPPAS AVAILABLE_CHARS: 10 digits + '-' (invalid/blank token).
    std::vector<std::string> c;
    c.reserve(11);
    for (int i = 0; i <= 9; i++)
        c.push_back(std::string(1, (char)('0' + i)));
    c.push_back(std::string("-")); // invalid char token
    return c;
}

static bool lprnet_decode_from_tensor_u8_best_effort(const HalModelTensorInfo &oi, const HalTensor &tout,
                                                     int blank_index, int charset_index_offset, bool time_major,
                                                     bool logits_are_softmax,
                                                     float min_score_threshold, int min_chars,
                                                     std::string &out_text, float &out_conf)
{
    out_text.clear();
    out_conf = 0.0f;
    if (!tout.data || tout.byte_size == 0)
        return false;
    if (tout.dtype != HAL_DTYPE_UINT8)
        return false;

    uint32_t T = 0, C = 0;
    if (!infer_rec_T_C_from_output(oi, tout, T, C) || T == 0 || C == 0)
        return false;

    RecU8View v{};
    v.base = (const uint8_t *)tout.data;
    v.T = T;
    v.C = C;
    v.time_major = time_major;
    v.outer_stride = time_major ? T : C;

    // TAPPAS-like decode:
    // - Dequantize to float using HalModelTensorInfo quant params when available.
    // - If outputs are logits: softmax per time-step.
    // - If outputs are already softmax/probabilities: normalize per time-step.
    // - Argmax per step, CTC de-dup.
    // - Treat the last class as an invalid token ("-") and skip it.
    // - Confidence is mean(max_prob_per_kept_char).
    if (min_score_threshold < 0.0f) min_score_threshold = 0.0f;
    if (min_score_threshold > 1.0f) min_score_threshold = 1.0f;
    if (min_chars < 0) min_chars = 0;

    const auto charset = lprnet_default_charset_11(); // "0123456789-"
    const int invalid_index_default = (int)charset.size() - 1;
    const int invalid_index = (blank_index >= 0) ? blank_index : invalid_index_default;

    const float scale = (oi.quant_scale > 0.0f) ? oi.quant_scale : (1.0f / 255.0f);
    const float zp = oi.quant_scale > 0.0f ? oi.quant_zero_point : 0.0f;
    (void)charset_index_offset; // unused in LPRNet digits-only mapping

    std::string decoded;
    decoded.reserve(32);
    double conf_sum = 0.0;
    uint32_t conf_cnt = 0;
    int prev = -1;

    // Temporary buffer for logits/probs per timestep (C is small: 11).
    float tmp[256];
    if (v.C > 256)
        return false;

    for (uint32_t t = 0; t < v.T; t++)
    {
        // Dequantize.
        float maxv = -1e30f;
        for (uint32_t c = 0; c < v.C; c++)
        {
            const float q = (float)rec_u8_at(v, t, c);
            const float v0 = (q - zp) * scale;
            tmp[c] = v0;
            if (v0 > maxv)
                maxv = v0;
        }
        float inv = 0.0f;
        if (!logits_are_softmax)
        {
            // Softmax (stable) over logits.
            float sum = 0.0f;
            for (uint32_t c = 0; c < v.C; c++)
            {
                const float e = std::exp(tmp[c] - maxv);
                tmp[c] = e;
                sum += e;
            }
            inv = (sum > 0.0f) ? (1.0f / sum) : 0.0f;
        }
        else
        {
            // Already probabilities-ish; clamp and renormalize.
            float sum = 0.0f;
            for (uint32_t c = 0; c < v.C; c++)
            {
                float p = tmp[c];
                if (!(p >= 0.0f)) p = 0.0f;
                tmp[c] = p;
                sum += p;
            }
            inv = (sum > 0.0f) ? (1.0f / sum) : 0.0f;
        }

        // Argmax on probs.
        uint32_t best_c = 0;
        float best_p = 0.0f;
        for (uint32_t c = 0; c < v.C; c++)
        {
            const float p = tmp[c] * inv;
            if (p > best_p)
            {
                best_p = p;
                best_c = c;
            }
        }

        const int cur = (int)best_c;
        if (cur == prev)
            continue;
        prev = cur;
        if (cur == invalid_index)
            continue;

        if ((size_t)cur < charset.size())
            decoded += charset[(size_t)cur];
        else
            decoded += "?";
        conf_sum += (double)best_p;
        conf_cnt++;
    }

    if (decoded.empty() || (int)decoded.size() < min_chars)
        return false;
    const float mean_conf = conf_cnt ? (float)(conf_sum / (double)conf_cnt) : 0.0f;
    if (mean_conf < min_score_threshold)
        return false;

    out_text = decoded;
    out_conf = mean_conf;
    return true;
}

static void rec_debug_dump_argmax_tensor_u8(const HalModelTensorInfo &oi, const HalTensor &tout,
                                           bool time_major, std::string &out)
{
    out.clear();
    uint32_t T = 0, C = 0;
    if (!infer_rec_T_C_from_output(oi, tout, T, C) || T == 0 || C == 0 || !tout.data || tout.dtype != HAL_DTYPE_UINT8)
        return;
    RecU8View v{};
    v.base = (const uint8_t *)tout.data;
    v.T = T;
    v.C = C;
    v.time_major = time_major;
    v.outer_stride = time_major ? T : C;
    rec_debug_dump_argmax_view_u8(v, 24, 0, out);
}

static void sanitize_lpr_publish(HalPostprocessResult &pr)
{
    pr.priv = nullptr;
    if (pr.type == HAL_POST_TYPE_OCR_RECOGNITION)
        pr.result.ocr.priv = nullptr;
    else if (pr.type == HAL_POST_TYPE_OCR_DETECTION || pr.type == HAL_POST_TYPE_DETECTION)
        pr.result.detection.priv = nullptr;
}

static void publish_merged_lpr(LprAppCtx *ctx, const HalPostprocessResult &pr)
{
    if (!ctx)
        return;
    HalPostprocessResult copy = pr;
    sanitize_lpr_publish(copy);
    std::lock_guard<std::mutex> lk(ctx->result.mu);
    const bool has_payload =
        (copy.type == HAL_POST_TYPE_OCR_RECOGNITION && copy.result.ocr.num_lines > 0) ||
        (copy.type == HAL_POST_TYPE_OCR_DETECTION && copy.result.detection.num_detections > 0) ||
        (copy.type == HAL_POST_TYPE_DETECTION && copy.result.detection.num_detections > 0);

    /* Prevent OSD flicker: do not overwrite last with empty results every frame.
     * Preview thread will keep drawing last_valid for a short hold window. */
    if (!has_payload)
    {
        ctx->result.valid = false;
        ctx->result.cv.notify_all();
        return;
    }
    ctx->result.last = copy;
    ctx->result.seq++;
    ctx->result.valid = true;
    ctx->result.last_valid = copy;
    ctx->result.last_valid_seq = ctx->result.seq;
    ctx->result.has_valid = true;
    ctx->result.last_valid_tp = std::chrono::steady_clock::now();
    ctx->result.cv.notify_all();
}

static void lpr_ai_worker_loop(LprAppCtx *ctx)
{
    if (!ctx || !ctx->det_infer || !ctx->det_post || !ctx->dsp_ctx)
        return;
    // Recognition stage can run either via vendor postprocess (`rec_post`) or via local CTC decode.
    if (!ctx->det_only && (!ctx->rec_infer || (!ctx->rec_post && !ctx->rec_local_decode)))
        return;

    while (!g_stop.load(std::memory_order_acquire))
    {
        using Clock = std::chrono::steady_clock;
        auto t_frame0 = Clock::now();
        uint64_t det_infer_us = 0;
        uint64_t det_post_us = 0;
        uint64_t rec_crop_us = 0;
        uint64_t rec_infer_us = 0;
        uint64_t rec_post_us = 0;
        uint32_t rec_rois = 0;

        SharedFrame *sf = nullptr;
        {
            std::unique_lock<std::mutex> lk(ctx->q_mu);
            ctx->q_cv.wait(lk, [&] { return g_stop.load(std::memory_order_acquire) || !ctx->q.empty(); });
            if (g_stop.load(std::memory_order_acquire))
                break;
            sf = ctx->q.front();
            ctx->q.pop_front();
        }
        if (!sf)
            continue;

        HalFrameBuffer *frame = &sf->fb;
        const uint32_t fw = frame->width;
        const uint32_t fh = frame->height;

        /* NOTE: HalPostprocessResult contains a very large union (keypoints arm is multi‑MB).
         * Allocate on heap to avoid overflowing small pthread stacks on embedded targets. */
        auto merged_holder = std::make_unique<HalPostprocessResult>();
        HalPostprocessResult &merged = *merged_holder;
        std::memset(&merged, 0, sizeof(merged));
        merged.type = HAL_POST_TYPE_OCR_RECOGNITION;
        merged.result.ocr.num_lines = 0;
        merged.priv = nullptr;
        merged.result.ocr.priv = nullptr;

        // ----- Stage A: license plate detector (YOLO) -----
        HalFrameBuffer *det_in = nullptr;
        std::vector<HalFrameBuffer *> det_chain;
        if (fw == ctx->det_w && fh == ctx->det_h)
        {
            det_in = frame;
        }
        else
        {
            HalFrameBufferRequest dreq{};
            dreq.width = ctx->det_w;
            dreq.height = ctx->det_h;
            dreq.format = HAL_PIX_FMT_NV12;
            dreq.mem_type = HAL_MEM_DMABUF;
            dreq.zero_initialize = false;
            if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&dreq, &det_in) == HAL_OK && det_in)
            {
                ml_clone_metadata_best_effort(frame, det_in);
                if (dsp_resize_chain(ctx->dsp_ctx, frame, det_in, HAL_DSP_INTERPOLATION_BILINEAR, det_chain) != HAL_OK)
                {
                    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(det_in);
                    det_in = nullptr;
                }
            }
        }

        std::vector<HalDetection> text_boxes;
        if (det_in)
        {
            HalTensor din{};
            HalTensor din2[2]{};
            int dnin = 1;
            HalTensor dout[HAL_MAX_TENSORS]{};
            int dnum_out = 0;
            uint32_t want_in = 1;
            bool use_nv12_planes = false;
            HalModelInfo dmi{};
            bool det_wants_rgb = false;
            {
                if (HAL_INFERENCE_OPS.get_model_info(ctx->det_infer, &dmi) == HAL_OK)
                {
                    dnum_out = (int)((dmi.num_outputs <= HAL_MAX_TENSORS) ? dmi.num_outputs : HAL_MAX_TENSORS);
                    want_in = dmi.num_inputs ? dmi.num_inputs : 1U;
                    use_nv12_planes = (dmi.num_inputs == 2 && dmi.inputs[0].ndim == 2 && dmi.inputs[1].ndim == 2);
                    det_wants_rgb = model_input_is_rgb_packed_single(dmi);
                }
            }
            // Ensure output tensor names are populated (vendor postprocess may look up tensors by name).
            for (int i = 0; i < dnum_out; i++)
            {
                if (dmi.outputs[i].name[0] != '\0')
                    std::snprintf(dout[i].name, sizeof(dout[i].name), "%s", dmi.outputs[i].name);
            }
            int dirc = HAL_ERR_NOT_SUPPORTED;

            HalFrameBuffer *det_rgb = nullptr;
            if (det_wants_rgb)
            {
                HalFrameBufferRequest rreq{};
                rreq.width = det_in->width;
                rreq.height = det_in->height;
                rreq.format = pick_packed_pixfmt_for_mode(ctx->det_infer, ctx->det_packed_mode);
                rreq.mem_type = HAL_MEM_DMABUF;
                rreq.zero_initialize = false;
                if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&rreq, &det_rgb) == HAL_OK && det_rgb)
                {
                    HalDspConvertFormatParams cfp{};
                    cfp.src = det_in;
                    cfp.dst = det_rgb;
                    if (HAL_DSP_OPS.convert_format(ctx->dsp_ctx, &cfp) != HAL_OK)
                    {
                        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(det_rgb);
                        det_rgb = nullptr;
                    }
                }
                if (det_rgb)
                {
                    HalTensor rin{};
                    if (build_rgb_tensor_from_frame(det_rgb, rin))
                    {
                        const auto t0 = Clock::now();
                        dirc = HAL_INFERENCE_OPS.run(ctx->det_infer, &rin, 1, dout, dnum_out);
                        det_infer_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                    }
                }
            }
            else if (use_nv12_planes && build_nv12_inputs_from_frame(det_in, din2, dnin))
            {
                const auto t0 = Clock::now();
                dirc = HAL_INFERENCE_OPS.run(ctx->det_infer, din2, dnin, dout, dnum_out);
                det_infer_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
            }
            else if (want_in == 1 && HAL_INFERENCE_OPS.tensor_from_frame(det_in, &din) == HAL_OK)
            {
                const auto t0 = Clock::now();
                dirc = HAL_INFERENCE_OPS.run(ctx->det_infer, &din, 1, dout, dnum_out);
                det_infer_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                HAL_INFERENCE_OPS.free_tensor(&din);
            }

            if (dirc == HAL_OK && dnum_out > 0)
            {
                bool got_boxes = false;
                if (HAL_POSTPROCESS_OPS.run_dyn && HAL_POSTPROCESS_OPS.free_result_dyn)
                {
                    HalPostprocessResultDyn dpr{};
                    const auto t0 = Clock::now();
                    const int dprc = HAL_POSTPROCESS_OPS.run_dyn(ctx->det_post, dout, dnum_out, &dpr);
                    det_post_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                    if (dprc == HAL_OK && dpr.type == HAL_POST_TYPE_DETECTION &&
                        dpr.result.detection.num_detections > 0 && dpr.result.detection.detections)
                    {
                        text_boxes.reserve(dpr.result.detection.num_detections);
                        for (uint32_t di = 0; di < dpr.result.detection.num_detections; di++)
                            text_boxes.push_back(dpr.result.detection.detections[di]);
                        got_boxes = true;
                    }
                    else if (ctx->verbose)
                    {
                        HAL_LOG_INFO("lpr_example_v2: det run_dyn rc=%d type=%d n=%u",
                                     dprc, (int)dpr.type,
                                     (unsigned)(dpr.type == HAL_POST_TYPE_DETECTION ? dpr.result.detection.num_detections : 0u));
                    }
                    HAL_POSTPROCESS_OPS.free_result_dyn(&dpr);
                }

                if (!got_boxes && ctx->verbose)
                    HAL_LOG_INFO("lpr_example_v2: det dyn post produced 0 boxes (no local fallback)");
            }
            for (int i = 0; i < dnum_out; i++)
                if (dout[i].data)
                    HAL_INFERENCE_OPS.free_tensor(&dout[i]);

            if (det_rgb)
                (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(det_rgb);
            if (det_in != frame)
                (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(det_in);
            for (auto *b : det_chain)
                (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(b);
        }

        std::sort(text_boxes.begin(), text_boxes.end(),
                  [](const HalDetection &a, const HalDetection &b) { return a.confidence > b.confidence; });

        if (ctx->det_only)
        {
            // Det-only mode: publish plate boxes (YOLO detection post).
            auto dres_holder = std::make_unique<HalPostprocessResult>();
            HalPostprocessResult &dres = *dres_holder;
            std::memset(&dres, 0, sizeof(dres));
            dres.type = HAL_POST_TYPE_DETECTION;
            dres.priv = nullptr;
            dres.result.detection.num_detections = 0;
            dres.result.detection.priv = nullptr;
            for (const auto &d : text_boxes)
            {
                if (dres.result.detection.num_detections >= HAL_MAX_DETECTIONS)
                    break;
                dres.result.detection.detections[dres.result.detection.num_detections++] = d;
            }
            publish_merged_lpr(ctx, dres);
            sharedframe_unref(sf);
            continue;
        }

        constexpr float kCropMargin = 0.05f;

        if (ctx->verbose)
            HAL_LOG_INFO("lpr_example_v2: processing %zu plate candidates", text_boxes.size());

        for (const HalDetection &td : text_boxes)
        {
            if (merged.result.ocr.num_lines >= ctx->max_text_regions || merged.result.ocr.num_lines >= HAL_MAX_OCR_LINES)
                break;
            if (td.confidence < ctx->min_det_confidence)
                continue;

            HalDspRoi roi{};
            if (!det_bbox_to_roi_expanded(td.bbox, kCropMargin, fw, fh, roi))
                continue;

            const uint32_t roi_w = roi.end_x - roi.start_x;
            const uint32_t roi_h = roi.end_y - roi.start_y;
            if (roi_w < 4 || roi_h < 4)
                continue;

            /* NV12 (YUV420) requires even WxH for any DSP dst buffer.
             * Some recognition models (e.g., LPRNet 300x75 RGB) have odd H.
             * We therefore compute an even-sized NV12 staging target for crop/resize,
             * then later (RGB24) resize down to the exact model WxH if needed. */
            const uint32_t rec_w_nv12 = align_up_even_u32(ctx->rec_w);
            const uint32_t rec_h_nv12 = align_up_even_u32(ctx->rec_h);

            uint32_t mid_w = std::max(rec_w_nv12, ceil_div_u32(roi_w, 16U));
            uint32_t mid_h = std::max(rec_h_nv12, ceil_div_u32(roi_h, 16U));
            mid_w = align_up_even_u32(mid_w);
            mid_h = align_up_even_u32(mid_h);

            HalFrameBuffer *mid = nullptr;
            HalFrameBufferRequest mreq{};
            mreq.width = mid_w;
            mreq.height = mid_h;
            mreq.format = HAL_PIX_FMT_NV12;
            mreq.mem_type = HAL_MEM_DMABUF;
            mreq.zero_initialize = false;
            if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&mreq, &mid) != HAL_OK || !mid)
                continue;
            ml_clone_metadata_best_effort(frame, mid);
            HalDspCropResizeParams cp{};
            cp.src = frame;
            cp.dst = mid;
            cp.crop = roi;
            cp.interpolation = HAL_DSP_INTERPOLATION_BILINEAR;
            // Recognition is sensitive to aspect ratio. Prefer "scale to fit" with letterbox padding.
            // This matches the common PaddleOCR preprocessing: fixed target H, width padded to max.
            cp.scaling_mode = ctx->rec_scaling_mode;
            cp.letterbox_alignment =
                (ctx->rec_scaling_mode == HAL_DSP_SCALING_LETTERBOX_UP_LEFT) ? HAL_DSP_LETTERBOX_UP_LEFT :
                (ctx->rec_scaling_mode == HAL_DSP_SCALING_LETTERBOX_MIDDLE) ? HAL_DSP_LETTERBOX_MIDDLE :
                                                                              HAL_DSP_LETTERBOX_NONE;
            // Black padding tends to preserve character contrast better for LPR.
            cp.letterbox_color = HalDspColor{.y = 0, .u = 128, .v = 128};
            {
                const auto t0 = Clock::now();
                const int cprc = HAL_DSP_OPS.crop_and_resize(ctx->dsp_ctx, &cp);
                rec_crop_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                if (cprc != HAL_OK)
                {
                    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(mid);
                    continue;
                }
            }

            HalFrameBuffer *rec_in = nullptr;
            std::vector<HalFrameBuffer *> rec_chain;
            if (mid_w == ctx->rec_w && mid_h == ctx->rec_h)
            {
                rec_in = mid;
            }
            else
            {
                HalFrameBufferRequest lreq{};
                /* NV12 requires even WxH. Some RGB models (e.g., LPRNet 300x75) have odd H;
                 * we therefore allocate an even-sized NV12 staging buffer, then (for RGB models)
                 * convert+resize to the exact model H in RGB24. */
                lreq.width = rec_w_nv12;
                lreq.height = rec_h_nv12;
                lreq.format = HAL_PIX_FMT_NV12;
                lreq.mem_type = HAL_MEM_DMABUF;
                lreq.zero_initialize = false;
                if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&lreq, &rec_in) == HAL_OK && rec_in)
                {
                    ml_clone_metadata_best_effort(frame, rec_in);
                    if (dsp_resize_chain(ctx->dsp_ctx, mid, rec_in, HAL_DSP_INTERPOLATION_BILINEAR, rec_chain) != HAL_OK)
                    {
                        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(rec_in);
                        rec_in = nullptr;
                    }
                }
            }

            if (rec_in)
            {
                HalTensor lin{};
                HalTensor lin2[2]{};
                int lnin = 1;
                HalTensor lout[HAL_MAX_TENSORS]{};
                int lnum_out = 0;
                uint32_t want_in = 1;
                bool use_nv12_planes = false;
                HalModelInfo lmi{};
                bool rec_wants_rgb = false;
                {
                    if (HAL_INFERENCE_OPS.get_model_info(ctx->rec_infer, &lmi) == HAL_OK)
                    {
                        lnum_out = (int)((lmi.num_outputs <= HAL_MAX_TENSORS) ? lmi.num_outputs : HAL_MAX_TENSORS);
                        want_in = lmi.num_inputs ? lmi.num_inputs : 1U;
                        use_nv12_planes = (lmi.num_inputs == 2 && lmi.inputs[0].ndim == 2 && lmi.inputs[1].ndim == 2);
                        rec_wants_rgb = ctx->rec_force_nv12 ? false : model_input_is_rgb_packed_single(lmi);
                    }
                }
                // Ensure output tensor names are populated (vendor postprocess may look up tensors by name).
                for (int i = 0; i < lnum_out; i++)
                {
                    if (lmi.outputs[i].name[0] != '\0')
                        std::snprintf(lout[i].name, sizeof(lout[i].name), "%s", lmi.outputs[i].name);
                }
                int lirc = HAL_ERR_NOT_SUPPORTED;
                HalFrameBuffer *rec_rgb = nullptr;
                HalFrameBuffer *rec_rgb_exact = nullptr;
                if (rec_wants_rgb)
                {
                    HalFrameBufferRequest rreq{};
                    rreq.width = rec_in->width;
                    rreq.height = rec_in->height;
                    rreq.format = pick_packed_pixfmt_for_mode(ctx->rec_infer, ctx->rec_packed_mode);
                    rreq.mem_type = HAL_MEM_DMABUF;
                    rreq.zero_initialize = false;
                    if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&rreq, &rec_rgb) == HAL_OK && rec_rgb)
                    {
                        HalDspConvertFormatParams cfp{};
                        cfp.src = rec_in;
                        cfp.dst = rec_rgb;
                        if (HAL_DSP_OPS.convert_format(ctx->dsp_ctx, &cfp) != HAL_OK)
                        {
                            (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(rec_rgb);
                            rec_rgb = nullptr;
                        }
                    }
                    /* If model expects odd H (e.g. 75), NV12 staging forces even H (e.g. 76).
                     * Resize RGB24 to the exact model WxH before feeding the network. */
                    if (rec_rgb && (rec_rgb->width != ctx->rec_w || rec_rgb->height != ctx->rec_h))
                    {
                        HalFrameBufferRequest ereq{};
                        ereq.width = ctx->rec_w;
                        ereq.height = ctx->rec_h;
                        ereq.format = pick_packed_pixfmt_for_mode(ctx->rec_infer, ctx->rec_packed_mode);
                        ereq.mem_type = HAL_MEM_DMABUF;
                        ereq.zero_initialize = false;
                        if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&ereq, &rec_rgb_exact) == HAL_OK && rec_rgb_exact)
                        {
                            HalDspResizeParams rp{};
                            rp.src = rec_rgb;
                            rp.dst = rec_rgb_exact;
                            rp.interpolation = HAL_DSP_INTERPOLATION_BILINEAR;
                            if (HAL_DSP_OPS.resize(ctx->dsp_ctx, &rp) != HAL_OK)
                            {
                                (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(rec_rgb_exact);
                                rec_rgb_exact = nullptr;
                            }
                        }
                    }

                    HalFrameBuffer *rgb_for_infer = rec_rgb_exact ? rec_rgb_exact : rec_rgb;
                    if (rgb_for_infer)
                    {
                        HalTensor rin{};
                        if (build_rgb_tensor_from_frame(rgb_for_infer, rin))
                        {
                            const auto t0 = Clock::now();
                            lirc = HAL_INFERENCE_OPS.run(ctx->rec_infer, &rin, 1, lout, lnum_out);
                            rec_infer_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                        }
                    }
                }
                else if (use_nv12_planes && build_nv12_inputs_from_frame(rec_in, lin2, lnin))
                {
                    const auto t0 = Clock::now();
                    lirc = HAL_INFERENCE_OPS.run(ctx->rec_infer, lin2, lnin, lout, lnum_out);
                    rec_infer_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                }
                else if (want_in == 1)
                {
                    // For single-input NV12 HEFs, pack Y+UV into a contiguous blob to avoid DMABUF plane contiguity issues.
                    static thread_local std::vector<uint8_t> nv12_blob;
                    if (build_nv12_blob_tensor_from_frame(rec_in, lin, nv12_blob))
                    {
                        const auto t0 = Clock::now();
                        lirc = HAL_INFERENCE_OPS.run(ctx->rec_infer, &lin, 1, lout, lnum_out);
                        rec_infer_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                    }
                    else if (HAL_INFERENCE_OPS.tensor_from_frame(rec_in, &lin) == HAL_OK)
                    {
                        const auto t0 = Clock::now();
                        lirc = HAL_INFERENCE_OPS.run(ctx->rec_infer, &lin, 1, lout, lnum_out);
                        rec_infer_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                        HAL_INFERENCE_OPS.free_tensor(&lin);
                    }
                }

                if (lirc == HAL_OK && lnum_out > 0)
                {
                    bool got_line = false;

                    // Prefer local LPRNet decode (vendor libocr_post.so on this image may be PaddleOCR-only).
                    if (ctx->rec_local_decode)
                    {
                        HalModelInfo mi{};
                        HalModelTensorInfo oi{};
                        bool have_oi = false;
                        if (HAL_INFERENCE_OPS.get_model_info(ctx->rec_infer, &mi) == HAL_OK && mi.num_outputs > 0)
                        {
                            oi = mi.outputs[0];
                            have_oi = true;
                        }
                        const HalModelTensorInfo &use_oi = have_oi ? oi : HalModelTensorInfo{};
                        std::string text;
                        float conf = 0.0f;
                        std::string dbg;
                        if (ctx->rec_debug && ctx->verbose)
                        {
                            rec_debug_dump_argmax_tensor_u8(use_oi, lout[0], ctx->rec_time_major, dbg);
                            if (!dbg.empty())
                                HAL_LOG_INFO("lpr_example_v2: rec debug argmax: %s", dbg.c_str());
                        }
                        if (lprnet_decode_from_tensor_u8_best_effort(use_oi, lout[0],
                                                                     ctx->rec_blank_index,
                                                                     ctx->rec_charset_index_offset,
                                                                     ctx->rec_time_major,
                                                                     ctx->rec_logits_are_softmax,
                                                                     ctx->rec_min_score_threshold,
                                                                     ctx->rec_min_chars,
                                                                     text, conf))
                        {
                            HalOcrLine &ol = merged.result.ocr.lines[merged.result.ocr.num_lines++];
                            ol.bbox = td.bbox;
                            ol.track_id = -1;
                            std::snprintf(ol.text, sizeof(ol.text), "%s", text.c_str());
                            ol.confidence = conf;
                            got_line = true;
                            if (ctx->verbose)
                                HAL_LOG_INFO("lpr_example_v2: rec local text=\"%s\" conf=%.3f", ol.text, ol.confidence);
                        }
                    }

                    if (!got_line && ctx->rec_post)
                    {
                        if (HAL_POSTPROCESS_OPS.run_dyn && HAL_POSTPROCESS_OPS.free_result_dyn)
                        {
                            HalPostprocessResultDyn rpr{};
                            const auto t0 = Clock::now();
                            const int prc = HAL_POSTPROCESS_OPS.run_dyn(ctx->rec_post, lout, lnum_out, &rpr);
                            rec_post_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                            if (prc == HAL_OK && rpr.type == HAL_POST_TYPE_OCR_RECOGNITION &&
                                rpr.result.ocr.num_lines > 0 && rpr.result.ocr.lines)
                            {
                                rec_rois++;
                                for (uint32_t li = 0; li < rpr.result.ocr.num_lines; ++li)
                                {
                                    if (merged.result.ocr.num_lines >= ctx->max_text_regions ||
                                        merged.result.ocr.num_lines >= HAL_MAX_OCR_LINES)
                                        break;
                                    const HalOcrLine &src = rpr.result.ocr.lines[li];
                                    if (src.text[0] == '\0')
                                        continue;
                                    HalOcrLine &ol = merged.result.ocr.lines[merged.result.ocr.num_lines++];
                                    ol.bbox = td.bbox;
                                    ol.track_id = src.track_id;
                                    std::snprintf(ol.text, sizeof(ol.text), "%s", src.text);
                                    ol.confidence = src.confidence;
                                    got_line = true;
                                }
                                if (ctx->verbose && got_line)
                                {
                                    const HalOcrLine &ol = merged.result.ocr.lines[merged.result.ocr.num_lines - 1];
                                    HAL_LOG_INFO("lpr_example_v2: rec dyn post text=\"%s\" conf=%.3f", ol.text, ol.confidence);
                                }
                            }
                            else if (ctx->verbose)
                            {
                                HAL_LOG_INFO("lpr_example_v2: rec run_dyn rc=%d type=%d lines=%u (fallback local CTC)",
                                             prc, (int)rpr.type,
                                             (unsigned)(rpr.type == HAL_POST_TYPE_OCR_RECOGNITION ? rpr.result.ocr.num_lines : 0u));
                            }
                            HAL_POSTPROCESS_OPS.free_result_dyn(&rpr);
                        }
                    }

                    if (!got_line && ctx->verbose)
                        HAL_LOG_INFO("lpr_example_v2: rec produced 0 lines");
                }
                for (int i = 0; i < lnum_out; i++)
                    if (lout[i].data)
                        HAL_INFERENCE_OPS.free_tensor(&lout[i]);

                if (rec_rgb)
                    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(rec_rgb);
                if (rec_rgb_exact)
                    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(rec_rgb_exact);
                for (auto *b : rec_chain)
                    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(b);
                if (rec_in != mid)
                    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(rec_in);
            }

            if (mid)
                (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(mid);
        }

        publish_merged_lpr(ctx, merged);
        sharedframe_unref(sf);

        static thread_local uint64_t stat_frames = 0;
        static thread_local uint64_t sum_det_infer = 0, sum_det_post = 0, sum_crop = 0, sum_rec_infer = 0, sum_rec_post = 0;
        stat_frames++;
        sum_det_infer += det_infer_us;
        sum_det_post += det_post_us;
        sum_crop += rec_crop_us;
        sum_rec_infer += rec_infer_us;
        sum_rec_post += rec_post_us;

        const uint64_t frame_us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t_frame0).count();
        const uint64_t every = ctx->verbose ? 10ULL : 30ULL;
        if ((stat_frames % every) == 0)
        {
            const double denom = (double)every;
            HAL_LOG_INFO("lpr_example_v2 perf avg over %llu frames: det_infer=%.2fms det_post=%.2fms crop=%.2fms rec_infer=%.2fms rec_post=%.2fms frame=%.2fms rec_rois=%u",
                         (unsigned long long)every,
                         (double)sum_det_infer / 1000.0 / denom,
                         (double)sum_det_post / 1000.0 / denom,
                         (double)sum_crop / 1000.0 / denom,
                         (double)sum_rec_infer / 1000.0 / denom,
                         (double)sum_rec_post / 1000.0 / denom,
                         (double)frame_us / 1000.0,
                         (unsigned)rec_rois);
            sum_det_infer = sum_det_post = sum_crop = sum_rec_infer = sum_rec_post = 0;
        }
    }
}

static void lpr_preview_worker_loop(LprAppCtx *ctx)
{
    if (!ctx || !ctx->codec_ctx)
        return;
    uint64_t last_seq = 0;
    /* Hold last_valid long enough to avoid OSD flicker when det/rec occasionally produce empty output. */
    const auto hold_ms = std::chrono::milliseconds(2000);

    while (!g_stop.load(std::memory_order_acquire))
    {
        SharedFrame *sf = nullptr;
        {
            std::unique_lock<std::mutex> lk(ctx->preview_mu);
            ctx->preview_cv.wait(lk, [&] { return g_stop.load(std::memory_order_acquire) || !ctx->preview_q.empty(); });
            if (g_stop.load(std::memory_order_acquire))
                break;
            sf = ctx->preview_q.front();
            ctx->preview_q.pop_front();
        }
        if (!sf)
            continue;

        FrameJob job{};
        job.frame = &sf->fb;

        auto pr_holder = std::make_unique<HalPostprocessResult>();
        HalPostprocessResult &pr = *pr_holder;
        std::memset(&pr, 0, sizeof(pr));
        bool have_pr = false;
        bool use_hold = false;
        {
            std::unique_lock<std::mutex> lk(ctx->result.mu);
            ctx->result.cv.wait_for(lk, std::chrono::milliseconds(10), [&] {
                return g_stop.load(std::memory_order_acquire) || (ctx->result.valid && ctx->result.seq != last_seq);
            });
            if (ctx->result.valid && ctx->result.seq != last_seq)
            {
                pr = ctx->result.last;
                last_seq = ctx->result.seq;
                have_pr = true;
            }
            else if (ctx->result.has_valid)
            {
                auto now = std::chrono::steady_clock::now();
                if (now - ctx->result.last_valid_tp <= hold_ms)
                {
                    pr = ctx->result.last_valid;
                    use_hold = true;
                }
            }
        }

        if (have_pr || use_hold)
            (void)HAL_DRAW_OPS.draw_result(&pr, job.frame, &ctx->draw_cfg);

        (void)HAL_CODEC_OPS.input_frame(ctx->codec_ctx, job.frame);
        sharedframe_unref(sf);
    }
}

static bool parse_host_port(const std::string &s, std::string &host, uint16_t &port)
{
    const auto pos = s.find(':');
    if (pos == std::string::npos)
        return false;
    host = s.substr(0, pos);
    port = (uint16_t)std::atoi(s.substr(pos + 1).c_str());
    return !host.empty() && port != 0;
}

static bool is_number(const std::string &s)
{
    if (s.empty())
        return false;
    for (char c : s)
        if (!std::isdigit((unsigned char)c))
            return false;
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    std::string media_json;
    std::string profile_arg;
    std::string det_hef;
    std::string det_post_file;
    std::string det_post_json;
    std::string rec_hef;
    std::string rec_post_file;
    std::string rec_post_json;
    std::string rec_backend = "lprnet";
    std::string udp_arg = "127.0.0.1:5004";
    bool verbose = false;
    uint32_t max_regions = 8;
    float min_det_conf = 0.25f;
    std::string rec_charset;
    int rec_blank = -1;
    bool rec_time_major = false;
    int rec_dict_offset = 0;
    float rec_min_score = 0.55f;
    int rec_min_chars = 4;
    std::string rec_scaling = "stretch";
    bool rec_debug = false;
    std::string det_packed_format = "rgb";
    std::string rec_packed_format = "rgb";

    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        auto need = [&](const char *opt) -> const char * {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "Missing value for %s\n", opt);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--media")
            media_json = need("--media");
        else if (a == "--profile")
            profile_arg = need("--profile");
        else if (a == "--lp-det-hef" || a == "--ocr-det-hef")
            det_hef = need(a.c_str());
        else if (a == "--lp-det-post-file" || a == "--ocr-det-post-file")
            det_post_file = need(a.c_str());
        else if (a == "--lp-det-post-json" || a == "--ocr-det-post-json")
            det_post_json = need(a.c_str());
        else if (a == "--lp-rec-hef" || a == "--ocr-rec-hef")
            rec_hef = need(a.c_str());
        else if (a == "--lp-rec-post-file" || a == "--ocr-rec-post-file")
            rec_post_file = need(a.c_str());
        else if (a == "--lp-rec-post-json" || a == "--ocr-rec-post-json")
            rec_post_json = need(a.c_str());
        else if (a == "--rec-backend")
            rec_backend = need("--rec-backend");
        else if (a == "--udp")
            udp_arg = need("--udp");
        else if (a == "--verbose")
            verbose = true;
        else if (a == "--max-plates" || a == "--max-text-regions")
            max_regions = (uint32_t)std::max(1, (int)std::strtoul(need(a.c_str()), nullptr, 10));
        else if (a == "--min-det-confidence")
            min_det_conf = (float)std::atof(need("--min-det-confidence"));
        else if (a == "--rec-charset")
            rec_charset = need("--rec-charset");
        else if (a == "--rec-blank-index")
            rec_blank = std::atoi(need("--rec-blank-index"));
        else if (a == "--rec-dict-index-offset")
            rec_dict_offset = std::atoi(need("--rec-dict-index-offset"));
        else if (a == "--rec-time-major")
            rec_time_major = true;
        else if (a == "--rec-min-score")
            rec_min_score = (float)std::atof(need("--rec-min-score"));
        else if (a == "--rec-min-chars")
            rec_min_chars = std::atoi(need("--rec-min-chars"));
        else if (a == "--rec-scaling")
            rec_scaling = need("--rec-scaling");
        else if (a == "--rec-debug")
            rec_debug = true;
        else if (a == "--det-packed-format")
            det_packed_format = need("--det-packed-format");
        else if (a == "--rec-packed-format")
            rec_packed_format = need("--rec-packed-format");
        else if (a == "--help" || a == "-h")
        {
            std::printf(
                "hal-lpr-example-v2: license plate detection (YOLO) + LPRNet OCR\n"
                "  --media <medialib.json> --profile <name_or_idx>\n"
                "  --lp-det-hef <tiny_yolov4_license_plates.hef> [--lp-det-post-file <yolov4_license_plate.json>]\n"
                "  [--lp-det-post-json '<obj>'] [--lp-rec-hef <lprnet.hef> [--lp-rec-post-file <lprnet.json>] ...]\n"
                "  [--udp host:port] [--verbose] [--max-plates N] [--min-det-confidence x]\n"
                "  [--rec-charset <dict.txt>] [--rec-dict-index-offset N] [--rec-blank-index N] [--rec-time-major]\n"
                "  [--rec-min-score x] [--rec-min-chars N]\n"
                "  [--rec-scaling stretch|letterbox_middle|letterbox_up_left|scale_and_crop] [--rec-debug]\n"
                "  [--det-packed-format rgb|bgr|auto] [--rec-packed-format rgb|bgr|auto]\n"
                "  [--rec-backend lprnet|ppocrv5]\n"
                "Aliases: --ocr-det-hef / --ocr-rec-hef (same as --lp-*).\n");
            return 0;
        }
    }

    if (det_hef.empty())
    {
        std::fprintf(stderr, "Error: require --lp-det-hef (--media optional; omit to use HAL embedded default) (see --help)\n");
        return 2;
    }

    std::string host;
    uint16_t port = 0;
    if (!parse_host_port(udp_arg, host, port))
    {
        std::fprintf(stderr, "Error: invalid --udp\n");
        return 2;
    }

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);

    /* Do not allocate LprAppCtx on the stack: LprSharedResult embeds two HalPostprocessResult unions whose
     * largest member (HalKeypointResult) is on the order of megabytes — exceeds typical main-thread stacks. */
    auto ctx_holder = std::make_unique<LprAppCtx>();
    LprAppCtx &ctx = *ctx_holder;
    ctx.verbose = verbose;
    ctx.max_text_regions = max_regions;
    ctx.min_det_confidence = min_det_conf;
    ctx.rec_charset_path = rec_charset;
    ctx.rec_blank_index = rec_blank;
    ctx.rec_time_major = rec_time_major;
    ctx.det_only = rec_hef.empty();

    HalMediaConfig mcfg{};
    mcfg.config_path = media_json.empty() ? nullptr : media_json.c_str();
    mcfg.config_json = nullptr;
    mcfg.image_config = {};

    int rc = HAL_MEDIA_OPS.init(&mcfg, &ctx.media_ctx);
    if (rc != HAL_OK || !ctx.media_ctx)
    {
        std::fprintf(stderr, "HAL_MEDIA_OPS.init failed rc=%d\n", rc);
        return 1;
    }

    if (!profile_arg.empty())
    {
        if (is_number(profile_arg))
        {
            char *plist[64]{};
            uint32_t pcount = 0;
            rc = HAL_MEDIA_OPS.get_profile_list(ctx.media_ctx, plist, &pcount);
            const uint32_t idx = (uint32_t)std::atoi(profile_arg.c_str());
            if (rc == HAL_OK && idx < pcount && plist[idx])
                (void)HAL_MEDIA_OPS.switch_profile(ctx.media_ctx, plist[idx], false);
        }
        else
            (void)HAL_MEDIA_OPS.switch_profile(ctx.media_ctx, profile_arg.c_str(), false);
    }

    (void)HAL_MEDIA_OPS.set_encoder_auto_feed(ctx.media_ctx, false);

    void *codec_list_raw = nullptr;
    uint32_t codec_count = 0;
    rc = HAL_MEDIA_OPS.get_codec_list(ctx.media_ctx, &codec_list_raw, &codec_count);
    auto **codec_list = reinterpret_cast<void **>(codec_list_raw);
    if (rc != HAL_OK || !codec_list || codec_count == 0)
    {
        std::fprintf(stderr, "get_codec_list failed rc=%d\n", rc);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }
    HalCodecContext *selected_codec = nullptr;
    for (uint32_t i = 0; i < codec_count; i++)
    {
        auto *cc = static_cast<HalCodecContext *>(codec_list[i]);
        if (!cc)
            continue;
        if (cc->config.packet_type == HAL_PACKET_TYPE_H264 || cc->config.packet_type == HAL_PACKET_TYPE_H265)
        {
            selected_codec = cc;
            break;
        }
    }
    if (!selected_codec)
        selected_codec = static_cast<HalCodecContext *>(codec_list[0]);
    ctx.codec_ctx = selected_codec;

    HalCodecConfig ccfg{};
    (void)HAL_CODEC_OPS.get_current_config(ctx.codec_ctx, &ccfg);
    HalUdpStreamConfig ucfg{};
    ucfg.host = host.c_str();
    ucfg.port = port;
    ucfg.mode = (ccfg.packet_type == HAL_PACKET_TYPE_H265) ? HalUdpStreamMode::RtpH265AnnexB : HalUdpStreamMode::RtpH264AnnexB;
    HalUdpStream udp(ucfg);
    ctx.udp = &udp;

    HalDspConfig dcfg{};
    dcfg.device_priority = 0;
    rc = HAL_DSP_OPS.init(&dcfg, &ctx.dsp_ctx);
    if (rc != HAL_OK || !ctx.dsp_ctx)
    {
        std::fprintf(stderr, "HAL_DSP_OPS.init failed rc=%d\n", rc);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }

    HalInferenceConfig dicfg{};
    std::snprintf(dicfg.model_path, sizeof(dicfg.model_path), "%s", det_hef.c_str());
    dicfg.batch_size = 1;
    dicfg.timeout_ms = 1000;
    dicfg.use_dma = true;
    dicfg.platform_config = nullptr;
    dicfg.platform_data = nullptr;
    dicfg.preprocess = {};
    ctx.det_infer = HAL_INFERENCE_OPS.create(&dicfg);
    if (!ctx.det_infer)
    {
        std::fprintf(stderr, "det HAL_INFERENCE_OPS.create failed\n");
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }
    log_model_info(ctx.det_infer, "det");
    // Configure NV12 -> RGB/BGR preprocessing when the model expects 3-channel packed input.
    {
        const HalPreprocessColor det_color = pick_preprocess_color_for_mode(ctx.det_infer, false, ctx.det_packed_mode);
        if (det_color != HAL_PREPROCESS_COLOR_NONE)
        {
            HAL_INFERENCE_OPS.destroy(ctx.det_infer);
            dicfg.preprocess = {};
            dicfg.preprocess.color = det_color;
            ctx.det_infer = HAL_INFERENCE_OPS.create(&dicfg);
            if (!ctx.det_infer)
            {
                std::fprintf(stderr, "det HAL_INFERENCE_OPS.create failed (preprocess=%d)\n", (int)det_color);
                HAL_DSP_OPS.deinit(ctx.dsp_ctx);
                HAL_MEDIA_OPS.deinit(ctx.media_ctx);
                return 1;
            }
            log_model_info(ctx.det_infer, "det(preprocess)");
        }
    }
    if (auto hw = model_input_hw(ctx.det_infer))
    {
        ctx.det_w = hw->first;
        ctx.det_h = hw->second;
    }

    HalPostprocessConfig dpc{};
    std::memset(&dpc, 0, sizeof(dpc));
    dpc.type = HAL_POST_TYPE_DETECTION;
    hal_detection_config_init(&dpc.config.detection);
    dpc.config.detection.confidence_threshold = min_det_conf;
    dpc.config.detection.config_file = det_post_file.empty() ? nullptr : det_post_file.c_str();
    dpc.config.detection.config_json = det_post_json.empty() ? nullptr : det_post_json.c_str();
    ctx.det_post = HAL_POSTPROCESS_OPS.create(&dpc);
    if (!ctx.det_post)
    {
        std::fprintf(stderr, "det HAL_POSTPROCESS_OPS.create failed\n");
        HAL_INFERENCE_OPS.destroy(ctx.det_infer);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }

    auto parse_packed = [](const std::string &s) -> LprAppCtx::PackedFormatMode {
        if (s == "bgr") return LprAppCtx::PACKED_BGR;
        if (s == "auto") return LprAppCtx::PACKED_AUTO;
        return LprAppCtx::PACKED_RGB;
    };
    ctx.det_packed_mode = parse_packed(det_packed_format);
    ctx.rec_packed_mode = parse_packed(rec_packed_format);

    if (!ctx.det_only)
    {
        HalInferenceConfig ricfg{};
        std::snprintf(ricfg.model_path, sizeof(ricfg.model_path), "%s", rec_hef.c_str());
        ricfg.batch_size = 1;
        ricfg.timeout_ms = 1000;
        ricfg.use_dma = true;
        ricfg.platform_config = nullptr;
        ricfg.platform_data = nullptr;
        ricfg.preprocess = {};
        ctx.rec_infer = HAL_INFERENCE_OPS.create(&ricfg);
        if (!ctx.rec_infer)
        {
            std::fprintf(stderr, "rec HAL_INFERENCE_OPS.create failed\n");
            HAL_POSTPROCESS_OPS.destroy(ctx.det_post);
            HAL_INFERENCE_OPS.destroy(ctx.det_infer);
            HAL_DSP_OPS.deinit(ctx.dsp_ctx);
            HAL_MEDIA_OPS.deinit(ctx.media_ctx);
            return 1;
        }
        log_model_info(ctx.rec_infer, "rec");
        // If HEF filename includes "nv12", force treating input as NV12 (some HEFs report NV12 as H/2 with C=3).
        ctx.rec_force_nv12 = ascii_contains_ci(rec_hef, "nv12");
        // Configure NV12 -> RGB/BGR preprocessing when the model expects 3-channel packed input.
        {
            const HalPreprocessColor rec_color =
                pick_preprocess_color_for_mode(ctx.rec_infer, ctx.rec_force_nv12, ctx.rec_packed_mode);
            if (rec_color != HAL_PREPROCESS_COLOR_NONE)
            {
                HAL_INFERENCE_OPS.destroy(ctx.rec_infer);
                ricfg.preprocess = {};
                ricfg.preprocess.color = rec_color;
                ctx.rec_infer = HAL_INFERENCE_OPS.create(&ricfg);
                if (!ctx.rec_infer)
                {
                    std::fprintf(stderr, "rec HAL_INFERENCE_OPS.create failed (preprocess=%d)\n", (int)rec_color);
                    HAL_POSTPROCESS_OPS.destroy(ctx.det_post);
                    HAL_INFERENCE_OPS.destroy(ctx.det_infer);
                    HAL_DSP_OPS.deinit(ctx.dsp_ctx);
                    HAL_MEDIA_OPS.deinit(ctx.media_ctx);
                    return 1;
                }
                log_model_info(ctx.rec_infer, "rec(preprocess)");
            }
        }
        if (auto hw = model_input_hw(ctx.rec_infer))
        {
            ctx.rec_w = hw->first;
            ctx.rec_h = hw->second;
        }
        // Fixup: for NV12 recognition HEFs like NV12(24x320x3), effective NV12 height is derived from byte_size.
        if (ctx.rec_force_nv12)
        {
            HalModelInfo mi{};
            if (HAL_INFERENCE_OPS.get_model_info(ctx.rec_infer, &mi) == HAL_OK && mi.num_inputs > 0)
            {
                const auto &in0 = mi.inputs[0];
                if (in0.ndim >= 4 && in0.shape[1] > 0 && in0.shape[2] > 0 && in0.byte_size > 0)
                {
                    const uint32_t w = (uint32_t)in0.shape[2];
                    const uint64_t bs = (uint64_t)in0.byte_size;
                    if (w > 0 && (bs * 2ULL) % 3ULL == 0ULL)
                    {
                        const uint64_t area = (bs * 2ULL) / 3ULL;
                        if (area % (uint64_t)w == 0ULL)
                        {
                            const uint32_t h_eff = (uint32_t)(area / (uint64_t)w);
                            if (h_eff > 0)
                            {
                                ctx.rec_w = w;
                                ctx.rec_h = h_eff;
                            }
                        }
                    }
                }
            }
        }

        // Select recognition backend.
        if (rec_backend == "ppocrv5")
        {
            ctx.rec_backend = LprAppCtx::REC_BACKEND_PPOCRV5;
            ctx.rec_local_decode = false; // use vendor OCR postprocess
            if (rec_charset.empty())
                rec_charset = "ppocrv5_dict.txt"; // common on-device path; override via --rec-charset
        }
        else
        {
            ctx.rec_backend = LprAppCtx::REC_BACKEND_LPRNET;
            ctx.rec_local_decode = true;
        }

        // Local decode defaults for LPRNet.
        ctx.rec_blank_index = rec_blank;
        ctx.rec_charset_index_offset = rec_dict_offset;
        ctx.rec_time_major = rec_time_major;
        ctx.rec_min_score_threshold = rec_min_score;
        ctx.rec_min_chars = rec_min_chars;
        ctx.rec_debug = rec_debug;

        if (rec_scaling == "stretch")
            ctx.rec_scaling_mode = HAL_DSP_SCALING_STRETCH;
        else if (rec_scaling == "letterbox_up_left")
            ctx.rec_scaling_mode = HAL_DSP_SCALING_LETTERBOX_UP_LEFT;
        else if (rec_scaling == "scale_and_crop")
            ctx.rec_scaling_mode = HAL_DSP_SCALING_SCALE_AND_CROP;
        else
            ctx.rec_scaling_mode = HAL_DSP_SCALING_LETTERBOX_MIDDLE;

        // Optional: vendor OCR postprocess. For ppocrv5 backend this is required.
        ctx.rec_post = nullptr;
        if (ctx.rec_backend == LprAppCtx::REC_BACKEND_PPOCRV5 || !rec_post_file.empty() || !rec_post_json.empty())
        {
            HalPostprocessConfig rpc{};
            std::memset(&rpc, 0, sizeof(rpc));
            rpc.type = HAL_POST_TYPE_OCR_RECOGNITION;
            hal_ocr_recognition_post_config_init(&rpc.config.ocr_recognition);
            if (!rec_charset.empty())
                std::snprintf(rpc.config.ocr_recognition.charset_path, sizeof(rpc.config.ocr_recognition.charset_path), "%s",
                              rec_charset.c_str());
            rpc.config.ocr_recognition.charset_index_offset = rec_dict_offset;
            rpc.config.ocr_recognition.blank_index = rec_blank;
            rpc.config.ocr_recognition.time_major = rec_time_major;
            rpc.config.ocr_recognition.logits_are_softmax = true;
            rpc.config.ocr_recognition.config_file = rec_post_file.empty() ? nullptr : rec_post_file.c_str();
            rpc.config.ocr_recognition.config_json = rec_post_json.empty() ? nullptr : rec_post_json.c_str();
            ctx.rec_post = HAL_POSTPROCESS_OPS.create(&rpc);
            if (ctx.rec_post)
            {
                ctx.rec_local_decode = false; // vendor is requested and successfully created
            }
            else if (ctx.rec_backend == LprAppCtx::REC_BACKEND_PPOCRV5)
            {
                std::fprintf(stderr, "ppocrv5 backend requires --lp-rec-post-file/--lp-rec-post-json (e.g. ppocrv5_recognition.json)\n");
                HAL_INFERENCE_OPS.destroy(ctx.rec_infer);
                HAL_POSTPROCESS_OPS.destroy(ctx.det_post);
                HAL_INFERENCE_OPS.destroy(ctx.det_infer);
                HAL_DSP_OPS.deinit(ctx.dsp_ctx);
                HAL_MEDIA_OPS.deinit(ctx.media_ctx);
                return 1;
            }
        }
    }

    hal_draw_config_init_default(&ctx.draw_cfg);
    ctx.draw_cfg.draw_detections = true;
    ctx.draw_cfg.draw_ocr = !ctx.det_only;

    void *video_list_raw = nullptr;
    uint32_t video_count = 0;
    rc = HAL_MEDIA_OPS.get_video_list(ctx.media_ctx, &video_list_raw, &video_count);
    auto **video_list = reinterpret_cast<void **>(video_list_raw);
    if (rc != HAL_OK || !video_list || video_count == 0)
    {
        std::fprintf(stderr, "get_video_list failed rc=%d\n", rc);
        HAL_POSTPROCESS_OPS.destroy(ctx.rec_post);
        HAL_INFERENCE_OPS.destroy(ctx.rec_infer);
        HAL_POSTPROCESS_OPS.destroy(ctx.det_post);
        HAL_INFERENCE_OPS.destroy(ctx.det_infer);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }

    uint32_t want_w = ccfg.width;
    uint32_t want_h = ccfg.height;
    if (want_w == 0 || want_h == 0)
    {
        want_w = ctx.det_w;
        want_h = ctx.det_h;
    }
    const uint32_t vid_idx = pick_video_index_exact_or_closest(video_list, video_count, want_w, want_h);
    ctx.video_ctx = video_list[vid_idx];
    auto *vctx = static_cast<HalVideoContext *>(ctx.video_ctx);
    ctx.stream_key = (vctx && vctx->video_name[0]) ? std::string(vctx->video_name) : std::string();
    HAL_LOG_INFO("lpr_example_v2: preview stream \"%s\" %ux%u encoder=%ux%u det_in=%ux%u rec_in=%ux%u",
                 ctx.stream_key.c_str(), vctx ? vctx->config.width : 0U, vctx ? vctx->config.height : 0U,
                 ccfg.width, ccfg.height, ctx.det_w, ctx.det_h, ctx.det_only ? 0U : ctx.rec_w, ctx.det_only ? 0U : ctx.rec_h);

    if (vctx && vctx->config.width && vctx->config.height)
    {
        HalCodecContext *best = nullptr;
        for (uint32_t i = 0; i < codec_count; i++)
        {
            auto *c = static_cast<HalCodecContext *>(codec_list[i]);
            if (!c)
                continue;
            if (c->config.packet_type != ccfg.packet_type)
                continue;
            if (c->config.width == vctx->config.width && c->config.height == vctx->config.height)
            {
                if (!best || (ctx.stream_key == std::string(c->codec_name)))
                    best = c;
            }
        }
        if (best && best != ctx.codec_ctx)
        {
            ctx.codec_ctx = best;
            (void)HAL_CODEC_OPS.get_current_config(ctx.codec_ctx, &ccfg);
        }
    }

    (void)HAL_CODEC_OPS.subscribe(ctx.codec_ctx, codec_cb, &ctx);

    const uint32_t ai_idx =
        pick_video_index_exact_or_closest_prefer_not(video_list, video_count, ctx.det_w, ctx.det_h, vid_idx);
    void *ai_video_ctx = video_list[ai_idx];
    auto *aiv = static_cast<HalVideoContext *>(ai_video_ctx);
    const std::string ai_key = (aiv && aiv->video_name[0]) ? std::string(aiv->video_name) : std::string();

    const bool same_stream = (ai_video_ctx == ctx.video_ctx);
    if (same_stream)
    {
        rc = HAL_VIDEO_OPS.subscribe_stream(ctx.video_ctx, ctx.stream_key.c_str(), video_cb_preview_and_ai, &ctx);
    }
    else
    {
        rc = HAL_VIDEO_OPS.subscribe_stream(ctx.video_ctx, ctx.stream_key.c_str(), video_cb_preview_only, &ctx);
        if (rc == HAL_OK)
            rc = HAL_VIDEO_OPS.subscribe_stream(ai_video_ctx, ai_key.c_str(), video_cb_ai_only, &ctx);
    }
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "subscribe_stream failed rc=%d\n", rc);
        (void)HAL_CODEC_OPS.unsubscribe(ctx.codec_ctx, codec_cb);
        HAL_POSTPROCESS_OPS.destroy(ctx.rec_post);
        HAL_INFERENCE_OPS.destroy(ctx.rec_infer);
        HAL_POSTPROCESS_OPS.destroy(ctx.det_post);
        HAL_INFERENCE_OPS.destroy(ctx.det_infer);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }

    rc = HAL_MEDIA_OPS.start(ctx.media_ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "HAL_MEDIA_OPS.start failed rc=%d\n", rc);
        (void)HAL_VIDEO_OPS.unsubscribe_stream(ctx.video_ctx, ctx.stream_key.c_str());
        if (!same_stream)
            (void)HAL_VIDEO_OPS.unsubscribe_stream(ai_video_ctx, ai_key.c_str());
        (void)HAL_CODEC_OPS.unsubscribe(ctx.codec_ctx, codec_cb);
        HAL_POSTPROCESS_OPS.destroy(ctx.rec_post);
        HAL_INFERENCE_OPS.destroy(ctx.rec_infer);
        HAL_POSTPROCESS_OPS.destroy(ctx.det_post);
        HAL_INFERENCE_OPS.destroy(ctx.det_infer);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }

    ctx.ai_worker = std::thread(lpr_ai_worker_loop, &ctx);
    ctx.preview_worker = std::thread(lpr_preview_worker_loop, &ctx);

    std::printf("lpr_example_v2 running. Ctrl+C to stop.\n");
    while (!g_stop.load(std::memory_order_acquire))
    {
        if (g_sig)
        {
            g_stop.store(true, std::memory_order_release);
            ctx.q_cv.notify_all();
            ctx.preview_cv.notify_all();
            ctx.result.cv.notify_all();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    (void)HAL_MEDIA_OPS.stop(ctx.media_ctx);
    (void)HAL_VIDEO_OPS.unsubscribe_stream(ctx.video_ctx, ctx.stream_key.c_str());
    if (!same_stream)
        (void)HAL_VIDEO_OPS.unsubscribe_stream(ai_video_ctx, ai_key.c_str());
    (void)HAL_CODEC_OPS.unsubscribe(ctx.codec_ctx, codec_cb);
    ctx.q_cv.notify_all();
    ctx.preview_cv.notify_all();
    ctx.result.cv.notify_all();
    if (ctx.ai_worker.joinable())
        ctx.ai_worker.join();
    if (ctx.preview_worker.joinable())
        ctx.preview_worker.join();

    HAL_POSTPROCESS_OPS.destroy(ctx.rec_post);
    HAL_INFERENCE_OPS.destroy(ctx.rec_infer);
    HAL_POSTPROCESS_OPS.destroy(ctx.det_post);
    HAL_INFERENCE_OPS.destroy(ctx.det_infer);
    (void)HAL_DSP_OPS.deinit(ctx.dsp_ctx);
    (void)HAL_MEDIA_OPS.deinit(ctx.media_ctx);
    return 0;
}
