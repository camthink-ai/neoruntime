/**
 * @file parallel_infer_example_v2.cpp
 * @brief N-model parallel inference demo (shared VDevice + Model Scheduler).
 *
 * Subscribes to a medialib video frontend, resizes/crops each frame per-model via
 * HailoDSP resize/convert (after CPU camera import), then pipelines async inference: frames are submitted as they arrive
 * (no wait for all models on the previous frame). Each model keeps a buffer pool
 * so in-flight jobs do not share tensors.
 *
 * Usage:
 *   hal-parallel-infer-example-v2 <medialib_json> <hef1> [hef2 ...] [options]
 *
 * Options:
 *   --frames N                 Stop after N frames (0 = run until Ctrl+C, default 0)
 *   --video-index I            Frontend index (default: largest / first)
 *   --letterbox                Letterbox resize (default: stretch to input size)
 *   --pipeline-depth N         In-flight buffers per model (default 8)
 *   --scheduler-threshold N    Per-model scheduler threshold (default 4)
 *   --scheduler-timeout-ms N   Per-model scheduler timeout (default 100)
 *   --priority <idx:prio>      Per-model priority, e.g. --priority 0:24 (repeatable)
 *   --stats-interval-ms N      Stats print interval (default 1000)
 */

#include "common/hal_log.h"
#include "dsp/hal_dsp.h"
#include "media/hal_media.h"
#include "media/hal_video.h"
#include "media/hal_video_internal.h"
#include "model/hal_inference.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <map>
#include <sys/ioctl.h>
#include <unistd.h>

#if defined(__linux__)
#include <linux/dma-buf.h>
#endif
#include <memory>
#include <mutex>
#include <algorithm>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr size_t kMaxModels = 8;
constexpr size_t kDefaultPipelineDepth = 4;
constexpr const char *kParallelInferBuildTag = "parallel_infer_v2 dsp-preprocess early-release 2026-06-09";

std::atomic<bool> g_stop{false};
static uint64_t g_stats_last_tick_ms = 0;
static uint64_t g_stats_last_frames_done = 0;
std::atomic<uint64_t> g_frames_in{0};
std::atomic<uint64_t> g_frames_done{0};
std::atomic<uint64_t> g_frames_dropped{0};

static uint64_t steady_ms()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

enum class InputKind
{
    Nv12Blob,
    Nv12Planes,
    RgbPacked,
};

/** Per in-flight inference job — not shared across concurrent frames. */
struct ModelPipelineBuf
{
    HalFrameBuffer *resize_nv12 = nullptr;
    HalFrameBuffer *rgb_fb = nullptr;
    std::vector<uint8_t> nv12_blob;
    std::vector<HalTensor> outputs;
};

struct ModelSlot
{
    ModelSlot() = default;
    ModelSlot(const ModelSlot &) = delete;
    ModelSlot &operator=(const ModelSlot &) = delete;
    ModelSlot(ModelSlot &&) = delete;
    ModelSlot &operator=(ModelSlot &&) = delete;

    std::string label;
    std::string hef_path;
    HalInferenceSession *session = nullptr;
    HalModelInfo info{};
    InputKind input_kind = InputKind::Nv12Blob;
    uint32_t in_w = 0;
    uint32_t in_h = 0;
    uint32_t num_inputs = 0;
    uint32_t num_outputs = 0;
    HalPixelFormat rgb_fmt = HAL_PIX_FMT_RGB24;

    std::vector<ModelPipelineBuf> pipeline_bufs;
    std::deque<ModelPipelineBuf *> free_bufs;
    std::mutex buf_mu;
    std::condition_variable buf_cv;

    uint64_t infer_ok = 0;
    uint64_t infer_err = 0;
    uint64_t preprocess_err = 0;
    double last_latency_ms = 0.0;
    double last_preprocess_ms = 0.0;
    double stats_pre_ms_acc = 0.0;
    double stats_infer_ms_acc = 0.0;
    uint64_t stats_samples = 0;
};

struct FrameBatch
{
    std::atomic<int> pending{0};
    void *video_ctx = nullptr;
    HalFrameBuffer *frame = nullptr;
};

struct AsyncJobCtx
{
    ModelSlot *slot = nullptr;
    ModelPipelineBuf *buf = nullptr;
    std::shared_ptr<FrameBatch> batch;
    uint64_t infer_t0 = 0;
    double preprocess_ms = 0.0;
};

struct PoolGeomKey
{
    uint32_t width = 0;
    uint32_t height = 0;
    HalPixelFormat fmt = HAL_PIX_FMT_NV12;

    bool operator<(const PoolGeomKey &o) const noexcept
    {
        if (width != o.width)
            return width < o.width;
        if (height != o.height)
            return height < o.height;
        return fmt < o.fmt;
    }
};

struct AppCtx
{
    void *media_ctx = nullptr;
    void *video_ctx = nullptr;
    void *dsp_ctx = nullptr;
    HalInferenceRuntime *runtime = nullptr;

    std::vector<std::unique_ptr<ModelSlot>> models;
    uint32_t video_index = 0;
    bool use_letterbox = false;
    uint32_t max_frames = 0;
    uint32_t stats_interval_ms = 1000;
    uint32_t pipeline_depth = kDefaultPipelineDepth;
    uint32_t scheduler_threshold = 4;
    uint32_t scheduler_timeout_ms = 100;
    uint32_t shared_preprocess_w = 0;
    uint32_t shared_preprocess_h = 0;
    uint32_t video_frame_w = 0;
    uint32_t video_frame_h = 0;
    HalFrameBuffer *camera_import_nv12 = nullptr;
    HalFrameBuffer *shared_preprocess_nv12 = nullptr;
    HalFrameBuffer *resize_chain_mid = nullptr;
    uint32_t resize_chain_mid_w = 0;
    uint32_t resize_chain_mid_h = 0;
    std::map<PoolGeomKey, uint32_t> pool_max_by_geom;

    std::mutex dsp_mu;
    std::mutex frame_q_mu;
    std::condition_variable frame_q_cv;
    std::deque<HalFrameBuffer *> frame_q;

    std::string stream_key;

    std::thread worker;
    std::thread stats_thread;
};

