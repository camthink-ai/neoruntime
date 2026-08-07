/**
 * @file hailo15_rs485_impl.cpp
 * @brief hailo15 RS485 implementation (MCU host_link).
 */

#include "../mcu/hailo15_mcu_priv.hpp"

extern "C" {
#include "peripheral/devices/hal_rs485.h"
#include "peripheral/hal_mcu.h"
}

#include "common/host_link/host_link_proto.h"

#include <cstring>

static int rs485_init(void *mcu_ctx, uint32_t baudrate, const char config[HAL_RS485_CONFIG_LEN])
{
    if (config == nullptr) return HAL_ERR_INVALID_ARG;
    host_link_rs485_init_t req{};
    req.baudrate = baudrate;
    memcpy(req.config, config, HAL_RS485_CONFIG_LEN);
    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_RS485_INIT,
                                      reinterpret_cast<const uint8_t *>(&req), sizeof(req),
                                      reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(st)) return HAL_ERR_INVALID_SIZE;
    return hailo15_mcu_map_status(st.status);
}

static int rs485_deinit(void *mcu_ctx)
{
    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_RS485_DEINIT,
                                      nullptr, 0,
                                      reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(st)) return HAL_ERR_INVALID_SIZE;
    return hailo15_mcu_map_status(st.status);
}

static int rs485_tx(void *mcu_ctx, const uint8_t *data, uint16_t len)
{
    if (len > 0 && data == nullptr) return HAL_ERR_INVALID_ARG;
    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_RS485_TX,
                                      data, len,
                                      reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(st)) return HAL_ERR_INVALID_SIZE;
    return hailo15_mcu_map_status(st.status);
}

static int subscribe(void *mcu_ctx, HalRs485RxCallback cb, void *userdata)
{
    if (cb == nullptr) return HAL_ERR_INVALID_ARG;
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    return hailo15_mcu_register_event_cb(ctx, HOST_LINK_CMD_EV_RS485_RX,
                                         [ctx, cb, userdata](uint16_t cmd, const uint8_t *payload, uint16_t len) {
                                             (void)cmd;
                                             cb(ctx, payload, len, userdata);
                                         });
}

static int unsubscribe(void *mcu_ctx)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    return hailo15_mcu_unregister_all_event_cbs(ctx, HOST_LINK_CMD_EV_RS485_RX);
}

static const char *rs485_get_version(void)
{
    return "Hailo15 HAL-RS485 2.0.0";
}

extern "C" {
HalRs485Ops HAL_RS485_OPS = {
    .rs485_init = rs485_init,
    .rs485_deinit = rs485_deinit,
    .rs485_tx = rs485_tx,
    .subscribe = subscribe,
    .unsubscribe = unsubscribe,
    .get_version = rs485_get_version,
};
}

