#include "lens_controller.h"
#include "bsp_ctrl.h"
#include "sys_config.h"
#include <limits.h>

static lens_ircut_mode_t s_ircut_mode = LENS_IRCUT_UNKNOWN;
static uint8_t s_ircut_level;
static uint8_t s_ircut_level_valid;
static uint8_t s_ircut_action_seen;
static uint32_t s_ircut_last_tick;
static osMutexId_t s_controller_mutex;
static uint8_t s_controller_initialized;
static uint8_t s_controller_configured;
static lens_config_mode_t s_last_config_mode = LENS_CONFIG_ALL;
static ms41908m_event_callback_t s_event_callback;

int lens_controller_bootstrap(void)
{
    if (s_controller_mutex == NULL) {
        s_controller_mutex = osMutexNew(NULL);
        if (s_controller_mutex == NULL) {
            return SYS_ERR_NO_MEM;
        }
    }
    return SYS_OK;
}

static int lens_controller_lock(void)
{
    int ret = lens_controller_bootstrap();

    if (ret != SYS_OK) {
        return ret;
    }
    return (osMutexAcquire(s_controller_mutex, osWaitForever) == osOK) ?
           SYS_OK : SYS_ERR_MUTEX;
}

static void lens_controller_unlock(void)
{
    if (s_controller_mutex != NULL) {
        (void)osMutexRelease(s_controller_mutex);
    }
}

static void lens_reset_runtime_state(void)
{
    s_ircut_mode = LENS_IRCUT_UNKNOWN;
    s_ircut_level = 0U;
    s_ircut_level_valid = 0U;
    s_ircut_action_seen = 0U;
    s_ircut_last_tick = 0U;
}

static const lens_axis_profile_t *lens_axis_profile(lens_axis_t axis)
{
    const lens_profile_t *profile = lens_profile_get_active();

    if (axis == LENS_AXIS_ZOOM) {
        return &profile->zoom;
    }
    if (axis == LENS_AXIS_FOCUS) {
        return &profile->focus;
    }
    return NULL;
}

static uint32_t lens_elapsed_ms(uint32_t start, uint32_t now)
{
    uint32_t tick_freq = osKernelGetTickFreq();

    if (tick_freq == 0U) {
        return 0U;
    }
    return (uint32_t)(((uint64_t)(now - start) * 1000ULL) / tick_freq);
}

static int lens_controller_configure_locked(lens_config_mode_t mode)
{
    const lens_profile_t *profile = lens_profile_get_active();
    int ret = SYS_OK;

    if (mode != LENS_CONFIG_ALL && mode != LENS_CONFIG_IRIS && mode != LENS_CONFIG_MOTOR) {
        return SYS_ERR_OUT_OF_RANGE;
    }

    if (mode == LENS_CONFIG_ALL || mode == LENS_CONFIG_IRIS) {
        if (!profile->capabilities.supports_iris || profile->iris_config == NULL) {
            if (mode == LENS_CONFIG_IRIS) {
                return SYS_ERR_NOT_SUPPORTED;
            }
        } else {
            ret = ms41908m_iris_config(profile->iris_config);
            if (ret != SYS_OK || mode == LENS_CONFIG_IRIS) {
                return ret;
            }
        }
    }

    if (mode == LENS_CONFIG_ALL || mode == LENS_CONFIG_MOTOR) {
        if (profile->motor_config == NULL) {
            return SYS_ERR_INVALID_STATE;
        }
        return ms41908m_motor_config(profile->motor_config);
    }

    return ret;
}

int lens_controller_init(void)
{
    int ret = lens_controller_lock();

    if (ret != SYS_OK) {
        return ret;
    }
    if (s_controller_initialized) {
        lens_controller_unlock();
        return SYS_ERR_INVALID_STATE;
    }
    lens_reset_runtime_state();
    ret = ms41908m_init();
    if (ret == SYS_OK) {
        s_controller_initialized = 1U;
        s_controller_configured = 0U;
        ms41908m_set_event_callback(s_event_callback);
    }
    lens_controller_unlock();
    return ret;
}

