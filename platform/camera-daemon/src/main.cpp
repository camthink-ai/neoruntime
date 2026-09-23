/**
 * @file main.cpp
 * @brief AIPC Camera Daemon - Entry point
 *
 * Loads YAML configuration and starts CameraDaemon.
 * Handles SIGINT/SIGTERM for graceful shutdown.
 */

#include "../include/camera_daemon.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <getopt.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <dlfcn.h>

#include "peripheral/devices/hal_factory.h"

extern "C" {
    #include "hal_log.h"
    #include "crash_handler.h"
    #include "peripheral/devices/hal_lens.h"
}

static CameraDaemon* g_daemon = nullptr;

// Send READY=1 without taking a build-time dependency on libsystemd.  This is
// the sd_notify wire protocol: one datagram to the Unix socket named by
// NOTIFY_SOCKET.  Abstract sockets are represented by a leading '@'.
static void notify_systemd_ready() {
    const char* notify_socket = std::getenv("NOTIFY_SOCKET");
    if (notify_socket == nullptr || notify_socket[0] == '\0') return;

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        HAL_LOG_WARNING("Failed to create systemd notify socket: %s", std::strerror(errno));
        return;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    size_t path_len = std::strlen(notify_socket);
    if (path_len >= sizeof(addr.sun_path)) {
        HAL_LOG_WARNING("NOTIFY_SOCKET path is too long");
        close(fd);
        return;
    }
    std::memcpy(addr.sun_path, notify_socket, path_len + 1);
    if (addr.sun_path[0] == '@') addr.sun_path[0] = '\0';

    const char ready[] = "READY=1\nSTATUS=Camera pipeline and gRPC control are ready";
    // Abstract socket names are length-delimited and must not include the
    // trailing NUL; filesystem socket paths do include it.
    const bool abstract_socket = notify_socket[0] == '@';
    const socklen_t addr_len = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + path_len + (abstract_socket ? 0 : 1));
    if (sendto(fd, ready, sizeof(ready) - 1, MSG_NOSIGNAL,
               reinterpret_cast<sockaddr*>(&addr), addr_len) < 0) {
        HAL_LOG_WARNING("Failed to notify systemd readiness: %s", std::strerror(errno));
    }
    close(fd);
}

static void signal_handler(int signo) {
    HAL_LOG_INFO("Received signal %d, shutting down...", signo);
    if (g_daemon) {
        g_daemon->stop();
    }
}

/* ========== Minimal YAML-like config parser ========== */

static std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::string strip_inline_comment(const std::string& s) {
    bool in_quote = false;
    char qc = 0;
    for (size_t i = 0; i < s.size(); i++) {
        if (!in_quote && (s[i] == '"' || s[i] == '\'')) { in_quote = true; qc = s[i]; }
        else if (in_quote && s[i] == qc) { in_quote = false; }
        else if (!in_quote && s[i] == '#') { return s.substr(0, i); }
    }
    return s;
}

static std::string strip_quotes(const std::string& s) {
    if (s.size() >= 2 &&
        ((s.front() == '"' && s.back() == '"') ||
         (s.front() == '\'' && s.back() == '\'')))
        return s.substr(1, s.size() - 2);
    return s;
}

static std::string get_value(const std::string& line) {
    auto pos = line.find(':');
    if (pos == std::string::npos) return "";
    return strip_quotes(trim(line.substr(pos + 1)));
}

static bool config_key_is(const std::string& line, const char* key) {
    auto pos = line.find(':');
    if (pos == std::string::npos) return false;
    return trim(line.substr(0, pos)) == key;
}

static std::invalid_argument config_parse_error(
    const char* key,
    const std::string& raw,
    const char* reason) {
    std::ostringstream oss;
    oss << "Invalid numeric config value for " << key << ": " << reason
        << " ('" << raw << "')";
    return std::invalid_argument(oss.str());
}

static const char* consume_number_suffix(char* end) {
    while (end && *end != '\0' && std::isspace(static_cast<unsigned char>(*end))) {
        ++end;
    }
    return end;
}

static uint32_t parse_u32_config(
    const std::string& raw,
    const char* key,
    uint32_t max_value = std::numeric_limits<uint32_t>::max()) {
    std::string val = trim(raw);
    if (val.empty()) {
        throw config_parse_error(key, raw, "empty value");
    }

    char* end = nullptr;
    errno = 0;
    double parsed = std::strtod(val.c_str(), &end);
    if (end == val.c_str()) {
        throw config_parse_error(key, raw, "not a number");
    }
    if (*consume_number_suffix(end) != '\0') {
        throw config_parse_error(key, raw, "trailing characters");
    }
    if (errno == ERANGE || !std::isfinite(parsed)) {
        throw config_parse_error(key, raw, "out of range");
    }
    if (parsed < 0 || parsed > static_cast<double>(max_value)) {
        throw config_parse_error(key, raw, "outside unsigned integer range");
    }

    double rounded = std::round(parsed);
    if (parsed != rounded) {
        throw config_parse_error(key, raw, "must be an integer");
    }
    return static_cast<uint32_t>(rounded);
}

static float parse_float_config(const std::string& raw, const char* key) {
    std::string val = trim(raw);
    if (val.empty()) {
        throw config_parse_error(key, raw, "empty value");
    }

    char* end = nullptr;
    errno = 0;
    double parsed = std::strtod(val.c_str(), &end);
    if (end == val.c_str()) {
        throw config_parse_error(key, raw, "not a number");
    }
    if (*consume_number_suffix(end) != '\0') {
        throw config_parse_error(key, raw, "trailing characters");
    }
    if (errno == ERANGE || !std::isfinite(parsed) ||
        parsed < -static_cast<double>(std::numeric_limits<float>::max()) ||
        parsed > static_cast<double>(std::numeric_limits<float>::max())) {
        throw config_parse_error(key, raw, "out of range");
    }
    return static_cast<float>(parsed);
}

// Derive the AIPC install prefix from the config file path:
//   /data/aipc/etc/camera-daemon.yaml -> /data/aipc
//   /opt/aipc/etc/camera-daemon.yaml  -> /opt/aipc  (legacy)
// Used for default HAL library / log paths when the YAML omits them.
static std::string derive_install_prefix(const std::string& config_path) {
    if (config_path.find("/data/") == 0 || config_path.find("/data\\") == 0)
        return "/data/aipc";
    return "/opt/aipc";
}

static bool media_config_contains_profile(const std::string& path,
                                          const std::string& profile_name) {
    if (path.empty() || profile_name.empty()) return false;

    std::ifstream file(path);
    if (!file.is_open()) return false;

    std::ostringstream contents;
    contents << file.rdbuf();
    const std::string json = contents.str();
    const std::string quoted_name = "\"" + profile_name + "\"";
    return json.find("\"name\"") != std::string::npos &&
           json.find(quoted_name) != std::string::npos;
}

/* Pack-installed product media config: the fallback container when the
 * configured media file does not provide the IR profile. Shared by the
 * per-lens substitution below and by select_product_media_config_for_infrared. */
static constexpr const char* kProductMediaConfig =
    "/data/aipc/etc/imaging/hailo15h/imx678/theia_sl410m/4k/"
    "medialib_configs/webserver_medialib_config.json";

static void select_product_media_config_for_infrared(DaemonConfig& config) {
    if (!config.infrared.enabled ||
        media_config_contains_profile(config.media_config_path,
                                      config.infrared.infrared_profile)) {
        return;
    }
    if (!media_config_contains_profile(kProductMediaConfig,
                                       config.infrared.infrared_profile)) {
        return;
    }

    HAL_LOG_WARNING("Configured media file '%s' does not provide profile '%s'; "
                    "using product media file '%s'",
                    config.media_config_path.c_str(),
                    config.infrared.infrared_profile.c_str(),
                    kProductMediaConfig);
    config.media_config_path = kProductMediaConfig;
}

