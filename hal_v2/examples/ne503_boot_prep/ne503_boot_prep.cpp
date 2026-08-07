/**
 * @file ne503_boot_prep.cpp
 * @brief NE503 boot prep: UTC RTC sync with MCU, optional version check + OTA (Ymodem).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "common/hal_log.h"

extern "C" {
#include "common/host_link/host_link_proto.h"
#include "peripheral/hal_mcu.h"
#include "peripheral/devices/hal_env_ctrl.h"
#include "peripheral/devices/hal_ota.h"
#include "peripheral/devices/hal_rtc.h"
}

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <time.h>
#include <unistd.h>

#include <string>

namespace {

enum class RtcMode {
    Unset = -1,
    None = 0,
    SyncFromMcu,
    PushToMcu,
};

enum class VersionCheck {
    Unset = -1,
    Off = 0,
    On,
};

/** Default: OTA when MCU version does not match target (see ExpectVer). */
enum class UpgradeIf {
    Mismatch,
    /** OTA only when MCU quad is strictly older than --expect-version (x.y.z => x.y.z.0). */
    OlderOnly,
};

struct ExpectVer {
    int32_t maj{};
    int32_t min{};
    int32_t pat{};
    int32_t bld{};
    /** True if user wrote four components; false for x.y.z only (build compared as 0 for ordering). */
    bool has_build{};
};

static const char *arg_value(int argc, char **argv, const char *key)
{
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], key) == 0) {
            return argv[i + 1];
        }
    }
    return nullptr;
}

static bool parse_u32(const char *s, uint32_t *out)
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

static bool parse_expect_version(const char *s, ExpectVer *out)
{
    if (!s || !out) {
        return false;
    }
    int a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(s, "%d.%d.%d.%d", &a, &b, &c, &d) == 4) {
        out->maj = a;
        out->min = b;
        out->pat = c;
        out->bld = d;
        out->has_build = true;
        return true;
    }
    if (std::sscanf(s, "%d.%d.%d", &a, &b, &c) == 3) {
        out->maj = a;
        out->min = b;
        out->pat = c;
        out->bld = 0;
        out->has_build = false;
        return true;
    }
    return false;
}

/** Lexicographic compare of version quads (major..build). */
static int quad_cmp(int32_t ma, int32_t mi, int32_t pa, int32_t bi, int32_t mb, int32_t mn, int32_t pb, int32_t bb)
{
    if (ma != mb) {
        return (ma < mb) ? -1 : 1;
    }
    if (mi != mn) {
        return (mi < mn) ? -1 : 1;
    }
    if (pa != pb) {
        return (pa < pb) ? -1 : 1;
    }
    if (bi != bb) {
        return (bi < bb) ? -1 : 1;
    }
    return 0;
}

/** Skip OTA: MCU already at target (mismatch policy). Three-part expect matches maj.min.patch only. */
static bool mcu_at_target_skip_ota(const HalMcuVersion *v, const ExpectVer &e)
{
    if (!v) {
        return false;
    }
    if (e.has_build) {
        return v->major == e.maj && v->minor == e.min && v->patch == e.pat && v->build == e.bld;
    }
    return v->major == e.maj && v->minor == e.min && v->patch == e.pat;
}

/** Target quad for ordering: x.y.z => x.y.z.0 */
static void expect_target_quad(const ExpectVer &e, int32_t *ma, int32_t *mi, int32_t *pa, int32_t *bi)
{
    *ma = e.maj;
    *mi = e.min;
    *pa = e.pat;
    *bi = e.has_build ? e.bld : 0;
}

static bool parse_dotted_ints(const char *s, int *vals, int maxvals, int *out_count)
{
    *out_count = 0;
    if (!s || !*s) {
        return false;
    }
    const char *p = s;
    while (*p && *out_count < maxvals) {
        char *end = nullptr;
        long v = std::strtol(p, &end, 10);
        if (end == p) {
            return false;
        }
        if (v < (long)INT32_MIN || v > (long)INT32_MAX) {
            return false;
        }
        vals[*out_count] = (int)v;
        (*out_count)++;
        if (*end == '\0') {
            return true;
        }
        if (*end != '.') {
            return false;
        }
        p = end + 1;
    }
    return *p == '\0';
}

