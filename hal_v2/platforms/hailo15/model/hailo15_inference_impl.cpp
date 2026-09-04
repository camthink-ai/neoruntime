/**
 * @file hailo15_inference_impl.cpp
 * @brief Hailo-15 HAL inference implementation (HailoRT).
 *
 * This module is conditionally built when HailoRT headers/libs are available.
 * When unavailable, it provides stubs returning HAL_ERR_NOT_SUPPORTED.
 */

#include "common/hal_common.h"
#include "common/hal_log.h"
#include "common/hal_buffer.h"
#include "common/hal_hailo15_priv.hpp"
#include "model/hal_inference.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#include <cstdlib>
#include <chrono>
#include <atomic>
#include <algorithm>

#if defined(HAL_HAVE_HAILORT)
#include "hailo/hailort.hpp"
#include "hailo/hailort.h"
#include "hailo/transform.hpp"
#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
#include <hailo_postprocess_tools/objects/hailo_objects.hpp>
#include <hailo_postprocess_tools/objects/hailo_tensors.hpp>
#endif
#endif

namespace
{

using TensorPriv = hal_v2::hailo15::TensorPriv;

static inline size_t align_up(size_t v, size_t a)
{
    if (a == 0)
        return v;
    const size_t rem = v % a;
    return rem ? (v + (a - rem)) : v;
}

static std::shared_ptr<void> alloc_aligned_shared(size_t size, size_t align)
{
    if (size == 0)
        return {};
    if (align == 0)
        align = 4096;
    void *ptr = nullptr;
    const size_t sz = align_up(size, align);
    const int rc = posix_memalign(&ptr, align, sz);
    if (rc != 0 || !ptr)
        return {};
    return std::shared_ptr<void>(ptr, [](void *m) { std::free(m); });
}

#if defined(HAL_HAVE_HAILORT)

/** Internal heap object behind the opaque C HalInferenceRuntime handle.
 *  Wraps a reference to the process-wide shared VDevice so runtime_acquire()/
 *  release() can hand out counted handles. The shared singleton already uses
 *  ROUND_ROBIN scheduling, so every acquired runtime shares one NPU scheduler
 *  with all model sessions — matching the upstream multi-model design. */
struct Hailo15RuntimeHandle
{
    std::shared_ptr<hailort::VDevice> vdevice;
};

/** Per-job context for run_async(). Kept alive (held in pending_async and
 *  captured by the HailoRT completion callback) until the NPU reports done. */
struct Hailo15InferAsyncCtx
{
    std::vector<HalTensor> outputs;
    HalInferenceAsyncCallback callback = nullptr;
    void *userdata = nullptr;
    std::unique_ptr<hailort::AsyncInferJob> job;
};

struct Hailo15InferPriv
{
    std::string device_id;
    std::shared_ptr<hailort::VDevice> vdevice;
    std::shared_ptr<hailort::InferModel> infer_model;
    hailort::ConfiguredInferModel configured;
    hailort::ConfiguredInferModel::Bindings bindings;
    std::vector<std::string> input_names;
    std::vector<std::string> output_names;
    std::string hef_basename;  // e.g. "hailo_yolov8n_384_640" from model path
    HalInferenceConfig cfg{};

    // Async inference bookkeeping (run_async). pending_async lets destroy()
    // flush in-flight jobs before tearing the session down, and lets
    // query_session_performance_stats report queue depth; fps_mtx /
    // fps_window_* drive the measured throughput window.
    std::atomic<uint64_t> total_inferences{0};
    std::mutex async_mtx;
    // Serializes bind() + run_async()/run() per session. p->bindings holds the
    // MemoryViews that set_buffer() rewrites on every call; without this lock,
    // concurrent workers serving the same session race on the output buffer
    // pointers HailoRT snapshots at submit time, aliasing buffers so one job's
    // completion frees an output buffer another job's NPU write is still in
    // flight (heap-use-after-free under K>=4 concurrency).
    std::mutex submit_mtx;
    std::vector<std::shared_ptr<Hailo15InferAsyncCtx>> pending_async;
    std::mutex fps_mtx;
    uint64_t fps_window_start_ms = 0;
    uint32_t fps_window_count = 0;

    // Cached host-side input transform (tensor_from_frame_ex). Keyed by
    // (srcW, srcH, contentW, contentH); frames from one pipeline are a
    // constant size, so the cache hit rate is ~100%.
    std::mutex transform_mu;
    std::unique_ptr<hailort::InputTransformContext> transform_ctx;
    uint32_t transform_key[4]{0, 0, 0, 0};
};

/**
 * Shared VDevice singleton for multi-model parallel inference.
 *
 * All models share a single VDevice with ROUND_ROBIN scheduling so that
 * HailoRT's internal scheduler can pipeline multiple network groups on the
 * NPU instead of serialising independent VDevice connections.
 *
 * Cross-process sharing: the VDevice is shared with camera-daemon's medialib
 * AI-ISP by joining its group_id (kSharedVDeviceGroupId = the medialib
 * hailort.device-id "device0") via hailort_server. A mismatched group makes the
 * server refuse ai-runtime's attach with HAILO_DEVICE_IN_USE(73) as soon as a
 * model loads while AI-ISP is active.
 *
 * Cached for the process lifetime; only reset_shared_vdevice() clears it (on
 * the connection-lost recovery path). Each model holds a shared_ptr copy, so
 * the device stays live while any model is loaded.
 * Thread-safety: guarded by g_shared_vdevice_mu.
 */
static std::mutex g_shared_vdevice_mu;
static std::shared_ptr<hailort::VDevice> g_shared_vdevice;

// group_id shared with camera-daemon's medialib AI-ISP VDevice. Must equal the
// medialib's hailort.device-id ("device0"); a mismatch makes hailort_server
// refuse the second attach (HAILO_DEVICE_IN_USE 73) and breaks NPU coexistence.
static constexpr const char *kSharedVDeviceGroupId = "device0";

static std::shared_ptr<hailort::VDevice> get_shared_vdevice()
{
    std::lock_guard<std::mutex> lock(g_shared_vdevice_mu);
    if (g_shared_vdevice)
        return g_shared_vdevice;

    hailo_vdevice_params_t params = {};
    hailo_init_vdevice_params(&params);

    // Join the medialib AI-ISP's group so ai-runtime and AI-ISP share the single NPU
    // via hailort_server. The medialib opens its VDevice with group_id = its hailort.device-id
    // ("device0"); the former default "aipc" made the server refuse the second attach with
    // HAILO_DEVICE_IN_USE(73) -> OUT_OF_PHYSICAL_DEVICES(74) as soon as a model loaded while
    // AI-ISP was active. Same group + multi_process_service lets HailoRT pipeline both
    // consumers' network groups on the one physical device.
    params.group_id = kSharedVDeviceGroupId;
    params.multi_process_service = true;
    params.scheduling_algorithm = HAILO_SCHEDULING_ALGORITHM_ROUND_ROBIN;

    auto exp = hailort::VDevice::create(params);
    if (!exp)
    {
        HAL_LOG_ERROR("hailo15_inference: shared VDevice create failed (status=%d)", (int)exp.status());
        return nullptr;
    }
    g_shared_vdevice = exp.release();
    HAL_LOG_INFO("hailo15_inference: created shared VDevice (group='%s', multi_process_service=1, scheduler=ROUND_ROBIN)",
                 params.group_id);
    return g_shared_vdevice;
}

/**
 * HailoRT statuses that unambiguously mean the cached VDevice's connection to
 * hailort_server is gone (the server crashed / was restarted / OOM-killed, or
 * the unix-socket endpoint died). On any of these the cached g_shared_vdevice
 * is permanently unusable: every later operation on it re-fails, and — because
 * get_shared_vdevice() only ever returns the cached pointer — ai-runtime would
 * stay dead until the whole process is restarted.
 *
 * Deliberately EXCLUDES:
 *  - HAILO_TIMEOUT: transient NPU stalls are normal (model-showcase infer loop
 *    already absorbs them); resetting on every timeout would churn the VDevice.
 *  - HAILO_INTERNAL_FAILURE: too broad — could mask a genuine model/fw bug.
 *  - HAILO_OUT_OF_PHYSICAL_DEVICES: a fresh-create failure (no cached singleton
 *    to heal); it signals a server-side leak, not a dead client connection.
 */
static inline bool hailo15_vdevice_connection_lost(hailo_status st)
{
    return st == HAILO_COMMUNICATION_CLOSED   // 62 — endpoint closed
        || st == HAILO_CONNECTION_REFUSED     // 89 — server gone / refused
        || st == HAILO_RPC_FAILED             // 77 — RPC failed
        || st == HAILO_STREAM_ABORT;          // 63 — stream recv/send aborted
}

/**
 * Clear the cached singleton so the next get_shared_vdevice() re-creates it
 * against a fresh hailort_server. Safe to call from any thread (including the
 * per-frame infer path): it only drops the GLOBAL reference; live sessions keep
 * their own shared_ptr copies (and their bound ConfiguredInferModels) alive
 * until they are torn down. Idempotent — returns false (no-op) when there was
 * nothing cached, so callers can rate-limit logging.
 *
 * Returns true iff the singleton was actually reset.
 */
static bool reset_shared_vdevice()
{
    std::lock_guard<std::mutex> lock(g_shared_vdevice_mu);
    if (!g_shared_vdevice)
        return false;
    HAL_LOG_WARNING("hailo15_inference: resetting shared VDevice after a "
                    "connection-lost error; the next model load will recreate it "
                    "(live sessions recover on their next infer/create)");
    g_shared_vdevice.reset();
    return true;
}

/**
 * Log + reset on a connection-lost error. Logs only when a reset actually
 * happened, so repeated failures on an already-reset (dead) session do not
 * spam the journal while waiting for the caller to reload the model.
 */
static void hailo15_notify_vdevice_lost(hailo_status st, const char *where)
{
    if (reset_shared_vdevice())
    {
        HAL_LOG_WARNING("hailo15_inference: VDevice connection lost during %s "
                        "(status=%d) — singleton cleared; recovery on next model load",
                        where, (int)st);
    }
}

static inline uint64_t monotonic_ms()
{
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count());
}

