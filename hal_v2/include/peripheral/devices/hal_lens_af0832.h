/**
 * @file hal_lens_af0832.h
 * @brief High-level AF0832 lens convenience API built on top of HalLensOps.
 *
 * This is platform-independent code (implemented in hal_v2/common/devices/).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../../common/hal_common.h"
#include "hal_lens.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Wire values match MCU ms41908m_state_t (host_link_lens_state_t fields).
 * Use for interpreting HalLensState zoom_state / focus_state / iris_state.
 */
typedef enum {
    HAL_LENS_MS41908M_STATE_NO_CFG = 0,
    HAL_LENS_MS41908M_STATE_STOPPED = 1,
    HAL_LENS_MS41908M_STATE_RUNNING = 2,
    HAL_LENS_MS41908M_STATE_RESET_ZERO = 3,
    HAL_LENS_MS41908M_STATE_ERROR = 4,
} HalLensMs41908mState;

/**
 * Event bits in MCU lens event (host_link_lens_evt_t.event).
 */
#define HAL_LENS_EVT_ZOOM_COMPLETED     (1u << 5)
#define HAL_LENS_EVT_FOCUS_COMPLETED    (1u << 4)
#define HAL_LENS_EVT_ZOOM_RESET_DONE    (1u << 8)
#define HAL_LENS_EVT_FOCUS_RESET_DONE   (1u << 9)
/** Completion-event mask for both axes; the MCU emits one event per axis. */
#define HAL_LENS_EVT_ZF_SYNC_RUN_DONE   (HAL_LENS_EVT_ZOOM_COMPLETED | HAL_LENS_EVT_FOCUS_COMPLETED)

/**
 * Default soft limits (HAL steps).
 *
 * These defaults are chosen to be compatible with the AF0832 tracking table used by
 * hal_lens_af0832_goto_by_ratio_distance(), including some margin up to the spec
 * mechanical dead-points (table step * 4).
 *
 * Replace with calibrated values via HalLensAf0832Params for production.
 */
#define HAL_LENS_AF0832_DEFAULT_ZOOM_MIN_POS   -3236  /* reversed zoom: -(809 * 4) */
#define HAL_LENS_AF0832_DEFAULT_ZOOM_MAX_POS   760    /* reversed zoom: -(-190 * 4) */
#define HAL_LENS_AF0832_DEFAULT_FOCUS_MIN_POS  -844   /* spec FOCUS back dead-point: -211 * 4 */
#define HAL_LENS_AF0832_DEFAULT_FOCUS_MAX_POS  592    /* spec FOCUS front dead-point: 148 * 4 */

typedef struct HalLensAf0832 HalLensAf0832;

typedef void (*HalLensAf0832OnEvent)(HalLensAf0832 *dev, uint32_t event, int32_t result,
                                     int32_t zoom_pos, int32_t focus_pos, void *userdata);

typedef struct {
    uint32_t bootstrap_timeout_ms;
    uint32_t op_timeout_ms;

    HalLensLimit zoom_limit;
    HalLensLimit focus_limit;

    /**
     * Default speed (pps) used by high-level convenience helpers
     * (e.g. hal_lens_af0832_goto_by_ratio_distance()).
     */
    uint16_t default_pps;
} HalLensAf0832Params;

void hal_lens_af0832_params_init_defaults(HalLensAf0832Params *p);

int hal_lens_af0832_create(void *mcu_ctx, const HalLensAf0832Params *params, HalLensAf0832 **out_dev);
void hal_lens_af0832_destroy(HalLensAf0832 *dev);

int hal_lens_af0832_bootstrap(HalLensAf0832 *dev);

void hal_lens_af0832_mark_bootstrapped(HalLensAf0832 *dev);

int hal_lens_af0832_set_event_callback(HalLensAf0832 *dev, HalLensAf0832OnEvent cb, void *userdata);

int hal_lens_af0832_state_get(HalLensAf0832 *dev, HalLensState *state);

int hal_lens_af0832_iris_target_set(HalLensAf0832 *dev, uint16_t target);
int hal_lens_af0832_iris_adc_get(HalLensAf0832 *dev, uint16_t *adc);

int hal_lens_af0832_iris_run(HalLensAf0832 *dev, const HalLensMotion *motion);
int hal_lens_af0832_zoom_run(HalLensAf0832 *dev, const HalLensMotion *motion);
int hal_lens_af0832_zoom_abs(HalLensAf0832 *dev, const HalLensMotion *motion);
int hal_lens_af0832_focus_run(HalLensAf0832 *dev, const HalLensMotion *motion);
int hal_lens_af0832_focus_abs(HalLensAf0832 *dev, const HalLensMotion *motion);

/**
 * @brief Async (non-blocking) motor movement — return immediately.
 *
 * These issue the motion command and return without waiting for completion.
 * Use hal_lens_af0832_wait_zoom() / hal_lens_af0832_wait_focus() to synchronise.
 * Multiple axes can be started back-to-back for concurrent movement.
 */
int hal_lens_af0832_zoom_run_async(HalLensAf0832 *dev, const HalLensMotion *motion);
int hal_lens_af0832_focus_run_async(HalLensAf0832 *dev, const HalLensMotion *motion);
int hal_lens_af0832_zoom_abs_async(HalLensAf0832 *dev, const HalLensMotion *motion);
int hal_lens_af0832_focus_abs_async(HalLensAf0832 *dev, const HalLensMotion *motion);

