#include "ms41908m.h"
#include <string.h>

/* ============================================================================
 * Macros
 * ============================================================================ */
#define MS41908M_ABS(x)                 (((x) < 0) ? -(x) : (x))
#define MS41908M_MAX(a, b)              (((a) > (b)) ? (a) : (b))

const ms41908m_iris_config_t g_default_iris_config = {
    /* 0x00: IRIS_TGT (0x0000) */
    .iris_tgt      = 0,
    /* 0x01: IRIS_CFG1 (0x608A) */
    .over_lpf_fc_1st = 2,
    .over_lpf_fc_2nd = 2,
    .dec_ave         = 0,
    .as_flt_off      = 0,
    .asound_lpf_fc   = 2,
    .dgain           = 0x30,
    /* 0x02: IRIS_CFG2 (0x66F0) */
    .iris_calc_nr    = 0,
    .iris_round      = 15,
    .pid_zero        = 6,
    .pid_pole        = 6,
    /* 0x03: IRIS_CFG3 (0x0E10) */
    .arw             = 0,
    .lmt_enb         = 1,
    .pwm_flt_off     = 0,
    .pwm_lpf_fc      = 0,
    .pwm_iris        = 7,
    .dt_adj_iris     = 0,
    /* 0x04: HALL_CFG (0x804C) */
    .hall_bias_dac   = 0x4C,
    .hall_offset_dac = 0x80,
    /* 0x05: IRIS_CFG4 (0x0504) */
    .tgt_lpf_fc      = 4,
    .tgt_flt_off     = 0,
    .pid_inv         = 0,
    .hall_gain       = 5,
    .aaf_fc          = 0,
    /* Pulses (not set in AN41908A defaults) */
    .start1          = 0,
    .width1          = 0,
    .p1_en           = 0,
    .start2          = 0,
    .width2          = 0,
    .p2_en           = 0,
    /* 0x0A: IRIS_TEST (0x0080) */
    .tgt_in_test     = 0x80,
    .duty_test       = 0,
    /* 0x0B: MODE_CFG (0x0480) */
    .asw_mode        = 0,
    .test_en1        = 1,
    .mode_sel_iris   = 0,
    .mode_sel_fz     = 0,
    .pdwnb           = 1,
    .adc_test        = 0,
    .pid_clip        = 0,
    /* 0x0E: TGT_UPDATE (0x0C00) */
    .tgt_update      = 0,
    .ave_speed       = 12,
};

const ms41908m_motor_config_t g_default_motor_config = {
    /* 0x20: PWM_CFG (0x5C0A) */
    .dt1       = 0x0A,
    .pwm_mode  = 30,
    .pwm_res   = 2,
    /* 0x21: TEST_CFG (0x0087) */
    .fz_test   = 7,
    .test_en2  = 1,
    /* 0x22: MOTOR_AB_DT2 (Focus, 0x0003) */
    .dt2a      = 3,
    .phmodab   = 0,
    /* 0x23: MOTOR_AB_PPW (Focus, 0xC8C8) */
    .ppwa      = 200,
    .ppwb      = 200,
    /* 0x24: MOTOR_AB_STEP */
    .leda      = 0,
    .microab   = 0,
    /* 0x27: MOTOR_CD_DT2 (Zoom, 0x1603) */
    .dt2b      = 3,
    .phmodcd   = 22,
    /* 0x28: MOTOR_CD_PPW (Zoom, 0xC8C8) */
    .ppwc      = 200,
    .ppwd      = 200,
    /* 0x29: MOTOR_CD_STEP */
    .ledb      = 0,
    .microcd   = 0,
};


/* ============================================================================
 * Private variables
 * ============================================================================ */
static ms41908m_instance_t s_instance = {0};
static void *s_motor_task = NULL;
static void *s_iris_timer = NULL;

/* Async reset-zero state machine (driven only from ms41908m_motor_task_handler) */

typedef enum {
    MS41908M_RZ_IDLE = 0,
    /* PI LOW: fast move out of blocked zone (poll until HIGH); then slow capture */
    MS41908M_RZ_EXIT_LOW_START,
    MS41908M_RZ_EXIT_LOW_WAIT,
    /* PI HIGH (or after exit): slow move until falling-edge IRQ (enter blocked / home) */
    MS41908M_RZ_CAPTURE_START,
    MS41908M_RZ_CAPTURE_WAIT,
} ms41908m_rz_phase_t;

typedef struct {
    ms41908m_rz_phase_t phase;
    ms41908m_type_t type;
    int32_t *position;
    ms41908m_state_t *state_ptr;
    uint8_t *is_r_zero;
    int32_t *pos_min;
    int32_t *pos_max;
    ms41908m_irq_type_t pi_irq;
    uint32_t pi_event;
    uint32_t complete_event;
    uint8_t pi_triggered;
    uint8_t abort_requested;
} ms41908m_reset_sm_t;

static ms41908m_reset_sm_t s_rst;
static int s_zoom_rst_last_result = SYS_OK;
static int s_focus_rst_last_result = SYS_OK;

/* Per-axis batch tracking — ensures VD_FZ is only issued after ALL running axes
 * have acknowledged their previous batch via PLS interrupt.
 *
 * Lifecycle per axis per batch:
 *   [idle / prev batch done]  batch_active=0  pls_rcvd=0
 *   VD_FZ issued           → batch_active=1  pls_rcvd=0   ← cannot fire again
 *   PLS received           → batch_active=1  pls_rcvd=1   ← ready for next VD_FZ
 *   Next VD_FZ issued      → batch_active=1  pls_rcvd=0
 */
#define MS41908M_BATCH_ACTIVE(s)    (s.focus_batch_active || s.zoom_batch_active)
#define MS41908M_BATCH_PENDING(s)   \
    ((s.focus_batch_active && !s.focus_pls_rcvd) || \
     (s.zoom_batch_active  && !s.zoom_pls_rcvd))

static ms41908m_event_callback_t s_app_event_cb = NULL;

#define MS41908M_APP_NOTIFY_CAP 8U
static uint32_t s_app_notify_ev[MS41908M_APP_NOTIFY_CAP];
static int s_app_notify_res[MS41908M_APP_NOTIFY_CAP];
static unsigned s_app_notify_cnt;

/* ============================================================================
 * Private function prototypes
 * ============================================================================ */
static void ms41908m_iris_timer_handler(void *timer);
static void ms41908m_motor_task_handler(void *arg);
static int ms41908m_write_iris_config(const ms41908m_iris_config_t *config);
static int ms41908m_write_motor_config(const ms41908m_motor_config_t *config);
static int ms41908m_motor_start_impl(ms41908m_type_t type, uint16_t pps, int32_t micro_steps);
static int ms41908m_motor_stop_impl(ms41908m_type_t type);
static void ms41908m_reset_sm_complete_ok_locked(void);
static void ms41908m_reset_sm_complete_err_locked(int err);
static void ms41908m_reset_sm_try_arm_zoom_locked(void);
static void ms41908m_reset_sm_try_arm_focus_locked(void);
static void ms41908m_reset_sm_run_locked(void);

static void ms41908m_queue_app_notify(uint32_t event_bits, int result)
{
    if (s_app_notify_cnt < MS41908M_APP_NOTIFY_CAP) {
        s_app_notify_ev[s_app_notify_cnt] = event_bits;
        s_app_notify_res[s_app_notify_cnt] = result;
        s_app_notify_cnt++;
    } else {
        WIC_LOGW("[ms41908m] app notify queue full, event 0x%lx dropped", (unsigned long)event_bits);
    }
}

static void ms41908m_port_set_event_notify(uint32_t event_bits, uint8_t is_from_isr, int result)
{
    ms41908m_port_set_event(event_bits, is_from_isr);
    ms41908m_queue_app_notify(event_bits, result);
}

static void ms41908m_dispatch_pending_app_events(void)
{
    unsigned i;
    ms41908m_event_callback_t cb;

    if (s_app_notify_cnt == 0) {
        return;
    }
    cb = s_app_event_cb;
    if (cb == NULL) {
        s_app_notify_cnt = 0;
        return;
    }
    for (i = 0; i < s_app_notify_cnt; i++) {
        cb(s_app_notify_ev[i], s_app_notify_res[i]);
    }
    s_app_notify_cnt = 0;
}

static uint8_t can_fire_vd_fz(void)
{
    if (MS41908M_BATCH_PENDING(s_instance)) {
        return 0;   /* at least one axis mid-batch; cannot interrupt */
    }
    return 1;       /* all running axes have fresh register values */
}

/* ============================================================================
 * Timer and task handlers
 * ============================================================================ */
static void ms41908m_iris_timer_handler(void *timer)
{
    (void)timer;
    if (s_instance.iris_state == MS41908M_STATE_RUNNING) {
        ms41908m_port_output_vd(MS41908M_TYPE_IRIS);
    }
}