/** Compare dotted version strings by padding with zeros (0.1.2 == 0.1.2.0). */
static bool dotted_versions_equal(const char *a, const char *b)
{
    int va[8]{};
    int vb[8]{};
    int na = 0;
    int nb = 0;
    if (!parse_dotted_ints(a, va, 8, &na) || na < 1) {
        return false;
    }
    if (!parse_dotted_ints(b, vb, 8, &nb) || nb < 1) {
        return false;
    }
    const int n = std::max(na, nb);
    for (int i = 0; i < n; i++) {
        const int ca = i < na ? va[i] : 0;
        const int cb = i < nb ? vb[i] : 0;
        if (ca != cb) {
            return false;
        }
    }
    return true;
}

/** tm.tm_wday (Sun=0..Sat=6) -> MCU weekday 1..7 (Mon=1 .. Sun=7). */
static uint8_t tm_wday_to_mcu_weekday(int tm_wday)
{
    if (tm_wday < 0 || tm_wday > 6) {
        return 1;
    }
    return (tm_wday == 0) ? 7u : (uint8_t)tm_wday;
}

static int rtc_push_system_utc_to_mcu(void *mcu)
{
    const time_t now = time(nullptr);
    if (now == (time_t)-1) {
        HAL_LOG_ERROR("time() failed errno=%d", errno);
        return HAL_ERROR;
    }
    struct tm tm {};
    gmtime_r(&now, &tm);
    HalRtcTime ht{};
    ht.year = (uint8_t)((tm.tm_year + 1900) % 100);
    ht.month = (uint8_t)(tm.tm_mon + 1);
    ht.day = (uint8_t)tm.tm_mday;
    ht.weekday = tm_wday_to_mcu_weekday(tm.tm_wday);
    ht.hour = (uint8_t)tm.tm_hour;
    ht.minute = (uint8_t)tm.tm_min;
    ht.second = (uint8_t)tm.tm_sec;
    return HAL_RTC_OPS.set_time(mcu, &ht);
}

static int rtc_sync_linux_from_mcu(void *mcu)
{
    HalRtcTime ht{};
    const int r = HAL_RTC_OPS.get_time(mcu, &ht);
    if (r != HAL_OK) {
        return r;
    }
    struct tm tm {};
    std::memset(&tm, 0, sizeof(tm));
    tm.tm_year = ht.year + 100;
    tm.tm_mon = (int)ht.month - 1;
    tm.tm_mday = (int)ht.day;
    tm.tm_hour = (int)ht.hour;
    tm.tm_min = (int)ht.minute;
    tm.tm_sec = (int)ht.second;
    tm.tm_isdst = 0;
    time_t sec = timegm(&tm);
    if (sec == (time_t)-1) {
        HAL_LOG_ERROR("timegm failed");
        return HAL_ERROR;
    }
    struct timespec ts {};
    ts.tv_sec = sec;
    ts.tv_nsec = 0;
    if (clock_settime(CLOCK_REALTIME, &ts) != 0) {
        HAL_LOG_ERROR("clock_settime(CLOCK_REALTIME) failed errno=%d (need root/CAP_SYS_TIME)", errno);
        return HAL_ERROR;
    }
    return HAL_OK;
}

