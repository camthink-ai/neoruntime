/**
 * @file hailo15_mcu_priv.hpp
 * @brief Internal helpers for hailo15 MCU transport.
 *
 * This header is NOT part of the public API. Peripheral implementations for
 * hailo15 may include it to subscribe to host_link events.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

extern "C" {
#include "common/hal_common.h"
#include "common/host_link/host_link.h"
#include "peripheral/hal_mcu.h"
}

struct Hailo15McuCtx;

using Hailo15McuEventCb = std::function<void(uint16_t cmd, const uint8_t *payload, uint16_t len)>;

int hailo15_mcu_register_event_cb(Hailo15McuCtx *ctx, uint16_t cmd, Hailo15McuEventCb cb);
int hailo15_mcu_unregister_all_event_cbs(Hailo15McuCtx *ctx, uint16_t cmd);

/**
 * Map host_link (transport/protocol) error to HAL error codes.
 *
 * Note: this does not cover MCU firmware "business" errors unless they reuse
 * host_link_err_t values.
 */
int hailo15_mcu_map_host_err(int err);

/**
 * Raw request with per-call ACK timeout override.
 *
 * Use for long-running MCU operations (e.g. OTA flash erase/write).
 */
int hailo15_mcu_raw_request_timeout(void *mcu_ctx, uint16_t cmd,
                                   const uint8_t *payload, uint16_t payload_len,
                                   uint8_t *response, uint16_t response_size,
                                   uint16_t *response_len, uint32_t timeout_ms);

/**
 * @brief Suspend host_link RX, flush the UART, then run a raw serial callback (e.g. Ymodem).
 *
 * The callback receives the same @a mcu_ctx; use hailo15_mcu_raw_write_all / hailo15_mcu_raw_read.
 */
extern "C" {
/**
 * Suspend host_link RX (same as serial_exclusive), flush UART, run @a fn, flush RX, resume.
 * Use for short raw probes before host_link (e.g. detect bootloader Ymodem 'C').
 */
int hailo15_mcu_with_rx_suspended(void *mcu_ctx, int (*fn)(void *mcu_ctx2, void *user), void *user);

int hailo15_mcu_serial_exclusive(void *mcu_ctx, int (*fn)(void *mcu_ctx2, void *user), void *user);

/** Raw UART write while holding MCU TX lock (use inside serial_exclusive callback). */
int hailo15_mcu_raw_write_all(void *mcu_ctx, const uint8_t *data, size_t len);

/**
 * @brief Raw UART read (use inside serial_exclusive callback; RX thread is suspended).
 * @return bytes read, 0 on timeout, or negative errno code.
 */
ssize_t hailo15_mcu_raw_read(void *mcu_ctx, void *buf, size_t cap, uint32_t timeout_ms);

/** Init-time MCU configuration (e.g. optional SoC force-reset GPIO). */
const HalMcuConfig *hailo15_mcu_get_config(void *mcu_ctx);
} /* extern "C" */

/**
 * Map MCU status values to a HAL return code.
 *
 * - 0 -> HAL_OK
 * - Already a HAL error code -> passthrough
 * - host_link_err_t -> converted via hailo15_mcu_map_host_err()
 * - Otherwise -> HAL_ERROR
 */
static inline int hailo15_mcu_map_status(int32_t status)
{
    if (status == 0) {
        return 0;
    }

    // If MCU already returned a HAL code, preserve it.
    if ((status <= HAL_ERROR) && (status >= HAL_ERR_UNKNOW)) {
        return status;
    }

    return hailo15_mcu_map_host_err((int)status);
}

