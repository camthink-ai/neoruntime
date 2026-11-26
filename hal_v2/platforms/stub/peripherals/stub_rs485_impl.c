/** @file stub_rs485_impl.c — HAL_RS485_OPS (stub, RX-injectable for tests) */
#include "peripheral/devices/hal_rs485.h"

#include <stddef.h>

static HalRs485RxCallback s_rx_cb;
static void *s_rx_cb_user;
static void *s_mcu_ctx;

static int stub_rs485_init(void *mcu_ctx, uint32_t baudrate, const char config[HAL_RS485_CONFIG_LEN])
{
    (void)baudrate;
    (void)config;
    s_mcu_ctx = mcu_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_rs485_deinit(void *mcu_ctx)
{
    (void)mcu_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_rs485_tx(void *mcu_ctx, const uint8_t *data, uint16_t len)
{
    (void)mcu_ctx;
    (void)data;
    (void)len;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_rs485_subscribe(void *mcu_ctx, HalRs485RxCallback cb, void *userdata)
{
    if (cb == NULL) {
        return HAL_ERR_INVALID_ARG;
    }
    s_mcu_ctx = mcu_ctx;
    s_rx_cb = cb;
    s_rx_cb_user = userdata;
    return HAL_OK;
}

static int stub_rs485_unsubscribe(void *mcu_ctx)
{
    (void)mcu_ctx;
    s_rx_cb = NULL;
    s_rx_cb_user = NULL;
    return HAL_OK;
}

int stub_rs485_inject_rx(const uint8_t *data, uint16_t len)
{
    if (s_rx_cb == NULL || data == NULL || len == 0u) {
        return HAL_ERR_INVALID_STATE;
    }
    s_rx_cb(s_mcu_ctx, data, len, s_rx_cb_user);
    return HAL_OK;
}

static const char *stub_rs485_get_version(void)
{
    return "HAL-RS485 stub 2.0.0 (platform stub)";
}

HalRs485Ops HAL_RS485_OPS = {
    .rs485_init = stub_rs485_init,
    .rs485_deinit = stub_rs485_deinit,
    .rs485_tx = stub_rs485_tx,
    .subscribe = stub_rs485_subscribe,
    .unsubscribe = stub_rs485_unsubscribe,
    .get_version = stub_rs485_get_version,
};
