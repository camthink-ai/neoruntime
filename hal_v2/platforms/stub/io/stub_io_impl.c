/**
 * @file stub_io_impl.c
 * @brief Stub platform — HAL_IO_OPS.
 */

#include "peripheral/hal_io.h"

#include <stddef.h>

static int stub_io_init(void **io_ctx_return)
{
    if (!io_ctx_return)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *io_ctx_return = NULL;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_io_deinit(void *io_ctx)
{
    (void)io_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_io_gpio_export(void *io_ctx, const HalGpioConfig *config)
{
    (void)io_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_io_gpio_unexport(void *io_ctx, uint32_t gpio_num)
{
    (void)io_ctx;
    (void)gpio_num;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_io_gpio_set_value(void *io_ctx, uint32_t gpio_num, bool value)
{
    (void)io_ctx;
    (void)gpio_num;
    (void)value;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_io_gpio_get_value(void *io_ctx, uint32_t gpio_num, bool *value)
{
    (void)io_ctx;
    (void)gpio_num;
    if (value)
    {
        *value = false;
    }
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_io_gpio_set_direction(void *io_ctx, uint32_t gpio_num, HalGpioDirection dir)
{
    (void)io_ctx;
    (void)gpio_num;
    (void)dir;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_io_gpio_subscribe(void *io_ctx, uint32_t gpio_num, HalGpioEdge edge, HalGpioEventCallback callback,
                                  void *userdata)
{
    (void)io_ctx;
    (void)gpio_num;
    (void)edge;
    (void)callback;
    (void)userdata;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_io_gpio_unsubscribe(void *io_ctx, uint32_t gpio_num)
{
    (void)io_ctx;
    (void)gpio_num;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_io_pwm_configure(void *io_ctx, const HalPwmConfig *config)
{
    (void)io_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_io_pwm_set_duty(void *io_ctx, uint32_t pwm_chip, uint32_t pwm_channel, uint32_t duty_ns)
{
    (void)io_ctx;
    (void)pwm_chip;
    (void)pwm_channel;
    (void)duty_ns;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_io_pwm_enable(void *io_ctx, uint32_t pwm_chip, uint32_t pwm_channel, bool enable)
{
    (void)io_ctx;
    (void)pwm_chip;
    (void)pwm_channel;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_io_get_version(void)
{
    return "HAL-IO stub 2.0.0 (platform stub)";
}

HalIoOps HAL_IO_OPS = {
    .init = stub_io_init,
    .deinit = stub_io_deinit,
    .gpio_export = stub_io_gpio_export,
    .gpio_unexport = stub_io_gpio_unexport,
    .gpio_set_value = stub_io_gpio_set_value,
    .gpio_get_value = stub_io_gpio_get_value,
    .gpio_set_direction = stub_io_gpio_set_direction,
    .gpio_subscribe = stub_io_gpio_subscribe,
    .gpio_unsubscribe = stub_io_gpio_unsubscribe,
    .pwm_configure = stub_io_pwm_configure,
    .pwm_set_duty = stub_io_pwm_set_duty,
    .pwm_enable = stub_io_pwm_enable,
    .get_version = stub_io_get_version,
};
