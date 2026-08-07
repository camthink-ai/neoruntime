/**
 * @file    ms41908m.h
 * @brief   MS41908M lens driver IC register definitions (Ruimon).
 *          Dual-channel stepper driver with built-in iris control.
 * @note    Serial interface: 24-bit, C0=R/W, A[5:0]=address, D[15:0]=data.
 */

#ifndef __MS41908M_REG_H__
#define __MS41908M_REG_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Register addresses (6-bit, 0x00–0x3F)
 * ============================================================================ */

/* --- Iris / ADC / PID (0x00–0x0F) --- */
#define MS41908M_REG_IRIS_TGT           0x00U  /* Iris target value */
#define MS41908M_REG_IRIS_CFG1          0x01U  /* ADC filter, PID pre-filter, DGAIN */
#define MS41908M_REG_IRIS_CFG2          0x02U  /* PID zero/pole, iris calc params */
#define MS41908M_REG_IRIS_CFG3          0x03U  /* PWM filter, ARW, dead-time, etc. */
#define MS41908M_REG_HALL_CFG           0x04U  /* Hall bias and offset DAC */
#define MS41908M_REG_IRIS_CFG4          0x05U  /* Target LPF, PID invert, Hall gain */
#define MS41908M_REG_PULSE1_START       0x06U  /* Pulse 1 start position */
#define MS41908M_REG_PULSE1_CFG         0x07U  /* Pulse 1 enable and width */
#define MS41908M_REG_PULSE2_START       0x08U  /* Pulse 2 start position */
#define MS41908M_REG_PULSE2_CFG         0x09U  /* Pulse 2 enable and width */
#define MS41908M_REG_IRIS_TEST          0x0AU  /* Duty test, target test value */
#define MS41908M_REG_MODE_CFG           0x0BU  /* PID clip, ADC test, mode select, etc. */
#define MS41908M_REG_IRSAD              0x0CU  /* Iris ADC value (read-only) */
#define MS41908M_REG_TGT_UPDATE         0x0EU  /* Average speed, target update */
#define MS41908M_REG_RESERVED_0F        0x0FU  /* Reserved */

/* --- Motor / PWM (0x20–0x2C) --- */
#define MS41908M_REG_PWM_CFG            0x20U  /* PWM resolution, mode, dead-time 1 */
#define MS41908M_REG_TEST_CFG           0x21U  /* Test enable 2, FZ test */
#define MS41908M_REG_MOTOR_AB_DT2       0x22U  /* Phase mode AB, dead-time 2A */
#define MS41908M_REG_MOTOR_AB_PPW       0x23U  /* PPWA, PPWB (phase pulse width) */
#define MS41908M_REG_MOTOR_AB_STEP      0x24U  /* Step sum, direction, brake, enable, micro, LED */
#define MS41908M_REG_MOTOR_AB_PPS       0x25U  /* Pulse per step (AB channel) */
#define MS41908M_REG_MOTOR_CD_DT2       0x27U  /* Phase mode CD, dead-time 2B */
#define MS41908M_REG_MOTOR_CD_PPW       0x28U  /* PPWC, PPWD */
#define MS41908M_REG_MOTOR_CD_STEP      0x29U  /* Step sum, direction, brake, enable, micro, LED */
#define MS41908M_REG_MOTOR_CD_PPS       0x2AU  /* Pulse per step (CD channel) */
#define MS41908M_REG_RESERVED_2C        0x2CU  /* Reserved */

/* Motor channel assignment (board-specific: AB=Focus/Zoom, CD=Zoom/Focus; see schematic) */
#define MS41908M_REG_FOCUS_STEP         0x24U
#define MS41908M_REG_FOCUS_PPS          0x25U
#define MS41908M_REG_ZOOM_STEP          0x29U
#define MS41908M_REG_ZOOM_PPS           0x2AU

/* ============================================================================
 * Register 0x00 – IRIS_TGT (Iris target value)
 * ============================================================================ */
#define MS41908M_IRIS_TGT_MASK          (0x03FFU)  /* [9:0] Iris target (0–1023) */

/* ============================================================================
 * Register 0x01 – IRIS_CFG1 (ADC filter, PID pre-filter, DGAIN)
 * ============================================================================ */