/** Account one completed inference: bump the total and roll the fps window. */
static void hailo15_record_inference(Hailo15InferPriv *p)
{
    p->total_inferences.fetch_add(1, std::memory_order_relaxed);
    const uint64_t now = monotonic_ms();
    std::lock_guard<std::mutex> lock(p->fps_mtx);
    if (p->fps_window_start_ms == 0)
        p->fps_window_start_ms = now;
    ++p->fps_window_count;
}

/** Bind caller inputs and (allocate-on-demand) outputs onto p->bindings.
 *  Extracted so the synchronous run() and asynchronous run_async() share one
 *  implementation. On success every outputs[i] is backed by a TensorPriv that
 *  the postprocess ROI bridge can later populate. */
static int hailo15_bind_inputs_outputs(Hailo15InferPriv *p, const HalTensor *inputs, HalTensor *outputs)
{
    const size_t want_in = p->input_names.size();
    const size_t want_out = p->output_names.size();

    // Bind inputs
    for (size_t i = 0; i < want_in; i++)
    {
        const auto &name = p->input_names[i];
        const HalTensor &in = inputs[i];
        if (!in.data || in.byte_size == 0)
            return HAL_ERR_INVALID_ARG;
        const size_t frame_size = p->infer_model->input(name)->get_frame_size();
        if (in.byte_size != frame_size)
        {
            HAL_LOG_ERROR("hailo15_inference: input[%zu] byte_size mismatch (got=%u expected=%zu)", i, in.byte_size,
                          frame_size);
            return HAL_ERR_INVALID_SIZE;
        }
        hailo_status st = p->bindings.input(name)->set_buffer(hailort::MemoryView(in.data, in.byte_size));
        if (HAILO_SUCCESS != st)
        {
            HAL_LOG_ERROR("hailo15_inference: set input buffer failed (st=%d)", (int)st);
            return HAL_ERR_RESULT;
        }
    }

    // Bind outputs (allocate when needed)
    for (size_t i = 0; i < want_out; i++)
    {
        const auto &name = p->output_names[i];
        HalTensor &out = outputs[i];
        const size_t frame_size = p->infer_model->output(name)->get_frame_size();
        std::shared_ptr<void> holder;
        if (!out.data)
        {
            holder = alloc_aligned_shared(frame_size, 4096);
            if (!holder)
                return HAL_ERR_NO_MEM;
            out.data = holder.get();
            out.byte_size = static_cast<uint32_t>(frame_size);
            out.dtype = HAL_DTYPE_UINT8;
            out.dma_fd = -1;
            std::snprintf(out.name, sizeof(out.name), "%s", name.c_str());
            out.ndim = 1;
            out.shape[0] = static_cast<int32_t>(frame_size);
            out.priv = new (std::nothrow) TensorPriv{holder};
            if (!out.priv)
                return HAL_ERR_NO_MEM;
        }
        else
        {
            if (out.byte_size < frame_size)
                return HAL_ERR_INSUFFICIENT_BUFFER;
#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
            /* Caller-supplied output buffer: still need TensorPriv so postprocess can attach HailoROI. */
            if (!out.priv)
            {
                std::shared_ptr<void> holder; /* empty — does not own out.data */
                out.priv = new (std::nothrow) TensorPriv{holder};
                if (!out.priv)
                    return HAL_ERR_NO_MEM;
            }
#endif
        }
        hailo_status st = p->bindings.output(name)->set_buffer(hailort::MemoryView(out.data, frame_size));
        if (HAILO_SUCCESS != st)
        {
            HAL_LOG_ERROR("hailo15_inference: set output buffer failed (st=%d)", (int)st);
            return HAL_ERR_RESULT;
        }
    }

    return HAL_OK;
}

/** Build a vendor-compatible ROI with attached output tensors so Hailo
 *  postprocess libraries can consume outputs[0].priv. For NMS outputs the
 *  network-group name prefix is replaced with the HEF basename (postprocess
 *  looks tensors up by HEF filename). */
static void hailo15_attach_postprocess_roi(Hailo15InferPriv *p, HalTensor *outputs, size_t want_out)
{
#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
    HailoROIPtr roi = std::make_shared<HailoROI>(HailoBBox(0.0f, 0.0f, 1.0f, 1.0f));
    for (size_t i = 0; i < want_out; i++)
    {
        const auto &name = p->output_names[i];
        HalTensor &out = outputs[i];
        auto exp = p->infer_model->output(name);
        if (!exp.has_value())
            continue;
        const auto &stinfo = exp.value();

        hailo_vstream_info_t vi{};
        std::memset(&vi, 0, sizeof(vi));
        const bool is_nms = stinfo.is_nms();
        if (is_nms && !p->hef_basename.empty())
        {
            // Replace network-group prefix with HEF filename for postprocess compat.
            // e.g. "yolov8n/yolov8_nms_postprocess" → "hailo_yolov8n_384_640/yolov8_nms_postprocess"
            const auto slash_pos = name.find('/');
            if (slash_pos != std::string::npos)
                std::snprintf(vi.name, sizeof(vi.name), "%s%s", p->hef_basename.c_str(), name.c_str() + slash_pos);
            else
                std::snprintf(vi.name, sizeof(vi.name), "%s", name.c_str());
        }
        else
        {
            std::snprintf(vi.name, sizeof(vi.name), "%s", name.c_str());
        }
        const hailo_3d_image_shape_t shp = stinfo.shape();
        vi.shape = shp;
        const hailo_format_t fmt = stinfo.format();
        vi.format = fmt;
        const std::vector<hailo_quant_info_t> qinfos = stinfo.get_quant_infos();
        if (!qinfos.empty())
            vi.quant_info = qinfos[0];

        if (stinfo.is_nms())
        {
            auto nms_exp = stinfo.get_nms_shape();
            if (nms_exp.has_value())
                vi.nms_shape = nms_exp.value();
        }

        auto ht = std::make_shared<HailoTensor>(static_cast<uint8_t *>(out.data), vi);
        roi->add_tensor(ht);
    }

    // Attach ROI to the first output's priv (must exist if we allocated outputs).
    if (want_out > 0 && outputs[0].priv)
    {
        auto *tp = static_cast<TensorPriv *>(outputs[0].priv);
        tp->roi = roi;
    }
#else
    (void)p;
    (void)outputs;
    (void)want_out;
#endif
}

/** Block until every in-flight async job for this session has completed.
 *  Called from destroy() so the completion callbacks (which capture p) never
 *  fire after the session is freed. */
static void hailo15_wait_pending_async(Hailo15InferPriv *p)
{
    std::vector<std::shared_ptr<Hailo15InferAsyncCtx>> snapshot;
    {
        std::lock_guard<std::mutex> lock(p->async_mtx);
        snapshot.swap(p->pending_async);
    }
    const uint32_t timeout_ms = p->cfg.timeout_ms ? p->cfg.timeout_ms : 10000;
    for (auto &ctx : snapshot)
    {
        if (ctx->job)
            ctx->job->wait(std::chrono::milliseconds(timeout_ms));
    }
    snapshot.clear();
}
#endif

static inline bool str_has_json_object_prefix(const char *s)
{
    if (!s)
        return false;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        ++s;
    return *s == '{';
}

static inline std::string extract_json_string_value_best_effort(const char *json, const char *key,
                                                                const char *default_value)
{
    if (!json || !key)
        return default_value ? std::string(default_value) : std::string();
    // Very small best-effort parser: searches for "key" : "value"
    const std::string kq = std::string("\"") + key + "\"";
    const char *p = std::strstr(json, kq.c_str());
    if (!p)
        return default_value ? std::string(default_value) : std::string();
    p = std::strchr(p, ':');
    if (!p)
        return default_value ? std::string(default_value) : std::string();
    p++;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
        ++p;
    if (*p != '\"')
        return default_value ? std::string(default_value) : std::string();
    p++;
    const char *q = std::strchr(p, '\"');
    if (!q)
        return default_value ? std::string(default_value) : std::string();
    return std::string(p, q);
}

static inline void preprocess_defaults(HalPreprocessConfig *p)
{
    if (!p)
        return;
    p->color = HAL_PREPROCESS_COLOR_NONE;
    p->resize = HAL_PREPROCESS_RESIZE_BILINEAR;
    p->letterbox = HAL_PREPROCESS_LETTERBOX_NONE;
    p->pad_value = 0;
    p->normalize = false;
    p->mean[0] = p->mean[1] = p->mean[2] = 0.0f;
    p->std[0] = p->std[1] = p->std[2] = 1.0f;
    p->mean[3] = 0.0f;
    p->std[3] = 1.0f;
    p->output_layout = HAL_TENSOR_LAYOUT_UNKNOWN;
}

#if defined(HAL_HAVE_HAILORT)
static inline void copy_bounded_cstr(char *dst, size_t cap, const char *src)
{
    if (!dst || cap == 0)
        return;
    if (!src)
    {
        dst[0] = '\0';
        return;
    }
    const size_t slen = std::strlen(src);
    const size_t copy_len = slen < cap - 1U ? slen : cap - 1U;
    std::memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';
}

static inline void copy_bounded_cstr(char *dst, size_t cap, const std::string &src)
{
    copy_bounded_cstr(dst, cap, src.c_str());
}

static inline HalDataType hailo_format_type_to_hal(hailo_format_type_t t)
{
    switch (t)
    {
        case HAILO_FORMAT_TYPE_UINT8:
            return HAL_DTYPE_UINT8;
        case HAILO_FORMAT_TYPE_UINT16:
            return HAL_DTYPE_UINT16;
        case HAILO_FORMAT_TYPE_FLOAT32:
            return HAL_DTYPE_FLOAT32;
        default:
            return HAL_DTYPE_UNKNOWN;
    }
}