void lens_controller_deinit(void)
{
    if (lens_controller_lock() != SYS_OK) {
        return;
    }
    if (s_controller_initialized) {
        ms41908m_deinit();
    }
    s_controller_initialized = 0U;
    s_controller_configured = 0U;
    lens_reset_runtime_state();
    lens_controller_unlock();
}

int lens_controller_configure(lens_config_mode_t mode)
{
    int ret = lens_controller_lock();

    if (ret != SYS_OK) {
        return ret;
    }
    if (!s_controller_initialized) {
        lens_controller_unlock();
        return SYS_ERR_INVALID_STATE;
    }
    ret = lens_controller_configure_locked(mode);
    if (ret == SYS_OK) {
        s_controller_configured = 1U;
        s_last_config_mode = mode;
    }
    lens_controller_unlock();
    return ret;
}

void lens_controller_set_event_callback(ms41908m_event_callback_t callback)
{
    if (lens_controller_lock() != SYS_OK) {
        return;
    }
    s_event_callback = callback;
    if (s_controller_initialized) {
        ms41908m_set_event_callback(callback);
    }
    lens_controller_unlock();
}

int lens_controller_select_profile(lens_model_t model)
{
    lens_model_t old_model;
    lens_config_mode_t old_config_mode;
    uint8_t was_configured;
    int ret;
    int rollback_ret = SYS_OK;

    if (lens_profile_get(model) == NULL) {
        return SYS_ERR_INVALID_ARG;
    }
    ret = lens_controller_lock();
    if (ret != SYS_OK) {
        return ret;
    }
    if (lens_profile_get_active_model() == model) {
        lens_controller_unlock();
        return SYS_OK;
    }
    if (!s_controller_initialized) {
        ret = lens_profile_select(model);
        lens_reset_runtime_state();
        if (ret == SYS_OK) {
            WIC_LOGI("[lens] active profile=%s (will apply on lens init)",
                     lens_profile_get_active()->name);
        }
        lens_controller_unlock();
        return ret;
    }
    if (ms41908m_get_zoom_state() == MS41908M_STATE_RUNNING ||
        ms41908m_get_zoom_state() == MS41908M_STATE_RESET_ZERO ||
        ms41908m_get_focus_state() == MS41908M_STATE_RUNNING ||
        ms41908m_get_focus_state() == MS41908M_STATE_RESET_ZERO ||
        ms41908m_iris_get_state() == MS41908M_STATE_RUNNING) {
        lens_controller_unlock();
        return SYS_ERR_BUSY;
    }

    old_model = lens_profile_get_active_model();
    old_config_mode = s_last_config_mode;
    was_configured = s_controller_configured;

    ms41908m_deinit();
    s_controller_initialized = 0U;
    s_controller_configured = 0U;
    ret = lens_profile_select(model);
    if (ret == SYS_OK) {
        ret = ms41908m_init();
    }
    if (ret == SYS_OK) {
        s_controller_initialized = 1U;
        ms41908m_set_event_callback(s_event_callback);
        if (was_configured) {
            ret = lens_controller_configure_locked(old_config_mode);
            if (ret == SYS_OK) {
                s_controller_configured = 1U;
                s_last_config_mode = old_config_mode;
            }
        }
    }
    if (ret != SYS_OK) {
        if (s_controller_initialized) {
            ms41908m_deinit();
            s_controller_initialized = 0U;
        }
        (void)lens_profile_select(old_model);
        rollback_ret = ms41908m_init();
        if (rollback_ret == SYS_OK) {
            s_controller_initialized = 1U;
            ms41908m_set_event_callback(s_event_callback);
            if (was_configured) {
                rollback_ret = lens_controller_configure_locked(old_config_mode);
                if (rollback_ret == SYS_OK) {
                    s_controller_configured = 1U;
                    s_last_config_mode = old_config_mode;
                }
            }
        }
        if (rollback_ret != SYS_OK) {
            WIC_LOGE("[lens] profile rollback failed: model=%d ret=%d",
                     (int)old_model, rollback_ret);
        }
    }
    lens_reset_runtime_state();
    if (ret == SYS_OK) {
        WIC_LOGI("[lens] active profile=%s initialized=%u configured=%u",
                 lens_profile_get_active()->name,
                 (unsigned)s_controller_initialized,
                 (unsigned)s_controller_configured);
    }
    lens_controller_unlock();
    return ret;
}

