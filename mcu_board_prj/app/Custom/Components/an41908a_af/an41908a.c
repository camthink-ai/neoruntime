/**
 * @file    an41908a.c
 * @brief   AN41908A AF lens driver for STM32G0
 * @note    Based on reference driver from Milesight
 */

#include "an41908a.h"
#include "iwdg.h"
#include <string.h>

/* Macros */
#define AF_ABS(a)               (((a) < 0) ? -(a) : (a))

/* Micro step configuration - 256 step mode */
#define MICRO_STEP_MODE         256
#define PSUM_TO_STEP            8       /* 256-step: PSUMxx * 8 = step count */
#define DISTANCE_TO_MICRO       32      /* 8-step to 256-step conversion */
#define MICROXX                 AN41908A_MICRO_256

/* AN41908A clock and timing */
#define AN41908A_OSCIN          27000000UL  /* 27MHz */
#define VD_FREQ                 16          /* VD frequency in Hz */
#define MICRO_8_STEP            8
#define VD_MDELAY               (1000 / VD_FREQ)  /* ~62ms per VD cycle */

/* Step register default: ENDIS=1 (output on), LED=1 (PI control), MICRO=256 */
#define STEP_REG_DEFAULT        (AN41908A_STEP_ENDIS_BIT | AN41908A_STEP_LED_BIT | MICROXX)

/* VD calculation macros (from reference driver) */
#define VD_PSUMXX(pps)          ((pps) <= 0 ? 1 : \
                                 (((32 * ((pps) / MICRO_8_STEP) / VD_FREQ) >= 1) ? \
                                  (32 * ((pps) / MICRO_8_STEP) / VD_FREQ) : 1))
#define VD_INTCTXX(psumxx)      (AN41908A_OSCIN / VD_FREQ / ((psumxx) * 24))

/* Private variables */
static an41908a_cfg_t *g_cfg = NULL;
static an41908a_ctrl_t g_ctrl = {0};
static volatile uint8_t g_initialized = 0;

/* IRQ done flags - set by PLS1/PLS2 interrupt */
static volatile uint8_t g_zm_irq_dn = 0;
static volatile uint8_t g_fs_irq_dn = 0;

/* Cached PPS values */
static uint32_t g_vd_psumxx_zm = 0;
static uint32_t g_vd_intctxx_zm = 0;
static uint32_t g_vd_psumxx_fs = 0;
static uint32_t g_vd_intctxx_fs = 0;

/* SPI timeout */
#define SPI_TIMEOUT             100

/* Private functions */
static void delay_us(uint32_t us)
{
    uint32_t cycles = us * (SystemCoreClock / 1000000) / 5;
    while (cycles--) {
        __NOP();
    }
}

static void delay_ms(uint32_t ms)
{
    osDelay(ms);
}

static void cs_low(void)
{
    HAL_GPIO_WritePin(g_cfg->cs_port, g_cfg->cs_pin, GPIO_PIN_RESET);
}

static void cs_high(void)
{
    HAL_GPIO_WritePin(g_cfg->cs_port, g_cfg->cs_pin, GPIO_PIN_SET);
}

static void vd_pulse_out(GPIO_TypeDef *port, uint16_t pin)
{
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_SET);
    delay_us(5);
    HAL_GPIO_WritePin(port, pin, GPIO_PIN_RESET);
}

/**
 * @brief Wait for zoom motor to complete one VD cycle
 */
static void wait_zm_stop(void)
{
    int timeout = (VD_MDELAY + 10) * 2;

    while (!g_zm_irq_dn && timeout > 0) {
        delay_us(500);
        timeout--;
    }
}

/**
 * @brief Wait for focus motor to complete one VD cycle
 */
static void wait_fs_stop(void)
{
    int timeout = (VD_MDELAY + 10) * 2;

    while (!g_fs_irq_dn && timeout > 0) {
        delay_us(500);
        timeout--;
    }
}

/**
 * @brief Write register to AN41908A
 */