static inline HalTensorLayout hailo_format_order_to_hal_layout(hailo_format_order_t o)
{
    switch (o)
    {
        case HAILO_FORMAT_ORDER_NHWC:
        case HAILO_FORMAT_ORDER_FCR:
        case HAILO_FORMAT_ORDER_F8CR:
            return HAL_TENSOR_LAYOUT_NHWC;
        case HAILO_FORMAT_ORDER_NHCW:
            return HAL_TENSOR_LAYOUT_NCHW;
        case HAILO_FORMAT_ORDER_NC:
            return HAL_TENSOR_LAYOUT_NC;
        case HAILO_FORMAT_ORDER_NHW:
            return HAL_TENSOR_LAYOUT_NHW;
        // YUV formats are not a tensor layout in the classic sense, but treat them as a 2D image-like layout.
        case HAILO_FORMAT_ORDER_NV12:
        case HAILO_FORMAT_ORDER_NV21:
        case HAILO_FORMAT_ORDER_I420:
        case HAILO_FORMAT_ORDER_YUY2:
            return HAL_TENSOR_LAYOUT_NHW;
        default:
            return HAL_TENSOR_LAYOUT_UNKNOWN;
    }
}

/** HEF stream_info union: shape is invalid when format is NMS — use nms_info instead. */
static inline bool hailo_format_order_is_nms_shape_invalid(hailo_format_order_t o)
{
    switch (o)
    {
        case HAILO_FORMAT_ORDER_HAILO_NMS:
        case HAILO_FORMAT_ORDER_HAILO_NMS_WITH_BYTE_MASK:
        case HAILO_FORMAT_ORDER_HAILO_NMS_ON_CHIP:
        case HAILO_FORMAT_ORDER_HAILO_NMS_BY_CLASS:
        case HAILO_FORMAT_ORDER_HAILO_NMS_BY_SCORE:
            return true;
        default:
            return false;
    }
}

/**
 * Map HailoRT InferStream into HalModelTensorInfo (shape, dtype, layout, quant, frame bytes, NMS meta).
 */
static void fill_hal_tensor_info_from_stream(HalModelTensorInfo *slot, const hailort::InferModel::InferStream &st,
                                             bool is_output)
{
    std::memset(slot->shape, 0, sizeof(slot->shape));
    slot->ndim = 0;
    slot->dtype = HAL_DTYPE_UNKNOWN;
    slot->layout = HAL_TENSOR_LAYOUT_UNKNOWN;
    slot->quant_scale = 0.f;
    slot->quant_zero_point = 0.f;
    slot->quant_range_min = 0.f;
    slot->quant_range_max = 0.f;
    slot->byte_size = 0;
    slot->is_nms = 0;
    slot->is_nv12 = 0;
    slot->reserved[0] = 0;
    slot->reserved[1] = 0;
    slot->nms_number_of_classes = 0;
    slot->nms_max_bboxes_per_class = 0;
    slot->nms_max_bboxes_total = 0;
    slot->nms_max_accumulated_mask_size = 0;

    const size_t fsz = st.get_frame_size();
    slot->byte_size =
        fsz > static_cast<size_t>(UINT32_MAX) ? UINT32_MAX : static_cast<uint32_t>(fsz);

    const hailo_format_t fmt = st.format();
    slot->dtype = hailo_format_type_to_hal(fmt.type);
    slot->layout = hailo_format_order_to_hal_layout(fmt.order);
    // Mark NV12-like YUV inputs. This is the most reliable signal for NV12 dimension reconciliation.
    if (fmt.order == HAILO_FORMAT_ORDER_NV12 || fmt.order == HAILO_FORMAT_ORDER_NV21 || fmt.order == HAILO_FORMAT_ORDER_I420)
        slot->is_nv12 = 1;

    const std::vector<hailo_quant_info_t> qinfos = st.get_quant_infos();
    if (!qinfos.empty())
    {
        const hailo_quant_info_t &qi = qinfos[0];
        slot->quant_zero_point = qi.qp_zp;
        slot->quant_scale = qi.qp_scale;
        slot->quant_range_min = qi.limvals_min;
        slot->quant_range_max = qi.limvals_max;
    }

    if (is_output && st.is_nms())
    {
        auto nms_exp = st.get_nms_shape();
        if (nms_exp.has_value())
        {
            const hailo_nms_shape_t &n = nms_exp.value();
            slot->is_nms = 1;
            slot->nms_number_of_classes = n.number_of_classes;
            slot->nms_max_bboxes_per_class = n.max_bboxes_per_class;
            slot->nms_max_bboxes_total = n.max_bboxes_total;
            slot->nms_max_accumulated_mask_size = n.max_accumulated_mask_size;
            slot->dtype = HAL_DTYPE_FLOAT32;
            slot->ndim = 1;
            slot->shape[0] = static_cast<int32_t>(slot->byte_size);
            return;
        }
    }

    const hailo_3d_image_shape_t shp = st.shape();
    if (fmt.order == HAILO_FORMAT_ORDER_NC)
    {
        slot->ndim = 2;
        slot->shape[0] = 1;
        slot->shape[1] = static_cast<int32_t>(shp.features);
    }
    else
    {
        slot->ndim = 4;
        slot->shape[0] = 1;
        slot->shape[1] = static_cast<int32_t>(shp.height);
        slot->shape[2] = static_cast<int32_t>(shp.width);
        slot->shape[3] = static_cast<int32_t>(shp.features);
    }
}

/**
 * Override spatial dims from HEF get_*_stream_infos() (matched by stream name).
 *
 * HailoRT documents that the **host buffer** layout for read/write uses ::hailo_stream_info_t::hw_shape, not
 * ::shape (logical / padded device layout). InferStream::shape() can therefore disagree with the NV12 blob
 * the host must supply — prefer hw_shape here and always apply for inputs after InferStream fill.
 *
 * If HAILO_FORMAT_FLAGS_TRANSPOSED is set, height/width are swapped vs the buffer the user passes.
 */
static void enrich_tensor_image_size_from_hef_stream_info(HalModelTensorInfo *slot, const hailort::Hef &hef,
                                                         bool is_input)
{
    hailort::Expected<std::vector<hailo_stream_info_t>> exp =
        is_input ? hef.get_input_stream_infos("") : hef.get_output_stream_infos("");
    if (!exp.has_value())
        return;
    auto suffix = [](const char *s) -> const char * {
        if (!s)
            return "";
        const char *last = std::strrchr(s, '/');
        return last ? (last + 1) : s;
    };
    for (const hailo_stream_info_t &si : exp.value())
    {
        // Some HailoRT APIs return stream names with/without the network prefix.
        // Match either full name or the basename suffix after the last '/'.
        if (std::strcmp(si.name, slot->name) != 0 && std::strcmp(suffix(si.name), suffix(slot->name)) != 0)
            continue;
        if (hailo_format_order_is_nms_shape_invalid(si.format.order))
            break;

        hailo_3d_image_shape_t shp{};
        bool have = false;
        if (si.hw_shape.height != 0U || si.hw_shape.width != 0U)
        {
            shp = si.hw_shape;
            have = true;
        }
        else if (si.shape.height != 0U || si.shape.width != 0U)
        {
            shp = si.shape;
            have = true;
        }
        if (!have)
            break;

        if ((si.format.flags & HAILO_FORMAT_FLAGS_TRANSPOSED) != 0)
            std::swap(shp.height, shp.width);

        if (slot->ndim >= 4)
        {
            slot->shape[1] = static_cast<int32_t>(shp.height);
            slot->shape[2] = static_cast<int32_t>(shp.width);
            slot->shape[3] = static_cast<int32_t>(shp.features);
        }

        // Best-effort: capture NV12 pixel format from HEF stream info when available.
        if (si.format.order == HAILO_FORMAT_ORDER_NV12 || si.format.order == HAILO_FORMAT_ORDER_NV21 || si.format.order == HAILO_FORMAT_ORDER_I420)
            slot->is_nv12 = 1;
        break;
    }
}

/**
 * NV12 host frame size = width * height * 3 / 2. Shapes from InferStream/HEF may disagree with byte_size.
 * Pixel count (W*H) is determined by byte_size; multiple factor pairs exist — pick (W,H) minimizing L1 distance
 * to shape[2]/shape[1] (NHWC spatial hint from the same metadata).
 */
