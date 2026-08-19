#ifndef __LENS_CONTROLLER_H__
#define __LENS_CONTROLLER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "lens_profile.h"

typedef enum {
    LENS_CONFIG_ALL = 0,
    LENS_CONFIG_IRIS = 1,
    LENS_CONFIG_MOTOR = 2,
} lens_config_mode_t;

typedef enum {
    LENS_IRCUT_UNKNOWN = 0,
    LENS_IRCUT_DAY,
    LENS_IRCUT_NIGHT,
} lens_ircut_mode_t;

int lens_controller_init(void);
void lens_controller_deinit(void);
int lens_controller_configure(lens_config_mode_t mode);

const lens_profile_t *lens_get_active_profile(void);
const lens_capabilities_t *lens_get_capabilities(void);

int lens_convert_relative(lens_axis_t axis, uint16_t physical_pps, int32_t physical_steps,
                          uint16_t *raw_pps, int32_t *raw_psum_units);
int lens_zoom_move_relative(uint16_t physical_pps, int32_t physical_steps);
int lens_focus_move_relative(uint16_t physical_pps, int32_t physical_steps);
int lens_dual_move_relative(uint16_t zoom_pps, int32_t zoom_steps,
                            uint16_t focus_pps, int32_t focus_steps);

/* Legacy/raw entry points keep the existing Host Link wire semantics unchanged. */
int lens_zoom_run_raw(uint16_t raw_pps, int32_t raw_psum_units);
int lens_focus_run_raw(uint16_t raw_pps, int32_t raw_psum_units);
int lens_dual_run_raw(uint16_t zoom_raw_pps, int32_t zoom_raw_psum_units,
                      uint16_t focus_raw_pps, int32_t focus_raw_psum_units);

int lens_zoom_move_absolute(uint16_t raw_pps, int32_t raw_position);
int lens_focus_move_absolute(uint16_t raw_pps, int32_t raw_position);
int lens_zoom_home(void);
int lens_focus_home(void);
int lens_zoom_set_position_limit(int32_t min_position, int32_t max_position);
int lens_focus_set_position_limit(int32_t min_position, int32_t max_position);

int lens_zoom_stop(void);
int lens_focus_stop(void);
int lens_zoom_wait(uint32_t timeout_ms);
int lens_focus_wait(uint32_t timeout_ms);
int lens_dual_wait(uint32_t timeout_ms);
int lens_zoom_wait_home(uint32_t timeout_ms);
int lens_focus_wait_home(uint32_t timeout_ms);

ms41908m_state_t lens_get_iris_state(void);
ms41908m_state_t lens_get_zoom_state(void);
ms41908m_state_t lens_get_focus_state(void);
int lens_get_zoom_position(void);
int lens_get_focus_position(void);
int lens_zoom_is_homed(void);
int lens_focus_is_homed(void);
int lens_read_pi_zoom(void);
int lens_read_pi_focus(void);

int lens_iris_run(void);
int lens_iris_stop(void);
int lens_iris_update_target(uint16_t target);
int lens_iris_read_adc(uint16_t *adc);

int lens_ircut_set_raw_level(uint8_t level);
uint8_t lens_ircut_get_raw_level(void);
int lens_ircut_set_day(void);
int lens_ircut_set_night(void);
lens_ircut_mode_t lens_ircut_get_mode(void);

#ifdef __cplusplus
}
#endif

#endif /* __LENS_CONTROLLER_H__ */
