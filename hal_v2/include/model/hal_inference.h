/**
 * @file hal_inference.h
 * @brief HAL AI inference interface
 * @version 2.0
 *
 * Platform-agnostic inference interface
 * Supports zero-copy via DMA-BUF
 * Decoupled from postprocessing and drawing
 */

#pragma once

#include "common/hal_types.h"
#include "common/hal_buffer.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_MAX_MODEL_PATH      256
#define HAL_MAX_TENSOR_NAME     64
#define HAL_MAX_TENSORS         16
#define HAL_MAX_TENSOR_DIMS     8

/* ========== Tensor ========== */
typedef struct {
    char name[HAL_MAX_TENSOR_NAME];     // Tensor name
    void *data;                         // Data pointer
    int32_t ndim;                       // Number of dimensions
    int32_t shape[HAL_MAX_TENSOR_DIMS]; // Shape per dimension
    HalDataType dtype;                  // Data type
    uint32_t byte_size;                 // Total size in bytes
    int32_t dma_fd;                     // DMA-BUF fd, -1 for CPU memory
    void *priv;                         // Platform-specific private data
} HalTensor;

/* ========== Tensor Layout ========== */
typedef enum {
    HAL_TENSOR_LAYOUT_UNKNOWN = 0,
    HAL_TENSOR_LAYOUT_NHWC,     // Batch, Height, Width, Channel
    HAL_TENSOR_LAYOUT_NCHW,     // Batch, Channel, Height, Width
    HAL_TENSOR_LAYOUT_NC,       // Batch, Channel
    HAL_TENSOR_LAYOUT_NHW,      // Batch, Height, Width
    HAL_TENSOR_LAYOUT_CHW,      // Channel, Height, Width
    HAL_TENSOR_LAYOUT_HWC,      // Height, Width, Channel
} HalTensorLayout;

/* ========== Preprocess ========== */

typedef enum {
    HAL_PREPROCESS_COLOR_NONE = 0,  // No conversion (frame already matches model input)
    HAL_PREPROCESS_COLOR_NV12_TO_RGB,
    HAL_PREPROCESS_COLOR_NV12_TO_BGR,
    HAL_PREPROCESS_COLOR_RGB_TO_BGR,
    HAL_PREPROCESS_COLOR_BGR_TO_RGB,
} HalPreprocessColor;

typedef enum {
    HAL_PREPROCESS_RESIZE_NEAREST = 0,
    HAL_PREPROCESS_RESIZE_BILINEAR,
} HalPreprocessResize;

typedef enum {
    HAL_PREPROCESS_LETTERBOX_NONE = 0,  // Stretch to fit
    HAL_PREPROCESS_LETTERBOX_KEEP_ASPECT, // Pad to preserve aspect ratio
} HalPreprocessLetterbox;

typedef struct {
    HalPreprocessColor color;
    HalPreprocessResize resize;
    HalPreprocessLetterbox letterbox;

    /* When letterbox==KEEP_ASPECT, pad with this value (0..255). */
    uint8_t pad_value;

    /* Output normalization:
     * - If enabled, output is float32 and computed as: out = (in/255 - mean) / std
     * - mean/std are per-channel and match output channel order after `color` conversion. */
    bool normalize;
    float mean[4];
    float std[4];

    /* Optional: force output layout when the platform needs it.
     * If UNKNOWN, implementation chooses the most efficient layout. */
    HalTensorLayout output_layout;
} HalPreprocessConfig;

/* ========== Model Info ========== */

/**
 * Per-input or per-output stream description (filled from HailoRT InferStream / HEF where available).
 */
typedef struct {
    char name[HAL_MAX_TENSOR_NAME];
    int32_t ndim;
    /** Spatial H/W for common NHWC image tensors are shape[1] / shape[2] when ndim >= 4. */
    int32_t shape[HAL_MAX_TENSOR_DIMS];
    HalDataType dtype;
    HalTensorLayout layout;

    /* Quantization (host-side transform params; optional). */
    float quant_scale;
    float quant_zero_point;
    float quant_range_min;
    float quant_range_max;

    /** Host buffer size in bytes for one frame (HailoRT InferStream::get_frame_size()). */
    uint32_t byte_size;

    /**
     * Non-zero if this output is an HailoRT NMS stream (InferStream::is_nms()).
     * Inputs are always 0; nms_* fields are valid only when is_nms != 0.
     */
    uint8_t is_nms;
    /**
     * Non-zero if this input is a host NV12 blob (pixel format), when known from the platform backend.
     * This lets backends disambiguate NV12-vs-RGB for shapes like "H/2 x W x 3" that otherwise collide.
     */
    uint8_t is_nv12;
    uint8_t reserved[2];
    uint32_t nms_number_of_classes;
    uint32_t nms_max_bboxes_per_class;
    uint32_t nms_max_bboxes_total;
    uint32_t nms_max_accumulated_mask_size;
} HalModelTensorInfo;

