/**
 * @file hal_lens_fg2009.c
 * @brief FG2009 open-loop geometry and dead-reckoning position model.
 *
 * The FG2009 lens has no PI/home feedback: the MCU only accepts relative
 * moves and its position counters carry no optical meaning. Absolute
 * semantics are provided here in software — after a hard-stop anchor the
 * model dead-reckons every relative move in vendor curve coordinates.
 */

#include "peripheral/devices/hal_lens_fg2009.h"

#include <math.h>
#include <stddef.h>

void hal_lens_fg2009_params_init_defaults(HalLensFg2009Params *params)
{
    if (params == NULL) return;
    /* Bench-calibrated 2026-08-19; production units may need their own
     * values via camera-daemon.yaml (lens.fg2009.*). */
    params->ram_steps        = 2500;
    params->park_zoom_steps  = 2400;
    params->park_focus_steps = 2164;
    params->zoom_pps         = HAL_LENS_FG2009_DEFAULT_PPS;
    params->focus_pps        = HAL_LENS_FG2009_DEFAULT_PPS;
}

float hal_lens_fg2009_max_ratio(void)
{
    return HAL_LENS_FG2009_TELE_EFFL_MM / HAL_LENS_FG2009_WIDE_EFFL_MM;
}

int32_t hal_lens_fg2009_ratio_to_steps(float zoom_ratio)
{
    const float max_ratio = hal_lens_fg2009_max_ratio();
    if (zoom_ratio < 1.0f) zoom_ratio = 1.0f;
    if (zoom_ratio > max_ratio) zoom_ratio = max_ratio;
    const double steps_per_ratio =
        (double)HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS / (double)(max_ratio - 1.0f);
    const double steps = (zoom_ratio - 1.0f) * steps_per_ratio;
    return hal_lens_fg2009_clamp_zoom_curve((int32_t)lround(steps));
}

float hal_lens_fg2009_steps_to_ratio(int32_t zoom_steps)
{
    const int32_t clamped = hal_lens_fg2009_clamp_zoom_curve(zoom_steps);
    const float max_ratio = hal_lens_fg2009_max_ratio();
    const double steps_per_ratio =
        (double)HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS / (double)(max_ratio - 1.0f);
    return 1.0f + (float)((double)clamped / steps_per_ratio);
}

int32_t hal_lens_fg2009_focus_level_to_steps(float level)
{
    if (level < 0.0f) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    /* Level 0 = NEAR (curve 0), level 1 = FAR (curve travel). */
    return (int32_t)lround((double)level * (double)HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS);
}

uint16_t hal_lens_fg2009_clamp_pps(uint16_t pps)
{
    if (pps == 0) return HAL_LENS_FG2009_DEFAULT_PPS;
    if (pps < HAL_LENS_FG2009_MIN_PPS) return HAL_LENS_FG2009_MIN_PPS;
    if (pps > HAL_LENS_FG2009_MAX_PPS) return HAL_LENS_FG2009_MAX_PPS;
    return pps;
}

int32_t hal_lens_fg2009_clamp_zoom_curve(int32_t steps)
{
    if (steps < 0) return 0;
    if (steps > HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS) return HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS;
    return steps;
}

int32_t hal_lens_fg2009_clamp_focus_curve(int32_t steps)
{
    if (steps < 0) return 0;
    if (steps > HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS) return HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS;
    return steps;
}

void hal_lens_fg2009_anchor(HalLensFg2009State *state)
{
    if (state == NULL) return;
    state->zoom_curve = HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS;
    state->focus_curve = HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS;
    state->anchored = true;
}

void hal_lens_fg2009_park(HalLensFg2009State *state, const HalLensFg2009Params *params)
{
    if (state == NULL || params == NULL) return;
    hal_lens_fg2009_apply_physical(state, params->park_zoom_steps, params->park_focus_steps);
}

void hal_lens_fg2009_apply_physical(HalLensFg2009State *state,
                                    int32_t zoom_physical, int32_t focus_physical)
{
    if (state == NULL) return;
    /* Physical +zoom steps move Tele->Wide (curve decreases); physical +focus
     * steps move Far->Near (curve decreases): curve -= physical. */
    state->zoom_curve =
        hal_lens_fg2009_clamp_zoom_curve(state->zoom_curve - zoom_physical);
    state->focus_curve =
        hal_lens_fg2009_clamp_focus_curve(state->focus_curve - focus_physical);
}

int32_t hal_lens_fg2009_zoom_physical_delta(const HalLensFg2009State *state,
                                            int32_t target_curve)
{
    if (state == NULL || !state->anchored) return 0;
    const int32_t target = hal_lens_fg2009_clamp_zoom_curve(target_curve);
    return state->zoom_curve - target;
}

int32_t hal_lens_fg2009_focus_physical_delta(const HalLensFg2009State *state,
                                             int32_t target_curve)
{
    if (state == NULL || !state->anchored) return 0;
    const int32_t target = hal_lens_fg2009_clamp_focus_curve(target_curve);
    return state->focus_curve - target;
}