static std::optional<std::pair<uint32_t, uint32_t>> model_input_hw(const HalModelInfo &mi)
{
    for (uint32_t i = 0; i < mi.num_inputs; i++)
    {
        const auto &in = mi.inputs[i];
        if (in.ndim >= 4)
        {
            const int32_t h = in.shape[1];
            const int32_t w = in.shape[2];
            if (w > 0 && h > 0)
                return std::make_pair(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
        }
    }
    return std::nullopt;
}

static InputKind detect_input_kind(const HalModelInfo &mi)
{
    if (mi.num_inputs >= 2)
        return InputKind::Nv12Planes;
    if (mi.num_inputs == 1)
    {
        const auto &in = mi.inputs[0];
        if (in.is_nv12)
            return InputKind::Nv12Blob;
        if (in.ndim >= 4 && in.shape[3] == 3)
            return InputKind::RgbPacked;
        if (in.byte_size > 0 && in.ndim >= 4)
        {
            const int32_t h = in.shape[1];
            const int32_t w = in.shape[2];
            if (h > 0 && w > 0)
            {
                const uint64_t nv12_bytes = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 3ULL / 2ULL;
                if (static_cast<uint64_t>(in.byte_size) == nv12_bytes)
                    return InputKind::Nv12Blob;
                const uint64_t rgb_bytes = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 3ULL;
                if (static_cast<uint64_t>(in.byte_size) == rgb_bytes)
                    return InputKind::RgbPacked;
            }
        }
    }
    return InputKind::Nv12Blob;
}

static const char *input_kind_str(InputKind k)
{
    switch (k)
    {
        case InputKind::Nv12Planes:
            return "NV12_2plane";
        case InputKind::RgbPacked:
            return "RGB_packed";
        case InputKind::Nv12Blob:
        default:
            return "NV12_blob";
    }
}

constexpr size_t kStatsModelColWidth = 32;
constexpr size_t kStatsInputColWidth = 20;

static void truncate_for_column(char *out, size_t out_size, const char *text, size_t col_width)
{
    if (!out || out_size == 0)
        return;
    if (!text || text[0] == '\0')
    {
        out[0] = '\0';
        return;
    }
    const size_t len = std::strlen(text);
    if (col_width <= 3 || len <= col_width)
    {
        std::snprintf(out, out_size, "%s", text);
        return;
    }
    std::snprintf(out, out_size, "%.*s...", static_cast<int>(col_width - 3), text);
}

/** Fixed-width stats table. Long model / input labels are truncated with "...". */
static void print_stats_table_header()
{
    std::printf("%-*s %-*s %8s %8s %8s %8s %6s %6s\n", static_cast<int>(kStatsModelColWidth), "model",
                static_cast<int>(kStatsInputColWidth), "input", "model_fps", "pre_ms", "e2e_ms", "ok", "err", "pend");
    char model_rule[kStatsModelColWidth + 1];
    char input_rule[kStatsInputColWidth + 1];
    std::memset(model_rule, '-', kStatsModelColWidth);
    model_rule[kStatsModelColWidth] = '\0';
    std::memset(input_rule, '-', kStatsInputColWidth);
    input_rule[kStatsInputColWidth] = '\0';
    std::printf("%-*s %-*s %8s %8s %8s %8s %6s %6s\n", static_cast<int>(kStatsModelColWidth), model_rule,
                static_cast<int>(kStatsInputColWidth), input_rule, "--------", "--------", "--------", "--------",
                "------", "------");
}

static void print_stats_table_row(const ModelSlot &slot, double model_fps, uint32_t pending, double avg_pre_ms,
                                  double avg_e2e_ms)
{
    char model_desc[kStatsModelColWidth + 4];
    char input_raw[48];
    char input_desc[kStatsInputColWidth + 4];
    std::snprintf(input_raw, sizeof(input_raw), "%ux%u %s", slot.in_w, slot.in_h,
                  input_kind_str(slot.input_kind));
    truncate_for_column(model_desc, sizeof(model_desc), slot.label.c_str(), kStatsModelColWidth);
    truncate_for_column(input_desc, sizeof(input_desc), input_raw, kStatsInputColWidth);
    std::printf("%-*s %-*s %8.2f %8.2f %8.2f %8llu %6llu %6u\n", static_cast<int>(kStatsModelColWidth), model_desc,
                static_cast<int>(kStatsInputColWidth), input_desc, model_fps, avg_pre_ms, avg_e2e_ms,
                static_cast<unsigned long long>(slot.infer_ok),
                static_cast<unsigned long long>(slot.infer_err), pending);
}

static void copy_frame_metadata_best_effort(const HalFrameBuffer *src, HalFrameBuffer *dst)
{
    if (!src || !dst)
        return;
    if (src->priv && dst->priv)
        (void)HAL_FRAME_BUFFER_OPS.copy_metadata_from_frame_buffer(src, dst);
    else
    {
        dst->sequence = src->sequence;
        dst->timestamp_ns = src->timestamp_ns;
    }
}

static void sync_dmabuf_plane(int fd, bool start, bool write)
{
#if defined(__linux__)
    if (fd < 0)
        return;
    struct dma_buf_sync sync{};
    sync.flags = (start ? DMA_BUF_SYNC_START : DMA_BUF_SYNC_END) |
                 (write ? DMA_BUF_SYNC_WRITE : DMA_BUF_SYNC_READ);
    (void)ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
#else
    (void)fd;
    (void)start;
    (void)write;
#endif
}

static void sync_frame_planes(const HalFrameBuffer *fb, bool start, bool write)
{
    if (!fb)
        return;
    for (uint32_t i = 0; i < fb->num_planes; i++)
        sync_dmabuf_plane(fb->dma_fds[i], start, write);
}

static void copy_nv12_planes(const HalFrameBuffer *src, HalFrameBuffer *dst)
{
    if (!src || !dst || src->num_planes < 2 || dst->num_planes < 2)
        return;
    sync_frame_planes(src, true, false);
    sync_frame_planes(dst, true, true);
    const uint32_t copy_w = std::min(src->width, dst->width);
    const uint32_t copy_h = std::min(src->height, dst->height);
    for (uint32_t row = 0; row < copy_h; row++)
    {
        std::memcpy(static_cast<uint8_t *>(dst->planes[0]) + static_cast<size_t>(row) * dst->strides[0],
                    static_cast<const uint8_t *>(src->planes[0]) + static_cast<size_t>(row) * src->strides[0],
                    copy_w);
    }
    for (uint32_t row = 0; row < copy_h / 2U; row++)
    {
        std::memcpy(static_cast<uint8_t *>(dst->planes[1]) + static_cast<size_t>(row) * dst->strides[1],
                    static_cast<const uint8_t *>(src->planes[1]) + static_cast<size_t>(row) * src->strides[1],
                    copy_w);
    }
    sync_frame_planes(dst, false, true);
    sync_frame_planes(src, false, false);
}

/**
 * HailoDSP idma cannot mix frontend camera DMA (base N) with hal_v2 request-pool DMA (base M).
 * CPU-copy the camera frame into an app-pool NV12 buffer before any DSP resize/convert.
 */
static int import_camera_to_app_pool(AppCtx *ctx, const HalFrameBuffer *camera, const HalFrameBuffer **out)
{
    if (!ctx || !camera || !out)
        return HAL_ERR_INVALID_ARG;
    if (!ctx->camera_import_nv12 || camera == ctx->camera_import_nv12)
    {
        *out = camera;
        return HAL_OK;
    }
    if (camera->format != HAL_PIX_FMT_NV12 || camera->width != ctx->video_frame_w ||
        camera->height != ctx->video_frame_h)
    {
        *out = camera;
        return HAL_OK;
    }
    copy_nv12_planes(camera, ctx->camera_import_nv12);
    copy_frame_metadata_best_effort(camera, ctx->camera_import_nv12);
    *out = ctx->camera_import_nv12;
    return HAL_OK;
}

static void compute_resize_chain_mid_size(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                                          uint32_t *out_w, uint32_t *out_h);

static void update_pool_budgets(AppCtx &ctx)
{
    ctx.pool_max_by_geom.clear();
    auto add = [&](uint32_t w, uint32_t h, HalPixelFormat fmt, uint32_t count) {
        if (w == 0U || h == 0U || count == 0U)
            return;
        PoolGeomKey key{w, h, fmt};
        ctx.pool_max_by_geom[key] += count;
    };

    add(ctx.video_frame_w, ctx.video_frame_h, HAL_PIX_FMT_NV12, 1U);
    add(ctx.shared_preprocess_w, ctx.shared_preprocess_h, HAL_PIX_FMT_NV12, 1U);

    uint32_t mid_w = 0;
    uint32_t mid_h = 0;
    compute_resize_chain_mid_size(ctx.video_frame_w, ctx.video_frame_h, ctx.shared_preprocess_w,
                                  ctx.shared_preprocess_h, &mid_w, &mid_h);
    add(mid_w, mid_h, HAL_PIX_FMT_NV12, 1U);

    for (const auto &slot_ptr : ctx.models)
    {
        const ModelSlot &slot = *slot_ptr;
        add(slot.in_w, slot.in_h, HAL_PIX_FMT_NV12, ctx.pipeline_depth);
        if (slot.input_kind == InputKind::RgbPacked)
            add(slot.in_w, slot.in_h, slot.rgb_fmt, ctx.pipeline_depth);
    }

    for (auto &kv : ctx.pool_max_by_geom)
        kv.second += 1U;
}

static uint32_t pool_max_for(const AppCtx &ctx, uint32_t width, uint32_t height, HalPixelFormat fmt)
{
    const PoolGeomKey key{width, height, fmt};
    const auto it = ctx.pool_max_by_geom.find(key);
    if (it == ctx.pool_max_by_geom.end())
        return 1U;
    return std::max(1U, it->second);
}

/** HailoDSP requires DMA-BUF on all planes; USERPTR cannot mix with camera DMA (see dsp_log0 idma_lookup). */
static HalFrameBuffer *request_pool_frame(const AppCtx *ctx, uint32_t width, uint32_t height, HalPixelFormat fmt)
{
    if (width == 0 || height == 0)
        return nullptr;
    HalFrameBufferRequest req{};
    req.width = width;
    req.height = height;
    req.format = fmt;
    req.mem_type = HAL_MEM_DMABUF;
    req.pool_max_buffers = ctx ? pool_max_for(*ctx, width, height, fmt) : 1U;
    req.zero_initialize = false;
    HalFrameBuffer *fb = nullptr;
    if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&req, &fb) != HAL_OK || !fb)
        return nullptr;
    return fb;
}

