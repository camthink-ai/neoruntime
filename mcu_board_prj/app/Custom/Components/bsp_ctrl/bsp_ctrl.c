#include <math.h>
#include "bsp_ctrl.h"
#include "adc.h"
#include "stm32g0xx_hal_adc_ex.h"
#include "tim.h"
#include "ms41908m_port.h"

#define BSP_EVENT_ADC_HALF_COMPLETE         (1 << 0)
#define BSP_EVENT_ADC_COMPLETE              (1 << 1)
#define BSP_EVENT_ADC_ERROR                 (1 << 2)
#define BSP_EVENT_ADC_ALL_BITS              (BSP_EVENT_ADC_HALF_COMPLETE | BSP_EVENT_ADC_COMPLETE | BSP_EVENT_ADC_ERROR)
#define BSP_EVENT_ALARM_IN0_RISING          (1 << 3)
#define BSP_EVENT_ALARM_IN0_FALLING         (1 << 4)
#define BSP_EVENT_ALARM_IN0_ALL_BITS        (BSP_EVENT_ALARM_IN0_RISING | BSP_EVENT_ALARM_IN0_FALLING)
#define BSP_EVENT_ALARM_IN1_RISING          (1 << 5)
#define BSP_EVENT_ALARM_IN1_FALLING         (1 << 6)
#define BSP_EVENT_ALARM_IN1_ALL_BITS        (BSP_EVENT_ALARM_IN1_RISING | BSP_EVENT_ALARM_IN1_FALLING)
#define BSP_EVENT_ALARM_IN_ALL_BITS         (BSP_EVENT_ALARM_IN0_ALL_BITS | BSP_EVENT_ALARM_IN1_ALL_BITS)

#define BSP_EVENT_ALL_BITS                  (0x00FFFFFF)

/**
 * Photo channel: R123 47k to ground (see PD_ADC schematic). μA_proxy = V_mV / R_kΩ.
 * It is on the same order of magnitude as the emitter current and varies monotonically with Ev.
 *
 * Datasheet LXD/GB5-A1ELB-1: Ev is highly linear with output current. At 20 lx (6500K white light),
 * the illumination current is about 150–300 µA under Vdd=5V and Rss=1k.
 *
 * For this board (3.3V + 47k + emitter follower), calibrate the proportionality by measurement
 * with an illuminance meter; update BSP_PD_LUX_PER_PROXY_UA accordingly.
 */
#define BSP_PD_R_EMITTER_KOHM               (47.0f)
#define BSP_PD_DARK_PROXY_UA                (0.0f)   /* Enter measured proxy uA under dark to set the zero point. */
#define BSP_PD_LUX_PER_PROXY_UA             (20.0f)  /* Calibration: Ev / max(0, I_proxy - dark) */
#define BSP_PD_LUX_CLAMP_MAX                (200000.0f)

static uint8_t s_led_duty[BSP_LED_MAX] = {0};
static int16_t s_adc_buffer[BSP_ADC_BUFFER_SIZE_MAX] = {0};

/** Effective VDDA (mV) for ADC scale; from VREFINT at init, fallback BSP_ADC_REF_VOLTAGE_MV. */
static uint32_t s_adc_vdda_mv = (uint32_t)BSP_ADC_REF_VOLTAGE_MV;

static EventGroupHandle_t s_event_group = NULL;
static bsp_alarm_in_callback_t s_alarm_in_callback[BSP_ALARM_IN_MAX] = {0};

static inline uint8_t bsp_ctrl_apply_invert(uint8_t level, uint8_t invert)
{
    return (invert != 0U) ? ((level != 0U) ? 0U : 1U) : level;
}

static inline uint8_t bsp_ctrl_get_alarm_out_invert_flag(bsp_alarm_out_t out)
{
    switch (out) {
        case BSP_ALARM_OUT1:
            return BSP_ALARM_OUT1_INVERT;
        case BSP_ALARM_OUT0:
        default:
            return BSP_ALARM_OUT0_INVERT;
    }
}

static inline uint8_t bsp_ctrl_get_wiegand_invert_flag(bsp_wiegand_t wiegand)
{
    switch (wiegand) {
        case BSP_WIEGAND1:
            return BSP_WIEGAND1_INVERT;
        case BSP_WIEGAND0:
        default:
            return BSP_WIEGAND0_INVERT;
    }
}

