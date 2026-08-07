/**
 * @file hailo15_env_ctrl_impl.cpp
 * @brief hailo15 environment control + reset_soc / reset_mcu (host_link).
 *
 * Board ne503: @ref HalMcuConfig::host_mcu_reset_gpio pulses MCU NRST from Linux (gpiochip1 line 2 → HAL 18).
 */

#include "../mcu/hailo15_mcu_priv.hpp"

extern "C" {
#include "peripheral/devices/hal_env_ctrl.h"
#include "peripheral/hal_mcu.h"
}

#include "common/host_link/host_link_proto.h"

#include <gpiod.h>

#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace {

static constexpr unsigned kSocGpioLinesPerChip = 16u;

static int host_mcu_reset_gpio_pulse(const HalMcuConfig *cfg)
{
    if (cfg == nullptr || cfg->host_mcu_reset_gpio == 0u) {
        return HAL_ERR_NOT_SUPPORTED;
    }
    const uint32_t g = cfg->host_mcu_reset_gpio;
    if (g >= kSocGpioLinesPerChip * 2u) {
        return HAL_ERR_INVALID_ARG;
    }
    const unsigned chip_idx = g / kSocGpioLinesPerChip;
    const unsigned offset = g % kSocGpioLinesPerChip;
    const bool active_low = (cfg->host_mcu_reset_active_low != 0u);
    uint32_t pulse_ms = cfg->host_mcu_reset_pulse_ms;
    if (pulse_ms == 0u) {
        pulse_ms = 200u;
    }

    gpiod_chip *chip = gpiod_chip_open_by_number(chip_idx);
    if (chip == nullptr) {
        return HAL_ERROR;
    }
    gpiod_line *line = gpiod_chip_get_line(chip, offset);
    if (line == nullptr) {
        gpiod_chip_close(chip);
        return HAL_ERROR;
    }
    constexpr const char *cons = "aipc_hal_mcu_reset_gpio";
    /* Drive physical levels (no ACTIVE_LOW): idle → assert reset → idle.
     * High-active reset: pulse drives 1; low-active: pulse drives 0. */
    const int idle_v = active_low ? 1 : 0;
    const int reset_assert_v = active_low ? 0 : 1;
    if (gpiod_line_request_output_flags(line, cons, 0, idle_v) != 0) {
        gpiod_chip_close(chip);
        return HAL_ERROR;
    }
    if (gpiod_line_set_value(line, reset_assert_v) != 0) {
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        return HAL_ERROR;
    }
    usleep(pulse_ms * 1000u);
    if (gpiod_line_set_value(line, idle_v) != 0) {
        gpiod_line_release(line);
        gpiod_chip_close(chip);
        return HAL_ERROR;
    }
    gpiod_line_release(line);
    gpiod_chip_close(chip);
    return HAL_OK;
}

static int duty_set(void *mcu_ctx, uint16_t cmd, bool enable)
{
    const uint8_t duty = enable ? 100u : 0u;
    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, cmd, &duty, sizeof(duty), reinterpret_cast<uint8_t *>(&st), sizeof(st),
                                      &resp_len);
    if (ret != HAL_OK) {
        return ret;
    }
    if (resp_len != sizeof(st)) {
        return HAL_ERR_INVALID_SIZE;
    }
    return hailo15_mcu_map_status(st.status);
}

static int duty_get(void *mcu_ctx, uint16_t cmd, bool *enable)
{
    if (enable == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    uint8_t d = 0;
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, cmd, nullptr, 0, &d, sizeof(d), &resp_len);
    if (ret != HAL_OK) {
        return ret;
    }
    if (resp_len != sizeof(d)) {
        return HAL_ERR_INVALID_SIZE;
    }
    *enable = (d != 0u);
    return HAL_OK;
}

static int radar_set_byte(void *mcu_ctx, bool enable)
{
    const uint8_t en = enable ? 1u : 0u;
    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_RADAR_SET, &en, sizeof(en),
                                      reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len);
    if (ret != HAL_OK) {
        return ret;
    }
    if (resp_len != sizeof(st)) {
        return HAL_ERR_INVALID_SIZE;
    }
    return hailo15_mcu_map_status(st.status);
}

static int fan_set(void *mcu_ctx, bool enable) { return duty_set(mcu_ctx, HOST_LINK_CMD_FAN_SET, enable); }
static int fan_get(void *mcu_ctx, bool *enable) { return duty_get(mcu_ctx, HOST_LINK_CMD_FAN_GET, enable); }
static int heat_set(void *mcu_ctx, bool enable) { return duty_set(mcu_ctx, HOST_LINK_CMD_HEAT_SET, enable); }
static int heat_get(void *mcu_ctx, bool *enable) { return duty_get(mcu_ctx, HOST_LINK_CMD_HEAT_GET, enable); }
static int radar_set(void *mcu_ctx, bool enable) { return radar_set_byte(mcu_ctx, enable); }
static int radar_get(void *mcu_ctx, bool *enable) { return duty_get(mcu_ctx, HOST_LINK_CMD_RADAR_GET, enable); }

static int reset_soc(void *mcu_ctx)
{
    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_RESET_SOC, nullptr, 0,
                                      reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len);
    if (ret != HAL_OK) {
        return ret;
    }
    if (resp_len != sizeof(st)) {
        return HAL_ERR_INVALID_SIZE;
    }
    return hailo15_mcu_map_status(st.status);
}

static int reset_mcu(void *mcu_ctx, bool force)
{
    if (force) {
        const HalMcuConfig *cfg = hailo15_mcu_get_config(mcu_ctx);
        return host_mcu_reset_gpio_pulse(cfg);
    }

    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_REBOOT, nullptr, 0,
                                      reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len);
    if (ret != HAL_OK) {
        return ret;
    }
    if (resp_len != sizeof(st)) {
        return HAL_ERR_INVALID_SIZE;
    }
    return hailo15_mcu_map_status(st.status);
}

} // namespace

static const char *env_get_version(void)
{
    return "Hailo15 HAL-ENV-CTRL 2.0.3 (reset_mcu: REBOOT cmd | host gpio)";
}

extern "C" {
HalEnvCtrlOps HAL_ENV_CTRL_OPS = {
    .fan_set = fan_set,
    .fan_get = fan_get,
    .heat_set = heat_set,
    .heat_get = heat_get,
    .radar_set = radar_set,
    .radar_get = radar_get,
    .reset_soc = reset_soc,
    .reset_mcu = reset_mcu,
    .get_version = env_get_version,
};
}

