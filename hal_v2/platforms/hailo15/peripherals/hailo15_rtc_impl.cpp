/**
 * @file hailo15_rtc_impl.cpp
 * @brief hailo15 RTC peripheral implementation (MCU host_link).
 */

#include "../mcu/hailo15_mcu_priv.hpp"

extern "C" {
#include "peripheral/devices/hal_rtc.h"
#include "peripheral/hal_mcu.h"
}

#include "common/host_link/host_link_proto.h"

static int rtc_get_time(void *mcu_ctx, HalRtcTime *out_time)
{
    if (out_time == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    host_link_rtc_tm_t wire{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_RTC_GET, nullptr, 0,
                                      reinterpret_cast<uint8_t *>(&wire), sizeof(wire), &resp_len);
    if (ret != HAL_OK) {
        return ret;
    }
    if (resp_len != sizeof(wire)) {
        return HAL_ERR_INVALID_SIZE;
    }
    out_time->year = wire.year;
    out_time->month = wire.month;
    out_time->day = wire.day;
    out_time->weekday = wire.weekday;
    out_time->hour = wire.hour;
    out_time->minute = wire.minute;
    out_time->second = wire.second;
    return HAL_OK;
}

static int rtc_set_time(void *mcu_ctx, const HalRtcTime *time)
{
    if (time == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    host_link_rtc_tm_t wire{};
    wire.year = time->year;
    wire.month = time->month;
    wire.day = time->day;
    wire.weekday = time->weekday;
    wire.hour = time->hour;
    wire.minute = time->minute;
    wire.second = time->second;

    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_RTC_SET,
                                      reinterpret_cast<const uint8_t *>(&wire), sizeof(wire),
                                      reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len);
    if (ret != HAL_OK) {
        return ret;
    }
    if (resp_len != sizeof(st)) {
        return HAL_ERR_INVALID_SIZE;
    }
    return hailo15_mcu_map_status(st.status);
}

static const char *rtc_get_version(void)
{
    return "Hailo15 HAL-RTC 2.0.0";
}

extern "C" {
HalRtcOps HAL_RTC_OPS = {
    .get_time = rtc_get_time,
    .set_time = rtc_set_time,
    .get_version = rtc_get_version,
};
}

