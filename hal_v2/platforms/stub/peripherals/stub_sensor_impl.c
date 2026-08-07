/** @file stub_sensor_impl.c — HAL_SENSOR_OPS (stub) */
#include "peripheral/devices/hal_sensor.h"

#include <stddef.h>

static int stub_pd_get(void *mcu_ctx, HalAdcValue *out_pd)
{
    (void)mcu_ctx;
    (void)out_pd;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_temp_get(void *mcu_ctx, HalAdcValue *out_temp)
{
    (void)mcu_ctx;
    (void)out_temp;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_ain_get(void *mcu_ctx, HalAdcValue *out_ain)
{
    (void)mcu_ctx;
    (void)out_ain;
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_sensor_get_version(void)
{
    return "HAL-SENSOR stub 2.0.0 (platform stub)";
}

HalSensorOps HAL_SENSOR_OPS = {
    .pd_get = stub_pd_get,
    .temp_get = stub_temp_get,
    .ain_get = stub_ain_get,
    .get_version = stub_sensor_get_version,
};
