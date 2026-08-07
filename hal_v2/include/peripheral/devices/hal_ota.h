/**
 * @file hal_ota.h
 * @brief MCU OTA via bootloader Ymodem (after OTA_ENTER_BOOT).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief OTA progress callback.
 *
 * Called from the caller's thread (i.e. inside install_from_file()).
 */
typedef void (*HalOtaProgressCb)(void *user, uint32_t sent_bytes, uint32_t total_bytes);

/**
 * @brief OTA install options.
 */
typedef struct {
    /** Delay after MCU acknowledges OTA_ENTER_BOOT before starting Ymodem (MCU reset + boot). */
    uint32_t post_enter_boot_delay_ms;
    /** Max wait for bootloader CRC mode ('C') before failing Ymodem start. */
    uint32_t ymodem_crc_char_timeout_ms;
    /**
     * If OTA_ENTER_BOOT fails: @c reset_mcu(..., false) (@c HOST_LINK_CMD_REBOOT); if that fails,
     * @c reset_mcu(..., true) when @ref HalMcuConfig.host_mcu_reset_gpio is non-zero;
     * if still failing, @c reset_soc(); wait @ref force_reboot_settle_ms; retry @c OTA_ENTER_BOOT once.
     */
    bool force_reboot_on_enter_boot_fail;
    /** Sleep after force reset before retrying OTA_ENTER_BOOT (default 2000 ms if unset/0). */
    uint32_t force_reboot_settle_ms;
    HalOtaProgressCb progress_cb;
    void *progress_user;
} HalOtaInstallOptions;

/**
 * @brief Reserved for legacy host_link OTA session status (not used on ne503 Ymodem path).
 */
typedef struct {
    int32_t status;
    uint32_t downloaded;
    uint32_t total;
    uint32_t app_size;
    uint32_t app_crc32;
    bool in_session;
} HalOtaDownloadStatus;

typedef struct HalOtaOps {
    /**
     * @brief MCU OTA: request bootloader Ymodem mode, then send firmware image.
     *
     * Flow matches mcu_board_prj app OTA host: HOST_LINK_CMD_OTA_ENTER_BOOT, MCU reboots,
     * bootloader receives the same OTA package file (pack_ota.py) via Ymodem-CRC.
     */
    int (*install_from_file)(void *mcu_ctx, const char *firmware_path,
                             const HalOtaInstallOptions *opt);

    /**
     * @brief Legacy session query (not supported on ne503 Ymodem OTA; returns HAL_ERR_NOT_SUPPORTED).
     */
    int (*get_download_status)(void *mcu_ctx, HalOtaDownloadStatus *out);

    /**
     * @brief Legacy abort (not supported on ne503 Ymodem OTA; returns HAL_ERR_NOT_SUPPORTED).
     */
    int (*abort)(void *mcu_ctx);
} HalOtaOps;

extern HalOtaOps HAL_OTA_OPS;

#ifdef __cplusplus
}
#endif