/* Adaptive lens probe (differential iris test). AF0832 carries a physical
 * iris driven by a Hall closed loop; FG2009 has none — iris register writes
 * still succeed (the driver chip is shared), but the ADC reading cannot move.
 * Drive the iris to a target far from the current reading and re-read:
 * movement toward the target = af0832, no movement = fg2009. Runs before the
 * lens service owns the UART: opens the lens bridge, probes, tears the
 * session down (the service re-initializes the MCU afterwards, same as a
 * daemon restart). Returns "af0832"/"fg2009", or "" when the probe could not
 * run (no bridge, MCU not answering, ambiguous movement) so the caller falls
 * back to the af0832 default. */
static std::string probe_lens_model(const DaemonConfig& config) {
    const std::string& lib_path = config.lens_bridge_lib;
    if (lib_path.empty()) {
        HAL_LOG_INFO("Lens probe: no lens bridge configured; skipped");
        return "";
    }
    void* handle = dlopen(lib_path.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        HAL_LOG_INFO("Lens probe: cannot load '%s' (%s)", lib_path.c_str(), dlerror());
        return "";
    }
    using IoInitFn = int (*)(const char*, uint32_t, uint32_t);
    using HandleFn = int (*)(int);
    using ModeFn = int (*)(int, int);
    using RunFn = int (*)(int, int, int);
    using AdcFn = int (*)(int, void*);
    using ProfileSetFn = int (*)(int, uint32_t);
    using StateGetFn = int (*)(int, HalLensState*);
    IoInitFn io_init = nullptr;
    HandleFn io_deinit = nullptr, lens_init = nullptr, lens_deinit = nullptr,
             iris_stop = nullptr;
    ModeFn lens_config = nullptr;
    RunFn iris_run = nullptr;
    ModeFn iris_target_set = nullptr;  // (handle, target)
    AdcFn iris_adc_get = nullptr;
    ProfileSetFn profile_set = nullptr;   // optional: MCU 0.1.8+ only
    HandleFn zoom_rz = nullptr;           // optional: PI end-stop probe
    StateGetFn state_get = nullptr;       // optional: zoom_rz_done polling
    *reinterpret_cast<void**>(&io_init) = dlsym(handle, "hal_bridge_io_init");
    *reinterpret_cast<void**>(&io_deinit) = dlsym(handle, "hal_bridge_io_deinit");
    *reinterpret_cast<void**>(&lens_init) = dlsym(handle, "hal_bridge_lens_init");
    *reinterpret_cast<void**>(&lens_deinit) = dlsym(handle, "hal_bridge_lens_deinit");
    *reinterpret_cast<void**>(&lens_config) = dlsym(handle, "hal_bridge_lens_config");
    *reinterpret_cast<void**>(&iris_run) = dlsym(handle, "hal_bridge_iris_run");
    *reinterpret_cast<void**>(&iris_stop) = dlsym(handle, "hal_bridge_iris_stop");
    *reinterpret_cast<void**>(&iris_target_set) = dlsym(handle, "hal_bridge_iris_target_set");
    *reinterpret_cast<void**>(&iris_adc_get) = dlsym(handle, "hal_bridge_iris_adc_get");
    *reinterpret_cast<void**>(&profile_set) = dlsym(handle, "hal_bridge_profile_set");
    *reinterpret_cast<void**>(&zoom_rz) = dlsym(handle, "hal_bridge_zoom_rz");
    *reinterpret_cast<void**>(&state_get) = dlsym(handle, "hal_bridge_lens_state_get");
    if (!io_init || !io_deinit || !lens_init || !lens_deinit || !lens_config ||
        !iris_run || !iris_stop || !iris_target_set || !iris_adc_get) {
        HAL_LOG_INFO("Lens probe: '%s' missing lens/iris symbols; skipped", lib_path.c_str());
        dlclose(handle);
        return "";
    }

    std::string result;
    if (io_init(config.lens_serial_device.c_str(),
                config.lens_baud_rate, config.lens_timeout_ms) == 0) {
        int ret = lens_init(1);
        if (ret != 0) {  // same retry shape as the lens service Init RPC
            lens_deinit(1);
            usleep(100 * 1000);
            ret = lens_init(1);
        }
        const int cfg_ret = ret == 0 ? lens_config(1, 0) : ret;
        if (cfg_ret != 0) {
            HAL_LOG_WARNING("Lens probe: MCU session failed (init=%d cfg=%d); falling back",
                            ret, cfg_ret);
        } else {
            /* MCU 0.1.8 gates iris ops by the active lens profile; a sticky
             * FG2009 profile from a previous session vetoes the probe with
             * NOT_SUPPORTED. Force AF0832 (iris ungated) for the probe — the
             * lens service re-pushes the final profile during Init. Firmware
             * without PROFILE_SET (0.1.7) replies an error, which is fine:
             * it never gates iris either. */
            if (profile_set) {
                const int pset = profile_set(1, 0 /* HAL_LENS_MODEL_AF0832 */);
                if (pset != 0) {
                    HAL_LOG_INFO("Lens probe: profile_set(af0832) ret=%d (pre-0.1.8 MCU; ignored)",
                                 pset);
                }
            }
            usleep(200 * 1000);  // let the MCU finish applying the config
            /* First read after boot can race the MCU's own init; retry. */
            auto read_adc = [&](uint16_t* out) -> int {
                for (int attempt = 1; attempt <= 3; ++attempt) {
                    const int ret = iris_adc_get(1, out);
                    if (ret == 0) return 0;
                    HAL_LOG_WARNING("Lens probe: iris ADC read attempt %d failed (ret=%d)",
                                    attempt, ret);
                    usleep(250 * 1000);
                }
                return -1;
            };
            uint16_t a0 = 0, a0b = 0, a1 = 0;
            bool iris_perturbed = false;
            if (read_adc(&a0) != 0 || read_adc(&a0b) != 0) {
                HAL_LOG_WARNING("Lens probe: iris ADC unreadable; falling back");
            } else {
                /* Target far from the current reading so a converged iris
                 * cannot already sit on it (10-bit ADC scale). */
                int target;
                if (a0 < 512) {
                    target = static_cast<int>(a0b) + 400;
                    if (target > 1023) target = 1023;
                } else {
                    target = static_cast<int>(a0b) - 400;
                    if (target < 0) target = 0;
                }
                /* A failed drive leaves the ADC untouched, which here would
                 * read as "no iris" and misfile a real AF0832 as fg2009.
                 * Treat a command failure as an inconclusive probe (result
                 * stays empty) and let the identity chain fall back to the
                 * af0832 default. */
                bool drive_ok = false;
                if (iris_target_set(1, target) == 0 && iris_run(1, 0, 0) == 0) {
                    iris_perturbed = true;
                    drive_ok = true;
                } else {
                    HAL_LOG_WARNING("Lens probe: iris drive command failed; falling back");
                }
                usleep(drive_ok ? 1200 * 1000 : 0);  // iris mechanical settle
                const int get_ret = drive_ok ? read_adc(&a1) : -1;
                iris_stop(1);
                if (!drive_ok) {
                    // skipped: drive failed, result stays empty
                } else if (get_ret != 0) {
                    HAL_LOG_WARNING("Lens probe: iris ADC re-read failed; falling back");
                } else {
                    const int moved = static_cast<int>(a1) > static_cast<int>(a0b)
                                          ? static_cast<int>(a1) - static_cast<int>(a0b)
                                          : static_cast<int>(a0b) - static_cast<int>(a1);
                    const int d_new = static_cast<int>(a1) > target
                                          ? static_cast<int>(a1) - target
                                          : target - static_cast<int>(a1);
                    const int d_old = static_cast<int>(a0b) > target
                                          ? static_cast<int>(a0b) - target
                                          : target - static_cast<int>(a0b);
                    HAL_LOG_INFO("Lens probe: iris adc %u->%u (target %d, moved %d%s)",
                                 a0b, a1, target, moved, a0 != a0b ? ", base noisy" : "");
                    if (moved >= 40 && d_new < d_old) {
                        result = "af0832";
                    } else if (moved < 40) {
                        result = "fg2009";
                    } else if (zoom_rz && state_get) {
                        /* Auxiliary PI probe for the ambiguous middle ground.
                         * AF0832 has the zoom photo-interrupter, so reset-zero
                         * completes (zoom_rz_done sets); FG2009 has none, the
                         * MCU motor times out (~5s) and done never sets. Still
                         * under the forced AF0832 profile, so 0.1.8 firmware
                         * does not gate the command. */
                        HalLensState st0{};
                        if (state_get(1, &st0) == 0 && st0.zoom_rz_done) {
                            result = "af0832";  // homed earlier => PI fired once
                            HAL_LOG_INFO("Lens probe: PI probe skipped (already homed) -> af0832");
                        } else if (zoom_rz(1) == 0) {
                            HAL_LOG_INFO("Lens probe: iris ambiguous; running zoom reset-zero (PI) probe");
                            for (int waited_ms = 0; waited_ms < 7000; waited_ms += 300) {
                                usleep(300 * 1000);
                                HalLensState st{};
                                if (state_get(1, &st) == 0 && st.zoom_rz_done) {
                                    result = "af0832";
                                    break;
                                }
                            }
                            if (result.empty()) result = "fg2009";
                            HAL_LOG_INFO("Lens probe: PI probe -> %s", result.c_str());
                        } else {
                            HAL_LOG_WARNING("Lens probe: ambiguous movement, zoom RZ start failed; falling back");
                        }
                    } else {
                        HAL_LOG_WARNING("Lens probe: ambiguous movement; falling back");
                    }
                }
            }
            /* Restore the AF0832 default iris: the probe chased a far target,
             * so drive the physical iris back to the system default (target 0,
             * g_default_iris_config.iris_tgt). Skipped when the probe itself
             * decided fg2009 — no physical iris exists there to restore. */
            if (iris_perturbed && result != "fg2009") {
                if (iris_target_set(1, 0) == 0 && iris_run(1, 0, 0) == 0) {
                    usleep(1200 * 1000);  // settle at the default target
                    iris_stop(1);
                    uint16_t a2 = 0;
                    if (iris_adc_get(1, &a2) == 0) {
                        HAL_LOG_INFO("Lens probe: iris restored to default target 0 (adc %u)", a2);
                    }
                } else {
                    HAL_LOG_WARNING("Lens probe: iris restore command failed; "
                                    "iris left perturbed");
                }
            }
        }
        lens_deinit(1);
        io_deinit(1);
    } else {
        HAL_LOG_WARNING("Lens probe: io_init on %s failed; falling back",
                        config.lens_serial_device.c_str());
    }
    dlclose(handle);
    return result;
}

