/**
 * @file hal_led.h
 * @brief LED and IR-cut peripherals (MCU-backed).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HAL_IRCUT_DAY = 0,
    HAL_IRCUT_NIGHT = 1,
} HalIrCutMode;

typedef struct {
    int (*led_set_duty)(void *mcu_ctx, uint8_t led_id, uint8_t duty_percent);
    int (*led_get_duty)(void *mcu_ctx, uint8_t led_id, uint8_t *duty_percent);

    int (*ircut_set_mode)(void *mcu_ctx, HalIrCutMode mode);
    int (*ircut_get_mode)(void *mcu_ctx, HalIrCutMode *mode);

    const char *(*get_version)(void);
} HalLedOps;

extern HalLedOps HAL_LED_OPS;

#ifdef __cplusplus
}
#endif

