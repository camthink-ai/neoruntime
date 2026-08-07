/**
 * @file hailo15_ota_impl.cpp
 * @brief hailo15 MCU OTA — matches ne503 MCU app: OTA_ENTER_BOOT + Ymodem in bootloader.
 */

#include "../mcu/hailo15_mcu_priv.hpp"

#include "common/hal_log.h"

extern "C" {
#include "peripheral/devices/hal_env_ctrl.h"
#include "peripheral/devices/hal_ota.h"
#include "peripheral/hal_mcu.h"
}

#include "common/host_link/host_link_proto.h"

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <time.h>
#include <unistd.h>

#include <inttypes.h>

namespace {

enum : uint8_t {
    YM_SOH = 0x01,
    YM_EOT = 0x04,
    YM_ACK = 0x06,
    YM_NAK = 0x15,
    YM_C = 0x43,
    YM_SUB = 0x1a,
};

static uint64_t mono_ms(void)
{
    struct timespec ts {};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static uint16_t crc16_ccitt(const uint8_t *buf, size_t l)
{
    uint16_t crc = 0u;
    for (size_t i = 0; i < l; i++) {
        crc ^= (uint16_t)buf[i] << 8u;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1u;
            }
        }
    }
    return crc;
}

/**
 * If the MCU is already in the bootloader spamming Ymodem-CRC ('C'), host_link OTA_ENTER_BOOT
 * will never get a response — the example retry would appear "stuck" on the second attempt.
 * Probe for 'C' with RX suspended before issuing host_link.
 */
static int peek_bootloader_offers_crc(void *mcu_ctx, void *user)
{
    (void)user;
    const uint64_t t0 = mono_ms();
    while ((mono_ms() - t0) < 450u) {
        uint8_t b = 0;
        const ssize_t n = hailo15_mcu_raw_read(mcu_ctx, &b, 1u, 50u);
        if (n < 0) {
            return HAL_ERROR;
        }
        if (n == 1 && b == YM_C) {
            return HAL_OK;
        }
    }
    return HAL_ERR_TIMEOUT;
}

static int wait_crc_mode(void *mcu_ctx, uint32_t total_timeout_ms)
{
    const uint64_t t0 = mono_ms();
    while ((mono_ms() - t0) < (uint64_t)total_timeout_ms) {
        const uint64_t left = (uint64_t)total_timeout_ms - (mono_ms() - t0);
        uint32_t slice = 200u;
        if (left < (uint64_t)slice) {
            slice = (uint32_t)left;
        }
        if (slice == 0u) {
            slice = 1u;
        }
        uint8_t b = 0;
        const ssize_t n = hailo15_mcu_raw_read(mcu_ctx, &b, 1u, slice);
        if (n < 0) {
            return HAL_ERROR;
        }
        if (n == 1 && b == YM_C) {
            return HAL_OK;
        }
    }
    return HAL_ERR_TIMEOUT;
}

static int expect_byte(void *mcu_ctx, uint8_t want, uint32_t timeout_ms)
{
    const uint64_t t0 = mono_ms();
    while ((mono_ms() - t0) < (uint64_t)timeout_ms) {
        uint8_t b = 0;
        const ssize_t n = hailo15_mcu_raw_read(mcu_ctx, &b, 1u, 100u);
        if (n < 0) {
            return HAL_ERROR;
        }
        if (n == 1 && b == want) {
            return HAL_OK;
        }
    }
    return HAL_ERR_TIMEOUT;
}

static int send_block(void *mcu_ctx, uint8_t seq, const uint8_t *data128)
{
    uint8_t frame[3u + 128u + 2u];
    frame[0] = YM_SOH;
    frame[1] = seq;
    frame[2] = static_cast<uint8_t>(255u - seq);
    std::memcpy(frame + 3u, data128, 128u);
    const uint16_t crc = crc16_ccitt(data128, 128u);
    frame[131] = static_cast<uint8_t>((crc >> 8) & 0xffu);
    frame[132] = static_cast<uint8_t>(crc & 0xffu);
    return hailo15_mcu_raw_write_all(mcu_ctx, frame, sizeof(frame));
}

struct YmodemUser {
    const std::vector<uint8_t> *file{};
    const char *firmware_path{};
    HalOtaProgressCb cb{};
    void *progress_user{};
    uint32_t post_delay_ms{800u};
    uint32_t crc_wait_ms{30000u};
};

