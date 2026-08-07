#ifndef __MS41908M_H__
#define __MS41908M_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "ms41908m_port.h"
#include "ms41908m_reg.h"

/* Clock and timing */
#define MS41908M_OSCIN_FREQ                     (27000000UL)                    /* 27MHz */
#define MS41908M_VD_FREQ                        (16UL)                          /* VD frequency in Hz */
#define MS41908M_VD_DELAY_MS                    (1000UL / MS41908M_VD_FREQ)     /* ~62ms per VD cycle */
/* VD calculation macros */
#define MS41908M_ONE_STEP_PSUM                  (32UL)                          /* 1 step = 32 micro steps */
#define MS41908M_CALC_INTCT_FROM_PPS(pps)       (MS41908M_OSCIN_FREQ / ((uint32_t)(pps) * 24UL))                        /* pps (micro steps/sec) to intct */
#define MS41908M_CALC_INTCT_FROM_PSUM(psum)     (MS41908M_OSCIN_FREQ / ((uint32_t)(psum) * MS41908M_VD_FREQ * 24UL))    /* psum (micro steps) to intct */
#define MS41908M_CALC_PSUM_FROM_INTCT(intct)    (MS41908M_OSCIN_FREQ / ((uint32_t)(intct) * MS41908M_VD_FREQ * 24UL))   /* intct to psum (micro steps) */
/* PPS and micro steps limits */
#define MS41908M_PPS_MIN                        (24UL)                      /* 24 micro steps/sec */
#define MS41908M_PPS_MAX                        (4000UL)                    /* 4000 micro steps/sec */
#define MS41908M_MSTEPS_MIN                     (1UL)                       /* 1 micro step */
#define MS41908M_MSTEPS_MAX                     (65535UL)                   /* 65535 micro steps */
/* Reset zero configuration */
#define MS41908M_RESET_FAST_PPS                 (1200UL)                        /* Fast exit from PI blocked (LOW) zone */
#define MS41908M_RESET_SLOW_PPS                 (400UL)                         /* Slow re-approach until PI falling edge */

#define MS41908M_TRAVEL_MARGIN                  (400UL)                        /* Travel margin: 400 micro steps */

