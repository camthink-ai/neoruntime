/**
 * @file stub_ota_impl.c
 * @brief Stub platform — HAL_OTA_OPS.
 */

#include "peripheral/devices/hal_ota.h"

#include <stddef.h>

static int stub_ota_install_from_file(void *mcu_ctx, const char *firmware_path,
                                      const HalOtaInstallOptions *opt)
{
    (void)mcu_ctx;
    (void)firmware_path;
    (void)opt;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_ota_get_download_status(void *mcu_ctx, HalOtaDownloadStatus *out)
{
    (void)mcu_ctx;
    if (out) {
        out->status = 0;
        out->downloaded = 0;
        out->total = 0;
        out->app_size = 0;
        out->app_crc32 = 0;
        out->in_session = 0;
    }
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_ota_abort(void *mcu_ctx)
{
    (void)mcu_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

HalOtaOps HAL_OTA_OPS = {
    .install_from_file = stub_ota_install_from_file,
    .get_download_status = stub_ota_get_download_status,
    .abort = stub_ota_abort,
};