typedef struct {
    /** Model path / identifier (implementation may set file path). */
    char name[128];
    /** Runtime label, e.g. "hailort". */
    char version[32];
    /** First network name from HEF (empty if unknown). */
    char network_name[128];
    /** First network group name from HEF (empty if unknown). */
    char network_group_name[64];
    uint32_t num_inputs;
    uint32_t num_outputs;

    HalModelTensorInfo inputs[HAL_MAX_TENSORS];
    HalModelTensorInfo outputs[HAL_MAX_TENSORS];
} HalModelInfo;

/* ========== Multi-model scheduler (HailoRT Model Scheduler) ========== */

/** Mirrors HailoRT scheduler priority range (see HAILO_SCHEDULER_PRIORITY_*). */
#define HAL_INFER_SCHED_PRIORITY_MIN     0
#define HAL_INFER_SCHED_PRIORITY_NORMAL  16
#define HAL_INFER_SCHED_PRIORITY_MAX     31

/** Default VDevice group id for in-process / multi-process NPU sharing on Hailo-15.
 *  Must equal the medialib AI-ISP's hailort.device-id ("device0") and the inference HAL's
 *  kSharedVDeviceGroupId so every consumer joins the same cross-process group via hailort_server. */
#define HAL_INFER_DEFAULT_VDEVICE_GROUP_ID "device0"

typedef enum {
    /** Scheduling disabled — each model owns the NPU context (legacy single-model mode). */
    HAL_INFER_SCHED_NONE = 0,
    /** Round-robin model scheduler (recommended for concurrent multi-model inference). */
    HAL_INFER_SCHED_ROUND_ROBIN = 1,
} HalInferSchedulingAlgorithm;

/**
 * Per-model scheduler tuning (HailoRT InferModel::set_scheduler_*).
 * Zero fields select HailoRT defaults (threshold=1, timeout=0ms, priority=NORMAL).
 */
typedef struct {
    uint32_t scheduler_threshold;
    uint32_t scheduler_timeout_ms;
    uint8_t scheduler_priority;
    uint8_t reserved[3];
} HalInferenceSchedulerConfig;

/**
 * Shared inference runtime — one VDevice per group_id, reference-counted.
 * Acquire via runtime_acquire(); pass the handle in HalInferenceConfig.runtime so
 * multiple models share hardware scheduling (Round-Robin by default).
 */
typedef struct HalInferenceRuntime HalInferenceRuntime;

typedef struct {
    HalInferSchedulingAlgorithm algorithm;
    /** Plain device id (e.g. "device0"); empty = first available device. */
    char device_id[32];
    /** VDevice group id; empty = HAL_INFER_DEFAULT_VDEVICE_GROUP_ID. */
    char vdevice_group_id[64];
    /** When true, share the physical NPU via hailort_server (cross-process). */
    bool multi_process_service;
    uint8_t reserved[7];
} HalInferenceRuntimeConfig;

/**
 * On-chip NMS runtime parameters (HailoRT InferModel::set_nms_*).
 *
 * Only applied to output streams using HailoRT's on-chip NMS format
 * (@c is_nms in the model info); other models ignore these fields.
 * Zero / negative values select the platform default (score 0.4, IoU 0.6,
 * per-class max 50, total max 100, no class filter) — matching the
 * zero-initialized-config = unchanged-behavior convention.
 *
 * @note These take effect at session create time (HailoRT applies NMS
 * parameters during configure()); they are not runtime-mutable on an
 * already-created session.
 */
typedef struct {
    float    score_threshold;        /* <=0 = default */
    float    iou_threshold;          /* <=0 = default */
    uint32_t max_proposals_per_class;/* 0 = default */
    uint32_t max_proposals_total;    /* 0 = default */
    uint32_t class_filter_mask[8];   /* 256-class bitmask; all-zero = no filter */
} HalInferenceNmsConfig;