int an41908a_write_reg(uint8_t addr, uint16_t data)
{
    uint8_t tx_buf[3];
    HAL_StatusTypeDef status;

    if (!g_initialized || !g_cfg) {
        return -1;
    }

    tx_buf[0] = addr & 0x3F;
    tx_buf[1] = data & 0xFF;
    tx_buf[2] = (data >> 8) & 0xFF;

    cs_low();
    delay_us(1);
    cs_high();
    delay_us(1);

    status = HAL_SPI_Transmit(g_cfg->hspi, tx_buf, 3, SPI_TIMEOUT);

    delay_us(1);
    cs_low();

    return (status == HAL_OK) ? 0 : -1;
}

/**
 * @brief Read register from AN41908A
 */
int an41908a_read_reg(uint8_t addr, uint16_t *data)
{
    uint8_t tx_buf[3] = {0};
    uint8_t rx_buf[3] = {0};
    HAL_StatusTypeDef status;

    if (!g_initialized || !g_cfg || !data) {
        return -1;
    }

    tx_buf[0] = (addr & 0x3F) | 0xC0;

    cs_low();
    delay_us(1);
    cs_high();
    delay_us(1);

    status = HAL_SPI_TransmitReceive(g_cfg->hspi, tx_buf, rx_buf, 3, SPI_TIMEOUT);

    delay_us(1);
    cs_low();

    if (status == HAL_OK) {
        *data = rx_buf[1] | ((uint16_t)rx_buf[2] << 8);
        return 0;
    }

    return -1;
}

/**
 * @brief Initialize motor control registers
 */
static int an41908a_motor_init(void)
{
    g_ctrl.zm_steps = STEP_REG_DEFAULT;
    g_ctrl.fs_steps = STEP_REG_DEFAULT;

    /* 0x20: PWM config - DT1=0x0A, PWMMODE=28, PWMRES=2 -> 30.1kHz */
    an41908a_write_reg(AN41908A_REG_PWM_CFG, 0x5C0A);

    /* 0x21: Test config - TESTEN2=1, FZTEST=7 (H output during rotation) */
    an41908a_write_reg(AN41908A_REG_TEST_CFG, 0x0087);

    /* === Focus (Motor AB) === */
    an41908a_write_reg(AN41908A_REG_MOTOR_AB_DT2, 0x0003);
    an41908a_write_reg(AN41908A_REG_MOTOR_AB_PPW, 0xC8C8);
    an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
    an41908a_write_reg(AN41908A_FOCUS_PPS_REG, 0x0502);

    /* === Zoom (Motor CD) === */
    an41908a_write_reg(AN41908A_REG_MOTOR_CD_DT2, 0x1603);
    an41908a_write_reg(AN41908A_REG_MOTOR_CD_PPW, 0xC8C8);
    an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);
    an41908a_write_reg(AN41908A_ZOOM_PPS_REG, 0x0400);

    return 0;
}

/**
 * @brief Initialize Iris control registers
 */
static int an41908a_iris_init(void)
{
    an41908a_write_reg(AN41908A_REG_IRS_TGT, 0x0000);
    an41908a_write_reg(AN41908A_REG_IRIS_CFG1, 0x608A);
    an41908a_write_reg(AN41908A_REG_IRIS_CFG2, 0x66F0);
    an41908a_write_reg(AN41908A_REG_IRIS_CFG3, 0x0E10);
    an41908a_write_reg(AN41908A_REG_HALL_CFG, 0x804C);
    an41908a_write_reg(AN41908A_REG_IRIS_CFG4, 0x0504);
    an41908a_write_reg(AN41908A_REG_IRIS_TEST, 0x0080);
    an41908a_write_reg(AN41908A_REG_MODE_CFG, 0x0480);
    an41908a_write_reg(AN41908A_REG_TGT_UPDATE, 0x0C00);

    an41908a_vd_pulse_is();

    return 0;
}

/**
 * @brief Initialize AN41908A driver
 */