const lens_profile_t *lens_get_active_profile(void)
{
    return lens_profile_get_active();
}

const lens_capabilities_t *lens_get_capabilities(void)
{
    return &lens_profile_get_active()->capabilities;
}

int lens_convert_relative(lens_axis_t axis, uint16_t physical_pps, int32_t physical_steps,
                          uint16_t *raw_pps, int32_t *raw_psum_units)
{
    const lens_axis_profile_t *axis_profile = lens_axis_profile(axis);
    uint32_t converted_pps;
    int64_t converted_steps;

    if (axis_profile == NULL || raw_pps == NULL || raw_psum_units == NULL) {
        return SYS_ERR_INVALID_ARG;
    }
    if (!lens_profile_get_active()->capabilities.supports_relative) {
        return SYS_ERR_NOT_SUPPORTED;
    }
    if (physical_steps == 0) {
        *raw_pps = 0U;
        *raw_psum_units = 0;
        return SYS_OK;
    }
    if (physical_pps < axis_profile->min_pps || physical_pps > axis_profile->max_pps) {
        return SYS_ERR_OUT_OF_RANGE;
    }

    converted_pps = (uint32_t)physical_pps * axis_profile->psum_units_per_step;
    converted_steps = (int64_t)physical_steps * axis_profile->psum_units_per_step *
                      axis_profile->direction_sign;
    if (converted_pps > UINT16_MAX || converted_pps < MS41908M_RAW_PPS_MIN ||
        converted_pps > MS41908M_RAW_PPS_MAX ||
        converted_steps > (int64_t)MS41908M_RAW_PSUM_MAX ||
        converted_steps < -(int64_t)MS41908M_RAW_PSUM_MAX ||
        converted_steps > INT32_MAX || converted_steps < INT32_MIN) {
        return SYS_ERR_OUT_OF_RANGE;
    }

    *raw_pps = (uint16_t)converted_pps;
    *raw_psum_units = (int32_t)converted_steps;
    return SYS_OK;
}

int lens_zoom_move_relative(uint16_t physical_pps, int32_t physical_steps)
{
    uint16_t raw_pps = 0U;
    int32_t raw_steps = 0;
    int ret = lens_controller_lock();

    if (ret != SYS_OK) return ret;
    ret = lens_convert_relative(LENS_AXIS_ZOOM, physical_pps, physical_steps,
                                &raw_pps, &raw_steps);
    if (ret == SYS_OK && physical_steps != 0) {
        ret = ms41908m_zoom_run(raw_pps, raw_steps);
    }
    WIC_LOGI("[lens] zoom rel profile=%s physical=%u/%ld raw=%u/%ld ret=%d",
             lens_profile_get_active()->name, (unsigned)physical_pps, (long)physical_steps,
             (unsigned)raw_pps, (long)raw_steps, ret);
    lens_controller_unlock();
    return ret;
}

