/**
 * @file hal_mcu.h
 * @brief HAL MCU - generic MCU communication transport.
 *
 * Provides an abstract serial / framed-protocol channel to a board-level MCU
 * (e.g. STM32 host_link).  Used by peripheral device modules in
 * include/peripheral/devices/ to issue commands and receive responses.
 *
 * The MCU module is intentionally generic: it offers a raw_request entry
 * point so that any peripheral can layer its own command IDs on top, plus a
 * few well-known operations (ping / echo / version) for sanity checks.
 *
 * Lifecycle:
 *   1. HAL_MCU_OPS.init(&config, &mcu_ctx)
 *   2. Pass mcu_ctx to peripheral device modules (devices/hal_*.h)
 *   3. HAL_MCU_OPS.deinit(mcu_ctx)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum payload size for a single MCU request or response. */
#define HAL_MCU_MAX_PAYLOAD 512

/** Maximum length of the version string returned by get_version. */
#define HAL_MCU_VERSION_MAX_LEN 64

/* --------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------- */

/** MCU transport configuration. */
typedef struct {
    const char *serial_device;   /**< serial device path, e.g. "/dev/ttyS0" */
    uint32_t    baud_rate;       /**< baud rate (e.g. 115200) */
    uint32_t    timeout_ms;      /**< default request/response timeout in ms */
    void       *priv;            /**< platform-specific extension (opaque) */
    /**
     * Optional Linux-side GPIO for pulsing @b MCU reset from the host (libgpiod; same numbering as
     * @ref HalGpioConfig::gpio_num on Hailo-15). Used only by @ref HalEnvCtrlOps.reset_mcu(..., @c force=true).
     * Unrelated to @c HOST_LINK_CMD_RESET_SOC. Zero disables the GPIO path (@c HAL_ERR_NOT_SUPPORTED).
     *
     * Hailo-15 (ne503): MCU NRST / timer_ext net @b gpiochip1 line 2 → HAL index @b 18 (typically active-high pulse).
     */
    uint32_t    host_mcu_reset_gpio;
    /** 1: MCU reset line is electrically active-low (@ref HalGpioConfig::active_low semantics). */
    uint8_t     host_mcu_reset_active_low;
    uint8_t     host_mcu_reset_pulse_ms;   /**< pulse hold width; 0 defaults to 200 ms in implementation */
    uint8_t     _pad[2];
} HalMcuConfig;

/** MCU firmware version information. */
typedef struct {
    int32_t major;
    int32_t minor;
    int32_t patch;
    int32_t build;
    char    version_str[HAL_MCU_VERSION_MAX_LEN];
} HalMcuVersion;

/* --------------------------------------------------------------------
 * MCU operations table
 * -------------------------------------------------------------------- */

/**
 * Function-pointer table for MCU communication operations.
 * Platform implementations populate HAL_MCU_OPS at link time.
 */
typedef struct {
    /**
     * @brief Initialize the MCU transport (open serial port, start I/O thread).
     * @param config        MCU configuration (copied internally).
     * @param mcu_ctx_return Receives the allocated MCU context on success.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*init)(const HalMcuConfig *config, void **mcu_ctx_return);

    /**
     * @brief Tear down the MCU transport.
     * @param mcu_ctx Context returned by init().
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*deinit)(void *mcu_ctx);

    /**
     * @brief Get MCU firmware version.
     * @param mcu_ctx MCU context.
     * @param version Receives the version information.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_version)(void *mcu_ctx, HalMcuVersion *version);

    /**
     * @brief Round-trip a 32-bit value (sanity check).
     * @param mcu_ctx MCU context.
     * @param value   Value to send.
     * @param echo    Receives the value echoed back by the MCU.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*ping)(void *mcu_ctx, uint32_t value, uint32_t *echo);

    /**
     * @brief Round-trip an arbitrary byte buffer.
     * @param mcu_ctx  MCU context.
     * @param data     Bytes to send.
     * @param len      Number of bytes to send.
     * @param out      Buffer to receive echoed bytes.
     * @param out_size Capacity of @a out in bytes.
     * @param out_len  Receives the number of echoed bytes written.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*echo)(void *mcu_ctx, const uint8_t *data, uint16_t len,
                uint8_t *out, uint16_t out_size, uint16_t *out_len);

    /**
     * @brief Query MCU transport status.
     * @param mcu_ctx MCU context.
     * @return HalStatus value (cast from int).
     */
    int (*get_status)(void *mcu_ctx);

    /**
     * @brief Generic command dispatch (request / response).
     *
     * Used by peripheral device modules to layer their own command IDs on
     * top of this transport.  The wire framing (header, CRC, addressing)
     * is hidden inside the implementation.
     *
     * @param mcu_ctx       MCU context.
     * @param cmd           Command ID (peripheral-specific).
     * @param payload       Request payload bytes (may be NULL if payload_len == 0).
     * @param payload_len   Request payload length (<= HAL_MCU_MAX_PAYLOAD).
     * @param response      Buffer to receive response payload (may be NULL if response_size == 0).
     * @param response_size Capacity of @a response in bytes.
     * @param response_len  Receives the actual response length on success.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*raw_request)(void *mcu_ctx, uint16_t cmd,
                       const uint8_t *payload, uint16_t payload_len,
                       uint8_t *response, uint16_t response_size,
                       uint16_t *response_len);

    /**
     * @brief Get the MCU HAL version string.
     * @return Static version string e.g. "Hailo15 HAL-MCU 2.0.0".
     */
    const char *(*get_hal_version)(void);
} HalMcuOps;

/** Platform-specific MCU operations (resolved at link time). */
extern HalMcuOps HAL_MCU_OPS;

#ifdef __cplusplus
}
#endif
