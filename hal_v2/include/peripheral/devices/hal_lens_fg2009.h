/**
 * @file hal_lens_fg2009.h
 * @brief FG2009 open-loop lens geometry and dead-reckoning position model.
 *
 * Pure math only — no serial IO (motion goes through HAL_LENS_OPS
 * zoom_rel/focus_rel/dual_rel). Constants come from the vendor tracking
 * curve (FG20090 tracing-curve spreadsheet, 2025-08-04 revision) and were
 * cross-checked on the bench; see mcu_board_prj/FG2009_BRINGUP.md.
 *
 * Coordinate conventions:
 *  - curve zoom  : 0 = WIDE .. HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS = TELE
 *  - curve focus : 0 = NEAR end .. HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS = FAR end
 *  - physical MCU step directions (bench-verified): +zoom = Tele->Wide,
 *    +focus = Far->Near, hence curve = anchor - sum(physical steps).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS 2463
#define HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS 2453
#define HAL_LENS_FG2009_WIDE_EFFL_MM 4.2393f
#define HAL_LENS_FG2009_TELE_EFFL_MM 9.5029f
#define HAL_LENS_FG2009_MIN_PPS 500
#define HAL_LENS_FG2009_MAX_PPS 900
#define HAL_LENS_FG2009_DEFAULT_PPS 600

/**
 * @brief Bootstrap parameters (bench-calibrated, override via camera-daemon.yaml).
 *
 * ram/park offsets are single-sample bench values — never derive them from
 * the vendor curve.
 */
typedef struct {
    int32_t ram_steps;        /**< overshoot that rams TELE/FAR hard stops */
    int32_t park_zoom_steps;  /**< physical steps back from the TELE stop */
    int32_t park_focus_steps; /**< physical steps back from the FAR stop */
    uint16_t zoom_pps;        /**< pps for bootstrap moves */
    uint16_t focus_pps;
} HalLensFg2009Params;

/** Dead-reckoned lens position in vendor curve coordinates. */
typedef struct {
    int32_t zoom_curve;
    int32_t focus_curve;
    bool anchored; /**< true once the hard-stop anchor has been applied */
} HalLensFg2009State;

void hal_lens_fg2009_params_init_defaults(HalLensFg2009Params *params);

/** Maximum optical zoom ratio = tele EFL / wide EFL (~2.2416). */
float hal_lens_fg2009_max_ratio(void);

/** Zoom ratio (1.0 .. max) -> curve steps (0 .. ZOOM_TRAVEL). Linear fit. */
int32_t hal_lens_fg2009_ratio_to_steps(float zoom_ratio);

/** Curve steps (0 .. ZOOM_TRAVEL) -> zoom ratio (1.0 .. max). */
float hal_lens_fg2009_steps_to_ratio(int32_t zoom_steps);

/** Focus slider level (0 = NEAR .. 1 = FAR) -> curve steps. */
int32_t hal_lens_fg2009_focus_level_to_steps(float level);

uint16_t hal_lens_fg2009_clamp_pps(uint16_t pps);
int32_t hal_lens_fg2009_clamp_zoom_curve(int32_t steps);
int32_t hal_lens_fg2009_clamp_focus_curve(int32_t steps);

/** Place the model at the hard-stop anchor (TELE, FAR) after the ram move. */
void hal_lens_fg2009_anchor(HalLensFg2009State *state);

/** Apply the park move (physical steps away from the rammed stops). */
void hal_lens_fg2009_park(HalLensFg2009State *state, const HalLensFg2009Params *params);

/** Fold a completed physical relative move into the model. */
void hal_lens_fg2009_apply_physical(HalLensFg2009State *state,
                                    int32_t zoom_physical, int32_t focus_physical);

/** Signed physical steps that move zoom from current to target curve pos. */
int32_t hal_lens_fg2009_zoom_physical_delta(const HalLensFg2009State *state,
                                            int32_t target_curve);

/** Signed physical steps that move focus from current to target curve pos. */
int32_t hal_lens_fg2009_focus_physical_delta(const HalLensFg2009State *state,
                                             int32_t target_curve);

#ifdef __cplusplus
}
#endif