/* ========== Inference Config ========== */
typedef struct {
    char model_path[HAL_MAX_MODEL_PATH];
    uint32_t batch_size;
    uint32_t timeout_ms;
    bool use_dma;                   // Use zero-copy DMA-BUF
    const char *platform_config;    // Platform-specific config (JSON string or file path)
    void *platform_data;            // Platform-specific data pointer
    HalPreprocessConfig preprocess; // Optional preprocessing rules for tensor_from_frame()
    /**
     * Optional shared runtime (from runtime_acquire). When set, the session loads its HEF on
     * the shared VDevice and participates in the model scheduler.
     */
    HalInferenceRuntime *runtime;
    /** Per-model scheduler parameters (effective only when runtime uses a non-NONE algorithm). */
    HalInferenceSchedulerConfig scheduler;
    /** On-chip NMS parameters, applied to NMS-format outputs at configure time. */
    HalInferenceNmsConfig nms;
} HalInferenceConfig;

/* ========== Inference performance (device + host) ========== */

/**
 * Snapshot of accelerator and host-side utilization.
 * Convention (aligned with HailoRT): a floating field set to -1.0f means that metric
 * could not be read; integer RAM fields set to -1 mean unavailable.
 */
typedef struct {
    /** NPU / NN-core utilization, percent 0..100, or -1.0f if unknown. */
    float npu_utilization;
    /** Host CPU utilization, percent 0..100, or -1.0f if unknown. */
    float cpu_utilization;
    /** Host RAM total (kibibytes), or -1 if unknown. */
    int64_t ram_total_kib;
    /** Host RAM used (kibibytes), or -1 if unknown. */
    int64_t ram_used_kib;
    /** DSP or secondary accelerator load, percent 0..100, or -1.0f if unknown / N/A. */
    float dsp_utilization;
    /** SoC on-die temperature, sensor 0, degrees Celsius, or -1.0f if unknown. */
    float soc_temp_c;
    /** SoC on-die temperature, sensor 1, degrees Celsius, or -1.0f if unknown. */
    float soc_temp_c1;
} HalInferencePerfStats;

/**
 * Per-session inference statistics (host-side counters + optional HailoRT queue depth).
 * @p utilization is reserved for future NPU per-network metrics; -1.0f when unknown.
 */
typedef struct {
    char network_group_name[64];
    float fps;
    float utilization;
    uint64_t total_inferences;
    uint32_t pending_async_jobs;
    uint32_t async_queue_size;
    uint64_t hw_latency_us;       /**< Pure NPU hardware latency (microseconds), 0 if unknown */
} HalInferenceSessionPerfStats;

/** Async completion callback — @p status is HAL_OK or a HAL error code. */
typedef void (*HalInferenceAsyncCallback)(HalTensor *outputs, int num_outputs, int status, void *userdata);

/* ========== Inference Session (opaque handle) ========== */
typedef struct HalInferenceSession HalInferenceSession;