static void release_pool_frame(HalFrameBuffer *fb)
{
    if (fb)
        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(fb);
}

static inline uint32_t ceil_div_u32(uint32_t a, uint32_t b)
{
    return (a + b - 1U) / b;
}

static void compute_resize_chain_mid_size(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h,
                                          uint32_t *out_w, uint32_t *out_h)
{
    constexpr uint32_t kMaxDownscale = 16U;
    uint32_t cur_w = src_w;
    uint32_t cur_h = src_h;
    uint32_t max_mid_w = 0;
    uint32_t max_mid_h = 0;
    while (cur_w > dst_w * kMaxDownscale || cur_h > dst_h * kMaxDownscale)
    {
        const uint32_t next_w = std::max(dst_w, ceil_div_u32(cur_w, kMaxDownscale));
        const uint32_t next_h = std::max(dst_h, ceil_div_u32(cur_h, kMaxDownscale));
        max_mid_w = std::max(max_mid_w, next_w);
        max_mid_h = std::max(max_mid_h, next_h);
        cur_w = next_w;
        cur_h = next_h;
    }
    if (out_w)
        *out_w = max_mid_w;
    if (out_h)
        *out_h = max_mid_h;
}

/** Multi-step DSP resize within app DMA pools (caller holds dsp_mu). */
static int dsp_resize_chain_locked(AppCtx *ctx, const HalFrameBuffer *src, HalFrameBuffer *dst,
                                   HalDspInterpolation interpolation, std::vector<HalFrameBuffer *> &temp_out)
{
    temp_out.clear();
    if (!ctx || !ctx->dsp_ctx || !src || !dst)
        return HAL_ERR_INVALID_ARG;

    constexpr uint32_t kMaxDownscale = 16U;
    const HalFrameBuffer *cur = src;
    while (cur->width > dst->width * kMaxDownscale || cur->height > dst->height * kMaxDownscale)
    {
        const uint32_t next_w = std::max(dst->width, ceil_div_u32(cur->width, kMaxDownscale));
        const uint32_t next_h = std::max(dst->height, ceil_div_u32(cur->height, kMaxDownscale));
        HalFrameBuffer *mid = nullptr;
        bool owned_mid = false;
        if (ctx->resize_chain_mid && next_w <= ctx->resize_chain_mid_w && next_h <= ctx->resize_chain_mid_h)
        {
            mid = ctx->resize_chain_mid;
        }
        else
        {
            mid = request_pool_frame(ctx, next_w, next_h, HAL_PIX_FMT_NV12);
            owned_mid = true;
        }
        if (!mid)
            goto fail;

        HalDspResizeParams rp{};
        rp.src = cur;
        rp.dst = mid;
        rp.interpolation = interpolation;
        if (HAL_DSP_OPS.resize(ctx->dsp_ctx, &rp) != HAL_OK)
        {
            if (owned_mid)
                release_pool_frame(mid);
            goto fail;
        }
        if (owned_mid)
            temp_out.push_back(mid);
        cur = mid;
    }

    {
        HalDspResizeParams rp{};
        rp.src = cur;
        rp.dst = dst;
        rp.interpolation = interpolation;
        if (HAL_DSP_OPS.resize(ctx->dsp_ctx, &rp) != HAL_OK)
            goto fail;
    }
    return HAL_OK;
fail:
    for (auto *b : temp_out)
        release_pool_frame(b);
    temp_out.clear();
    return HAL_ERR_CHECK;
}

static int dsp_resize_nv12(AppCtx *ctx, const HalFrameBuffer *src, HalFrameBuffer *dst,
                           std::vector<HalFrameBuffer *> &temp_out)
{
    temp_out.clear();
    if (!ctx || !ctx->dsp_ctx || !src || !dst)
        return HAL_ERR_INVALID_ARG;

    std::lock_guard<std::mutex> lk(ctx->dsp_mu);
    if (ctx->use_letterbox)
    {
        HalDspRoi crop{};
        crop.start_x = 0;
        crop.start_y = 0;
        crop.end_x = src->width;
        crop.end_y = src->height;
        HalDspCropResizeParams cp{};
        cp.src = src;
        cp.dst = dst;
        cp.crop = crop;
        cp.interpolation = HAL_DSP_INTERPOLATION_BILINEAR;
        cp.scaling_mode = HAL_DSP_SCALING_LETTERBOX_MIDDLE;
        cp.letterbox_alignment = HAL_DSP_LETTERBOX_MIDDLE;
        cp.letterbox_color.y = 16;
        cp.letterbox_color.u = 128;
        cp.letterbox_color.v = 128;
        return HAL_DSP_OPS.crop_and_resize(ctx->dsp_ctx, &cp);
    }
    return dsp_resize_chain_locked(ctx, src, dst, HAL_DSP_INTERPOLATION_BILINEAR, temp_out);
}

static std::string basename_no_ext(const std::string &path)
{
    const auto slash = path.find_last_of('/');
    std::string base = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = base.rfind('.');
    if (dot != std::string::npos)
        base.resize(dot);
    return base;
}

static int preprocess_to_model_nv12(AppCtx *ctx, uint32_t in_w, uint32_t in_h, HalFrameBuffer *resize_dst,
                                    const HalFrameBuffer *src, const HalFrameBuffer **out_nv12, uint64_t *pre_ms_out)
{
    const uint64_t t0 = steady_ms();
    if (!ctx || !src || !resize_dst || !out_nv12)
        return HAL_ERR_INVALID_ARG;

    if (src->width == in_w && src->height == in_h && src->format == HAL_PIX_FMT_NV12)
    {
        /* Always copy into per-job resize_dst — shared_preprocess_nv12 is reused every frame. */
        if (src != resize_dst)
            copy_nv12_planes(src, resize_dst);
        copy_frame_metadata_best_effort(src, resize_dst);
        *out_nv12 = resize_dst;
        if (pre_ms_out)
            *pre_ms_out = steady_ms() - t0;
        return HAL_OK;
    }

    std::vector<HalFrameBuffer *> temp;
    const int rc = dsp_resize_nv12(ctx, src, resize_dst, temp);
    for (auto *b : temp)
        release_pool_frame(b);
    if (rc != HAL_OK)
        return rc;
    copy_frame_metadata_best_effort(src, resize_dst);
    *out_nv12 = resize_dst;
    if (pre_ms_out)
        *pre_ms_out = steady_ms() - t0;
    return HAL_OK;
}