static void trim_cstr(char *s, size_t cap)
{
    if (!s || cap == 0) {
        return;
    }
    size_t n = strnlen(s, cap);
    while (n > 0 && std::isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
}

/**
 * @param skip_version_match  If true: only magic/size check (see --skip-package-version-check).
 *                            Else: hdr.app_version must match --expect-version (0.1.2 == 0.1.2.0).
 */
static int verify_firmware_package(const char *path, const char *expect_ver, bool skip_version_match)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        HAL_LOG_ERROR("open firmware failed: %s errno=%d", path, errno);
        return HAL_ERR_NOT_FOUND;
    }
    host_link_ota_pkg_hdr_t hdr{};
    const ssize_t n = read(fd, &hdr, sizeof(hdr));
    close(fd);
    if (n != (ssize_t)sizeof(hdr)) {
        HAL_LOG_ERROR("firmware read header failed");
        return HAL_ERR_RESULT;
    }
    if (hdr.magic != HOST_LINK_OTA_PACKAGE_MAGIC) {
        HAL_LOG_ERROR("firmware bad magic (not OTA package)");
        return HAL_ERR_INVALID_FMT;
    }
    if (skip_version_match) {
        return HAL_OK;
    }
    char ver_buf[HOST_LINK_OTA_PACKAGE_VERSION_MAX_LEN + 1]{};
    std::memcpy(ver_buf, hdr.app_version,
                std::min(sizeof(ver_buf) - 1, sizeof(hdr.app_version)));
    trim_cstr(ver_buf, sizeof(ver_buf));
    if (!dotted_versions_equal(ver_buf, expect_ver)) {
        HAL_LOG_ERROR("firmware app_version \"%s\" does not match --expect-version \"%s\"", ver_buf, expect_ver);
        return HAL_ERR_INVALID_ARG;
    }
    return HAL_OK;
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

static bool parse_upgrade_if(const char *s, UpgradeIf *out)
{
    if (!s || !out) {
        return false;
    }
    if (std::strcmp(s, "mismatch") == 0) {
        *out = UpgradeIf::Mismatch;
        return true;
    }
    if (std::strcmp(s, "older") == 0) {
        *out = UpgradeIf::OlderOnly;
        return true;
    }
    return false;
}

static void usage(const char *argv0)
{
    std::fprintf(stderr,
                 "Usage: %s --rtc <none|sync-from-mcu|push-to-mcu> --version-check <on|off>\n"
                 "       [--serial <path>]  default /dev/ttyS0\n"
                 "       [--baud <n>]       default 921600\n"
                 "       [--timeout-ms <n>] host_link default timeout (default 1500)\n"
                 "       [--mcu-reset-gpio <n>] [--mcu-reset-active-low <0|1>] [--mcu-reset-pulse-ms <ms>]\n"
                 "       default NRST gpio 18 (ne503)\n"
                 "When --version-check on (required):\n"
                 "       --firmware <ota_package.bin> --expect-version <x.y.z|x.y.z.w>\n"
                 "       [--upgrade-if mismatch|older]  default mismatch\n"
                 "           mismatch: OTA when MCU != target (x.y.z ignores build)\n"
                 "           older:    OTA only when MCU version < target (x.y.z => x.y.z.0)\n"
                 "       [--skip-package-version-check]  do not compare OTA header app_version to --expect-version\n"
                 "       [--skip-mcu-version-check]       do not call get_version; always OTA after firmware checks\n"
                 "           (--upgrade-if is ignored). If both skips are set, --expect-version is optional.\n"
                 "Optional OTA tuning:\n"
                 "       [--post-boot-delay-ms <ms>] [--settle-after-reset-ms <ms>]\n",
                 argv0);
}

static bool parse_rtc_mode(const char *s, RtcMode *out)
{
    if (!s || !out) {
        return false;
    }
    if (std::strcmp(s, "none") == 0) {
        *out = RtcMode::None;
        return true;
    }
    if (std::strcmp(s, "sync-from-mcu") == 0) {
        *out = RtcMode::SyncFromMcu;
        return true;
    }
    if (std::strcmp(s, "push-to-mcu") == 0) {
        *out = RtcMode::PushToMcu;
        return true;
    }
    return false;
}

static bool parse_version_check(const char *s, VersionCheck *out)
{
    if (!s || !out) {
        return false;
    }
    if (std::strcmp(s, "off") == 0) {
        *out = VersionCheck::Off;
        return true;
    }
    if (std::strcmp(s, "on") == 0) {
        *out = VersionCheck::On;
        return true;
    }
    return false;
}

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