int an41908a_init(an41908a_cfg_t *cfg)
{
    if (!cfg || !cfg->hspi) {
        return -1;
    }

    g_cfg = cfg;
    memset((void *)&g_ctrl, 0, sizeof(g_ctrl));

    cs_low();

    /* Enable lens power */
    if (g_cfg->lens_en_port) {
        HAL_GPIO_WritePin(g_cfg->lens_en_port, g_cfg->lens_en_pin, GPIO_PIN_RESET);
    }
    delay_ms(10);

    /* Reset chip */
    if (g_cfg->rstb_port) {
        HAL_GPIO_WritePin(g_cfg->rstb_port, g_cfg->rstb_pin, GPIO_PIN_RESET);
        delay_ms(10);
        HAL_GPIO_WritePin(g_cfg->rstb_port, g_cfg->rstb_pin, GPIO_PIN_SET);
        delay_ms(50);
    }

    g_initialized = 1;

    an41908a_motor_init();
    an41908a_iris_init();
    an41908a_vd_pulse_fz();

    return 0;
}

void an41908a_deinit(void)
{
    if (g_cfg) {
        an41908a_stop_all();
        if (g_cfg->lens_en_port) {
            HAL_GPIO_WritePin(g_cfg->lens_en_port, g_cfg->lens_en_pin, GPIO_PIN_RESET);
        }
    }
    g_initialized = 0;
    g_cfg = NULL;
}

void an41908a_vd_pulse_fz(void)
{
    if (g_cfg && g_cfg->vd_fz_port) {
        vd_pulse_out(g_cfg->vd_fz_port, g_cfg->vd_fz_pin);
    }
}

void an41908a_vd_pulse_is(void)
{
    if (g_cfg && g_cfg->vd_is_port) {
        vd_pulse_out(g_cfg->vd_is_port, g_cfg->vd_is_pin);
    }
}

/**
 * @brief Run zoom motor (blocking)
 * @param pps       Pulse per second (speed), e.g., 800
 * @param distance  Steps to move in 8-step units (positive=in, negative=out)
 * @return 0=success, -1=error, -3=PI limit triggered
 */
int an41908a_zoom_run(uint16_t pps, int32_t distance)
{
    uint8_t dir;
    uint32_t dist_micro;
    uint32_t psum_val;
    int32_t vd_num;
    int ret = 0;

    if (!g_initialized || distance == 0) {
        return -1;
    }

    g_ctrl.af_mode = AF_MODE_ZOOM;
    g_ctrl.zm_busy = 1;

    dir = (distance > 0) ? AF_DIR_CW : AF_DIR_CCW;
    dist_micro = AF_ABS(distance) * DISTANCE_TO_MICRO;
    g_ctrl.dist_zm = dist_micro;

    /* Calculate PPS parameters */
    g_vd_psumxx_zm = VD_PSUMXX(pps);
    if (g_vd_psumxx_zm > 127) g_vd_psumxx_zm = 127;
    if (g_vd_psumxx_zm < 1) g_vd_psumxx_zm = 1;
    g_vd_intctxx_zm = VD_INTCTXX(g_vd_psumxx_zm);

    /* Set PPS register */
    an41908a_write_reg(AN41908A_ZOOM_PPS_REG, (uint16_t)g_vd_intctxx_zm);

    /* Calculate VD cycles needed */
    psum_val = g_vd_psumxx_zm;
    vd_num = dist_micro / (psum_val * PSUM_TO_STEP);
    g_ctrl.vd_psum_zm = psum_val;
    g_ctrl.vd_num_zm = vd_num;

    /* Read current steps register */
    an41908a_read_reg(AN41908A_ZOOM_STEPS_REG, &g_ctrl.zm_steps);

    /* Run VD cycles */
    while (vd_num > 0 || dist_micro > 0) {
        g_zm_irq_dn = 0;

        /* Check PI limit during run */
        // if (g_cfg->pi_zm_port && an41908a_read_pi_zoom() == PI_STATUS_LOW) {
        //     ret = -3;  /* PI limit triggered */
        //     break;
        // }

        /* Build steps register */
        g_ctrl.zm_steps &= 0xFE00;
        if (dir == AF_DIR_CCW) {
            g_ctrl.zm_steps |= AN41908A_STEP_CCWCW_BIT;
        }

        if (vd_num > 0) {
            g_ctrl.zm_steps |= (psum_val & 0x7F);
            dist_micro -= psum_val * PSUM_TO_STEP;
            vd_num--;
        } else {
            /* Last partial cycle */
            uint32_t remain = dist_micro / PSUM_TO_STEP;
            if (remain > 0) {
                g_ctrl.zm_steps |= (remain & 0x7F);
                dist_micro = 0;
            } else {
                break;
            }
        }

        an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);
        an41908a_vd_pulse_fz();

        /* Wait for completion */
        wait_zm_stop();

        /* Update position */
        g_ctrl.zm_idx += (dir == AF_DIR_CW) ? 1 : -1;
    }

    /* Clear PSUM to stop */
    g_ctrl.zm_steps &= 0xFF00;
    an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);
    an41908a_vd_pulse_fz();

    g_ctrl.zm_busy = 0;
    return ret;
}