int lens_focus_move_relative(uint16_t physical_pps, int32_t physical_steps)
{
    uint16_t raw_pps = 0U;
    int32_t raw_steps = 0;
    int ret = lens_controller_lock();

    if (ret != SYS_OK) return ret;
    ret = lens_convert_relative(LENS_AXIS_FOCUS, physical_pps, physical_steps,
                                &raw_pps, &raw_steps);
    if (ret == SYS_OK && physical_steps != 0) {
        ret = ms41908m_focus_run(raw_pps, raw_steps);
    }
    WIC_LOGI("[lens] focus rel profile=%s physical=%u/%ld raw=%u/%ld ret=%d",
             lens_profile_get_active()->name, (unsigned)physical_pps, (long)physical_steps,
             (unsigned)raw_pps, (long)raw_steps, ret);
    lens_controller_unlock();
    return ret;
}

int lens_dual_move_relative(uint16_t zoom_pps, int32_t zoom_steps,
                            uint16_t focus_pps, int32_t focus_steps)
{
    uint16_t zoom_raw_pps;
    uint16_t focus_raw_pps;
    int32_t zoom_raw_steps;
    int32_t focus_raw_steps;
    int ret;

    ret = lens_controller_lock();
    if (ret != SYS_OK) {
        return ret;
    }
    if (!lens_profile_get_active()->capabilities.supports_sync_relative) {
        lens_controller_unlock();
        return SYS_ERR_NOT_SUPPORTED;
    }
    ret = lens_convert_relative(LENS_AXIS_ZOOM, zoom_pps, zoom_steps,
                                &zoom_raw_pps, &zoom_raw_steps);
    if (ret != SYS_OK) {
        lens_controller_unlock();
        return ret;
    }
    ret = lens_convert_relative(LENS_AXIS_FOCUS, focus_pps, focus_steps,
                                &focus_raw_pps, &focus_raw_steps);
    if (ret != SYS_OK) {
        lens_controller_unlock();
        return ret;
    }
    if (zoom_steps == 0 && focus_steps == 0) {
        ret = SYS_OK;
    } else {
        ret = ms41908m_zf_sync_run(zoom_raw_pps, zoom_raw_steps,
                                   focus_raw_pps, focus_raw_steps);
    }
    WIC_LOGI("[lens] dual rel profile=%s z=%u/%ld->%u/%ld f=%u/%ld->%u/%ld ret=%d",
             lens_profile_get_active()->name,
             (unsigned)zoom_pps, (long)zoom_steps,
             (unsigned)zoom_raw_pps, (long)zoom_raw_steps,
             (unsigned)focus_pps, (long)focus_steps,
             (unsigned)focus_raw_pps, (long)focus_raw_steps, ret);
    lens_controller_unlock();
    return ret;
}

int lens_zoom_run_raw(uint16_t raw_pps, int32_t raw_psum_units)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    if (!lens_profile_get_active()->capabilities.supports_relative) {
        lens_controller_unlock();
        return SYS_ERR_NOT_SUPPORTED;
    }
    ret = ms41908m_zoom_run(raw_pps, raw_psum_units);
    lens_controller_unlock();
    return ret;
}

int lens_focus_run_raw(uint16_t raw_pps, int32_t raw_psum_units)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    if (!lens_profile_get_active()->capabilities.supports_relative) {
        lens_controller_unlock();
        return SYS_ERR_NOT_SUPPORTED;
    }
    ret = ms41908m_focus_run(raw_pps, raw_psum_units);
    lens_controller_unlock();
    return ret;
}

int lens_dual_run_raw(uint16_t zoom_raw_pps, int32_t zoom_raw_psum_units,
                      uint16_t focus_raw_pps, int32_t focus_raw_psum_units)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    if (!lens_profile_get_active()->capabilities.supports_sync_relative) {
        lens_controller_unlock();
        return SYS_ERR_NOT_SUPPORTED;
    }
    ret = ms41908m_zf_sync_run(zoom_raw_pps, zoom_raw_psum_units,
                               focus_raw_pps, focus_raw_psum_units);
    lens_controller_unlock();
    return ret;
}