static bool build_rgb_packed_input(const HalFrameBuffer *frame, HalTensor &out, std::vector<uint8_t> &storage)
{
    if (!frame || (frame->format != HAL_PIX_FMT_RGB24 && frame->format != HAL_PIX_FMT_BGR24))
        return false;
    const uint32_t row_bytes = frame->width * 3U;
    const uint32_t tight = row_bytes * frame->height;
    storage.resize(tight);
    sync_frame_planes(frame, true, false);
    uint8_t *dst = storage.data();
    for (uint32_t row = 0; row < frame->height; row++)
    {
        std::memcpy(dst + static_cast<size_t>(row) * row_bytes,
                    static_cast<const uint8_t *>(frame->planes[0]) + static_cast<size_t>(row) * frame->strides[0],
                    row_bytes);
    }
    sync_frame_planes(frame, false, false);
    std::memset(&out, 0, sizeof(out));
    out.data = storage.data();
    out.ndim = 3;
    out.shape[0] = static_cast<int32_t>(frame->height);
    out.shape[1] = static_cast<int32_t>(frame->width);
    out.shape[2] = 3;
    out.dtype = HAL_DTYPE_UINT8;
    out.byte_size = tight;
    out.dma_fd = -1;
    return true;
}

static bool build_nv12_planes_inputs(const HalFrameBuffer *frame, HalTensor *inputs, int &n)
{
    n = 0;
    if (!frame || frame->format != HAL_PIX_FMT_NV12 || frame->num_planes < 2)
        return false;
    HalTensor y{};
    y.data = frame->planes[0];
    y.ndim = 2;
    y.shape[0] = static_cast<int32_t>(frame->height);
    y.shape[1] = static_cast<int32_t>(frame->width);
    y.dtype = HAL_DTYPE_UINT8;
    y.byte_size = frame->sizes[0];
    y.dma_fd = frame->dma_fds[0];
    HalTensor uv = y;
    uv.data = frame->planes[1];
    uv.shape[0] = static_cast<int32_t>(frame->height / 2);
    uv.byte_size = frame->sizes[1];
    uv.dma_fd = frame->dma_fds[1];
    inputs[0] = y;
    inputs[1] = uv;
    n = 2;
    return true;
}

static bool build_nv12_blob_input(const HalFrameBuffer *frame, HalTensor &out, std::vector<uint8_t> &storage)
{
    if (!frame || frame->format != HAL_PIX_FMT_NV12 || frame->num_planes < 2)
        return false;
    const uint32_t tight = frame->width * frame->height * 3U / 2U;
    storage.resize(tight);
    uint8_t *dst = storage.data();
    size_t off = 0;
    for (uint32_t row = 0; row < frame->height; row++)
    {
        std::memcpy(dst + off, static_cast<const uint8_t *>(frame->planes[0]) + static_cast<size_t>(row) * frame->strides[0],
                    frame->width);
        off += frame->width;
    }
    for (uint32_t row = 0; row < frame->height / 2U; row++)
    {
        std::memcpy(dst + off,
                    static_cast<const uint8_t *>(frame->planes[1]) + static_cast<size_t>(row) * frame->strides[1],
                    frame->width);
        off += frame->width;
    }
    std::memset(&out, 0, sizeof(out));
    out.data = storage.data();
    out.ndim = 1;
    out.shape[0] = static_cast<int32_t>(tight);
    out.dtype = HAL_DTYPE_UINT8;
    out.byte_size = tight;
    out.dma_fd = -1;
    return true;
}

static int prepare_model_inputs(AppCtx *ctx, const ModelSlot &slot, ModelPipelineBuf &buf, const HalFrameBuffer *nv12,
                                HalTensor *inputs, int &num_inputs)
{
    num_inputs = 0;
    if (!ctx || !nv12)
        return HAL_ERR_INVALID_ARG;

    if (slot.input_kind == InputKind::RgbPacked)
    {
        if (!buf.rgb_fb)
            return HAL_ERR_NO_MEM;
        HalDspConvertFormatParams cf{};
        cf.src = nv12;
        cf.dst = buf.rgb_fb;
        {
            std::lock_guard<std::mutex> lk(ctx->dsp_mu);
            if (HAL_DSP_OPS.convert_format(ctx->dsp_ctx, &cf) != HAL_OK)
                return HAL_ERR_RESULT;
        }
        if (!build_rgb_packed_input(buf.rgb_fb, inputs[0], buf.nv12_blob))
            return HAL_ERR_RESULT;
        num_inputs = 1;
        return HAL_OK;
    }

    if (slot.input_kind == InputKind::Nv12Planes)
    {
        if (!build_nv12_planes_inputs(nv12, inputs, num_inputs))
            return HAL_ERR_RESULT;
        return HAL_OK;
    }

    if (!build_nv12_blob_input(nv12, inputs[0], buf.nv12_blob))
        return HAL_ERR_RESULT;
    num_inputs = 1;
    return HAL_OK;
}

static ModelPipelineBuf *acquire_pipeline_buf(ModelSlot &slot)
{
    std::unique_lock<std::mutex> lk(slot.buf_mu);
    slot.buf_cv.wait(lk, [&] { return g_stop.load(std::memory_order_acquire) || !slot.free_bufs.empty(); });
    if (slot.free_bufs.empty())
        return nullptr;
    ModelPipelineBuf *buf = slot.free_bufs.front();
    slot.free_bufs.pop_front();
    return buf;
}

static void release_pipeline_buf(ModelSlot &slot, ModelPipelineBuf *buf)
{
    if (!buf)
        return;
    std::lock_guard<std::mutex> lk(slot.buf_mu);
    slot.free_bufs.push_back(buf);
    slot.buf_cv.notify_one();
}

static void wait_model_pipeline_idle(ModelSlot &slot)
{
    std::unique_lock<std::mutex> lk(slot.buf_mu);
    slot.buf_cv.wait(lk, [&] { return slot.free_bufs.size() == slot.pipeline_bufs.size(); });
}

static void update_shared_preprocess_size(AppCtx &ctx)
{
    ctx.shared_preprocess_w = 0;
    ctx.shared_preprocess_h = 0;
    for (const auto &slot_ptr : ctx.models)
    {
        ctx.shared_preprocess_w = std::max(ctx.shared_preprocess_w, slot_ptr->in_w);
        ctx.shared_preprocess_h = std::max(ctx.shared_preprocess_h, slot_ptr->in_h);
    }
}

static bool init_camera_import_buffer(AppCtx &ctx)
{
    if (ctx.video_frame_w == 0 || ctx.video_frame_h == 0)
        return true;
    if (ctx.camera_import_nv12)
        return true;
    ctx.camera_import_nv12 =
        request_pool_frame(&ctx, ctx.video_frame_w, ctx.video_frame_h, HAL_PIX_FMT_NV12);
    return ctx.camera_import_nv12 != nullptr;
}

static void release_camera_import_buffer(AppCtx &ctx)
{
    release_pool_frame(ctx.camera_import_nv12);
    ctx.camera_import_nv12 = nullptr;
}

static bool init_shared_preprocess_buffer(AppCtx &ctx)
{
    if (ctx.shared_preprocess_w == 0 || ctx.shared_preprocess_h == 0)
        return true;
    if (ctx.shared_preprocess_nv12)
        return true;
    ctx.shared_preprocess_nv12 =
        request_pool_frame(&ctx, ctx.shared_preprocess_w, ctx.shared_preprocess_h, HAL_PIX_FMT_NV12);
    return ctx.shared_preprocess_nv12 != nullptr;
}

static void release_shared_preprocess_buffer(AppCtx &ctx)
{
    release_pool_frame(ctx.shared_preprocess_nv12);
    ctx.shared_preprocess_nv12 = nullptr;
}