static void ms41908m_motor_task_handler(void *arg)
{
    (void)arg;
    int ret = 0;
    uint16_t step_reg = 0;
    uint32_t event_bits = 0;
    uint32_t wait_mask;

    while (1) {
        wait_mask = MS41908M_EVENT_PLS1 | MS41908M_EVENT_PLS2 |
                    MS41908M_EVENT_REQ_RESET_ZOOM | MS41908M_EVENT_REQ_RESET_FOCUS;
        if (s_rst.phase != MS41908M_RZ_IDLE) {
            wait_mask |= s_rst.pi_event | s_rst.complete_event;
        }

        /*
         * Important:
         * When reset-zero is active, reset SM needs to poll PI/COMPLETE bits immediately after wake.
         * If clear_on_exit=1, outer wait would clear those bits before reset SM can consume them.
         */
        event_bits = ms41908m_port_wait_for_event(wait_mask, 100, 0, 0);

        ret = ms41908m_port_lock();
        if (ret != SYS_OK) {
            continue;
        }

        /* PLS timeout guard: force-stop if motor runs too long without PLS.
         * Only active during normal operation; reset-zero SM has its own timeout. */
        if (s_rst.phase == MS41908M_RZ_IDLE) {
            uint32_t now = osKernelGetTickCount();

            if (s_instance.focus_state == MS41908M_STATE_RUNNING) {
                if ((now - s_instance.focus_start_tick) >= pdMS_TO_TICKS(MS41908M_MOTOR_TIMEOUT_MS)) {
                    WIC_LOGE("[ms41908m] Focus motor timeout (%lu ms), force stop",
                             (unsigned long)MS41908M_MOTOR_TIMEOUT_MS);
                    (void)ms41908m_motor_stop_impl(MS41908M_TYPE_FOCUS);
                    s_instance.focus_state = MS41908M_STATE_ERROR;
                    s_instance.focus_is_r_zero = 0;
                    ms41908m_port_set_event_notify(MS41908M_EVENT_FOCUS_COMPLETED, 0, SYS_ERR_TIMEOUT);
                }
            }

            if (s_instance.zoom_state == MS41908M_STATE_RUNNING) {
                if ((now - s_instance.zoom_start_tick) >= pdMS_TO_TICKS(MS41908M_MOTOR_TIMEOUT_MS)) {
                    WIC_LOGE("[ms41908m] Zoom motor timeout (%lu ms), force stop",
                             (unsigned long)MS41908M_MOTOR_TIMEOUT_MS);
                    (void)ms41908m_motor_stop_impl(MS41908M_TYPE_ZOOM);
                    s_instance.zoom_state = MS41908M_STATE_ERROR;
                    s_instance.zoom_is_r_zero = 0;
                    ms41908m_port_set_event_notify(MS41908M_EVENT_ZOOM_COMPLETED, 0, SYS_ERR_TIMEOUT);
                }
            }
        }

        /* Reset request: arm state machine (zoom_state/focus_state already RESET_ZERO from API) */
        if (event_bits & MS41908M_EVENT_REQ_RESET_ZOOM) {
            ms41908m_reset_sm_try_arm_zoom_locked();
            /* Consume request bit (outer wait no longer clears it) */
            ms41908m_port_clear_event(MS41908M_EVENT_REQ_RESET_ZOOM);
        }
        if (event_bits & MS41908M_EVENT_REQ_RESET_FOCUS) {
            ms41908m_reset_sm_try_arm_focus_locked();
            ms41908m_port_clear_event(MS41908M_EVENT_REQ_RESET_FOCUS);
        }

        if (event_bits & (MS41908M_EVENT_PLS1 | MS41908M_EVENT_PLS2)) {
            /*
             * Phase 1: update per-axis registers and mark pls_rcvd.
             *           Do NOT fire VD_FZ here — we defer to Phase 2 so that
             *           ALL running axes' registers are fresh before the pulse.
             */
            /* Handle Focus (PLS1) */
            if ((event_bits & MS41908M_EVENT_PLS1) && s_instance.focus_state == MS41908M_STATE_RUNNING) {
                s_instance.focus_pls_rcvd = 1;   /* PLS acknowledged */
                if (s_instance.focus_run_config.is_valid) {
                    /* Update focus position */
                    if (s_instance.focus_run_config.direction == 1) {
                        s_instance.focus_position += s_instance.focus_run_config.load_psum;
                    } else {
                        s_instance.focus_position -= s_instance.focus_run_config.load_psum;
                    }

                    /* Check remaining steps */
                    if (s_instance.focus_run_config.unload_psum > 0) {
                        if (s_instance.focus_run_config.unload_psum >= s_instance.focus_run_config.load_psum) {
                            s_instance.focus_run_config.unload_psum -= s_instance.focus_run_config.load_psum;
                        } else {
                            s_instance.focus_run_config.load_psum = s_instance.focus_run_config.unload_psum;
                            s_instance.focus_run_config.unload_psum = 0;
                            ret = ms41908m_port_read(MS41908M_REG_FOCUS_STEP, &step_reg);
                            if (ret == SYS_OK) {
                                step_reg &= 0xFF00;
                                step_reg |= s_instance.focus_run_config.load_psum;
                                ret = ms41908m_port_write(MS41908M_REG_FOCUS_STEP, step_reg);
                            }
                        }

                        if (ret != SYS_OK) {
                            ms41908m_motor_stop_impl(MS41908M_TYPE_FOCUS);
                            s_instance.focus_state = MS41908M_STATE_ERROR;
                            s_instance.focus_is_r_zero = 0;
                            WIC_LOGE("[ms41908m] Focus step update failed");
                        }
                        /* VD_FZ deferred to Phase 2 */
                    } else {
                        /* Focus completed */
                        ms41908m_motor_stop_impl(MS41908M_TYPE_FOCUS);
                        /* stop_impl clears batch tracking; step=0 written,
                         * VD_FZ deferred to Phase 2 if another axis needs it */
                        if (s_rst.phase != MS41908M_RZ_IDLE &&
                            s_rst.type == MS41908M_TYPE_FOCUS &&
                            s_rst.complete_event == MS41908M_EVENT_FOCUS_COMPLETED) {
                            ms41908m_port_set_event(MS41908M_EVENT_FOCUS_COMPLETED, 0);
                        } else {
                            ms41908m_port_set_event_notify(MS41908M_EVENT_FOCUS_COMPLETED, 0, SYS_OK);
                        }
                        WIC_LOGI("[ms41908m] Focus completed");
                    }
                }
            }

            /* Handle Zoom (PLS2) */
            if ((event_bits & MS41908M_EVENT_PLS2) && s_instance.zoom_state == MS41908M_STATE_RUNNING) {
                s_instance.zoom_pls_rcvd = 1;    /* PLS acknowledged */
                if (s_instance.zoom_run_config.is_valid) {
                    /* Update zoom position */
                    if (s_instance.zoom_run_config.direction == 1) {
                        s_instance.zoom_position += s_instance.zoom_run_config.load_psum;
                    } else {
                        s_instance.zoom_position -= s_instance.zoom_run_config.load_psum;
                    }

                    /* Check remaining steps */
                    if (s_instance.zoom_run_config.unload_psum > 0) {
                        if (s_instance.zoom_run_config.unload_psum >= s_instance.zoom_run_config.load_psum) {
                            s_instance.zoom_run_config.unload_psum -= s_instance.zoom_run_config.load_psum;
                        } else {
                            s_instance.zoom_run_config.load_psum = s_instance.zoom_run_config.unload_psum;
                            s_instance.zoom_run_config.unload_psum = 0;
                            ret = ms41908m_port_read(MS41908M_REG_ZOOM_STEP, &step_reg);
                            if (ret == SYS_OK) {
                                step_reg &= 0xFF00;
                                step_reg |= s_instance.zoom_run_config.load_psum;
                                ret = ms41908m_port_write(MS41908M_REG_ZOOM_STEP, step_reg);
                            }
                        }

                        if (ret != SYS_OK) {
                            ms41908m_motor_stop_impl(MS41908M_TYPE_ZOOM);
                            s_instance.zoom_state = MS41908M_STATE_ERROR;
                            s_instance.zoom_is_r_zero = 0;
                            WIC_LOGE("[ms41908m] Zoom step update failed");
                        }
                        /* VD_FZ deferred to Phase 2 */
                    } else {
                        /* Zoom completed */
                        ms41908m_motor_stop_impl(MS41908M_TYPE_ZOOM);
                        /* stop_impl clears batch tracking; step=0 written,
                         * VD_FZ deferred to Phase 2 if another axis needs it */
                        if (s_rst.phase != MS41908M_RZ_IDLE &&
                            s_rst.type == MS41908M_TYPE_ZOOM &&
                            s_rst.complete_event == MS41908M_EVENT_ZOOM_COMPLETED) {
                            ms41908m_port_set_event(MS41908M_EVENT_ZOOM_COMPLETED, 0);
                        } else {
                            ms41908m_port_set_event_notify(MS41908M_EVENT_ZOOM_COMPLETED, 0, SYS_OK);
                        }
                        WIC_LOGI("[ms41908m] Zoom completed");
                    }
                }
            }

            /*
             * Phase 2: fire ONE VD_FZ pulse when ALL running axes are ready.
             * can_fire_vd_fz() returns true only when no running axis has
             * batch_active=1 && pls_rcvd=0 (i.e. mid-batch, can't interrupt).
             */
            if (can_fire_vd_fz()) {
                uint8_t any_axis_running = 0;
                uint32_t now_tick = osKernelGetTickCount();
                if (s_instance.focus_state == MS41908M_STATE_RUNNING) {
                    s_instance.focus_batch_active = 1;
                    s_instance.focus_pls_rcvd    = 0;
                    s_instance.focus_start_tick  = now_tick;
                    any_axis_running = 1;
                }
                if (s_instance.zoom_state == MS41908M_STATE_RUNNING) {
                    s_instance.zoom_batch_active = 1;
                    s_instance.zoom_pls_rcvd    = 0;
                    s_instance.zoom_start_tick   = now_tick;
                    any_axis_running = 1;
                }
                if (any_axis_running) {
                    ms41908m_port_output_vd(MS41908M_TYPE_FOCUS); /* either type: same VD_FZ pin */
                }
            }
        }

        /* Clear PLS bits after we've handled the step update */
        if (event_bits & (MS41908M_EVENT_PLS1 | MS41908M_EVENT_PLS2)) {
            ms41908m_port_clear_event(MS41908M_EVENT_PLS1 | MS41908M_EVENT_PLS2);
        }

        /* After PLS: drive reset SM so COMPLETE / STOPPED are visible same tick */
        if (s_rst.phase != MS41908M_RZ_IDLE) {
            ms41908m_reset_sm_run_locked();
        }

        ms41908m_port_unlock();
        ms41908m_dispatch_pending_app_events();
    }
}

/* ============================================================================
 * Iris register write helper
 * ============================================================================ */