/**
 * @brief Run focus motor (blocking)
 * @param pps       Pulse per second (speed), e.g., 800
 * @param distance  Steps to move in 8-step units (positive=near, negative=far)
 * @return 0=success, -1=error, -3=PI limit triggered
 */
int an41908a_focus_run(uint16_t pps, int32_t distance)
{
    uint8_t dir;
    uint32_t dist_micro;
    uint32_t psum_val;
    int32_t vd_num;
    int ret = 0;

    if (!g_initialized || distance == 0) {
        return -1;
    }

    g_ctrl.af_mode = AF_MODE_FOCUS;
    g_ctrl.fs_busy = 1;

    dir = (distance > 0) ? AF_DIR_CW : AF_DIR_CCW;
    dist_micro = AF_ABS(distance) * DISTANCE_TO_MICRO;
    g_ctrl.dist_fs = dist_micro;

    /* Calculate PPS parameters */
    g_vd_psumxx_fs = VD_PSUMXX(pps);
    if (g_vd_psumxx_fs > 127) g_vd_psumxx_fs = 127;
    if (g_vd_psumxx_fs < 1) g_vd_psumxx_fs = 1;
    g_vd_intctxx_fs = VD_INTCTXX(g_vd_psumxx_fs);

    /* Set PPS register */
    an41908a_write_reg(AN41908A_FOCUS_PPS_REG, (uint16_t)g_vd_intctxx_fs);

    /* Calculate VD cycles needed */
    psum_val = g_vd_psumxx_fs;
    vd_num = dist_micro / (psum_val * PSUM_TO_STEP);
    g_ctrl.vd_psum_fs = psum_val;
    g_ctrl.vd_num_fs = vd_num;

    /* Read current steps register */
    an41908a_read_reg(AN41908A_FOCUS_STEPS_REG, &g_ctrl.fs_steps);

    /* Run VD cycles */
    while (vd_num > 0 || dist_micro > 0) {
        g_fs_irq_dn = 0;

        /* Check PI limit during run */
        // if (g_cfg->pi_fs_port && an41908a_read_pi_focus() == PI_STATUS_LOW) {
        //     ret = -3;  /* PI limit triggered */
        //     break;
        // }

        /* Build steps register */
        g_ctrl.fs_steps &= 0xFE00;
        if (dir == AF_DIR_CCW) {
            g_ctrl.fs_steps |= AN41908A_STEP_CCWCW_BIT;
        }

        if (vd_num > 0) {
            g_ctrl.fs_steps |= (psum_val & 0x7F);
            dist_micro -= psum_val * PSUM_TO_STEP;
            vd_num--;
        } else {
            uint32_t remain = dist_micro / PSUM_TO_STEP;
            if (remain > 0) {
                g_ctrl.fs_steps |= (remain & 0x7F);
                dist_micro = 0;
            } else {
                break;
            }
        }

        an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
        an41908a_vd_pulse_fz();

        wait_fs_stop();

        /* Update position */
        g_ctrl.fs_idx += (dir == AF_DIR_CW) ? 1 : -1;
    }

    /* Clear PSUM to stop */
    g_ctrl.fs_steps &= 0xFF00;
    an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
    an41908a_vd_pulse_fz();

    g_ctrl.fs_busy = 0;
    return ret;
}

/**
 * @brief Stop zoom motor
 */
