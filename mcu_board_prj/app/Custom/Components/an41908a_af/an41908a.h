/**
 * @file    an41908a.h
 * @brief   AN41908A AF lens driver for STM32G0
 */

#ifndef __AN41908A_H__
#define __AN41908A_H__

#include "sys_config.h"
#include <stdint.h>
#include <stdbool.h>

/* AN41908A Register Addresses */
/* Iris control registers (0x00-0x0E) */
#define AN41908A_REG_IRS_TGT        0x00
#define AN41908A_REG_IRIS_CFG1      0x01
#define AN41908A_REG_IRIS_CFG2      0x02
#define AN41908A_REG_IRIS_CFG3      0x03
#define AN41908A_REG_HALL_CFG       0x04
#define AN41908A_REG_IRIS_CFG4      0x05
#define AN41908A_REG_PULSE1_START   0x06
#define AN41908A_REG_PULSE1_WIDTH   0x07
#define AN41908A_REG_PULSE2_START   0x08
#define AN41908A_REG_PULSE2_WIDTH   0x09
#define AN41908A_REG_IRIS_TEST      0x0A
#define AN41908A_REG_MODE_CFG       0x0B
#define AN41908A_REG_TGT_UPDATE     0x0E

/* Motor control registers (0x20-0x2A) */
#define AN41908A_REG_PWM_CFG        0x20    /* DT1, PWMMODE, PWMRES */
#define AN41908A_REG_TEST_CFG       0x21    /* TESTEN2, FZTEST */
#define AN41908A_REG_MOTOR_AB_DT2   0x22    /* DT2A, PHMODAB */
#define AN41908A_REG_MOTOR_AB_PPW   0x23    /* PPWA, PPWB */
#define AN41908A_REG_MOTOR_AB_STEP  0x24    /* PSUMAB, CCWCWAB, BRAKEAB, ENDISAB, MICROAB */
#define AN41908A_REG_MOTOR_AB_PPS   0x25    /* INTCTAB */
#define AN41908A_REG_MOTOR_CD_DT2   0x27    /* DT2B, PHMODCD */
#define AN41908A_REG_MOTOR_CD_PPW   0x28    /* PPWC, PPWD */
#define AN41908A_REG_MOTOR_CD_STEP  0x29    /* PSUMCD, CCWCWCD, BRAKECD, ENDISCD, MICROCD */
#define AN41908A_REG_MOTOR_CD_PPS   0x2A    /* INTCTCD */

/* Motor assignment: AB=Focus, CD=Zoom */
#define AN41908A_FOCUS_STEPS_REG    0x24
#define AN41908A_FOCUS_PPS_REG      0x25
#define AN41908A_ZOOM_STEPS_REG     0x29
#define AN41908A_ZOOM_PPS_REG       0x2A

/* Step register bits */
#define AN41908A_STEP_CCWCW_BIT     (1 << 8)    /* Direction: 0=CW, 1=CCW */
#define AN41908A_STEP_BRAKE_BIT     (1 << 9)    /* Brake */
#define AN41908A_STEP_ENDIS_BIT     (1 << 10)   /* Enable output */
#define AN41908A_STEP_LED_BIT       (1 << 11)   /* LED output / PI control */
#define AN41908A_STEP_MICRO_MASK    (3 << 12)   /* Micro step mode */

/* Micro step modes */
#define AN41908A_MICRO_256          (0 << 12)
#define AN41908A_MICRO_128          (2 << 12)
#define AN41908A_MICRO_64           (3 << 12)

/* Motor direction */
#define AF_DIR_CW                   0
#define AF_DIR_CCW                  1

/* PI (Photo Interrupter) status */
#define PI_STATUS_LOW               0   /* PI blocked / at limit */
#define PI_STATUS_HIGH              1   /* PI not blocked */

/* Travel limits (in 8-step units, adjustable per lens) */
#define AF_ZOOM_MAX_IDX             500
#define AF_ZOOM_MIN_IDX             (-500)
#define AF_FOCUS_MAX_IDX            700
#define AF_FOCUS_MIN_IDX            (-1400)

