/** @file stub_rs485_impl.c — HAL_RS485_OPS (stub) */
#include "peripheral/devices/hal_rs485.h"

static int stub_rs485_init(void *mcu_ctx, uint32_t baudrate, const char config[HAL_RS485_CONFIG_LEN])
{
    (void)mcu_ctx;
    (void)baudrate;
    (void)config;
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
    (void)mcu_ctx;
    (void)cb;
    (void)userdata;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_rs485_unsubscribe(void *mcu_ctx)
{
    (void)mcu_ctx;
    return HAL_ERR_NOT_SUPPORTED;
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