/* Applies the lens model identity: pure software adaptivity only. The
 * adaptive iris probe (with its PI auxiliary probe) decides; af0832 is the
 * last-resort default so an unprobed unit still boots. The factory EEPROM
 * is deliberately not consulted — lens identity must not depend on what a
 * factory station burned (a mis-stamped unit would boot the wrong lens
 * branch with no runtime correction). */
static void apply_lens_model_identity(DaemonConfig& config) {
    const std::string probed = probe_lens_model(config);
    if (!probed.empty()) {
        config.lens_model = probed;
        HAL_LOG_INFO("Lens product model: %s (adaptive iris probe)", probed.c_str());
        return;
    }
    config.lens_model = "af0832";
    HAL_LOG_INFO("Lens product model: af0832 (probe carried no lens model)");
}

/* Per-lens IR profile: both lens versions share one media config, but each
 * carries its own Infrared_* entry (optics differ, so does the IQ tuning).
 * When the yaml still has the AF default name, the FG2009 motorized-zoom
 * lens switches to its own entry; an explicit non-default profile_name is
 * respected for bench tuning — same contract as the FG2009 IR zoom LUT
 * substitution. Resolved once here so every later consumer (mode checks,
 * profile switch, boot persistence guard) sees the effective name.
 *
 * The substitution only fires when something can actually resolve the new
 * name: an empty config_path means the compiled-in default bundle (whose
 * profile list is build-gated to carry both entries), otherwise either the
 * configured or the pack product container must list it. An upgraded unit
 * running new binaries against preserved pre-per-lens imaging trees keeps
 * the shared Infrared_Basic entry (correct FG2009 tuning) instead of
 * renaming into a profile nothing can serve — night mode degrades to the
 * previous behavior rather than failing HAL_ERR_PROFILE_INVALID. */
static void apply_lens_infrared_profile(DaemonConfig& config) {
    if (config.lens_model != "fg2009" ||
        config.infrared.infrared_profile != "Infrared_Basic") {
        return;
    }
    static const std::string kFg2009IrProfile = "Infrared_Basic_FG2009";
    if (!config.media_config_path.empty() &&
        !media_config_contains_profile(config.media_config_path,
                                       kFg2009IrProfile) &&
        !media_config_contains_profile(kProductMediaConfig, kFg2009IrProfile)) {
        HAL_LOG_WARNING("FG2009 lens but no media config provides '%s'; keeping "
                        "'%s' (upgrade the imaging trees for per-lens IR)",
                        kFg2009IrProfile.c_str(),
                        config.infrared.infrared_profile.c_str());
        return;
    }
    config.infrared.infrared_profile = kFg2009IrProfile;
    HAL_LOG_INFO("FG2009 lens; IR profile -> %s",
                 config.infrared.infrared_profile.c_str());
}

