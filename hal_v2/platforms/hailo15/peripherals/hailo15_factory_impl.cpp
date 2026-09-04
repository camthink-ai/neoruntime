/**
 * @file hailo15_factory_impl.cpp
 * @brief hailo15 factory identity storage — Linux / HailoRT I2C / custom backends.
 *
 * Thin storage layer over the shared CTFB format engine
 * (common/devices/hal_factory_format.c). Three backends are supported:
 *
 *   - Linux (default): read/write the kernel at24 EEPROM file node (e.g.
 *     /sys/bus/i2c/devices/1-0050/eeprom) with pread/pwrite. This is the same
 *     path the platform `factory-eeprom.sh` tool uses, and is the correct entry
 *     point when the AT24C02 hangs off a SoC I2C bus with the at24 driver bound.
 *     No vendor (HailoRT) dependency — always available.
 *   - I2C: HailoRT `hailort::Device::i2c_read` / `i2c_write` control path.
 *     Compiled in only when HAL_HAVE_HAILORT is defined. NOTE: this targets the
 *     Hailo chip's own I2C controller and requires the I2C control opcode to be
 *     compiled into the HailoRT firmware; it is NOT available on all builds
 *     (some HAILO15H firmwares reject it with CONTROL_UNSUPPORTED). The AT24C02
 *     has an 8-byte page write buffer and a ~5 ms write cycle, so writes are
 *     page-chunked. Selecting this backend without hailort returns NOT_SUPPORTED.
 *   - Custom: a caller-supplied @ref HalFactoryStorage (SPI flash, MTD, file,
 *     ...) is forwarded verbatim to the format engine; the backend owns its
 *     write granularity / wear / erase.
 */

#if defined(HAL_HAVE_HAILORT)
#include <hailo/hailort.hpp>
#include <hailo/hailort.h>
#endif

extern "C" {
#include "peripheral/devices/hal_factory.h"
#include "common/hal_factory_format.h"
}

#include "common/hal_log.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>

#include <fcntl.h>
#include <unistd.h> /* pread/pwrite/usleep */