static void reconcile_input_nv12_dims_from_byte_size(HalModelTensorInfo *slot)
{
    if (slot->byte_size == 0U)
        return;
    // Only attempt NV12 inference when it is either:
    // - explicitly marked NV12 by the backend, OR
    // - the byte_size does not match what the reported NHWC (RGB-like) shape would require.
    //
    // This prevents corrupting true RGB inputs (where byte_size == W*H*3 for UINT8 NHWC).
    bool byte_size_mismatches_reported_rgb = false;
    if (slot->ndim >= 4 && slot->dtype == HAL_DTYPE_UINT8)
    {
        const int32_t h = slot->shape[1];
        const int32_t w = slot->shape[2];
        const int32_t c = slot->shape[3];
        if (h > 0 && w > 0 && c == 3)
        {
            const uint64_t rgb_bytes = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 3ULL;
            byte_size_mismatches_reported_rgb = (static_cast<uint64_t>(slot->byte_size) != rgb_bytes);
        }
    }
    if (slot->is_nv12 == 0 && !byte_size_mismatches_reported_rgb)
        return;

    // Guard rails: only attempt NV12 inference on likely-NV12 tensors.
    // RGB packed inputs (C=3) can accidentally satisfy the NV12 size arithmetic, which would corrupt H/W.
    if (slot->ndim >= 4)
    {
        const int32_t h = slot->shape[1];
        const int32_t w = slot->shape[2];
        const int32_t c = slot->shape[3];
        if (h > 0 && w > 0)
        {
            const uint64_t bs = static_cast<uint64_t>(slot->byte_size);
            if (c == 3)
            {
                const uint64_t rgb_bytes = static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 3ULL;
                // When the backend marked this input as NV12, allow the common "NV12 encoded as H/2 x W x 3"
                // collision with RGB. Reconcile will infer the effective H from byte_size.
                (void)rgb_bytes;
            }
            // If channels are not 1, it's very unlikely to be NV12.
            if (c != 1 && c != 3)
                return;
        }
    }

    const uint64_t bs = static_cast<uint64_t>(slot->byte_size);
    if ((bs * 2ULL) % 3ULL != 0ULL)
        return;
    const uint64_t area = (bs * 2ULL) / 3ULL;
    if (area > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()) || area == 0ULL)
        return;

    uint32_t iw = 0;
    uint32_t ih = 0;
    if (slot->ndim >= 4 && slot->shape[1] > 0 && slot->shape[2] > 0)
    {
        ih = static_cast<uint32_t>(slot->shape[1]);
        iw = static_cast<uint32_t>(slot->shape[2]);
    }

    auto nv12_ok = [&](uint32_t ww, uint32_t hh) -> bool {
        if (ww == 0U || hh == 0U)
            return false;
        return static_cast<uint64_t>(ww) * static_cast<uint64_t>(hh) * 3ULL / 2ULL == bs;
    };

    if (nv12_ok(iw, ih))
        return;
    if (nv12_ok(ih, iw))
    {
        if (slot->ndim >= 4)
            std::swap(slot->shape[1], slot->shape[2]);
        return;
    }

    uint32_t best_w = 0;
    uint32_t best_h = 0;
    uint64_t best_score = UINT64_MAX;
    const uint64_t lim = static_cast<uint64_t>(std::sqrt(static_cast<long double>(area))) + 2ULL;
    for (uint64_t a = 1; a <= lim && a <= area; ++a)
    {
        if (area % a != 0ULL)
            continue;
        const uint64_t b = area / a;
        if (b > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()))
            continue;
        const uint32_t ww = static_cast<uint32_t>(a);
        const uint32_t hh = static_cast<uint32_t>(b);
        if (!nv12_ok(ww, hh))
            continue;
        const uint64_t s1 = (iw > 0U && ih > 0U)
                                ? ((ww > iw ? ww - iw : iw - ww) + (hh > ih ? hh - ih : ih - hh))
                                : (static_cast<uint64_t>(ww) + static_cast<uint64_t>(hh));
        const uint64_t s2 = (iw > 0U && ih > 0U)
                                ? ((hh > iw ? hh - iw : iw - hh) + (ww > ih ? ww - ih : ih - ww))
                                : (static_cast<uint64_t>(hh) + static_cast<uint64_t>(ww));
        const uint64_t sc = std::min(s1, s2);
        if (sc < best_score)
        {
            best_score = sc;
            if (s1 <= s2)
            {
                best_w = ww;
                best_h = hh;
            }
            else
            {
                best_w = hh;
                best_h = ww;
            }
        }
    }
    if (best_w == 0U || best_h == 0U)
        return;
    /* If format did not mark NV12, only accept the common "H/2 x W x 3" legacy fixup:
     * inferred height is 2x the reported height and width matches. */
    if (slot->is_nv12 == 0 && slot->ndim >= 4)
    {
        const uint32_t ih0 = static_cast<uint32_t>(slot->shape[1] > 0 ? slot->shape[1] : 0);
        const uint32_t iw0 = static_cast<uint32_t>(slot->shape[2] > 0 ? slot->shape[2] : 0);
        if (!(ih0 > 0U && iw0 > 0U && best_w == iw0 && best_h == ih0 * 2U))
            return;
    }
    if (slot->ndim >= 4)
    {
        slot->shape[1] = static_cast<int32_t>(best_h);
        slot->shape[2] = static_cast<int32_t>(best_w);
    }
}
#endif

} // namespace