static int ms41908m_write_iris_config(const ms41908m_iris_config_t *config)
{
    int ret = SYS_OK;
    uint16_t reg_val = 0;

    /* 0x00: IRIS_TGT */
    reg_val = config->iris_tgt & MS41908M_IRIS_TGT_MASK;
    ret = ms41908m_port_write(MS41908M_REG_IRIS_TGT, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x01: IRIS_CFG1 */
    reg_val  = (((uint16_t)config->over_lpf_fc_1st  << MS41908M_OVER_LPF_FC_1ST_SHIFT) & MS41908M_OVER_LPF_FC_1ST_MASK);
    reg_val |= (((uint16_t)config->over_lpf_fc_2nd  << MS41908M_OVER_LPF_FC_2ND_SHIFT) & MS41908M_OVER_LPF_FC_2ND_MASK);
    reg_val |= (((uint16_t)config->dec_ave          << MS41908M_DEC_AVE_SHIFT)        & MS41908M_DEC_AVE_MASK);
    reg_val |= (((uint16_t)config->as_flt_off       << MS41908M_AS_FLT_OFF_SHIFT)     & MS41908M_AS_FLT_OFF_MASK);
    reg_val |= (((uint16_t)config->asound_lpf_fc    << MS41908M_ASOUND_LPF_FC_SHIFT)  & MS41908M_ASOUND_LPF_FC_MASK);
    reg_val |= (((uint16_t)config->dgain            << MS41908M_DGAIN_SHIFT)          & MS41908M_DGAIN_MASK);
    ret = ms41908m_port_write(MS41908M_REG_IRIS_CFG1, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x02: IRIS_CFG2 */
    reg_val  = (((uint16_t)config->iris_calc_nr << MS41908M_IRIS_CALC_NR_SHIFT) & MS41908M_IRIS_CALC_NR_MASK);
    reg_val |= (((uint16_t)config->iris_round   << MS41908M_IRIS_ROUND_SHIFT)   & MS41908M_IRIS_ROUND_MASK);
    reg_val |= (((uint16_t)config->pid_zero     << MS41908M_PID_ZERO_SHIFT)     & MS41908M_PID_ZERO_MASK);
    reg_val |= (((uint16_t)config->pid_pole     << MS41908M_PID_POLE_SHIFT)     & MS41908M_PID_POLE_MASK);
    ret = ms41908m_port_write(MS41908M_REG_IRIS_CFG2, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x03: IRIS_CFG3 */
    reg_val  = (((uint16_t)config->arw          << MS41908M_ARW_SHIFT)          & MS41908M_ARW_MASK);
    reg_val |= (((uint16_t)config->lmt_enb      << MS41908M_LMT_ENB_SHIFT)      & MS41908M_LMT_ENB_MASK);
    reg_val |= (((uint16_t)config->pwm_flt_off  << MS41908M_PWM_FLT_OFF_SHIFT)  & MS41908M_PWM_FLT_OFF_MASK);
    reg_val |= (((uint16_t)config->pwm_lpf_fc   << MS41908M_PWM_LPF_FC_SHIFT)   & MS41908M_PWM_LPF_FC_MASK);
    reg_val |= (((uint16_t)config->pwm_iris     << MS41908M_PWM_IRIS_SHIFT)     & MS41908M_PWM_IRIS_MASK);
    reg_val |= (((uint16_t)config->dt_adj_iris  << MS41908M_DT_ADJ_IRIS_SHIFT)  & MS41908M_DT_ADJ_IRIS_MASK);
    ret = ms41908m_port_write(MS41908M_REG_IRIS_CFG3, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x04: HALL_CFG */
    reg_val  = (((uint16_t)config->hall_bias_dac    << MS41908M_HALL_BAIS_DAC_SHIFT)   & MS41908M_HALL_BAIS_DAC_MASK);
    reg_val |= (((uint16_t)config->hall_offset_dac  << MS41908M_HALL_OFFSET_DAC_SHIFT) & MS41908M_HALL_OFFSET_DAC_MASK);
    ret = ms41908m_port_write(MS41908M_REG_HALL_CFG, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x05: IRIS_CFG4 */
    reg_val  = (((uint16_t)config->tgt_lpf_fc  << MS41908M_TGT_LPF_FC_SHIFT)  & MS41908M_TGT_LPF_FC_MASK);
    reg_val |= (((uint16_t)config->tgt_flt_off << MS41908M_TGT_FLT_OFF_SHIFT) & MS41908M_TGT_FLT_OFF_MASK);
    reg_val |= (((uint16_t)config->pid_inv     << MS41908M_PID_INV_SHIFT)     & MS41908M_PID_INV_MASK);
    reg_val |= (((uint16_t)config->hall_gain   << MS41908M_HALL_GAIN_SHIFT)   & MS41908M_HALL_GAIN_MASK);
    reg_val |= (((uint16_t)config->aaf_fc      << MS41908M_AAF_FC_SHIFT)      & MS41908M_AAF_FC_MASK);
    ret = ms41908m_port_write(MS41908M_REG_IRIS_CFG4, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x06: PULSE1_START */
    reg_val = config->start1 & MS41908M_START1_MASK;
    ret = ms41908m_port_write(MS41908M_REG_PULSE1_START, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x07: PULSE1_CFG */
    reg_val = config->width1 & MS41908M_WIDTH1_MASK;
    reg_val |= (config->p1_en & 0x01) << MS41908M_P1EN_SHIFT;
    ret = ms41908m_port_write(MS41908M_REG_PULSE1_CFG, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x08: PULSE2_START */
    reg_val = config->start2 & MS41908M_START2_MASK;
    ret = ms41908m_port_write(MS41908M_REG_PULSE2_START, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x09: PULSE2_CFG */
    reg_val = config->width2 & MS41908M_WIDTH2_MASK;
    reg_val |= (config->p2_en & 0x01) << MS41908M_P2EN_SHIFT;
    ret = ms41908m_port_write(MS41908M_REG_PULSE2_CFG, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x0A: IRIS_TEST */
    reg_val  = (config->tgt_in_test & MS41908M_TGT_IN_TEST_MASK);
    reg_val |= (((uint16_t)config->duty_test << MS41908M_DUTY_TEST_SHIFT) & MS41908M_DUTY_TEST_MASK);
    ret = ms41908m_port_write(MS41908M_REG_IRIS_TEST, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x0B: MODE_CFG */
    reg_val  = (((uint16_t)config->asw_mode      << MS41908M_ASWMODE_SHIFT)      & MS41908M_ASWMODE_MASK);
    // reg_val |= (((uint16_t)config->test_en1      << MS41908M_TEST_EN1_SHIFT)     & MS41908M_TEST_EN1_MASK);
    reg_val |= MS41908M_TEST_EN1_MASK;    // test enable 1 need always be 1
    reg_val |= (((uint16_t)config->mode_sel_iris << MS41908M_MODESEL_IRIS_SHIFT) & MS41908M_MODESEL_IRIS_MASK);
    reg_val |= (((uint16_t)config->mode_sel_fz   << MS41908M_MODESEL_FZ_SHIFT)   & MS41908M_MODESEL_FZ_MASK);
    reg_val |= (((uint16_t)config->pdwnb         << MS41908M_PDWNB_SHIFT)        & MS41908M_PDWNB_MASK);
    reg_val |= (((uint16_t)config->adc_test      << MS41908M_ADC_TEST_SHIFT)     & MS41908M_ADC_TEST_MASK);
    reg_val |= (((uint16_t)config->pid_clip      << MS41908M_PID_CLIP_SHIFT)     & MS41908M_PID_CLIP_MASK);
    ret = ms41908m_port_write(MS41908M_REG_MODE_CFG, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x0E: TGT_UPDATE */
    reg_val  = ((uint16_t)config->tgt_update) & MS41908M_TGT_UPDATE_MASK;
    reg_val |= (((uint16_t)config->ave_speed << MS41908M_AVE_SPEED_SHIFT) & MS41908M_AVE_SPEED_MASK);
    ret = ms41908m_port_write(MS41908M_REG_TGT_UPDATE, reg_val);

    return ret;
}

/* ============================================================================
 * Motor register write helper
 * ============================================================================ */
static int ms41908m_write_motor_config(const ms41908m_motor_config_t *config)
{
    int ret = SYS_OK;
    uint16_t reg_val = 0;

    /* 0x20: PWM_CFG */
    reg_val  = ((uint16_t)config->dt1) & MS41908M_DT1_MASK;
    reg_val |= (((uint16_t)config->pwm_mode << MS41908M_PWMMODE_SHIFT) & MS41908M_PWMMODE_MASK);
    reg_val |= (((uint16_t)config->pwm_res  << MS41908M_PWMRES_SHIFT)  & MS41908M_PWMRES_MASK);
    ret = ms41908m_port_write(MS41908M_REG_PWM_CFG, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x21: TEST_CFG TESTEN2=1, FZTEST=7 (H output during rotation) */
    reg_val = 0x0087;
    // reg_val  = (((uint16_t)config->fz_test  << 0)                       & MS41908M_FZTEST_MASK);
    // reg_val |= (((uint16_t)config->test_en2 << MS41908M_TESTEN2_SHIFT)  & MS41908M_TESTEN2_MASK);
    ret = ms41908m_port_write(MS41908M_REG_TEST_CFG, reg_val);
    if (ret != SYS_OK) return ret;

    // test enable 1 need always be 1
    reg_val = 0;
    ret = ms41908m_port_read(MS41908M_REG_MODE_CFG, &reg_val);
    if (ret != SYS_OK) return ret;
    reg_val |= MS41908M_TEST_EN1_MASK;
    ret = ms41908m_port_write(MS41908M_REG_MODE_CFG, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x22: MOTOR_AB_DT2 (Focus) */
    reg_val  = (((uint16_t)config->dt2a    << 0)                     & MS41908M_DT2A_MASK);
    reg_val |= (((uint16_t)config->phmodab << MS41908M_PHMODAB_SHIFT) & MS41908M_PHMODAB_MASK);
    ret = ms41908m_port_write(MS41908M_REG_MOTOR_AB_DT2, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x23: MOTOR_AB_PPW (Focus) */
    reg_val  = ((uint16_t)config->ppwa) & MS41908M_PPWA_MASK;
    reg_val |= (((uint16_t)config->ppwb << MS41908M_PPWB_SHIFT) & MS41908M_PPWB_MASK);
    ret = ms41908m_port_write(MS41908M_REG_MOTOR_AB_PPW, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x24: MOTOR_AB_STEP (Focus) - Set MICRO and LED from config */
    reg_val = MS41908M_STEP_ENDIS_BIT;
    reg_val |= (config->leda & 0x01) ? MS41908M_STEP_LED_BIT : 0;
    reg_val |= (config->microab & 0x03) << 12;
    ret = ms41908m_port_write(MS41908M_REG_FOCUS_STEP, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x25: MOTOR_AB_PPS (Focus) - Will be set when running */
    ret = ms41908m_port_write(MS41908M_REG_FOCUS_PPS, 0);
    if (ret != SYS_OK) return ret;

    /* 0x27: MOTOR_CD_DT2 (Zoom) */
    reg_val  = (((uint16_t)config->dt2b    << 0)                     & MS41908M_DT2B_MASK);
    reg_val |= (((uint16_t)config->phmodcd << MS41908M_PHMODCD_SHIFT) & MS41908M_PHMODCD_MASK);
    ret = ms41908m_port_write(MS41908M_REG_MOTOR_CD_DT2, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x28: MOTOR_CD_PPW (Zoom) */
    reg_val  = ((uint16_t)config->ppwc) & MS41908M_PPWC_MASK;
    reg_val |= (((uint16_t)config->ppwd << MS41908M_PPWD_SHIFT) & MS41908M_PPWD_MASK);
    ret = ms41908m_port_write(MS41908M_REG_MOTOR_CD_PPW, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x29: MOTOR_CD_STEP (Zoom) - Set MICRO and LED from config */
    reg_val = MS41908M_STEP_ENDIS_BIT;
    reg_val |= (config->ledb & 0x01) ? MS41908M_STEP_LED_BIT : 0;
    reg_val |= (config->microcd & 0x03) << 12;
    ret = ms41908m_port_write(MS41908M_REG_ZOOM_STEP, reg_val);
    if (ret != SYS_OK) return ret;

    /* 0x2A: MOTOR_CD_PPS (Zoom) - Will be set when running */
    ret = ms41908m_port_write(MS41908M_REG_ZOOM_PPS, 0);

    return ret;
}

/* ============================================================================
 * Reset-zero state machine (motor task only, under port_lock)
 * ============================================================================ */

static uint8_t ms41908m_reset_sm_active(void)
{
    return (s_rst.phase != MS41908M_RZ_IDLE) ? 1U : 0U;
}

static void ms41908m_reset_sm_complete_ok_locked(void)
{
    uint32_t done_ev;

    /* Safety: ensure the axis output is stopped before finishing SM */
    if (*s_rst.state_ptr == MS41908M_STATE_RUNNING) {
        (void)ms41908m_motor_stop_impl(s_rst.type);
    }

    *s_rst.position = 0;
    *s_rst.is_r_zero = 1;
    *s_rst.state_ptr = MS41908M_STATE_STOPPED;
    s_rst.phase = MS41908M_RZ_IDLE;
    s_rst.abort_requested = 0;

    /* Consume any pending events */
    ms41908m_port_clear_event(s_rst.pi_event | s_rst.complete_event);

    /* Disable PI IRQ now that reset-zero is done */
    ms41908m_port_pi_irq_disable();

    if (s_rst.type == MS41908M_TYPE_FOCUS) {
        s_focus_rst_last_result = SYS_OK;
        done_ev = MS41908M_EVENT_FOCUS_RESET_DONE;
    } else {
        s_zoom_rst_last_result = SYS_OK;
        done_ev = MS41908M_EVENT_ZOOM_RESET_DONE;
    }

    ms41908m_port_set_event_notify(done_ev, 0, SYS_OK);
    WIC_LOGI("[ms41908m] Reset zero completed");
}

static void ms41908m_reset_sm_complete_err_locked(int err)
{
    uint32_t done_ev;

    /* Safety: ensure the axis output is stopped before finishing SM */
    if (*s_rst.state_ptr == MS41908M_STATE_RUNNING) {
        (void)ms41908m_motor_stop_impl(s_rst.type);
    }

    /* Failure invalidates reset-zero status. */
    *s_rst.is_r_zero = 0;
    *s_rst.state_ptr = MS41908M_STATE_STOPPED;
    s_rst.phase = MS41908M_RZ_IDLE;
    s_rst.abort_requested = 0;

    /* Consume any pending events */
    ms41908m_port_clear_event(s_rst.pi_event | s_rst.complete_event);

    /* Disable PI IRQ now that reset-zero is done */
    ms41908m_port_pi_irq_disable();

    if (s_rst.type == MS41908M_TYPE_FOCUS) {
        s_focus_rst_last_result = err;
        done_ev = MS41908M_EVENT_FOCUS_RESET_DONE;
    } else {
        s_zoom_rst_last_result = err;
        done_ev = MS41908M_EVENT_ZOOM_RESET_DONE;
    }

    ms41908m_port_set_event_notify(done_ev, 0, err);
    if (err == SYS_ERR_NOT_FINISHED) {
        WIC_LOGW("[ms41908m] Reset aborted by user stop");
    } else {
        WIC_LOGE("[ms41908m] Reset zero failed: %d", err);
    }
}

static uint8_t ms41908m_rz_pi_is_high_locked(void)
{
    return (ms41908m_port_read_irq_status(s_rst.pi_irq) != 0U) ? 1U : 0U;
}

/* Steps from zero (PI home edge) toward min(-) limit, plus margin. */
static int32_t ms41908m_rz_span_zero_to_min_locked(void)
{
    int32_t span = -*s_rst.pos_min;

    if (span < 0) {
        span = 0;
    }
    return span + MS41908M_TRAVEL_MARGIN;
}

/* Steps from zero (PI home edge) toward max(+) limit, plus margin. */
static int32_t ms41908m_rz_span_zero_to_max_locked(void)
{
    int32_t span = *s_rst.pos_max;

    if (span < 0) {
        span = 0;
    }
    return span + MS41908M_TRAVEL_MARGIN;
}

/*
 * PI LOW: in shade. Worst case at the far end of the shaded side from zero.
 * micro_steps sign: + = toward max(+), - = toward min(-).
 */
static int32_t ms41908m_rz_steps_to_clear_pi_locked(void)
{
    if (s_rst.type == MS41908M_TYPE_ZOOM) {
        /* shade at min(-): cross PI toward max(+) */
        return ms41908m_rz_span_zero_to_min_locked();
    }
    /* focus shade at max(+): cross PI toward min(-) */
    return -ms41908m_rz_span_zero_to_max_locked();
}

/*
 * From clear (PI HIGH), slow approach into shade until falling-edge IRQ (home).
 * Zoom: clear at max(+) => -steps toward min(-)
 * Focus: clear at min(-) => +steps toward max(+)
 */
static int32_t ms41908m_rz_steps_to_home_edge_locked(void)
{
    if (s_rst.type == MS41908M_TYPE_ZOOM) {
        return -ms41908m_rz_span_zero_to_max_locked();
    }
    return ms41908m_rz_span_zero_to_min_locked();
}

/**
 * Arm reset-zero SM after limits are valid.
 * Step travel uses PI side + zero-centered limits (see ms41908m_rz_steps_to_*).
 */
static void ms41908m_reset_sm_arm_locked(ms41908m_type_t type,
                                         int32_t *pos_min,
                                         int32_t *pos_max,
                                         int32_t *position,
                                         ms41908m_state_t *state_ptr,
                                         uint8_t *is_r_zero,
                                         ms41908m_irq_type_t pi_irq,
                                         uint32_t pi_event,
                                         uint32_t complete_event)
{
    uint8_t pi_high;

    memset(&s_rst, 0, sizeof(s_rst));
    s_rst.type = type;
    s_rst.position = position;
    s_rst.state_ptr = state_ptr;
    s_rst.is_r_zero = is_r_zero;
    s_rst.pos_min = pos_min;
    s_rst.pos_max = pos_max;
    s_rst.pi_irq = pi_irq;
    s_rst.pi_event = pi_event;
    s_rst.complete_event = complete_event;
    *s_rst.is_r_zero = 0;

    ms41908m_port_clear_event(s_rst.pi_event | s_rst.complete_event);

    /* Enable PI (zero-point) IRQ only during reset-zero */
    ms41908m_port_pi_irq_enable();

    pi_high = ms41908m_rz_pi_is_high_locked();
    if (pi_high) {
        s_rst.phase = MS41908M_RZ_CAPTURE_START;
        if (type == MS41908M_TYPE_ZOOM) {
            WIC_LOGI("[ms41908m] Zoom reset: PI=HIGH (clear at max+), pos=%ld, slow capture",
                     (long)*position);
        } else {
            WIC_LOGI("[ms41908m] Focus reset: PI=HIGH (clear at min-), pos=%ld, slow capture",
                     (long)*position);
        }
    } else {
        s_rst.phase = MS41908M_RZ_EXIT_LOW_START;
        if (type == MS41908M_TYPE_ZOOM) {
            WIC_LOGI("[ms41908m] Zoom reset: PI=LOW (shade at min-), pos=%ld, fast exit to clear",
                     (long)*position);
        } else {
            WIC_LOGI("[ms41908m] Focus reset: PI=LOW (shade at max+), pos=%ld, fast exit to clear",
                     (long)*position);
        }
    }
}

static void ms41908m_reset_sm_try_arm_zoom_locked(void)
{
    int32_t *pos_min = &s_instance.zoom_pos_min;
    int32_t *pos_max = &s_instance.zoom_pos_max;

    if (s_rst.phase != MS41908M_RZ_IDLE) {
        return;
    }
    if (s_instance.zoom_state != MS41908M_STATE_RESET_ZERO) {
        return;
    }

    if (*pos_max <= *pos_min || *pos_min > 0 || *pos_max < 0) {
        WIC_LOGE("[ms41908m] Reset zero failed: invalid limit config (min=%ld, max=%ld)",
                 (long)*pos_min, (long)*pos_max);
        s_instance.zoom_state = MS41908M_STATE_STOPPED;
        s_zoom_rst_last_result = SYS_ERR_INVALID_STATE;
        ms41908m_port_set_event_notify(MS41908M_EVENT_ZOOM_RESET_DONE, 0, SYS_ERR_INVALID_STATE);
        return;
    }

    ms41908m_reset_sm_arm_locked(MS41908M_TYPE_ZOOM,
                                 pos_min,
                                 pos_max,
                                 &s_instance.zoom_position,
                                 &s_instance.zoom_state,
                                 &s_instance.zoom_is_r_zero,
                                 MS41908M_IRQ_PI_ZOOM,
                                 MS41908M_EVENT_PI_ZOOM,
                                 MS41908M_EVENT_ZOOM_COMPLETED);
}

static void ms41908m_reset_sm_try_arm_focus_locked(void)
{
    int32_t *pos_min = &s_instance.focus_pos_min;
    int32_t *pos_max = &s_instance.focus_pos_max;

    if (s_rst.phase != MS41908M_RZ_IDLE) {
        return;
    }
    if (s_instance.focus_state != MS41908M_STATE_RESET_ZERO) {
        return;
    }

    if (*pos_max <= *pos_min || *pos_min > 0 || *pos_max < 0) {
        WIC_LOGE("[ms41908m] Reset zero failed: invalid limit config (min=%ld, max=%ld)",
                 (long)*pos_min, (long)*pos_max);
        s_instance.focus_state = MS41908M_STATE_STOPPED;
        s_focus_rst_last_result = SYS_ERR_INVALID_STATE;
        ms41908m_port_set_event_notify(MS41908M_EVENT_FOCUS_RESET_DONE, 0, SYS_ERR_INVALID_STATE);
        return;
    }

    ms41908m_reset_sm_arm_locked(MS41908M_TYPE_FOCUS,
                                 pos_min,
                                 pos_max,
                                 &s_instance.focus_position,
                                 &s_instance.focus_state,
                                 &s_instance.focus_is_r_zero,
                                 MS41908M_IRQ_PI_FOCUS,
                                 MS41908M_EVENT_PI_FOCUS,
                                 MS41908M_EVENT_FOCUS_COMPLETED);
}

static void ms41908m_reset_sm_step_once_locked(void)
{
    int ret;
    uint32_t ev;

    int32_t exit_travel;
    int32_t capture_travel;

    switch (s_rst.phase) {
    case MS41908M_RZ_IDLE:
        break;

    case MS41908M_RZ_EXIT_LOW_START:
        if (s_rst.abort_requested) {
            ms41908m_reset_sm_complete_err_locked(SYS_ERR_NOT_FINISHED);
            break;
        }
        if (ms41908m_rz_pi_is_high_locked()) {
            s_rst.phase = MS41908M_RZ_CAPTURE_START;
            break;
        }
        exit_travel = ms41908m_rz_steps_to_clear_pi_locked();
        ret = ms41908m_motor_start_impl(s_rst.type,
                                        MS41908M_RESET_FAST_PPS,
                                        exit_travel);
        WIC_LOGI("[ms41908m] %s fast exit to PI=HIGH: micro_steps=%ld",
                 (s_rst.type == MS41908M_TYPE_ZOOM) ? "Zoom" : "Focus",
                 (long)exit_travel);
        if (ret != SYS_OK) {
            ms41908m_reset_sm_complete_err_locked(ret);
            break;
        }
        s_rst.phase = MS41908M_RZ_EXIT_LOW_WAIT;
        break;

    case MS41908M_RZ_EXIT_LOW_WAIT:
        if (s_rst.abort_requested) {
            ms41908m_reset_sm_complete_err_locked(SYS_ERR_NOT_FINISHED);
            break;
        }
        if (ms41908m_rz_pi_is_high_locked()) {
            (void)ms41908m_motor_stop_impl(s_rst.type);
            ms41908m_port_clear_event(s_rst.pi_event | s_rst.complete_event);
            WIC_LOGI("[ms41908m] PI released to HIGH during fast exit");
            s_rst.phase = MS41908M_RZ_CAPTURE_START;
            break;
        }
        ev = ms41908m_port_wait_for_event(s_rst.complete_event, 0, 0, 0);
        if (*s_rst.state_ptr == MS41908M_STATE_RUNNING) {
            if (ev & s_rst.complete_event) {
                ms41908m_port_clear_event(s_rst.complete_event);
            }
            break;
        }
        if (ev & s_rst.complete_event) {
            ms41908m_port_clear_event(s_rst.complete_event);
        }
        if (ms41908m_rz_pi_is_high_locked()) {
            WIC_LOGI("[ms41908m] PI released to HIGH at end of fast exit");
            s_rst.phase = MS41908M_RZ_CAPTURE_START;
        } else {
            WIC_LOGE("[ms41908m] Reset zero failed: PI still LOW after fast exit");
            ms41908m_reset_sm_complete_err_locked(SYS_ERR_TIMEOUT);
        }
        break;

    case MS41908M_RZ_CAPTURE_START:
        if (s_rst.abort_requested) {
            ms41908m_reset_sm_complete_err_locked(SYS_ERR_NOT_FINISHED);
            break;
        }
        ms41908m_port_clear_event(s_rst.pi_event | s_rst.complete_event);
        capture_travel = ms41908m_rz_steps_to_home_edge_locked();
        ret = ms41908m_motor_start_impl(s_rst.type, MS41908M_RESET_SLOW_PPS, capture_travel);
        WIC_LOGI("[ms41908m] %s slow capture (home edge): micro_steps=%ld",
                 (s_rst.type == MS41908M_TYPE_ZOOM) ? "Zoom" : "Focus",
                 (long)capture_travel);
        if (ret != SYS_OK) {
            ms41908m_reset_sm_complete_err_locked(ret);
            break;
        }
        s_rst.phase = MS41908M_RZ_CAPTURE_WAIT;
        break;

    case MS41908M_RZ_CAPTURE_WAIT:
        if (s_rst.abort_requested) {
            ms41908m_reset_sm_complete_err_locked(SYS_ERR_NOT_FINISHED);
            break;
        }
        ev = ms41908m_port_wait_for_event(s_rst.pi_event | s_rst.complete_event, 0, 0, 0);
        if (*s_rst.state_ptr == MS41908M_STATE_RUNNING) {
            if (ev & s_rst.pi_event) {
                (void)ms41908m_motor_stop_impl(s_rst.type);
                s_rst.pi_triggered = 1;
                ms41908m_port_clear_event(s_rst.pi_event | s_rst.complete_event);
                WIC_LOGI("[ms41908m] PI falling edge during slow capture");
                ms41908m_reset_sm_complete_ok_locked();
            } else if (ev & s_rst.complete_event) {
                ms41908m_port_clear_event(s_rst.complete_event);
            }
        } else {
            if (ev & s_rst.pi_event) {
                (void)ms41908m_motor_stop_impl(s_rst.type);
                s_rst.pi_triggered = 1;
                ms41908m_port_clear_event(s_rst.pi_event | s_rst.complete_event);
                WIC_LOGI("[ms41908m] PI falling edge during slow capture");
                ms41908m_reset_sm_complete_ok_locked();
            } else if (ev & s_rst.complete_event) {
                ms41908m_port_clear_event(s_rst.complete_event);
                if (!s_rst.pi_triggered) {
                    WIC_LOGE("[ms41908m] Reset zero failed: PI never triggered on slow capture");
                    ms41908m_reset_sm_complete_err_locked(SYS_ERR_TIMEOUT);
                }
            }
        }
        break;

    default:
        s_rst.phase = MS41908M_RZ_IDLE;
        break;
    }
}

static void ms41908m_reset_sm_run_locked(void)
{
    unsigned guard = 0;

    while (s_rst.phase != MS41908M_RZ_IDLE && guard++ < 32U) {
        ms41908m_reset_sm_step_once_locked();
    }
}

/* ============================================================================
 * Motor start/stop (SPI + state) — caller must hold ms41908m_port_lock()
 * ============================================================================ */
static int ms41908m_motor_start_impl(ms41908m_type_t type, uint16_t pps, int32_t micro_steps)
{
    int ret = SYS_OK;
    uint16_t step_reg = 0;
    uint16_t pps_reg = 0;
    uint8_t step_addr = 0;
    uint8_t pps_addr = 0;
    ms41908m_run_config_t *run_config = NULL;
    ms41908m_state_t *state = NULL;
    int32_t *position = NULL;
    uint8_t *is_r_zero = NULL;
    int32_t *pos_min = NULL;
    int32_t *pos_max = NULL;
    if (pps < MS41908M_PPS_MIN || pps > MS41908M_PPS_MAX) {
        return SYS_ERR_INVALID_ARG;
    }
    if (MS41908M_ABS(micro_steps) < MS41908M_MSTEPS_MIN || MS41908M_ABS(micro_steps) > MS41908M_MSTEPS_MAX) {
        return SYS_ERR_INVALID_ARG;
    }

    if (type == MS41908M_TYPE_FOCUS) {
        step_addr = MS41908M_REG_FOCUS_STEP;
        pps_addr = MS41908M_REG_FOCUS_PPS;
        run_config = &s_instance.focus_run_config;
        state = &s_instance.focus_state;
        position = &s_instance.focus_position;
        is_r_zero = &s_instance.focus_is_r_zero;
        pos_min = &s_instance.focus_pos_min;
        pos_max = &s_instance.focus_pos_max;
    } else if (type == MS41908M_TYPE_ZOOM) {
        step_addr = MS41908M_REG_ZOOM_STEP;
        pps_addr = MS41908M_REG_ZOOM_PPS;
        run_config = &s_instance.zoom_run_config;
        state = &s_instance.zoom_state;
        position = &s_instance.zoom_position;
        is_r_zero = &s_instance.zoom_is_r_zero;
        pos_min = &s_instance.zoom_pos_min;
        pos_max = &s_instance.zoom_pos_max;
    } else {
        return SYS_ERR_INVALID_ARG;
    }

    /* Must have called ms41908m_motor_config() first */
    if (*state == MS41908M_STATE_NO_CFG) {
        return SYS_ERR_INVALID_STATE;
    }

    /* Check if motor is already running */
    if (*state == MS41908M_STATE_RUNNING) {
        return SYS_ERR_BUSY;
    }

    /* Allow start from STOPPED or RESET_ZERO (motor task reset SM uses this path) */
    if (*state != MS41908M_STATE_STOPPED && *state != MS41908M_STATE_RESET_ZERO) {
        return SYS_ERR_INVALID_STATE;
    }

    /* Check position limit if enabled (is_r_zero && pos_max > pos_min) */
    if (*is_r_zero && *pos_max > *pos_min) {
        int32_t target_pos = *position + micro_steps;
        if (target_pos < *pos_min || target_pos > *pos_max) {
            WIC_LOGW("[ms41908m] Position limit exceeded: target=%ld, min=%ld, max=%ld",
                     (long)target_pos, (long)*pos_min, (long)*pos_max);
            return SYS_ERR_OUT_OF_RANGE;
        }
    }

    /* Calculate VD parameters */
    uint16_t intct = MS41908M_CALC_INTCT_FROM_PPS(pps);
    if (intct > 65535) intct = 65535;
    else if (intct < 1) intct = 1;
    uint16_t psum = MS41908M_CALC_PSUM_FROM_INTCT(intct);
    if (psum > 255) psum = 255;
    else if (psum < 1) psum = 1;
    if (psum > MS41908M_ABS(micro_steps)) {
        psum = MS41908M_ABS(micro_steps);
    }
    
    /* micro_steps sign: + toward max(+), - toward min(-); zoom/focus share this rule */
    run_config->direction = (micro_steps >= 0) ? 1 : 0;
    run_config->load_psum = psum;
    run_config->unload_psum = MS41908M_ABS(micro_steps) - psum;
    run_config->intct = intct;
    run_config->is_valid = 1;

    /* Read current step register to preserve MICRO and LED settings */
    ret = ms41908m_port_read(step_addr, &step_reg);
    if (ret != SYS_OK) {
        run_config->is_valid = 0;
        return ret;
    }

    /* Modify only PSUM, direction, and enable bits, keep MICRO/LED/BRAKE from config */
    step_reg &= (MS41908M_STEP_MICRO_MASK | MS41908M_STEP_LED_BIT);  /* Keep MICRO and LED */
    step_reg |= MS41908M_STEP_ENDIS_BIT;                             /* Enable output */
    step_reg |= (psum & 0xFF);                                       /* Set PSUM */
    if (run_config->direction == 0) {
        step_reg |= MS41908M_STEP_CCWCW_BIT;  /* CCW direction */
    }

    /* Write PPS register */
    pps_reg = intct & 0xFFFF;
    ret = ms41908m_port_write(pps_addr, pps_reg);
    if (ret != SYS_OK) {
        run_config->is_valid = 0;
        return ret;
    }

    /* Write step register */
    ret = ms41908m_port_write(step_addr, step_reg);
    if (ret != SYS_OK) {
        run_config->is_valid = 0;
        return ret;
    }

    /* Clear completion event */
    if (type == MS41908M_TYPE_FOCUS) {
        ms41908m_port_clear_event(MS41908M_EVENT_FOCUS_COMPLETED);
    } else {
        ms41908m_port_clear_event(MS41908M_EVENT_ZOOM_COMPLETED);
    }

    /* Set state and trigger VD. Skip VD_FZ if another axis is mid-batch —
     * that axis's next PLS will fire VD_FZ, picking up our fresh register. */
    *state = MS41908M_STATE_RUNNING;
    /* Record start tick for PLS timeout detection */
    if (type == MS41908M_TYPE_FOCUS) {
        s_instance.focus_start_tick = osKernelGetTickCount();
    } else {
        s_instance.zoom_start_tick = osKernelGetTickCount();
    }

    /* Mark this axis as having a pending batch (registers written, VD not yet issued) */
    if (type == MS41908M_TYPE_FOCUS) {
        s_instance.focus_batch_active = 0;
        s_instance.focus_pls_rcvd    = 0;
    } else {
        s_instance.zoom_batch_active  = 0;
        s_instance.zoom_pls_rcvd     = 0;
    }

    if (can_fire_vd_fz()) {
        /* Mark batch active for all running axes */
        if (s_instance.focus_state == MS41908M_STATE_RUNNING) {
            s_instance.focus_batch_active = 1;
            s_instance.focus_pls_rcvd    = 0;
            s_instance.focus_start_tick  = osKernelGetTickCount();
        }
        if (s_instance.zoom_state == MS41908M_STATE_RUNNING) {
            s_instance.zoom_batch_active = 1;
            s_instance.zoom_pls_rcvd    = 0;
            s_instance.zoom_start_tick   = osKernelGetTickCount();
        }
        ms41908m_port_output_vd(type);
    }

    WIC_LOGI("[ms41908m] Motor started: type=%d, intct=%d, load_psum=%c%d, unload_psum=%c%d",
             type, intct, run_config->direction == 1 ? '+' : '-', run_config->load_psum,
             run_config->direction == 1 ? '+' : '-', run_config->unload_psum);

    return SYS_OK;
}

static int ms41908m_motor_stop_impl(ms41908m_type_t type)
{
    int ret = SYS_OK;
    uint16_t step_reg = 0;
    uint8_t step_addr = 0;
    ms41908m_run_config_t *run_config = NULL;
    ms41908m_state_t *state = NULL;

    if (type == MS41908M_TYPE_FOCUS) {
        step_addr = MS41908M_REG_FOCUS_STEP;
        run_config = &s_instance.focus_run_config;
        state = &s_instance.focus_state;
    } else if (type == MS41908M_TYPE_ZOOM) {
        step_addr = MS41908M_REG_ZOOM_STEP;
        run_config = &s_instance.zoom_run_config;
        state = &s_instance.zoom_state;
    } else {
        return SYS_ERR_INVALID_ARG;
    }

    if (*state == MS41908M_STATE_NO_CFG) {
        return SYS_OK;
    }

    /* Read current step register */
    ret = ms41908m_port_read(step_addr, &step_reg);
    if (ret != SYS_OK) return ret;

    /* Clear PSUM to stop motor */
    step_reg &= 0xFF00;
    ret = ms41908m_port_write(step_addr, step_reg);
    if (ret != SYS_OK) return ret;

    /* Clear batch tracking for the stopped axis */
    if (type == MS41908M_TYPE_FOCUS) {
        s_instance.focus_batch_active = 0;
        s_instance.focus_pls_rcvd    = 0;
    } else {
        s_instance.zoom_batch_active  = 0;
        s_instance.zoom_pls_rcvd     = 0;
    }

    /* Update state before checking can_fire_vd_fz so the axis is excluded */
    run_config->is_valid = 0;
    run_config->unload_psum = 0;
    if (s_rst.phase != MS41908M_RZ_IDLE && s_rst.type == type) {
        *state = MS41908M_STATE_RESET_ZERO;
    } else {
        *state = MS41908M_STATE_STOPPED;
    }

    /* Trigger VD to apply step=0. Skip if another axis is mid-batch —
     * its next PLS will fire VD_FZ and pick up our PSUM=0. */
    if (can_fire_vd_fz()) {
        if (s_instance.focus_state == MS41908M_STATE_RUNNING) {
            s_instance.focus_batch_active = 1;
            s_instance.focus_pls_rcvd    = 0;
            s_instance.focus_start_tick  = osKernelGetTickCount();
        }
        if (s_instance.zoom_state == MS41908M_STATE_RUNNING) {
            s_instance.zoom_batch_active = 1;
            s_instance.zoom_pls_rcvd    = 0;
            s_instance.zoom_start_tick   = osKernelGetTickCount();
        }
        ms41908m_port_output_vd(type);
    }

    return SYS_OK;
}

/* ============================================================================
 * Public API: Initialization
 * ============================================================================ */
int ms41908m_init(void)
{
    int ret = SYS_OK;

    if (s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    memset(&s_instance, 0, sizeof(s_instance));
    memset(&s_rst, 0, sizeof(s_rst));
    s_zoom_rst_last_result = SYS_ERR_INVALID_STATE;
    s_focus_rst_last_result = SYS_ERR_INVALID_STATE;
    s_instance.iris_state  = MS41908M_STATE_NO_CFG;
    s_instance.zoom_state  = MS41908M_STATE_NO_CFG;
    s_instance.focus_state = MS41908M_STATE_NO_CFG;

    /* Initialize port layer */
    ret = ms41908m_port_init();
    if (ret != SYS_OK) {
        WIC_LOGE("[ms41908m] Port init failed: %d", ret);
        return ret;
    }

    /* Create motor task */
    s_motor_task = ms41908m_port_create_task(ms41908m_motor_task_handler, NULL);
    if (s_motor_task == NULL) {
        WIC_LOGE("[ms41908m] Failed to create motor task");
        ms41908m_port_deinit();
        return SYS_ERR_NO_MEM;
    }

    /* Create iris timer (VD period ~62ms) */
    s_iris_timer = ms41908m_port_create_timer(ms41908m_iris_timer_handler, MS41908M_VD_DELAY_MS);
    if (s_iris_timer == NULL) {
        WIC_LOGE("[ms41908m] Failed to create iris timer");
        ms41908m_port_delete_task(s_motor_task);
        s_motor_task = NULL;
        ms41908m_port_deinit();
        return SYS_ERR_NO_MEM;
    }

    s_instance.is_initialized = 1;
    WIC_LOGI("[ms41908m] Initialized");

    return SYS_OK;
}

void ms41908m_deinit(void)
{
    if (!s_instance.is_initialized) {
        return;
    }

    /* Stop all motors */
    ms41908m_iris_stop();
    ms41908m_focus_stop();
    ms41908m_zoom_stop();

    /* Delete timer */
    if (s_iris_timer != NULL) {
        ms41908m_port_stop_timer(s_iris_timer);
        ms41908m_port_delete_timer(s_iris_timer);
        s_iris_timer = NULL;
    }

    /* Delete task */
    if (s_motor_task != NULL) {
        ms41908m_port_delete_task(s_motor_task);
        s_motor_task = NULL;
    }

    /* Deinit port */
    ms41908m_port_deinit();

    s_app_event_cb = NULL;
    s_app_notify_cnt = 0;

    memset(&s_instance, 0, sizeof(s_instance));
    memset(&s_rst, 0, sizeof(s_rst));
    WIC_LOGI("[ms41908m] Deinitialized");
}

void ms41908m_set_event_callback(ms41908m_event_callback_t callback)
{
    s_app_event_cb = callback;
}

/* ============================================================================
 * Public API: Iris control
 * ============================================================================ */
int ms41908m_iris_config(const ms41908m_iris_config_t *config)
{
    int ret = SYS_OK;

    if (!s_instance.is_initialized || config == NULL) {
        return SYS_ERR_INVALID_ARG;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) return ret;

    ret = ms41908m_write_iris_config(config);
    if (ret == SYS_OK) {
        if (s_instance.iris_state == MS41908M_STATE_NO_CFG ||
            s_instance.iris_state == MS41908M_STATE_ERROR) {
            s_instance.iris_state = MS41908M_STATE_STOPPED;
        }
    }
    ms41908m_port_output_vd(MS41908M_TYPE_IRIS);

    ms41908m_port_unlock();
    return ret;
}

int ms41908m_iris_update_target(uint16_t target)
{
    int ret = SYS_OK;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }
    if (s_instance.iris_state == MS41908M_STATE_NO_CFG) {
        return SYS_ERR_INVALID_STATE;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) return ret;

    ret = ms41908m_port_write(MS41908M_REG_IRIS_TGT, target & MS41908M_IRIS_TGT_MASK);

    ms41908m_port_unlock();
    return ret;
}

uint16_t ms41908m_iris_read_adc(void)
{
    uint16_t adc_val = 0;

    if (!s_instance.is_initialized) {
        return 0;
    }

    if (ms41908m_port_lock() != SYS_OK) {
        return 0;
    }

    ms41908m_port_read(MS41908M_REG_IRSAD, &adc_val);

    ms41908m_port_unlock();
    return adc_val & 0x03FF;
}

ms41908m_state_t ms41908m_iris_get_state(void)
{
    return s_instance.iris_state;
}

int ms41908m_iris_run(void)
{
    int ret = SYS_OK;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }
    if (s_instance.iris_state == MS41908M_STATE_NO_CFG) {
        return SYS_ERR_INVALID_STATE;
    }

    if (s_instance.iris_state == MS41908M_STATE_RUNNING) {
        return SYS_OK;
    }

    ret = ms41908m_port_start_timer(s_iris_timer);
    if (ret == SYS_OK) {
        s_instance.iris_state = MS41908M_STATE_RUNNING;
        ms41908m_port_output_vd(MS41908M_TYPE_IRIS);
    }

    return ret;
}

int ms41908m_iris_stop(void)
{
    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }
    if (s_instance.iris_state == MS41908M_STATE_NO_CFG) {
        return SYS_OK;
    }

    ms41908m_port_stop_timer(s_iris_timer);
    s_instance.iris_state = MS41908M_STATE_STOPPED;

    return SYS_OK;
}

/* ============================================================================
 * Public API: Motor configuration
 * ============================================================================ */
int ms41908m_motor_config(const ms41908m_motor_config_t *config)
{
    int ret = SYS_OK;

    if (!s_instance.is_initialized || config == NULL) {
        return SYS_ERR_INVALID_ARG;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) return ret;

    if (ms41908m_reset_sm_active()) {
        ms41908m_port_unlock();
        return SYS_ERR_BUSY;
    }

    ret = ms41908m_write_motor_config(config);
    if (ret == SYS_OK) {
        if (s_instance.zoom_state == MS41908M_STATE_NO_CFG ||
            s_instance.zoom_state == MS41908M_STATE_ERROR) {
            s_instance.zoom_state = MS41908M_STATE_STOPPED;
        }
        if (s_instance.focus_state == MS41908M_STATE_NO_CFG ||
            s_instance.focus_state == MS41908M_STATE_ERROR) {
            s_instance.focus_state = MS41908M_STATE_STOPPED;
        }
    }

    ms41908m_port_unlock();
    return ret;
}

/* ============================================================================
 * Public API: Motor state and position
 * ============================================================================ */
ms41908m_state_t ms41908m_get_zoom_state(void)
{
    return s_instance.zoom_state;
}

ms41908m_state_t ms41908m_get_focus_state(void)
{
    return s_instance.focus_state;
}

int ms41908m_get_zoom_position(void)
{
    return s_instance.zoom_position;
}

int ms41908m_get_focus_position(void)
{
    return s_instance.focus_position;
}

/* ============================================================================
 * Public API: Motor run
 * ============================================================================ */
int ms41908m_zoom_run(uint16_t pps, int32_t micro_steps)
{
    int ret = SYS_OK;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    if (micro_steps == 0) {
        return SYS_OK;
    }

    if (s_instance.zoom_state == MS41908M_STATE_NO_CFG) {
        return SYS_ERR_INVALID_STATE;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) return ret;

    /* Under lock: allow concurrent zoom+focus — only check self + reset SM */
    if (s_instance.zoom_state == MS41908M_STATE_RUNNING ||
        s_instance.zoom_state == MS41908M_STATE_RESET_ZERO ||
        ms41908m_reset_sm_active()) {
        ms41908m_port_unlock();
        return SYS_ERR_BUSY;
    }

    ret = ms41908m_motor_start_impl(MS41908M_TYPE_ZOOM, pps, micro_steps);

    ms41908m_port_unlock();
    return ret;
}

int ms41908m_focus_run(uint16_t pps, int32_t micro_steps)
{
    int ret = SYS_OK;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    if (micro_steps == 0) {
        return SYS_OK;
    }

    if (s_instance.focus_state == MS41908M_STATE_NO_CFG) {
        return SYS_ERR_INVALID_STATE;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) return ret;

    if (s_instance.focus_state == MS41908M_STATE_RUNNING ||
        s_instance.focus_state == MS41908M_STATE_RESET_ZERO ||
        ms41908m_reset_sm_active()) {
        ms41908m_port_unlock();
        return SYS_ERR_BUSY;
    }

    ret = ms41908m_motor_start_impl(MS41908M_TYPE_FOCUS, pps, micro_steps);

    ms41908m_port_unlock();
    return ret;
}

int ms41908m_zoom_run_to_position(uint16_t pps, int32_t position)
{
    int ret;
    int32_t delta;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }
    if (s_instance.zoom_state == MS41908M_STATE_NO_CFG) {
        return SYS_ERR_INVALID_STATE;
    }
    if (!s_instance.zoom_is_r_zero) {
        return SYS_ERR_INVALID_STATE;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) {
        return ret;
    }

    if (s_instance.zoom_state == MS41908M_STATE_RUNNING ||
        s_instance.zoom_state == MS41908M_STATE_RESET_ZERO ||
        ms41908m_reset_sm_active()) {
        ms41908m_port_unlock();
        return SYS_ERR_BUSY;
    }

    delta = position - s_instance.zoom_position;
    if (delta == 0) {
        ms41908m_port_unlock();
        return SYS_OK;
    }

    ret = ms41908m_motor_start_impl(MS41908M_TYPE_ZOOM, pps, delta);
    ms41908m_port_unlock();
    return ret;
}

int ms41908m_focus_run_to_position(uint16_t pps, int32_t position)
{
    int ret;
    int32_t delta;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }
    if (s_instance.focus_state == MS41908M_STATE_NO_CFG) {
        return SYS_ERR_INVALID_STATE;
    }
    if (!s_instance.focus_is_r_zero) {
        return SYS_ERR_INVALID_STATE;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) {
        return ret;
    }

    if (s_instance.focus_state == MS41908M_STATE_RUNNING ||
        s_instance.focus_state == MS41908M_STATE_RESET_ZERO ||
        ms41908m_reset_sm_active()) {
        ms41908m_port_unlock();
        return SYS_ERR_BUSY;
    }

    delta = position - s_instance.focus_position;
    if (delta == 0) {
        ms41908m_port_unlock();
        return SYS_OK;
    }

    ret = ms41908m_motor_start_impl(MS41908M_TYPE_FOCUS, pps, delta);
    ms41908m_port_unlock();
    return ret;
}

/* ============================================================================
 * Public API: Motor reset zero
 * ============================================================================ */
int ms41908m_zoom_reset_zero(void)
{
    int ret;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    if (s_instance.zoom_state == MS41908M_STATE_NO_CFG) {
        return SYS_ERR_INVALID_STATE;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) {
        return ret;
    }

    if (s_instance.focus_state == MS41908M_STATE_RUNNING ||
        s_instance.zoom_state == MS41908M_STATE_RUNNING ||
        s_instance.zoom_state == MS41908M_STATE_RESET_ZERO ||
        ms41908m_reset_sm_active()) {
        ms41908m_port_unlock();
        return SYS_ERR_BUSY;
    }

    ms41908m_port_clear_event(MS41908M_EVENT_ZOOM_RESET_DONE);
    s_instance.zoom_is_r_zero = 0;
    s_instance.zoom_state = MS41908M_STATE_RESET_ZERO;
    WIC_LOGI("[ms41908m] Zoom reset zero requested");
    ms41908m_port_unlock();

    ms41908m_port_set_event(MS41908M_EVENT_REQ_RESET_ZOOM, 0);
    return SYS_OK;
}

int ms41908m_focus_reset_zero(void)
{
    int ret;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    if (s_instance.focus_state == MS41908M_STATE_NO_CFG) {
        return SYS_ERR_INVALID_STATE;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) {
        return ret;
    }

    if (s_instance.zoom_state == MS41908M_STATE_RUNNING ||
        s_instance.focus_state == MS41908M_STATE_RUNNING ||
        s_instance.focus_state == MS41908M_STATE_RESET_ZERO ||
        ms41908m_reset_sm_active()) {
        ms41908m_port_unlock();
        return SYS_ERR_BUSY;
    }

    ms41908m_port_clear_event(MS41908M_EVENT_FOCUS_RESET_DONE);
    s_instance.focus_is_r_zero = 0;
    s_instance.focus_state = MS41908M_STATE_RESET_ZERO;
    WIC_LOGI("[ms41908m] Focus reset zero requested");
    ms41908m_port_unlock();

    ms41908m_port_set_event(MS41908M_EVENT_REQ_RESET_FOCUS, 0);
    return SYS_OK;
}

int ms41908m_zoom_wait_reset_done(uint32_t timeout_ms)
{
    uint32_t ev;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    ev = ms41908m_port_wait_for_event(MS41908M_EVENT_ZOOM_RESET_DONE, timeout_ms, 1, 0);
    if (ev & MS41908M_EVENT_ZOOM_RESET_DONE) {
        return s_zoom_rst_last_result;
    }

    return SYS_ERR_TIMEOUT;
}

int ms41908m_focus_wait_reset_done(uint32_t timeout_ms)
{
    uint32_t ev;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    ev = ms41908m_port_wait_for_event(MS41908M_EVENT_FOCUS_RESET_DONE, timeout_ms, 1, 0);
    if (ev & MS41908M_EVENT_FOCUS_RESET_DONE) {
        return s_focus_rst_last_result;
    }

    return SYS_ERR_TIMEOUT;
}

int ms41908m_zoom_get_last_reset_result(void)
{
    return s_zoom_rst_last_result;
}

int ms41908m_focus_get_last_reset_result(void)
{
    return s_focus_rst_last_result;
}

/* ============================================================================
 * Public API: Wait for completion
 * ============================================================================ */
int ms41908m_zoom_wait_for_completion(uint32_t timeout_ms)
{
    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    if (s_instance.zoom_state != MS41908M_STATE_RUNNING) {
        return SYS_OK;
    }

    uint32_t events = ms41908m_port_wait_for_event(MS41908M_EVENT_ZOOM_COMPLETED, timeout_ms, 1, 0);
    if (events & MS41908M_EVENT_ZOOM_COMPLETED) {
        return SYS_OK;
    }

    return SYS_ERR_TIMEOUT;
}

int ms41908m_focus_wait_for_completion(uint32_t timeout_ms)
{
    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    if (s_instance.focus_state != MS41908M_STATE_RUNNING) {
        return SYS_OK;
    }

    uint32_t events = ms41908m_port_wait_for_event(MS41908M_EVENT_FOCUS_COMPLETED, timeout_ms, 1, 0);
    if (events & MS41908M_EVENT_FOCUS_COMPLETED) {
        return SYS_OK;
    }

    return SYS_ERR_TIMEOUT;
}

/* ============================================================================
 * Public API: Position limit
 * ============================================================================ */
int ms41908m_zoom_set_position_limit(int32_t position_min, int32_t position_max)
{
    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    if (position_max <= position_min) {
        return SYS_ERR_INVALID_ARG;
    }

    /* Zero point must be within range */
    if (position_min > 0 || position_max < 0) {
        return SYS_ERR_INVALID_ARG;
    }

    s_instance.zoom_pos_min = position_min;
    s_instance.zoom_pos_max = position_max;

    return SYS_OK;
}

int ms41908m_focus_set_position_limit(int32_t position_min, int32_t position_max)
{
    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    if (position_max <= position_min) {
        return SYS_ERR_INVALID_ARG;
    }

    /* Zero point must be within range */
    if (position_min > 0 || position_max < 0) {
        return SYS_ERR_INVALID_ARG;
    }

    s_instance.focus_pos_min = position_min;
    s_instance.focus_pos_max = position_max;

    return SYS_OK;
}

int ms41908m_zoom_is_reset_zero(void)
{
    return s_instance.zoom_is_r_zero;
}

int ms41908m_focus_is_reset_zero(void)
{
    return s_instance.focus_is_r_zero;
}

int ms41908m_read_pi_zoom(void)
{
    if (!s_instance.is_initialized) {
        return -1;
    }
    return (ms41908m_port_read_irq_status(MS41908M_IRQ_PI_ZOOM) != 0U) ? 1 : 0;
}

int ms41908m_read_pi_focus(void)
{
    if (!s_instance.is_initialized) {
        return -1;
    }
    return (ms41908m_port_read_irq_status(MS41908M_IRQ_PI_FOCUS) != 0U) ? 1 : 0;
}

/* ============================================================================
 * Public API: Motor stop
 * ============================================================================ */
int ms41908m_zoom_stop(void)
{
    int ret = SYS_OK;
    uint8_t was_running = 0;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) return ret;

    was_running = (s_instance.zoom_state == MS41908M_STATE_RUNNING) ? 1U : 0U;

    if (s_rst.phase != MS41908M_RZ_IDLE && s_rst.type == MS41908M_TYPE_ZOOM) {
        s_rst.abort_requested = 1;
    }

    ret = ms41908m_motor_stop_impl(MS41908M_TYPE_ZOOM);
    if (ret == SYS_OK && was_running) {
        /* Stopping during motion makes absolute position uncertain. */
        s_instance.zoom_is_r_zero = 0;
    }

    ms41908m_port_unlock();
    return ret;
}

int ms41908m_focus_stop(void)
{
    int ret = SYS_OK;
    uint8_t was_running = 0;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) return ret;

    was_running = (s_instance.focus_state == MS41908M_STATE_RUNNING) ? 1U : 0U;

    if (s_rst.phase != MS41908M_RZ_IDLE && s_rst.type == MS41908M_TYPE_FOCUS) {
        s_rst.abort_requested = 1;
    }

    ret = ms41908m_motor_stop_impl(MS41908M_TYPE_FOCUS);
    if (ret == SYS_OK && was_running) {
        /* Stopping during motion makes absolute position uncertain. */
        s_instance.focus_is_r_zero = 0;
    }

    ms41908m_port_unlock();
    return ret;
}

/* ============================================================================
 * Public API: Synchronous dual-axis move (zf_sync)
 * Both axes are configured via SPI, then a single VD_FZ pulse starts them
 * simultaneously. Each axis runs at its own speed for its own distance.
 * Completion is signalled via MS41908M_EVT_ZF_SYNC_RUN_DONE when BOTH axes finish.
 * ============================================================================ */
int ms41908m_zf_sync_run(uint16_t zm_pps, int32_t zm_micro_steps,
                         uint16_t fs_pps, int32_t fs_micro_steps)
{
    int ret = SYS_OK;
    uint16_t step_reg = 0;
    uint16_t pps_reg = 0;
    uint16_t intct;
    uint16_t psum;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    if (s_instance.zoom_state == MS41908M_STATE_NO_CFG ||
        s_instance.focus_state == MS41908M_STATE_NO_CFG) {
        return SYS_ERR_INVALID_STATE;
    }

    ret = ms41908m_port_lock();
    if (ret != SYS_OK) return ret;

    /* Neither axis may be running or in reset */
    if (s_instance.zoom_state == MS41908M_STATE_RUNNING ||
        s_instance.zoom_state == MS41908M_STATE_RESET_ZERO ||
        s_instance.focus_state == MS41908M_STATE_RUNNING ||
        s_instance.focus_state == MS41908M_STATE_RESET_ZERO ||
        ms41908m_reset_sm_active()) {
        ms41908m_port_unlock();
        return SYS_ERR_BUSY;
    }

    /* --- Configure Zoom --- */
    if (zm_micro_steps != 0) {
        if (zm_pps < MS41908M_PPS_MIN || zm_pps > MS41908M_PPS_MAX ||
            MS41908M_ABS(zm_micro_steps) < MS41908M_MSTEPS_MIN ||
            MS41908M_ABS(zm_micro_steps) > MS41908M_MSTEPS_MAX) {
            ms41908m_port_unlock();
            return SYS_ERR_INVALID_ARG;
        }

        intct = MS41908M_CALC_INTCT_FROM_PPS(zm_pps);
        psum  = MS41908M_CALC_PSUM_FROM_INTCT(intct);
        if (psum > MS41908M_ABS(zm_micro_steps)) psum = MS41908M_ABS(zm_micro_steps);
        if (psum > 255) psum = 255;
        if (psum < 1) psum = 1;
        intct = MS41908M_CALC_INTCT_FROM_PSUM(psum);
        if (intct > 65535) intct = 65535;
        if (intct < 1) intct = 1;

        s_instance.zoom_run_config.direction = (zm_micro_steps >= 0) ? 1 : 0;
        s_instance.zoom_run_config.load_psum = psum;
        s_instance.zoom_run_config.unload_psum = MS41908M_ABS(zm_micro_steps) - psum;
        s_instance.zoom_run_config.intct = intct;
        s_instance.zoom_run_config.is_valid = 1;

        pps_reg = intct & 0xFFFF;
        ret = ms41908m_port_write(MS41908M_REG_ZOOM_PPS, pps_reg);
        if (ret != SYS_OK) goto sync_err;

        ret = ms41908m_port_read(MS41908M_REG_ZOOM_STEP, &step_reg);
        if (ret != SYS_OK) goto sync_err;
        step_reg &= (MS41908M_STEP_MICRO_MASK | MS41908M_STEP_LED_BIT);
        step_reg |= MS41908M_STEP_ENDIS_BIT;
        step_reg |= (psum & 0xFF);
        if (s_instance.zoom_run_config.direction == 0) {
            step_reg |= MS41908M_STEP_CCWCW_BIT;
        }
        ret = ms41908m_port_write(MS41908M_REG_ZOOM_STEP, step_reg);
        if (ret != SYS_OK) goto sync_err;

        ms41908m_port_clear_event(MS41908M_EVENT_ZOOM_COMPLETED);
        s_instance.zoom_start_tick = osKernelGetTickCount();
        s_instance.zoom_state = MS41908M_STATE_RUNNING;
    }

    /* --- Configure Focus --- */
    if (fs_micro_steps != 0) {
        if (fs_pps < MS41908M_PPS_MIN || fs_pps > MS41908M_PPS_MAX ||
            MS41908M_ABS(fs_micro_steps) < MS41908M_MSTEPS_MIN ||
            MS41908M_ABS(fs_micro_steps) > MS41908M_MSTEPS_MAX) {
            if (zm_micro_steps != 0) {
                s_instance.zoom_run_config.is_valid = 0;
                s_instance.zoom_state = MS41908M_STATE_STOPPED;
            }
            ms41908m_port_unlock();
            return SYS_ERR_INVALID_ARG;
        }

        intct = MS41908M_CALC_INTCT_FROM_PPS(fs_pps);
        psum  = MS41908M_CALC_PSUM_FROM_INTCT(intct);
        if (psum > MS41908M_ABS(fs_micro_steps)) psum = MS41908M_ABS(fs_micro_steps);
        if (psum > 255) psum = 255;
        if (psum < 1) psum = 1;
        intct = MS41908M_CALC_INTCT_FROM_PSUM(psum);
        if (intct > 65535) intct = 65535;
        if (intct < 1) intct = 1;

        s_instance.focus_run_config.direction = (fs_micro_steps >= 0) ? 1 : 0;
        s_instance.focus_run_config.load_psum = psum;
        s_instance.focus_run_config.unload_psum = MS41908M_ABS(fs_micro_steps) - psum;
        s_instance.focus_run_config.intct = intct;
        s_instance.focus_run_config.is_valid = 1;

        pps_reg = intct & 0xFFFF;
        ret = ms41908m_port_write(MS41908M_REG_FOCUS_PPS, pps_reg);
        if (ret != SYS_OK) goto sync_err;

        ret = ms41908m_port_read(MS41908M_REG_FOCUS_STEP, &step_reg);
        if (ret != SYS_OK) goto sync_err;
        step_reg &= (MS41908M_STEP_MICRO_MASK | MS41908M_STEP_LED_BIT);
        step_reg |= MS41908M_STEP_ENDIS_BIT;
        step_reg |= (psum & 0xFF);
        if (s_instance.focus_run_config.direction == 0) {
            step_reg |= MS41908M_STEP_CCWCW_BIT;
        }
        ret = ms41908m_port_write(MS41908M_REG_FOCUS_STEP, step_reg);
        if (ret != SYS_OK) goto sync_err;

        ms41908m_port_clear_event(MS41908M_EVENT_FOCUS_COMPLETED);
        s_instance.focus_start_tick = osKernelGetTickCount();
        s_instance.focus_state = MS41908M_STATE_RUNNING;
    }

    if (s_instance.zoom_state != MS41908M_STATE_RUNNING &&
        s_instance.focus_state != MS41908M_STATE_RUNNING) {
        ms41908m_port_unlock();
        return SYS_OK;
    }

    if (s_instance.zoom_state == MS41908M_STATE_RUNNING) {
        s_instance.zoom_batch_active = 1;
        s_instance.zoom_pls_rcvd    = 0;
        s_instance.zoom_start_tick   = osKernelGetTickCount();
    }
    if (s_instance.focus_state == MS41908M_STATE_RUNNING) {
        s_instance.focus_batch_active = 1;
        s_instance.focus_pls_rcvd    = 0;
        s_instance.focus_start_tick  = osKernelGetTickCount();
    }
    ms41908m_port_output_vd(MS41908M_TYPE_FOCUS);

    WIC_LOGI("[ms41908m] ZF sync started: zoom=%c%lu@%u, focus=%c%lu@%u",
             s_instance.zoom_run_config.direction  ? '+' : '-',
             (unsigned long)MS41908M_ABS(zm_micro_steps), zm_pps,
             s_instance.focus_run_config.direction ? '+' : '-',
             (unsigned long)MS41908M_ABS(fs_micro_steps), fs_pps);

    ms41908m_port_unlock();
    return SYS_OK;

sync_err:
    s_instance.zoom_run_config.is_valid = 0;
    s_instance.focus_run_config.is_valid = 0;
    s_instance.zoom_state  = (s_instance.zoom_state  == MS41908M_STATE_NO_CFG)
                           ? MS41908M_STATE_NO_CFG : MS41908M_STATE_STOPPED;
    s_instance.focus_state = (s_instance.focus_state == MS41908M_STATE_NO_CFG)
                           ? MS41908M_STATE_NO_CFG : MS41908M_STATE_STOPPED;
    ms41908m_port_unlock();
    return ret;
}

int ms41908m_zf_sync_wait_for_completion(uint32_t timeout_ms)
{
    uint32_t ev;
    uint32_t wait_mask = MS41908M_EVENT_FOCUS_COMPLETED | MS41908M_EVENT_ZOOM_COMPLETED;

    if (!s_instance.is_initialized) {
        return SYS_ERR_INVALID_STATE;
    }

    if (s_instance.zoom_state != MS41908M_STATE_RUNNING &&
        s_instance.focus_state != MS41908M_STATE_RUNNING) {
        return SYS_OK;
    }

    ev = ms41908m_port_wait_for_event(wait_mask, timeout_ms, 1, 1);
    if ((ev & wait_mask) == wait_mask) {
        return SYS_OK;
    }

    return SYS_ERR_TIMEOUT;
}
