/**
 * @file hal_factory_format.h
 * @brief Storage-agnostic CTFB v1 A/B factory-identity format engine.
 *
 * This is the shared, medium-independent part of the factory EEPROM HAL: CRC,
 * dual-slot selection, and field encode/decode. It operates on a small linear
 * byte I/O interface (@ref HalFactoryIo) supplied by the caller, so the exact
 * same logic drives the built-in HailoRT I2C backend, a custom SPI-flash / MTD /
 * file backend, or a host-side unit-test fixture.
 *
 * Internal header — not part of the public HAL API.
 */
#pragma once

#include <stdint.h>
#include <stddef.h>

#include "peripheral/devices/hal_factory.h"

#ifdef __cplusplus
extern "C" {
#endif

/* EEPROM / image geometry. */
#define HAL_FACTORY_EEPROM_SIZE 256
#define HAL_FACTORY_SLOT_SIZE   128
#define HAL_FACTORY_SLOT_A_BASE 0
#define HAL_FACTORY_SLOT_B_BASE 128

/**
 * Linear byte I/O used by the format engine. @a addr is an absolute offset into
 * the 256-byte image; @a len is 1..256. Read/write return 0 or a negative
 * HalErrorCode. Write granularity / wear / erase are the backend's job.
 */
typedef struct {
    int (*read)(void *user, uint32_t addr, uint8_t *buf, uint32_t len);
    int (*write)(void *user, uint32_t addr, const uint8_t *buf, uint32_t len);
    void *user;
} HalFactoryIo;

/**
 * Adapt a public @ref HalFactoryStorage into a @ref HalFactoryIo. The two share
 * the same function signatures; this just hides the struct-shape difference.
 */
static inline HalFactoryIo hal_factory_io_from_storage(const HalFactoryStorage *s)
{
    HalFactoryIo io;
    io.read = s->read;
    io.write = s->write;
    io.user = s->user;
    return io;
}

int hal_factory_format_read_all(const HalFactoryIo *io, HalFactoryInfo *out);
int hal_factory_format_get(const HalFactoryIo *io, HalFactoryField field,
                           char *value, uint32_t value_size);
int hal_factory_format_set(const HalFactoryIo *io, HalFactoryField field, const char *value);
int hal_factory_format_erase(const HalFactoryIo *io);

#ifdef __cplusplus
}
#endif
