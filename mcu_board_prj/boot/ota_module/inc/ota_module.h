#ifndef __OTA_MODULE_H__
#define __OTA_MODULE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ota_module_config.h"

#define OTA_PACKAGE_MAGIC               0x5441544F
#define OTA_INFO_MAGIC                  OTA_PACKAGE_MAGIC
#define OTA_PACKAGE_VERSION_MAX_LEN     32

#ifndef OTA_APP_MAX_VERIFY_COUNT
#define OTA_APP_MAX_VERIFY_COUNT        3
#endif

#ifndef OTA_IS_DUAL_APP
#define OTA_IS_DUAL_APP                 0
#endif

#ifndef OTA_IS_HAVE_FACTORY_APP
#define OTA_IS_HAVE_FACTORY_APP         0
#endif

/**
 * @brief OTA module error codes
 * @note All OTA module public APIs return only these unified error codes.
 */
typedef enum {
    OTA_MODULE_ERR_OK = 0,
    OTA_MODULE_ERR_INVALID_ARG = -1,
    OTA_MODULE_ERR_NOT_INIT = -2,
    OTA_MODULE_ERR_ALREADY_IN_PROGRESS = -3,
    OTA_MODULE_ERR_FLASH_OP = -4,
    OTA_MODULE_ERR_RECORD_IO = -5,
    OTA_MODULE_ERR_RECORD_INVALID = -6,
    OTA_MODULE_ERR_PACKAGE_MAGIC = -7,
    OTA_MODULE_ERR_PACKAGE_HEADER_CRC = -8,
    OTA_MODULE_ERR_SLOT_UNAVAILABLE = -9,
    OTA_MODULE_ERR_FLASH_SIZE = -10,
    OTA_MODULE_ERR_SESSION_MISMATCH = -11,
    OTA_MODULE_ERR_OFFSET_MISMATCH = -12,
    OTA_MODULE_ERR_SIZE_MISMATCH = -13,
    OTA_MODULE_ERR_APP_STATUS = -14,
    OTA_MODULE_ERR_CRC_MISMATCH = -15,
    OTA_MODULE_ERR_NO_BOOT_CANDIDATE = -16,
    OTA_MODULE_ERR_JUMP_FAILED = -17,
} ota_module_err_t;

/* ================= Logging =================
 * Platform can enable logs by defining `OTA_MODULE_LOG(...)`.
 * Example:
 *   #define OTA_MODULE_LOG(fmt, ...) printf(fmt, ##__VA_ARGS__)
 *
 * Control verbosity at compile-time with `OTA_MODULE_LOG_LEVEL`:
 *   0=ERROR, 1=WARN, 2=INFO, 3=DEBUG (default: ERROR)
 */
#ifndef OTA_MODULE_LOG_LEVEL
#define OTA_MODULE_LOG_LEVEL 0
#endif

#define OTA_LOG_LEVEL_ERROR 0
#define OTA_LOG_LEVEL_WARN  1
#define OTA_LOG_LEVEL_INFO  2
#define OTA_LOG_LEVEL_DEBUG 3

#ifndef OTA_MODULE_LOG
#define OTA_MODULE_LOG(...) printf(__VA_ARGS__)
#endif

#if OTA_MODULE_LOG_LEVEL >= OTA_LOG_LEVEL_ERROR
#define OTA_LOGE(fmt, ...) OTA_MODULE_LOG("[OTA][E] " fmt "\r\n", ##__VA_ARGS__)
#else
#define OTA_LOGE(fmt, ...) do {} while (0)
#endif

#if OTA_MODULE_LOG_LEVEL >= OTA_LOG_LEVEL_WARN
#define OTA_LOGW(fmt, ...) OTA_MODULE_LOG("[OTA][W] " fmt "\r\n", ##__VA_ARGS__)
#else
#define OTA_LOGW(fmt, ...) do {} while (0)
#endif

#if OTA_MODULE_LOG_LEVEL >= OTA_LOG_LEVEL_INFO
#define OTA_LOGI(fmt, ...) OTA_MODULE_LOG("[OTA][I] " fmt "\r\n", ##__VA_ARGS__)
#else
#define OTA_LOGI(fmt, ...) do {} while (0)
#endif

#if OTA_MODULE_LOG_LEVEL >= OTA_LOG_LEVEL_DEBUG
#define OTA_LOGD(fmt, ...) OTA_MODULE_LOG("[OTA][D] " fmt "\r\n", ##__VA_ARGS__)
#else
#define OTA_LOGD(fmt, ...) do {} while (0)
#endif