#define MS41908M_OVER_LPF_FC_1ST_SHIFT  (0U)
#define MS41908M_OVER_LPF_FC_1ST_MASK   (3U << 0)   /* [1:0] ADC feedback filter 1st stage cutoff */
#define MS41908M_OVER_LPF_FC_2ND_SHIFT  (2U)
#define MS41908M_OVER_LPF_FC_2ND_MASK   (3U << 2)   /* [3:2] ADC feedback filter 2nd stage cutoff */
#define MS41908M_DEC_AVE_SHIFT          (4U)
#define MS41908M_DEC_AVE_MASK           (1U << 4)  /* [4] Moving average for iris target */
#define MS41908M_AS_FLT_OFF_SHIFT       (5U)
#define MS41908M_AS_FLT_OFF_MASK        (1U << 5)  /* [5] PID pre low-pass filter enable (0=on) */
#define MS41908M_ASOUND_LPF_FC_SHIFT    (6U)
#define MS41908M_ASOUND_LPF_FC_MASK     (7U << 6)  /* [8:6] PID pre LPF cutoff frequency */
#define MS41908M_DGAIN_SHIFT            (9U)
#define MS41908M_DGAIN_MASK             (0x7FU << 9) /* [15:9] PID digital gain */

/* ============================================================================
 * Register 0x02 – IRIS_CFG2 (PID zero/pole, iris calc)
 * ============================================================================ */
#define MS41908M_IRIS_CALC_NR_SHIFT     (0U)
#define MS41908M_IRIS_CALC_NR_MASK      (15U << 0)  /* [3:0] PID integrator error cap */
#define MS41908M_IRIS_ROUND_SHIFT       (4U)
#define MS41908M_IRIS_ROUND_MASK        (15U << 4)  /* [7:4] PID differentiator error cap */
#define MS41908M_PID_ZERO_SHIFT         (8U)
#define MS41908M_PID_ZERO_MASK          (15U << 8) /* [11:8] PID zero */
#define MS41908M_PID_POLE_SHIFT         (12U)
#define MS41908M_PID_POLE_MASK          (15U << 12)/* [15:12] PID pole */

/* ============================================================================
 * Register 0x03 – IRIS_CFG3 (PWM filter, ARW, dead-time, etc.)
 * ============================================================================ */
#define MS41908M_ARW_SHIFT              (0U)
#define MS41908M_ARW_MASK               (15U << 0)  /* [3:0] PID anti-reset windup bits */
#define MS41908M_LMT_ENB_SHIFT          (4U)
#define MS41908M_LMT_ENB_MASK           (1U << 4)   /* [4] Integrator limit disable */
#define MS41908M_PWM_FLT_OFF_SHIFT      (5U)
#define MS41908M_PWM_FLT_OFF_MASK       (1U << 5)  /* [5] PID post LPF enable (0=on) */
#define MS41908M_PWM_LPF_FC_SHIFT       (6U)
#define MS41908M_PWM_LPF_FC_MASK        (7U << 6)  /* [8:6] PID post LPF cutoff */
#define MS41908M_PWM_IRIS_SHIFT         (9U)
#define MS41908M_PWM_IRIS_MASK          (7U << 9)  /* [11:9] Iris PWM frequency */
#define MS41908M_DT_ADJ_IRIS_SHIFT      (12U)
#define MS41908M_DT_ADJ_IRIS_MASK       (3U << 12) /* [13:12] Iris output dead-time */

/* ============================================================================
 * Register 0x04 – HALL_CFG (Hall sensor)
 * ============================================================================ */
#define MS41908M_HALL_BAIS_DAC_SHIFT    (0U)
#define MS41908M_HALL_BAIS_DAC_MASK     (0xFFU << 0) /* [7:0] Hall bias current DAC */
#define MS41908M_HALL_OFFSET_DAC_SHIFT  (8U)
#define MS41908M_HALL_OFFSET_DAC_MASK   (0xFFU << 8) /* [15:8] Hall offset calibration DAC */

/* ============================================================================
 * Register 0x05 – IRIS_CFG4 (Target LPF, PID invert, Hall gain)
 * ============================================================================ */
#define MS41908M_TGT_LPF_FC_SHIFT       (0U)
#define MS41908M_TGT_LPF_FC_MASK        (15U << 0)  /* [3:0] Iris target LPF cutoff */
#define MS41908M_TGT_FLT_OFF_SHIFT      (4U)
#define MS41908M_TGT_FLT_OFF_MASK       (1U << 4)   /* [4] Target LPF enable (0=on) */
#define MS41908M_PID_INV_SHIFT          (5U)
#define MS41908M_PID_INV_MASK           (1U << 5)   /* [5] PID polarity invert */
#define MS41908M_HALL_GAIN_SHIFT        (8U)
#define MS41908M_HALL_GAIN_MASK         (15U << 8)  /* [11:8] Hall gain */
#define MS41908M_AAF_FC_SHIFT           (12U)
#define MS41908M_AAF_FC_MASK            (1U << 12) /* [12] Anti‑aliasing filter cutoff */

/* ============================================================================
 * Register 0x06 – PULSE1_START
 * ============================================================================ */