/* Motor mode */
typedef enum {
    AF_MODE_NONE = -1,
    AF_MODE_ZOOM = 0,
    AF_MODE_FOCUS,
    AF_MODE_BOTH,
} af_mode_e;

/* Motor status */
typedef enum {
    AF_STATUS_IDLE = 0,
    AF_STATUS_BUSY,
    AF_STATUS_ERROR,
} af_status_e;

/* AF control structure */
typedef struct {
    int32_t     zm_idx;         /* Zoom position index */
    int32_t     fs_idx;         /* Focus position index */
    uint16_t    zm_steps;       /* Zoom steps register value */
    uint16_t    fs_steps;       /* Focus steps register value */
    uint32_t    dist_zm;        /* Zoom distance in micro steps */
    uint32_t    dist_fs;        /* Focus distance in micro steps */
    uint32_t    vd_psum_zm;     /* Zoom PSUM value */
    uint32_t    vd_psum_fs;     /* Focus PSUM value */
    int32_t     vd_num_zm;      /* Zoom VD count */
    int32_t     vd_num_fs;      /* Focus VD count */
    uint8_t     zm_busy;        /* Zoom busy flag */
    uint8_t     fs_busy;        /* Focus busy flag */
    af_mode_e   af_mode;        /* Current AF mode */
} an41908a_ctrl_t;

/* Configuration structure */
typedef struct {
    SPI_HandleTypeDef   *hspi;
    /* SPI CS */
    GPIO_TypeDef        *cs_port;
    uint16_t            cs_pin;
    /* VD sync signals */
    GPIO_TypeDef        *vd_is_port;
    uint16_t            vd_is_pin;
    GPIO_TypeDef        *vd_fz_port;
    uint16_t            vd_fz_pin;
    /* Chip reset */
    GPIO_TypeDef        *rstb_port;
    uint16_t            rstb_pin;
    /* Lens power enable */
    GPIO_TypeDef        *lens_en_port;
    uint16_t            lens_en_pin;
    /* PLS1/PLS2 - Motor completion pulse (optional, for IRQ) */
    GPIO_TypeDef        *pls1_port;     /* Focus PLS */
    uint16_t            pls1_pin;
    GPIO_TypeDef        *pls2_port;     /* Zoom PLS */
    uint16_t            pls2_pin;
    /* PI - Photo Interrupter / Optical limit switch */
    GPIO_TypeDef        *pi_zm_port;    /* Zoom PI (Z_RST) */
    uint16_t            pi_zm_pin;
    GPIO_TypeDef        *pi_fs_port;    /* Focus PI (F_RST) */
    uint16_t            pi_fs_pin;
} an41908a_cfg_t;

/* API Functions */
int an41908a_init(an41908a_cfg_t *cfg);
void an41908a_deinit(void);

/* Register access */
int an41908a_write_reg(uint8_t addr, uint16_t data);
int an41908a_read_reg(uint8_t addr, uint16_t *data);

/* Motor control */
int an41908a_zoom_run(uint16_t pps, int32_t distance);
int an41908a_focus_run(uint16_t pps, int32_t distance);
int an41908a_zoom_stop(void);
int an41908a_focus_stop(void);
int an41908a_stop_all(void);

/* Status */
bool an41908a_zoom_is_busy(void);
bool an41908a_focus_is_busy(void);
int32_t an41908a_get_zoom_pos(void);
int32_t an41908a_get_focus_pos(void);

/* PI (Photo Interrupter) - Optical limit switch */
int an41908a_pi_init(void);             /* Move to home position using PI */
uint8_t an41908a_read_pi_zoom(void);    /* Read zoom PI state */
uint8_t an41908a_read_pi_focus(void);   /* Read focus PI state */

/* VD pulse output */
void an41908a_vd_pulse_fz(void);
void an41908a_vd_pulse_is(void);

/* IRQ handler (call from PLS1/PLS2 EXTI interrupt) */
void an41908a_irq_handler(uint8_t motor);

#endif /* __AN41908A_H__ */
