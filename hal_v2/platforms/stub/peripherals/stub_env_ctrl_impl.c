/** @file stub_env_ctrl_impl.c — HAL_ENV_CTRL_OPS (stub) */
#include "peripheral/devices/hal_env_ctrl.h"

static int stub_fan_set(void *mcu_ctx, bool enable)
{
    (void)mcu_ctx;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_fan_get(void *mcu_ctx, bool *enable)
{
    (void)mcu_ctx;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_heat_set(void *mcu_ctx, bool enable)
{
    (void)mcu_ctx;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_heat_get(void *mcu_ctx, bool *enable)
{
    (void)mcu_ctx;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_radar_set(void *mcu_ctx, bool enable)
{
    (void)mcu_ctx;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_radar_get(void *mcu_ctx, bool *enable)
{
    (void)mcu_ctx;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_reset_soc(void *mcu_ctx)
{
    (void)mcu_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_reset_mcu(void *mcu_ctx, bool force)
{
    (void)mcu_ctx;
    (void)force;
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_env_get_version(void)
{
    return "HAL-ENV_CTRL stub 2.0.0 (platform stub)";
}

HalEnvCtrlOps HAL_ENV_CTRL_OPS = {
    .fan_set = stub_fan_set,
    .fan_get = stub_fan_get,
    .heat_set = stub_heat_set,
    .heat_get = stub_heat_get,
    .radar_set = stub_radar_set,
    .radar_get = stub_radar_get,
    .reset_soc = stub_reset_soc,
    .reset_mcu = stub_reset_mcu,
    .get_version = stub_env_get_version,
};
