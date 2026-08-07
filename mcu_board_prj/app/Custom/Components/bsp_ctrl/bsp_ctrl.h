#ifndef __BSP_CTRL_H__
#define __BSP_CTRL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "sys_config.h"

#define BSP_CTRL_TASK_NAME              "bsp_ctrl"
#define BSP_CTRL_TASK_STACK_SIZE        (1536)
#define BSP_CTRL_TASK_PRIORITY          (2)

#define BSP_ADC_BUFFER_SIZE_MAX         (40)
/** Nominal VDDA (mV) for fallback only; runtime scale uses VREFINT-measured VDDA in bsp_ctrl_init(). */
#define BSP_ADC_REF_VOLTAGE_MV          (3300)

/* Set to 1 to invert the logical level for each GPIO-based peripheral channel. */
#if !defined(BSP_ALARM_OUT0_INVERT)
#define BSP_ALARM_OUT0_INVERT           (0U)
#endif

#if !defined(BSP_ALARM_OUT1_INVERT)
#define BSP_ALARM_OUT1_INVERT           (1U)
#endif

#if !defined(BSP_WIEGAND0_INVERT)
#define BSP_WIEGAND0_INVERT             (1U)
#endif

#if !defined(BSP_WIEGAND1_INVERT)
#define BSP_WIEGAND1_INVERT             (1U)
#endif

#if !defined(BSP_ALARM_IN0_INVERT)
#define BSP_ALARM_IN0_INVERT            (1U)
#endif

#if !defined(BSP_ALARM_IN1_INVERT)
#define BSP_ALARM_IN1_INVERT            (1U)
#endif

typedef enum {
    BSP_LED_IR_NEAR = 0,    // TIM1_CH1
    BSP_LED_IR_FAR,         // TIM1_CH2
    BSP_LED_WHITE2,         // TIM3_CH2
    BSP_LED_WHITE1,         // TIM3_CH1, white1 and ir1 share the same PWM channel
    BSP_LED_IR1,            // TIM3_CH1, ir1 and white2 share the same PWM channel
    BSP_LED_MAX,
} bsp_led_t;

typedef enum {
    BSP_ALARM_IN0 = 0,
    BSP_ALARM_IN1,
    BSP_ALARM_IN_MAX,
} bsp_alarm_in_t;

typedef enum {
    BSP_ALARM_OUT0 = 0,
    BSP_ALARM_OUT1,
    BSP_ALARM_OUT_MAX,
} bsp_alarm_out_t;

typedef enum {
    BSP_WIEGAND0 = 0,
    BSP_WIEGAND1,
    BSP_WIEGAND_MAX,
} bsp_wiegand_t;

typedef void (*bsp_alarm_in_callback_t)(bsp_alarm_in_t in, uint8_t level);

int bsp_ctrl_init(void);
int bsp_ctrl_set_led_duty(bsp_led_t led, uint8_t duty);
uint8_t bsp_ctrl_get_led_duty(bsp_led_t led);

int bsp_ctrl_set_ir_cut(uint8_t enable);
uint8_t bsp_ctrl_get_ir_cut(void);

uint16_t bsp_ctrl_get_pd_voltage_mv(void);
float bsp_ctrl_convert_pd_to_lux(uint16_t voltage_mv);
uint16_t bsp_ctrl_get_temp_voltage_mv(void);
float bsp_ctrl_convert_lmt87_to_celcius(uint16_t voltage_mv);

int bsp_ctrl_set_fan_duty(uint8_t duty);
uint8_t bsp_ctrl_get_fan_duty(void);

int bsp_ctrl_set_heat_duty(uint8_t duty);
uint8_t bsp_ctrl_get_heat_duty(void);

int bsp_ctrl_set_radar_en(uint8_t enable);
uint8_t bsp_ctrl_get_radar_en(void);

int bsp_ctrl_set_alarm_out(bsp_alarm_out_t out, uint8_t enable);
uint8_t bsp_ctrl_get_alarm_out(bsp_alarm_out_t out);

int bsp_ctrl_set_wiegand(bsp_wiegand_t wiegand, uint8_t enable);
uint8_t bsp_ctrl_get_wiegand(bsp_wiegand_t wiegand);

int bsp_ctrl_register_alarm_in(bsp_alarm_in_t in, bsp_alarm_in_callback_t callback);
int bsp_ctrl_get_alarm_in(bsp_alarm_in_t in);

int bsp_ctrl_reset_soc_power(void);

#ifdef __cplusplus
}
#endif

#endif /* __BSP_CTRL_H__ */
