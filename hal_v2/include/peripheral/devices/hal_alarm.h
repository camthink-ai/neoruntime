/**
 * @file hal_alarm.h
 * @brief Alarm outputs + Wiegand outputs + alarm input events via MCU.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool alarm_out0;
    bool alarm_out1;
    bool wiegand0;
    bool wiegand1;
} HalAlarmOutputsState;

typedef void (*HalAlarmEventCallback)(void *mcu_ctx, uint8_t channel, bool level, void *userdata);

typedef struct {
    int (*alarm_out_set)(void *mcu_ctx, uint8_t channel, bool enable);
    int (*alarm_out_get)(void *mcu_ctx, uint8_t channel, bool *enable);

    int (*wiegand_out_set)(void *mcu_ctx, uint8_t channel, bool enable);
    int (*wiegand_out_get)(void *mcu_ctx, uint8_t channel, bool *enable);

    int (*outputs_get)(void *mcu_ctx, HalAlarmOutputsState *out_state);

    int (*subscribe)(void *mcu_ctx, HalAlarmEventCallback cb, void *userdata);
    int (*unsubscribe)(void *mcu_ctx);

    const char *(*get_version)(void);
} HalAlarmOps;

extern HalAlarmOps HAL_ALARM_OPS;

#ifdef __cplusplus
}
#endif

