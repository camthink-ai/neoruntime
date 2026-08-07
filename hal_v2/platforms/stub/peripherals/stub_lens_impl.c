/** @file stub_lens_impl.c — HAL_LENS_OPS (stub) */
#include "peripheral/devices/hal_lens.h"

#define STUB() return HAL_ERR_NOT_SUPPORTED

static int stub_lens_init(void *mcu_ctx)
{
    (void)mcu_ctx;
    STUB();
}

static int stub_lens_deinit(void *mcu_ctx)
{
    (void)mcu_ctx;
    STUB();
}

static int stub_lens_config(void *mcu_ctx, HalLensMode mode)
{
    (void)mcu_ctx;
    (void)mode;
    STUB();
}

static int stub_state_get(void *mcu_ctx, HalLensState *out_state)
{
    (void)mcu_ctx;
    (void)out_state;
    STUB();
}

static int stub_iris_run(void *mcu_ctx, const HalLensMotion *motion)
{
    (void)mcu_ctx;
    (void)motion;
    STUB();
}

static int stub_iris_stop(void *mcu_ctx)
{
    (void)mcu_ctx;
    STUB();
}

static int stub_iris_target_set(void *mcu_ctx, uint16_t target)
{
    (void)mcu_ctx;
    (void)target;
    STUB();
}

static int stub_iris_adc_get(void *mcu_ctx, uint16_t *out_adc)
{
    (void)mcu_ctx;
    (void)out_adc;
    STUB();
}

static int stub_zoom_run(void *mcu_ctx, const HalLensMotion *motion)
{
    (void)mcu_ctx;
    (void)motion;
    STUB();
}

static int stub_zoom_abs(void *mcu_ctx, const HalLensMotion *motion)
{
    (void)mcu_ctx;
    (void)motion;
    STUB();
}

static int stub_zoom_stop(void *mcu_ctx)
{
    (void)mcu_ctx;
    STUB();
}

static int stub_zoom_rz(void *mcu_ctx)
{
    (void)mcu_ctx;
    STUB();
}

static int stub_zoom_limit_set(void *mcu_ctx, const HalLensLimit *limit)
{
    (void)mcu_ctx;
    (void)limit;
    STUB();
}

static int stub_focus_run(void *mcu_ctx, const HalLensMotion *motion)
{
    (void)mcu_ctx;
    (void)motion;
    STUB();
}

static int stub_focus_abs(void *mcu_ctx, const HalLensMotion *motion)
{
    (void)mcu_ctx;
    (void)motion;
    STUB();
}

static int stub_focus_stop(void *mcu_ctx)
{
    (void)mcu_ctx;
    STUB();
}

static int stub_focus_rz(void *mcu_ctx)
{
    (void)mcu_ctx;
    STUB();
}

static int stub_focus_limit_set(void *mcu_ctx, const HalLensLimit *limit)
{
    (void)mcu_ctx;
    (void)limit;
    STUB();
}

static int stub_zf_sync_run(void *mcu_ctx, const HalLensZfSync *params)
{
    (void)mcu_ctx;
    (void)params;
    STUB();
}

static int stub_lens_subscribe(void *mcu_ctx, HalLensEventCallback cb, void *userdata)
{
    (void)mcu_ctx;
    (void)cb;
    (void)userdata;
    STUB();
}

static int stub_lens_unsubscribe(void *mcu_ctx)
{
    (void)mcu_ctx;
    STUB();
}

static const char *stub_lens_get_version(void)
{
    return "HAL-LENS stub 2.0.0 (platform stub)";
}

HalLensOps HAL_LENS_OPS = {
    .lens_init = stub_lens_init,
    .lens_deinit = stub_lens_deinit,
    .lens_config = stub_lens_config,
    .state_get = stub_state_get,
    .iris_run = stub_iris_run,
    .iris_stop = stub_iris_stop,
    .iris_target_set = stub_iris_target_set,
    .iris_adc_get = stub_iris_adc_get,
    .zoom_run = stub_zoom_run,
    .zoom_abs = stub_zoom_abs,
    .zoom_stop = stub_zoom_stop,
    .zoom_rz = stub_zoom_rz,
    .zoom_limit_set = stub_zoom_limit_set,
    .focus_run = stub_focus_run,
    .focus_abs = stub_focus_abs,
    .focus_stop = stub_focus_stop,
    .focus_rz = stub_focus_rz,
    .focus_limit_set = stub_focus_limit_set,
    .zf_sync_run = stub_zf_sync_run,
    .subscribe = stub_lens_subscribe,
    .unsubscribe = stub_lens_unsubscribe,
    .get_version = stub_lens_get_version,
};