/* Iris configuration */
typedef struct {
    /* 0x00: IRIS_TGT */
    uint16_t iris_tgt;          /* [9:0] Iris target value */
    /* 0x01: IRIS_CFG1 */
    uint8_t  over_lpf_fc_1st;   /* [1:0] OVER_LPF_FC_1ST: ADC feedback filter 1st stage cutoff */
    uint8_t  over_lpf_fc_2nd;   /* [3:2] OVER_LPF_FC_2ND: ADC feedback filter 2nd stage cutoff */
    uint8_t  dec_ave;           /* [4]   DEC_AVE: enable moving average for iris target */
    uint8_t  as_flt_off;        /* [5]   AS_FLT_OFF: disable/enable PID pre LPF (0=enable) */
    uint8_t  asound_lpf_fc;     /* [8:6] ASOUND_LPF_FC: PID pre LPF cutoff */
    uint8_t  dgain;             /* [15:9] DGAIN: PID digital gain */
    /* 0x02: IRIS_CFG2 */
    uint8_t  iris_calc_nr;      /* [3:0] IRIS_CALC_NR: integral error accumulation limit */
    uint8_t  iris_round;        /* [7:4] IRIS_ROUND: differential error accumulation limit */
    uint8_t  pid_zero;          /* [11:8] PID_ZERO */
    uint8_t  pid_pole;          /* [15:12] PID_POLE */
    /* 0x03: IRIS_CFG3 */
    uint8_t  arw;               /* [3:0] ARW: anti‑reset‑windup bits */
    uint8_t  lmt_enb;           /* [4]   LMT_ENB: integrator stop/limit control */
    uint8_t  pwm_flt_off;       /* [5]   PWM_FLT_OFF: disable/enable PID post LPF (0=enable) */
    uint8_t  pwm_lpf_fc;        /* [8:6] PWM_LPF_FC: PID post LPF cutoff */
    uint8_t  pwm_iris;          /* [11:9] PWM_IRIS: iris PWM frequency selection */
    uint8_t  dt_adj_iris;       /* [13:12] DT_ADJ_IRIS: output dead‑time for iris driver */
    /* 0x04: HALL_CFG */
    uint8_t  hall_bias_dac;     /* [7:0] HALL_BAIS_DAC: Hall bias current DAC */
    uint8_t  hall_offset_dac;   /* [15:8] HALL_OFFSET_DAC: Hall amplifier offset DAC */
    /* 0x05: IRIS_CFG4 */
    uint8_t  tgt_lpf_fc;        /* [3:0] TGT_LPF_FC: iris target LPF cutoff */
    uint8_t  tgt_flt_off;       /* [4]   TGT_FLT_OFF: disable/enable target LPF (0=enable) */
    uint8_t  pid_inv;           /* [5]   PID_INV: invert PID polarity */
    uint8_t  hall_gain;         /* [11:8] HALL_GAIN: Hall amplifier gain */
    uint8_t  aaf_fc;            /* [12] AAF_FC: anti‑aliasing filter cutoff */
    /* 0x06 / 0x07: Pulse 1 */
    uint16_t start1;            /* 0x06 [9:0] START1: pulse 1 start position */
    uint16_t width1;            /* 0x07 [11:0] WIDTH1: pulse 1 width */
    uint8_t  p1_en;             /* 0x07 [15]   P1EN: enable pulse 1 */
    /* 0x08 / 0x09: Pulse 2 */
    uint16_t start2;            /* 0x08 [9:0] START2: pulse 2 start position */
    uint8_t  width2;            /* 0x09 [5:0] WIDTH2: pulse 2 width */
    uint8_t  p2_en;             /* 0x09 [15]  P2EN: enable pulse 2 */
    /* 0x0A: IRIS_TEST */
    uint16_t tgt_in_test;       /* [9:0] TGT_IN_TEST: target value in test mode */
    uint8_t  duty_test;         /* [10]  DUTY_TEST: duty test enable */
    /* 0x0B: MODE_CFG */
    uint8_t  asw_mode;          /* [4:3] ASWMODE: ASW mode select */
    uint8_t  test_en1;          /* [7]   TEST_EN1: test enable 1 */
    uint8_t  mode_sel_iris;     /* [8]   MODESEL_IRIS: iris mode select */
    uint8_t  mode_sel_fz;       /* [9]   MODESEL_FZ: FZ (focus/zoom) mode select */
    uint8_t  pdwnb;             /* [10]  PDWNB: iris block power‑down (0=power‑down) */
    uint8_t  adc_test;          /* [11]  ADC_TEST: ADC test enable */
    uint8_t  pid_clip;          /* [15:12] PID_CLIP: PID clipping level */
    /* 0x0E: TGT_UPDATE */
    uint8_t  tgt_update;        /* [7:0] TGT_UPDATE: target update rate */
    uint8_t  ave_speed;         /* [12:8] AVE_SPEED: averaging speed for target */
} ms41908m_iris_config_t;

/* Motor configuration (Focus -> α (AB), Zoom -> β (CD)) */
typedef struct {
    /* 0x20: PWM_CFG (common PWM configuration for motor channels) */
    uint8_t  dt1;               /* [7:0]  DT1: primary dead-time */
    uint8_t  pwm_mode;          /* [12:8] PWMMODE: PWM mode selection */
    uint8_t  pwm_res;           /* [14:13] PWMRES: PWM resolution */

    /* 0x21: TEST_CFG (test mode configuration) */
    uint8_t  fz_test;           /* [4:0] FZTEST: FZ test value */
    uint8_t  test_en2;          /* [7]   TEST_EN2: test enable 2 */

    /* 0x22: MOTOR_AB_DT2 (phase mode AB, dead-time 2A) */
    uint8_t  dt2a;              /* [7:0]  DT2A: dead-time 2A */
    uint8_t  phmodab;           /* [13:8] PHMODAB: phase mode AB */

    /* 0x23: MOTOR_AB_PPW (PPWA, PPWB) */
    uint8_t  ppwa;              /* [7:0]  PPWA: peak pulse width A */
    uint8_t  ppwb;              /* [15:8] PPWB: peak pulse width B */

    /* 0x24: MOTOR_AB_STEP */
    uint8_t  leda;              /* [11]   LEDA: LED A */
    uint8_t  microab;           /* [13:12] MICROAB: micro step mode */

    /* 0x27: MOTOR_CD_DT2 (phase mode CD, dead-time 2B) */
    uint8_t  dt2b;              /* [7:0]  DT2B: dead-time 2B */
    uint8_t  phmodcd;           /* [13:8] PHMODCD: phase mode CD */

    /* 0x28: MOTOR_CD_PPW (PPWC, PPWD) */
    uint8_t  ppwc;              /* [7:0]  PPWC: peak pulse width C */
    uint8_t  ppwd;              /* [15:8] PPWD: peak pulse width D */

    /* 0x29: MOTOR_CD_STEP */
    uint8_t  ledb;              /* [11]   LEDB: LED B */
    uint8_t  microcd;           /* [13:12] MICROCD: micro step mode */
} ms41908m_motor_config_t;

