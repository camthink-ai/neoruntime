#ifndef __LENS_PROFILE_H__
#define __LENS_PROFILE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "ms41908m.h"
#include <stdint.h>

typedef enum {
    LENS_MODEL_AF0832 = 0,
    LENS_MODEL_FG2009,
    LENS_MODEL_COUNT,
} lens_model_t;

typedef enum {
    LENS_AXIS_ZOOM = 0,
    LENS_AXIS_FOCUS,
} lens_axis_t;

typedef struct {
    uint8_t psum_units_per_step;
    uint16_t min_pps;
    uint16_t max_pps;
    uint16_t default_pps;
    uint16_t nominal_travel_steps;
    uint16_t travel_tolerance_steps;
    int8_t direction_sign;
} lens_axis_profile_t;

typedef struct {
    uint8_t supports_relative;
    uint8_t supports_absolute;
    uint8_t supports_home;
    uint8_t supports_pi;
    uint8_t supports_sync_relative;
    uint8_t supports_ircut;
    uint8_t supports_iris;
} lens_capabilities_t;

typedef struct {
    lens_model_t model;
    const char *name;
    const ms41908m_motor_config_t *motor_config;
    const ms41908m_iris_config_t *iris_config;
    lens_axis_profile_t zoom;
    lens_axis_profile_t focus;
    lens_capabilities_t capabilities;
    uint8_t ircut_day_level;
    uint8_t ircut_night_level;
    uint16_t ircut_min_interval_ms;
} lens_profile_t;

extern const lens_profile_t g_lens_profile_af0832;
extern const lens_profile_t g_lens_profile_fg2009;

const lens_profile_t *lens_profile_get(lens_model_t model);
const lens_profile_t *lens_profile_get_active(void);
lens_model_t lens_profile_get_active_model(void);
int lens_profile_select(lens_model_t model);

#ifdef __cplusplus
}
#endif

#endif /* __LENS_PROFILE_H__ */