extern "C" {

// Apply on-chip NMS parameters from HalInferenceConfig to every NMS-format
// output stream of the model. HailoRT applies these at configure() time, so
// this must run before infer_model->configure(). Zero/negative fields keep
// HailoRT defaults (callers get the old behavior with a zeroed config).
static void apply_nms_params(hailort::InferModel &model, const HalInferenceConfig &cfg)
{
    if (cfg.nms.score_threshold <= 0.0f && cfg.nms.iou_threshold <= 0.0f &&
        cfg.nms.max_proposals_per_class == 0U && cfg.nms.max_proposals_total == 0U)
    {
        bool any_mask = false;
        for (uint32_t m : cfg.nms.class_filter_mask)
        {
            if (m != 0U)
            {
                any_mask = true;
                break;
            }
        }
        if (!any_mask)
        {
            return; /* fully default — nothing to do */
        }
    }
    for (const auto &name : model.get_output_names())
    {
        auto out_exp = model.output(name);
        if (!out_exp)
        {
            continue;
        }
        // Non-NMS streams reject get_nms_shape() with INVALID_OPERATION — skip them.
        auto shape_exp = out_exp.value().get_nms_shape();
        if (!shape_exp)
        {
            continue;
        }
        if (cfg.nms.score_threshold > 0.0f)
        {
            out_exp.value().set_nms_score_threshold(cfg.nms.score_threshold);
        }
        if (cfg.nms.iou_threshold > 0.0f)
        {
            out_exp.value().set_nms_iou_threshold(cfg.nms.iou_threshold);
        }
        if (cfg.nms.max_proposals_per_class > 0U)
        {
            out_exp.value().set_nms_max_proposals_per_class(cfg.nms.max_proposals_per_class);
        }
        if (cfg.nms.max_proposals_total > 0U)
        {
            out_exp.value().set_nms_max_proposals_total(cfg.nms.max_proposals_total);
        }
        std::vector<bool> mask;
        bool any = false;
        for (uint32_t word : cfg.nms.class_filter_mask)
        {
            for (uint32_t bit = 0; bit < 32U && mask.size() < 256U; ++bit)
            {
                const bool on = ((word >> bit) & 1U) != 0U;
                any = any || on;
                mask.push_back(on);
            }
        }
        if (any)
        {
            out_exp.value().set_nms_classes_filter_mask(mask);
        }
    }
}
static HalInferenceSession *hailo15_infer_create(const HalInferenceConfig *config)
{
    if (!config || config->model_path[0] == '\0')
        return nullptr;

#if !defined(HAL_HAVE_HAILORT)
    (void)config;
    HAL_LOG_WARNING("hailo15_inference: built without HailoRT; returning NULL");
    return nullptr;
#else
    auto *p = new (std::nothrow) Hailo15InferPriv();
    if (!p)
        return nullptr;
    p->cfg = *config;
    if (p->cfg.preprocess.resize == HAL_PREPROCESS_RESIZE_NEAREST ||
        p->cfg.preprocess.resize == HAL_PREPROCESS_RESIZE_BILINEAR)
    {
        // ok
    }
    else
    {
        preprocess_defaults(&p->cfg.preprocess);
    }

    // device id: from platform_config JSON or default.
    const char *pcfg = config->platform_config;
    std::string device_id = "device0";
    if (pcfg && str_has_json_object_prefix(pcfg))
    {
        device_id = extract_json_string_value_best_effort(pcfg, "device_id", "device0");
    }
    // device_id + hef_basename are derived once and (re)applied to `p` in both the
    // primary path here and the no-latency-flag retry branch below, which deletes
    // and rebuilds `p` — without re-applying them the retry would leave both empty.
    p->device_id = device_id;

    // Extract HEF basename (without directory and .hef extension) for tensor naming
    std::string hef_basename;
    {
        const char *mp = config->model_path;
        const char *slash = std::strrchr(mp, '/');
        hef_basename = slash ? (slash + 1) : mp;
        const auto dot = hef_basename.rfind('.');
        if (dot != std::string::npos && hef_basename.substr(dot) == ".hef")
            hef_basename.resize(dot);
    }
    p->hef_basename = hef_basename;

    // Acquire shared VDevice — enables HailoRT ROUND_ROBIN scheduling across
    // all models so the NPU can pipeline inference for multiple network groups.
    p->vdevice = get_shared_vdevice();
    if (!p->vdevice)
    {
        HAL_LOG_ERROR("hailo15_inference: failed to acquire shared VDevice");
        delete p;
        return nullptr;
    }

    auto infer_model_exp = p->vdevice->create_infer_model(config->model_path);
    if (!infer_model_exp)
    {
        const hailo_status im_st = infer_model_exp.status();
        // Self-heal: if the cached singleton's connection to hailort_server is
        // gone (server crashed / restarted / OOM-killed), the cached handle is
        // permanently unusable and get_shared_vdevice() would keep returning
        // it. Drop it and rebuild the model against a fresh VDevice once, so a
        // model load transparently recovers instead of needing an ai-runtime
        // restart.
        if (hailo15_vdevice_connection_lost(im_st))
        {
            HAL_LOG_WARNING("hailo15_inference: create_infer_model lost VDevice "
                            "connection (status=%d) — resetting singleton and retrying once",
                            (int)im_st);
            reset_shared_vdevice();
            p->vdevice = get_shared_vdevice();
            if (p->vdevice)
            {
                // hailort::Expected<T> is move-constructible but NOT move-
                // assignable, so the retry uses a fresh local and releases it
                // straight into infer_model instead of reassigning the original.
                auto infer_model_retry = p->vdevice->create_infer_model(config->model_path);
                if (!infer_model_retry)
                {
                    HAL_LOG_ERROR("hailo15_inference: create_infer_model failed after "
                                  "VDevice reset (status=%d)",
                                  (int)infer_model_retry.status());
                    delete p;
                    return nullptr;
                }
                p->infer_model = infer_model_retry.release();
            }
            else
            {
                HAL_LOG_ERROR("hailo15_inference: failed to reacquire shared VDevice after reset");
                delete p;
                return nullptr;
            }
        }
        else
        {
            HAL_LOG_ERROR("hailo15_inference: create_infer_model failed (status=%d)", (int)im_st);
            delete p;
            return nullptr;
        }
    }
    // First attempt succeeded — adopt it. (On a self-heal retry the model was
    // already released into p->infer_model above, so this is skipped.)
    if (!p->infer_model)
        p->infer_model = infer_model_exp.release();
    if (config->batch_size > 0)
        p->infer_model->set_batch_size(config->batch_size);

    // Enable HW latency measurement so get_hw_latency_measurement() works.
    // Some models (e.g., older HEFs compiled with prior HailoRT versions) do not
    // support this flag and will fail configure() with HAILO_INVALID_OPERATION.
    // Fall back to configuring without the flag when that happens.
    p->infer_model->set_hw_latency_measurement_flags(HAILO_LATENCY_MEASURE);

    apply_nms_params(*p->infer_model, *config);

    // Default: keep model formats; tensor_from_frame() will align.
    auto configured_exp = p->infer_model->configure();
    if (!configured_exp)
    {
        if (configured_exp.status() == HAILO_INVALID_OPERATION)
        {
            // Latency flag not supported by this HEF — rebuild the model without it.
            HAL_LOG_WARNING("hailo15_inference: configure failed with HAILO_INVALID_OPERATION "
                            "(status=%d), retrying without latency measurement flag",
                            (int)configured_exp.status());

            // Drop the priv that already holds the broken infer_model and start fresh.
            delete p;

            p = new (std::nothrow) Hailo15InferPriv();
            if (!p)
            {
                HAL_LOG_ERROR("hailo15_inference: OOM retrying without latency flag");
                return nullptr;
            }
            p->cfg = *config;
            // Re-apply the fields the primary path set before configure(): the
            // rebuild above replaced `p`, so without these device_id/hef_basename
            // would be empty and NMS/postprocess tensor-name mapping would break.
            p->device_id = device_id;
            p->hef_basename = hef_basename;

            p->vdevice = get_shared_vdevice();
            if (!p->vdevice)
            {
                HAL_LOG_ERROR("hailo15_inference: failed to acquire shared VDevice on retry");
                delete p;
                return nullptr;
            }

            auto infer_model_exp2 = p->vdevice->create_infer_model(config->model_path);
            if (!infer_model_exp2)
            {
                HAL_LOG_ERROR("hailo15_inference: create_infer_model failed on retry (status=%d)",
                              (int)infer_model_exp2.status());
                delete p;
                return nullptr;
            }
            p->infer_model = infer_model_exp2.release();
            if (config->batch_size > 0)
                p->infer_model->set_batch_size(config->batch_size);
            apply_nms_params(*p->infer_model, *config);

            // Do NOT set latency measurement flag this time.
            auto configured_exp2 = p->infer_model->configure();
            if (!configured_exp2)
            {
                HAL_LOG_ERROR("hailo15_inference: configure failed on retry (status=%d)",
                              (int)configured_exp2.status());
                delete p;
                return nullptr;
            }
            // Latency measurement is unavailable for this model — get_hw_latency()
            // and get_hw_latency_measurement() will return 0.
            p->configured = configured_exp2.release();
        }
        else
        {
            HAL_LOG_ERROR("hailo15_inference: configure failed (status=%d)",
                          (int)configured_exp.status());
            delete p;
            return nullptr;
        }
    }
    else
    {
        // Success path: the original configure() succeeded — assign the result.
        p->configured = configured_exp.release();
    }

    auto bindings_exp = p->configured.create_bindings();
    if (!bindings_exp)
    {
        HAL_LOG_ERROR("hailo15_inference: create_bindings failed (status=%d)", (int)bindings_exp.status());
        delete p;
        return nullptr;
    }
    p->bindings = bindings_exp.release();

    for (const auto &n : p->infer_model->get_input_names())
        p->input_names.push_back(n);
    for (const auto &n : p->infer_model->get_output_names())
        p->output_names.push_back(n);

    return reinterpret_cast<HalInferenceSession *>(p);
#endif
}

static void hailo15_infer_destroy(HalInferenceSession *session)
{
    if (!session)
        return;
#if !defined(HAL_HAVE_HAILORT)
    (void)session;
#else
    auto *p = reinterpret_cast<Hailo15InferPriv *>(session);
    // Drain in-flight async jobs first: their completion callbacks capture p,
    // so freeing the session underneath a pending job would use-after-free.
    hailo15_wait_pending_async(p);
    delete p;
#endif
}

static int hailo15_infer_get_model_info(HalInferenceSession *session, HalModelInfo *info)
{
    if (!session || !info)
        return HAL_ERR_INVALID_ARG;
    std::memset(info, 0, sizeof(*info));

#if !defined(HAL_HAVE_HAILORT)
    (void)session;
    return HAL_ERR_NOT_SUPPORTED;
#else
    auto *p = reinterpret_cast<Hailo15InferPriv *>(session);
    /* model_path may be longer than info->name — bounded copy (avoid strncpy truncation warnings). */
    {
        const size_t cap = sizeof(info->name);
        const size_t plen = std::strlen(p->cfg.model_path);
        const size_t copy_len = plen < cap - 1U ? plen : cap - 1U;
        std::memcpy(info->name, p->cfg.model_path, copy_len);
        info->name[copy_len] = '\0';
    }
    std::snprintf(info->version, sizeof(info->version), "%s", "hailort");

    /* HEF-level identifiers (first network / first group). */
    {
        const hailort::Hef &hef = p->infer_model->hef();
        auto nets_exp = hef.get_network_infos();
        if (nets_exp.has_value() && !nets_exp.value().empty())
            copy_bounded_cstr(info->network_name, sizeof(info->network_name), nets_exp.value()[0].name);
        const std::vector<std::string> groups = hef.get_network_groups_names();
        if (!groups.empty())
            copy_bounded_cstr(info->network_group_name, sizeof(info->network_group_name), groups[0].c_str());
    }

    info->num_inputs = static_cast<uint32_t>(p->input_names.size());
    info->num_outputs = static_cast<uint32_t>(p->output_names.size());
    if (info->num_inputs > HAL_MAX_TENSORS)
        info->num_inputs = HAL_MAX_TENSORS;
    if (info->num_outputs > HAL_MAX_TENSORS)
        info->num_outputs = HAL_MAX_TENSORS;

    for (uint32_t i = 0; i < info->num_inputs; i++)
    {
        const auto &n = p->input_names[i];
        copy_bounded_cstr(info->inputs[i].name, sizeof(info->inputs[i].name), n);
        auto exp = p->infer_model->input(n);
        if (!exp.has_value())
            continue;
        fill_hal_tensor_info_from_stream(&info->inputs[i], exp.value(), false);
    }
    for (uint32_t i = 0; i < info->num_outputs; i++)
    {
        const auto &n = p->output_names[i];
        copy_bounded_cstr(info->outputs[i].name, sizeof(info->outputs[i].name), n);
        auto exp = p->infer_model->output(n);
        if (!exp.has_value())
            continue;
        fill_hal_tensor_info_from_stream(&info->outputs[i], exp.value(), true);
    }

    /* Host-side H/W: HEF hw_shape (and transpose) override InferStream::shape() — required for correct NV12 dims. */
    {
        const hailort::Hef &hef = p->infer_model->hef();
        for (uint32_t i = 0; i < info->num_inputs; i++)
        {
            if (info->inputs[i].name[0] == '\0')
                continue;
            enrich_tensor_image_size_from_hef_stream_info(&info->inputs[i], hef, true);
        }
        for (uint32_t i = 0; i < info->num_outputs; i++)
        {
            if (info->outputs[i].name[0] == '\0')
                continue;
            if (info->outputs[i].is_nms != 0)
                continue;
            enrich_tensor_image_size_from_hef_stream_info(&info->outputs[i], hef, false);
        }
    }

    for (uint32_t i = 0; i < info->num_inputs; i++)
        reconcile_input_nv12_dims_from_byte_size(&info->inputs[i]);

    return HAL_OK;
#endif
}

static int hailo15_infer_alloc_input(HalInferenceSession *session, int input_idx, HalTensor *tensor)
{
    if (!session || !tensor || input_idx < 0)
        return HAL_ERR_INVALID_ARG;
    std::memset(tensor, 0, sizeof(*tensor));
    tensor->dma_fd = -1;

#if !defined(HAL_HAVE_HAILORT)
    (void)session;
    (void)input_idx;
    return HAL_ERR_NOT_SUPPORTED;
#else
    auto *p = reinterpret_cast<Hailo15InferPriv *>(session);
    if (static_cast<size_t>(input_idx) >= p->input_names.size())
        return HAL_ERR_NOT_FOUND;
    const auto &name = p->input_names[input_idx];

    const size_t frame_size = p->infer_model->input(name)->get_frame_size();
    auto buf = alloc_aligned_shared(frame_size, 4096);
    if (!buf)
        return HAL_ERR_NO_MEM;

    std::snprintf(tensor->name, sizeof(tensor->name), "%s", name.c_str());
    tensor->data = buf.get();
    tensor->ndim = 1;
    tensor->shape[0] = static_cast<int32_t>(frame_size);
    tensor->dtype = HAL_DTYPE_UINT8;
    tensor->byte_size = static_cast<uint32_t>(frame_size);
    tensor->priv = new (std::nothrow) TensorPriv{buf
#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
                                                 ,
                                                 nullptr
#endif
    };
    if (!tensor->priv)
        return HAL_ERR_NO_MEM;
    return HAL_OK;
#endif
}

static int hailo15_infer_tensor_from_frame(const HalFrameBuffer *frame, HalTensor *tensor)
{
    if (!frame || !tensor)
        return HAL_ERR_INVALID_ARG;

    uint32_t plane0_sz = 0;
    uint32_t plane1_sz = 0;
    switch (frame->format)
    {
    case HAL_PIX_FMT_NV12:
        if (frame->num_planes < 2 || !frame->planes[0] || !frame->planes[1])
            return HAL_ERR_NOT_SUPPORTED;
        plane0_sz = frame->sizes[0];
        plane1_sz = frame->sizes[1];
        break;
    case HAL_PIX_FMT_RGB24:
    case HAL_PIX_FMT_BGR24:
    case HAL_PIX_FMT_GRAY8:
        if (frame->num_planes < 1 || !frame->planes[0])
            return HAL_ERR_NOT_SUPPORTED;
        plane0_sz = frame->sizes[0];
        break;
    default:
        return HAL_ERR_NOT_SUPPORTED;
    }

    const uint32_t total = plane0_sz + plane1_sz;
    if (total == 0)
        return HAL_ERR_INVALID_ARG;

    std::memset(tensor, 0, sizeof(*tensor));
    tensor->dma_fd = -1;
    auto buf = alloc_aligned_shared(total, 4096);
    if (!buf)
        return HAL_ERR_NO_MEM;
    std::memcpy(buf.get(), frame->planes[0], plane0_sz);
    if (plane1_sz > 0)
        std::memcpy(static_cast<uint8_t *>(buf.get()) + plane0_sz, frame->planes[1], plane1_sz);

    tensor->data = buf.get();
    tensor->ndim = 1;
    tensor->shape[0] = static_cast<int32_t>(total);
    tensor->dtype = HAL_DTYPE_UINT8;
    tensor->byte_size = total;
    tensor->priv = new (std::nothrow) TensorPriv{buf
#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
                                                 ,
                                                 nullptr
#endif
    };
    if (!tensor->priv)
        return HAL_ERR_NO_MEM;
    return HAL_OK;
}

/* Session-aware tensor_from_frame: applies HalInferenceConfig::preprocess
 * (resize / color / letterbox / normalize) to match the model's first input
 * stream, using HailoRT's host-side InputTransformContext.
 *
 * Fast path: frame geometry + format already match the model input and
 * normalize is off -> verbatim copy (same as the legacy tensor_from_frame).
 *
 * Letterbox (KEEP_ASPECT) runs the transform at the aspect-preserving content
 * size, then composes the result onto the padded destination canvas.
 */
static int hailo15_infer_tensor_from_frame_ex(HalInferenceSession *session,
                                              const HalFrameBuffer *frame, HalTensor *tensor)
{
#if !defined(HAL_HAVE_HAILORT)
    (void)session; (void)frame; (void)tensor;
    return HAL_ERR_NOT_SUPPORTED;
#else
    auto *p = reinterpret_cast<Hailo15InferPriv *>(session);
    if (!p || p->input_names.empty() || !frame || !tensor)
    {
        return HAL_ERR_INVALID_ARG;
    }
    const std::string &name = p->input_names[0];
    auto in_exp = p->infer_model->input(name);
    if (!in_exp.has_value())
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    auto &st = in_exp.value();
    const hailo_3d_image_shape_t dsh = st.shape();
    const hailo_format_t dfm = st.format();
    if (dsh.width == 0 || dsh.height == 0)
    {
        return HAL_ERR_INVALID_STATE;
    }

    /* Source validation (geometry mapping happens in the staging step). */
    switch (frame->format)
    {
    case HAL_PIX_FMT_NV12:
        if (frame->num_planes < 2 || !frame->planes[0] || !frame->planes[1])
        {
            return HAL_ERR_INVALID_ARG;
        }
        break;
    case HAL_PIX_FMT_RGB24:
    case HAL_PIX_FMT_BGR24:
        if (frame->num_planes < 1 || !frame->planes[0])
        {
            return HAL_ERR_INVALID_ARG;
        }
        break;
    default:
        HAL_LOG_ERROR("hailo15_inference: tensor_from_frame_ex: unsupported source format %d",
                      (int)frame->format);
        return HAL_ERR_NOT_SUPPORTED;
    }

    /* ---- Fast path: exact match -> verbatim copy ---- */
    const bool normalize = p->cfg.preprocess.normalize;
    if (frame->width == dsh.width && frame->height == dsh.height && !normalize &&
        ((frame->format == HAL_PIX_FMT_NV12 && dfm.order == HAILO_FORMAT_ORDER_NV12) ||
         (frame->format == HAL_PIX_FMT_RGB24 && dfm.order == HAILO_FORMAT_ORDER_RGB888)))
    {
        return hailo15_infer_tensor_from_frame(frame, tensor);
    }

    /* ---- Target channel order ----
     * An explicit preprocess.color request wins; otherwise stage RGB: this
     * HailoRT stack exposes packed model inputs as RGB888/NHWC — there is no
     * BGR order, so BGR output only happens on an explicit user request. */
    const bool out_bgr = (p->cfg.preprocess.color == HAL_PREPROCESS_COLOR_NV12_TO_BGR ||
                          p->cfg.preprocess.color == HAL_PREPROCESS_COLOR_RGB_TO_BGR);
    /* Packed 3-channel interleaved model inputs only: NV12-order inputs are
     * served by the exact-match fast path above and cannot be resized on the
     * CPU staging path; planar (NHCW) or non-3-channel inputs are rejected. */
    const bool packed_rgb_ok =
        (dfm.order == HAILO_FORMAT_ORDER_RGB888 || dfm.order == HAILO_FORMAT_ORDER_NHWC) &&
        dsh.features == 3;
    if (!packed_rgb_ok)
    {
        HAL_LOG_ERROR("hailo15_inference: tensor_from_frame_ex: model input order %d "
                      "(features %u) only supported on the exact-match fast path",
                      (int)dfm.order, (unsigned)dsh.features);
        return HAL_ERR_NOT_SUPPORTED;
    }

    /* ---- General path: stage the source as packed RGB888/BGR888 ----
     * NV12 cannot be expressed by a packed 3-channel staging buffer, so
     * convert it here on the CPU (BT.601 limited range) and resize on the
     * CPU (bilinear). Production NV12-input models take the fast path above
     * and skip this. Plane strides are honored: media/DMA buffers commonly
     * carry row padding. */
    const uint32_t w = frame->width, h = frame->height;
    const size_t rgb_sz = (size_t)w * h * 3;
    auto src_buf = alloc_aligned_shared(rgb_sz, 4096);
    if (!src_buf)
    {
        return HAL_ERR_NO_MEM;
    }
    uint8_t *s = static_cast<uint8_t *>(src_buf.get());
    const uint8_t *yp = static_cast<const uint8_t *>(frame->planes[0]);
    if (frame->format == HAL_PIX_FMT_NV12)
    {
        const uint8_t *uvp = static_cast<const uint8_t *>(frame->planes[1]);
        const uint32_t ys = frame->strides[0] ? frame->strides[0] : w;
        const uint32_t uvs = frame->strides[1] ? frame->strides[1] : w;
        for (uint32_t j = 0; j < h; ++j)
        {
            const size_t yrow = (size_t)j * ys;
            const size_t uvrow = (size_t)(j / 2) * uvs;
            for (uint32_t i = 0; i < w; ++i)
            {
                const int Y = yp[yrow + i];
                const int U = (int)uvp[uvrow + (i & ~1u)] - 128;
                const int V = (int)uvp[uvrow + (i & ~1u) + 1] - 128;
                /* BT.601 limited range; the +128 rounding bias belongs inside
                 * the division (adding it afterwards offsets every channel by
                 * half-range: black would decode to 128, not 0). */
                int R = ((Y - 16) * 298 + 409 * V + 128) / 256;
                int G = ((Y - 16) * 298 - 100 * U - 208 * V + 128) / 256;
                int B = ((Y - 16) * 298 + 516 * U + 128) / 256;
                R = R < 0 ? 0 : (R > 255 ? 255 : R);
                G = G < 0 ? 0 : (G > 255 ? 255 : G);
                B = B < 0 ? 0 : (B > 255 ? 255 : B);
                const size_t o = ((size_t)j * w + i) * 3;
                if (out_bgr)
                {
                    s[o + 0] = (uint8_t)B;
                    s[o + 1] = (uint8_t)G;
                    s[o + 2] = (uint8_t)R;
                }
                else
                {
                    s[o + 0] = (uint8_t)R;
                    s[o + 1] = (uint8_t)G;
                    s[o + 2] = (uint8_t)B;
                }
            }
        }
    }
    else if (frame->format == HAL_PIX_FMT_BGR24 || frame->format == HAL_PIX_FMT_RGB24)
    {
        const uint8_t *q = static_cast<const uint8_t *>(frame->planes[0]);
        const uint32_t srs = frame->strides[0] ? frame->strides[0] : w * 3;
        const bool swap = ((frame->format == HAL_PIX_FMT_BGR24) != out_bgr);
        for (uint32_t j = 0; j < h; ++j)
        {
            const uint8_t *src_row = q + (size_t)j * srs;
            uint8_t *dst_row = s + (size_t)j * w * 3;
            if (swap)
            {
                for (uint32_t i = 0; i < w; ++i)
                {
                    dst_row[(size_t)i * 3 + 0] = src_row[(size_t)i * 3 + 2];
                    dst_row[(size_t)i * 3 + 1] = src_row[(size_t)i * 3 + 1];
                    dst_row[(size_t)i * 3 + 2] = src_row[(size_t)i * 3 + 0];
                }
            }
            else
            {
                std::memcpy(dst_row, src_row, (size_t)w * 3);
            }
        }
    }
    else
    {
        std::memcpy(s, frame->planes[0], rgb_sz);
    }

    /* ---- Target content geometry (letterbox keeps the source aspect) ---- */
    const uint32_t outW = dsh.width, outH = dsh.height;
    uint32_t contW = outW, contH = outH;
    if (p->cfg.preprocess.letterbox == HAL_PREPROCESS_LETTERBOX_KEEP_ASPECT)
    {
        const float scale = std::min((float)outW / (float)frame->width, (float)outH / (float)frame->height);
        contW = (uint32_t)((float)frame->width * scale) & ~1u;
        contH = (uint32_t)((float)frame->height * scale) & ~1u;
        if (contW < 2) contW = 2;
        if (contH < 2) contH = 2;
        if (contW > outW) contW = outW;
        if (contH > outH) contH = outH;
    }

    /* ---- CPU resize (bilinear) + letterbox canvas ----
     * InputTransformContext proved unreliable for arbitrary rescales on this
     * stack (identity-size color conversion works, true resizes fail with
     * HAILO_INVALID_OPERATION), so the general path resizes on the CPU.
     * Correctness-first: production NV12-input models take the zero-copy fast
     * path above and never land here. */
    auto dst_buf = alloc_aligned_shared((size_t)outW * outH * 3, 4096);
    if (!dst_buf)
    {
        return HAL_ERR_NO_MEM;
    }
    uint8_t *final_data = static_cast<uint8_t *>(dst_buf.get());
    size_t final_sz = (size_t)outW * outH * 3;
    std::shared_ptr<void> canvas;
    {
        const uint32_t sw = frame->width, sh = frame->height;
        const float fx = (float)contW / (float)sw;
        const float fy = (float)contH / (float)sh;
        const uint8_t pv = p->cfg.preprocess.pad_value;
        const uint32_t off_x = ((outW - contW) / 2) & ~1u;
        const uint32_t off_y = ((outH - contH) / 2) & ~1u;
        std::memset(final_data, pv, final_sz);
        for (uint32_t y = 0; y < contH; ++y)
        {
            const float sy = (y + 0.5f) / fy - 0.5f;
            int y0 = (int)sy; if (y0 < 0) y0 = 0; if (y0 > (int)sh - 1) y0 = (int)sh - 1;
            int y1 = y0 + 1; if (y1 > (int)sh - 1) y1 = (int)sh - 1;
            float wy = sy - y0; if (wy < 0) wy = 0; if (wy > 1) wy = 1;
            uint8_t *drow = final_data + ((size_t)(off_y + y) * outW + off_x) * 3;
            const uint8_t *r0 = s + (size_t)y0 * sw * 3;
            const uint8_t *r1 = s + (size_t)y1 * sw * 3;
            for (uint32_t x = 0; x < contW; ++x)
            {
                const float sx = (x + 0.5f) / fx - 0.5f;
                int x0 = (int)sx; if (x0 < 0) x0 = 0; if (x0 > (int)sw - 1) x0 = (int)sw - 1;
                int x1 = x0 + 1; if (x1 > (int)sw - 1) x1 = (int)sw - 1;
                float wx = sx - x0; if (wx < 0) wx = 0; if (wx > 1) wx = 1;
                for (uint32_t c = 0; c < 3; ++c)
                {
                    const float v = (1 - wy) * ((1 - wx) * r0[(size_t)x0 * 3 + c] + wx * r0[(size_t)x1 * 3 + c]) +
                                    wy * ((1 - wx) * r1[(size_t)x0 * 3 + c] + wx * r1[(size_t)x1 * 3 + c]);
                    drow[(size_t)x * 3 + c] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v));
                }
            }
        }
    }

    /* ---- Normalize (uint8 -> float32 with per-channel mean/std) ---- */
    HalDataType odtype = HAL_DTYPE_UINT8;
    if (normalize && dfm.type == HAILO_FORMAT_TYPE_FLOAT32)
    {
        const size_t n = final_sz;
        auto fbuf = alloc_aligned_shared(n * sizeof(float), 4096);
        if (!fbuf)
        {
            return HAL_ERR_NO_MEM;
        }
        float *fo = static_cast<float *>(fbuf.get());
        for (size_t i = 0; i < n; ++i)
        {
            const uint32_t c = (uint32_t)(i % 3);
            const float mean = p->cfg.preprocess.mean[c];
            const float sd = p->cfg.preprocess.std[c] != 0.0f ? p->cfg.preprocess.std[c] : 1.0f;
            fo[i] = (((float)final_data[i] / 255.0f) - mean) / sd;
        }
        canvas = fbuf;
        final_data = static_cast<uint8_t *>(fbuf.get());
        final_sz = n * sizeof(float);
        odtype = HAL_DTYPE_FLOAT32;
    }
    else if (normalize)
    {
        HAL_LOG_WARNING("hailo15_inference: preprocess.normalize requested but model input is not FLOAT32 - ignored");
    }

    std::memset(tensor, 0, sizeof(*tensor));
    tensor->data = final_data;
    tensor->dma_fd = -1;
    tensor->ndim = 1;
    /* shape counts elements; byte_size counts bytes (uint8: identical). */
    const size_t elem_size = (odtype == HAL_DTYPE_FLOAT32) ? sizeof(float) : 1;
    tensor->shape[0] = (int32_t)(final_sz / elem_size);
    tensor->dtype = odtype;
    tensor->byte_size = (uint32_t)final_sz;
    std::snprintf(tensor->name, sizeof(tensor->name), "%s", name.c_str());
    tensor->priv = new (std::nothrow) TensorPriv{canvas ? canvas : dst_buf
#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
                                                   ,
                                                   nullptr
#endif
    };
    if (!tensor->priv)
    {
        return HAL_ERR_NO_MEM;
    }
    return HAL_OK;