static bool should_hw_reset_before_next_ota_attempt(int prev_rc)
{
    return prev_rc == HAL_ERR_TIMEOUT;
}

} // namespace

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
    }

    const char *serial = arg_value(argc, argv, "--serial");
    if (!serial) {
        serial = "/dev/ttyS0";
    }
    const char *baud_s = arg_value(argc, argv, "--baud");
    if (!baud_s) {
        baud_s = "921600";
    }
    const char *timeout_ms_s = arg_value(argc, argv, "--timeout-ms");
    uint32_t timeout_ms = 1500u;
    if (timeout_ms_s != nullptr && !parse_u32(timeout_ms_s, &timeout_ms)) {
        std::fprintf(stderr, "invalid --timeout-ms\n");
        return 1;
    }

    const char *rtc_s = arg_value(argc, argv, "--rtc");
    RtcMode rtc_mode = RtcMode::Unset;
    if (rtc_s != nullptr && !parse_rtc_mode(rtc_s, &rtc_mode)) {
        std::fprintf(stderr, "invalid --rtc (need none|sync-from-mcu|push-to-mcu)\n");
        return 1;
    }

    const char *vc_s = arg_value(argc, argv, "--version-check");
    VersionCheck vc = VersionCheck::Unset;
    if (vc_s != nullptr && !parse_version_check(vc_s, &vc)) {
        std::fprintf(stderr, "invalid --version-check (need on|off)\n");
        return 1;
    }

    if (rtc_mode == RtcMode::Unset || vc == VersionCheck::Unset) {
        std::fprintf(stderr, "missing required --rtc and/or --version-check\n");
        usage(argv[0]);
        return 1;
    }

    const char *firmware_path = arg_value(argc, argv, "--firmware");
    const char *expect_ver_s = arg_value(argc, argv, "--expect-version");
    const char *upgrade_if_s = arg_value(argc, argv, "--upgrade-if");
    ExpectVer exp{};

    UpgradeIf upgrade_if = UpgradeIf::Mismatch;
    if (upgrade_if_s != nullptr && !parse_upgrade_if(upgrade_if_s, &upgrade_if)) {
        std::fprintf(stderr, "invalid --upgrade-if (use mismatch|older)\n");
        return 1;
    }

    const bool skip_package_version_check = has_flag(argc, argv, "--skip-package-version-check");
    const bool skip_mcu_version_check = has_flag(argc, argv, "--skip-mcu-version-check");

    if (vc == VersionCheck::On) {
        if (!firmware_path) {
            std::fprintf(stderr, "--version-check on requires --firmware\n");
            return 1;
        }
        const bool expect_optional = skip_package_version_check && skip_mcu_version_check;
        if (!expect_optional && !expect_ver_s) {
            std::fprintf(stderr,
                         "--version-check on requires --expect-version (unless both "
                         "--skip-package-version-check and --skip-mcu-version-check)\n");
            return 1;
        }
        if (expect_ver_s != nullptr && !parse_expect_version(expect_ver_s, &exp)) {
            std::fprintf(stderr, "--expect-version must be x.y.z or x.y.z.w\n");
            return 1;
        }
    }

    if (skip_mcu_version_check && upgrade_if_s != nullptr) {
        std::fprintf(stderr,
                     "warning: --upgrade-if is ignored when --skip-mcu-version-check is set\n");
    }

    const char *mr_gpio_s = arg_value(argc, argv, "--mcu-reset-gpio");
    const char *mr_active_low_s = arg_value(argc, argv, "--mcu-reset-active-low");
    const char *mr_pulse_ms_s = arg_value(argc, argv, "--mcu-reset-pulse-ms");
    const char *post_boot_ms_s = arg_value(argc, argv, "--post-boot-delay-ms");
    const char *settle_ms_s = arg_value(argc, argv, "--settle-after-reset-ms");

    uint32_t baud = 0;
    if (!parse_u32(baud_s, &baud) || baud == 0u) {
        std::fprintf(stderr, "invalid --baud\n");
        return 1;
    }

    HalMcuConfig cfg{};
    cfg.serial_device = serial;
    cfg.baud_rate = baud;
    cfg.timeout_ms = timeout_ms;
    cfg.priv = nullptr;
    cfg.host_mcu_reset_gpio = 18u;
    cfg.host_mcu_reset_active_low = 0u;
    cfg.host_mcu_reset_pulse_ms = 200u;

    if (mr_gpio_s != nullptr) {
        uint32_t g = 0;
        if (!parse_u32(mr_gpio_s, &g) || g == 0u) {
            std::fprintf(stderr, "invalid --mcu-reset-gpio\n");
            return 1;
        }
        cfg.host_mcu_reset_gpio = g;
    }
    if (mr_active_low_s != nullptr) {
        uint32_t al = 0;
        if (!parse_u32(mr_active_low_s, &al) || al > 1u) {
            std::fprintf(stderr, "invalid --mcu-reset-active-low\n");
            return 1;
        }
        cfg.host_mcu_reset_active_low = (uint8_t)al;
    }
    if (mr_pulse_ms_s != nullptr) {
        uint32_t pm = 0;
        if (!parse_u32(mr_pulse_ms_s, &pm)) {
            std::fprintf(stderr, "invalid --mcu-reset-pulse-ms\n");
            return 1;
        }
        cfg.host_mcu_reset_pulse_ms = (uint8_t)((pm > 255u) ? 255u : pm);
    }

    void *mcu = nullptr;
    int rc = HAL_MCU_OPS.init(&cfg, &mcu);
    if (rc != HAL_OK) {
        HAL_LOG_ERROR("HAL_MCU_OPS.init failed rc=%d (%s)", rc, hal_error_to_string((HalErrorCode)rc));
        return 2;
    }

    if (rtc_mode == RtcMode::SyncFromMcu) {
        rc = rtc_sync_linux_from_mcu(mcu);
        HAL_LOG_INFO("rtc sync-from-mcu rc=%d (%s)", rc, hal_error_to_string((HalErrorCode)rc));
        if (rc != HAL_OK) {
            (void)HAL_MCU_OPS.deinit(mcu);
            return 3;
        }
    } else if (rtc_mode == RtcMode::PushToMcu) {
        rc = rtc_push_system_utc_to_mcu(mcu);
        HAL_LOG_INFO("rtc push-to-mcu rc=%d (%s)", rc, hal_error_to_string((HalErrorCode)rc));
        if (rc != HAL_OK) {
            (void)HAL_MCU_OPS.deinit(mcu);
            return 3;
        }
    }

    if (vc == VersionCheck::Off) {
        HAL_LOG_INFO("version-check off; skipping OTA");
        (void)HAL_MCU_OPS.deinit(mcu);
        return 0;
    }

    rc = verify_firmware_package(firmware_path, expect_ver_s, skip_package_version_check);
    if (rc != HAL_OK) {
        HAL_LOG_ERROR("firmware header check failed rc=%d (%s)", rc, hal_error_to_string((HalErrorCode)rc));
        (void)HAL_MCU_OPS.deinit(mcu);
        return 4;
    }

    if (skip_mcu_version_check) {
        HAL_LOG_INFO("skip MCU version check (--skip-mcu-version-check); proceeding to OTA");
    } else {
        HalMcuVersion ver{};
        rc = HAL_MCU_OPS.get_version(mcu, &ver);
        if (rc == HAL_OK) {
            if (upgrade_if == UpgradeIf::OlderOnly) {
                int32_t tma = 0;
                int32_t tmi = 0;
                int32_t tpa = 0;
                int32_t tbi = 0;
                expect_target_quad(exp, &tma, &tmi, &tpa, &tbi);
                const int cmp =
                    quad_cmp(ver.major, ver.minor, ver.patch, ver.build, tma, tmi, tpa, tbi);
                if (cmp >= 0) {
                    HAL_LOG_INFO("MCU version %s already at or above target %s; skipping OTA", ver.version_str,
                                 expect_ver_s);
                    (void)HAL_MCU_OPS.deinit(mcu);
                    return 0;
                }
                HAL_LOG_INFO("MCU version %s older than target %s; starting OTA", ver.version_str, expect_ver_s);
            } else {
                if (mcu_at_target_skip_ota(&ver, exp)) {
                    HAL_LOG_INFO("MCU version %s matches target %s; skipping OTA", ver.version_str, expect_ver_s);
                    (void)HAL_MCU_OPS.deinit(mcu);
                    return 0;
                }
                HAL_LOG_INFO("MCU version %s != target %s; starting OTA", ver.version_str, expect_ver_s);
            }
        } else {
            HAL_LOG_WARNING("get_version failed rc=%d (%s); will OTA", rc, hal_error_to_string((HalErrorCode)rc));
        }
    }

    HalOtaInstallOptions opt{};
    opt.progress_cb = progress_cb;
    opt.progress_user = nullptr;
    opt.force_reboot_on_enter_boot_fail = true;
    opt.post_enter_boot_delay_ms = 800u;
    opt.ymodem_crc_char_timeout_ms = 5000u;
    opt.force_reboot_settle_ms = 2000u;

    if (post_boot_ms_s != nullptr) {
        uint32_t v = 0;
        if (!parse_u32(post_boot_ms_s, &v)) {
            std::fprintf(stderr, "invalid --post-boot-delay-ms\n");
            (void)HAL_MCU_OPS.deinit(mcu);
            return 1;
        }
        opt.post_enter_boot_delay_ms = v;
    }
    if (settle_ms_s != nullptr) {
        uint32_t v = 0;
        if (!parse_u32(settle_ms_s, &v)) {
            std::fprintf(stderr, "invalid --settle-after-reset-ms\n");
            (void)HAL_MCU_OPS.deinit(mcu);
            return 1;
        }
        opt.force_reboot_settle_ms = v;
    }

    const uint32_t settle_after_hw_ms =
        (opt.force_reboot_settle_ms != 0u) ? opt.force_reboot_settle_ms : 2000u;

    constexpr uint32_t kMaxOtaAttempts = 3u;
    int last_rc = HAL_ERROR;

    for (uint32_t attempt = 1u; attempt <= kMaxOtaAttempts; ++attempt) {
        HAL_LOG_INFO("OTA attempt %u/%u firmware=%s", static_cast<unsigned>(attempt),
                     static_cast<unsigned>(kMaxOtaAttempts), firmware_path);
        last_rc = HAL_OTA_OPS.install_from_file(mcu, firmware_path, &opt);
        if (last_rc == HAL_OK) {
            break;
        }
        HAL_LOG_WARNING("OTA failed rc=%d (%s)", last_rc, hal_error_to_string((HalErrorCode)last_rc));
        if (attempt >= kMaxOtaAttempts) {
            break;
        }
        if (should_hw_reset_before_next_ota_attempt(last_rc)) {
            HAL_LOG_WARNING("timeout waiting for bootloader 'C'; pulsing MCU NRST (GPIO %" PRIu32 ") before retry",
                            cfg.host_mcu_reset_gpio);
            const int rr = HAL_ENV_CTRL_OPS.reset_mcu(mcu, true);
            if (rr != HAL_OK) {
                HAL_LOG_WARNING("reset_mcu(GPIO) rc=%d (%s)", rr, hal_error_to_string((HalErrorCode)rr));
            }
            if (settle_after_hw_ms > 0u) {
                usleep(settle_after_hw_ms * 1000u);
            }
        }
    }

    if (last_rc != HAL_OK) {
        HAL_LOG_ERROR(
            "OTA aborted after %u attempts (last rc=%d %s). If timeout: no bootloader 'C' within 5s per attempt.",
            static_cast<unsigned>(kMaxOtaAttempts), last_rc, hal_error_to_string((HalErrorCode)last_rc));
    } else {
        HAL_LOG_INFO("OTA finished successfully");
    }

    (void)HAL_MCU_OPS.deinit(mcu);
    return (last_rc == HAL_OK) ? 0 : 5;
}
