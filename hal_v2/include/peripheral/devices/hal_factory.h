/**
 * @file hal_factory.h
 * @brief HAL Factory - board-level factory identity EEPROM (CTFB v1 A/B).
 *
 * Provides access to the factory-programmed device identity block stored on the
 * onboard 256-byte storage device. The on-disk layout and semantics are identical
 * to the platform `factory-eeprom.sh` tool so this HAL component is a drop-in
 * replacement for the shell tool's read/write path:
 *
 *   - 256-byte storage split into two 128-byte slots (Slot A @0x00, Slot B @0x80).
 *   - Each slot is a self-describing record:
 *       +0x00  magic  "CTFB" (4 ASCII bytes)
 *       +0x04  layout version (u8) == 1
 *       +0x05  flags (u8): bit0 = programmed, bit1 = MAC valid
 *       +0x06  CRC16-CCITT-FALSE (u16, little-endian) over payload [0x10..0x7F]
 *       +0x08  sequence number (u32, little-endian)
 *       +0x0C  reserved (0xFF x4)
 *       +0x10  MAC   (6 raw bytes)
 *       +0x18  SN    (32 ASCII bytes, 0xFF-padded)
 *       +0x38  PN    (16 ASCII bytes, 0xFF-padded)
 *       +0x48  BATCH ( 8 ASCII bytes, 0xFF-padded)
 *       +0x50  HWREV ( 8 ASCII bytes, 0xFF-padded)
 *   - The "active" slot is the valid slot (magic+version+CRC ok) with the highest
 *     sequence number. Writes go to the *inactive* slot with seq = active.seq + 1
 *     (power-fail safe), preserving all other fields.
 *
 * ## Storage-agnostic design
 *
 * The CTFB layout / CRC / slot logic is independent of the physical medium. The
 * only transport-specific part is how a contiguous, byte-addressable region is
 * read/written. Select the backend at init:
 *
 *   - @ref HAL_FACTORY_BACKEND_LINUX (default): read/write the Linux EEPROM file
 *     node (e.g. the at24 sysfs node `/sys/bus/i2c/devices/1-0050/eeprom`) with
 *     `pread`/`pwrite`. This is the same path the platform `factory-eeprom.sh`
 *     tool uses and works when the EEPROM hangs off a SoC I2C bus with the
 *     kernel `at24` driver bound.
 *   - @ref HAL_FACTORY_BACKEND_I2C: HailoRT I2C control interface
 *     (@c hailort::Device::i2c_read / @c i2c_write). NOTE: this targets the Hailo
 *     chip's own I2C controller and requires the I2C control opcode to be
 *     compiled into the HailoRT firmware; it is not available on all builds.
 *   - @ref HAL_FACTORY_BACKEND_CUSTOM: a caller-supplied @ref HalFactoryStorage
 *     (read/write a byte range). Use this for SPI NOR flash, an MTD partition, a
 *     raw file, or any other linear byte-addressable medium. The backend is
 *     responsible for its own write granularity / wear / erase; the format layer
 *     only issues logical byte-range reads and writes.
 *
 * Lifecycle:
 *   1. HAL_FACTORY_OPS.init(&config, &factory_ctx)   (config may be NULL → Linux defaults)
 *   2. HAL_FACTORY_OPS.read_all / get / set / erase (factory_ctx)
 *   3. HAL_FACTORY_OPS.deinit(factory_ctx)
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
 * Field enumeration
 * -------------------------------------------------------------------- */

/** Factory identity field. */
typedef enum {
    HAL_FACTORY_FIELD_SN    = 0,  /**< serial number (32 ASCII) */
    HAL_FACTORY_FIELD_MAC   = 1,  /**< MAC address (6 raw bytes) */
    HAL_FACTORY_FIELD_PN    = 2,  /**< product number (16 ASCII) */
    HAL_FACTORY_FIELD_BATCH = 3,  /**< batch number (8 ASCII) */
    HAL_FACTORY_FIELD_HWREV = 4,  /**< hardware revision (8 ASCII) */
} HalFactoryField;

/* --------------------------------------------------------------------
 * Storage backend
 * -------------------------------------------------------------------- */

/** Byte order of multi-byte values in the I2C register/transfer. */
typedef enum {
    HAL_FACTORY_BIG_ENDIAN    = 0,
    HAL_FACTORY_LITTLE_ENDIAN = 1,
} HalFactoryEndianness;

/** How the factory storage is accessed. */
typedef enum {
    HAL_FACTORY_BACKEND_LINUX   = 0,  /**< Linux EEPROM file node (at24 sysfs, default). */
    HAL_FACTORY_BACKEND_I2C     = 1,  /**< HailoRT I2C control interface. */
    HAL_FACTORY_BACKEND_CUSTOM  = 2,  /**< caller-supplied linear storage. */
} HalFactoryBackend;

