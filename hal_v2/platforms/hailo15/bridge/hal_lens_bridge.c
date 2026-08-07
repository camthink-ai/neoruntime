/**
 * HAL Lens Bridge v2 — thin C wrappers around HAL_V2's HAL_MCU_OPS + HAL_LENS_OPS.
 *
 * Exports simple function signatures suitable for dlsym() resolution from Go.
 * The bridge stores a single mcu_ctx pointer internally after io_init.
 *
 * Links against libaipc_hal.so (monolithic HAL v2).
 */

#include "peripheral/hal_mcu.h"
#include "peripheral/devices/hal_lens.h"
#include "peripheral/devices/hal_lens_af0832.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Single-instance MCU context (device-control is single-process). */
static void *g_mcu_ctx = NULL;

/* Single-instance AF0832 high-level device (created on demand). */
static HalLensAf0832 *g_af0832 = NULL;

/* ---- MCU auto-reset on persistent errors --------------------------------- */

#define CONSECUTIVE_ERROR_THRESHOLD 5

static HalMcuConfig g_mcu_config;       /* saved config for auto-reset */
static bool          g_config_saved = false;
static int           g_consecutive_errors = 0;

/**
 * Called when the consecutive-error counter hits the threshold.
 * Tears down and recreates the MCU serial context, then re-initialises
 * the lens layer.  If the reset succeeds the error counter is cleared;
 * otherwise the counter is left as-is so the upper layers (C++ service,
 * Go server) can attempt their own recovery.
 */
static void try_reset_mcu_context(void) {
    if (!g_config_saved) {
        fprintf(stderr, "HAL_LENS_BRIDGE: reset requested but no saved config\n");
        g_consecutive_errors = 0;  /* can't recover, stop trying */
        return;
    }

    fprintf(stderr, "HAL_LENS_BRIDGE: %d consecutive errors, resetting MCU context\n",
            g_consecutive_errors);

    /* Tear down AF0832 first — it holds references into the MCU context. */
    if (g_af0832 != NULL) {
        hal_lens_af0832_destroy(g_af0832);
        g_af0832 = NULL;
    }

    /* Release the serial link. */
    if (g_mcu_ctx != NULL) {
        HAL_MCU_OPS.deinit(g_mcu_ctx);
        g_mcu_ctx = NULL;
    }

    /* Re-establish the serial link. */
    int ret = HAL_MCU_OPS.init(&g_mcu_config, &g_mcu_ctx);
    if (ret != 0) {
        fprintf(stderr, "HAL_LENS_BRIDGE: MCU reinit failed: %d\n", ret);
        return;
    }

    /* Re-initialise the lens protocol layer. */
    ret = HAL_LENS_OPS.lens_init(g_mcu_ctx);
    if (ret != 0) {
        fprintf(stderr, "HAL_LENS_BRIDGE: lens reinit after MCU reset failed: %d\n", ret);
        /* Clean up the MCU context so we don't leave dangling state. */
        HAL_MCU_OPS.deinit(g_mcu_ctx);
        g_mcu_ctx = NULL;
        return;
    }

    ret = HAL_LENS_OPS.lens_config(g_mcu_ctx, (HalLensMode)0);
    if (ret != 0) {
        fprintf(stderr, "HAL_LENS_BRIDGE: lens config after MCU reset failed: %d\n", ret);
        HAL_MCU_OPS.deinit(g_mcu_ctx);
        g_mcu_ctx = NULL;
        return;
    }

    g_consecutive_errors = 0;
    fprintf(stderr, "HAL_LENS_BRIDGE: MCU context reset succeeded\n");
}

/**
 * Track the result of an MCU-bound operation.
 * Must be called immediately after every HAL_LENS_OPS / HAL_MCU_OPS call
 * that talks to the MCU (except lifecycle ops: init / deinit / config).
 */
#define TRACK_MCU_ERROR(ret) do {                         \
    if ((ret) != 0) {                                      \
        g_consecutive_errors++;                             \
        if (g_consecutive_errors >= CONSECUTIVE_ERROR_THRESHOLD) { \
            try_reset_mcu_context();                         \
        }                                                    \
    } else {                                                 \
        g_consecutive_errors = 0;                             \
    }                                                         \
} while(0)

/* ── IO lifecycle ─────────────────────────────────────────── */

int hal_bridge_io_init(const char *serial_device, uint32_t baud_rate,
                       uint32_t timeout_ms) {
    if (g_mcu_ctx != NULL) return 0;

    HalMcuConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.serial_device = serial_device;
    cfg.baud_rate     = baud_rate;
    cfg.timeout_ms    = timeout_ms;

    /* Save config for auto-reset on persistent MCU errors.
     * strdup the device path — the caller may free the pointer after return. */
    if (g_config_saved && g_mcu_config.serial_device != NULL) {
        free((void *)g_mcu_config.serial_device);
    }
    g_mcu_config = cfg;
    {
        size_t len = strlen(serial_device) + 1;
        char *copy = (char *)malloc(len);
        if (copy == NULL) return -1;
        memcpy(copy, serial_device, len);
        g_mcu_config.serial_device = copy;
    }
    if (g_mcu_config.serial_device == NULL) {
        return -1;
    }
    g_config_saved = true;

    return HAL_MCU_OPS.init(&cfg, &g_mcu_ctx);
}