static inline uint8_t bsp_ctrl_get_alarm_in_invert_flag(bsp_alarm_in_t in)
{
    switch (in) {
        case BSP_ALARM_IN1:
            return BSP_ALARM_IN1_INVERT;
        case BSP_ALARM_IN0:
        default:
            return BSP_ALARM_IN0_INVERT;
    }
}

/**
 * Measure VDDA via internal VREFINT (factory cal @ 3.0 V), then restore Cube MX ADC1+DMA.
 * STM32 SAR full-scale is VDDA; assuming a fixed 3300 mV when VDDA is lower makes readings high (~100 mV typical).
 */
static void bsp_ctrl_adc_init_vdda_from_vrefint(void)
{
    uint32_t vdda_mv = 0U;
    uint32_t vref_raw = 0U;
    ADC_ChannelConfTypeDef sConfig = {0};

    (void)HAL_ADC_Stop_DMA(&hadc1);

    do {
        if (HAL_ADC_DeInit(&hadc1) != HAL_OK) {
            break;
        }

        hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
        hadc1.Init.NbrOfConversion = 1;
        hadc1.Init.ContinuousConvMode = DISABLE;
        hadc1.Init.DMAContinuousRequests = DISABLE;
        if (HAL_ADC_Init(&hadc1) != HAL_OK) {
            break;
        }

        sConfig.Channel = ADC_CHANNEL_VREFINT;
        sConfig.Rank = ADC_REGULAR_RANK_1;
        sConfig.SamplingTime = ADC_SAMPLINGTIME_COMMON_1;
        if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
            break;
        }

        HAL_Delay(2);

        if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK) {
            break;
        }
        if (HAL_ADC_Start(&hadc1) != HAL_OK) {
            break;
        }
        if (HAL_ADC_PollForConversion(&hadc1, 50) != HAL_OK) {
            (void)HAL_ADC_Stop(&hadc1);
            break;
        }

        vref_raw = HAL_ADC_GetValue(&hadc1);
        (void)HAL_ADC_Stop(&hadc1);

        vdda_mv = __HAL_ADC_CALC_VREFANALOG_VOLTAGE((uint16_t)vref_raw, ADC_RESOLUTION_12B);
    } while (0);

    (void)HAL_ADC_DeInit(&hadc1);
    (void)MX_ADC1_Init();

    if (vdda_mv >= 2500U && vdda_mv <= 3800U) {
        s_adc_vdda_mv = vdda_mv;
        WIC_LOGD("[bsp_ctrl] VDDA(from VREFINT) ~ %lu mV", (unsigned long)s_adc_vdda_mv);
    } else {
        s_adc_vdda_mv = (uint32_t)BSP_ADC_REF_VOLTAGE_MV;
        WIC_LOGW("[bsp_ctrl] VREFINT VDDA invalid (%lu mV), using BSP_ADC_REF_VOLTAGE_MV=%d",
                 (unsigned long)vdda_mv, BSP_ADC_REF_VOLTAGE_MV);
    }

    if (HAL_ADCEx_Calibration_Start(&hadc1) != HAL_OK) {
        WIC_LOGW("[bsp_ctrl] HAL_ADCEx_Calibration_Start failed after ADC restore");
    }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    HAL_ADC_Stop_DMA(hadc);
    xEventGroupSetBitsFromISR(s_event_group, BSP_EVENT_ADC_COMPLETE, NULL);
}

void HAL_ADC_ErrorCallback(ADC_HandleTypeDef *hadc)
{
    HAL_ADC_Stop_DMA(hadc);
    xEventGroupSetBitsFromISR(s_event_group, BSP_EVENT_ADC_ERROR, NULL);
}

void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == ALARM_IN0_Pin) {
        xEventGroupSetBitsFromISR(s_event_group, BSP_EVENT_ALARM_IN0_RISING, NULL);
    } else if (GPIO_Pin == ALARM_IN1_Pin) {
        xEventGroupSetBitsFromISR(s_event_group, BSP_EVENT_ALARM_IN1_RISING, NULL);
    }
}

