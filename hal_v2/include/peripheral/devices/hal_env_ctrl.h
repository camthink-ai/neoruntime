/**
 * @file hal_env_ctrl.h
 * @brief Environment control (fan/heater/radar) and reset paths (MCU command vs host GPIO).
 */
#pragma once

#include <stdbool.h>
#include "../../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int (*fan_set)(void *mcu_ctx, bool enable);
    int (*fan_get)(void *mcu_ctx, bool *enable);

    int (*heat_set)(void *mcu_ctx, bool enable);
    int (*heat_get)(void *mcu_ctx, bool *enable);

    int (*radar_set)(void *mcu_ctx, bool enable);
    int (*radar_get)(void *mcu_ctx, bool *enable);

    /** @brief host_link @c HOST_LINK_CMD_RESET_SOC — MCU toggles application SoC PWR_RST (not MCU reboot). */
    int (*reset_soc)(void *mcu_ctx);

    /**
     * @param force @c false: host_link @c HOST_LINK_CMD_REBOOT (MCU reboot over UART).
     *              @c true: Linux pulses @ref HalMcuConfig.host_mcu_reset_gpio (MCU NRST from SoC side).
     */
    int (*reset_mcu)(void *mcu_ctx, bool force);

    const char *(*get_version)(void);
} HalEnvCtrlOps;

extern HalEnvCtrlOps HAL_ENV_CTRL_OPS;

#ifdef __cplusplus
}
#endif