int an41908a_zoom_stop(void)
{
    if (!g_initialized) {
        return -1;
    }

    an41908a_read_reg(AN41908A_ZOOM_STEPS_REG, &g_ctrl.zm_steps);
    g_ctrl.zm_steps &= 0xFF00;
    an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);
    an41908a_vd_pulse_fz();

    g_ctrl.zm_busy = 0;
    return 0;
}

/**
 * @brief Stop focus motor
 */
int an41908a_focus_stop(void)
{
    if (!g_initialized) {
        return -1;
    }

    an41908a_read_reg(AN41908A_FOCUS_STEPS_REG, &g_ctrl.fs_steps);
    g_ctrl.fs_steps &= 0xFF00;
    an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
    an41908a_vd_pulse_fz();

    g_ctrl.fs_busy = 0;
    return 0;
}

int an41908a_stop_all(void)
{
    an41908a_zoom_stop();
    an41908a_focus_stop();
    return 0;
}

bool an41908a_zoom_is_busy(void)
{
    return g_ctrl.zm_busy != 0;
}

bool an41908a_focus_is_busy(void)
{
    return g_ctrl.fs_busy != 0;
}

int32_t an41908a_get_zoom_pos(void)
{
    return g_ctrl.zm_idx;
}

int32_t an41908a_get_focus_pos(void)
{
    return g_ctrl.fs_idx;
}

/**
 * @brief Read zoom PI (Photo Interrupter) state
 * @return PI_STATUS_LOW (0) or PI_STATUS_HIGH (1)
 */
uint8_t an41908a_read_pi_zoom(void)
{
    if (g_cfg && g_cfg->pi_zm_port) {
        return HAL_GPIO_ReadPin(g_cfg->pi_zm_port, g_cfg->pi_zm_pin) == GPIO_PIN_SET ?
               PI_STATUS_HIGH : PI_STATUS_LOW;
    }
    return PI_STATUS_HIGH;
}

/**
 * @brief Read focus PI (Photo Interrupter) state
 * @return PI_STATUS_LOW (0) or PI_STATUS_HIGH (1)
 */
uint8_t an41908a_read_pi_focus(void)
{
    if (g_cfg && g_cfg->pi_fs_port) {
        return HAL_GPIO_ReadPin(g_cfg->pi_fs_port, g_cfg->pi_fs_pin) == GPIO_PIN_SET ?
               PI_STATUS_HIGH : PI_STATUS_LOW;
    }
    return PI_STATUS_HIGH;
}

/**
 * @brief Initialize motors to home position using PI (Photo Interrupter)
 * @note  Three-phase PI initialization based on reference driver:
 *        Phase 1: Fast run to PI=HIGH side
 *        Phase 2: Fast run to PI=LOW side
 *        Phase 3: Slow run to PI=HIGH side (precise home position)
 * @return 0 on success, -1 on focus PI timeout, -2 on zoom PI timeout
 */