static bool init_resize_chain_mid(AppCtx &ctx)
{
    uint32_t mid_w = 0;
    uint32_t mid_h = 0;
    compute_resize_chain_mid_size(ctx.video_frame_w, ctx.video_frame_h, ctx.shared_preprocess_w,
                                  ctx.shared_preprocess_h, &mid_w, &mid_h);
    if (mid_w == 0 || mid_h == 0)
        return true;
    ctx.resize_chain_mid = request_pool_frame(&ctx, mid_w, mid_h, HAL_PIX_FMT_NV12);
    if (!ctx.resize_chain_mid)
        return false;
    ctx.resize_chain_mid_w = mid_w;
    ctx.resize_chain_mid_h = mid_h;
    return true;
}

static void release_resize_chain_mid(AppCtx &ctx)
{
    release_pool_frame(ctx.resize_chain_mid);
    ctx.resize_chain_mid = nullptr;
    ctx.resize_chain_mid_w = 0;
    ctx.resize_chain_mid_h = 0;
}

/** Downscale camera frame once to max(model inputs) before per-model resize. */
static int ensure_frame_preprocess_src(AppCtx *ctx, const HalFrameBuffer *frame, const HalFrameBuffer **out_src)
{
    if (!ctx || !frame || !out_src)
        return HAL_ERR_INVALID_ARG;
    if (ctx->shared_preprocess_w == 0 || ctx->shared_preprocess_h == 0)
    {
        *out_src = frame;
        return HAL_OK;
    }
    if (frame->format == HAL_PIX_FMT_NV12 && frame->width <= ctx->shared_preprocess_w &&
        frame->height <= ctx->shared_preprocess_h)
    {
        *out_src = frame;
        return HAL_OK;
    }
    if (!ctx->shared_preprocess_nv12)
        return HAL_ERR_NO_MEM;
    std::vector<HalFrameBuffer *> temp;
    const int rc = dsp_resize_nv12(ctx, frame, ctx->shared_preprocess_nv12, temp);
    for (auto *b : temp)
        release_pool_frame(b);
    if (rc != HAL_OK)
        return rc;
    copy_frame_metadata_best_effort(frame, ctx->shared_preprocess_nv12);
    *out_src = ctx->shared_preprocess_nv12;
    return HAL_OK;
}

static void release_video_frame(void *video_ctx, HalFrameBuffer *heap_frame)
{
    if (!heap_frame)
        return;
    if (video_ctx)
        (void)HAL_VIDEO_OPS.release_frame(video_ctx, heap_frame);
    delete heap_frame;
}

/** Return frontend buffer to medialib as soon as CPU import/preprocess is done. */
static void release_batch_camera_frame(const std::shared_ptr<FrameBatch> &batch)
{
    if (!batch || !batch->frame)
        return;
    release_video_frame(batch->video_ctx, batch->frame);
    batch->frame = nullptr;
}

static void on_async_done(ModelSlot *slot, ModelPipelineBuf *buf, std::shared_ptr<FrameBatch> batch,
                          uint64_t infer_start_ms, double preprocess_ms, int status);

static HalFrameBuffer *clone_callback_frame(HalFrameBuffer *stack_frame)
{
    if (!stack_frame)
        return nullptr;
    auto *heap = new (std::nothrow) HalFrameBuffer{};
    if (!heap)
        return nullptr;
    *heap = *stack_frame;
    stack_frame->priv = nullptr;
    return heap;
}

static void async_infer_callback(HalTensor *outputs, int num_outputs, int status, void *userdata)
{
    (void)outputs;
    (void)num_outputs;
    auto *job = static_cast<AsyncJobCtx *>(userdata);
    if (!job)
        return;
    ModelSlot *slot = job->slot;
    ModelPipelineBuf *buf = job->buf;
    const uint64_t infer_start_ms = job->infer_t0;
    const double preprocess_ms = job->preprocess_ms;
    auto batch = job->batch;
    delete job;
    if (!slot || !batch)
        return;
    on_async_done(slot, buf, batch, infer_start_ms, preprocess_ms, status);
}

static void on_async_done(ModelSlot *slot, ModelPipelineBuf *buf, std::shared_ptr<FrameBatch> batch,
                          uint64_t infer_start_ms, double preprocess_ms, int status)
{
    const double ms = static_cast<double>(steady_ms() - infer_start_ms);
    slot->last_latency_ms = ms;
    slot->last_preprocess_ms = preprocess_ms;
    slot->stats_pre_ms_acc += preprocess_ms;
    slot->stats_infer_ms_acc += ms;
    ++slot->stats_samples;
    if (status == HAL_OK)
        ++slot->infer_ok;
    else
        ++slot->infer_err;

    if (batch->pending.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        g_frames_done.fetch_add(1, std::memory_order_relaxed);
        release_batch_camera_frame(batch);
    }

    release_pipeline_buf(*slot, buf);
}

static void process_frame(AppCtx *ctx, HalFrameBuffer *frame)
{
    auto batch = std::make_shared<FrameBatch>();
    batch->video_ctx = ctx->video_ctx;
    batch->frame = frame;
    batch->pending.store(static_cast<int>(ctx->models.size()), std::memory_order_relaxed);

    if (ctx->models.empty())
    {
        release_video_frame(ctx->video_ctx, frame);
        return;
    }

    const HalFrameBuffer *dsp_frame = frame;
    if (import_camera_to_app_pool(ctx, frame, &dsp_frame) != HAL_OK)
    {
        HAL_LOG_ERROR("parallel_infer: camera import %ux%u failed", ctx->video_frame_w, ctx->video_frame_h);
        for (size_t mi = 0; mi < ctx->models.size(); mi++)
        {
            ModelSlot &slot = *ctx->models[mi];
            ++slot.preprocess_err;
            on_async_done(&slot, nullptr, batch, steady_ms(), 0.0, HAL_ERR_RESULT);
        }
        return;
    }

    const HalFrameBuffer *pre_src = dsp_frame;
    if (ensure_frame_preprocess_src(ctx, dsp_frame, &pre_src) != HAL_OK)
    {
        HAL_LOG_ERROR("parallel_infer: shared preprocess %ux%u failed", ctx->shared_preprocess_w,
                      ctx->shared_preprocess_h);
        for (size_t mi = 0; mi < ctx->models.size(); mi++)
        {
            ModelSlot &slot = *ctx->models[mi];
            ++slot.preprocess_err;
            on_async_done(&slot, nullptr, batch, steady_ms(), 0.0, HAL_ERR_RESULT);
        }
        return;
    }

    /* Do not hold frontend DMA buffers across async inference — medialib DSP reuses them. */
    release_batch_camera_frame(batch);

    for (size_t mi = 0; mi < ctx->models.size(); mi++)
    {
        ModelSlot &slot = *ctx->models[mi];
        ModelPipelineBuf *pbuf = acquire_pipeline_buf(slot);
        if (!pbuf)
        {
            const int remaining = static_cast<int>(ctx->models.size() - mi);
            if (batch->pending.fetch_sub(remaining, std::memory_order_acq_rel) == remaining)
            {
                release_batch_camera_frame(batch);
                g_frames_done.fetch_add(1, std::memory_order_relaxed);
            }
            return;
        }

        const HalFrameBuffer *nv12 = nullptr;
        uint64_t pre_ms = 0;
        const int prc =
            preprocess_to_model_nv12(ctx, slot.in_w, slot.in_h, pbuf->resize_nv12, pre_src, &nv12, &pre_ms);
        if (prc != HAL_OK)
        {
            ++slot.preprocess_err;
            on_async_done(&slot, pbuf, batch, steady_ms(), static_cast<double>(pre_ms), HAL_ERR_RESULT);
            continue;
        }
        HalTensor inputs[2]{};
        int nin = 0;
        if (prepare_model_inputs(ctx, slot, *pbuf, nv12, inputs, nin) != HAL_OK)
        {
            ++slot.preprocess_err;
            on_async_done(&slot, pbuf, batch, steady_ms(), static_cast<double>(pre_ms), HAL_ERR_RESULT);
            continue;
        }

        if (pbuf->outputs.size() != slot.num_outputs)
        {
            for (auto &ot : pbuf->outputs)
                if (ot.priv)
                    HAL_INFERENCE_OPS.free_tensor(&ot);
            pbuf->outputs.assign(slot.num_outputs, HalTensor{});
        }

        auto *job = new (std::nothrow) AsyncJobCtx{};
        if (!job)
        {
            on_async_done(&slot, pbuf, batch, steady_ms(), static_cast<double>(pre_ms), HAL_ERR_NO_MEM);
            continue;
        }
        job->slot = &slot;
        job->buf = pbuf;
        job->batch = batch;
        job->preprocess_ms = static_cast<double>(pre_ms);
        job->infer_t0 = steady_ms();
        const int arc = HAL_INFERENCE_OPS.run_async(slot.session, inputs, nin, pbuf->outputs.data(),
                                                    static_cast<int>(slot.num_outputs), async_infer_callback, job);
        if (arc != HAL_OK)
        {
            delete job;
            on_async_done(&slot, pbuf, batch, job->infer_t0, static_cast<double>(pre_ms), arc);
        }
    }
}

