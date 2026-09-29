/**
 * @file stub_dsp_impl.c
 * @brief Stub platform — HAL_DSP_OPS.
 */

#include "dsp/hal_dsp.h"

#include <stddef.h>
#include <stdint.h>

#define STUB_RET() return HAL_ERR_NOT_SUPPORTED

static int stub_dsp_init(const HalDspConfig *config, void **dsp_ctx_return)
{
    (void)config;
    if (!dsp_ctx_return)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *dsp_ctx_return = NULL;
    STUB_RET();
}

static int stub_dsp_deinit(void *dsp_ctx)
{
    (void)dsp_ctx;
    STUB_RET();
}

static int stub_dsp_convert_format(void *dsp_ctx, const HalDspConvertFormatParams *params)
{
    (void)dsp_ctx;
    (void)params;
    STUB_RET();
}

static int stub_dsp_resize(void *dsp_ctx, const HalDspResizeParams *params)
{
    (void)dsp_ctx;
    (void)params;
    STUB_RET();
}

static int stub_dsp_crop_and_resize(void *dsp_ctx, const HalDspCropResizeParams *params)
{
    (void)dsp_ctx;
    (void)params;
    STUB_RET();
}

static int stub_dsp_multi_crop_and_resize(void *dsp_ctx, const HalDspMultiCropResizeParams *params)
{
    (void)dsp_ctx;
    (void)params;
    STUB_RET();
}

static int stub_dsp_blend(void *dsp_ctx, const HalDspBlendParams *params)
{
    (void)dsp_ctx;
    (void)params;
    STUB_RET();
}

static int stub_dsp_flip_rotate(void *dsp_ctx, const HalDspFlipRotateParams *params)
{
    (void)dsp_ctx;
    (void)params;
    STUB_RET();
}

static int stub_dsp_privacy_mask(void *dsp_ctx, const HalDspPrivacyMaskParams *params)
{
    (void)dsp_ctx;
    (void)params;
    STUB_RET();
}

static int stub_dsp_submit(void *dsp_ctx, HalDspOpType op_type, const void *params, HalDspJobHandle *job_out)
{
    (void)dsp_ctx;
    (void)op_type;
    (void)params;
    if (job_out)
    {
        *job_out = NULL;
    }
    STUB_RET();
}

static int stub_dsp_wait(void *dsp_ctx, HalDspJobHandle job, uint32_t timeout_ms, HalDspJobResult *result_out)
{
    (void)dsp_ctx;
    (void)job;
    (void)timeout_ms;
    (void)result_out;
    STUB_RET();
}

static int stub_dsp_cancel(void *dsp_ctx, HalDspJobHandle job)
{
    (void)dsp_ctx;
    (void)job;
    STUB_RET();
}

static int stub_dsp_job_release(void *dsp_ctx, HalDspJobHandle job)
{
    (void)dsp_ctx;
    (void)job;
    STUB_RET();
}

/* ---- M3: rotate / dewarp / telescopic (static simulation) ---- */

static int stub_dsp_rotate(void *dsp_ctx, const HalDspRotateParams *params)
{
    (void)dsp_ctx;
    if (!params || !params->src || !params->dst)
    {
        return HAL_ERR_INVALID_ARG;
    }
    return HAL_ERR_NOT_SUPPORTED; /* stub: no DSP hardware */
}

static int stub_dsp_dewarp(void *dsp_ctx, const HalDspDewarpParams *params)
{
    (void)dsp_ctx;
    if (!params || !params->src || !params->dst)
    {
        return HAL_ERR_INVALID_ARG;
    }
    return HAL_ERR_NOT_SUPPORTED; /* stub: no DSP hardware */
}

static int stub_dsp_multi_crop_resize_telescopic(void *dsp_ctx, const HalDspMultiCropResizeParams *params)
{
    (void)dsp_ctx;
    if (!params || !params->src || params->output_count == 0U || !params->outputs)
    {
        return HAL_ERR_INVALID_ARG;
    }
    return HAL_ERR_NOT_SUPPORTED; /* stub: no DSP hardware */
}

static const char *stub_dsp_get_version(void)
{
    return "HAL-DSP stub 2.1.0 (platform stub)";
}

HalDspOps HAL_DSP_OPS = {
    .init = stub_dsp_init,
    .deinit = stub_dsp_deinit,
    .convert_format = stub_dsp_convert_format,
    .resize = stub_dsp_resize,
    .crop_and_resize = stub_dsp_crop_and_resize,
    .multi_crop_and_resize = stub_dsp_multi_crop_and_resize,
    .blend = stub_dsp_blend,
    .flip_rotate = stub_dsp_flip_rotate,
    .privacy_mask = stub_dsp_privacy_mask,
    .submit = stub_dsp_submit,
    .wait = stub_dsp_wait,
    .cancel = stub_dsp_cancel,
    .job_release = stub_dsp_job_release,
    .get_version = stub_dsp_get_version,
    .rotate = stub_dsp_rotate,
    .dewarp = stub_dsp_dewarp,
    .multi_crop_resize_telescopic = stub_dsp_multi_crop_resize_telescopic,
};