int an41908a_pi_init(void)
{
    uint8_t fpi, zpi;
    uint8_t bfs_pi, bzm_pi;
    uint32_t ftmp, ztmp;
    uint32_t fcnt, zcnt;
    uint16_t fast_speed = 0x80;  /* Fast speed for PI search */
    uint16_t slow_speed = 0x10;  /* Slow speed for precise positioning */
    uint16_t intct_fast;

    if (!g_initialized) {
        return -1;
    }

    /* Calculate fast PPS */
    intct_fast = (uint16_t)VD_INTCTXX(VD_PSUMXX(800));

    /* Set max travel count (prevent infinite loop) */
    fcnt = 50000;
    zcnt = 50000;

    g_ctrl.fs_busy = 1;
    g_ctrl.zm_busy = 1;

    /* Read steps registers */
    an41908a_read_reg(AN41908A_FOCUS_STEPS_REG, &g_ctrl.fs_steps);
    an41908a_read_reg(AN41908A_ZOOM_STEPS_REG, &g_ctrl.zm_steps);

    /* Set fast PPS */
    an41908a_write_reg(AN41908A_FOCUS_PPS_REG, intct_fast);
    an41908a_write_reg(AN41908A_ZOOM_PPS_REG, intct_fast);

    /* Initial PI check with PULSE_ON */
    g_ctrl.fs_steps &= 0xFE00;
    g_ctrl.fs_steps |= AN41908A_STEP_LED_BIT;  /* PULSE_ON - PI control switch */
    g_ctrl.zm_steps &= 0xFE00;
    g_ctrl.zm_steps |= AN41908A_STEP_LED_BIT;
    an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
    an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);
    an41908a_vd_pulse_fz();
    delay_ms(100);

    /* ========== Phase 1: Fast run to PI=HIGH side ========== */
    ftmp = 0;
    ztmp = 0;
    bfs_pi = 0;
    bzm_pi = 0;

    fpi = an41908a_read_pi_focus();
    zpi = an41908a_read_pi_zoom();

    /* Set direction: if PI=LOW, set CCW; if PI=HIGH, clear direction (CW) */
    g_ctrl.fs_steps &= 0xFE00;
    g_ctrl.fs_steps |= AN41908A_STEP_LED_BIT | fast_speed;
    if (fpi == PI_STATUS_LOW) {
        g_ctrl.fs_steps |= AN41908A_STEP_CCWCW_BIT;
    }

    g_ctrl.zm_steps &= 0xFE00;
    g_ctrl.zm_steps |= AN41908A_STEP_LED_BIT | fast_speed;
    if (zpi == PI_STATUS_LOW) {
        g_ctrl.zm_steps |= AN41908A_STEP_CCWCW_BIT;
    }

    an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
    an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);

    /* Run until both PI=HIGH */
    while (!bfs_pi || !bzm_pi) {
        g_fs_irq_dn = 0;
        g_zm_irq_dn = 0;

        if (!bfs_pi) {
            fpi = an41908a_read_pi_focus();
            if (fpi == PI_STATUS_HIGH) {
                bfs_pi = 1;
                g_ctrl.fs_steps &= 0xFF00;
                an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
            } else {
                ftmp += fast_speed;
            }
        }

        if (!bzm_pi) {
            zpi = an41908a_read_pi_zoom();
            if (zpi == PI_STATUS_HIGH) {
                bzm_pi = 1;
                g_ctrl.zm_steps &= 0xFF00;
                an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);
            } else {
                ztmp += fast_speed;
            }
        }

        if (ftmp >= fcnt || ztmp >= zcnt) {
            goto pi_fail;
        }

        an41908a_vd_pulse_fz();
        delay_ms(20);
        if (!bfs_pi) wait_fs_stop();
        if (!bzm_pi) wait_zm_stop();
        HAL_IWDG_Refresh(&hiwdg);
    }

    /* ========== Phase 2: Fast run to PI=LOW side ========== */
    ftmp = 0;
    ztmp = 0;
    bfs_pi = 0;
    bzm_pi = 0;

    fpi = an41908a_read_pi_focus();
    zpi = an41908a_read_pi_zoom();

    /* Set direction: if PI=HIGH, set CCW; if PI=LOW, clear direction (CW) */
    g_ctrl.fs_steps &= 0xFE00;
    g_ctrl.fs_steps |= AN41908A_STEP_LED_BIT | fast_speed;
    if (fpi == PI_STATUS_HIGH) {
        g_ctrl.fs_steps |= AN41908A_STEP_CCWCW_BIT;
    }

    g_ctrl.zm_steps &= 0xFE00;
    g_ctrl.zm_steps |= AN41908A_STEP_LED_BIT | fast_speed;
    if (zpi == PI_STATUS_HIGH) {
        g_ctrl.zm_steps |= AN41908A_STEP_CCWCW_BIT;
    }

    an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
    an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);

    /* Run until both PI=LOW */
    while (!bfs_pi || !bzm_pi) {
        g_fs_irq_dn = 0;
        g_zm_irq_dn = 0;

        if (!bfs_pi) {
            fpi = an41908a_read_pi_focus();
            if (fpi == PI_STATUS_LOW) {
                bfs_pi = 1;
                g_ctrl.fs_steps &= 0xFF00;
                an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
            } else {
                ftmp += fast_speed;
            }
        }

        if (!bzm_pi) {
            zpi = an41908a_read_pi_zoom();
            if (zpi == PI_STATUS_LOW) {
                bzm_pi = 1;
                g_ctrl.zm_steps &= 0xFF00;
                an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);
            } else {
                ztmp += fast_speed;
            }
        }

        if (ftmp >= fcnt || ztmp >= zcnt) {
            goto pi_fail;
        }

        an41908a_vd_pulse_fz();
        delay_ms(20);
        if (!bfs_pi) wait_fs_stop();
        if (!bzm_pi) wait_zm_stop();
        HAL_IWDG_Refresh(&hiwdg);
    }

    /* ========== Phase 3: Slow run to PI=HIGH side (precise home) ========== */
    ftmp = 0;
    ztmp = 0;
    bfs_pi = 0;
    bzm_pi = 0;

    fpi = an41908a_read_pi_focus();
    zpi = an41908a_read_pi_zoom();

    /* Set direction: if PI=LOW, set CCW; if PI=HIGH, clear direction (CW) */
    g_ctrl.fs_steps &= 0xFE00;
    g_ctrl.fs_steps |= AN41908A_STEP_LED_BIT | slow_speed;
    if (fpi == PI_STATUS_LOW) {
        g_ctrl.fs_steps |= AN41908A_STEP_CCWCW_BIT;
    }

    g_ctrl.zm_steps &= 0xFE00;
    g_ctrl.zm_steps |= AN41908A_STEP_LED_BIT | slow_speed;
    if (zpi == PI_STATUS_LOW) {
        g_ctrl.zm_steps |= AN41908A_STEP_CCWCW_BIT;
    }

    an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
    an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);

    /* Run until both PI=HIGH */
    while (!bfs_pi || !bzm_pi) {
        g_fs_irq_dn = 0;
        g_zm_irq_dn = 0;

        if (!bfs_pi) {
            fpi = an41908a_read_pi_focus();
            if (fpi == PI_STATUS_HIGH) {
                bfs_pi = 1;
                g_ctrl.fs_steps &= 0xFF00;
                g_ctrl.fs_steps &= ~AN41908A_STEP_LED_BIT;  /* Close PI control */
                an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
            } else {
                ftmp += slow_speed;
            }
        }

        if (!bzm_pi) {
            zpi = an41908a_read_pi_zoom();
            if (zpi == PI_STATUS_HIGH) {
                bzm_pi = 1;
                g_ctrl.zm_steps &= 0xFF00;
                g_ctrl.zm_steps &= ~AN41908A_STEP_LED_BIT;  /* Close PI control */
                an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);
            } else {
                ztmp += slow_speed;
            }
        }

        if (ftmp >= fcnt || ztmp >= zcnt) {
            goto pi_fail;
        }

        an41908a_vd_pulse_fz();
        delay_ms(10);
        if (!bfs_pi) wait_fs_stop();
        if (!bzm_pi) wait_zm_stop();
        HAL_IWDG_Refresh(&hiwdg);
    }

    /* Success - output final VD pulse */
    an41908a_vd_pulse_fz();

    g_ctrl.fs_busy = 0;
    g_ctrl.zm_busy = 0;
    g_ctrl.fs_idx = 0;
    g_ctrl.zm_idx = 0;

    return 0;

pi_fail:
    /* Stop motors */
    g_ctrl.fs_steps &= 0xFF00;
    g_ctrl.zm_steps &= 0xFF00;
    an41908a_write_reg(AN41908A_FOCUS_STEPS_REG, g_ctrl.fs_steps);
    an41908a_write_reg(AN41908A_ZOOM_STEPS_REG, g_ctrl.zm_steps);
    an41908a_vd_pulse_fz();

    g_ctrl.fs_busy = 0;
    g_ctrl.zm_busy = 0;

    return (ftmp >= fcnt) ? -1 : -2;
}

/**
 * @brief IRQ handler - call from PLS1/PLS2 EXTI interrupt
 * @param motor 0=zoom(PLS2), 1=focus(PLS1)
 */
void an41908a_irq_handler(uint8_t motor)
{
    if (motor == 0) {
        g_zm_irq_dn = 1;
    } else {
        g_fs_irq_dn = 1;
    }
}
