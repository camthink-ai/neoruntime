/** @file stub_led_impl.c — HAL_LED_OPS (stub) */
#include "peripheral/devices/hal_led.h"

static int stub_led_set_duty(void *mcu_ctx, uint8_t led_id, uint8_t duty_percent)
{
    (void)mcu_ctx;
    (void)led_id;
    (void)duty_percent;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_led_get_duty(void *mcu_ctx, uint8_t led_id, uint8_t *duty_percent)
{
    (void)mcu_ctx;
    (void)led_id;
    if (duty_percent)
    {
        *duty_percent = 0;
    }
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_ircut_set_mode(void *mcu_ctx, HalIrCutMode mode)
{
    (void)mcu_ctx;
    (void)mode;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_ircut_get_mode(void *mcu_ctx, HalIrCutMode *mode)
{
    (void)mcu_ctx;
    (void)mode;
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_led_get_version(void)
{
    return "HAL-LED stub 2.0.0 (platform stub)";
}

HalLedOps HAL_LED_OPS = {
    .led_set_duty = stub_led_set_duty,
    .led_get_duty = stub_led_get_duty,
    .ircut_set_mode = stub_ircut_set_mode,
    .ircut_get_mode = stub_ircut_get_mode,
    .get_version = stub_led_get_version,
};