static void worker_loop(AppCtx *ctx)
{
    while (!g_stop.load(std::memory_order_acquire))
    {
        HalFrameBuffer *frame = nullptr;
        {
            std::unique_lock<std::mutex> lk(ctx->frame_q_mu);
            ctx->frame_q_cv.wait(lk, [&] {
                return g_stop.load(std::memory_order_acquire) || !ctx->frame_q.empty();
            });
            if (g_stop.load(std::memory_order_acquire))
                break;
            frame = ctx->frame_q.front();
            ctx->frame_q.pop_front();
        }
        if (!frame)
            continue;
        process_frame(ctx, frame);

        if (ctx->max_frames > 0 && g_frames_done.load(std::memory_order_relaxed) >= ctx->max_frames)
            g_stop.store(true, std::memory_order_release);
    }
}

static void stats_loop(AppCtx *ctx)
{
    while (!g_stop.load(std::memory_order_acquire))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(ctx->stats_interval_ms));
        if (g_stop.load(std::memory_order_acquire))
            break;

        HalInferencePerfStats sys{};
        const int sys_rc = HAL_INFERENCE_OPS.query_system_performance_stats(nullptr, 200, &sys);

        const uint64_t now_ms = steady_ms();
        const uint64_t frames_done = g_frames_done.load(std::memory_order_relaxed);
        double frame_fps = 0.0;
        if (g_stats_last_tick_ms != 0U && now_ms > g_stats_last_tick_ms)
        {
            frame_fps = static_cast<double>(frames_done - g_stats_last_frames_done) * 1000.0 /
                        static_cast<double>(now_ms - g_stats_last_tick_ms);
        }
        g_stats_last_tick_ms = now_ms;
        g_stats_last_frames_done = frames_done;

        const double interval_s = static_cast<double>(ctx->stats_interval_ms) / 1000.0;

        std::printf("\n=== parallel infer stats (frames in=%llu done=%llu dropped=%llu frame_fps=%.2f) ===\n",
                    static_cast<unsigned long long>(g_frames_in.load()),
                    static_cast<unsigned long long>(frames_done),
                    static_cast<unsigned long long>(g_frames_dropped.load()), frame_fps);
        std::printf(
            "  frame_fps = camera ticks where all %zu models finished; e2e_ms = run_async return to callback "
            "(scheduler queue + NPU, not pure infer)\n",
            ctx->models.size());
        if (sys_rc == HAL_OK)
        {
            std::printf("system: NPU=%.1f%% CPU=%.1f%% RAM=%lld/%lld KiB DSP=%.1f%%\n",
                        static_cast<double>(sys.npu_utilization), static_cast<double>(sys.cpu_utilization),
                        static_cast<long long>(sys.ram_used_kib), static_cast<long long>(sys.ram_total_kib),
                        static_cast<double>(sys.dsp_utilization));
        }

        print_stats_table_header();
        for (auto &slot_ptr : ctx->models)
        {
            ModelSlot &slot = *slot_ptr;
            HalInferenceSessionPerfStats ps{};
            uint32_t pending = 0;
            if (HAL_INFERENCE_OPS.query_session_performance_stats)
            {
                if (HAL_INFERENCE_OPS.query_session_performance_stats(slot.session, 1000, &ps) == HAL_OK)
                    pending = ps.pending_async_jobs;
            }
            const double model_fps =
                (slot.stats_samples > 0 && interval_s > 0.0)
                    ? static_cast<double>(slot.stats_samples) / interval_s
                    : 0.0;
            const double avg_pre =
                slot.stats_samples > 0 ? slot.stats_pre_ms_acc / static_cast<double>(slot.stats_samples)
                                       : slot.last_preprocess_ms;
            const double avg_e2e =
                slot.stats_samples > 0 ? slot.stats_infer_ms_acc / static_cast<double>(slot.stats_samples)
                                       : slot.last_latency_ms;
            print_stats_table_row(slot, model_fps, pending, avg_pre, avg_e2e);
            slot.stats_pre_ms_acc = 0.0;
            slot.stats_infer_ms_acc = 0.0;
            slot.stats_samples = 0;
        }
        std::fflush(stdout);
    }
}

static void video_callback(void *video_ctx, HalFrameBuffer *frame, void *userdata)
{
    auto *ctx = static_cast<AppCtx *>(userdata);
    if (!ctx || !frame || g_stop.load(std::memory_order_acquire))
        return;
    HalFrameBuffer *heap = clone_callback_frame(frame);
    if (!heap)
        return;
    g_frames_in.fetch_add(1, std::memory_order_relaxed);

    std::unique_lock<std::mutex> lk(ctx->frame_q_mu);
    while (ctx->frame_q.size() >= ctx->pipeline_depth && !g_stop.load(std::memory_order_acquire))
    {
        HalFrameBuffer *drop = ctx->frame_q.front();
        ctx->frame_q.pop_front();
        lk.unlock();
        g_frames_dropped.fetch_add(1, std::memory_order_relaxed);
        release_video_frame(video_ctx, drop);
        lk.lock();
    }
    if (g_stop.load(std::memory_order_acquire))
    {
        lk.unlock();
        release_video_frame(video_ctx, heap);
        return;
    }
    ctx->frame_q.push_back(heap);
    ctx->frame_q_cv.notify_one();
}

static uint32_t pick_largest_video_index(void **video_list, uint32_t count)
{
    uint32_t best = 0;
    uint64_t best_area = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        auto *v = static_cast<HalVideoContext *>(video_list[i]);
        if (!v)
            continue;
        const uint64_t area = static_cast<uint64_t>(v->config.width) * static_cast<uint64_t>(v->config.height);
        if (area >= best_area)
        {
            best_area = area;
            best = i;
        }
    }
    return best;
}

static bool init_pipeline_buf(const AppCtx &ctx, const ModelSlot &slot, ModelPipelineBuf &buf)
{
    buf.resize_nv12 = request_pool_frame(&ctx, slot.in_w, slot.in_h, HAL_PIX_FMT_NV12);
    if (!buf.resize_nv12)
        return false;

    if (slot.input_kind == InputKind::RgbPacked)
    {
        buf.rgb_fb = request_pool_frame(&ctx, slot.in_w, slot.in_h, slot.rgb_fmt);
        if (!buf.rgb_fb)
            return false;
    }

    buf.outputs.assign(slot.num_outputs, HalTensor{});
    return true;
}

static bool init_model_pipeline(AppCtx *ctx, ModelSlot &slot)
{
    slot.pipeline_bufs.clear();
    slot.free_bufs.clear();
    slot.pipeline_bufs.resize(ctx->pipeline_depth);
    for (auto &buf : slot.pipeline_bufs)
    {
        if (!init_pipeline_buf(*ctx, slot, buf))
            return false;
        slot.free_bufs.push_back(&buf);
    }
    return true;
}

