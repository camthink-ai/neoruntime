/**
 * @file stub_mcu_impl.c
 * @brief Stub platform — HAL_MCU_OPS.
 */

#include "peripheral/hal_mcu.h"

#include <stddef.h>
#include <string.h>

static int stub_mcu_init(const HalMcuConfig *config, void **mcu_ctx_return)
{
    (void)config;
    if (!mcu_ctx_return)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *mcu_ctx_return = NULL;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_mcu_deinit(void *mcu_ctx)
{
    (void)mcu_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_mcu_get_version(void *mcu_ctx, HalMcuVersion *version)
{
    (void)mcu_ctx;
    if (!version)
    {
        return HAL_ERR_INVALID_ARG;
    }
    memset(version, 0, sizeof(*version));
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_mcu_ping(void *mcu_ctx, uint32_t value, uint32_t *echo)
{
    (void)mcu_ctx;
    (void)value;
    if (echo)
    {
        *echo = 0;
    }
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_mcu_echo(void *mcu_ctx, const uint8_t *data, uint16_t len, uint8_t *out, uint16_t out_size,
                         uint16_t *out_len)
{
    (void)mcu_ctx;
    (void)data;
    (void)len;
    (void)out;
    (void)out_size;
    if (out_len)
    {
        *out_len = 0;
    }
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_mcu_get_status(void *mcu_ctx)
{
    (void)mcu_ctx;
    return (int)HAL_STATUS_UNINITIALIZED;
}

static int stub_mcu_raw_request(void *mcu_ctx, uint16_t cmd, const uint8_t *payload, uint16_t payload_len,
                                uint8_t *response, uint16_t response_size, uint16_t *response_len)
{
    (void)mcu_ctx;
    (void)cmd;
    (void)payload;
    (void)payload_len;
    (void)response;
    (void)response_size;
    if (response_len)
    {
        *response_len = 0;
    }
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *stub_mcu_get_hal_version(void)
{
    return "HAL-MCU stub 2.0.0 (platform stub)";
}

HalMcuOps HAL_MCU_OPS = {
    .init = stub_mcu_init,
    .deinit = stub_mcu_deinit,
    .get_version = stub_mcu_get_version,
    .ping = stub_mcu_ping,
    .echo = stub_mcu_echo,
    .get_status = stub_mcu_get_status,
    .raw_request = stub_mcu_raw_request,
    .get_hal_version = stub_mcu_get_hal_version,
};