int hal_bridge_io_deinit(int io_handle) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return 0;

    /* Destroy AF0832 first — it holds references into the MCU context. */
    if (g_af0832 != NULL) {
        hal_lens_af0832_destroy(g_af0832);
        g_af0832 = NULL;
    }

    int ret = HAL_MCU_OPS.deinit(g_mcu_ctx);
    g_mcu_ctx = NULL;

    /* Release the saved serial device path. */
    if (g_config_saved && g_mcu_config.serial_device != NULL) {
        free((void *)g_mcu_config.serial_device);
        g_mcu_config.serial_device = NULL;
    }
    g_config_saved = false;
    g_consecutive_errors = 0;
    return ret;
}

/* ── Lens lifecycle ───────────────────────────────────────── */

int hal_bridge_lens_init(int io_handle) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    return HAL_LENS_OPS.lens_init(g_mcu_ctx);
}

int hal_bridge_lens_deinit(int io_handle) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    return HAL_LENS_OPS.lens_deinit(g_mcu_ctx);
}

int hal_bridge_lens_config(int io_handle, uint8_t mode) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    return HAL_LENS_OPS.lens_config(g_mcu_ctx, (HalLensMode)mode);
}

/* ── Lens state ───────────────────────────────────────────── */

int hal_bridge_lens_state_get(int io_handle, HalLensState *state) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    int ret = HAL_LENS_OPS.state_get(g_mcu_ctx, state);
    TRACK_MCU_ERROR(ret);
    return ret;
}

/* ── Zoom ─────────────────────────────────────────────────── */

int hal_bridge_zoom_run(int io_handle, uint16_t pps, int32_t steps) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    HalLensMotion m = {.pps = pps, .value = steps};
    int ret = HAL_LENS_OPS.zoom_run(g_mcu_ctx, &m);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_zoom_abs(int io_handle, uint16_t pps, int32_t position) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    HalLensMotion m = {.pps = pps, .value = position};
    int ret = HAL_LENS_OPS.zoom_abs(g_mcu_ctx, &m);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_zoom_stop(int io_handle) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    int ret = HAL_LENS_OPS.zoom_stop(g_mcu_ctx);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_zoom_rz(int io_handle) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    int ret = HAL_LENS_OPS.zoom_rz(g_mcu_ctx);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_zoom_limit_set(int io_handle, int32_t min, int32_t max) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    HalLensLimit lim = {.min_pos = min, .max_pos = max};
    int ret = HAL_LENS_OPS.zoom_limit_set(g_mcu_ctx, &lim);
    TRACK_MCU_ERROR(ret);
    return ret;
}

/* ── Focus ────────────────────────────────────────────────── */

int hal_bridge_focus_run(int io_handle, uint16_t pps, int32_t steps) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    HalLensMotion m = {.pps = pps, .value = steps};
    int ret = HAL_LENS_OPS.focus_run(g_mcu_ctx, &m);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_focus_abs(int io_handle, uint16_t pps, int32_t position) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    HalLensMotion m = {.pps = pps, .value = position};
    int ret = HAL_LENS_OPS.focus_abs(g_mcu_ctx, &m);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_focus_stop(int io_handle) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    int ret = HAL_LENS_OPS.focus_stop(g_mcu_ctx);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_focus_rz(int io_handle) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    int ret = HAL_LENS_OPS.focus_rz(g_mcu_ctx);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_focus_limit_set(int io_handle, int32_t min, int32_t max) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    HalLensLimit lim = {.min_pos = min, .max_pos = max};
    int ret = HAL_LENS_OPS.focus_limit_set(g_mcu_ctx, &lim);
    TRACK_MCU_ERROR(ret);
    return ret;
}

/* ── Zoom-Focus Synchronous Dual-Axis ────────────────────────── */

int hal_bridge_zf_sync_run(int io_handle,
                           uint16_t zm_pps, int32_t zm_micro_steps,
                           uint16_t fs_pps, int32_t fs_micro_steps) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    HalLensZfSync params = {
        .zm_pps = zm_pps,
        .zm_micro_steps = zm_micro_steps,
        .fs_pps = fs_pps,
        .fs_micro_steps = fs_micro_steps,
    };
    int ret = HAL_LENS_OPS.zf_sync_run(g_mcu_ctx, &params);
    TRACK_MCU_ERROR(ret);
    return ret;
}