void HAL_GPIO_EXTI_Falling_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin == ALARM_IN0_Pin) {
        if (s_event_group == NULL) return;
        xEventGroupSetBitsFromISR(s_event_group, BSP_EVENT_ALARM_IN0_FALLING, NULL);
    } else if (GPIO_Pin == ALARM_IN1_Pin) {
        if (s_event_group == NULL) return;
        xEventGroupSetBitsFromISR(s_event_group, BSP_EVENT_ALARM_IN1_FALLING, NULL);
    } else {
        ms41908m_port_irq_handler(GPIO_Pin);
    }
}

int bsp_ctrl_read_adc(int16_t *value1, int16_t *value2)
{
    int i = 0, all_value1 = 0, all_value2 = 0;
    uint32_t event_bits = 0;
    if (value1 == NULL || value2 == NULL) return SYS_ERR_INVALID_ARG;

    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)s_adc_buffer, BSP_ADC_BUFFER_SIZE_MAX) != HAL_OK) return SYS_ERR_HAL;
    event_bits = xEventGroupWaitBits(s_event_group, BSP_EVENT_ADC_ALL_BITS, pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
    if (event_bits & BSP_EVENT_ADC_ERROR) {
        WIC_LOGE("[bsp_ctrl_read_adc] ADC error(0x%08X)!", HAL_ADC_GetError(&hadc1));
        return SYS_ERR_HAL;
    }
    if ((event_bits & BSP_EVENT_ADC_COMPLETE) == 0) {
        HAL_ADC_Stop_DMA(&hadc1);
        WIC_LOGE("[bsp_ctrl_read_adc] ADC complete timeout!");
        return SYS_ERR_TIMEOUT;
    }

    for (i = 0; i < BSP_ADC_BUFFER_SIZE_MAX; i += 2) {
        all_value1 += s_adc_buffer[i];
        all_value2 += s_adc_buffer[i + 1];
    }
    *value1 = all_value1 / (BSP_ADC_BUFFER_SIZE_MAX / 2);
    *value2 = all_value2 / (BSP_ADC_BUFFER_SIZE_MAX / 2);

    return SYS_OK;
}

