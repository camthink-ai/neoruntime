/**
 * @file hal_sensor.h
 * @brief ADC sensors (PD, temp, AIN) via MCU.
 */
#pragma once

#include <stdint.h>
#include "../../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t mv;     /**< milli-volts */
    int32_t  milli;  /**< scaled value (platform-defined), e.g. lux*1000 or degC*1000 */
} HalAdcValue;

typedef struct {
    int (*pd_get)(void *mcu_ctx, HalAdcValue *out_pd);
    int (*temp_get)(void *mcu_ctx, HalAdcValue *out_temp);
    int (*ain_get)(void *mcu_ctx, HalAdcValue *out_ain);
    const char *(*get_version)(void);
} HalSensorOps;

extern HalSensorOps HAL_SENSOR_OPS;

#ifdef __cplusplus
}
#endif

