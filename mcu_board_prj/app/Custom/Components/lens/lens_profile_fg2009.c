#include "lens_profile.h"

/* Register baseline: reuse the proven board-level MS41908M settings.
 * Optical directions have been verified on FG2009 hardware. PPW still needs
 * to be checked against the 145 mA peak-current limit on each production unit. */
static const ms41908m_motor_config_t s_fg2009_motor_config = {
    .dt1 = 0x0A,
    .pwm_mode = 30,
    .pwm_res = 2,
    .fz_test = 7,
    .test_en2 = 1,
    .dt2a = 3,
    .phmodab = 0,
    .ppwa = 200,
    .ppwb = 200,
    .leda = 0,
    .microab = 0,
    .dt2b = 3,
    .phmodcd = 22,
    .ppwc = 200,
    .ppwd = 200,
    .ledb = 0,
    .microcd = 0,
};

const lens_profile_t g_lens_profile_fg2009 = {
    .model = LENS_MODEL_FG2009,
    .name = "FG20090AD4K-H33B78S",
    .motor_config = &s_fg2009_motor_config,
    .iris_config = NULL,
    .zoom = {
        .psum_units_per_step = 8,
        .min_pps = 500,
        .max_pps = 900,
        .default_pps = 600,
        /* Confirmed against the vendor focus-curve TELE limit coordinate. */
        .nominal_travel_steps = 2463,
        .travel_tolerance_steps = 20,
        .direction_sign = 1,
    },
    .focus = {
        .psum_units_per_step = 8,
        .min_pps = 500,
        .max_pps = 900,
        .default_pps = 600,
        .nominal_travel_steps = 2453,
        .travel_tolerance_steps = 20,
        /* Verified on FG2009 hardware with the current J9 wiring:
         * negative shell steps move Focus from Near to Far, while positive
         * shell steps move it from Far to Near. The vendor focus-curve
         * coordinate therefore changes with the opposite sign. */
        .direction_sign = -1,
    },
    .capabilities = {
        .supports_relative = 1,
        .supports_absolute = 0,
        .supports_home = 0,
        .supports_pi = 0,
        .supports_sync_relative = 1,
        .supports_ircut = 1,
        .supports_iris = 0,
    },
    /* Verified on FG2009 hardware: level 0 inserts the IR-cut filter (day),
     * while level 1 removes it (night). */
    .ircut_day_level = 0,
    .ircut_night_level = 1,
    .ircut_min_interval_ms = 2000,
};
