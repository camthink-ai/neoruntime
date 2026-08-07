/**
 * @file hal_lens.h
 * @brief Lens motor primitives via MCU.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_LENS_MODE_ALL = 0,
    HAL_LENS_MODE_IRIS = 1,
    HAL_LENS_MODE_MOTOR = 2,
} HalLensMode;

typedef struct {
    uint16_t pps;   /**< pulses per second */
    int32_t  value; /**< run: micro steps; abs: target position */
} HalLensMotion;

typedef struct {
    int32_t min_pos;
    int32_t max_pos;
} HalLensLimit;

/**
 * @brief Zoom-focus synchronous dual-axis motion parameters.
 *
 * Both axes start on the same VD_FZ edge for simultaneous movement
 * with independent speeds. Set micro_steps to 0 to skip an axis.
 */
typedef struct {
    uint16_t zm_pps;          /**< zoom pulses per second */
    int32_t  zm_micro_steps;  /**< zoom micro steps (0 = skip) */
    uint16_t fs_pps;          /**< focus pulses per second */
    int32_t  fs_micro_steps;  /**< focus micro steps (0 = skip) */
} HalLensZfSync;

typedef struct {
    uint8_t iris_state;
    uint8_t zoom_state;
    uint8_t focus_state;
    bool    zoom_rz_done;
    bool    focus_rz_done;
    int32_t zoom_pos;
    int32_t focus_pos;
} HalLensState;

typedef void (*HalLensEventCallback)(void *mcu_ctx, uint32_t event, int32_t result,
                                     int32_t zoom_pos, int32_t focus_pos, void *userdata);

typedef struct {
    int (*lens_init)(void *mcu_ctx);
    int (*lens_deinit)(void *mcu_ctx);
    int (*lens_config)(void *mcu_ctx, HalLensMode mode);
    int (*state_get)(void *mcu_ctx, HalLensState *out_state);

    int (*iris_run)(void *mcu_ctx, const HalLensMotion *motion);
    int (*iris_stop)(void *mcu_ctx);
    int (*iris_target_set)(void *mcu_ctx, uint16_t target);
    int (*iris_adc_get)(void *mcu_ctx, uint16_t *out_adc);

    int (*zoom_run)(void *mcu_ctx, const HalLensMotion *motion);
    int (*zoom_abs)(void *mcu_ctx, const HalLensMotion *motion);
    int (*zoom_stop)(void *mcu_ctx);
    int (*zoom_rz)(void *mcu_ctx);
    int (*zoom_limit_set)(void *mcu_ctx, const HalLensLimit *limit);

    int (*focus_run)(void *mcu_ctx, const HalLensMotion *motion);
    int (*focus_abs)(void *mcu_ctx, const HalLensMotion *motion);
    int (*focus_stop)(void *mcu_ctx);
    int (*focus_rz)(void *mcu_ctx);
    int (*focus_limit_set)(void *mcu_ctx, const HalLensLimit *limit);

    /**
     * @brief Start zoom and focus simultaneously on the same VD_FZ edge.
     *
     * Each axis runs at its own speed for its own distance.
     * Set micro_steps to 0 for an axis to skip it.
     * Wait for completion via zf_sync_wait or monitor lens events.
     *
     * @return HAL_OK on success, otherwise error code.
     */
    int (*zf_sync_run)(void *mcu_ctx, const HalLensZfSync *params);

    int (*subscribe)(void *mcu_ctx, HalLensEventCallback cb, void *userdata);
    int (*unsubscribe)(void *mcu_ctx);

    const char *(*get_version)(void);
} HalLensOps;

extern HalLensOps HAL_LENS_OPS;

#ifdef __cplusplus
}
#endif