#define MS41908M_START1_MASK            (0x03FFU)   /* [9:0] Pulse 1 start position */

/* ============================================================================
 * Register 0x07 – PULSE1_CFG (P1EN, WIDTH1)
 * ============================================================================ */
#define MS41908M_WIDTH1_MASK            (0x0FFFU)   /* [11:0] Pulse 1 width */
#define MS41908M_P1EN_SHIFT             (15U)
#define MS41908M_P1EN_MASK              (1U << 15)  /* [15] Pulse 1 enable */

/* ============================================================================
 * Register 0x08 – PULSE2_START
 * ============================================================================ */
#define MS41908M_START2_MASK            (0x03FFU)   /* [9:0] Pulse 2 start position */

/* ============================================================================
 * Register 0x09 – PULSE2_CFG (P2EN, WIDTH2)
 * ============================================================================ */
#define MS41908M_WIDTH2_MASK            (0x003FU)   /* [5:0] Pulse 2 width */
#define MS41908M_P2EN_SHIFT             (15U)
#define MS41908M_P2EN_MASK              (1U << 15)   /* [15] Pulse 2 enable */

/* ============================================================================
 * Register 0x0A – IRIS_TEST
 * ============================================================================ */
#define MS41908M_TGT_IN_TEST_MASK       (0x03FFU)   /* [9:0] Target value in test mode */
#define MS41908M_DUTY_TEST_SHIFT        (10U)
#define MS41908M_DUTY_TEST_MASK         (1U << 10)  /* [10] Duty test mode */

/* ============================================================================
 * Register 0x0B – MODE_CFG (PID clip, ADC test, mode select, etc.)
 * ============================================================================ */
#define MS41908M_ASWMODE_SHIFT          (3U)
#define MS41908M_ASWMODE_MASK           (3U << 3)   /* [4:3] ASW mode */
#define MS41908M_TEST_EN1_SHIFT         (7U)
#define MS41908M_TEST_EN1_MASK          (1U << 7)   /* [7] Test enable 1 */
#define MS41908M_MODESEL_IRIS_SHIFT     (8U)
#define MS41908M_MODESEL_IRIS_MASK      (1U << 8)   /* [8] Iris mode select */
#define MS41908M_MODESEL_FZ_SHIFT       (9U)
#define MS41908M_MODESEL_FZ_MASK        (1U << 9)   /* [9] Focus/Zoom mode select */
#define MS41908M_PDWNB_SHIFT            (10U)
#define MS41908M_PDWNB_MASK             (1U << 10)   /* [10] PDWNB: iris block power‑down (0=power‑down) */
#define MS41908M_ADC_TEST_SHIFT         (11U)
#define MS41908M_ADC_TEST_MASK          (1U << 11)   /* [11] ADC test */
#define MS41908M_PID_CLIP_SHIFT         (12U)
#define MS41908M_PID_CLIP_MASK          (15U << 12)  /* [15:12] PID clip */

/* ============================================================================
 * Register 0x0C – IRSAD (read-only, iris ADC value)
 * ============================================================================ */
#define MS41908M_IRSAD_MASK             (0x03FFU)   /* [9:0] Iris ADC value */

/* ============================================================================
 * Register 0x0E – TGT_UPDATE (average speed, target update)
 * ============================================================================ */
#define MS41908M_TGT_UPDATE_MASK        (0x00FFU)   /* [7:0] Target update rate */
#define MS41908M_AVE_SPEED_SHIFT        (8U)
#define MS41908M_AVE_SPEED_MASK         (31U << 8)  /* [12:8] Average speed */

/* ============================================================================
 * Register 0x20 – PWM_CFG (PWM resolution, mode, dead-time 1)
 * ============================================================================ */
#define MS41908M_DT1_MASK               (0x00FFU)   /* [7:0] Dead-time 1 */
#define MS41908M_PWMMODE_SHIFT          (8U)
#define MS41908M_PWMMODE_MASK           (31U << 8)  /* [12:8] PWM mode */
#define MS41908M_PWMRES_SHIFT           (13U)
#define MS41908M_PWMRES_MASK            (3U << 13) /* [14:13] PWM resolution */

/* ============================================================================
 * Register 0x21 – TEST_CFG
 * ============================================================================ */
#define MS41908M_FZTEST_MASK            (31U << 0)  /* [4:0] FZ test value */
#define MS41908M_TESTEN2_SHIFT          (7U)
#define MS41908M_TESTEN2_MASK           (1U << 7)   /* [7] Test enable 2 */

/* ============================================================================
 * Register 0x22 – MOTOR_AB_DT2 (phase mode AB, dead-time 2A)
 * ============================================================================ */
