/**
 * @file hal_rtc.h
 * @brief RTC peripheral (MCU-backed).
 */
#pragma once

#include <stdint.h>
#include "../../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t year;     /**< 0..99 */
    uint8_t month;    /**< 1..12 */
    uint8_t day;      /**< 1..31 */
    uint8_t weekday;  /**< 1..7 */
    uint8_t hour;     /**< 0..23 */
    uint8_t minute;   /**< 0..59 */
    uint8_t second;   /**< 0..59 */
} HalRtcTime;

typedef struct {
    int (*get_time)(void *mcu_ctx, HalRtcTime *out_time);
    int (*set_time)(void *mcu_ctx, const HalRtcTime *time);
    const char *(*get_version)(void);
} HalRtcOps;

extern HalRtcOps HAL_RTC_OPS;

#ifdef __cplusplus
}
#endif