static DaemonConfig load_config(const std::string& path) {
    DaemonConfig cfg;

    // Defaults
    // HAL library: derive from the install prefix so a missing video_library
    // key still resolves to the real monolithic library. The previous default
    // (hal-hailo15.so) did not exist and caused HAL load failures when the
    // YAML key was absent.
    cfg.video_lib = derive_install_prefix(path) + "/lib/hal/libaipc_hal.so";
    cfg.codec_lib = "";
    cfg.device_path = "/dev/video0";
    cfg.device_width = 1920;
    cfg.device_height = 1080;
    cfg.device_fps = 30;
    cfg.device_format = 0;  // NV12
    cfg.fd_pub_sock_path = "/run/aipc/camera.sock";
    cfg.fd_pub_max_clients = 16;
    cfg.fd_pub_max_outstanding = 3;
    cfg.fd_pub_lease_ms = 200;
    cfg.rtsp_enabled = true;
    cfg.rtsp_port = 8554;
    cfg.encoded_pub_enabled = true;
    cfg.encoded_pub_dir = "/run/aipc/encoded";
    cfg.watchdog_scan_ms = 100;
    cfg.watchdog_timeout_ms = 5000;
    cfg.watchdog_warn_ms = 3000;
    cfg.log_level = "info";

    // FG2009 open-loop geometry: bench-calibrated bootstrap offsets
    hal_lens_fg2009_params_init_defaults(&cfg.lens_fg2009);

    // Streams derived from encoders after YAML parse (see below)
    cfg.streams = {};

    // Default encoder for main stream (aligned with video_test)
    EncoderCfg main_enc;
    main_enc.stream_name = "main";
    main_enc.codec = "h264";
    main_enc.width = 1920;
    main_enc.height = 1080;
    main_enc.fps = 30;
    main_enc.bitrate = 4000000;
    main_enc.gop = 60;
    main_enc.cbr = true;
    main_enc.rc_mode = "CBR";
    main_enc.max_bitrate = 0;
    main_enc.qp_min = 20;
    main_enc.qp_max = 45;
    main_enc.output_pool_max_buffers = 11;
    main_enc.max_queue_size = 8;
    // OSD disabled by default — enable via osd_config_path in YAML
    cfg.encoders = { main_enc };

    // Default OSD (empty — enable via config file to avoid missing font errors)
    cfg.osd_overlays = {};

    // Try to load config file
    std::ifstream file(path);
    if (!file.is_open()) {
        HAL_LOG_WARNING("Config file not found: %s, using defaults", path.c_str());
        return cfg;
    }

    HAL_LOG_INFO("Loading config: %s", path.c_str());

    std::string line;
    std::string section;
    std::string lens_subsection;

    while (std::getline(file, line)) {
        line = strip_inline_comment(line);
        std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') continue;

        // Track section context
        if (trimmed.find("hal:") == 0) { section = "hal"; continue; }
        if (trimmed.find("media:") == 0) { section = "media"; continue; }
        if (trimmed.find("video:") == 0) { section = "video"; continue; }
        if (trimmed.find("rtsp:") == 0) { section = "rtsp"; continue; }
        if (trimmed.find("watchdog:") == 0) { section = "watchdog"; continue; }
        if (trimmed.find("ai_overlay:") == 0) { section = "ai_overlay"; continue; }
        if (trimmed.find("audio:") == 0) { section = "audio"; continue; }
        if (trimmed.find("autofocus:") == 0) { section = "autofocus"; continue; }
        if (trimmed.find("infrared:") == 0) { section = "infrared"; continue; }
        if (trimmed.find("light_sensor:") == 0) { section = "light_sensor"; continue; }
        if (trimmed.find("service:") == 0) { section = "service"; continue; }
        if (trimmed.find("lens:") == 0) { section = "lens"; lens_subsection.clear(); continue; }
        if (trimmed.find("dsp:") == 0) { section = "dsp"; continue; }
        if (trimmed.find("injection:") == 0) { section = "injection"; continue; }
        if (trimmed.find("streams:") == 0) { section = "streams"; cfg.streams.clear(); continue; }
        if (trimmed.find("encoders:") == 0) { section = "encoders"; cfg.encoders.clear(); continue; }

        std::string val = get_value(trimmed);

        if (section == "hal") {
            if (trimmed.find("video_library:") != std::string::npos)
                cfg.video_lib = val;
            else if (trimmed.find("codec_library:") != std::string::npos)
                cfg.codec_lib = val;
            else if (trimmed.find("lens_library:") != std::string::npos)
                cfg.lens_bridge_lib = val;
            else if (trimmed.find("led_library:") != std::string::npos)
                cfg.led_lib = val;
        } else if (section == "media") {
            if (trimmed.find("config_path:") != std::string::npos)
                cfg.media_config_path = val;
            else if (trimmed.find("config_json:") != std::string::npos)
                cfg.media_config_json = val;
            else if (trimmed.find("backup_path:") != std::string::npos)
                cfg.backup_folder_path = val;
        } else if (section == "video") {
            if (trimmed.find("device_path:") != std::string::npos)
                cfg.device_path = val;
            else if (trimmed.find("config_path:") != std::string::npos)
                cfg.video_config_path = val;
        } else if (section == "rtsp") {
            if (trimmed.find("enabled:") != std::string::npos)
                cfg.rtsp_enabled = (val == "true" || val == "1");
            else if (trimmed.find("port:") != std::string::npos)
                cfg.rtsp_port = static_cast<uint16_t>(parse_u32_config(val, "rtsp.port", 65535));
        } else if (section == "watchdog") {
            if (trimmed.find("scan_interval_ms:") != std::string::npos)
                cfg.watchdog_scan_ms = parse_u32_config(val, "watchdog.scan_interval_ms");
            else if (trimmed.find("frame_timeout_ms:") != std::string::npos)
                cfg.watchdog_timeout_ms = parse_u32_config(val, "watchdog.frame_timeout_ms");
            else if (trimmed.find("warn_threshold_ms:") != std::string::npos)
                cfg.watchdog_warn_ms = parse_u32_config(val, "watchdog.warn_threshold_ms");
        } else if (section == "dsp") {
            // P2: DspServiceConfig knobs (defaults live in dsp_service.h).
            // Quota and retained-resource caps are per process incarnation.
            if (trimmed.find("quota_jobs_per_sec:") != std::string::npos)
                cfg.dsp.quota_jobs_per_sec = parse_float_config(val, "dsp.quota_jobs_per_sec");
            else if (trimmed.find("quota_mpix_per_sec:") != std::string::npos)
                cfg.dsp.quota_mpix_per_sec = parse_float_config(val, "dsp.quota_mpix_per_sec");
            else if (trimmed.find("job_timeout_ms:") != std::string::npos)
                cfg.dsp.job_timeout_ms = parse_u32_config(val, "dsp.job_timeout_ms");
            else if (trimmed.find("max_batch:") != std::string::npos)
                cfg.dsp.max_batch = parse_u32_config(val, "dsp.max_batch");
            else if (trimmed.find("max_buffers_per_client:") != std::string::npos)
                cfg.dsp.max_buffers_per_client = parse_u32_config(val, "dsp.max_buffers_per_client");
            else if (trimmed.find("max_client_pixels:") != std::string::npos)
                cfg.dsp.max_client_pixels = parse_u32_config(val, "dsp.max_client_pixels");
            else if (trimmed.find("max_total_buffers:") != std::string::npos)
                cfg.dsp.max_total_buffers = parse_u32_config(val, "dsp.max_total_buffers");
            else if (trimmed.find("max_total_buffer_pixels:") != std::string::npos)
                cfg.dsp.max_total_buffer_pixels = parse_u32_config(val, "dsp.max_total_buffer_pixels");
            else if (trimmed.find("max_imports_per_client:") != std::string::npos)
                cfg.dsp.max_imports_per_client = parse_u32_config(val, "dsp.max_imports_per_client");
            else if (trimmed.find("max_import_bytes_per_client:") != std::string::npos)
                cfg.dsp.max_import_bytes_per_client = parse_u32_config(val, "dsp.max_import_bytes_per_client");
            else if (trimmed.find("max_total_imports:") != std::string::npos)
                cfg.dsp.max_total_imports = parse_u32_config(val, "dsp.max_total_imports");
            else if (trimmed.find("max_total_import_bytes:") != std::string::npos)
                cfg.dsp.max_total_import_bytes = parse_u32_config(val, "dsp.max_total_import_bytes");
            else if (trimmed.find("max_async_jobs_per_client:") != std::string::npos)
                cfg.dsp.max_async_jobs_per_client = parse_u32_config(val, "dsp.max_async_jobs_per_client");
        } else if (section == "injection") {
            // P0-P2: app frame injection knobs (defaults live in
            // injection_service.h). `enabled` is the master gate and ships
            // false: opt-in, enable per deployment.
            if (trimmed.find("enabled:") != std::string::npos)
                cfg.injection.enabled = (val == "true" || val == "1");
            else if (trimmed.find("queue_capacity:") != std::string::npos)
                cfg.injection.queue_capacity = parse_u32_config(val, "injection.queue_capacity");
            else if (trimmed.find("allowed_apps:") != std::string::npos) {
                // P2-12 manifest permission gate: comma-separated app
                // identities (SO_PEERCRED cmdline basenames). Empty/missing
                // = allow all (development default).
                cfg.injection.allowed_apps.clear();
                size_t pos = 0;
                while (pos <= val.size()) {
                    size_t comma = val.find(',', pos);
                    std::string app = val.substr(pos, (comma == std::string::npos
                                                        ? val.size() : comma) - pos);
                    /* trim spaces around each entry */
                    const size_t b = app.find_first_not_of(" \t");
                    const size_t e = app.find_last_not_of(" \t");
                    if (b != std::string::npos)
                        cfg.injection.allowed_apps.push_back(app.substr(b, e - b + 1));
                    if (comma == std::string::npos)
                        break;
                    pos = comma + 1;
                }
            }
        } else if (section == "ai_overlay") {
            if (trimmed.find("enabled:") != std::string::npos)
                cfg.ai_overlay_enabled = (val == "true" || val == "1");
            else if (trimmed.find("event_bus_endpoint:") != std::string::npos)
                cfg.ai_overlay_event_bus_endpoint = val;
            else if (trimmed.find("topic_prefix:") != std::string::npos)
                cfg.ai_overlay_topic_prefix = val;
            else if (trimmed.find("draw_labels:") != std::string::npos)
                cfg.ai_overlay_draw_labels = (val == "true" || val == "1");
            else if (trimmed.find("draw_confidence:") != std::string::npos)
                cfg.ai_overlay_draw_confidence = (val == "true" || val == "1");
            else if (trimmed.find("draw_landmarks:") != std::string::npos)
                cfg.ai_overlay_draw_landmarks = (val == "true" || val == "1");
            else if (trimmed.find("enable_face_blur:") != std::string::npos)
                cfg.ai_overlay_enable_face_blur = (val == "true" || val == "1");
            else if (trimmed.find("face_blur_block_size:") != std::string::npos)
                cfg.ai_overlay_face_blur_block_size = parse_u32_config(val, "ai_overlay.face_blur_block_size");
            else if (trimmed.find("box_thickness:") != std::string::npos)
                cfg.ai_overlay_box_thickness = parse_u32_config(val, "ai_overlay.box_thickness");
            else if (trimmed.find("result_ttl_ms:") != std::string::npos)
                cfg.ai_overlay_result_ttl_ms = parse_u32_config(val, "ai_overlay.result_ttl_ms");
            else if (trimmed.find("strict_frame_lock:") != std::string::npos)
                cfg.ai_overlay_strict_frame_lock = (val == "true" || val == "1");
            else if (trimmed.find("strict_wait_cap_ms:") != std::string::npos)
                cfg.ai_overlay_strict_wait_cap_ms = parse_u32_config(val, "ai_overlay.strict_wait_cap_ms");
            else if (trimmed.find("legacy_auto_bind:") != std::string::npos)
                cfg.ai_overlay_legacy_auto_bind = (val == "true" || val == "1");
            else if (trimmed.find("overlay_library:") != std::string::npos)
                cfg.ai_overlay_lib = val;
            else if (trimmed.find("stream_map:") != std::string::npos && !val.empty()) {
                // Flat format: "src1:dst1,src2:dst2,..."
                std::istringstream ss(val);
                std::string pair;
                while (std::getline(ss, pair, ',')) {
                    auto c = pair.find(':');
                    if (c != std::string::npos) {
                        std::string k = trim(pair.substr(0, c));
                        std::string v = trim(pair.substr(c + 1));
                        if (!k.empty() && !v.empty())
                            cfg.ai_overlay_stream_map[k] = v;
                    }
                }
            } else if (trimmed.find("result_ttl_map:") != std::string::npos && !val.empty()) {
                // Flat format: "dst1:ms1,dst2:ms2,..." — per-display-stream TTL
                // overrides (keyed like stream_map values; 0 = derive, see
                // resolve_result_ttl_ms).
                std::istringstream ss(val);
                std::string pair;
                while (std::getline(ss, pair, ',')) {
                    auto c = pair.find(':');
                    if (c != std::string::npos) {
                        std::string k = trim(pair.substr(0, c));
                        std::string v = trim(pair.substr(c + 1));
                        if (!k.empty() && !v.empty())
                            cfg.ai_overlay_stream_result_ttls[k] =
                                parse_u32_config(v, "ai_overlay.result_ttl_map");
                    }
                }
            } else if (trimmed.find("bindings:") != std::string::npos && !val.empty()) {
                // Flat format: "infer1:display1,infer2:display2,..." — same
                // direction as stream_map. A bound infer stream's platform
                // results draw on the display stream (the behavior-decoupling
                // admission path); unbound platform events are dropped and
                // counted. legacy_auto_bind admits everything (old behavior).
                std::istringstream ss(val);
                std::string pair;
                while (std::getline(ss, pair, ',')) {
                    auto c = pair.find(':');
                    if (c != std::string::npos) {
                        std::string k = trim(pair.substr(0, c));
                        std::string v = trim(pair.substr(c + 1));
                        if (!k.empty() && !v.empty())
                            cfg.ai_overlay_bindings[k] = v;
                    }
                }
            }
        } else if (section == "streams") {
            bool is_new = (trimmed.find("- ") == 0);
            if (is_new) {
                StreamCfg s;
                s.width = 1920; s.height = 1080; s.fps = 30; // Defaults
                s.pool_max_buffers = 8; s.max_queue_size = 12;
                cfg.streams.push_back(s);
                trimmed = trim(trimmed.substr(1)); // Skip '-'
                val = get_value(trimmed);
            }
            if (!cfg.streams.empty()) {
                auto& s = cfg.streams.back();
                if (trimmed.find("name:") != std::string::npos) s.name = val;
                else if (trimmed.find("width:") != std::string::npos) s.width = parse_u32_config(val, "streams.width");
                else if (trimmed.find("height:") != std::string::npos) s.height = parse_u32_config(val, "streams.height");
                else if (trimmed.find("fps:") != std::string::npos) s.fps = parse_u32_config(val, "streams.fps");
                else if (trimmed.find("pool_max_buffers:") != std::string::npos) s.pool_max_buffers = parse_u32_config(val, "streams.pool_max_buffers");
                else if (trimmed.find("max_queue_size:") != std::string::npos) s.max_queue_size = parse_u32_config(val, "streams.max_queue_size");
            }
        } else if (section == "encoders") {
            bool is_new = (trimmed.find("- ") == 0);
            if (is_new) {
                EncoderCfg enc;
                enc.codec = "h264"; enc.bitrate = 4000000; enc.gop = 60; // Defaults
                enc.cbr = true; enc.rc_mode = "CBR"; enc.max_bitrate = 0;
                enc.qp_min = 20; enc.qp_max = 45;
                enc.output_pool_max_buffers = 11; enc.max_queue_size = 8;
                cfg.encoders.push_back(enc);
                trimmed = trim(trimmed.substr(1)); // Skip '-'
                val = get_value(trimmed);
            }
            if (!cfg.encoders.empty()) {
                auto& enc = cfg.encoders.back();
                if (trimmed.find("stream_name:") != std::string::npos) enc.stream_name = val;
                else if (trimmed.find("codec:") != std::string::npos) enc.codec = val;
                else if (trimmed.find("width:") != std::string::npos) enc.width = parse_u32_config(val, "encoders.width");
                else if (trimmed.find("height:") != std::string::npos) enc.height = parse_u32_config(val, "encoders.height");
                else if (trimmed.find("fps:") != std::string::npos) enc.fps = parse_u32_config(val, "encoders.fps");
                else if (trimmed.find("bitrate:") != std::string::npos) enc.bitrate = parse_u32_config(val, "encoders.bitrate");
                else if (trimmed.find("gop:") != std::string::npos) enc.gop = parse_u32_config(val, "encoders.gop");
                else if (trimmed.find("cbr:") != std::string::npos) enc.cbr = (val == "true" || val == "1");
                else if (trimmed.find("rc_mode:") != std::string::npos) enc.rc_mode = val;
                else if (trimmed.find("max_bitrate:") != std::string::npos) enc.max_bitrate = parse_u32_config(val, "encoders.max_bitrate");
                else if (trimmed.find("qp_min:") != std::string::npos) enc.qp_min = parse_u32_config(val, "encoders.qp_min");
                else if (trimmed.find("qp_max:") != std::string::npos) enc.qp_max = parse_u32_config(val, "encoders.qp_max");
                else if (trimmed.find("osd_config_path:") != std::string::npos) enc.osd_config_path = val;
                else if (trimmed.find("enabled:") != std::string::npos) enc.enabled = (val == "true" || val == "1");
            }
        } else if (section == "audio") {
            if (config_key_is(trimmed, "enabled"))
                cfg.audio.enabled = (val == "true" || val == "1");
            else if (trimmed.find("capture_device:") != std::string::npos)
                cfg.audio.capture_device = val;
            else if (trimmed.find("sample_rate:") != std::string::npos)
                cfg.audio.sample_rate = parse_u32_config(val, "audio.sample_rate");
            else if (trimmed.find("channels:") != std::string::npos)
                cfg.audio.channels = parse_u32_config(val, "audio.channels");
            else if (trimmed.find("codec:") != std::string::npos)
                cfg.audio.codec = val;
            else if (trimmed.find("bitrate:") != std::string::npos)
                cfg.audio.bitrate = parse_u32_config(val, "audio.bitrate");
            else if (trimmed.find("volume:") != std::string::npos)
                cfg.audio.volume = parse_float_config(val, "audio.volume");
            else if (trimmed.find("mute:") != std::string::npos)
                cfg.audio.mute = (val == "true" || val == "1");
            else if (trimmed.find("audio_library:") != std::string::npos)
                cfg.audio.audio_lib = val;
            else if (config_key_is(trimmed, "playback_enabled")) {
                // platform-api owns this UI/talk gate; it must not alter capture.
            }
        } else if (section == "autofocus") {
            if (trimmed.find("enabled:") != std::string::npos)
                cfg.autofocus.enabled = (val == "true" || val == "1");
            else if (trimmed.find("startup_af:") != std::string::npos)
                cfg.autofocus.startup_af = (val == "true" || val == "1");
            else if (trimmed.find("stream_name:") != std::string::npos)
                cfg.autofocus.stream_name = val;
            else if (trimmed.find("calibration_path:") != std::string::npos)
                cfg.autofocus.calibration_path = val;
            else if (trimmed.find("startup_zoom_ratio:") != std::string::npos)
                cfg.autofocus.startup_zoom_ratio = parse_float_config(val, "autofocus.startup_zoom_ratio");
            else if (trimmed.find("startup_focus_distance_m:") != std::string::npos)
                cfg.autofocus.startup_focus_distance_m = parse_float_config(val, "autofocus.startup_focus_distance_m");
            else if (trimmed.find("startup_wait_frames:") != std::string::npos)
                cfg.autofocus.startup_wait_frames = static_cast<int>(parse_u32_config(val, "autofocus.startup_wait_frames"));
            else if (trimmed.find("startup_recovery_span:") != std::string::npos)
                cfg.autofocus.startup_recovery_span = static_cast<int>(parse_u32_config(val, "autofocus.startup_recovery_span"));
            else if (trimmed.find("startup_ready_timeout_ms:") != std::string::npos)
                cfg.autofocus.startup_ready_timeout_ms = static_cast<int>(parse_u32_config(val, "autofocus.startup_ready_timeout_ms"));
            else if (trimmed.find("frame_wait_timeout_ms:") != std::string::npos)
                cfg.autofocus.frame_wait_timeout_ms = static_cast<int>(parse_u32_config(val, "autofocus.frame_wait_timeout_ms"));
            else if (trimmed.find("move_timeout_ms:") != std::string::npos)
                cfg.autofocus.move_timeout_ms = static_cast<int>(parse_u32_config(val, "autofocus.move_timeout_ms"));
            else if (trimmed.find("follow_sync_motion:") != std::string::npos)
                cfg.autofocus.follow_sync_motion = (val == "true" || val == "1");
            else if (trimmed.find("follow_sync_fallback_sequential:") != std::string::npos)
                cfg.autofocus.follow_sync_fallback_sequential = (val == "true" || val == "1");
            else if (trimmed.find("follow_sync_zoom_max_pps:") != std::string::npos)
                cfg.autofocus.follow_sync_zoom_max_pps = static_cast<int>(parse_u32_config(val, "autofocus.follow_sync_zoom_max_pps"));
            else if (trimmed.find("follow_sync_focus_max_pps:") != std::string::npos)
                cfg.autofocus.follow_sync_focus_max_pps = static_cast<int>(parse_u32_config(val, "autofocus.follow_sync_focus_max_pps"));
            else if (trimmed.find("follow_sync_min_pps:") != std::string::npos)
                cfg.autofocus.follow_sync_min_pps = static_cast<int>(parse_u32_config(val, "autofocus.follow_sync_min_pps"));
            else if (trimmed.find("follow_sync_zoom_tolerance:") != std::string::npos)
                cfg.autofocus.follow_sync_zoom_tolerance = static_cast<int>(parse_u32_config(val, "autofocus.follow_sync_zoom_tolerance"));
            else if (trimmed.find("follow_sync_focus_tolerance:") != std::string::npos)
                cfg.autofocus.follow_sync_focus_tolerance = static_cast<int>(parse_u32_config(val, "autofocus.follow_sync_focus_tolerance"));
            else if (trimmed.find("follow_path_zoom_step_wide:") != std::string::npos)
                cfg.autofocus.follow_path_zoom_step_wide = static_cast<int>(parse_u32_config(val, "autofocus.follow_path_zoom_step_wide"));
            else if (trimmed.find("follow_path_zoom_step_mid:") != std::string::npos)
                cfg.autofocus.follow_path_zoom_step_mid = static_cast<int>(parse_u32_config(val, "autofocus.follow_path_zoom_step_mid"));
            else if (trimmed.find("follow_path_zoom_step_tele:") != std::string::npos)
                cfg.autofocus.follow_path_zoom_step_tele = static_cast<int>(parse_u32_config(val, "autofocus.follow_path_zoom_step_tele"));
            else if (trimmed.find("follow_path_focus_step_wide:") != std::string::npos)
                cfg.autofocus.follow_path_focus_step_wide = static_cast<int>(parse_u32_config(val, "autofocus.follow_path_focus_step_wide"));
            else if (trimmed.find("follow_path_focus_step_mid:") != std::string::npos)
                cfg.autofocus.follow_path_focus_step_mid = static_cast<int>(parse_u32_config(val, "autofocus.follow_path_focus_step_mid"));
            else if (trimmed.find("follow_path_focus_step_tele:") != std::string::npos)
                cfg.autofocus.follow_path_focus_step_tele = static_cast<int>(parse_u32_config(val, "autofocus.follow_path_focus_step_tele"));
            else if (trimmed.find("follow_curve_error_enable:") != std::string::npos)
                cfg.autofocus.follow_curve_error_enable = (val == "true" || val == "1");
            else if (trimmed.find("follow_curve_error_wide:") != std::string::npos)
                cfg.autofocus.follow_curve_error_wide = static_cast<int>(parse_u32_config(val, "autofocus.follow_curve_error_wide"));
            else if (trimmed.find("follow_curve_error_mid:") != std::string::npos)
                cfg.autofocus.follow_curve_error_mid = static_cast<int>(parse_u32_config(val, "autofocus.follow_curve_error_mid"));
            else if (trimmed.find("follow_curve_error_tele:") != std::string::npos)
                cfg.autofocus.follow_curve_error_tele = static_cast<int>(parse_u32_config(val, "autofocus.follow_curve_error_tele"));
            else if (trimmed.find("pps:") != std::string::npos)
                cfg.autofocus.pps = static_cast<int>(parse_u32_config(val, "autofocus.pps"));
            else if (trimmed.find("min_focus_pos:") != std::string::npos)
                cfg.autofocus.min_focus_pos = std::stoi(val);
            else if (trimmed.find("max_focus_pos:") != std::string::npos)
                cfg.autofocus.max_focus_pos = std::stoi(val);
            else if (trimmed.find("coarse_step:") != std::string::npos)
                cfg.autofocus.coarse_step = static_cast<int>(parse_u32_config(val, "autofocus.coarse_step"));
            else if (trimmed.find("coarse_span:") != std::string::npos)
                cfg.autofocus.coarse_span = static_cast<int>(parse_u32_config(val, "autofocus.coarse_span"));
            else if (trimmed.find("fine_step:") != std::string::npos)
                cfg.autofocus.fine_step = static_cast<int>(parse_u32_config(val, "autofocus.fine_step"));
            else if (trimmed.find("fine_span:") != std::string::npos)
                cfg.autofocus.fine_span = static_cast<int>(parse_u32_config(val, "autofocus.fine_span"));
            else if (trimmed.find("trace_scan:") != std::string::npos)
                cfg.autofocus.trace_scan = (val == "true" || val == "1");
            else if (trimmed.find("max_moves:") != std::string::npos)
                cfg.autofocus.max_moves = static_cast<int>(parse_u32_config(val, "autofocus.max_moves"));
            else if (trimmed.find("balanced_retry:") != std::string::npos)
                cfg.autofocus.balanced_retry = static_cast<int>(parse_u32_config(val, "autofocus.balanced_retry"));
            else if (trimmed.find("confidence_accept:") != std::string::npos)
                cfg.autofocus.confidence_accept = parse_float_config(val, "autofocus.confidence_accept");
            else if (trimmed.find("confidence_recovery:") != std::string::npos)
                cfg.autofocus.confidence_recovery = parse_float_config(val, "autofocus.confidence_recovery");
            else if (trimmed.find("sensor_native_width:") != std::string::npos)
                cfg.autofocus.sensor_native_width = static_cast<int>(parse_u32_config(val, "autofocus.sensor_native_width"));
            else if (trimmed.find("sensor_native_height:") != std::string::npos)
                cfg.autofocus.sensor_native_height = static_cast<int>(parse_u32_config(val, "autofocus.sensor_native_height"));
        } else if (section == "infrared") {
            if (trimmed.find("enabled:") != std::string::npos)
                cfg.infrared.enabled = (val == "true" || val == "1");
            else if (trimmed.find("profile_name:") != std::string::npos)
                cfg.infrared.infrared_profile = val;
            else if (trimmed.find("default_mode:") != std::string::npos)
                cfg.infrared.default_mode = val;
            else if (trimmed.find("near_led_id:") != std::string::npos)
                cfg.infrared.near_led_id = parse_u32_config(val, "infrared.near_led_id", 255);
            else if (trimmed.find("far_led_id:") != std::string::npos)
                cfg.infrared.far_led_id = parse_u32_config(val, "infrared.far_led_id", 255);
            else if (trimmed.find("auto_follow:") != std::string::npos)
                cfg.infrared.auto_follow = (val == "true" || val == "1");
            else if (trimmed.find("lut_path:") != std::string::npos)
                cfg.infrared.lut_path = val;
            else if (trimmed.find("deadband_percent:") != std::string::npos)
                cfg.infrared.deadband_percent = static_cast<int>(parse_u32_config(val, "infrared.deadband_percent", 100));
            else if (trimmed.find("endpoint_settle_frames:") != std::string::npos)
                cfg.infrared.endpoint_settle_frames = static_cast<int>(parse_u32_config(val, "infrared.endpoint_settle_frames"));
            else if (trimmed.find("mode_settle_frames:") != std::string::npos)
                cfg.infrared.mode_settle_frames = static_cast<int>(parse_u32_config(val, "infrared.mode_settle_frames"));
            else if (trimmed.find("log_updates:") != std::string::npos)
                cfg.infrared.log_updates = (val == "true" || val == "1");
        } else if (section == "light_sensor") {
            if (trimmed.find("enabled:") != std::string::npos)
                cfg.light_sensor.enabled = (val == "true" || val == "1");
            else if (trimmed.find("auto_on_boot:") != std::string::npos)
                cfg.light_sensor.auto_on_boot = (val == "true" || val == "1");
            else if (trimmed.find("night_enter:") != std::string::npos)
                cfg.light_sensor.night_enter = static_cast<int>(parse_u32_config(val, "light_sensor.night_enter", 100));
            else if (trimmed.find("day_enter:") != std::string::npos)
                cfg.light_sensor.day_enter = static_cast<int>(parse_u32_config(val, "light_sensor.day_enter", 100));
            else if (trimmed.find("sample_interval_ms:") != std::string::npos)
                cfg.light_sensor.sample_interval_ms = static_cast<int>(parse_u32_config(val, "light_sensor.sample_interval_ms", 60000));
            else if (trimmed.find("stable_samples:") != std::string::npos)
                cfg.light_sensor.stable_samples = static_cast<int>(parse_u32_config(val, "light_sensor.stable_samples", 100));
            else if (trimmed.find("min_hold_s:") != std::string::npos)
                cfg.light_sensor.min_hold_ms = static_cast<int>(parse_u32_config(val, "light_sensor.min_hold_s", 3600)) * 1000;
            else if (trimmed.find("dark_mv:") != std::string::npos)
                cfg.light_sensor.dark_mv = static_cast<int>(parse_u32_config(val, "light_sensor.dark_mv", 3300));
            else if (trimmed.find("bright_mv:") != std::string::npos)
                cfg.light_sensor.bright_mv = static_cast<int>(parse_u32_config(val, "light_sensor.bright_mv", 3300));
        } else if (section == "service") {
            if (trimmed.find("log_level:") != std::string::npos)
                cfg.log_level = val;
            else if (trimmed.find("log_file:") != std::string::npos)
                cfg.log_file = val;
        } else if (section == "lens") {
            if (trimmed.find("fg2009:") == 0) {
                lens_subsection = "fg2009";
            } else if (trimmed.find("position_persistence:") != std::string::npos) {
                cfg.lens_position_persistence = (val == "true" || val == "1");
            } else if (trimmed.find("image_probe_") != std::string::npos) {
                // lens: image_probe_* keys (image-sharpness fixed-lens probe).
                // Matched before the fg2009 subsection branch: keys unique to
                // this block must parse wherever they sit under lens:.
                if (trimmed.find("image_probe_enabled:") != std::string::npos)
                    cfg.lens_image_probe_enabled = (val == "true" || val == "1");
                else if (trimmed.find("image_probe_steps:") != std::string::npos)
                    cfg.lens_image_probe_steps = (int)parse_u32_config(val, "lens.image_probe_steps");
                else if (trimmed.find("image_probe_frames:") != std::string::npos)
                    cfg.lens_image_probe_frames = (int)parse_u32_config(val, "lens.image_probe_frames");
                else if (trimmed.find("image_probe_settle_ms:") != std::string::npos)
                    cfg.lens_image_probe_settle_ms = (int)parse_u32_config(val, "lens.image_probe_settle_ms");
                else if (trimmed.find("image_probe_pps:") != std::string::npos)
                    cfg.lens_image_probe_pps = (int)parse_u32_config(val, "lens.image_probe_pps", HAL_LENS_FG2009_MAX_PPS);
                else if (trimmed.find("image_probe_ready_timeout_ms:") != std::string::npos)
                    cfg.lens_image_probe_ready_timeout_ms = (int)parse_u32_config(val, "lens.image_probe_ready_timeout_ms");
                else if (trimmed.find("image_probe_retries:") != std::string::npos)
                    cfg.lens_image_probe_retries = (int)parse_u32_config(val, "lens.image_probe_retries");
                else if (trimmed.find("image_probe_retry_interval_ms:") != std::string::npos)
                    cfg.lens_image_probe_retry_interval_ms = (int)parse_u32_config(val, "lens.image_probe_retry_interval_ms");
                else if (trimmed.find("self_init_enabled:") != std::string::npos)
                    cfg.lens_self_init_enabled = (int)parse_u32_config(val, "lens.self_init_enabled");
                else if (trimmed.find("image_probe_move_timeout_ms:") != std::string::npos)
                    cfg.lens_image_probe_move_timeout_ms = (int)parse_u32_config(val, "lens.image_probe_move_timeout_ms");
                else if (trimmed.find("image_probe_texture_floor:") != std::string::npos)
                    cfg.lens_image_probe_texture_floor = parse_u32_config(val, "lens.image_probe_texture_floor");
                else if (trimmed.find("image_probe_motor_ratio:") != std::string::npos)
                    cfg.lens_image_probe_motor_ratio = parse_float_config(val, "lens.image_probe_motor_ratio");
                else if (trimmed.find("image_probe_flat_ratio:") != std::string::npos)
                    cfg.lens_image_probe_flat_ratio = parse_float_config(val, "lens.image_probe_flat_ratio");
                else if (trimmed.find("image_probe_return_ratio:") != std::string::npos)
                    cfg.lens_image_probe_return_ratio = parse_float_config(val, "lens.image_probe_return_ratio");
                else if (trimmed.find("image_probe_luma_guard_ratio:") != std::string::npos)
                    cfg.lens_image_probe_luma_guard_ratio = parse_float_config(val, "lens.image_probe_luma_guard_ratio");
            } else if (lens_subsection == "fg2009") {
                if (trimmed.find("ram_steps:") != std::string::npos)
                    cfg.lens_fg2009.ram_steps = (int32_t)parse_u32_config(val, "lens.fg2009.ram_steps");
                else if (trimmed.find("park_zoom_steps:") != std::string::npos)
                    cfg.lens_fg2009.park_zoom_steps = (int32_t)parse_u32_config(val, "lens.fg2009.park_zoom_steps");
                else if (trimmed.find("park_focus_steps:") != std::string::npos)
                    cfg.lens_fg2009.park_focus_steps = (int32_t)parse_u32_config(val, "lens.fg2009.park_focus_steps");
                else if (trimmed.find("zoom_pps:") != std::string::npos)
                    cfg.lens_fg2009.zoom_pps = (uint16_t)parse_u32_config(val, "lens.fg2009.zoom_pps", HAL_LENS_FG2009_MAX_PPS);
                else if (trimmed.find("focus_pps:") != std::string::npos)
                    cfg.lens_fg2009.focus_pps = (uint16_t)parse_u32_config(val, "lens.fg2009.focus_pps", HAL_LENS_FG2009_MAX_PPS);
                else if (trimmed.find("af_coarse_step:") != std::string::npos)
                    cfg.lens_fg2009_af_coarse_step = (int)parse_u32_config(val, "lens.fg2009.af_coarse_step");
                else if (trimmed.find("af_coarse_span:") != std::string::npos)
                    cfg.lens_fg2009_af_coarse_span = (int)parse_u32_config(val, "lens.fg2009.af_coarse_span");
                else if (trimmed.find("af_coarse_span_low_zoom:") != std::string::npos)
                    cfg.lens_fg2009_af_coarse_span_low_zoom = (int)parse_u32_config(val, "lens.fg2009.af_coarse_span_low_zoom");
                else if (trimmed.find("af_coarse_span_zoom_threshold:") != std::string::npos)
                    cfg.lens_fg2009_af_coarse_span_zoom_threshold = parse_float_config(val, "lens.fg2009.af_coarse_span_zoom_threshold");
                else if (trimmed.find("af_fine_span:") != std::string::npos)
                    cfg.lens_fg2009_af_fine_span = (int)parse_u32_config(val, "lens.fg2009.af_fine_span");
                else if (trimmed.find("af_confidence_accept:") != std::string::npos)
                    cfg.lens_fg2009_af_confidence_accept = parse_float_config(val, "lens.fg2009.af_confidence_accept");
                else if (trimmed.find("af_balanced_retry:") != std::string::npos)
                    cfg.lens_fg2009_af_balanced_retry = (int)parse_u32_config(val, "lens.fg2009.af_balanced_retry");
                else if (trimmed.find("af_boot_oneshot:") != std::string::npos)
                    cfg.lens_fg2009_af_boot_oneshot = (int)parse_u32_config(val, "lens.fg2009.af_boot_oneshot");
                else if (trimmed.find("af_pps:") != std::string::npos)
                    cfg.lens_fg2009_af_pps = (int)parse_u32_config(val, "lens.fg2009.af_pps", HAL_LENS_FG2009_MAX_PPS);
                else if (trimmed.find("af_move_timeout_ms:") != std::string::npos)
                    cfg.lens_fg2009_af_move_timeout_ms = (int)parse_u32_config(val, "lens.fg2009.af_move_timeout_ms");
                else if (trimmed.find("focus_curve_path:") != std::string::npos)
                    cfg.lens_fg2009_focus_curve_path = val;
            }
        }
    }

    // Derive raw stream configs from encoder configs.
    // YAML encoders: is the single source of truth for stream resolution.
    // If no explicit streams: section was provided, auto-generate from encoders.
    if (cfg.streams.empty() && !cfg.encoders.empty()) {
        for (const auto& enc : cfg.encoders) {
            if (!enc.enabled) {
                HAL_LOG_INFO("CameraDaemon: Skipping disabled encoder '%s' at startup",
                             enc.stream_name.c_str());
                continue;
            }
            cfg.streams.push_back({
                enc.stream_name,
                enc.width,
                enc.height,
                enc.fps,
                8,   // pool_max_buffers
                12,  // max_queue_size
            });
        }
    }

    return cfg;
}

