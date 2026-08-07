/** @file stub_alarm_impl.c — HAL_ALARM_OPS (stub) */
#include "peripheral/devices/hal_alarm.h"

#include <stddef.h>

static int stub_alarm_out_set(void *mcu_ctx, uint8_t channel, bool enable)
{
    (void)mcu_ctx;
    (void)channel;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_alarm_out_get(void *mcu_ctx, uint8_t channel, bool *enable)
{
    (void)mcu_ctx;
    (void)channel;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_wiegand_out_set(void *mcu_ctx, uint8_t channel, bool enable)
{
    (void)mcu_ctx;
    (void)channel;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_wiegand_out_get(void *mcu_ctx, uint8_t channel, bool *enable)
{
    (void)mcu_ctx;
    (void)channel;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_outputs_get(void *mcu_ctx, HalAlarmOutputsState *out_state)
{
    (void)mcu_ctx;
    (void)out_state;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_alarm_subscribe(void *mcu_ctx, HalAlarmEventCallback cb, void *userdata)
{
    (void)mcu_ctx;
    (void)cb;
    (void)userdata;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_alarm_unsubscribe(void *mcu_ctx)
{
    (void)mcu_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_alarm_get_version(void)
{
    return "HAL-ALARM stub 2.0.0 (platform stub)";
}

HalAlarmOps HAL_ALARM_OPS = {
    .alarm_out_set = stub_alarm_out_set,
    .alarm_out_get = stub_alarm_out_get,
    .wiegand_out_set = stub_wiegand_out_set,
    .wiegand_out_get = stub_wiegand_out_get,
    .outputs_get = stub_outputs_get,
    .subscribe = stub_alarm_subscribe,
    .unsubscribe = stub_alarm_unsubscribe,
    .get_version = stub_alarm_get_version,
};
