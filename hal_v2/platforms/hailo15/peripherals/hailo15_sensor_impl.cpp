/**
 * @file hailo15_sensor_impl.cpp
 * @brief hailo15 ADC sensor implementation (MCU host_link).
 */

extern "C" {
#include "peripheral/devices/hal_sensor.h"
#include "peripheral/hal_mcu.h"
}

#include "common/host_link/host_link_proto.h"

static int adc_get(void *mcu_ctx, uint16_t cmd, HalAdcValue *out)
{
    if (out == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    host_link_adc_milli_t wire{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, cmd, nullptr, 0,
                                      reinterpret_cast<uint8_t *>(&wire), sizeof(wire), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(wire)) return HAL_ERR_INVALID_SIZE;
    out->mv = wire.mv;
    out->milli = wire.milli;
    return HAL_OK;
}

static int pd_get(void *mcu_ctx, HalAdcValue *out_pd) { return adc_get(mcu_ctx, HOST_LINK_CMD_PD_GET, out_pd); }
static int temp_get(void *mcu_ctx, HalAdcValue *out_temp) { return adc_get(mcu_ctx, HOST_LINK_CMD_TEMP_GET, out_temp); }
static int ain_get(void *mcu_ctx, HalAdcValue *out_ain) { return adc_get(mcu_ctx, HOST_LINK_CMD_AIN_GET, out_ain); }

static const char *sensor_get_version(void)
{
    return "Hailo15 HAL-SENSOR 2.0.0";
}

extern "C" {
HalSensorOps HAL_SENSOR_OPS = {
    .pd_get = pd_get,
    .temp_get = temp_get,
    .ain_get = ain_get,
    .get_version = sensor_get_version,
};
}