#endif
}

static int hailo15_infer_run(HalInferenceSession *session,
                             const HalTensor *inputs, int num_inputs,
                             HalTensor *outputs, int num_outputs)
{
    if (!session || !inputs || num_inputs <= 0 || !outputs || num_outputs <= 0)
        return HAL_ERR_INVALID_ARG;

#if !defined(HAL_HAVE_HAILORT)
    (void)session;
    (void)inputs;
    (void)num_inputs;
    (void)outputs;
    (void)num_outputs;
    return HAL_ERR_NOT_SUPPORTED;
#else
    auto *p = reinterpret_cast<Hailo15InferPriv *>(session);
    const size_t want_in = p->input_names.size();
    const size_t want_out = p->output_names.size();
    if (static_cast<size_t>(num_inputs) < want_in || static_cast<size_t>(num_outputs) < want_out)
    {
        return HAL_ERR_INSUFFICIENT_BUFFER;
    }

    auto ready = p->configured.wait_for_async_ready(std::chrono::milliseconds(p->cfg.timeout_ms ? p->cfg.timeout_ms : 1000));
    if (HAILO_SUCCESS != ready)
    {
        if (hailo15_vdevice_connection_lost(ready))
            hailo15_notify_vdevice_lost(ready, "wait_for_async_ready");
        return HAL_ERR_TIMEOUT;
    }

    std::unique_ptr<hailort::AsyncInferJob> job;
    {
        // Bind + submit must be atomic per session: p->bindings is shared state.
        std::lock_guard<std::mutex> lk(p->submit_mtx);

        const int bind_rc = hailo15_bind_inputs_outputs(p, inputs, outputs);
        if (bind_rc != HAL_OK)
            return bind_rc;

        auto job_exp = p->configured.run_async(p->bindings, [](const hailort::AsyncInferCompletionInfo &) {});
        if (!job_exp)
        {
            HAL_LOG_ERROR("hailo15_inference: run_async failed (status=%d)", (int)job_exp.status());
            if (hailo15_vdevice_connection_lost(job_exp.status()))
                hailo15_notify_vdevice_lost(job_exp.status(), "run_async");
            return HAL_ERR_RESULT;
        }
        job = std::make_unique<hailort::AsyncInferJob>(job_exp.release());
    }
    hailo_status st = job->wait(std::chrono::milliseconds(p->cfg.timeout_ms ? p->cfg.timeout_ms : 10000));
    if (HAILO_SUCCESS != st)
    {
        HAL_LOG_ERROR("hailo15_inference: infer wait failed (st=%d)", (int)st);
        if (hailo15_vdevice_connection_lost(st))
            hailo15_notify_vdevice_lost(st, "infer wait");
        return HAL_ERR_TIMEOUT;
    }

    hailo15_record_inference(p);
    hailo15_attach_postprocess_roi(p, outputs, want_out);
    return HAL_OK;
#endif
}