static int ymodem_send_cb(void *mcu_ctx, void *user)
{
    auto *u = static_cast<YmodemUser *>(user);
    if (u == nullptr || u->file == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    const std::vector<uint8_t> &file = *u->file;
    if (file.size() < sizeof(host_link_ota_pkg_hdr_t)) {
        return HAL_ERR_INVALID_SIZE;
    }

    if (u->post_delay_ms > 0u) {
        usleep(u->post_delay_ms * 1000u);
    }

    int rc = wait_crc_mode(mcu_ctx, u->crc_wait_ms);
    if (rc != HAL_OK) {
        return rc;
    }

    uint8_t b0[128];
    std::memset(b0, YM_SUB, sizeof(b0));
    const char *path = (u->firmware_path != nullptr) ? u->firmware_path : "ota.bin";
    const char *bn = std::strrchr(path, '/');
    bn = (bn != nullptr) ? (bn + 1) : path;
    (void)std::snprintf(reinterpret_cast<char *>(b0), sizeof(b0), "%s", bn);
    const size_t name_end = std::strlen(reinterpret_cast<char *>(b0)) + 1u;
    if (name_end < sizeof(b0)) {
        (void)std::snprintf(reinterpret_cast<char *>(b0) + name_end, sizeof(b0) - name_end, "%zu", file.size());
    }

    rc = send_block(mcu_ctx, 0u, b0);
    if (rc != HAL_OK) {
        return rc;
    }
    rc = expect_byte(mcu_ctx, YM_ACK, 5000u);
    if (rc != HAL_OK) {
        return rc;
    }

    uint8_t seq = 1u;
    size_t off = 0;
    const size_t total = file.size();
    while (off < total) {
        uint8_t blk[128];
        std::memset(blk, YM_SUB, sizeof(blk));
        const size_t chunk = (total - off) > 128u ? 128u : (total - off);
        std::memcpy(blk, file.data() + off, chunk);
        rc = send_block(mcu_ctx, seq, blk);
        if (rc != HAL_OK) {
            return rc;
        }
        rc = expect_byte(mcu_ctx, YM_ACK, 8000u);
        if (rc != HAL_OK) {
            return rc;
        }
        off += chunk;
        seq++;
        if (u->cb != nullptr) {
            u->cb(u->progress_user, static_cast<uint32_t>(off), static_cast<uint32_t>(total));
        }
    }

    const uint8_t eot = YM_EOT;
    rc = hailo15_mcu_raw_write_all(mcu_ctx, &eot, 1u);
    if (rc != HAL_OK) {
        return rc;
    }
    rc = expect_byte(mcu_ctx, YM_NAK, 5000u);
    if (rc != HAL_OK) {
        return rc;
    }
    rc = hailo15_mcu_raw_write_all(mcu_ctx, &eot, 1u);
    if (rc != HAL_OK) {
        return rc;
    }
    rc = expect_byte(mcu_ctx, YM_ACK, 5000u);
    if (rc != HAL_OK) {
        return rc;
    }

    rc = wait_crc_mode(mcu_ctx, 10000u);
    if (rc != HAL_OK) {
        return rc;
    }

    uint8_t closing[128];
    std::memset(closing, 0, sizeof(closing));
    rc = send_block(mcu_ctx, 0u, closing);
    if (rc != HAL_OK) {
        return rc;
    }
    rc = expect_byte(mcu_ctx, YM_ACK, 5000u);
    if (rc != HAL_OK) {
        return rc;
    }

    return HAL_OK;
}

} // namespace

static int ota_enter_boot_once(void *mcu_ctx)
{
    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = hailo15_mcu_raw_request_timeout(mcu_ctx, HOST_LINK_CMD_OTA_ENTER_BOOT, nullptr, 0u,
                                              reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len, 5000u);
    if (ret != HAL_OK) {
        return ret;
    }
    if (resp_len != sizeof(st)) {
        return HAL_ERR_INVALID_SIZE;
    }
    return hailo15_mcu_map_status(st.status);
}

