/**
 * @file stub_inference_impl.c
 * @brief Stub platform — HAL_INFERENCE_OPS (no HailoRT / NPU).
 */

#include "common/hal_common.h"
#include "model/hal_inference.h"

#include <stdlib.h>
#include <stddef.h>

/* Per-session batch, set by create() from HalInferenceConfig::batch_size —
 * the real backend keeps batch state per HailoRT infer session, so multiple
 * sessions with different batches must coexist (InferBatch aggregation is
 * unit-tested against exactly that). Heap-allocated so each create() is
 * independent; get_model_info()/run()/run_async() scale frame sizes by the
 * session's batch the same way hailo15's set_batch_size() does. */
struct HalInferenceSession { uint32_t batch; };
struct HalInferenceRuntime { int unused; };

static uint32_t session_batch(const HalInferenceSession *s)
{
    return (s && s->batch > 0) ? s->batch : 1;
}

static HalInferenceSession *stub_infer_create(const HalInferenceConfig *config)
{
    HalInferenceSession *s = (HalInferenceSession *)calloc(1, sizeof(*s));
    if (s)
        s->batch = (config && config->batch_size > 0) ? config->batch_size : 1;
    /* Return a valid pointer so callers can test the full lifecycle */
    return s;
}

static void stub_infer_destroy(HalInferenceSession *session)
{
    free(session);
}

static int stub_infer_get_model_info(HalInferenceSession *session, HalModelInfo *info)
{
    if (!session || !info)
    {
        return HAL_ERR_INVALID_ARG;
    }
    const uint32_t batch = session_batch(session);
    /* Provide a minimal model info for testing. Sizes are scaled by the
     * configured batch: this models the platform contract that registration
     * verifies (input byte_size == B x single-frame). The real hailo15
     * backend reports get_frame_size() unscaled when the HEF does not serve
     * the batch (verified on-device 2026-09-14) — such registrations are
     * rejected by model_manager, so the stub only sees batches it serves. */
    info->num_inputs = 1;
    info->num_outputs = 1;
    info->inputs[0].ndim = 4;
    info->inputs[0].shape[0] = (int32_t)batch;
    info->inputs[0].shape[1] = 3;
    info->inputs[0].shape[2] = 640;
    info->inputs[0].shape[3] = 640;
    info->inputs[0].dtype = HAL_DTYPE_UINT8;
    /* [B,C,H,W] uint8 — the geometry the platform's batch registration check
     * verifies against (single-frame = 3*640*640, byte_size = B x that). */
    info->inputs[0].layout = HAL_TENSOR_LAYOUT_NCHW;
    info->inputs[0].byte_size = 640 * 640 * 3 * batch;
    info->outputs[0].ndim = 2;
    info->outputs[0].shape[0] = (int32_t)batch;
    info->outputs[0].shape[1] = 1024;
    info->outputs[0].dtype = HAL_DTYPE_FLOAT32;
    info->outputs[0].layout = HAL_TENSOR_LAYOUT_NC;
    info->outputs[0].byte_size = 1024 * sizeof(float) * batch;
    return 0;
}