#define MS41908M_DT2A_MASK              (0x00FFU)   /* [7:0] Dead-time 2A */
#define MS41908M_PHMODAB_SHIFT          (8U)
#define MS41908M_PHMODAB_MASK           (63U << 8)  /* [13:8] Phase mode AB */

/* ============================================================================
 * Register 0x23 – MOTOR_AB_PPW (PPWA, PPWB)
 * ============================================================================ */
#define MS41908M_PPWA_MASK              (0x00FFU)   /* [7:0] Phase pulse width A */
#define MS41908M_PPWB_SHIFT             (8U)
#define MS41908M_PPWB_MASK              (0xFFU << 8)/* [15:8] Phase pulse width B */

/* ============================================================================
 * Register 0x24 – MOTOR_AB_STEP (Focus/Zoom step control)
 * ============================================================================ */
#define MS41908M_PSUMAB_MASK            (0x00FFU)   /* [7:0] Step sum (pulse count) */
#define MS41908M_CCWCWAB_SHIFT          (8U)
#define MS41908M_CCWCWAB_MASK           (1U << 8)   /* [8] Direction: 0=CW, 1=CCW */
#define MS41908M_BRAKEAB_SHIFT          (9U)
#define MS41908M_BRAKEAB_MASK           (1U << 9)   /* [9] Brake */
#define MS41908M_ENDISAB_SHIFT          (10U)
#define MS41908M_ENDISAB_MASK           (1U << 10)  /* [10] Output enable */
#define MS41908M_LEDB_SHIFT             (11U)
#define MS41908M_LEDB_MASK              (1U << 11)  /* [11] LED B / PI control */
#define MS41908M_MICROAB_SHIFT          (12U)
#define MS41908M_MICROAB_MASK           (3U << 12) /* [13:12] Micro step mode (e.g. 256/128/64) */

/* ============================================================================
 * Register 0x25 – MOTOR_AB_PPS (pulses per step, AB channel)
 * ============================================================================ */
#define MS41908M_INTCTAB_MASK           (0xFFFFU)   /* [15:0] Pulse count / PPS */

/* ============================================================================
 * Register 0x27 – MOTOR_CD_DT2
 * ============================================================================ */
#define MS41908M_DT2B_MASK              (0x00FFU)
#define MS41908M_PHMODCD_SHIFT          (8U)
#define MS41908M_PHMODCD_MASK           (63U << 8)

/* ============================================================================
 * Register 0x28 – MOTOR_CD_PPW
 * ============================================================================ */
#define MS41908M_PPWC_MASK              (0x00FFU)
#define MS41908M_PPWD_SHIFT             (8U)
#define MS41908M_PPWD_MASK              (0xFFU << 8)

/* ============================================================================
 * Register 0x29 – MOTOR_CD_STEP (Zoom/Focus step control)
 * ============================================================================ */
#define MS41908M_PSUMCD_MASK            (0x00FFU)
#define MS41908M_CCWCWCD_SHIFT          (8U)
#define MS41908M_CCWCWCD_MASK           (1U << 8)
#define MS41908M_BRAKECD_SHIFT          (9U)
#define MS41908M_BRAKECD_MASK           (1U << 9)
#define MS41908M_ENDISCD_SHIFT          (10U)
#define MS41908M_ENDISCD_MASK           (1U << 10)
#define MS41908M_LEDA_SHIFT             (11U)
#define MS41908M_LEDA_MASK              (1U << 11)
#define MS41908M_MICROCD_SHIFT          (12U)
#define MS41908M_MICROCD_MASK           (3U << 12)

/* ============================================================================
 * Register 0x2A – MOTOR_CD_PPS
 * ============================================================================ */
#define MS41908M_INTCTCD_MASK           (0xFFFFU)

/* ============================================================================
 * Step register helper bits (same for AB and CD)
 * ============================================================================ */
#define MS41908M_STEP_CCWCW_BIT         (1U << 8)   /* Direction: 0=CW, 1=CCW */
#define MS41908M_STEP_BRAKE_BIT         (1U << 9)
#define MS41908M_STEP_ENDIS_BIT         (1U << 10)  /* Enable driver output */
#define MS41908M_STEP_LED_BIT           (1U << 11)  /* LED / PI control */
#define MS41908M_STEP_MICRO_MASK        (3U << 12)  /* Micro step: 00=256, 10=128, 11=64 */
#define MS41908M_MICRO_256              (0U << 12)
#define MS41908M_MICRO_128              (2U << 12)
#define MS41908M_MICRO_64               (3U << 12)

#ifdef __cplusplus
}
#endif

#endif /* __MS41908M_REG_H__ */
