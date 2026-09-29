#include "lens_profile.h"

const lens_profile_t g_lens_profile_af0832 = {
    .model = LENS_MODEL_AF0832,
    .name = "AF0832D09",
    .motor_config = &g_default_motor_config,
    .iris_config = &g_default_iris_config,
    .zoom = {
        .psum_units_per_step = 4,
        .min_pps = 24,
        .max_pps = 1000,
        .default_pps = 800,
        .nominal_travel_steps = 999,
        .travel_tolerance_steps = 0,
        .direction_sign = 1,
    },
    .focus = {
        .psum_units_per_step = 4,
        .min_pps = 24,
        .max_pps = 1000,
        .default_pps = 800,
        .nominal_travel_steps = 359,
        .travel_tolerance_steps = 0,
        .direction_sign = 1,
    },
    .capabilities = {
        .supports_relative = 1,
        .supports_absolute = 1,
        .supports_home = 1,
        .supports_pi = 1,
        .supports_sync_relative = 1,
        .supports_ircut = 1,
        .supports_iris = 1,
    },
    .ircut_day_level = 1,
    .ircut_night_level = 0,
    .ircut_min_interval_ms = 0,
};