/**
 * Linear, byte-addressable storage accessor used for non-I2C backends
 * (SPI flash, MTD partition, file, ...). Return 0 on success or a negative
 * HalErrorCode. Implementations must honour arbitrary byte ranges: @a addr is
 * an absolute offset into the 256-byte factory image, @a len is 1..256.
 *
 * Write granularity / wear levelling / erase are the backend's responsibility.
 */
typedef struct {
    int (*read)(void *user, uint32_t addr, uint8_t *buf, uint32_t len);
    int (*write)(void *user, uint32_t addr, const uint8_t *buf, uint32_t len);
    void *user;  /**< opaque backend context. */
} HalFactoryStorage;

/**
 * Factory access configuration. Pass NULL to @ref HalFactoryOps.init to use the
 * built-in Linux EEPROM backend with the platform default device node.
 */
typedef struct {
    HalFactoryBackend backend;   /**< storage backend (default Linux sysfs). */
    HalFactoryStorage storage;   /**< used when @a backend == @ref HAL_FACTORY_BACKEND_CUSTOM. */

    /** Linux backend device node (e.g. "/sys/bus/i2c/devices/1-0050/eeprom"). NULL → platform default. */
    const char *device_path;

    /* ---- I2C-only (used when @a backend == @ref HAL_FACTORY_BACKEND_I2C) ---- */
    uint16_t slave_address;         /**< 7-bit I2C slave address (default 0x50). */
    uint8_t  register_address_size; /**< EEPROM word/register address width in bytes (default 1). */
    uint8_t  bus_index;             /**< Hailo I2C bus index (default 1). */
    uint8_t  endianness;            /**< @ref HalFactoryEndianness (default little-endian). */
} HalFactoryConfig;

/* --------------------------------------------------------------------
 * Identity info
 * -------------------------------------------------------------------- */

/**
 * The factory identity block read from the active slot. All string fields are
 * NUL-terminated and empty when unprogrammed.
 */
typedef struct {
    char     serial_number[33];       /**< SN */
    char     mac_address[18];         /**< MAC formatted "aa:bb:cc:dd:ee:ff" (lowercase); empty string when the field is unprogrammed */
    char     product_number[17];      /**< PN */
    char     batch[9];                /**< BATCH */
    char     hardware_revision[9];    /**< HWREV */
    char     active_slot;             /**< 'A', 'B', or '\0' when no valid slot. */
    uint32_t seq;                     /**< active slot sequence number (0 when none). */
    bool     valid;                   /**< true when an active valid slot was found. */
} HalFactoryInfo;

/* --------------------------------------------------------------------
 * Operations table
 * -------------------------------------------------------------------- */

/**
 * Function-pointer table for factory storage operations.
 * Platform implementations populate HAL_FACTORY_OPS at link time.
 */
typedef struct {
    /**
     * @brief Initialize factory storage access.
     * @param config            Backend/I2C config; NULL selects the built-in I2C defaults.
     * @param factory_ctx_return Receives the allocated context on success.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*init)(const HalFactoryConfig *config, void **factory_ctx_return);

    /**
     * @brief Tear down factory storage access.
     * @param factory_ctx Context returned by init().
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*deinit)(void *factory_ctx);

    /**
     * @brief Read the complete factory identity block from the active slot.
     * @param factory_ctx Context returned by init().
     * @param out         Receives the identity block. @c out->valid is false and
     *                    @c out->active_slot is '\\0' when no slot validates.
     * @return 0 on success, negative HalErrorCode on failure (incl. HAL_ERR_NOT_FOUND
     *         when no valid slot exists).
     */
    int (*read_all)(void *factory_ctx, HalFactoryInfo *out);

    /**
     * @brief Read a single field from the active slot.
     * @param factory_ctx Context returned by init().
     * @param field       Field to read.
     * @param value       Caller buffer receiving the NUL-terminated value.
     * @param value_size  Capacity of @a value in bytes.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get)(void *factory_ctx, HalFactoryField field, char *value, uint32_t value_size);

    /**
     * @brief Write one field, preserving all other fields.
     *
     * Writes to the inactive slot with seq = active.seq + 1 (power-fail safe),
     * recomputes the header CRC, then reads the slot back and verifies it.
     *
     * @param factory_ctx Context returned by init().
     * @param field       Field to write.
     * @param value       New value. MAC must be "aa:bb:cc:dd:ee:ff" (case-insensitive).
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*set)(void *factory_ctx, HalFactoryField field, const char *value);

    /**
     * @brief Erase the whole 256-byte factory image (all bytes to 0xFF).
     *
     * Destructive and irreversible. Intended for factory re-programming only.
     * @param factory_ctx Context returned by init().
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*erase)(void *factory_ctx);

    /**
     * @brief Get the factory HAL version string.
     * @return Static version string e.g. "Hailo15 HAL-FACTORY 2.0.0".
     */
    const char *(*get_version)(void);
} HalFactoryOps;

/** Platform-specific factory storage operations (resolved at link time). */
extern HalFactoryOps HAL_FACTORY_OPS;

#ifdef __cplusplus
}
#endif