namespace {

/* Linux backend default device node (at24 sysfs). */
constexpr const char *kDefaultDevicePath = "/sys/bus/i2c/devices/1-0050/eeprom";

struct FactoryCtx {
    std::mutex mtx;
    HalFactoryIo io; /* format-engine I/O view */
    int fd{-1};      /* valid only for the Linux backend */
#if defined(HAL_HAVE_HAILORT)
    std::unique_ptr<hailort::Device> dev; /* valid only for the I2C backend */
    hailo_i2c_slave_config_t cfg;
#endif
};

/* ---- Linux backend I/O ---- */
static int linux_io_read(void *user, uint32_t addr, uint8_t *buf, uint32_t len)
{
    auto *ctx = static_cast<FactoryCtx *>(user);
    const ssize_t n = pread(ctx->fd, buf, len, addr);
    if (n < 0) {
        HAL_LOG_ERROR("factory: pread @0x%x len=%u failed (errno=%d)", addr, len, errno);
        return HAL_ERROR;
    }
    if (static_cast<uint32_t>(n) != len) {
        HAL_LOG_ERROR("factory: pread short read @0x%x (%zd/%u)", addr, n, len);
        return HAL_ERROR;
    }
    return HAL_OK;
}

static int linux_io_write(void *user, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    auto *ctx = static_cast<FactoryCtx *>(user);
    const ssize_t n = pwrite(ctx->fd, buf, len, addr);
    if (n < 0) {
        HAL_LOG_ERROR("factory: pwrite @0x%x len=%u failed (errno=%d)", addr, len, errno);
        return HAL_ERROR;
    }
    if (static_cast<uint32_t>(n) != len) {
        HAL_LOG_ERROR("factory: pwrite short write @0x%x (%zd/%u)", addr, n, len);
        return HAL_ERROR;
    }
    return HAL_OK;
}

#if defined(HAL_HAVE_HAILORT)
/* ---- I2C backend I/O (page-chunked writes) ---- */
constexpr uint16_t kDefaultSlaveAddress = 0x50;
constexpr uint8_t  kDefaultRegAddrSize   = 1;
constexpr uint8_t  kDefaultBusIndex      = 1;
constexpr size_t   kEepromPageSize       = 8; /* AT24C02 8-byte page */
constexpr useconds_t kWriteCycleUs       = 5000;

static int i2c_read_raw(FactoryCtx *ctx, uint32_t reg, uint8_t *buf, size_t len)
{
    hailort::MemoryView view(buf, len);
    const hailo_status st = ctx->dev->i2c_read(ctx->cfg, reg, view);
    if (st != HAILO_SUCCESS) {
        HAL_LOG_ERROR("factory: i2c_read @0x%x len=%zu failed (status=%d)",
                      reg, len, static_cast<int>(st));
        return HAL_ERROR;
    }
    return HAL_OK;
}

static int i2c_write_raw(FactoryCtx *ctx, uint32_t reg, const uint8_t *buf, size_t len)
{
    const hailort::MemoryView view = hailort::MemoryView::create_const(buf, len);
    const hailo_status st = ctx->dev->i2c_write(ctx->cfg, reg, view);
    if (st != HAILO_SUCCESS) {
        HAL_LOG_ERROR("factory: i2c_write @0x%x len=%zu failed (status=%d)",
                      reg, len, static_cast<int>(st));
        return HAL_ERROR;
    }
    return HAL_OK;
}

static int i2c_io_read(void *user, uint32_t addr, uint8_t *buf, uint32_t len)
{
    auto *ctx = static_cast<FactoryCtx *>(user);
    return i2c_read_raw(ctx, addr, buf, len);
}

static int i2c_io_write(void *user, uint32_t addr, const uint8_t *buf, uint32_t len)
{
    auto *ctx = static_cast<FactoryCtx *>(user);
    /* Page-chunk the write (AT24C02 8-byte page). Slot bases 0/128 are
       page-aligned so chunk boundaries stay within a physical page. */
    for (size_t off = 0; off < len; off += kEepromPageSize) {
        const size_t n = (len - off) < kEepromPageSize ? (len - off) : kEepromPageSize;
        const int rc = i2c_write_raw(ctx, static_cast<uint32_t>(addr + off), buf + off, n);
        if (rc != HAL_OK) {
            return rc;
        }
        usleep(kWriteCycleUs);
    }
    return HAL_OK;
}
#endif /* HAL_HAVE_HAILORT */

/* ---- OPS entry points ---- */
static int factory_init(const HalFactoryConfig *config, void **out)
{
    if (out == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    *out = nullptr;

    HalFactoryBackend backend = HAL_FACTORY_BACKEND_LINUX;
    HalFactoryStorage custom{};
    const char *device_path = kDefaultDevicePath;
    if (config != nullptr) {
        backend = config->backend;
        custom = config->storage;
        if (config->device_path != nullptr && config->device_path[0] != '\0') {
            device_path = config->device_path;
        }
    }

    auto *ctx = new (std::nothrow) FactoryCtx();
    if (ctx == nullptr) {
        return HAL_ERR_NO_MEM;
    }

    if (backend == HAL_FACTORY_BACKEND_CUSTOM) {
        if (custom.read == nullptr || custom.write == nullptr) {
            delete ctx;
            return HAL_ERR_INVALID_ARG;
        }
        ctx->io = hal_factory_io_from_storage(&custom);
    } else if (backend == HAL_FACTORY_BACKEND_LINUX) {
        ctx->fd = open(device_path, O_RDWR);
        if (ctx->fd < 0) {
            HAL_LOG_ERROR("factory: open %s failed (errno=%d)", device_path, errno);
            delete ctx;
            return HAL_ERR_NOT_READY;
        }
        ctx->io.read = linux_io_read;
        ctx->io.write = linux_io_write;
        ctx->io.user = ctx;
    } else { /* HAL_FACTORY_BACKEND_I2C */
#if defined(HAL_HAVE_HAILORT)
        auto dev = hailort::Device::create();
        if (!dev) {
            HAL_LOG_ERROR("factory: HailoRT Device::create failed (status=%d)",
                          static_cast<int>(dev.status()));
            delete ctx;
            return HAL_ERR_NOT_READY;
        }
        ctx->dev = std::move(dev.value());

        ctx->cfg.endianness = HAILO_LITTLE_ENDIAN;
        ctx->cfg.slave_address = kDefaultSlaveAddress;
        ctx->cfg.register_address_size = kDefaultRegAddrSize;
        ctx->cfg.bus_index = kDefaultBusIndex;
        ctx->cfg.should_hold_bus = false;
        if (config != nullptr) {
            ctx->cfg.slave_address = config->slave_address;
            ctx->cfg.register_address_size = config->register_address_size;
            ctx->cfg.bus_index = config->bus_index;
            ctx->cfg.endianness = (config->endianness == HAL_FACTORY_BIG_ENDIAN)
                                      ? HAILO_BIG_ENDIAN
                                      : HAILO_LITTLE_ENDIAN;
        }

        ctx->io.read = i2c_io_read;
        ctx->io.write = i2c_io_write;
        ctx->io.user = ctx;
#else
        HAL_LOG_ERROR("factory: HailoRT I2C backend requested but HAL built without hailort");
        delete ctx;
        return HAL_ERR_NOT_SUPPORTED;
#endif
    }

    *out = ctx;
    return HAL_OK;
}

static int factory_deinit(void *factory_ctx)
{
    auto *ctx = static_cast<FactoryCtx *>(factory_ctx);
    if (ctx == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
    delete ctx;
    return HAL_OK;
}

static int factory_read_all(void *factory_ctx, HalFactoryInfo *out)
{
    if (factory_ctx == nullptr || out == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    auto *ctx = static_cast<FactoryCtx *>(factory_ctx);
    std::lock_guard<std::mutex> lock(ctx->mtx);
    return hal_factory_format_read_all(&ctx->io, out);
}

static int factory_get(void *factory_ctx, HalFactoryField field, char *value, uint32_t value_size)
{
    if (factory_ctx == nullptr || value == nullptr || value_size == 0) {
        return HAL_ERR_INVALID_ARG;
    }
    auto *ctx = static_cast<FactoryCtx *>(factory_ctx);
    std::lock_guard<std::mutex> lock(ctx->mtx);
    return hal_factory_format_get(&ctx->io, field, value, value_size);
}

static int factory_set(void *factory_ctx, HalFactoryField field, const char *value)
{
    if (factory_ctx == nullptr || value == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    auto *ctx = static_cast<FactoryCtx *>(factory_ctx);
    std::lock_guard<std::mutex> lock(ctx->mtx);
    return hal_factory_format_set(&ctx->io, field, value);
}

static int factory_erase(void *factory_ctx)
{
    if (factory_ctx == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    auto *ctx = static_cast<FactoryCtx *>(factory_ctx);
    std::lock_guard<std::mutex> lock(ctx->mtx);
    return hal_factory_format_erase(&ctx->io);
}

static const char *factory_get_version(void)
{
    return "Hailo15 HAL-FACTORY 2.0.0";
}

} // namespace

extern "C" {
HalFactoryOps HAL_FACTORY_OPS = {
    .init = factory_init,
    .deinit = factory_deinit,
    .read_all = factory_read_all,
    .get = factory_get,
    .set = factory_set,
    .erase = factory_erase,
    .get_version = factory_get_version,
};
}