/**
 * Submit one inference without blocking. The caller's output buffers must stay
 * valid until the callback fires (HailoRT snapshots the MemoryView pointers at
 * submission time, the NPU writes them asynchronously). The completion ctx is
 * shared between pending_async (so destroy() can drain it) and the HailoRT
 * callback, which keeps it alive until the NPU reports done. */
static int hailo15_infer_run_async(HalInferenceSession *session,
                                   const HalTensor *inputs, int num_inputs,
                                   HalTensor *outputs, int num_outputs,
                                   HalInferenceAsyncCallback callback, void *userdata)
{
    if (!session || !inputs || num_inputs <= 0 || !outputs || num_outputs <= 0 || !callback)
        return HAL_ERR_INVALID_ARG;

#if !defined(HAL_HAVE_HAILORT)
    (void)num_inputs;
    (void)num_outputs;
    (void)callback;
    (void)userdata;
    return HAL_ERR_NOT_SUPPORTED;
#else
    auto *p = reinterpret_cast<Hailo15InferPriv *>(session);
    const size_t want_in = p->input_names.size();
    const size_t want_out = p->output_names.size();
    if (static_cast<size_t>(num_inputs) < want_in || static_cast<size_t>(num_outputs) < want_out)
        return HAL_ERR_INSUFFICIENT_BUFFER;

    auto ready = p->configured.wait_for_async_ready(std::chrono::milliseconds(p->cfg.timeout_ms ? p->cfg.timeout_ms : 1000));
    if (HAILO_SUCCESS != ready)
    {
        if (hailo15_vdevice_connection_lost(ready))
            hailo15_notify_vdevice_lost(ready, "wait_for_async_ready");
        return HAL_ERR_TIMEOUT;
    }

    // Bind + submit must be atomic per session: p->bindings is shared state
    // rewritten by set_buffer() on every call. Without this lock, concurrent
    // workers serving the same session race on the output buffer pointers
    // HailoRT snapshots at submit time, aliasing buffers so one job's
    // completion frees an output buffer another job's NPU write is still in
    // flight (heap-use-after-free under K>=4 concurrency). The completion
    // callback (below) and destroy()'s drain only ever take async_mtx, never
    // submit_mtx, so nesting is deadlock-free.
    std::shared_ptr<Hailo15InferAsyncCtx> ctx;
    {
        std::lock_guard<std::mutex> lk(p->submit_mtx);

        const int bind_rc = hailo15_bind_inputs_outputs(p, inputs, outputs);
        if (bind_rc != HAL_OK)
            return bind_rc;

        // Snapshot the output tensors for the callback. ctx is held by both
        // pending_async (so destroy() can flush it) and the HailoRT completion
        // callback, keeping it — and the AsyncInferJob it owns — alive until
        // the NPU reports done.
        ctx = std::make_shared<Hailo15InferAsyncCtx>();
        ctx->outputs.assign(outputs, outputs + want_out);
        ctx->callback = callback;
        ctx->userdata = userdata;

        // Register ctx BEFORE submitting so a callback that fires before
        // run_async returns still finds (and erases) it. Without this, a
        // fast-completing job would strand ctx in pending_async and block
        // destroy()'s drain.
        {
            std::lock_guard<std::mutex> lock(p->async_mtx);
            p->pending_async.push_back(ctx);
        }

        auto job_exp = p->configured.run_async(
            p->bindings,
            [p, ctx, want_out](const hailort::AsyncInferCompletionInfo &info) {
                const int status = (info.status == HAILO_SUCCESS) ? HAL_OK : HAL_ERR_RESULT;
                if (status == HAL_OK)
                {
                    hailo15_record_inference(p);
                    hailo15_attach_postprocess_roi(p, ctx->outputs.data(), want_out);
                }
                {
                    std::lock_guard<std::mutex> lock(p->async_mtx);
                    auto &vec = p->pending_async;
                    vec.erase(std::remove(vec.begin(), vec.end(), ctx), vec.end());
                }
                if (ctx->callback)
                    ctx->callback(ctx->outputs.data(), static_cast<int>(want_out), status, ctx->userdata);
            });
        if (!job_exp)
        {
            HAL_LOG_ERROR("hailo15_inference: run_async failed (status=%d)", (int)job_exp.status());
            if (hailo15_vdevice_connection_lost(job_exp.status()))
                hailo15_notify_vdevice_lost(job_exp.status(), "run_async");
            // Roll back the pre-registration above: no job was created, so no
            // completion callback will ever fire to remove ctx.
            std::lock_guard<std::mutex> lock(p->async_mtx);
            p->pending_async.erase(std::remove(p->pending_async.begin(), p->pending_async.end(), ctx),
                                   p->pending_async.end());
            return HAL_ERR_RESULT;
        }
        ctx->job = std::make_unique<hailort::AsyncInferJob>(job_exp.release());
    }
    return HAL_OK;
#endif
}