/**
 * @brief OTA application status
 * @note When an unverified app is verified as normal, its status will be set to active, and other activated apps will be set as backup
 */
typedef enum {
    OTA_APP_STATUS_INVALID = 0, 
    OTA_APP_STATUS_DOWNLOADING,
    OTA_APP_STATUS_UNVERIFIED,
    OTA_APP_STATUS_ACTIVE,
    OTA_APP_STATUS_BACKUP,
    OTA_APP_STATUS_MAX,
} ota_app_status_t;

/**
 * @brief OTA download status
 * @note 
 */
typedef enum {
    OTA_DOWNLOAD_STATUS_INVALID = 0,
    OTA_DOWNLOAD_STATUS_DOWNLOADING,
    OTA_DOWNLOAD_STATUS_COMPLETED,
    OTA_DOWNLOAD_STATUS_MAX,
} ota_download_status_t;

/**
 * @brief OTA flash type
 * @note 
 */
typedef enum {
    OTA_FLASH_TYPE_INTERNAL = 0,
    OTA_FLASH_TYPE_EXTERNAL,
    OTA_FLASH_TYPE_RAM,
    OTA_FLASH_TYPE_MAX,
} ota_flash_type_t;

#pragma pack(push, 1)
/**
 * @brief OTA package header
 * @note 
 */
typedef struct {
    uint32_t    magic;
    uint32_t    build_timestamp;
    uint32_t    app_offset;
    uint32_t    app_size;
    uint32_t    app_crc32;
    char        app_version[OTA_PACKAGE_VERSION_MAX_LEN];
    
    uint32_t    header_crc32;
} ota_package_header_t;

/**
 * @brief OTA application information
 * @note 
 */
typedef struct {
    ota_app_status_t status;
    uint32_t    verify_count;
    ota_flash_type_t flash_type;
    uint32_t    flash_addr;
    uint32_t    flash_size;
} ota_app_info_t;

/**
 * @brief OTA information
 * @note 
 */
typedef struct {
    uint32_t    magic;

    ota_app_info_t ota_app_info[OTA_IS_DUAL_APP ? 2 : 1];
#if OTA_IS_HAVE_FACTORY_APP
    ota_app_info_t factory_app_info;
#endif

    uint32_t    boot_ymodem_flag; /* 0: not boot Ymodem, 1: boot Ymodem */
    uint32_t    info_crc32;
} ota_record_info_t;
#pragma pack(pop)

/**
 * @brief OTA download information
 * @note 
 */
typedef struct {
    ota_download_status_t status;
    ota_app_info_t ota_app_info;
    ota_package_header_t package_header;
    uint32_t download_size;
    uint32_t download_crc32;
} ota_download_info_t;

/**
 * @brief OTA flash read function
 * @param flash_type: flash type
 * @param addr: address
 * @param data: data
 * @param size: size
 * @return 0: success, other: error
 */
typedef int (*ota_flash_read_func_t)(ota_flash_type_t flash_type, uint32_t addr, uint8_t *data, uint32_t size);

/**
 * @brief OTA flash write function
 * @param flash_type: flash type
 * @param addr: address
 * @param data: data
 * @param size: size
 * @return 0: success, other: error
 */
typedef int (*ota_flash_write_func_t)(ota_flash_type_t flash_type, uint32_t addr, const uint8_t *data, uint32_t size);

/**
 * @brief OTA flash erase function
 * @param flash_type: flash type
 * @param addr: address
 * @param size: size
 * @return 0: success, other: error
 */
typedef int (*ota_flash_erase_func_t)(ota_flash_type_t flash_type, uint32_t addr, uint32_t size);

/**
 * @brief OTA CRC32 function
 * @param mode: CRC32 mode
 * @note 0: RESET, 1: CONTINUE
 * @param data: data
 * @param size: size
 * @return CRC32 value
 */
typedef uint32_t (*ota_crc32_func_t)(uint8_t mode, const uint8_t *data, uint32_t size);

/**
 * @brief OTA jump function
 * @param jump_addr: jump address
 * @return 0: success, other: error
 */
typedef int (*ota_jump_func_t)(uint32_t jump_addr);

/**
 * @brief OTA module configuration
 * @note 
 */