int lens_zoom_move_absolute(uint16_t raw_pps, int32_t raw_position)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    if (!lens_profile_get_active()->capabilities.supports_absolute) {
        lens_controller_unlock();
        return SYS_ERR_NOT_SUPPORTED;
    }
    ret = ms41908m_zoom_run_to_position(raw_pps, raw_position);
    lens_controller_unlock();
    return ret;
}

int lens_focus_move_absolute(uint16_t raw_pps, int32_t raw_position)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    if (!lens_profile_get_active()->capabilities.supports_absolute) {
        lens_controller_unlock();
        return SYS_ERR_NOT_SUPPORTED;
    }
    ret = ms41908m_focus_run_to_position(raw_pps, raw_position);
    lens_controller_unlock();
    return ret;
}

int lens_zoom_home(void)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    ret = lens_profile_get_active()->capabilities.supports_home ?
          ms41908m_zoom_reset_zero() : SYS_ERR_NOT_SUPPORTED;
    lens_controller_unlock();
    return ret;
}

int lens_focus_home(void)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    ret = lens_profile_get_active()->capabilities.supports_home ?
          ms41908m_focus_reset_zero() : SYS_ERR_NOT_SUPPORTED;
    lens_controller_unlock();
    return ret;
}

int lens_zoom_set_position_limit(int32_t min_position, int32_t max_position)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    if (!lens_profile_get_active()->capabilities.supports_absolute) {
        lens_controller_unlock();
        return SYS_ERR_NOT_SUPPORTED;
    }
    ret = ms41908m_zoom_set_position_limit(min_position, max_position);
    lens_controller_unlock();
    return ret;
}

int lens_focus_set_position_limit(int32_t min_position, int32_t max_position)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    if (!lens_profile_get_active()->capabilities.supports_absolute) {
        lens_controller_unlock();
        return SYS_ERR_NOT_SUPPORTED;
    }
    ret = ms41908m_focus_set_position_limit(min_position, max_position);
    lens_controller_unlock();
    return ret;
}

int lens_zoom_stop(void)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    ret = ms41908m_zoom_stop();
    lens_controller_unlock();
    return ret;
}
int lens_focus_stop(void)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    ret = ms41908m_focus_stop();
    lens_controller_unlock();
    return ret;
}
int lens_zoom_wait(uint32_t timeout_ms) { return ms41908m_zoom_wait_for_completion(timeout_ms); }
int lens_focus_wait(uint32_t timeout_ms) { return ms41908m_focus_wait_for_completion(timeout_ms); }
int lens_dual_wait(uint32_t timeout_ms) { return ms41908m_zf_sync_wait_for_completion(timeout_ms); }

int lens_zoom_wait_home(uint32_t timeout_ms)
{
    return lens_profile_get_active()->capabilities.supports_home ?
           ms41908m_zoom_wait_reset_done(timeout_ms) : SYS_ERR_NOT_SUPPORTED;
}

int lens_focus_wait_home(uint32_t timeout_ms)
{
    return lens_profile_get_active()->capabilities.supports_home ?
           ms41908m_focus_wait_reset_done(timeout_ms) : SYS_ERR_NOT_SUPPORTED;
}

ms41908m_state_t lens_get_iris_state(void)
{
    return lens_profile_get_active()->capabilities.supports_iris ?
           ms41908m_iris_get_state() : MS41908M_STATE_NO_CFG;
}

ms41908m_state_t lens_get_zoom_state(void) { return ms41908m_get_zoom_state(); }
ms41908m_state_t lens_get_focus_state(void) { return ms41908m_get_focus_state(); }
int lens_get_zoom_position(void) { return ms41908m_get_zoom_position(); }
int lens_get_focus_position(void) { return ms41908m_get_focus_position(); }

int lens_zoom_is_homed(void)
{
    return lens_profile_get_active()->capabilities.supports_home ?
           ms41908m_zoom_is_reset_zero() : 0;
}