/**
 * @brief Block until the specific axis stops moving.
 *
 * Arms the event wait before checking motor state so a completion event cannot
 * be lost between the state query and the wait.
 *
 * @param timeout_ms Maximum wait time in milliseconds.
 * @param result     Optional output for the completion result (0 = success).
 * @return HAL_OK on completion, HAL_ERR_TIMEOUT on timeout.
 */
int hal_lens_af0832_wait_zoom(HalLensAf0832 *dev, uint32_t timeout_ms, int32_t *result);
int hal_lens_af0832_wait_focus(HalLensAf0832 *dev, uint32_t timeout_ms, int32_t *result);

/**
 * @brief Start zoom and focus on the same VD_FZ edge and wait for both axes.
 *
 * Each active axis runs at its own speed for its own distance. The underlying
 * MCU command is asynchronous, but this convenience call blocks until every
 * active axis emits its completion event or the configured operation timeout
 * expires. Set an axis micro_steps value to 0 to skip that axis.
 *
 * @param dev Lens device (bootstrapped).
 * @param params Dual-axis motion parameters (set micro_steps to 0 to skip axis).
 * @return HAL_OK after all active axes complete, HAL_ERR_TIMEOUT on timeout,
 *         otherwise an error code.
 */
int hal_lens_af0832_zf_sync_run(HalLensAf0832 *dev, const HalLensZfSync *params);

/**
 * @brief Move zoom and focus to absolute targets on one synchronized trajectory segment.
 *
 * Current positions are read once, absolute targets are clamped to the configured
 * soft limits, and the resulting relative distances are submitted with one
 * ZF_SYNC_RUN command. The call waits for every active axis.
 *
 * @param zoom Optional zoom absolute target and PPS; NULL skips zoom.
 * @param focus Optional focus absolute target and PPS; NULL skips focus.
 * @param timeout_ms Completion timeout; 0 uses the configured operation timeout.
 */
int hal_lens_af0832_zf_sync_abs(HalLensAf0832 *dev,
                                const HalLensMotion *zoom,
                                const HalLensMotion *focus,
                                uint32_t timeout_ms);

/**
 * @brief Force reset-zero (homing) for both zoom and focus motors.
 *
 * Unlike hal_lens_af0832_bootstrap(), this API always triggers zoom_rz and focus_rz
 * even if the current state already reports *_rz_done. The call blocks until both
 * reset-zero operations complete (by event and/or rz_done polling), or until timeout.
 *
 * @param dev Lens device (bootstrapped).
 * @return HAL_OK on success, otherwise a negative HalErrorCode / HAL_ERROR.
 */
int hal_lens_af0832_force_reset_zero(HalLensAf0832 *dev);

/**
 * @brief Move lens by "zoom ratio" and "focus distance" using the AF0832 tracking table.
 *
 * Converts (zoom_ratio, focus_distance_m) to absolute zoom/focus target positions based on the
 * product spec table, using repository convention: 1 table step == 4 HAL steps.
 *
 * Mapping rules:
 * - zoom_ratio clamped to [1.00, 2.88], linearly interpolated between zoom rows.
 * - focus_distance_m:
 *   - <= 0 => INF column
 *   - otherwise clamped to [1.5, 10.0] meters and linearly interpolated between distance columns.
 * - focus step bilinear interpolation: distance interpolation inside each zoom row, then zoom interpolation.
 *
 * Execution:
 * - calculates relative zoom/focus travel from the current absolute positions;
 * - scales each axis PPS so both movements finish at approximately the same time;
 * - starts both active axes on one VD_FZ edge and waits for both completion events.
 */
int hal_lens_af0832_goto_by_ratio_distance(HalLensAf0832 *dev, float zoom_ratio, float focus_distance_m);

/**
 * @brief Calculate table-based absolute motor targets without moving the lens.
 *
 * Uses the same AF0832 tracking table and coordinate convention as
 * hal_lens_af0832_goto_by_ratio_distance().
 *
 * @param zoom_ratio Optical zoom ratio; clamped to the table range.
 * @param focus_distance_m Focus distance in meters; <= 0 means INF.
 * @param zoom_target Optional output absolute zoom target in HAL steps.
 * @param focus_target Optional output absolute focus target in HAL steps.
 */
int hal_lens_af0832_calc_targets(float zoom_ratio, float focus_distance_m,
                                 int32_t *zoom_target, int32_t *focus_target);

/**
 * @brief Estimate object distance from a zoom ratio and measured best focus position.
 *
 * This is a reverse lookup against the AF0832 focus tracking table. It is intended
 * as a practical seed for follow-zoom: run AF once, estimate distance, then predict
 * the next focus position when zoom changes.
 *
 * @param zoom_ratio Current optical zoom ratio.
 * @param focus_pos Current/best absolute focus position in HAL steps.
 * @param distance_m Output estimated distance in meters; 0 means INF.
 */
int hal_lens_af0832_estimate_distance(float zoom_ratio, int32_t focus_pos, float *distance_m);

/**
 * Reverse-lookup: convert a HAL zoom position back to an optical zoom ratio
 * using the AF0832 calibration table with linear interpolation.
 */
float hal_lens_af0832_pos_to_ratio(int32_t hal_zoom_pos);

#ifdef __cplusplus
}
#endif
