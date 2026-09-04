/**
 * @file stub_inference_impl.c
 * @brief Stub platform — HAL_INFERENCE_OPS (no HailoRT / NPU).
 */

#include "common/hal_common.h"
#include "model/hal_inference.h"

#include <stddef.h>

struct HalInferenceSession { int unused; };
struct HalInferenceRuntime { int unused; };
static HalInferenceSession stub_infer_session_singleton;

static HalInferenceSession *stub_infer_create(const HalInferenceConfig *config)
{
    (void)config;
    /* Return a valid pointer so callers can test the full lifecycle */
    return &stub_infer_session_singleton;
}

static void stub_infer_destroy(HalInferenceSession *session)
{
    (void)session;
}

static int stub_infer_get_model_info(HalInferenceSession *session, HalModelInfo *info)
{
    if (!session || !info)
    {
        return HAL_ERR_INVALID_ARG;
    }
    /* Provide a minimal model info for testing */
    info->num_inputs = 1;
    info->num_outputs = 1;
    info->inputs[0].ndim = 4;
    info->inputs[0].shape[0] = 1;
    info->inputs[0].shape[1] = 3;
    info->inputs[0].shape[2] = 640;
    info->inputs[0].shape[3] = 640;
    info->inputs[0].dtype = HAL_DTYPE_UINT8;
    info->inputs[0].byte_size = 640 * 640 * 3;
    info->outputs[0].ndim = 2;
    info->outputs[0].shape[0] = 1;
    info->outputs[0].shape[1] = 1024;
    info->outputs[0].dtype = HAL_DTYPE_FLOAT32;
    info->outputs[0].byte_size = 1024 * sizeof(float);
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
    (void)inputs;
    (void)num_inputs;
    (void)outputs;
    (void)num_outputs;
    return 0; /* stub: succeed with whatever output buffers the caller provides */
}

static int stub_infer_run_async(HalInferenceSession *session, const HalTensor *inputs, int num_inputs,
                                HalTensor *outputs, int num_outputs, HalInferenceAsyncCallback callback, void *userdata)
{
    (void)session;
    (void)inputs;
    (void)num_inputs;
    (void)outputs;
    (void)num_outputs;
    (void)callback;
    (void)userdata;
    return HAL_ERR_NOT_SUPPORTED;
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
    .tensor_from_frame_ex = stub_infer_tensor_from_frame_ex,
    .run = stub_infer_run,
    .run_async = stub_infer_run_async,
    .runtime_acquire = stub_infer_runtime_acquire,
    .runtime_release = stub_infer_runtime_release,
    .query_session_performance_stats = stub_infer_query_session_performance_stats,
    .free_tensor = stub_infer_free_tensor,
    .query_system_performance_stats = stub_infer_query_system_performance_stats,
    .get_version = stub_infer_get_version,
};