/* module status */
typedef enum {
    MS41908M_STATE_NO_CFG = 0,  /* not yet loaded via ms41908m_*_config() */
    MS41908M_STATE_STOPPED,     /* configured, idle */
    MS41908M_STATE_RUNNING,
    MS41908M_STATE_RESET_ZERO,
    MS41908M_STATE_ERROR,
} ms41908m_state_t;

/* ms41908m run configuration */
typedef struct {
    uint8_t is_valid;
    uint8_t direction;
    uint16_t intct;
    int32_t unload_psum;
    int32_t load_psum;
} ms41908m_run_config_t;

/* ms41908m instance structure */
typedef struct {
    uint8_t is_initialized;

    ms41908m_state_t iris_state;

    ms41908m_state_t zoom_state;    /* Zoom and focus may run concurrently; the driver coordinates registers before each VD_FZ pulse */
    uint8_t zoom_is_r_zero;
    int32_t zoom_position;
    int32_t zoom_pos_min, zoom_pos_max;
    ms41908m_run_config_t zoom_run_config;
    uint32_t zoom_start_tick;       /* osKernelGetTickCount() when motor started */

    ms41908m_state_t focus_state;
    uint8_t focus_is_r_zero;
    int32_t focus_position;
    int32_t focus_pos_min, focus_pos_max;
    ms41908m_run_config_t focus_run_config;
    uint32_t focus_start_tick;      /* osKernelGetTickCount() when motor started */

    /* Per-axis batch tracking for VD_FZ coordination */
    uint8_t focus_batch_active;     /* VD_FZ has been issued; waiting for PLS1 */
    uint8_t focus_pls_rcvd;         /* PLS1 received; register updated, ready for next VD_FZ */
    uint8_t zoom_batch_active;      /* VD_FZ has been issued; waiting for PLS2 */
    uint8_t zoom_pls_rcvd;          /* PLS2 received; register updated, ready for next VD_FZ */
} ms41908m_instance_t;

/* Motor movement timeout: force-stop if PLS lost (ms) */
#define MS41908M_MOTOR_TIMEOUT_MS   (5000U)

/* Application event callback (invoked from motor task context, after driver lock is released).
 * event: one of MS41908M_EVT_* below (same bit values as internal port events).
 * result: SYS_OK for run completion or successful reset; on MS41908M_EVT_*_RESET_DONE failure,
 *         result is the error code (same as ms41908m_*_get_last_reset_result()). */
typedef void (*ms41908m_event_callback_t)(uint32_t event, int result);

#define MS41908M_EVT_FOCUS_RUN_DONE     MS41908M_EVENT_FOCUS_COMPLETED
#define MS41908M_EVT_ZOOM_RUN_DONE      MS41908M_EVENT_ZOOM_COMPLETED
#define MS41908M_EVT_FOCUS_RESET_DONE   MS41908M_EVENT_FOCUS_RESET_DONE
#define MS41908M_EVT_ZOOM_RESET_DONE    MS41908M_EVENT_ZOOM_RESET_DONE
#define MS41908M_EVT_ZF_SYNC_RUN_DONE   (MS41908M_EVENT_FOCUS_COMPLETED | MS41908M_EVENT_ZOOM_COMPLETED)

