/**
 * @file ota_test_v2.cpp
 * @brief Example: MCU OTA via Hal_v2 (OTA_ENTER_BOOT + bootloader Ymodem).
 *
 * Usage:
 *   hal-ota-test-v2 --serial <device> --baud <baud> --firmware <ota_package.bin>
 *       [--post-boot-delay-ms <ms>] [--ymodem-wait-ms <ms>]
 *       [--ota-max-attempts <n>] [--ota-retry-delay-ms <ms>] [--retry-soft-reboot-between-attempts]
 *       [--force-reboot-on-fail] [--force-reboot-settle-ms <ms>]
 *       [--mcu-reset-gpio <n>] [--mcu-reset-active-low <0|1>] [--mcu-reset-pulse-ms <ms>]
 *
 * Notes:
 *   - --mcu-reset-* configures HalMcuConfig.host_mcu_reset_* (Linux pulses MCU NRST; same recovery intent as
 *     HOST_LINK_CMD_REBOOT, unrelated to HOST_LINK_CMD_RESET_SOC).
 *   - With --force-reboot-on-fail: OTA_ENTER_BOOT fail → reset_mcu(false) REBOOT, then reset_mcu(true) if needed,
 *     else reset_soc(); --mcu-reset-gpio only affects the reset_mcu(..., true) path.
 *   - When transfer stalls (HAL_ERR_TIMEOUT mid-Ymodem), MCU bootloader may need several seconds before it
 *     sends 'C' again; use --ota-max-attempts > 1 and --ota-retry-delay-ms (default >= 4000 ms) so each retry
 *     runs enter-boot + Ymodem again without reflashing MCU firmware.
 *   - ne503: gpiochip1 line 2 → --mcu-reset-gpio 18; omit --mcu-reset-active-low for high-active (default 0).
 *
 * Deprecated CLI (still accepted): --soc-reset-gpio, --soc-reset-active-low, --soc-reset-pulse-ms.
 *
 * Example:
 * @code
 *   hal-ota-test-v2 --serial /dev/ttySx --baud 115200 --firmware /path/to/ota_package.bin \
 *       --mcu-reset-gpio 18 --force-reboot-on-fail [--force-reboot-settle-ms 2000]
 * @endcode
 */

#include "common/hal_log.h"

extern "C" {
#include "peripheral/hal_mcu.h"
#include "peripheral/devices/hal_env_ctrl.h"
#include "peripheral/devices/hal_ota.h"
}

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <unistd.h>

namespace
{
static void progress_cb(void *user, uint32_t sent, uint32_t total)
{
    (void)user;
    if (total == 0u) {
        HAL_LOG_INFO("OTA progress: %u bytes", sent);
        return;
    }
    const uint32_t pct = (sent * 100u) / total;
    HAL_LOG_INFO("OTA progress: %u/%u (%u%%)", sent, total, pct);
}

static const char *arg_value(int argc, char **argv, const char *key)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], key) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

static bool parse_u32_opt(const char *s, uint32_t *out)
{
    if (!out || !s) {
        return false;
    }
    char *end = nullptr;
    unsigned long v = std::strtoul(s, &end, 0);
    if (end == s || *end != 0) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

static bool has_flag(int argc, char **argv, const char *flag)
{
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], flag) == 0) {
            return true;
        }
    }
    return false;
}

/** Prefer @a primary; if missing, fall back to @a deprecated and print a warning. */
static const char *arg_primary_or_deprecated(int argc, char **argv, const char *primary, const char *deprecated)
{
    const char *v = arg_value(argc, argv, primary);
    if (v != nullptr) {
        return v;
    }
    v = arg_value(argc, argv, deprecated);
    if (v != nullptr) {
        std::fprintf(stderr, "warning: %s is deprecated; use %s\n", deprecated, primary);
    }
    return v;
}
} // namespace