/**
 * Acquire a handle to the shared NPU runtime. The HEAD design shares one
 * ROUND_ROBIN VDevice across every model session, so every acquired runtime
 * is backed by the same scheduler — the @p config is accepted for API
 * compatibility but the singleton's scheduling wins. */
static HalInferenceRuntime *hailo15_infer_runtime_acquire(const HalInferenceRuntimeConfig *config)
{
#if !defined(HAL_HAVE_HAILORT)
    (void)config;
    return nullptr;
#else
    (void)config;
    auto vdev = get_shared_vdevice();
    if (!vdev)
        return nullptr;
    auto *wrapper = new (std::nothrow) Hailo15RuntimeHandle();
    if (!wrapper)
        return nullptr;
    wrapper->vdevice = vdev;
    HAL_LOG_INFO("hailo15_inference: runtime_acquire group=aipc algorithm=ROUND_ROBIN");
    return reinterpret_cast<HalInferenceRuntime *>(wrapper);
#endif
}

/** Drop one reference to the shared runtime. The underlying VDevice is
 *  process-lifetime, so this only frees the wrapper handle. */
static void hailo15_infer_runtime_release(HalInferenceRuntime *runtime)
{
#if !defined(HAL_HAVE_HAILORT)
    (void)runtime;
#else
    if (!runtime)
        return;
    auto *wrapper = reinterpret_cast<Hailo15RuntimeHandle *>(runtime);
    wrapper->vdevice.reset();
    delete wrapper;
#endif
}

static void hailo15_infer_free_tensor(HalTensor *tensor)
{
    if (!tensor)
        return;
    if (tensor->priv)
    {
        auto *tp = static_cast<TensorPriv *>(tensor->priv);
        delete tp;
    }
    std::memset(tensor, 0, sizeof(*tensor));
    tensor->dma_fd = -1;
}

static const char *hailo15_infer_get_version(void)
{
#if defined(HAL_HAVE_HAILORT)
    return "Hailo15 HAL-INFERENCE (HailoRT)";
#else
    return "Hailo15 HAL-INFERENCE (stub)";
#endif
}

static int hailo15_infer_query_session_performance_stats(HalInferenceSession *session,
                                                          uint32_t sampling_period_ms,
                                                          HalInferenceSessionPerfStats *out)
{
    (void)sampling_period_ms;

    if (!session || !out) return HAL_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));

#if !defined(HAL_HAVE_HAILORT)
    return HAL_ERR_NOT_SUPPORTED;
#else
    auto *p = reinterpret_cast<Hailo15InferPriv *>(session);

    // Query HailoRT for pure NPU hardware latency (theoretical max fps).
    auto latency_exp = p->configured.get_hw_latency_measurement();
    if (latency_exp) {
        auto ns = latency_exp->avg_hw_latency.count();
        out->hw_latency_us = static_cast<uint64_t>(ns / 1000);
        if (out->hw_latency_us > 0) {
            out->fps = 1'000'000.0f / static_cast<float>(out->hw_latency_us);
        }
    }

    // Async bookkeeping: total completions + current queue depth. These let
    // GetStats report how many jobs are outstanding even when callers no
    // longer query stats per-inference.
    out->total_inferences = p->total_inferences.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(p->async_mtx);
        out->pending_async_jobs = static_cast<uint32_t>(p->pending_async.size());
    }
    auto q_exp = p->configured.get_async_queue_size();
    if (q_exp)
        out->async_queue_size = static_cast<uint32_t>(*q_exp);

    out->utilization = -1.0f; // reserved for future per-network NPU metrics

    // Fill network group name from the HEF basename
    copy_bounded_cstr(out->network_group_name, sizeof(out->network_group_name),
                      p->hef_basename.c_str());
    return HAL_OK;
#endif
}

static int hailo15_infer_query_system_performance_stats(const char *device_id, uint32_t sampling_period_ms,
                                                        HalInferencePerfStats *out)
{
    if (!out)
        return HAL_ERR_INVALID_ARG;

#if !defined(HAL_HAVE_HAILORT)
    (void)device_id;
    (void)sampling_period_ms;
    return HAL_ERR_NOT_SUPPORTED;
#else
    // Helper to fill output from a hailo_performance_stats_t
    auto fill_output = [](const hailo_performance_stats_t &s, HalInferencePerfStats *o) {
        o->cpu_utilization = s.cpu_utilization;
        o->npu_utilization = s.nnc_utilization;
        o->ram_total_kib   = s.ram_size_total;
        o->ram_used_kib    = s.ram_size_used;
        o->dsp_utilization = (s.dsp_utilization >= 0)
            ? static_cast<float>(s.dsp_utilization)
            : -1.0f;
    };

    const uint32_t period_ms = sampling_period_ms ? sampling_period_ms : 100U;
    const std::chrono::milliseconds period(period_ms);
    // HailoRT 5.3.0 IntegratedDevice returns real NNC counters even when
    // hailort_server is running in multi_process_service mode.
    auto dev_exp = (device_id && device_id[0])
        ? hailort::Device::create(std::string(device_id))
        : hailort::Device::create();
    if (dev_exp)
    {
        std::unique_ptr<hailort::Device> dev(dev_exp.release());
        auto stats_exp = dev->query_performance_stats(period);
        if (stats_exp) {
            fill_output(stats_exp.value(), out);
            /* On-die temperature: optional diagnostic — a read failure must not
             * fail the whole query (fields keep their "unknown" value). */
            out->soc_temp_c = -1.0f;
            out->soc_temp_c1 = -1.0f;
            auto temp_exp = dev->get_chip_temperature();
            if (temp_exp) {
                out->soc_temp_c = temp_exp.value().ts0_temperature;
                out->soc_temp_c1 = temp_exp.value().ts1_temperature;
            }
            return HAL_OK;
        }
    }

    HAL_LOG_ERROR("hailo15_inference: query_performance_stats failed (Device::create status=%d)",
                  (int)dev_exp.status());
    return HAL_ERR_RESULT;
#endif
}

HalInferenceOps HAL_INFERENCE_OPS = {
    .create = hailo15_infer_create,
    .destroy = hailo15_infer_destroy,
    .get_model_info = hailo15_infer_get_model_info,
    .alloc_input = hailo15_infer_alloc_input,
    .tensor_from_frame = hailo15_infer_tensor_from_frame,
    .run = hailo15_infer_run,
    .run_async = hailo15_infer_run_async,
    .runtime_acquire = hailo15_infer_runtime_acquire,
    .runtime_release = hailo15_infer_runtime_release,
    .query_session_performance_stats = hailo15_infer_query_session_performance_stats,
    .free_tensor = hailo15_infer_free_tensor,
    .query_system_performance_stats = hailo15_infer_query_system_performance_stats,
    .get_version = hailo15_infer_get_version,
    /* M3 additions (appended at the table tail, after get_version) */
    .tensor_from_frame_ex = hailo15_infer_tensor_from_frame_ex,
};

} // extern "C"