int lens_focus_is_homed(void)
{
    return lens_profile_get_active()->capabilities.supports_home ?
           ms41908m_focus_is_reset_zero() : 0;
}

int lens_read_pi_zoom(void)
{
    return lens_profile_get_active()->capabilities.supports_pi ?
           ms41908m_read_pi_zoom() : SYS_ERR_NOT_SUPPORTED;
}

int lens_read_pi_focus(void)
{
    return lens_profile_get_active()->capabilities.supports_pi ?
           ms41908m_read_pi_focus() : SYS_ERR_NOT_SUPPORTED;
}

int lens_iris_run(void)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    ret = lens_profile_get_active()->capabilities.supports_iris ?
          ms41908m_iris_run() : SYS_ERR_NOT_SUPPORTED;
    lens_controller_unlock();
    return ret;
}

int lens_iris_stop(void)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    ret = lens_profile_get_active()->capabilities.supports_iris ?
          ms41908m_iris_stop() : SYS_ERR_NOT_SUPPORTED;
    lens_controller_unlock();
    return ret;
}

int lens_iris_update_target(uint16_t target)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    ret = lens_profile_get_active()->capabilities.supports_iris ?
          ms41908m_iris_update_target(target) : SYS_ERR_NOT_SUPPORTED;
    lens_controller_unlock();
    return ret;
}

int lens_iris_read_adc(uint16_t *adc)
{
    int ret;

    if (adc == NULL) {
        return SYS_ERR_INVALID_ARG;
    }
    ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    if (!lens_profile_get_active()->capabilities.supports_iris) {
        lens_controller_unlock();
        return SYS_ERR_NOT_SUPPORTED;
    }
    *adc = ms41908m_iris_read_adc();
    lens_controller_unlock();
    return SYS_OK;
}

static int lens_ircut_set_raw_level_locked(uint8_t level)
{
    const lens_profile_t *profile = lens_profile_get_active();
    uint32_t now;

    if (!profile->capabilities.supports_ircut) {
        return SYS_ERR_NOT_SUPPORTED;
    }
    level = level ? 1U : 0U;
    if (s_ircut_level_valid && s_ircut_level == level) {
        return SYS_OK;
    }

    now = osKernelGetTickCount();
    if (s_ircut_action_seen && profile->ircut_min_interval_ms > 0U &&
        lens_elapsed_ms(s_ircut_last_tick, now) < profile->ircut_min_interval_ms) {
        return SYS_ERR_BUSY;
    }

    if (bsp_ctrl_set_ir_cut(level) != SYS_OK) {
        return SYS_ERR_HAL;
    }
    s_ircut_level = level;
    s_ircut_level_valid = 1U;
    s_ircut_action_seen = 1U;
    s_ircut_last_tick = now;
    if (level == profile->ircut_day_level) {
        s_ircut_mode = LENS_IRCUT_DAY;
    } else if (level == profile->ircut_night_level) {
        s_ircut_mode = LENS_IRCUT_NIGHT;
    } else {
        s_ircut_mode = LENS_IRCUT_UNKNOWN;
    }
    return SYS_OK;
}

int lens_ircut_set_raw_level(uint8_t level)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    ret = lens_ircut_set_raw_level_locked(level);
    lens_controller_unlock();
    return ret;
}

uint8_t lens_ircut_get_raw_level(void)
{
    return bsp_ctrl_get_ir_cut();
}

int lens_ircut_set_day(void)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    ret = lens_ircut_set_raw_level_locked(lens_profile_get_active()->ircut_day_level);
    lens_controller_unlock();
    return ret;
}

int lens_ircut_set_night(void)
{
    int ret = lens_controller_lock();
    if (ret != SYS_OK) return ret;
    ret = lens_ircut_set_raw_level_locked(lens_profile_get_active()->ircut_night_level);
    lens_controller_unlock();
    return ret;
}

lens_ircut_mode_t lens_ircut_get_mode(void)
{
    return s_ircut_mode;
}
