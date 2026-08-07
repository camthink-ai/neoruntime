/**
 * @file stub_isp_impl.c
 * @brief Stub platform — HAL_ISP_OPS.
 *
 * Stores ISP settings in static state and reflects them on readback.
 * This enables end-to-end testing of ISP configuration dispatch.
 */

#include "media/hal_isp.h"

#include <stddef.h>
#include <string.h>

/* ---- Static ISP state (simulates hardware registers) ---- */

static HalIspManualConfig g_manual_state = {
    .manual_state = false,    /* auto mode */
    .brightness   = 50,
    .contrast     = 50,
    .saturation   = 50,
    .sharpness    = 50,
};

static HalIspExposureConfig g_exposure_state = {
    .auto_exposure     = true,
    .backlight         = 50,
    .exposure_time_us  = 0,
    .gain              = 0,
};

static HalIspImageConfig g_image_state = {
    .pwr_freq          = HAL_ISP_PWR_FREQ_50HZ,
    .noise_reduction   = 50,
    .wdr_value         = 0,
    .awb_idx           = 0,
    .awb_profile_list  = NULL,
    .awb_profile_count = 0,
    .manual_config     = {0},   /* synced from g_manual_state */
    .exposure_config   = {0},   /* synced from g_exposure_state */
};

/* Sync nested configs on init */
static void sync_nested_configs(void)
{
    g_image_state.manual_config   = g_manual_state;
    g_image_state.exposure_config = g_exposure_state;
}

/* ---- ISP ops implementation ---- */

static int stub_isp_set_image_config(void *video_ctx, const HalIspImageConfig *config)
{
    (void)video_ctx;
    if (!config) return HAL_ERR_INVALID_ARG;

    /* Copy top-level fields */
    g_image_state.pwr_freq          = config->pwr_freq;
    g_image_state.noise_reduction   = config->noise_reduction;
    g_image_state.wdr_value         = config->wdr_value;
    g_image_state.awb_idx           = config->awb_idx;

    /* Copy nested configs */
    g_image_state.manual_config     = config->manual_config;
    g_image_state.exposure_config   = config->exposure_config;

    /* Sync flat caches */
    g_manual_state   = config->manual_config;
    g_exposure_state = config->exposure_config;

    return HAL_OK;
}

static int stub_isp_get_current_image_config(void *video_ctx, HalIspImageConfig *config)
{
    (void)video_ctx;
    if (!config) return HAL_ERR_INVALID_ARG;

    sync_nested_configs();
    *config = g_image_state;
    return HAL_OK;
}

static int stub_isp_set_manual_config(void *video_ctx, const HalIspManualConfig *config)
{
    (void)video_ctx;
    if (!config) return HAL_ERR_INVALID_ARG;

    g_manual_state = *config;
    g_image_state.manual_config = g_manual_state;
    return HAL_OK;
}

static int stub_isp_get_current_manual_config(void *video_ctx, HalIspManualConfig *config)
{
    (void)video_ctx;
    if (!config) return HAL_ERR_INVALID_ARG;

    *config = g_manual_state;
    return HAL_OK;
}

static int stub_isp_set_exposure_config(void *video_ctx, const HalIspExposureConfig *config)
{
    (void)video_ctx;
    if (!config) return HAL_ERR_INVALID_ARG;

    g_exposure_state = *config;
    g_image_state.exposure_config = g_exposure_state;
    return HAL_OK;
}

static int stub_isp_get_current_exposure_config(void *video_ctx, HalIspExposureConfig *config)
{
    (void)video_ctx;
    if (!config) return HAL_ERR_INVALID_ARG;

    *config = g_exposure_state;
    return HAL_OK;
}

/* ---- AF ops (stub, not supported) ---- */

static int stub_isp_set_af_windows_config(void *video_ctx, const HalIspAfWindowsConfig *config)
{
    (void)video_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_isp_get_af_windows_config(void *video_ctx, HalIspAfWindowsConfig *config)
{
    (void)video_ctx;
    if (!config) return HAL_ERR_INVALID_ARG;
    memset(config, 0, sizeof(*config));
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_isp_subscribe_af_measurement(void *video_ctx, bool enable)
{
    (void)video_ctx;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_isp_wait_af_measurement(void *video_ctx, int timeout_ms)
{
    (void)video_ctx;
    (void)timeout_ms;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_isp_get_af_measurement(void *video_ctx, HalIspAfMeasurement *meas)
{
    (void)video_ctx;
    if (!meas) return HAL_ERR_INVALID_ARG;
    memset(meas, 0, sizeof(*meas));
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_isp_get_version(void)
{
    return "HAL-ISP stub 2.1.0 (stateful)";
}

/* ---- Ops table ---- */

HalIspOps HAL_ISP_OPS = {
    .set_image_config           = stub_isp_set_image_config,
    .get_current_image_config   = stub_isp_get_current_image_config,
    .set_manual_config          = stub_isp_set_manual_config,
    .get_current_manual_config  = stub_isp_get_current_manual_config,
    .set_exposure_config        = stub_isp_set_exposure_config,
    .get_current_exposure_config = stub_isp_get_current_exposure_config,
    .set_af_windows_config      = stub_isp_set_af_windows_config,
    .get_af_windows_config      = stub_isp_get_af_windows_config,
    .subscribe_af_measurement   = stub_isp_subscribe_af_measurement,
    .wait_af_measurement        = stub_isp_wait_af_measurement,
    .get_af_measurement         = stub_isp_get_af_measurement,
    .get_version                = stub_isp_get_version,
};