/* ── Iris ──────────────────────────────────────────────────── */

int hal_bridge_iris_run(int io_handle, uint16_t pps, int32_t steps) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    HalLensMotion m = {.pps = pps, .value = steps};
    int ret = HAL_LENS_OPS.iris_run(g_mcu_ctx, &m);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_iris_stop(int io_handle) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    int ret = HAL_LENS_OPS.iris_stop(g_mcu_ctx);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_iris_target_set(int io_handle, uint16_t target) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    int ret = HAL_LENS_OPS.iris_target_set(g_mcu_ctx, target);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_iris_adc_get(int io_handle, uint16_t *adc) {
    (void)io_handle;
    if (g_mcu_ctx == NULL) return -1;
    int ret = HAL_LENS_OPS.iris_adc_get(g_mcu_ctx, adc);
    TRACK_MCU_ERROR(ret);
    return ret;
}

/* ── AF0832 high-level API ─────────────────────────────────── */

int hal_bridge_af0832_create(int zoom_min, int zoom_max,
                              int focus_min, int focus_max,
                              uint16_t default_pps) {
    if (g_mcu_ctx == NULL) return -1;
    if (g_af0832 != NULL) return 0; /* already created */

    HalLensAf0832Params params;
    hal_lens_af0832_params_init_defaults(&params);
    params.zoom_limit.min_pos  = zoom_min;
    params.zoom_limit.max_pos  = zoom_max;
    params.focus_limit.min_pos = focus_min;
    params.focus_limit.max_pos = focus_max;
    params.default_pps         = default_pps;
    params.bootstrap_timeout_ms = 60000;
    params.op_timeout_ms        = 60000;

    return hal_lens_af0832_create(g_mcu_ctx, &params, &g_af0832);
}

void hal_bridge_af0832_mark_bootstrapped(void) {
    if (g_af0832 != NULL) {
        hal_lens_af0832_mark_bootstrapped(g_af0832);
    }
}

int hal_bridge_af0832_bootstrap(void) {
    if (g_af0832 == NULL) return -1;
    int ret = hal_lens_af0832_bootstrap(g_af0832);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_af0832_force_reset_zero(void) {
    if (g_af0832 == NULL) return -1;
    int ret = hal_lens_af0832_force_reset_zero(g_af0832);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_af0832_goto_by_ratio_distance(float zoom_ratio,
                                               float focus_distance_m) {
    if (g_af0832 == NULL) return -1;
    int ret = hal_lens_af0832_goto_by_ratio_distance(g_af0832, zoom_ratio,
                                                      focus_distance_m);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_af0832_zoom_abs(uint16_t pps, int32_t position) {
    if (g_af0832 == NULL) return -1;
    HalLensMotion motion = {.pps = pps, .value = position};
    int ret = hal_lens_af0832_zoom_abs(g_af0832, &motion);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_af0832_focus_abs(uint16_t pps, int32_t position) {
    if (g_af0832 == NULL) return -1;
    HalLensMotion motion = {.pps = pps, .value = position};
    int ret = hal_lens_af0832_focus_abs(g_af0832, &motion);
    TRACK_MCU_ERROR(ret);
    return ret;
}

int hal_bridge_af0832_zf_sync_abs(uint16_t zoom_pps, int32_t zoom_position,
                                  uint16_t focus_pps, int32_t focus_position,
                                  uint32_t timeout_ms) {
    if (g_af0832 == NULL) return -1;
    HalLensMotion zoom = {.pps = zoom_pps, .value = zoom_position};
    HalLensMotion focus = {.pps = focus_pps, .value = focus_position};
    int ret = hal_lens_af0832_zf_sync_abs(g_af0832, &zoom, &focus, timeout_ms);
    TRACK_MCU_ERROR(ret);
    return ret;
}

float hal_bridge_af0832_pos_to_ratio(int32_t hal_zoom_pos) {
    return hal_lens_af0832_pos_to_ratio(hal_zoom_pos);
}

int hal_bridge_af0832_calc_targets(float zoom_ratio, float focus_distance_m,
                                   int32_t *zoom_target, int32_t *focus_target) {
    return hal_lens_af0832_calc_targets(zoom_ratio, focus_distance_m,
                                        zoom_target, focus_target);
}

int hal_bridge_af0832_estimate_distance(float zoom_ratio, int32_t focus_pos,
                                         float *distance_m) {
    return hal_lens_af0832_estimate_distance(zoom_ratio, focus_pos, distance_m);
}

void hal_bridge_af0832_destroy(void) {
    if (g_af0832 != NULL) {
        hal_lens_af0832_destroy(g_af0832);
        g_af0832 = NULL;
    }
}

/* ── Shared MCU context (for LED/IR-cut ops) ─────────────── */

void *hal_bridge_get_mcu_ctx(void) {
    return g_mcu_ctx;
}