int main(int argc, char **argv)
{
    const char *serial_device = arg_value(argc, argv, "--serial");
    const char *baud_s = arg_value(argc, argv, "--baud");
    const char *firmware_path = arg_value(argc, argv, "--firmware");
    const char *post_boot_ms_s = arg_value(argc, argv, "--post-boot-delay-ms");
    const char *ymodem_wait_ms_s = arg_value(argc, argv, "--ymodem-wait-ms");
    const char *force_settle_ms_s = arg_value(argc, argv, "--force-reboot-settle-ms");
    const char *mr_gpio_s = arg_primary_or_deprecated(argc, argv, "--mcu-reset-gpio", "--soc-reset-gpio");
    const char *mr_active_low_s =
        arg_primary_or_deprecated(argc, argv, "--mcu-reset-active-low", "--soc-reset-active-low");
    const char *mr_pulse_ms_s =
        arg_primary_or_deprecated(argc, argv, "--mcu-reset-pulse-ms", "--soc-reset-pulse-ms");
    const bool force_reboot_on_fail = has_flag(argc, argv, "--force-reboot-on-fail");
    const bool retry_soft_reboot = has_flag(argc, argv, "--retry-soft-reboot-between-attempts");
    const char *ota_max_attempts_s = arg_value(argc, argv, "--ota-max-attempts");
    const char *ota_retry_delay_ms_s = arg_value(argc, argv, "--ota-retry-delay-ms");

    if (!serial_device || !baud_s || !firmware_path) {
        std::fprintf(stderr,
                     "Usage: %s --serial <device> --baud <baud> --firmware <ota_package.bin> "
                     "[--post-boot-delay-ms <ms>] [--ymodem-wait-ms <ms>] "
                     "[--ota-max-attempts <n>] [--ota-retry-delay-ms <ms>] [--retry-soft-reboot-between-attempts] "
                     "[--force-reboot-on-fail] [--force-reboot-settle-ms <ms>] "
                     "[--mcu-reset-gpio <n>] [--mcu-reset-active-low <0|1>] [--mcu-reset-pulse-ms <ms>]\n"
                     "  (ne503 MCU NRST: chip1 line 2 → 18; same role as HOST_LINK_CMD_REBOOT)\n",
                     argv[0]);
        return 1;
    }

    uint32_t max_attempts = 3u;
    if (ota_max_attempts_s != nullptr) {
        if (!parse_u32_opt(ota_max_attempts_s, &max_attempts) || max_attempts == 0u) {
            std::fprintf(stderr, "Invalid --ota-max-attempts (need integer >= 1)\n");
            return 1;
        }
    }
    uint32_t retry_delay_ms = 4000u;
    if (ota_retry_delay_ms_s != nullptr) {
        if (!parse_u32_opt(ota_retry_delay_ms_s, &retry_delay_ms)) {
            std::fprintf(stderr, "Invalid --ota-retry-delay-ms\n");
            return 1;
        }
    }

    const uint32_t baud = static_cast<uint32_t>(std::strtoul(baud_s, nullptr, 10));
    if (baud == 0u) {
        std::fprintf(stderr, "Invalid --baud value: %s\n", baud_s);
        return 1;
    }

    HalMcuConfig cfg{};
    cfg.serial_device = serial_device;
    cfg.baud_rate = baud;
    cfg.timeout_ms = 1000;
    cfg.priv = nullptr;
    if (mr_gpio_s != nullptr) {
        uint32_t g = 0;
        if (!parse_u32_opt(mr_gpio_s, &g) || g == 0u) {
            std::fprintf(stderr, "Invalid --mcu-reset-gpio\n");
            return 1;
        }
        cfg.host_mcu_reset_gpio = g;
        if (mr_active_low_s != nullptr) {
            uint32_t al = 0;
            if (!parse_u32_opt(mr_active_low_s, &al) || al > 1u) {
                std::fprintf(stderr, "Invalid --mcu-reset-active-low\n");
                return 1;
            }
            cfg.host_mcu_reset_active_low = (uint8_t)al;
        } else {
            cfg.host_mcu_reset_active_low = 0u;
        }
        if (mr_pulse_ms_s != nullptr) {
            uint32_t pm = 0;
            if (!parse_u32_opt(mr_pulse_ms_s, &pm)) {
                std::fprintf(stderr, "Invalid --mcu-reset-pulse-ms\n");
                return 1;
            }
            cfg.host_mcu_reset_pulse_ms = (uint8_t)((pm > 255u) ? 255u : pm);
        } else {
            cfg.host_mcu_reset_pulse_ms = 200u;
        }
    }

    void *mcu_ctx = nullptr;
    int rc = HAL_MCU_OPS.init(&cfg, &mcu_ctx);
    if (rc != HAL_OK) {
        HAL_LOG_ERROR("HAL_MCU_OPS.init failed rc=%d", rc);
        return 2;
    }

    HalOtaInstallOptions opt{};
    opt.progress_cb = &progress_cb;
    opt.progress_user = nullptr;
    opt.force_reboot_on_enter_boot_fail = force_reboot_on_fail;
    if (force_settle_ms_s != nullptr) {
        uint32_t v = 0;
        if (!parse_u32_opt(force_settle_ms_s, &v)) {
            std::fprintf(stderr, "Invalid --force-reboot-settle-ms\n");
            (void)HAL_MCU_OPS.deinit(mcu_ctx);
            return 1;
        }
        opt.force_reboot_settle_ms = v;
    }
    if (post_boot_ms_s != nullptr) {
        uint32_t v = 0;
        if (!parse_u32_opt(post_boot_ms_s, &v)) {
            std::fprintf(stderr, "Invalid --post-boot-delay-ms\n");
            (void)HAL_MCU_OPS.deinit(mcu_ctx);
            return 1;
        }
        opt.post_enter_boot_delay_ms = v;
    }
    if (ymodem_wait_ms_s != nullptr) {
        uint32_t v = 0;
        if (!parse_u32_opt(ymodem_wait_ms_s, &v)) {
            std::fprintf(stderr, "Invalid --ymodem-wait-ms\n");
            (void)HAL_MCU_OPS.deinit(mcu_ctx);
            return 1;
        }
        opt.ymodem_crc_char_timeout_ms = v;
    }

    const uint32_t settle_soft_ms =
        (opt.force_reboot_settle_ms != 0u) ? opt.force_reboot_settle_ms : 2000u;

    HAL_LOG_INFO("Starting OTA (enter boot + Ymodem): firmware=%s", firmware_path);
    rc = HAL_ERROR;
    for (uint32_t attempt = 1u; attempt <= max_attempts; ++attempt) {
        if (max_attempts > 1u) {
            HAL_LOG_INFO("OTA attempt %u/%u", static_cast<unsigned>(attempt), static_cast<unsigned>(max_attempts));
        }
        rc = HAL_OTA_OPS.install_from_file(mcu_ctx, firmware_path, &opt);
        if (rc == HAL_OK) {
            break;
        }
        if (attempt >= max_attempts) {
            break;
        }
        HAL_LOG_WARNING(
            "OTA failed rc=%d (%s); waiting %u ms before retry (bootloader may need idle time to send 'C' again)",
            rc, hal_error_to_string(static_cast<HalErrorCode>(rc)), static_cast<unsigned>(retry_delay_ms));
        if (retry_delay_ms > 0u) {
            usleep(static_cast<unsigned int>(retry_delay_ms) * 1000u);
        }
        if (retry_soft_reboot) {
            const int rr = HAL_ENV_CTRL_OPS.reset_mcu(mcu_ctx, false);
            if (rr != HAL_OK) {
                HAL_LOG_WARNING("HOST_LINK REBOOT between attempts rc=%d (%s)", rr,
                                hal_error_to_string(static_cast<HalErrorCode>(rr)));
            }
            if (settle_soft_ms > 0u) {
                usleep(static_cast<unsigned int>(settle_soft_ms) * 1000u);
            }
        }
    }

    HAL_LOG_INFO("OTA install finished rc=%d (%s)", rc, hal_error_to_string((HalErrorCode)rc));
    (void)HAL_MCU_OPS.deinit(mcu_ctx);
    return (rc == HAL_OK) ? 0 : 3;
}

