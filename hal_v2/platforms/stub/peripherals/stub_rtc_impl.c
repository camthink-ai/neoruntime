/** @file stub_rtc_impl.c — HAL_RTC_OPS (stub) */
#include "peripheral/devices/hal_rtc.h"

#include <stddef.h>
#include <string.h>

static int stub_rtc_get_time(void *mcu_ctx, HalRtcTime *out_time)
{
    (void)mcu_ctx;
    if (!out_time)
    {
        return HAL_ERR_INVALID_ARG;
    }
    memset(out_time, 0, sizeof(*out_time));
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_rtc_set_time(void *mcu_ctx, const HalRtcTime *time)
{
    (void)mcu_ctx;
    (void)time;
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_rtc_get_version(void)
{
    return "HAL-RTC stub 2.0.0 (platform stub)";
}

HalRtcOps HAL_RTC_OPS = {
    .get_time = stub_rtc_get_time,
    .set_time = stub_rtc_set_time,
    .get_version = stub_rtc_get_version,
};
