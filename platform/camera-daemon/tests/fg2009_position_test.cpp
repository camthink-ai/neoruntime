#include "peripheral/devices/hal_lens_fg2009.h"

#include <cassert>
#include <cmath>

static bool near(float a, float b, float eps) { return std::fabs(a - b) <= eps; }

int main() {
    /* Ratio <-> steps mapping (vendor tracking curve, linear fit). */
    assert(near(hal_lens_fg2009_max_ratio(), 2.2416f, 0.001f));
    assert(hal_lens_fg2009_ratio_to_steps(1.0f) == 0);
    assert(hal_lens_fg2009_ratio_to_steps(hal_lens_fg2009_max_ratio()) ==
           HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS);
    /* Ratio 2.0 lands at ~1984 steps on the linear fit; the exact vendor
     * curve interpolation gives 1977 — the 7-step (0.28%) linearization
     * error is accepted for slider/LUT use. */
    const double steps_per_ratio =
        (double)HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS / (hal_lens_fg2009_max_ratio() - 1.0f);
    assert(std::abs(hal_lens_fg2009_ratio_to_steps(2.0f) -
                    (int32_t)lround(steps_per_ratio)) <= 1);
    assert(std::abs(hal_lens_fg2009_ratio_to_steps(2.0f) - 1977) <= 8);
    /* Out-of-range ratios clamp to the endpoints. */
    assert(hal_lens_fg2009_ratio_to_steps(0.5f) == 0);
    assert(hal_lens_fg2009_ratio_to_steps(9.9f) == HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS);
    /* Round trip. */
    for (int32_t s = 0; s <= HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS; s += 97) {
        const float r = hal_lens_fg2009_steps_to_ratio(s);
        assert(std::abs(hal_lens_fg2009_ratio_to_steps(r) - s) <= 1);
    }

    /* Focus level mapping: 0 = NEAR (curve 0), 1 = FAR (curve travel). */
    assert(hal_lens_fg2009_focus_level_to_steps(0.0f) == 0);
    assert(hal_lens_fg2009_focus_level_to_steps(1.0f) ==
           HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS);
    assert(hal_lens_fg2009_focus_level_to_steps(-1.0f) == 0);
    assert(hal_lens_fg2009_focus_level_to_steps(2.0f) ==
           HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS);

    /* PPS clamping. */
    assert(hal_lens_fg2009_clamp_pps(0) == HAL_LENS_FG2009_DEFAULT_PPS);
    assert(hal_lens_fg2009_clamp_pps(100) == HAL_LENS_FG2009_MIN_PPS);
    assert(hal_lens_fg2009_clamp_pps(5000) == HAL_LENS_FG2009_MAX_PPS);
    assert(hal_lens_fg2009_clamp_pps(700) == 700);

    /* Defaults carry the bench-calibrated bootstrap offsets. */
    HalLensFg2009Params params;
    hal_lens_fg2009_params_init_defaults(&params);
    assert(params.ram_steps == 2500);
    assert(params.park_zoom_steps == 2400);
    assert(params.park_focus_steps == 2164);
    assert(params.zoom_pps == 600 && params.focus_pps == 600);

    /* Bootstrap: ram both hard stops, anchor, park — bench-verified landing
     * point is (zoom 63, focus 289). */
    HalLensFg2009State state = {0, 0, false};
    assert(hal_lens_fg2009_zoom_physical_delta(&state, 1000) == 0);
    hal_lens_fg2009_anchor(&state);
    assert(state.zoom_curve == HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS);
    assert(state.focus_curve == HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS);
    assert(state.anchored);
    hal_lens_fg2009_park(&state, &params);
    assert(state.zoom_curve == 63);
    assert(state.focus_curve == 289);

    /* Dead reckoning: curve = anchor - sum(physical). Moving toward TELE
     * (target above current) needs negative physical zoom steps. */
    assert(hal_lens_fg2009_zoom_physical_delta(&state, 1000) == 63 - 1000);
    assert(hal_lens_fg2009_focus_physical_delta(&state, 0) == 289);
    hal_lens_fg2009_apply_physical(&state, 63 - 1000, 289);
    assert(state.zoom_curve == 1000);
    assert(state.focus_curve == 0);

    /* Clamping keeps the model inside travel even on overshoot: negative
     * zoom physical = Wide->Tele (curve rises to TELE), positive focus
     * physical = Far->Near (curve falls to NEAR). */
    hal_lens_fg2009_apply_physical(&state, -5000, 5000);
    assert(state.zoom_curve == HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS);
    assert(state.focus_curve == 0);
    hal_lens_fg2009_apply_physical(&state, 5000, -5000);
    assert(state.zoom_curve == 0);
    assert(state.focus_curve == HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS);

    /* Zoom ratio of the anchored state feeds the IR LUT follow. */
    assert(near(hal_lens_fg2009_steps_to_ratio(0), 1.0f, 0.0001f));
    assert(near(hal_lens_fg2009_steps_to_ratio(HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS),
                hal_lens_fg2009_max_ratio(), 0.0001f));
    return 0;
}