static int stub_infer_alloc_input(HalInferenceSession *session, int input_idx, HalTensor *tensor)
{
    (void)session;
    (void)input_idx;
    (void)tensor;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_infer_tensor_from_frame(const HalFrameBuffer *frame, HalTensor *tensor)
{
    (void)frame;
    if (!tensor) return HAL_ERR_INVALID_ARG;
    /* stub: mark tensor as having no DMA fd */
    tensor->dma_fd = -1;
    tensor->byte_size = 640 * 640 * 3;
    tensor->ndim = 4;
    tensor->shape[0] = 1; tensor->shape[1] = 3; tensor->shape[2] = 640; tensor->shape[3] = 640;
    tensor->dtype = HAL_DTYPE_UINT8;
    tensor->data = NULL;
    tensor->name[0] = '\0';
    return 0;
}

static int stub_infer_tensor_from_frame_ex(HalInferenceSession *session, const HalFrameBuffer *frame,
                                           HalTensor *tensor)
{
    (void)session;
    /* Stub has no model geometry; behave as the raw-copy variant. */
    return stub_infer_tensor_from_frame(frame, tensor);
}

static int stub_infer_run(HalInferenceSession *session, const HalTensor *inputs, int num_inputs, HalTensor *outputs,
                          int num_outputs)
{
    if (!session) return HAL_ERR_INVALID_ARG;
    /* Mirror the hailo15 bind check: the caller must supply exactly
     * B x per-frame bytes for input[0]. This turns a wrong-sized batch
     * payload into a testable failure instead of a silent success. */
    if (num_inputs > 0 && inputs)
    {
        const uint32_t want = 640 * 640 * 3 * session_batch(session);
        if (inputs[0].byte_size != want)
        {
            return HAL_ERR_INVALID_SIZE;
        }
    }
    (void)outputs;
    (void)num_outputs;
    return 0; /* stub: succeed with whatever output buffers the caller provides */
}

static int stub_infer_run_async(HalInferenceSession *session, const HalTensor *inputs, int num_inputs,
                                HalTensor *outputs, int num_outputs, HalInferenceAsyncCallback callback, void *userdata)
{
    /* Same validation as the synchronous path, then fire the callback inline so
     * the gRPC InferBatch aggregation path is exercisable against the stub. */
    int rc = stub_infer_run(session, inputs, num_inputs, outputs, num_outputs);
    if (rc != 0) return rc;
    if (callback) callback(outputs, num_outputs, 0, userdata);
    return 0;
}

static HalInferenceRuntime *stub_infer_runtime_acquire(const HalInferenceRuntimeConfig *config)
{
    (void)config;
    return NULL;
}

static void stub_infer_runtime_release(HalInferenceRuntime *runtime)
{
    (void)runtime;
}

static int stub_infer_query_session_performance_stats(HalInferenceSession *session, uint32_t sampling_period_ms,
                                                      HalInferenceSessionPerfStats *out)
{
    (void)session;
    (void)sampling_period_ms;
    if (!out)
        return HAL_ERR_INVALID_ARG;
    return HAL_ERR_NOT_SUPPORTED;
}

static void stub_infer_free_tensor(HalTensor *tensor)
{
    (void)tensor;
}

static const char *stub_infer_get_version(void)
{
    return "HAL-INFERENCE stub 2.1.0 (platform stub)";
}

static int stub_infer_query_system_performance_stats(const char *device_id, uint32_t sampling_period_ms,
                                                     HalInferencePerfStats *out)
{
    (void)device_id;
    (void)sampling_period_ms;
    if (!out)
        return HAL_ERR_INVALID_ARG;
    return HAL_ERR_NOT_SUPPORTED;
}

HalInferenceOps HAL_INFERENCE_OPS = {
    .create = stub_infer_create,
    .destroy = stub_infer_destroy,
    .get_model_info = stub_infer_get_model_info,
    .alloc_input = stub_infer_alloc_input,
    .tensor_from_frame = stub_infer_tensor_from_frame,
    .run = stub_infer_run,
    .run_async = stub_infer_run_async,
    .runtime_acquire = stub_infer_runtime_acquire,
    .runtime_release = stub_infer_runtime_release,
    .query_session_performance_stats = stub_infer_query_session_performance_stats,
    .free_tensor = stub_infer_free_tensor,
    .query_system_performance_stats = stub_infer_query_system_performance_stats,
    .get_version = stub_infer_get_version,
    /* M3 addition (appended at the table tail, after get_version) */
    .tensor_from_frame_ex = stub_infer_tensor_from_frame_ex,
};

/* ABI guard companion (see hal_inference.h). */
const uint32_t HAL_INFERENCE_OPS_ABI_SIZE = (uint32_t)sizeof(HalInferenceOps);