extern const ms41908m_iris_config_t g_default_iris_config;
extern const ms41908m_motor_config_t g_default_motor_config;

int ms41908m_init(void);
void ms41908m_deinit(void);
void ms41908m_set_event_callback(ms41908m_event_callback_t callback);

int ms41908m_iris_config(const ms41908m_iris_config_t *config);
int ms41908m_iris_update_target(uint16_t target);
uint16_t ms41908m_iris_read_adc(void);
ms41908m_state_t ms41908m_iris_get_state(void);
int ms41908m_iris_run(void);
int ms41908m_iris_stop(void);

int ms41908m_motor_config(const ms41908m_motor_config_t *config);
ms41908m_state_t ms41908m_get_zoom_state(void);         /* Zoom and focus may run concurrently */
ms41908m_state_t ms41908m_get_focus_state(void);
int ms41908m_get_zoom_position(void);       /* return the current position, if reset zero is not done or while running, return value is not guaranteed to be correct  */
int ms41908m_get_focus_position(void);      /* return the current position, if reset zero is not done or while running, return value is not guaranteed to be correct */
int ms41908m_zoom_run(uint16_t pps, int32_t micro_steps);
int ms41908m_focus_run(uint16_t pps, int32_t micro_steps);
int ms41908m_zoom_run_to_position(uint16_t pps, int32_t position);   /* zero + if limits set (*_set_position_limit), target must be in [min,max] */
int ms41908m_focus_run_to_position(uint16_t pps, int32_t position);   /* zero + if limits set (*_set_position_limit), target must be in [min,max] */
/* Synchronous dual-axis move: both axes start on the same VD_FZ edge with independent speeds. */
int ms41908m_zf_sync_run(uint16_t zm_pps, int32_t zm_micro_steps,
                         uint16_t fs_pps, int32_t fs_micro_steps);
int ms41908m_zf_sync_wait_for_completion(uint32_t timeout_ms);
/* Reset-zero: API only sets MS41908M_STATE_RESET_ZERO and notifies the motor task; actual work
 * runs in ms41908m_motor_task_handler. Returns SYS_OK if the request was queued.
 * Wait for MS41908M_EVENT_*_RESET_DONE then use *_get_last_reset_result() for the outcome. */
int ms41908m_zoom_reset_zero(void);     /* reset zero need set position limit first */
int ms41908m_focus_reset_zero(void);     /* reset zero need set position limit first */
int ms41908m_zoom_wait_reset_done(uint32_t timeout_ms);
int ms41908m_focus_wait_reset_done(uint32_t timeout_ms);
int ms41908m_zoom_get_last_reset_result(void);
int ms41908m_focus_get_last_reset_result(void);
int ms41908m_zoom_wait_for_completion(uint32_t timeout_ms);
int ms41908m_focus_wait_for_completion(uint32_t timeout_ms);
int ms41908m_zoom_set_position_limit(int32_t position_min, int32_t position_max);
int ms41908m_focus_set_position_limit(int32_t position_min, int32_t position_max);
int ms41908m_zoom_is_reset_zero(void);      /* return 1 if reset zero is done, 0 otherwise */
int ms41908m_focus_is_reset_zero(void);     /* return 1 if reset zero is done, 0 otherwise */
/* PI GPIO level: 1=HIGH, 0=LOW.
 *
 * Hardware PI placement (verified on bench):
 *   Zoom:  PI_HIGH = max(+) side, clear (unblocked)
 *          PI_LOW  = min(-) side, shaded
 *   Focus: PI_HIGH = min(-) side, clear (unblocked)
 *          PI_LOW  = max(+) side, shaded
 *
 * Motor micro_steps sign (shell, host_link, reset-zero all use the same path):
 *   micro_steps > 0 => toward max(+); micro_steps < 0 => toward min(-)
 *   No extra axis inversion in ms41908m_motor_start_impl().
 */
int ms41908m_read_pi_zoom(void);
int ms41908m_read_pi_focus(void);
int ms41908m_zoom_stop(void);               /* if stop at running, the position is uncertain */
int ms41908m_focus_stop(void);              /* if stop at running, the position is uncertain */

#ifdef __cplusplus
}
#endif

#endif /* __MS41908M_H__ */
