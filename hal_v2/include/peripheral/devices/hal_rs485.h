/**
 * @file hal_rs485.h
 * @brief RS485 UART via MCU with RX events.
 */
#pragma once

#include <stdint.h>
#include "../../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_RS485_CONFIG_LEN 3

typedef void (*HalRs485RxCallback)(void *mcu_ctx, const uint8_t *data, uint16_t len, void *userdata);

typedef struct {
    int (*rs485_init)(void *mcu_ctx, uint32_t baudrate, const char config[HAL_RS485_CONFIG_LEN]);
    int (*rs485_deinit)(void *mcu_ctx);
    int (*rs485_tx)(void *mcu_ctx, const uint8_t *data, uint16_t len);

    int (*subscribe)(void *mcu_ctx, HalRs485RxCallback cb, void *userdata);
    int (*unsubscribe)(void *mcu_ctx);

    const char *(*get_version)(void);
} HalRs485Ops;

extern HalRs485Ops HAL_RS485_OPS;

#ifdef __cplusplus
}
#endif