static void setup_logging(const std::string& level, const std::string& log_file, const std::string& config_path) {
    int log_level = HAL_LOG_LEVEL_INFO;
    if (level == "debug") log_level = HAL_LOG_LEVEL_DEBUG;
    else if (level == "warning") log_level = HAL_LOG_LEVEL_WARNING;
    else if (level == "error") log_level = HAL_LOG_LEVEL_ERROR;

    hal_log_set_level(log_level);
    hal_log_set_color(1);
    hal_log_set_timestamp(1);

    // Remap /var/log/aipc/<name> → <prefix>/logs/<name>
    // On embedded devices /var/log is tmpfs (lost on reboot), so redirect
    // to the persistent install prefix (e.g. /data/logs/).
    std::string resolved_log_file = log_file;
    const std::string var_log_prefix = "/var/log/aipc/";
    if (log_file.compare(0, var_log_prefix.size(), var_log_prefix) == 0) {
        std::string file_name = log_file.substr(var_log_prefix.size());
        // Derive prefix from the config file path: /opt/aipc/etc/ → /opt/aipc
        // or use /data if config is under /data/etc/
        std::string prefix = "/opt/aipc";
        if (config_path.find("/data/") == 0 || config_path.find("/data\\") == 0)
            prefix = "/data";
        resolved_log_file = prefix + "/logs/" + file_name;
    }

    // Configure log file output if specified
    if (!resolved_log_file.empty()) {
        // Ensure log directory exists
        std::string log_dir = resolved_log_file;
        size_t last_slash = log_dir.find_last_of('/');
        if (last_slash != std::string::npos) {
            log_dir = log_dir.substr(0, last_slash);
            mkdir(log_dir.c_str(), 0755);
        }
        // Enable file logging with rotation (10MB max, 5 files)
        hal_log_set_file(1, resolved_log_file.c_str(), 10 * 1024 * 1024, 5);
        HAL_LOG_INFO("Logging to file: %s", resolved_log_file.c_str());
    }
}