/* ========== Inference Operations ========== */
typedef struct HalInferenceOps {
    /**
     * @brief Create inference session
     * @param config Inference configuration
     * @return Session handle, NULL on failure
     */
    HalInferenceSession* (*create)(const HalInferenceConfig *config);

    /**
     * @brief Destroy inference session
     * @param session Session handle
     */
    void (*destroy)(HalInferenceSession *session);

    /**
     * @brief Get model information
     * @param session Session handle
     * @param info Output model info
     * @return HAL_OK on success
     */
    int (*get_model_info)(HalInferenceSession *session, HalModelInfo *info);

    /**
     * @brief Allocate input tensor (for zero-copy)
     * @param session Session handle
     * @param input_idx Input index
     * @param tensor Output tensor
     * @return HAL_OK on success
     */
    int (*alloc_input)(HalInferenceSession *session, int input_idx, HalTensor *tensor);

    /**
     * @brief Create input tensor from frame buffer (RAW COPY — no preprocessing).
     *
     * Copies the frame planes verbatim into a flat uint8 tensor. The
     * HalInferenceConfig::preprocess rules are NOT applied here; the caller is
     * responsible for matching the model input geometry/format, or must use
     * @ref tensor_from_frame_ex instead.
     *
     * @param frame Input frame
     * @param tensor Output tensor
     * @return HAL_OK on success
     */
    int (*tensor_from_frame)(const HalFrameBuffer *frame, HalTensor *tensor);

    /**
     * @brief Run inference (synchronous)
     * @param session Session handle
     * @param inputs Input tensors
     * @param num_inputs Number of inputs
     * @param outputs Output tensors.
     *
     * Ownership rules:
     * - Caller provides an array of `HalTensor` with length `num_outputs`.
     * - For each output tensor:
     *   - If `outputs[i].data == NULL`, the implementation may allocate the buffer and set `data`/`byte_size`/`dtype`
     *     (and possibly `dma_fd`/`priv`), and the caller must release it via free_tensor().
     *   - If `outputs[i].data != NULL`, the implementation writes into the caller-provided buffer; in this case
     *     free_tensor() must NOT be called for that tensor unless the implementation explicitly documented that it
     *     overwrote ownership (not recommended).
     * @param num_outputs Number of outputs
     * @return HAL_OK on success
     */
    int (*run)(HalInferenceSession *session,
               const HalTensor *inputs, int num_inputs,
               HalTensor *outputs, int num_outputs);

    /**
     * @brief Run inference asynchronously (non-blocking).
     *
     * Caller must provide output tensor slots (same ownership rules as @c run). Input/output
     * buffers must remain valid until @p callback is invoked.
     *
     * @return HAL_OK if the job was queued, or an error code immediately.
     */
    int (*run_async)(HalInferenceSession *session,
                     const HalTensor *inputs, int num_inputs,
                     HalTensor *outputs, int num_outputs,
                     HalInferenceAsyncCallback callback,
                     void *userdata);

    /**
     * @brief Acquire (or create) a shared inference runtime for multi-model scheduling.
     *
     * Reference-counted: call runtime_release() once per successful acquire / create() that
     * used the runtime pointer.
     */
    HalInferenceRuntime* (*runtime_acquire)(const HalInferenceRuntimeConfig *config);

    /** @brief Release a reference obtained from runtime_acquire(). */
    void (*runtime_release)(HalInferenceRuntime *runtime);

    /**
     * @brief Sample per-session FPS / queue statistics (no extra HEF load).
     * @param sampling_period_ms Rolling window for FPS; 0 = implementation default (1000 ms).
     */
    int (*query_session_performance_stats)(HalInferenceSession *session, uint32_t sampling_period_ms,
                                           HalInferenceSessionPerfStats *out);

    /**
     * @brief Free tensor allocated by alloc_input or run
     * @param tensor Tensor to free
     */
    void (*free_tensor)(HalTensor *tensor);

    /**
     * @brief Sample system NPU (and related host stats) without an inference session.
     *
     * No model path or HEF is required — this is independent of `create` / loaded networks.
     * Intended as a lightweight monitor hook: opens the default or selected accelerator,
     * samples once, and returns (no persistent session).
     *
     * @param device_id Optional HailoRT **plain** device id (e.g. integrated id @c "device0", PCIe BDF, or IP per
     *                  HailoRT docs). This is **not** the JSON @c platform_config string used by @c create().
     *                  NULL or @c "" selects the implementation default (typically the first available device).
     * @param sampling_period_ms Averaging window in milliseconds; 0 means implementation default (e.g. 100 ms).
     * @param out Written only on HAL_OK; on other return codes contents are left unchanged.
     * @return HAL_OK on success, HAL_ERR_INVALID_ARG if out is NULL, HAL_ERR_NOT_SUPPORTED, or other HAL errors.
     */
    int (*query_system_performance_stats)(const char *device_id, uint32_t sampling_period_ms, HalInferencePerfStats *out);

    /**
     * @brief Get version string
     * @return Version string
     */
    const char* (*get_version)(void);

    /**
     * @brief Create input tensor from a frame, applying the session's
     *        HalInferenceConfig::preprocess rules (resize / color conversion /
     *        letterbox / normalize) to match the model's first input stream.
     *
     * Unlike @ref tensor_from_frame, this is session-aware: the target geometry
     * and format come from the model input stream. Fast path with no copy
     * overhead when the frame already matches the model input exactly.
     *
     * Supported source formats: NV12, RGB24, BGR24. Resized/converted staging
     * targets packed RGB888/BGR888 model inputs; NV12-order model inputs are
     * only served by the exact-match fast path.
     *
     * @param session Inference session (defines target input + preprocess rules).
     * @param frame   Input frame (any of the supported formats, any geometry).
     * @param tensor  Output tensor; uint8 unless preprocess.normalize is set
     *                (then float32, (in/255 - mean)/std per channel; shape[0]
     *                counts elements, byte_size counts bytes).
     * @return HAL_OK on success, negative HalErrorCode on failure.
     */
    int (*tensor_from_frame_ex)(HalInferenceSession *session, const HalFrameBuffer *frame, HalTensor *tensor);
} HalInferenceOps;

