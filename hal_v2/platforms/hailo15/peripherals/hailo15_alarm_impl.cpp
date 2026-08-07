/**
 * @file hailo15_alarm_impl.cpp
 * @brief hailo15 alarm outputs + alarm input events (MCU host_link).
 */

#include "../mcu/hailo15_mcu_priv.hpp"

extern "C" {
#include "peripheral/devices/hal_alarm.h"
#include "peripheral/hal_mcu.h"
}

#include "common/host_link/host_link_proto.h"

#include <cstring>

static int ch_set(void *mcu_ctx, uint16_t cmd, uint8_t channel, bool enable)
{
    host_link_ch_enable_t req{.channel = channel, .enable = (uint8_t)(enable ? 1 : 0)};
    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, cmd,
                                      reinterpret_cast<const uint8_t *>(&req), sizeof(req),
                                      reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(st)) return HAL_ERR_INVALID_SIZE;
    return hailo15_mcu_map_status(st.status);
}

static int ch_get(void *mcu_ctx, uint16_t cmd, uint8_t channel, bool *enable)
{
    if (enable == nullptr) return HAL_ERR_INVALID_ARG;
    uint8_t resp = 0;
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, cmd, &channel, sizeof(channel),
                                      &resp, sizeof(resp), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(resp)) return HAL_ERR_INVALID_SIZE;
    *enable = (resp != 0);
    return HAL_OK;
}

static int alarm_out_set(void *mcu_ctx, uint8_t channel, bool enable) { return ch_set(mcu_ctx, HOST_LINK_CMD_AOUT_SET, channel, enable); }
static int alarm_out_get(void *mcu_ctx, uint8_t channel, bool *enable) { return ch_get(mcu_ctx, HOST_LINK_CMD_AOUT_GET, channel, enable); }
static int wiegand_out_set(void *mcu_ctx, uint8_t channel, bool enable) { return ch_set(mcu_ctx, HOST_LINK_CMD_WOUT_SET, channel, enable); }
static int wiegand_out_get(void *mcu_ctx, uint8_t channel, bool *enable) { return ch_get(mcu_ctx, HOST_LINK_CMD_WOUT_GET, channel, enable); }

static int outputs_get(void *mcu_ctx, HalAlarmOutputsState *out_state)
{
    if (out_state == nullptr) return HAL_ERR_INVALID_ARG;
    bool en = false;
    int ret = alarm_out_get(mcu_ctx, 0, &en);
    if (ret != HAL_OK) return ret;
    out_state->alarm_out0 = en;
    ret = alarm_out_get(mcu_ctx, 1, &en);
    if (ret != HAL_OK) return ret;
    out_state->alarm_out1 = en;
    ret = wiegand_out_get(mcu_ctx, 0, &en);
    if (ret != HAL_OK) return ret;
    out_state->wiegand0 = en;
    ret = wiegand_out_get(mcu_ctx, 1, &en);
    if (ret != HAL_OK) return ret;
    out_state->wiegand1 = en;
    return HAL_OK;
}

static int subscribe(void *mcu_ctx, HalAlarmEventCallback cb, void *userdata)
{
    if (cb == nullptr) return HAL_ERR_INVALID_ARG;
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    return hailo15_mcu_register_event_cb(ctx, HOST_LINK_CMD_EV_ALARM_IN,
                                         [ctx, cb, userdata](uint16_t cmd, const uint8_t *payload, uint16_t len) {
                                             (void)cmd;
                                             if (payload == nullptr || len != sizeof(host_link_alarm_in_evt_t)) {
                                                 return;
                                             }
                                             host_link_alarm_in_evt_t ev{};
                                             memcpy(&ev, payload, sizeof(ev));
                                             cb(ctx, ev.channel, ev.level != 0, userdata);
                                         });
}

static int unsubscribe(void *mcu_ctx)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    return hailo15_mcu_unregister_all_event_cbs(ctx, HOST_LINK_CMD_EV_ALARM_IN);
}

static const char *alarm_get_version(void)
{
    return "Hailo15 HAL-ALARM 2.0.0";
}

extern "C" {
HalAlarmOps HAL_ALARM_OPS = {
    .alarm_out_set = alarm_out_set,
    .alarm_out_get = alarm_out_get,
    .wiegand_out_set = wiegand_out_set,
    .wiegand_out_get = wiegand_out_get,
    .outputs_get = outputs_get,
    .subscribe = subscribe,
    .unsubscribe = unsubscribe,
    .get_version = alarm_get_version,
};
}