static void print_usage(const char* prog) {
    std::cerr << "Usage: " << prog << " [options]\n"
              << "  -c, --config <path>  Config file (default: /data/aipc/etc/camera-daemon.yaml)\n"
              << "  -h, --help           Show this help\n";
}

int main(int argc, char** argv) {
    std::string config_path = "/data/aipc/etc/camera-daemon.yaml";

    // Parse command line
    static struct option long_opts[] = {
        {"config", required_argument, nullptr, 'c'},
        {"help",   no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "c:h", long_opts, nullptr)) != -1) {
        switch (opt) {
        case 'c': config_path = optarg; break;
        case 'h': print_usage(argv[0]); return 0;
        default:  print_usage(argv[0]); return 1;
        }
    }

    // Load configuration
    DaemonConfig config = load_config(config_path);
    setup_logging(config.log_level, config.log_file, config_path);
    // Lens model first: the IR profile name substitution below must happen
    // before the media-config validation resolves the effective name.
    apply_lens_model_identity(config);
    apply_lens_infrared_profile(config);
    select_product_media_config_for_infrared(config);

    HAL_LOG_INFO("===================================");
    HAL_LOG_INFO("AIPC Camera Daemon v2.0.0");
    HAL_LOG_INFO("===================================");

    // Crash handler (SIGSEGV, SIGABRT, SIGBUS, SIGFPE -> backtrace)
    // Use same prefix derivation as setup_logging for crash dumps
    {
        std::string crash_dir = "/opt/aipc/logs";
        if (config_path.find("/data/") == 0) crash_dir = "/data/logs";
        crash_handler_install("camera-daemon", crash_dir.c_str());
    }

    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Create and initialize daemon
    CameraDaemon daemon;
    g_daemon = &daemon;

    if (!daemon.init(config)) {
        HAL_LOG_ERROR("Failed to initialize camera daemon");
        return 1;
    }

    // init() creates the CameraControl/LensHAL socket only after the media,
    // ISP and encoder pipeline is initialized.  Notify systemd at that real
    // readiness boundary so After=camera-daemon.service has useful semantics.
    notify_systemd_ready();

    // Run (blocks until stop signal)
    daemon.run();

    g_daemon = nullptr;
    HAL_LOG_INFO("Camera daemon exited");
    return 0;
}