static bool init_all_frame_buffers(AppCtx &ctx)
{
    update_pool_budgets(ctx);
    if (!init_camera_import_buffer(ctx))
        return false;
    if (!init_shared_preprocess_buffer(ctx))
        return false;
    if (!init_resize_chain_mid(ctx))
        return false;
    for (auto &slot_ptr : ctx.models)
    {
        if (!init_model_pipeline(&ctx, *slot_ptr))
            return false;
    }
    return true;
}

static void free_pipeline_buf(ModelPipelineBuf &buf)
{
    for (auto &t : buf.outputs)
        if (t.priv)
            HAL_INFERENCE_OPS.free_tensor(&t);
    release_pool_frame(buf.resize_nv12);
    buf.resize_nv12 = nullptr;
    release_pool_frame(buf.rgb_fb);
    buf.rgb_fb = nullptr;
}

static void free_model_slot(ModelSlot &slot)
{
    if (slot.session)
        HAL_INFERENCE_OPS.destroy(slot.session);
    for (auto &buf : slot.pipeline_bufs)
        free_pipeline_buf(buf);
    slot.pipeline_bufs.clear();
    slot.free_bufs.clear();
}

static void print_usage(const char *prog)
{
    std::fprintf(stderr,
                 "Usage: %s <hef1> [hef2 ...] [medialib_json] [options]\n"
                 "  medialib_json  optional .json media config; omit (or --default) to use the HAL\n"
                 "                 embedded default config.\n"
                 "  --default      force the HAL embedded default media config.\n"
                 "  --frames N\n"
                 "  --video-index I\n"
                 "  --letterbox\n"
                 "  --pipeline-depth N\n"
                 "  --scheduler-threshold N\n"
                 "  --scheduler-timeout-ms N\n"
                 "  --priority <model_idx:prio>\n"
                 "  --stats-interval-ms N\n",
                 prog);
}

} // namespace

static void on_sigint(int)
{
    g_stop.store(true, std::memory_order_release);
}