/* ========== Global Operations Table ========== */
extern HalInferenceOps HAL_INFERENCE_OPS;

/* ========== On-chip NMS output decoding ========== */

/**
 * One detection decoded from an on-chip NMS output tensor.
 * Coordinates are normalized [0..1] relative to the model input frame.
 */
typedef struct {
    float y_min, x_min, y_max, x_max;   /* normalized [0..1] */
    float score;                        /* confidence [0..1] */
} HalNmsDetection;

/**
 * @brief Decode an on-chip NMS output tensor into detections.
 *
 * Raw run() outputs of streams whose HalModelTensorInfo::is_nms is set use the
 * Hailo-15 NMS layout (measured on target, HailoRT 5.3.0):
 *
 *   [count : float32] [count x { y_min, x_min, y_max, x_max, score : float32 x5 }]
 *
 * The per-class variants repeat this block per class; this helper decodes the
 * first (highest-priority) block, which on single-head detectors is the
 * complete result. Coordinates are normalized to the model input frame.
 *
 * Platform-neutral pure decoding — works on any HAL build.
 *
 * @param t          Output tensor obtained from run() (is_nms stream).
 * @param out        Caller array receiving decoded detections.
 * @param max_count  Capacity of @p out; excess boxes are truncated.
 * @param count_out  Receives the number of decoded boxes (<= max_count).
 * @return HAL_OK on success, HAL_ERR_INVALID_ARG / HAL_ERR_INVALID_SIZE on
 *         malformed input.
 */
int hal_inference_decode_nms(const HalTensor *t, HalNmsDetection *out,
                             uint32_t max_count, uint32_t *count_out);

/* ========== Helper Functions ========== */

/**
 * @brief Get tensor element count
 * @param tensor Tensor
 * @return Number of elements
 */
static inline uint32_t hal_tensor_get_element_count(const HalTensor *tensor) {
    uint32_t count = 1;
    for (int i = 0; i < tensor->ndim; i++) {
        count *= tensor->shape[i];
    }
    return count;
}

/**
 * @brief Get tensor size in bytes
 * @param tensor Tensor
 * @return Size in bytes
 */
static inline uint32_t hal_tensor_get_size(const HalTensor *tensor) {
    return hal_tensor_get_element_count(tensor) * hal_dtype_size(tensor->dtype);
}

/** Initialize HalInferenceSchedulerConfig with HailoRT defaults. */
static inline void hal_inference_scheduler_config_defaults(HalInferenceSchedulerConfig *cfg) {
    if (!cfg) return;
    cfg->scheduler_threshold = 0;
    cfg->scheduler_timeout_ms = 0;
    cfg->scheduler_priority = 0;
    cfg->reserved[0] = cfg->reserved[1] = cfg->reserved[2] = 0;
}

/**
 * Initialize HalInferenceRuntimeConfig for Hailo-15 multi-model parallel inference:
 * Round-Robin scheduler, shared group @c device0, multi-process service enabled.
 */
static inline void hal_inference_runtime_config_defaults(HalInferenceRuntimeConfig *cfg) {
    if (!cfg) return;
    cfg->algorithm = HAL_INFER_SCHED_ROUND_ROBIN;
    cfg->device_id[0] = '\0';
    {
        const char *gid = HAL_INFER_DEFAULT_VDEVICE_GROUP_ID;
        size_t i = 0;
        while (gid[i] != '\0' && i + 1U < sizeof(cfg->vdevice_group_id)) {
            cfg->vdevice_group_id[i] = gid[i];
            ++i;
        }
        cfg->vdevice_group_id[i] = '\0';
    }
    cfg->multi_process_service = true;
    cfg->reserved[0] = cfg->reserved[1] = cfg->reserved[2] = cfg->reserved[3] = 0;
    cfg->reserved[4] = cfg->reserved[5] = cfg->reserved[6] = 0;
}

#ifdef __cplusplus
}
#endif