static int hailo15_ota_install_from_file(void *mcu_ctx, const char *firmware_path, const HalOtaInstallOptions *opt)
{
    if (mcu_ctx == nullptr || firmware_path == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }

    HalOtaInstallOptions local{};
    if (opt != nullptr) {
        local = *opt;
    } else {
        local.post_enter_boot_delay_ms = 800u;
        local.ymodem_crc_char_timeout_ms = 30000u;
    }
    if (local.post_enter_boot_delay_ms == 0u) {
        local.post_enter_boot_delay_ms = 800u;
    }
    if (local.ymodem_crc_char_timeout_ms == 0u) {
        local.ymodem_crc_char_timeout_ms = 30000u;
    }
    if (local.force_reboot_settle_ms == 0u) {
        local.force_reboot_settle_ms = 2000u;
    }

    std::ifstream f(firmware_path, std::ios::binary);
    if (!f.is_open()) {
        return HAL_ERR_NOT_FOUND;
    }
    f.seekg(0, std::ios::end);
    const auto sz = static_cast<size_t>(f.tellg());
    f.seekg(0, std::ios::beg);
    if (sz < sizeof(host_link_ota_pkg_hdr_t)) {
        return HAL_ERR_INVALID_SIZE;
    }
    std::vector<uint8_t> buf(sz);
    f.read(reinterpret_cast<char *>(buf.data()), static_cast<std::streamsize>(sz));
    if (static_cast<size_t>(f.gcount()) != sz) {
        return HAL_ERR_RESULT;
    }

    host_link_ota_pkg_hdr_t hdr{};
    std::memcpy(&hdr, buf.data(), sizeof(hdr));
    if (hdr.magic != HOST_LINK_OTA_PACKAGE_MAGIC) {
        return HAL_ERR_INVALID_FMT;
    }

    const int peek_rc = hailo15_mcu_with_rx_suspended(mcu_ctx, peek_bootloader_offers_crc, nullptr);

    int ret = HAL_ERROR;
    if (peek_rc == HAL_OK) {
        HAL_LOG_INFO("Bootloader Ymodem CRC mode active ('C' seen); skipping OTA_ENTER_BOOT");
        ret = HAL_OK;
    } else {
        ret = ota_enter_boot_once(mcu_ctx);
    }
    if (ret != HAL_OK && local.force_reboot_on_enter_boot_fail) {
        HAL_LOG_WARNING("OTA_ENTER_BOOT failed rc=%d; retry after MCU reset (REBOOT cmd, then host NRST GPIO, else RESET_SOC); settle %" PRIu32 " ms)",
                     ret, local.force_reboot_settle_ms);
        int r_force = HAL_ENV_CTRL_OPS.reset_mcu(mcu_ctx, false);
        if (r_force != HAL_OK) {
            r_force = HAL_ENV_CTRL_OPS.reset_mcu(mcu_ctx, true);
        }
        if (r_force != HAL_OK) {
            (void)HAL_ENV_CTRL_OPS.reset_soc(mcu_ctx);
        }
        usleep(local.force_reboot_settle_ms * 1000u);
        ret = ota_enter_boot_once(mcu_ctx);
    }
    if (ret != HAL_OK) {
        return ret;
    }

    YmodemUser yu{};
    yu.file = &buf;
    yu.firmware_path = firmware_path;
    yu.cb = local.progress_cb;
    yu.progress_user = local.progress_user;
    yu.post_delay_ms = local.post_enter_boot_delay_ms;
    yu.crc_wait_ms = local.ymodem_crc_char_timeout_ms;

    return hailo15_mcu_serial_exclusive(mcu_ctx, ymodem_send_cb, &yu);
}

static int hailo15_ota_get_download_status(void *mcu_ctx, HalOtaDownloadStatus *out)
{
    (void)mcu_ctx;
    if (out != nullptr) {
        out->status = 0;
        out->downloaded = 0;
        out->total = 0;
        out->app_size = 0;
        out->app_crc32 = 0;
        out->in_session = false;
    }
    return HAL_ERR_NOT_SUPPORTED;
}

static int hailo15_ota_abort(void *mcu_ctx)
{
    (void)mcu_ctx;
    return HAL_ERR_NOT_SUPPORTED;
}

extern "C" {
HalOtaOps HAL_OTA_OPS = {
    .install_from_file = hailo15_ota_install_from_file,
    .get_download_status = hailo15_ota_get_download_status,
    .abort = hailo15_ota_abort,
};
}