void bsp_ctrl_task(void *arg)
{
    uint32_t event_bits = 0;

    for (;;) {
        event_bits = xEventGroupWaitBits(s_event_group, BSP_EVENT_ALARM_IN_ALL_BITS, pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
        if (event_bits & BSP_EVENT_ALARM_IN0_ALL_BITS) {
            WIC_LOGD("[bsp_ctrl_task] ALARM_IN0 %s", (event_bits & BSP_EVENT_ALARM_IN0_RISING) ? "rising" : "falling");
            if (s_alarm_in_callback[BSP_ALARM_IN0] != NULL) {
                s_alarm_in_callback[BSP_ALARM_IN0](BSP_ALARM_IN0,
                    bsp_ctrl_apply_invert(((event_bits & BSP_EVENT_ALARM_IN0_RISING) ? 1U : 0U),
                                          bsp_ctrl_get_alarm_in_invert_flag(BSP_ALARM_IN0)));
            }
        }
        if (event_bits & BSP_EVENT_ALARM_IN1_ALL_BITS) {
            WIC_LOGD("[bsp_ctrl_task] ALARM_IN1 %s", (event_bits & BSP_EVENT_ALARM_IN1_RISING) ? "rising" : "falling");
            if (s_alarm_in_callback[BSP_ALARM_IN1] != NULL) {
                s_alarm_in_callback[BSP_ALARM_IN1](BSP_ALARM_IN1,
                    bsp_ctrl_apply_invert(((event_bits & BSP_EVENT_ALARM_IN1_RISING) ? 1U : 0U),
                                          bsp_ctrl_get_alarm_in_invert_flag(BSP_ALARM_IN1)));
            }
        }
    }
}

int bsp_ctrl_init(void)
{
    for (int i = 0; i < BSP_LED_MAX; i++) {
        s_led_duty[i] = 0U;
    }

    bsp_ctrl_adc_init_vdda_from_vrefint();

    s_event_group = xEventGroupCreate();
    if (s_event_group == NULL) {
        WIC_LOGE("[bsp_ctrl_init] Failed to create event group!");
        return SYS_ERR_NO_MEM;
    }
    if (xTaskCreate(bsp_ctrl_task, BSP_CTRL_TASK_NAME, BSP_CTRL_TASK_STACK_SIZE, NULL, BSP_CTRL_TASK_PRIORITY, NULL) != pdPASS) {
        WIC_LOGE("[bsp_ctrl_init] Failed to create task!");
        return SYS_ERR_NO_MEM;
    }
    WIC_LOGD("[bsp_ctrl_init] ok!");
    return SYS_OK;
}

/**
 * @brief Set the duty of the LED
 * @param led: the LED to set the duty of
 * @param duty: the duty to set the LED to
 * @return SYS_OK if successful, SYS_ERR_INVALID_ARG if the duty is invalid
 * @return SYS_ERR_HAL if the HAL error occurs
 */
int bsp_ctrl_set_led_duty(bsp_led_t led, uint8_t duty)
{
    TIM_HandleTypeDef *htim = NULL;
    uint32_t channel = 0, period = 0, pulse = 0;
    if (duty > 100) duty = 100;
    if (duty == s_led_duty[led]) return SYS_OK;

    switch (led) {
        case BSP_LED_IR_NEAR:
            htim = &htim1;
            channel = TIM_CHANNEL_1;
            break;
        case BSP_LED_IR_FAR:
            htim = &htim1;
            channel = TIM_CHANNEL_2;
            break;
        case BSP_LED_WHITE2:
            htim = &htim3;
            channel = TIM_CHANNEL_2;
            break;
        case BSP_LED_WHITE1:
        case BSP_LED_IR1:
            htim = &htim3;
            channel = TIM_CHANNEL_1;
            break;
        default:
            return SYS_ERR_INVALID_ARG;
    }

    if (duty == 0) {
        if (led == BSP_LED_WHITE1 || led == BSP_LED_IR1) {
            if ((led == BSP_LED_WHITE1 && s_led_duty[BSP_LED_IR1] == 0) || (led == BSP_LED_IR1 && s_led_duty[BSP_LED_WHITE1] == 0)) {
                if (HAL_TIM_PWM_Stop(htim, channel) != HAL_OK) return SYS_ERR_HAL;
                __HAL_TIM_SET_COMPARE(htim, channel, 0);
                s_led_duty[led] = 0;
                return SYS_OK;
            }
            if (led == BSP_LED_WHITE1) HAL_GPIO_WritePin(WLED_EN_GPIO_Port, WLED_EN_Pin, GPIO_PIN_RESET);
            else if (led == BSP_LED_IR1) HAL_GPIO_WritePin(RLED_EN_GPIO_Port, RLED_EN_Pin, GPIO_PIN_RESET);
        } else {
            if (HAL_TIM_PWM_Stop(htim, channel) != HAL_OK) return SYS_ERR_HAL;
            __HAL_TIM_SET_COMPARE(htim, channel, 0);
            s_led_duty[led] = 0;
            return SYS_OK;
        }
    } else {
        period = __HAL_TIM_GET_AUTORELOAD(htim);
        pulse = ((period + 1U) * duty) / 100U;
        __HAL_TIM_SET_COMPARE(htim, channel, pulse);
        if (TIM_CHANNEL_STATE_GET(htim, channel) != HAL_TIM_CHANNEL_STATE_BUSY) {
            if (HAL_TIM_PWM_Start(htim, channel) != HAL_OK) return SYS_ERR_HAL;
        }
        s_led_duty[led] = duty;
        if (led == BSP_LED_WHITE1) {
            HAL_GPIO_WritePin(WLED_EN_GPIO_Port, WLED_EN_Pin, GPIO_PIN_SET);
            s_led_duty[BSP_LED_IR1] = duty;
        } else if (led == BSP_LED_IR1) {
            HAL_GPIO_WritePin(RLED_EN_GPIO_Port, RLED_EN_Pin, GPIO_PIN_SET);
            s_led_duty[BSP_LED_WHITE1] = duty;
        }
    }

    return SYS_OK;
}


/**
 * @brief  Get current PWM duty of LED
 * @param  led: LED index
 * @retval Duty value 0-100
 */
uint8_t bsp_ctrl_get_led_duty(bsp_led_t led)
{
    if (led >= BSP_LED_MAX) return 0;
    return s_led_duty[led];
}
 
/**
 * @brief  Control IR-CUT relay
 * @param  enable: 0 disable, non-zero enable
 * @retval SYS_OK
 */
int bsp_ctrl_set_ir_cut(uint8_t enable)
{
    enable = (enable ? 1U : 0U);
    HAL_GPIO_WritePin(IR_CUT_EN_GPIO_Port, IR_CUT_EN_Pin, enable ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return SYS_OK;
}

/**
 * @brief  Get IR-CUT relay state by GPIO level
 * @retval 0: disabled, 1: enabled
 */
uint8_t bsp_ctrl_get_ir_cut(void)
{
    return (HAL_GPIO_ReadPin(IR_CUT_EN_GPIO_Port, IR_CUT_EN_Pin) == GPIO_PIN_SET) ? 1U : 0U;
}

/**
 * @brief  Get photo-diode ADC channel voltage
 * @retval Voltage in millivolts
 */
uint16_t bsp_ctrl_get_pd_voltage_mv(void)
{
    int16_t pd_raw = 0;
    int16_t temp_raw = 0;
    uint32_t mv = 0U;

    if (bsp_ctrl_read_adc(&pd_raw, &temp_raw) != SYS_OK) {
        return 0U;
    }
    if (pd_raw < 0) pd_raw = 0;

    mv = __HAL_ADC_CALC_DATA_TO_VOLTAGE(s_adc_vdda_mv, (uint32_t)pd_raw, ADC_RESOLUTION_12B);
    if (mv > 0xFFFFU) mv = 0xFFFFU;
    return (uint16_t)mv;
}

/**
 * @brief  Convert PD_ADC voltage to approximate illuminance (lux)
 * @param  voltage_mv: PD_ADC in millivolts (emitter node on R123 47k)
 * @retval Ev in lux (6500K-white equivalent scale, board-dependent)
 */
float bsp_ctrl_convert_pd_to_lux(uint16_t voltage_mv)
{
    float i_proxy_ua = (float)voltage_mv / BSP_PD_R_EMITTER_KOHM;
    float lux;

    if (i_proxy_ua <= BSP_PD_DARK_PROXY_UA) {
        return 0.0f;
    }
    i_proxy_ua -= BSP_PD_DARK_PROXY_UA;
    lux = i_proxy_ua * BSP_PD_LUX_PER_PROXY_UA;
    if (lux > BSP_PD_LUX_CLAMP_MAX) {
        lux = BSP_PD_LUX_CLAMP_MAX;
    }
    return lux;
}

/**
 * @brief  Get temperature sensor ADC channel voltage
 * @retval Voltage in millivolts
 */
uint16_t bsp_ctrl_get_temp_voltage_mv(void)
{
    int16_t pd_raw = 0;
    int16_t temp_raw = 0;
    uint32_t mv = 0U;

    if (bsp_ctrl_read_adc(&pd_raw, &temp_raw) != SYS_OK) {
        return 0U;
    }
    if (temp_raw < 0) temp_raw = 0;

    mv = __HAL_ADC_CALC_DATA_TO_VOLTAGE(s_adc_vdda_mv, (uint32_t)temp_raw, ADC_RESOLUTION_12B);
    if (mv > 0xFFFFU) mv = 0xFFFFU;
    return (uint16_t)mv;
}

/**
 * @brief  Convert LMT87 voltage to Celcius
 * @param  voltage_mv: LMT87 output in millivolts (same unit as TI Table 3)
 * @retval Temperature in degrees Celsius (TI Equation 1 inverted)
 * @note  Datasheet: V_mV = 2230.8 - 13.582*(T-30) - 0.00433*(T-30)^2
 *        Use double for disc/sqrt: float loses bits in (-b+sqrt(d)) near V≈2231 mV.
 */
float bsp_ctrl_convert_lmt87_to_celcius(uint16_t voltage_mv)
{
    const double v_mv = (double)voltage_mv;
    const double a = 0.00433;
    const double b = 13.582;
    const double v0 = 2230.8;
    const double t_ref = 30.0;
    double c = v_mv - v0;
    double disc = b * b - 4.0 * a * c;
    double x;

    if (disc < 0.0) {
        disc = 0.0;
    }
    /* Root with x near 0 at V≈v0 (smaller |x|): (-b+sqrt(d))/(2a) */
    x = (-b + sqrt(disc)) / (2.0 * a);
    return (float)(t_ref + x);
}

/**
 * @brief  Set fan duty (IO based, no PWM yet)
 * @param  duty: 0 to 100, 0=off, others=on
 * @retval SYS_OK
 */
int bsp_ctrl_set_fan_duty(uint8_t duty)
{
    if (duty > 100U) duty = 100U;
    HAL_GPIO_WritePin(FAN_EN_GPIO_Port, FAN_EN_Pin, (duty == 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    return SYS_OK;
}

/**
 * @brief  Get fan duty by GPIO level
 * @note   Currently returns 0 or 100 according to IO state
 * @retval 0 or 100
 */
uint8_t bsp_ctrl_get_fan_duty(void)
{
    return (HAL_GPIO_ReadPin(FAN_EN_GPIO_Port, FAN_EN_Pin) == GPIO_PIN_SET) ? 100U : 0U;
}

/**
 * @brief  Set heater duty (IO based, no PWM yet)
 * @param  duty: 0 to 100, 0=off, others=on
 * @retval SYS_OK
 */
int bsp_ctrl_set_heat_duty(uint8_t duty)
{
    if (duty > 100U) duty = 100U;
    HAL_GPIO_WritePin(HEAT_EN_GPIO_Port, HEAT_EN_Pin, (duty == 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
    return SYS_OK;
}

/**
 * @brief  Get heater duty by GPIO level
 * @note   Currently returns 0 or 100 according to IO state
 * @retval 0 or 100
 */
uint8_t bsp_ctrl_get_heat_duty(void)
{
    return (HAL_GPIO_ReadPin(HEAT_EN_GPIO_Port, HEAT_EN_Pin) == GPIO_PIN_SET) ? 100U : 0U;
}

/**
 * @brief  Enable or disable radar module
 * @param  enable: 0 disable, non-zero enable
 * @retval SYS_OK
 */
int bsp_ctrl_set_radar_en(uint8_t enable)
{
    enable = (enable ? 1U : 0U);
    HAL_GPIO_WritePin(RADAR_EN_GPIO_Port, RADAR_EN_Pin, enable ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return SYS_OK;
}

/**
 * @brief  Get radar enable state by GPIO level
 * @retval 0: disabled, 1: enabled
 */
uint8_t bsp_ctrl_get_radar_en(void)
{
    return (HAL_GPIO_ReadPin(RADAR_EN_GPIO_Port, RADAR_EN_Pin) == GPIO_PIN_SET) ? 1U : 0U;
}

/**
 * @brief  Control alarm output line
 * @param  out: alarm output index
 * @param  enable: 0 disable, non-zero enable
 * @retval SYS_OK or error code
 */
int bsp_ctrl_set_alarm_out(bsp_alarm_out_t out, uint8_t enable)
{
    GPIO_TypeDef *port = NULL;
    uint16_t pin = 0U;

    if (out >= BSP_ALARM_OUT_MAX) return SYS_ERR_INVALID_ARG;
    enable = (enable ? 1U : 0U);
    enable = bsp_ctrl_apply_invert(enable, bsp_ctrl_get_alarm_out_invert_flag(out));

    switch (out) {
        case BSP_ALARM_OUT0:
            port = ALARM_OUT0_GPIO_Port;
            pin = ALARM_OUT0_Pin;
            break;
        case BSP_ALARM_OUT1:
            port = ALARM_OUT1_GPIO_Port;
            pin = ALARM_OUT1_Pin;
            break;
        default:
            return SYS_ERR_INVALID_ARG;
    }

    HAL_GPIO_WritePin(port, pin, enable ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return SYS_OK;
}

/**
 * @brief  Get alarm output state by GPIO level
 * @param  out: alarm output index
 * @retval 0: low/disabled, 1: high/enabled
 */
uint8_t bsp_ctrl_get_alarm_out(bsp_alarm_out_t out)
{
    GPIO_TypeDef *port = NULL;
    uint16_t pin = 0U;

    if (out >= BSP_ALARM_OUT_MAX) return 0U;

    switch (out) {
        case BSP_ALARM_OUT0:
            port = ALARM_OUT0_GPIO_Port;
            pin = ALARM_OUT0_Pin;
            break;
        case BSP_ALARM_OUT1:
            port = ALARM_OUT1_GPIO_Port;
            pin = ALARM_OUT1_Pin;
            break;
        default:
            return 0U;
    }

    return bsp_ctrl_apply_invert(((HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET) ? 1U : 0U),
                                  bsp_ctrl_get_alarm_out_invert_flag(out));
}

/**
 * @brief  Enable or disable Wiegand output line (placeholder)
 * @param  wiegand: Wiegand index
 * @param  enable: 0 disable, non-zero enable
 * @retval SYS_OK or error code
 */
int bsp_ctrl_set_wiegand(bsp_wiegand_t wiegand, uint8_t enable)
{
    GPIO_TypeDef *port = NULL;
    uint16_t pin = 0U;

    if (wiegand >= BSP_WIEGAND_MAX) return SYS_ERR_INVALID_ARG;
    enable = (enable ? 1U : 0U);
    enable = bsp_ctrl_apply_invert(enable, bsp_ctrl_get_wiegand_invert_flag(wiegand));

    switch (wiegand) {
        case BSP_WIEGAND0:
            port = WIEGAND_OUT0_GPIO_Port;
            pin = WIEGAND_OUT0_Pin;
            break;
        case BSP_WIEGAND1:
            port = WIEGAND_OUT1_GPIO_Port;
            pin = WIEGAND_OUT1_Pin;
            break;
        default:
            return SYS_ERR_INVALID_ARG;
    }

    HAL_GPIO_WritePin(port, pin, enable ? GPIO_PIN_SET : GPIO_PIN_RESET);
    return SYS_OK;
}

/**
 * @brief  Get Wiegand output state by GPIO level
 * @param  wiegand: Wiegand index
 * @retval 0: low/disabled, 1: high/enabled
 */
uint8_t bsp_ctrl_get_wiegand(bsp_wiegand_t wiegand)
{
    GPIO_TypeDef *port = NULL;
    uint16_t pin = 0U;

    if (wiegand >= BSP_WIEGAND_MAX) return 0U;

    switch (wiegand) {
        case BSP_WIEGAND0:
            port = WIEGAND_OUT0_GPIO_Port;
            pin = WIEGAND_OUT0_Pin;
            break;
        case BSP_WIEGAND1:
            port = WIEGAND_OUT1_GPIO_Port;
            pin = WIEGAND_OUT1_Pin;
            break;
        default:
            return 0U;
    }

    return bsp_ctrl_apply_invert(((HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET) ? 1U : 0U),
                                  bsp_ctrl_get_wiegand_invert_flag(wiegand));
}

/**
 * @brief  Register callback for alarm input edge events
 * @param  in: alarm input index
 * @param  callback: user callback, NULL to unregister
 * @retval SYS_OK or error code
 */
int bsp_ctrl_register_alarm_in(bsp_alarm_in_t in, bsp_alarm_in_callback_t callback)
{
    if (in >= BSP_ALARM_IN_MAX) return SYS_ERR_INVALID_ARG;
    s_alarm_in_callback[in] = callback;
    return SYS_OK;
}

/**
 * @brief  Get current alarm input level
 * @param  in: alarm input index
 * @retval 0: low, 1: high, negative on error
 */
int bsp_ctrl_get_alarm_in(bsp_alarm_in_t in)
{
    GPIO_TypeDef *port = NULL;
    uint16_t pin = 0U;

    if (in >= BSP_ALARM_IN_MAX) return SYS_ERR_INVALID_ARG;

    switch (in) {
        case BSP_ALARM_IN0:
            port = ALARM_IN0_GPIO_Port;
            pin = ALARM_IN0_Pin;
            break;
        case BSP_ALARM_IN1:
            port = ALARM_IN1_GPIO_Port;
            pin = ALARM_IN1_Pin;
            break;
        default:
            return SYS_ERR_INVALID_ARG;
    }

    return bsp_ctrl_apply_invert(((HAL_GPIO_ReadPin(port, pin) == GPIO_PIN_SET) ? 1U : 0U),
                                  bsp_ctrl_get_alarm_in_invert_flag(in));
}

/**
 * @brief  Reset SoC power by toggling PWR_RST pin
 * @note   Generates a low pulse then releases the reset line
 * @retval SYS_OK
 */
int bsp_ctrl_reset_soc_power(void)
{
    HAL_GPIO_WritePin(PWR_RST_GPIO_Port, PWR_RST_Pin, GPIO_PIN_SET);
    vTaskDelay(pdMS_TO_TICKS(500));
    HAL_GPIO_WritePin(PWR_RST_GPIO_Port, PWR_RST_Pin, GPIO_PIN_RESET);
    return SYS_OK;
}

