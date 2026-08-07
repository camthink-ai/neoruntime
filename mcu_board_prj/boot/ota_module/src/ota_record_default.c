#include "../inc/ota_module.h"

/*
 * Default OTA record used when record storage area is corrupted (magic/CRC invalid).
 *
 * NOTE:
 * - You MUST fill in the real partition layout (flash_addr/flash_size) for your platform.
 * - Until then, the module can compile, but boot/download won't be functional on hardware.
 */

const ota_record_info_t ota_record_default_info = {
    .magic = OTA_INFO_MAGIC,

/* ota_app_info[] */
#if OTA_IS_DUAL_APP
    .ota_app_info = {
        {
            .status = OTA_APP_STATUS_ACTIVE,
            .verify_count = 0,
            .flash_type = OTA_FLASH_TYPE_INTERNAL,
            .flash_addr = OTA_PLATFORM_APP1_ADDR,
            .flash_size = OTA_PLATFORM_APP_PARTITION_SIZE,
        },
        {
            .status = OTA_APP_STATUS_INVALID,
            .verify_count = 0,
            .flash_type = OTA_FLASH_TYPE_INTERNAL,
            .flash_addr = OTA_PLATFORM_APP2_ADDR,
            .flash_size = OTA_PLATFORM_APP_PARTITION_SIZE,
        },
    },
#else
    .ota_app_info = {
        {
            .status = OTA_APP_STATUS_ACTIVE,
            .verify_count = 0,
            .flash_type = OTA_FLASH_TYPE_INTERNAL,
            .flash_addr = OTA_PLATFORM_APP1_ADDR,
            .flash_size = OTA_PLATFORM_APP_PARTITION_SIZE,
        },
    },
#endif

#if OTA_IS_HAVE_FACTORY_APP
    .factory_app_info = {
        .status = OTA_APP_STATUS_INVALID,
        .verify_count = 0,
        .flash_type = OTA_FLASH_TYPE_INTERNAL,
        .flash_addr = 0,
        .flash_size = 0,
    },
#endif

    .boot_ymodem_flag = 0,
    .info_crc32 = 0, /* computed at init/record_store time */
};