int main(int argc, char **argv)
{
    auto ends_with = [](const char *s, const char *suf) {
        size_t ls = std::strlen(s), lf = std::strlen(suf);
        return ls >= lf && std::strcmp(s + ls - lf, suf) == 0;
    };

    AppCtx ctx{};
    const char *json_path = nullptr;   // optional; nullptr => HAL embedded default config
    std::vector<std::string> hef_paths;
    std::vector<std::pair<uint32_t, uint8_t>> priorities;

    for (int i = 1; i < argc; i++)
    {
        if (std::strcmp(argv[i], "--default") == 0)
        {
            json_path = nullptr;
            continue;
        }
        if (argv[i][0] != '-')
        {
            /* positional: *.json -> media config; everything else (e.g. *.hef) -> model */
            if (ends_with(argv[i], ".json"))
            {
                json_path = argv[i];
            }
            else
            {
                hef_paths.emplace_back(argv[i]);
            }
            continue;
        }
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            ctx.max_frames = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--video-index") == 0 && i + 1 < argc)
            ctx.video_index = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--letterbox") == 0)
            ctx.use_letterbox = true;
        else if (std::strcmp(argv[i], "--pipeline-depth") == 0 && i + 1 < argc)
            ctx.pipeline_depth = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--scheduler-threshold") == 0 && i + 1 < argc)
            ctx.scheduler_threshold = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--scheduler-timeout-ms") == 0 && i + 1 < argc)
            ctx.scheduler_timeout_ms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--stats-interval-ms") == 0 && i + 1 < argc)
            ctx.stats_interval_ms = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 10));
        else if (std::strcmp(argv[i], "--priority") == 0 && i + 1 < argc)
        {
            const char *spec = argv[++i];
            const char *colon = std::strchr(spec, ':');
            if (colon)
            {
                const uint32_t idx = static_cast<uint32_t>(std::strtoul(spec, nullptr, 10));
                const uint32_t pr = static_cast<uint32_t>(std::strtoul(colon + 1, nullptr, 10));
                priorities.emplace_back(idx, static_cast<uint8_t>(pr));
            }
        }
        else
        {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    if (hef_paths.empty() || hef_paths.size() > kMaxModels)
    {
        std::fprintf(stderr, "Provide 1..%zu HEF paths\n", kMaxModels);
        return EXIT_FAILURE;
    }
    if (ctx.pipeline_depth < 1)
        ctx.pipeline_depth = 1;

    std::signal(SIGINT, on_sigint);
    std::signal(SIGTERM, on_sigint);

    HalMediaConfig mcfg{};
    mcfg.config_path = json_path;
    int rc = HAL_MEDIA_OPS.init(&mcfg, &ctx.media_ctx);
    if (rc != HAL_OK || !ctx.media_ctx)
    {
        std::fprintf(stderr, "HAL_MEDIA_OPS.init failed rc=%d\n", rc);
        return EXIT_FAILURE;
    }
    /* Same as ai_example_v2: app feeds encoders only when needed; avoids auto_feed errors
     * for frontends without a matching encoder (e.g. sink2 in ai_example profile). */
    (void)HAL_MEDIA_OPS.set_encoder_auto_feed(ctx.media_ctx, false);

    HalDspConfig dcfg{};
    dcfg.device_priority = 0;
    rc = HAL_DSP_OPS.init(&dcfg, &ctx.dsp_ctx);
    if (rc != HAL_OK || !ctx.dsp_ctx)
    {
        std::fprintf(stderr, "HAL_DSP_OPS.init failed rc=%d\n", rc);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }

    HalInferenceRuntimeConfig rtcfg{};
    hal_inference_runtime_config_defaults(&rtcfg);
    ctx.runtime = HAL_INFERENCE_OPS.runtime_acquire ? HAL_INFERENCE_OPS.runtime_acquire(&rtcfg) : nullptr;
    if (!ctx.runtime)
    {
        std::fprintf(stderr, "runtime_acquire failed (need HailoRT + scheduler)\n");
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }
    std::printf("Shared runtime: group=%s algorithm=round_robin multi_process=%s\n", rtcfg.vdevice_group_id,
                rtcfg.multi_process_service ? "true" : "false");

    for (size_t mi = 0; mi < hef_paths.size(); mi++)
    {
        ctx.models.push_back(std::make_unique<ModelSlot>());
        ModelSlot &slot = *ctx.models.back();
        slot.hef_path = hef_paths[mi];
        slot.label = basename_no_ext(slot.hef_path);

        HalInferenceConfig icfg{};
        std::snprintf(icfg.model_path, sizeof(icfg.model_path), "%s", slot.hef_path.c_str());
        icfg.batch_size = 1;
        icfg.timeout_ms = 5000;
        icfg.use_dma = false;
        icfg.runtime = ctx.runtime;
        icfg.scheduler.scheduler_threshold = ctx.scheduler_threshold;
        icfg.scheduler.scheduler_timeout_ms = ctx.scheduler_timeout_ms;
        for (const auto &pr : priorities)
        {
            if (pr.first == mi)
                icfg.scheduler.scheduler_priority = pr.second;
        }

        slot.session = HAL_INFERENCE_OPS.create(&icfg);
        if (!slot.session)
        {
            std::fprintf(stderr, "create failed for %s\n", slot.hef_path.c_str());
            ctx.models.pop_back();
            break;
        }
        if (HAL_INFERENCE_OPS.get_model_info(slot.session, &slot.info) != HAL_OK)
        {
            std::fprintf(stderr, "get_model_info failed for %s\n", slot.hef_path.c_str());
            free_model_slot(slot);
            ctx.models.pop_back();
            break;
        }
        slot.num_inputs = slot.info.num_inputs;
        slot.num_outputs = slot.info.num_outputs;
        slot.input_kind = detect_input_kind(slot.info);
        if (slot.input_kind == InputKind::RgbPacked)
            slot.rgb_fmt = HAL_PIX_FMT_RGB24;
        if (auto hw = model_input_hw(slot.info))
        {
            slot.in_w = hw->first;
            slot.in_h = hw->second;
        }
        if (slot.in_w == 0 || slot.in_h == 0)
        {
            std::fprintf(stderr, "Could not determine input size for %s\n", slot.hef_path.c_str());
            free_model_slot(slot);
            ctx.models.pop_back();
            break;
        }
        std::printf("Model[%zu] %-18s  in=%ux%u  kind=%s  group=%s  prio=%u  th=%u  to=%ums\n", mi,
                    slot.label.c_str(), slot.in_w, slot.in_h, input_kind_str(slot.input_kind),
                    slot.info.network_group_name,
                    icfg.scheduler.scheduler_priority ? icfg.scheduler.scheduler_priority
                                                      : HAL_INFER_SCHED_PRIORITY_NORMAL,
                    ctx.scheduler_threshold, ctx.scheduler_timeout_ms);
    }
    if (ctx.models.size() != hef_paths.size())
    {
        for (auto &s : ctx.models)
            free_model_slot(*s);
        ctx.models.clear();
        if (HAL_INFERENCE_OPS.runtime_release)
            HAL_INFERENCE_OPS.runtime_release(ctx.runtime);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }
    update_shared_preprocess_size(ctx);
    if (ctx.models.size() >= 4U && ctx.pipeline_depth > 2U)
    {
        std::printf("Note: %zu models with pipeline_depth=%u — consider --pipeline-depth 2 if CMA is tight\n",
                    ctx.models.size(), ctx.pipeline_depth);
    }
    void *video_list_raw = nullptr;
    uint32_t video_count = 0;
    rc = HAL_MEDIA_OPS.get_video_list(ctx.media_ctx, &video_list_raw, &video_count);
    auto **video_list = reinterpret_cast<void **>(video_list_raw);
    if (rc != HAL_OK || !video_list || video_count == 0)
    {
        std::fprintf(stderr, "get_video_list failed rc=%d\n", rc);
        for (auto &s : ctx.models)
            free_model_slot(*s);
        release_shared_preprocess_buffer(ctx);
        if (HAL_INFERENCE_OPS.runtime_release)
            HAL_INFERENCE_OPS.runtime_release(ctx.runtime);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }
    if (ctx.video_index >= video_count)
        ctx.video_index = pick_largest_video_index(video_list, video_count);
    ctx.video_ctx = video_list[ctx.video_index];
    {
        auto *v = static_cast<HalVideoContext *>(ctx.video_ctx);
        ctx.stream_key = (v && v->video_name[0]) ? std::string(v->video_name) : std::string();
        ctx.video_frame_w = v ? v->config.width : 0U;
        ctx.video_frame_h = v ? v->config.height : 0U;
        std::printf("Video frontend[%u]: %ux%u (%s)\n", ctx.video_index, ctx.video_frame_w, ctx.video_frame_h,
                    v ? v->video_name : "?");
    }
    if (!init_all_frame_buffers(ctx))
    {
        std::fprintf(stderr, "Frame buffer pool init failed (check CMA / pool budgets)\n");
        for (auto &s : ctx.models)
            free_model_slot(*s);
        release_resize_chain_mid(ctx);
        release_shared_preprocess_buffer(ctx);
        release_camera_import_buffer(ctx);
        ctx.models.clear();
        if (HAL_INFERENCE_OPS.runtime_release)
            HAL_INFERENCE_OPS.runtime_release(ctx.runtime);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }
    {
        const PoolGeomKey nv12_key{ctx.shared_preprocess_w, ctx.shared_preprocess_h, HAL_PIX_FMT_NV12};
        const auto it = ctx.pool_max_by_geom.find(nv12_key);
        const uint32_t pool_max = (it != ctx.pool_max_by_geom.end()) ? it->second : 0U;
        std::printf("Shared preprocess target: %ux%u (DMA-BUF pool_max=%u; DSP resize per frame)\n",
                    ctx.shared_preprocess_w, ctx.shared_preprocess_h, pool_max);
    }

    rc = HAL_VIDEO_OPS.subscribe_stream(ctx.video_ctx,
                                        ctx.stream_key.empty() ? nullptr : ctx.stream_key.c_str(), video_callback,
                                        &ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "HAL_VIDEO_OPS.subscribe_stream failed rc=%d\n", rc);
        for (auto &s : ctx.models)
            free_model_slot(*s);
        release_resize_chain_mid(ctx);
        release_shared_preprocess_buffer(ctx);
        release_camera_import_buffer(ctx);
        if (HAL_INFERENCE_OPS.runtime_release)
            HAL_INFERENCE_OPS.runtime_release(ctx.runtime);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }

    rc = HAL_MEDIA_OPS.start(ctx.media_ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "HAL_MEDIA_OPS.start failed rc=%d\n", rc);
        (void)HAL_VIDEO_OPS.unsubscribe_stream(ctx.video_ctx,
                                               ctx.stream_key.empty() ? nullptr : ctx.stream_key.c_str());
        for (auto &s : ctx.models)
            free_model_slot(*s);
        release_resize_chain_mid(ctx);
        release_shared_preprocess_buffer(ctx);
        release_camera_import_buffer(ctx);
        if (HAL_INFERENCE_OPS.runtime_release)
            HAL_INFERENCE_OPS.runtime_release(ctx.runtime);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }

    ctx.worker = std::thread(worker_loop, &ctx);
    ctx.stats_thread = std::thread(stats_loop, &ctx);

    std::printf("Build: %s\n", kParallelInferBuildTag);
    std::printf("Running %zu models pipelined (depth=%u, th=%u, to=%ums). Ctrl+C to stop.\n", ctx.models.size(),
                ctx.pipeline_depth, ctx.scheduler_threshold, ctx.scheduler_timeout_ms);
    while (!g_stop.load(std::memory_order_acquire))
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

    ctx.frame_q_cv.notify_all();
    if (ctx.worker.joinable())
        ctx.worker.join();
    if (ctx.stats_thread.joinable())
        ctx.stats_thread.join();

    for (auto &slot_ptr : ctx.models)
        wait_model_pipeline_idle(*slot_ptr);

    {
        std::lock_guard<std::mutex> lk(ctx.frame_q_mu);
        for (auto *f : ctx.frame_q)
            release_video_frame(ctx.video_ctx, f);
        ctx.frame_q.clear();
    }

    if (ctx.video_ctx)
        (void)HAL_VIDEO_OPS.unsubscribe_stream(ctx.video_ctx,
                                               ctx.stream_key.empty() ? nullptr : ctx.stream_key.c_str());

    (void)HAL_MEDIA_OPS.stop(ctx.media_ctx);

    for (auto &slot_ptr : ctx.models)
        free_model_slot(*slot_ptr);
    ctx.models.clear();
    release_resize_chain_mid(ctx);
    release_shared_preprocess_buffer(ctx);
    release_camera_import_buffer(ctx);

    if (HAL_INFERENCE_OPS.runtime_release)
        HAL_INFERENCE_OPS.runtime_release(ctx.runtime);
    HAL_DSP_OPS.deinit(ctx.dsp_ctx);
    HAL_MEDIA_OPS.deinit(ctx.media_ctx);
    std::printf("Done. frames in=%llu done=%llu dropped=%llu\n",
                static_cast<unsigned long long>(g_frames_in.load()),
                static_cast<unsigned long long>(g_frames_done.load()),
                static_cast<unsigned long long>(g_frames_dropped.load()));
    return EXIT_SUCCESS;
}