typedef struct {
    ota_flash_type_t        record_flash_type;
    uint32_t                record_addr;

    uint32_t                flash_start_addr[OTA_FLASH_TYPE_MAX];
    uint32_t                flash_size[OTA_FLASH_TYPE_MAX];
    uint32_t                flash_sector_size[OTA_FLASH_TYPE_MAX];

    ota_flash_read_func_t   flash_read_func;
    ota_flash_write_func_t  flash_write_func;
    ota_flash_erase_func_t  flash_erase_func;
    ota_crc32_func_t        crc32_func;
    ota_jump_func_t         jump_func;
} ota_module_config_t;

/**
 * @brief OTA platform configuration
 * @note 
 */
extern const ota_module_config_t ota_module_platform_config;

/**
 * @brief Initialize OTA module
 * @param config: OTA module configuration
 * @return 0: success, other: error
 */
int ota_module_init(const ota_module_config_t *config);

/**
 * @brief Boot preprocess
 * @note Before starting the app: refresh partition states as needed, then jump to the selected OTA slot (execute in place). Copy-to-another-region, if required, is left to the integrator.
 * @return 0: success, other: error
 */
int ota_module_boot_preprocess(void);

/**
 * @brief True if OTA record has at least one ACTIVE application slot (OTA partition or factory).
 * @note Used by bootloader when Ymodem wait times out: may system-reset to re-enter boot chain.
 */
int ota_module_has_active_app_region(void);

/**
 * @brief OTA get record information
 * @param info: record information
 * @return 0: success, other: error
 */
int ota_module_get_record_info(ota_record_info_t *info);

/**
 * @brief OTA download start
 * @param package_header: package header (pointer valid for the duration of this call)
 * @note This API only validates `ota_package_header_t` (magic + header_crc32)
 *       and erases the required flash range:
 *       [0, app_offset + app_size) relative to the selected OTA partition start.
 *       It does NOT write any firmware bytes; all bytes are written by `ota_module_ota_download()`.
 *       When configured as single-slot (`OTA_IS_DUAL_APP==0`), the module may reuse/overwrite
 *       the only slot even if it is ACTIVE/UNVERIFIED. This is intended for bootloader-driven
 *       updates; overwriting an executing image from the running app is unsafe.
 * @return 0: success, other: error
 */
int ota_module_ota_download_start(const ota_package_header_t *package_header);

/**
 * @brief OTA download
 * @param offset: offset in the OTA package (relative to `ota_package_header_t` start)
 *       (i.e. `offset=0` means writing the package header start)
 * @note This function writes raw bytes to the selected OTA partition:
 *       `flash_addr + offset`. Upstream is expected to write continuously
 *       from `offset=0` until `offset=app_offset+app_size`.
 * @param data: data
 * @param size: size
 * @return 0: success, other: error
 */
int ota_module_ota_download(uint32_t offset, const uint8_t *data, uint32_t size);

/**
 * @brief OTA get download information
 * @param info: download information
 * @note This API is meaningful within the same download session
 *       (from `ota_module_ota_download_start()` to `ota_module_ota_download_finish()` /
 *        `ota_module_ota_download_abort()`).
 * @return 0: success, other: error
 */
int ota_module_ota_get_download_info(ota_download_info_t *info);

/**
 * @brief OTA download finish
 * @note Post processing after OTA (checking app is valid and updating the status of each partition, etc.)
 * @return 0: success, other: error
 */
int ota_module_ota_download_finish(void);

/**
 * @brief Mark current running app as verified/normal
 * @note Call from the application after it has successfully run the new image.
 *       Promotes the first `UNVERIFIED` OTA slot to `ACTIVE` and demotes other active/unverified OTA slots to `BACKUP`.
 * @return 0: success, other: error
 */
int ota_module_app_mark_verified(void);

/**
 * @brief Abort current OTA download
 * @note Keep the target OTA partition in an unavailable state (INVALID),
 *       so `ota_module_boot_preprocess()` will skip it and next OTA download
 *       can reuse/overwrite it.
 * @return 0: success, other: error
 */
int ota_module_ota_download_abort(void);

/**
 * @brief Boot Ymodem wait
 * @param flag: 0: not boot Ymodem, 1: boot Ymodem
 * @return 0: success, other: error
 */
int ota_module_set_boot_ymodem_flag(uint32_t flag);

/**
 * @brief Boot Ymodem wait
 * @return 0: not boot Ymodem, 1: boot Ymodem
 */
uint32_t ota_module_get_boot_ymodem_flag(void);

#ifdef __cplusplus
}
#endif

#endif /* __OTA_MODULE_H__ */
