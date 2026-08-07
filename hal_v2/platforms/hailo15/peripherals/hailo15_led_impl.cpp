/**
 * @file hailo15_led_impl.cpp
 * @brief hailo15 LED + IR-cut implementation (MCU host_link).
 */

#include "../mcu/hailo15_mcu_priv.hpp"

extern "C" {
#include "peripheral/devices/hal_led.h"
#include "peripheral/hal_mcu.h"
}

#include "common/host_link/host_link_proto.h"

static int led_set_duty(void *mcu_ctx, uint8_t led_id, uint8_t duty_percent)
{
    if (duty_percent > 100) {
        return HAL_ERR_INVALID_ARG;
    }
    host_link_led_set_t req{.led_id = led_id, .duty = duty_percent};
    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_LED_SET,
                                      reinterpret_cast<const uint8_t *>(&req), sizeof(req),
                                      reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(st)) return HAL_ERR_INVALID_SIZE;
    return hailo15_mcu_map_status(st.status);
}

static int led_get_duty(void *mcu_ctx, uint8_t led_id, uint8_t *duty_percent)
{
    if (duty_percent == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    uint8_t resp = 0;
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_LED_GET,
                                      &led_id, sizeof(led_id),
                                      &resp, sizeof(resp), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(resp)) return HAL_ERR_INVALID_SIZE;
    *duty_percent = resp;
    return HAL_OK;
}

static int ircut_set_mode(void *mcu_ctx, HalIrCutMode mode)
{
    uint8_t req = (mode == HAL_IRCUT_NIGHT) ? 1 : 0;
    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_IRCUT_SET,
                                      &req, sizeof(req),
                                      reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(st)) return HAL_ERR_INVALID_SIZE;
    return hailo15_mcu_map_status(st.status);
}

static int ircut_get_mode(void *mcu_ctx, HalIrCutMode *mode)
{
    if (mode == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    uint8_t resp = 0;
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_IRCUT_GET,
                                      nullptr, 0,
                                      &resp, sizeof(resp), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(resp)) return HAL_ERR_INVALID_SIZE;
    *mode = (resp == 0) ? HAL_IRCUT_DAY : HAL_IRCUT_NIGHT;
    return HAL_OK;
}

static const char *led_get_version(void)
{
    return "Hailo15 HAL-LED 2.0.0";
}

extern "C" {
HalLedOps HAL_LED_OPS = {
    .led_set_duty = led_set_duty,
    .led_get_duty = led_get_duty,
    .ircut_set_mode = ircut_set_mode,
    .ircut_get_mode = ircut_get_mode,
    .get_version = led_get_version,
};
}

