/**
 * @file auto_af_test.cpp
 * @brief Auto-focus demo built on HAL V2 (Hailo-15 MediaLibrary + ISP AF stats + Lens/AF0832 + OSD).
 *
 * Features:
 * - CLI UX copied from test_media_all_func: raw line editing, history, tab completion
 * - ISP AF windows + measurement via HAL_ISP_OPS
 * - Lens control via HAL_MCU_OPS + HAL_LENS_OPS and AF0832 convenience layer
 * - AF: global-sample quadratic fit, coarse+fine expand-from-center scans (early stop from fitted peak),
 *   frame sync via HAL_VIDEO_OPS.subscribe_stream before each ISP sample
 * - AF window + focus-metric curve overlays via HAL_OSD_OPS (curve drawn after af_run_once when af_osd on)
 *
 * Usage:
 *   hal-auto-af-test <medialib_json_path> [--backup <dir>]
 *
 * MCU/lens are initialized from CLI:
 *   mcu_init <serial_dev> [baud] [timeout_ms]
 *   af_create
 */

#include "common/hal_log.h"
#include "common/hal_udp_stream.hpp"
#include "media/hal_codec_internal.h"
#include "media/hal_media.h"
#include "media/hal_video_internal.h"
#include "media/hal_isp.h"
#include "media/hal_osd.h"

extern "C" {
#include "peripheral/hal_mcu.h"
#include "peripheral/devices/hal_lens_af0832.h"
}

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <algorithm>
#include <cmath>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <map>

#include <termios.h>
#include <unistd.h>
#include <strings.h>

namespace
{

constexpr size_t MAX_LINE = 512;
constexpr int HISTORY_MAX = 50;

std::atomic<bool> g_shutdown{false};
volatile sig_atomic_t g_signal_pending = 0;

void on_signal(int)
{
    g_signal_pending = 1;
}

void install_signal_handlers()
{
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGINT, &sa, nullptr);
    (void)sigaction(SIGTERM, &sa, nullptr);
}

void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    (void)std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    HAL_LOG_ERROR("%s", buf);
}

const char *status_str(HalStatus s)
{
    switch (s)
    {
        case HAL_STATUS_UNINITIALIZED:
            return "UNINITIALIZED";
        case HAL_STATUS_INITIALIZED:
            return "INITIALIZED";
        case HAL_STATUS_RUNNING:
            return "RUNNING";
        case HAL_STATUS_STOPPED:
            return "STOPPED";
        case HAL_STATUS_ERROR:
            return "ERROR";
        default:
            return "?";
    }
}

const char *packet_type_str(HalPacketType pt)
{
    switch (pt)
    {
        case HAL_PACKET_TYPE_H264:
            return "H264";
        case HAL_PACKET_TYPE_H265:
            return "H265";
        case HAL_PACKET_TYPE_MJPEG:
            return "MJPEG";
        case HAL_PACKET_TYPE_DATA:
            return "DATA";
        default:
            return "?";
    }
}

/* ------------------------------------------------------------------ */
/* App state                                                          */
/* ------------------------------------------------------------------ */

void *g_media_ctx = nullptr;
void **g_video_list = nullptr;
uint32_t g_video_count = 0;
void **g_codec_list = nullptr;
uint32_t g_codec_count = 0;

static int g_vidx = 0; /* default frontend video index */
static int g_cidx = 0; /* default encoder codec index for OSD */

static void *g_mcu_ctx = nullptr;
static HalLensAf0832 *g_af0832 = nullptr;
static std::mutex g_lens_mu;

static HalUdpStream *g_udp = nullptr;
static int g_udp_codec_index = -1;
static void (*g_udp_cb)(void *, HalPacketBuffer *, void *) = nullptr;

static std::mutex g_af_mu;
static std::condition_variable g_af_cv;
static std::atomic<bool> g_af_thread_exit{false};
static std::atomic<bool> g_af_pause{false};
static std::atomic<bool> g_af_tracking{false};
static std::atomic<bool> g_af_run_once_req{false};
static std::atomic<bool> g_af_stop_req{false};
static std::atomic<int> g_af_state{0}; /* 0 idle, 1 running */
static std::atomic<uint64_t> g_af_heartbeat_ms{0};
static std::atomic<uint64_t> g_af_runs_started{0};

/* AF parameters (tunable via CLI) */
struct AfParams
{
    int min_pos = HAL_LENS_AF0832_DEFAULT_FOCUS_MIN_POS;
    int max_pos = HAL_LENS_AF0832_DEFAULT_FOCUS_MAX_POS;
    int pps = 1200;              /* lens speed: pulses per second (MCU may reject 0) */
    int coarse_span_steps = 256; /* coarse scan span around current focus_pos; 0 => full [min,max] */
    int coarse_step = 32;
    int fine_step = 8;
    int fine_span_steps = 32; /* fine scan around fitted coarse peak: +/- fine_span_steps (focus units) */
    int sync_frames = 1; /* frontend frames to wait after lens move before first ISP sample (settle) */
    int metric_avg_frames = 1; /* after settle, read ISP metric this many frames apart and average (>=1, reduces curve jitter) */
    int max_total_moves = 200; /* maximum number of positions to probe */
    int metric_min_luma = 4096; /* below this treat as invalid (guards against stale/zero stats) */
    double metric_ratio_cap = 0.0; /* cap sum/(luma+1) per window; 0 = no cap (default) */
    int metric_weights[HAL_ISP_AF_MAX_WINDOWS] = {60, 30, 10}; /* sum to 100 */

    /* Global quadratic early-stop: min distinct samples before trusting vertex-based stop */
    int fit_min_samples = 8;
    /* |a| must exceed this for downward-opening parabola (focus metric vs position) */
    double fit_min_curvature = 1e-12;
    /**
     * Before early-stop may trigger, merged sample positions must span at least
     * min(early_stop_min_spread, hi-lo) pulses (hi/lo = current scan interval).
     * Reduces false peaks from stopping after only a few samples. 0 = disable spread guard.
     */
    int early_stop_min_spread = 128;
    /**
     * If merged global-best focus lies outside the first fine scan window, run a second fine scan
     * centered on that global peak and merge samples (0 = disable).
     */
    int fine_rescan_if_global_outside = 1;
    /**
     * After picking candidates from the merged curve, re-measure at each focus (highest curve m first).
     * Accept first position where measured m >= peak_verify_min_frac * curve sample m at that position.
     * 0 = off (only visit global argmax once, no smpl/now ratio test). Typical: 0.5–0.65 for rejecting
     * one-frame spikes that do not reproduce after settle.
     */
    double peak_verify_min_frac = 0.55;
    /** Max distinct ranked-by-m candidates to try when peak_verify_min_frac > 0 (includes first). */
    int peak_verify_max_tries = 8;
};
static AfParams g_afp{};

struct AfFocusSample
{
    int pos{};
    double m{};
};

/* Current AF windows (pixels in video stream coordinates) */
static HalIspAfWindowsConfig g_af_windows{};

/* OSD window visualization */
static bool g_af_osd_enabled = false;
static int g_af_osd_thickness = 3;
static struct
{
    uint8_t a = 255;
    uint8_t r = 0;
    uint8_t g = 255;
    uint8_t b = 0;
} g_af_osd_color;

static std::vector<uint8_t> g_osd_buf[HAL_ISP_AF_MAX_WINDOWS];
static bool g_osd_added[HAL_ISP_AF_MAX_WINDOWS] = {false, false, false};
static const char *k_osd_ids[HAL_ISP_AF_MAX_WINDOWS] = {"af_win1", "af_win2", "af_win3"};

/* AF focus-metric curve overlay (encoder plane) */
static std::vector<uint8_t> g_osd_curve_buf;
static bool g_osd_curve_added = false;
static const char *k_osd_curve_id = "af_curve";

static std::mutex g_af_vsync_mu;
static std::condition_variable g_af_vsync_cv;
static uint64_t g_af_vsync_count = 0;
static bool g_af_vsync_active = false;

static int refresh_stream_lists()
{
    if (!g_media_ctx)
    {
        return HAL_ERR_INVALID_STATE;
    }
    void *vl = nullptr;
    uint32_t vc = 0;
    void *cl = nullptr;
    uint32_t cc = 0;
    int r1 = HAL_MEDIA_OPS.get_video_list(g_media_ctx, &vl, &vc);
    int r2 = HAL_MEDIA_OPS.get_codec_list(g_media_ctx, &cl, &cc);
    if (r1 != HAL_OK)
    {
        return r1;
    }
    if (r2 != HAL_OK)
    {
        return r2;
    }
    g_video_list = reinterpret_cast<void **>(vl);
    g_video_count = vc;
    g_codec_list = reinterpret_cast<void **>(cl);
    g_codec_count = cc;
    return HAL_OK;
}

static HalVideoContext *video_at(int idx)
{
    if (!g_video_list || idx < 0 || static_cast<uint32_t>(idx) >= g_video_count)
    {
        return nullptr;
    }
    return static_cast<HalVideoContext *>(g_video_list[idx]);
}

static HalCodecContext *codec_at(int idx)
{
    if (!g_codec_list || idx < 0 || static_cast<uint32_t>(idx) >= g_codec_count)
    {
        return nullptr;
    }
    return static_cast<HalCodecContext *>(g_codec_list[idx]);
}

static void print_streams()
{
    std::printf("--- Frontend (video) streams: %u ---\n", static_cast<unsigned>(g_video_count));
    for (uint32_t i = 0; i < g_video_count; i++)
    {
        auto *vc = static_cast<HalVideoContext *>(g_video_list[i]);
        if (!vc)
        {
            std::printf("  [%u] (null)\n", static_cast<unsigned>(i));
            continue;
        }
        HalVideoConfig cfg{};
        if (HAL_VIDEO_OPS.get_current_config(vc, &cfg) == HAL_OK)
        {
            const HalStatus vst = HAL_VIDEO_OPS.get_status(vc);
            std::printf("  [%u] name=%s %ux%u @%ufps fmt=%s pool_max=%u status=%s\n", static_cast<unsigned>(i),
                        vc->video_name, cfg.width, cfg.height, cfg.framerate, hal_pixel_format_to_string(cfg.format),
                        cfg.pool_max_buffers, status_str(vst));
        }
        else
        {
            std::printf("  [%u] name=%s (get_current_config failed)\n", static_cast<unsigned>(i), vc->video_name);
        }
    }
    std::printf("--- Encoder (codec) streams: %u ---\n", static_cast<unsigned>(g_codec_count));
    for (uint32_t i = 0; i < g_codec_count; i++)
    {
        auto *cc = static_cast<HalCodecContext *>(g_codec_list[i]);
        if (!cc)
        {
            std::printf("  [%u] (null)\n", static_cast<unsigned>(i));
            continue;
        }
        const HalStatus cst = static_cast<HalStatus>(HAL_CODEC_OPS.get_status(cc));
        std::printf("  [%u] stream_id=%s %ux%u @%ufps pkt=%s status=%s\n", static_cast<unsigned>(i), cc->codec_name,
                    cc->config.width, cc->config.height, cc->config.framerate, packet_type_str(cc->config.packet_type),
                    status_str(cst));
    }
    std::fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* CLI                                                                 */
/* ------------------------------------------------------------------ */

static const char *k_commands[] = {
    "help",
    "quit",
    "exit",
    "streams",
    "versions",
    "vidx",
    "cidx",
    "isp_af_set",
    "isp_af_get",
    "isp_af_meas",
    "af_param_get",
    "af_param_set",
    "af_run_once",
    "af_stop",
    "af_track",
    "af_status",
    "mcu_init",
    "mcu_deinit",
    "af_create",
    "af_destroy",
    "zoom_goto",
    "af_osd",
    "af_osd_style",
    "udp_start",
    "udp_stop",
    "udp_select",
    "udp_push",
    "udp_stats",
};
static const size_t k_commands_count = sizeof(k_commands) / sizeof(k_commands[0]);

typedef struct
{
    char items[HISTORY_MAX][MAX_LINE];
    int count;
} CliHistory;

static void history_add(CliHistory *h, const char *line)
{
    if (!h || !line || line[0] == '\0')
    {
        return;
    }
    if (h->count > 0 && std::strcmp(h->items[h->count - 1], line) == 0)
    {
        return;
    }
    if (h->count < HISTORY_MAX)
    {
        std::snprintf(h->items[h->count], MAX_LINE, "%s", line);
        h->count++;
        return;
    }
    for (int i = 1; i < HISTORY_MAX; ++i)
    {
        std::snprintf(h->items[i - 1], MAX_LINE, "%s", h->items[i]);
    }
    std::snprintf(h->items[HISTORY_MAX - 1], MAX_LINE, "%s", line);
}

static void print_prompt_with_cursor(const char *line, size_t cursor)
{
    size_t len = std::strlen(line);
    std::printf("\raf> %s", line);
    std::printf("\x1b[K");
    if (cursor < len)
    {
        std::printf("\x1b[%zuD", len - cursor);
    }
    std::fflush(stdout);
}

static void redraw_prompt_after_aux_output(const char *line, size_t cursor)
{
    std::printf("\r");
    print_prompt_with_cursor(line, cursor);
}

static void complete_command(char *line, size_t cap)
{
    if (std::strchr(line, ' ') != NULL)
    {
        return;
    }

    size_t plen = std::strlen(line);
    const char *single = NULL;
    size_t match_count = 0;
    for (size_t i = 0; i < k_commands_count; ++i)
    {
        if (std::strncmp(k_commands[i], line, plen) == 0)
        {
            match_count++;
            single = k_commands[i];
        }
    }

    if (match_count == 1 && single != NULL)
    {
        std::snprintf(line, cap, "%s", single);
        return;
    }

    if (match_count > 1)
    {
        std::printf("\n");
        for (size_t i = 0; i < k_commands_count; ++i)
        {
            if (std::strncmp(k_commands[i], line, plen) == 0)
            {
                std::printf("  %s\n", k_commands[i]);
            }
        }
        redraw_prompt_after_aux_output(line, std::strlen(line));
    }
}

static int read_line_raw(char *line, size_t cap, CliHistory *history)
{
    struct termios oldt;
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &oldt) != 0)
    {
        return -1;
    }
    raw = oldt;
    raw.c_iflag &= (tcflag_t) ~(IXON | IXOFF);
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
    {
        return -1;
    }

    size_t len = 0;
    size_t cursor = 0;
    int hist_pos = (history != nullptr) ? history->count : 0;
    line[0] = '\0';
    print_prompt_with_cursor(line, cursor);

    while (1)
    {
        unsigned char c = 0;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0)
        {
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            return -1;
        }

        if (c == '\r' || c == '\n')
        {
            break;
        }

        if (c == 0x7F || c == '\b')
        {
            if (cursor > 0 && len > 0)
            {
                std::memmove(&line[cursor - 1], &line[cursor], len - cursor + 1);
                cursor--;
                len--;
                print_prompt_with_cursor(line, cursor);
            }
            continue;
        }

        if (c == '\t')
        {
            complete_command(line, cap);
            len = std::strlen(line);
            cursor = len;
            print_prompt_with_cursor(line, cursor);
            continue;
        }

        if (c == 0x1B)
        {
            unsigned char b1 = 0;
            if (read(STDIN_FILENO, &b1, 1) <= 0)
            {
                continue;
            }
            if (b1 == '[')
            {
                unsigned char seq[24];
                size_t sn = 0;
                while (sn + 1 < sizeof(seq))
                {
                    if (read(STDIN_FILENO, &seq[sn], 1) <= 0)
                    {
                        sn = 0;
                        break;
                    }
                    sn++;
                    if (seq[sn - 1] >= 0x40 && seq[sn - 1] <= 0x7E)
                    {
                        break;
                    }
                }
                if (sn == 0)
                {
                    continue;
                }
                const unsigned char fin = seq[sn - 1];
                if (history != nullptr)
                {
                    if (fin == 'A')
                    {
                        if (history->count > 0 && hist_pos > 0)
                        {
                            hist_pos--;
                            std::snprintf(line, cap, "%s", history->items[hist_pos]);
                            len = std::strlen(line);
                            cursor = len;
                            print_prompt_with_cursor(line, cursor);
                        }
                    }
                    else if (fin == 'B')
                    {
                        if (history->count > 0 && hist_pos < history->count - 1)
                        {
                            hist_pos++;
                            std::snprintf(line, cap, "%s", history->items[hist_pos]);
                            len = std::strlen(line);
                            cursor = len;
                            print_prompt_with_cursor(line, cursor);
                        }
                        else
                        {
                            hist_pos = history->count;
                            line[0] = '\0';
                            len = 0;
                            cursor = 0;
                            print_prompt_with_cursor(line, cursor);
                        }
                    }
                    else if (fin == 'C' && cursor < len)
                    {
                        cursor++;
                        print_prompt_with_cursor(line, cursor);
                    }
                    else if (fin == 'D' && cursor > 0)
                    {
                        cursor--;
                        print_prompt_with_cursor(line, cursor);
                    }
                }
                continue;
            }
            if (b1 == 'O' && history != nullptr)
            {
                unsigned char b2 = 0;
                if (read(STDIN_FILENO, &b2, 1) <= 0)
                {
                    continue;
                }
                if (b2 == 'A' && history->count > 0 && hist_pos > 0)
                {
                    hist_pos--;
                    std::snprintf(line, cap, "%s", history->items[hist_pos]);
                    len = std::strlen(line);
                    cursor = len;
                    print_prompt_with_cursor(line, cursor);
                }
                else if (b2 == 'B')
                {
                    if (history->count > 0 && hist_pos < history->count - 1)
                    {
                        hist_pos++;
                        std::snprintf(line, cap, "%s", history->items[hist_pos]);
                        len = std::strlen(line);
                        cursor = len;
                        print_prompt_with_cursor(line, cursor);
                    }
                    else
                    {
                        hist_pos = history->count;
                        line[0] = '\0';
                        len = 0;
                        cursor = 0;
                        print_prompt_with_cursor(line, cursor);
                    }
                }
                else if (b2 == 'C' && cursor < len)
                {
                    cursor++;
                    print_prompt_with_cursor(line, cursor);
                }
                else if (b2 == 'D' && cursor > 0)
                {
                    cursor--;
                    print_prompt_with_cursor(line, cursor);
                }
                continue;
            }
            continue;
        }

        if (std::isprint(c) != 0 && len + 1 < cap)
        {
            std::memmove(&line[cursor + 1], &line[cursor], len - cursor + 1);
            line[cursor] = (char)c;
            len++;
            cursor++;
            print_prompt_with_cursor(line, cursor);
        }
    }

    (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::printf("\n");
    std::fflush(stdout);
    return 0;
}

static void print_help()
{
    std::printf(
        "Commands:\n"
        "  help | quit | exit\n"
        "  streams                     # list video/codec contexts\n"
        "  versions                    # print HAL versions\n"
        "  vidx <n>                    # select default video idx (AF stats)\n"
        "  cidx <n>                    # select default codec idx (OSD overlays)\n"
        "  # ISP AF statistics:\n"
        "  isp_af_set <vidx> <0|1> <x1> <y1> <w1> <h1> [x2 y2 w2 h2] [x3 y3 w3 h3]\n"
        "  isp_af_get <vidx>\n"
        "  isp_af_meas <vidx>\n"
        "  # MCU/Lens:\n"
        "  mcu_init <dev> [baud] [timeout_ms]\n"
        "  mcu_deinit\n"
        "  af_create                   # create+bootstrap AF0832 lens helper\n"
        "  af_destroy\n"
        "  zoom_goto <ratio> <distance_m>\n"
        "  # AF control:\n"
        "  af_param_get\n"
        "  af_param_set <name> <value> # ... fit_min_curvature fine_rescan_if_global_outside peak_verify_min_frac "
        "peak_verify_max_tries (see af_param_get)\n"
        "  af_run_once\n"
        "  af_stop\n"
        "  af_track <0|1>\n"
        "  af_status\n"
        "  # OSD window visualization:\n"
        "  af_osd <0|1>\n"
        "  af_osd_style <thickness_px> <a> <r> <g> <b>\n"
        "  # RTP UDP push (codec index = encoder row in \"streams\"):\n"
        "  udp_start [host] [port]\n"
        "  udp_stop\n"
        "  udp_select|udp_push <codec_idx> [host] [port]\n"
        "  udp_stats\n");
    std::fflush(stdout);
}

/* ------------------------------------------------------------------ */
/* UDP stats (copied from test_media_all_func, simplified)             */
/* ------------------------------------------------------------------ */

struct CodecUdpStats
{
    uint64_t pkt_count{0};
    uint64_t ts_not_strictly_increasing{0};
    uint64_t last_ts_ns{0};
    bool have_ts{false};

    uint64_t idr_seen{0};
    bool seen_idr{false};
    uint64_t last_idr_ts_ns{0};
    uint64_t pkts_since_idr{0};

    uint64_t idr_interval_count{0};
    uint64_t idr_interval_sum_pkts{0};
    uint64_t idr_interval_min_pkts{(uint64_t)-1};
    uint64_t idr_interval_max_pkts{0};
    uint64_t idr_interval_sum_ns{0};
    uint64_t idr_interval_min_ns{(uint64_t)-1};
    uint64_t idr_interval_max_ns{0};
};

static std::mutex g_codec_udp_stats_mu;
static CodecUdpStats g_codec_udp_stats{};

static void reset_codec_udp_stats()
{
    std::lock_guard<std::mutex> lock(g_codec_udp_stats_mu);
    g_codec_udp_stats = CodecUdpStats{};
}

static bool annex_b_h264_keyframe(const uint8_t *data, uint32_t size)
{
    if (!data || size < 5)
    {
        return false;
    }
    for (uint32_t i = 0; i + 4 < size; i++)
    {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            uint8_t nal = static_cast<uint8_t>(data[i + 3] & 0x1f);
            if (nal == 5 || nal == 7)
            {
                return true;
            }
        }
        if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
        {
            uint8_t nal = static_cast<uint8_t>(data[i + 4] & 0x1f);
            if (nal == 5 || nal == 7)
            {
                return true;
            }
        }
    }
    return false;
}

static bool annex_b_h265_keyframe(const uint8_t *data, uint32_t size)
{
    if (!data || size < 6)
    {
        return false;
    }
    for (uint32_t i = 0; i + 4 < size; i++)
    {
        uint32_t sc = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            sc = 3;
        }
        else if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
        {
            sc = 4;
        }
        else
        {
            continue;
        }
        const uint32_t off = i + sc;
        if (off >= size)
        {
            continue;
        }
        const uint8_t nal_type = static_cast<uint8_t>((data[off] >> 1) & 0x3f);
        if (nal_type == 19 || nal_type == 20)
        {
            return true;
        }
    }
    return false;
}

static void codec_udp_cb(void *codec_ctx, HalPacketBuffer *pkt, void *userdata)
{
    if (!codec_ctx || !pkt || !userdata)
    {
        return;
    }
    auto *stream = static_cast<HalUdpStream *>(userdata);

    /* Stats */
    bool is_idr = false;
    {
        auto *cc = static_cast<HalCodecContext *>(codec_ctx);
        if (cc && (cc->config.packet_type == HAL_PACKET_TYPE_H264))
        {
            is_idr = annex_b_h264_keyframe(pkt->data, pkt->size);
        }
        else if (cc && (cc->config.packet_type == HAL_PACKET_TYPE_H265))
        {
            is_idr = annex_b_h265_keyframe(pkt->data, pkt->size);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_codec_udp_stats_mu);
        g_codec_udp_stats.pkt_count++;
        if (g_codec_udp_stats.have_ts && pkt->timestamp_ns <= g_codec_udp_stats.last_ts_ns)
        {
            g_codec_udp_stats.ts_not_strictly_increasing++;
        }
        g_codec_udp_stats.last_ts_ns = pkt->timestamp_ns;
        g_codec_udp_stats.have_ts = true;

        if (is_idr)
        {
            g_codec_udp_stats.idr_seen++;
            if (g_codec_udp_stats.seen_idr)
            {
                const uint64_t gap_pkts = g_codec_udp_stats.pkts_since_idr + 1U;
                uint64_t gap_ns = 0;
                if (pkt->timestamp_ns > g_codec_udp_stats.last_idr_ts_ns)
                {
                    gap_ns = pkt->timestamp_ns - g_codec_udp_stats.last_idr_ts_ns;
                }
                g_codec_udp_stats.idr_interval_count++;
                g_codec_udp_stats.idr_interval_sum_pkts += gap_pkts;
                g_codec_udp_stats.idr_interval_min_pkts = std::min(g_codec_udp_stats.idr_interval_min_pkts, gap_pkts);
                g_codec_udp_stats.idr_interval_max_pkts = std::max(g_codec_udp_stats.idr_interval_max_pkts, gap_pkts);
                g_codec_udp_stats.idr_interval_sum_ns += gap_ns;
                g_codec_udp_stats.idr_interval_min_ns = std::min(g_codec_udp_stats.idr_interval_min_ns, gap_ns);
                g_codec_udp_stats.idr_interval_max_ns = std::max(g_codec_udp_stats.idr_interval_max_ns, gap_ns);
            }
            g_codec_udp_stats.seen_idr = true;
            g_codec_udp_stats.last_idr_ts_ns = pkt->timestamp_ns;
            g_codec_udp_stats.pkts_since_idr = 0;
        }
        else
        {
            g_codec_udp_stats.pkts_since_idr++;
        }
    }

    stream->push_annex_b(pkt->data, pkt->size, pkt->timestamp_ns);
    (void)HAL_CODEC_OPS.release_packet(codec_ctx, pkt);
}

static void udp_stop_internal()
{
    if (g_udp_codec_index >= 0 && g_codec_list && static_cast<uint32_t>(g_udp_codec_index) < g_codec_count && g_udp_cb)
    {
        void *ctx = g_codec_list[g_udp_codec_index];
        if (ctx)
        {
            (void)HAL_CODEC_OPS.unsubscribe(ctx, g_udp_cb);
        }
    }
    g_udp_cb = nullptr;
    g_udp_codec_index = -1;
    if (g_udp)
    {
        g_udp->stop_accepting();
        g_udp->shutdown();
        delete g_udp;
        g_udp = nullptr;
    }
    reset_codec_udp_stats();
}

static int udp_start_for_index(int idx, const char *host, uint16_t port)
{
    if (!g_codec_list || idx < 0 || static_cast<uint32_t>(idx) >= g_codec_count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    udp_stop_internal();

    auto *cc = static_cast<HalCodecContext *>(g_codec_list[idx]);
    if (!cc)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (cc->config.packet_type != HAL_PACKET_TYPE_H264 && cc->config.packet_type != HAL_PACKET_TYPE_H265)
    {
        return HAL_ERR_INVALID_FMT;
    }

    HalUdpStreamConfig us{};
    us.host = host;
    us.port = port;
    if (cc->config.packet_type == HAL_PACKET_TYPE_H265)
    {
        us.mode = HalUdpStreamMode::RtpH265AnnexB;
        us.rtp_payload_type = 97U;
    }
    else
    {
        us.mode = HalUdpStreamMode::RtpH264AnnexB;
        us.rtp_payload_type = 96U;
    }

    g_udp = new HalUdpStream(us);
    if (!g_udp->ok())
    {
        delete g_udp;
        g_udp = nullptr;
        return HAL_ERROR;
    }

    g_udp_cb = codec_udp_cb;
    int rc = HAL_CODEC_OPS.subscribe(cc, g_udp_cb, g_udp);
    if (rc != HAL_OK)
    {
        g_udp->shutdown();
        delete g_udp;
        g_udp = nullptr;
        g_udp_cb = nullptr;
        return rc;
    }

    g_udp_codec_index = idx;
    return HAL_OK;
}

/* ------------------------------------------------------------------ */
/* AF metric + helpers                                                 */
/* ------------------------------------------------------------------ */

/**
 * Hailo-15 ISP: @c sum is focus/clarity energy (peaks at best focus); @c luma is mean brightness in the window.
 * This demo combines weighted sum/(luma+1) and treats **larger** scores as sharper — consistent with
 * hill-climb / peak search on the focus curve (see HalIspAfMeasurement in hal_isp.h).
 */
static double metric_from_meas(const HalIspAfMeasurement &m)
{
    double total = 0.0;
    for (uint32_t i = 0; i < m.window_count && i < HAL_ISP_AF_MAX_WINDOWS; ++i)
    {
        const uint32_t l = m.luma[i];
        if (l < (uint32_t)g_afp.metric_min_luma)
        {
            continue;
        }
        double mi = (double)m.sum[i] / (double)(l + 1u);
        if (g_afp.metric_ratio_cap > 0.0 && mi > g_afp.metric_ratio_cap)
        {
            mi = g_afp.metric_ratio_cap;
        }
        total += mi * ((double)g_afp.metric_weights[i] / 100.0);
    }
    return total;
}

static void sleep_frames_estimate(int vidx, int frames)
{
    if (frames <= 0)
    {
        return;
    }
    HalVideoContext *vc = video_at(vidx);
    uint32_t fps = 30;
    if (vc)
    {
        HalVideoConfig cfg{};
        if (HAL_VIDEO_OPS.get_current_config(vc, &cfg) == HAL_OK && cfg.framerate > 0)
        {
            fps = cfg.framerate;
        }
    }
    const int ms = (int)((1000.0 * (double)frames) / (double)std::max(1u, fps));
    std::this_thread::sleep_for(std::chrono::milliseconds(std::max(1, ms)));
}

static void af_vsync_frame_cb(void *video_ctx, HalFrameBuffer *frame, void *userdata)
{
    (void)userdata;
    {
        std::lock_guard<std::mutex> lk(g_af_vsync_mu);
        g_af_vsync_count++;
    }
    g_af_vsync_cv.notify_all();
    if (HAL_VIDEO_OPS.release_frame && video_ctx && frame)
    {
        (void)HAL_VIDEO_OPS.release_frame(video_ctx, frame);
    }
}

static int af_vsync_subscribe(int vidx)
{
    HalVideoContext *vc = video_at(vidx);
    if (!vc || !HAL_VIDEO_OPS.subscribe_stream)
    {
        g_af_vsync_active = false;
        return HAL_ERR_NOT_SUPPORTED;
    }
    const int r = HAL_VIDEO_OPS.subscribe_stream(vc, vc->video_name, af_vsync_frame_cb, nullptr);
    g_af_vsync_active = (r == HAL_OK);
    return r;
}

static void af_vsync_unsubscribe(int vidx)
{
    HalVideoContext *vc = video_at(vidx);
    if (!vc || !HAL_VIDEO_OPS.unsubscribe_stream)
    {
        g_af_vsync_active = false;
        return;
    }
    (void)HAL_VIDEO_OPS.unsubscribe_stream(vc, vc->video_name);
    g_af_vsync_active = false;
}

static void wait_post_move_frames(int vidx, int frames)
{
    const int n = std::max(1, frames);
    if (g_af_vsync_active)
    {
        std::unique_lock<std::mutex> lk(g_af_vsync_mu);
        const uint64_t target = g_af_vsync_count + (uint64_t)n;
        (void)g_af_vsync_cv.wait_for(lk, std::chrono::milliseconds(900),
                                      [&] { return g_af_vsync_count >= target; });
        lk.unlock();
    }
    if (!g_af_vsync_active)
    {
        sleep_frames_estimate(vidx, n);
    }
}

static int read_metric_isp(int vidx, double *out_metric)
{
    if (!out_metric)
    {
        return HAL_ERR_INVALID_ARG;
    }
    HalVideoContext *vc = video_at(vidx);
    if (!vc || !HAL_ISP_OPS.get_af_measurement)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    HalIspAfMeasurement mm{};
    const int r = HAL_ISP_OPS.get_af_measurement(vc, &mm);
    if (r != HAL_OK)
    {
        return r;
    }
    if (mm.window_count == 0)
    {
        return HAL_ERR_INVALID_STATE;
    }
    *out_metric = metric_from_meas(mm);
    return HAL_OK;
}

/** After lens is already still: wait @c sync_frames, then sample @c metric_avg_frames consecutive frames (one read per frame) and return the mean. */
static int read_metric_isp_averaged_after_still(int vidx, double *out_metric)
{
    if (!out_metric)
    {
        return HAL_ERR_INVALID_ARG;
    }
    wait_post_move_frames(vidx, std::max(1, g_afp.sync_frames));
    const int n = std::max(1, g_afp.metric_avg_frames);
    double acc = 0.0;
    for (int i = 0; i < n; i++)
    {
        double m = 0.0;
        const int r = read_metric_isp(vidx, &m);
        if (r != HAL_OK)
        {
            return r;
        }
        acc += m;
        if (i + 1 < n)
        {
            wait_post_move_frames(vidx, 1);
        }
    }
    *out_metric = acc / (double)n;
    return HAL_OK;
}

static void merge_af_samples_for_fit(const std::vector<AfFocusSample> &in, std::vector<double> &xs,
                                     std::vector<double> &ys)
{
    std::map<int, std::pair<double, int>> acc;
    for (const auto &s : in)
    {
        auto &t = acc[s.pos];
        t.first += s.m;
        t.second++;
    }
    xs.clear();
    ys.clear();
    for (const auto &kv : acc)
    {
        xs.push_back((double)kv.first);
        ys.push_back(kv.second.first / (double)kv.second.second);
    }
}

static bool solve_linear_gauss3(double M[3][4])
{
    for (int col = 0; col < 3; col++)
    {
        int piv = col;
        for (int r = col; r < 3; r++)
        {
            if (std::fabs(M[r][col]) > std::fabs(M[piv][col]))
            {
                piv = r;
            }
        }
        if (std::fabs(M[piv][col]) < 1e-18)
        {
            return false;
        }
        if (piv != col)
        {
            for (int c = col; c < 4; c++)
            {
                std::swap(M[piv][c], M[col][c]);
            }
        }
        const double div = M[col][col];
        for (int c = col; c < 4; c++)
        {
            M[col][c] /= div;
        }
        for (int r = 0; r < 3; r++)
        {
            if (r == col)
            {
                continue;
            }
            const double f = M[r][col];
            for (int c = col; c < 4; c++)
            {
                M[r][c] -= f * M[col][c];
            }
        }
    }
    return true;
}

static bool quadratic_fit_xy(const std::vector<double> &x, const std::vector<double> &y, double min_curvature,
                             double *out_a, double *out_b, double *out_c, double *out_xv, double *out_yv)
{
    const size_t n = x.size();
    if (n < 3 || y.size() != n || !out_a || !out_b || !out_c || !out_xv || !out_yv)
    {
        return false;
    }
    double p4 = 0.0, p3 = 0.0, p2 = 0.0, p1 = 0.0;
    const double p0 = (double)n;
    double q2 = 0.0, q1 = 0.0, q0 = 0.0;
    for (size_t i = 0; i < n; i++)
    {
        const double xi = x[i];
        const double yi = y[i];
        const double x2 = xi * xi;
        const double x3 = x2 * xi;
        const double x4 = x2 * x2;
        p1 += xi;
        p2 += x2;
        p3 += x3;
        p4 += x4;
        q0 += yi;
        q1 += yi * xi;
        q2 += yi * x2;
    }
    double A[3][4] = {
        {p4, p3, p2, q2},
        {p3, p2, p1, q1},
        {p2, p1, p0, q0},
    };
    if (!solve_linear_gauss3(A))
    {
        return false;
    }
    const double a = A[0][3];
    const double b = A[1][3];
    const double c = A[2][3];
    if (a >= -min_curvature)
    {
        return false;
    }
    const double xv = -b / (2.0 * a);
    const double yv = a * xv * xv + b * xv + c;
    if (!std::isfinite(xv) || !std::isfinite(yv))
    {
        return false;
    }
    *out_a = a;
    *out_b = b;
    *out_c = c;
    *out_xv = xv;
    *out_yv = yv;
    return true;
}

static bool af_fit_stop_extend_right(const std::vector<AfFocusSample> &all, int step, int min_fit_n,
                                     double min_curv, int lo, int hi)
{
    std::vector<double> xs;
    std::vector<double> ys;
    merge_af_samples_for_fit(all, xs, ys);
    if (xs.size() < (size_t)std::max(3, min_fit_n))
    {
        return false;
    }
    if (g_afp.early_stop_min_spread > 0 && hi > lo)
    {
        const int spread = (int)std::lround(xs.back() - xs.front());
        const int span = hi - lo;
        const int need = std::min(g_afp.early_stop_min_spread, span);
        if (spread < need)
        {
            return false;
        }
    }
    double a = 0.0, b = 0.0, c = 0.0, xv = 0.0, yv = 0.0;
    if (!quadratic_fit_xy(xs, ys, min_curv, &a, &b, &c, &xv, &yv))
    {
        return false;
    }
    const double xmax = xs.back();
    return xmax + 1e-6 >= xv + 0.5 * (double)std::max(1, step);
}

static bool af_fit_stop_extend_left(const std::vector<AfFocusSample> &all, int step, int min_fit_n, double min_curv,
                                    int lo, int hi)
{
    std::vector<double> xs;
    std::vector<double> ys;
    merge_af_samples_for_fit(all, xs, ys);
    if (xs.size() < (size_t)std::max(3, min_fit_n))
    {
        return false;
    }
    if (g_afp.early_stop_min_spread > 0 && hi > lo)
    {
        const int spread = (int)std::lround(xs.back() - xs.front());
        const int span = hi - lo;
        const int need = std::min(g_afp.early_stop_min_spread, span);
        if (spread < need)
        {
            return false;
        }
    }
    double a = 0.0, bb = 0.0, c = 0.0, xv = 0.0, yv = 0.0;
    if (!quadratic_fit_xy(xs, ys, min_curv, &a, &bb, &c, &xv, &yv))
    {
        return false;
    }
    const double xmin = xs.front();
    return xmin - 1e-6 <= xv - 0.5 * (double)std::max(1, step);
}

static double af_fitted_peak_x(const std::vector<AfFocusSample> &curve, int min_fit_n, double min_curv)
{
    std::vector<double> xs;
    std::vector<double> ys;
    merge_af_samples_for_fit(curve, xs, ys);
    double a = 0.0, b = 0.0, c = 0.0, xv = 0.0, yv = 0.0;
    if (xs.size() >= (size_t)std::max(3, min_fit_n) && quadratic_fit_xy(xs, ys, min_curv, &a, &b, &c, &xv, &yv))
    {
        return xv;
    }
    if (curve.empty())
    {
        return 0.0;
    }
    int bp = curve[0].pos;
    double bm = curve[0].m;
    for (const auto &s : curve)
    {
        if (s.m > bm)
        {
            bm = s.m;
            bp = s.pos;
        }
    }
    return (double)bp;
}

static void af_curve_merge_plot(const std::vector<AfFocusSample> &a, const std::vector<AfFocusSample> &b,
                                std::vector<AfFocusSample> *out_sorted)
{
    if (!out_sorted)
    {
        return;
    }
    std::map<int, double> m;
    for (const auto &s : a)
    {
        m[s.pos] = s.m;
    }
    for (const auto &s : b)
    {
        m[s.pos] = s.m;
    }
    out_sorted->clear();
    out_sorted->reserve(m.size());
    for (const auto &kv : m)
    {
        out_sorted->push_back(AfFocusSample{kv.first, kv.second});
    }
}

static void af_curve_global_discrete_max(const std::vector<AfFocusSample> &curve, int *out_pos, double *out_m)
{
    if (!out_pos || !out_m || curve.empty())
    {
        return;
    }
    int bp = curve[0].pos;
    double bm = curve[0].m;
    for (const auto &s : curve)
    {
        if (s.m > bm)
        {
            bm = s.m;
            bp = s.pos;
        }
    }
    *out_pos = bp;
    *out_m = bm;
}

static double af_curve_m_at_pos(const std::vector<AfFocusSample> &curve, int pos)
{
    for (const auto &s : curve)
    {
        if (s.pos == pos)
        {
            return s.m;
        }
    }
    return 0.0;
}

static int af_set_focus_and_measure(int vidx, int pos, double *out_metric);

static int af_verify_focus_and_measure(int vidx, int pos, int *move_count, double *out_m, const char *phase)
{
    if (!out_m || !move_count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    const char *ph = phase ? phase : "peak_verify";
    if (g_af_stop_req.load(std::memory_order_acquire))
    {
        std::printf("af_%s: aborted at pos=%d: af_stop requested\n", ph, pos);
        std::fflush(stdout);
        return HAL_ERR_INVALID_STATE;
    }
    if (g_af_pause.load(std::memory_order_acquire))
    {
        std::printf("af_%s: aborted at pos=%d: af_pause\n", ph, pos);
        std::fflush(stdout);
        return HAL_ERR_INVALID_STATE;
    }
    if (++(*move_count) > g_afp.max_total_moves)
    {
        std::printf("af_%s: aborted at pos=%d: max_total_moves=%d exceeded\n", ph, pos, g_afp.max_total_moves);
        std::fflush(stdout);
        return HAL_ERR_INVALID_STATE;
    }
    return af_set_focus_and_measure(vidx, pos, out_m);
}

/* ------------------------------------------------------------------ */
/* OSD: generate ARGB border buffer for a window                        */
/* ------------------------------------------------------------------ */

#include "auto_af_test_glcdfont.inc"

static void set_px_argb(uint8_t *dst, int w, int h, int x, int y, uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    if (!dst || x < 0 || y < 0 || x >= w || y >= h)
    {
        return;
    }
    uint8_t *p = dst + (size_t)y * (size_t)w * 4u + (size_t)x * 4u;
    p[0] = a;
    p[1] = r;
    p[2] = g;
    p[3] = b;
}

static void draw_char_argb_glcd(uint8_t *dst, int w, int h, int x, int y, char ch, uint8_t a, uint8_t r, uint8_t g,
                               uint8_t b, int scale)
{
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (uch < 32 || uch > 127)
    {
        return;
    }
    const uint8_t *glyph = kAfOsdFont5x7[uch - 32];
    scale = std::max(1, scale);
    for (int row = 0; row < 7; row++)
    {
        const uint8_t bits = glyph[row];
        for (int col = 0; col < 5; col++)
        {
            if ((bits & (uint8_t)(1u << (5 - col))) != 0)
            {
                for (int sy = 0; sy < scale; sy++)
                {
                    for (int sx = 0; sx < scale; sx++)
                    {
                        set_px_argb(dst, w, h, x + col * scale + sx, y + row * scale + sy, a, r, g, b);
                    }
                }
            }
        }
    }
}

static void draw_string_argb_glcd(uint8_t *dst, int w, int h, int x, int y, const char *s, uint8_t a, uint8_t r,
                                  uint8_t g, uint8_t b, int scale)
{
    if (!s)
    {
        return;
    }
    int cx = x;
    const int adv = 6 * std::max(1, scale);
    for (; *s != '\0'; ++s)
    {
        if (*s == '\n')
        {
            cx = x;
            y += 8 * std::max(1, scale);
            continue;
        }
        draw_char_argb_glcd(dst, w, h, cx, y, *s, a, r, g, b, scale);
        cx += adv;
    }
}

static void draw_circle_outline_argb(uint8_t *dst, int w, int h, int cx, int cy, int rad, uint8_t a, uint8_t r,
                                     uint8_t g, uint8_t b)
{
    if (!dst || rad <= 0)
    {
        return;
    }
    int x = rad;
    int y = 0;
    int err = 0;
    while (x >= y)
    {
        set_px_argb(dst, w, h, cx + x, cy + y, a, r, g, b);
        set_px_argb(dst, w, h, cx + y, cy + x, a, r, g, b);
        set_px_argb(dst, w, h, cx - y, cy + x, a, r, g, b);
        set_px_argb(dst, w, h, cx - x, cy + y, a, r, g, b);
        set_px_argb(dst, w, h, cx - x, cy - y, a, r, g, b);
        set_px_argb(dst, w, h, cx - y, cy - x, a, r, g, b);
        set_px_argb(dst, w, h, cx + y, cy - x, a, r, g, b);
        set_px_argb(dst, w, h, cx + x, cy - y, a, r, g, b);
        y++;
        err += 1 + 2 * y;
        if (2 * (err - x) + 1 > 0)
        {
            x--;
            err += 1 - 2 * x;
        }
    }
}

static void draw_line_argb(uint8_t *dst, int w, int h, int x0, int y0, int x1, int y1, uint8_t a, uint8_t r, uint8_t g,
                           uint8_t b)
{
    if (!dst || w <= 0 || h <= 0)
    {
        return;
    }
    const size_t stride = (size_t)w * 4u;
    auto plot = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= w || y >= h)
        {
            return;
        }
        uint8_t *p = dst + (size_t)y * stride + (size_t)x * 4u;
        p[0] = a;
        p[1] = r;
        p[2] = g;
        p[3] = b;
    };
    const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
    if (steps == 0)
    {
        plot(x0, y0);
        return;
    }
    for (int i = 0; i <= steps; i++)
    {
        const int x = x0 + (int)std::llround(((double)(x1 - x0) * (double)i) / (double)steps);
        const int y = y0 + (int)std::llround(((double)(y1 - y0) * (double)i) / (double)steps);
        plot(x, y);
    }
}

static void draw_border_argb(uint8_t *dst, int w, int h, int thickness, uint8_t a, uint8_t r, uint8_t g, uint8_t b)
{
    if (!dst || w <= 0 || h <= 0 || thickness <= 0)
    {
        return;
    }
    const int t = std::min(thickness, std::min(w, h) / 2);
    const size_t stride = (size_t)w * 4u;
    auto set_px = [&](int x, int y) {
        uint8_t *p = dst + (size_t)y * stride + (size_t)x * 4u;
        p[0] = a;
        p[1] = r;
        p[2] = g;
        p[3] = b;
    };
    /* top/bottom */
    for (int y = 0; y < t; y++)
    {
        for (int x = 0; x < w; x++)
        {
            set_px(x, y);
            set_px(x, h - 1 - y);
        }
    }
    /* left/right */
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < t; x++)
        {
            set_px(x, y);
            set_px(w - 1 - x, y);
        }
    }
}

static int osd_update_windows(int vidx, int cidx)
{
    HalVideoContext *vc = video_at(vidx);
    HalCodecContext *cc = codec_at(cidx);
    if (!vc || !cc)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (!HAL_OSD_OPS.add_custom_overlay || !HAL_OSD_OPS.set_custom_overlay || !HAL_OSD_OPS.set_overlay_enabled)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }

    if (!g_af_osd_enabled)
    {
        /* Hide existing overlays without removing them. */
        for (uint32_t i = 0; i < HAL_ISP_AF_MAX_WINDOWS; ++i)
        {
            if (g_osd_added[i])
            {
                (void)HAL_OSD_OPS.set_overlay_enabled(cc, k_osd_ids[i], false);
            }
        }
        if (g_osd_curve_added)
        {
            (void)HAL_OSD_OPS.set_overlay_enabled(cc, k_osd_curve_id, false);
        }
        return HAL_OK;
    }

    HalVideoConfig vcfg{};
    (void)HAL_VIDEO_OPS.get_current_config(vc, &vcfg);
    const int vw = (int)vcfg.width;
    const int vh = (int)vcfg.height;
    const int ew = (int)cc->config.width;
    const int eh = (int)cc->config.height;
    if (vw <= 0 || vh <= 0 || ew <= 0 || eh <= 0)
    {
        return HAL_ERR_INVALID_STATE;
    }

    const float sx = (float)ew / (float)vw;
    const float sy = (float)eh / (float)vh;

    for (uint32_t i = 0; i < HAL_ISP_AF_MAX_WINDOWS; ++i)
    {
        if (i >= g_af_windows.window_count)
        {
            if (g_osd_added[i])
            {
                (void)HAL_OSD_OPS.set_overlay_enabled(cc, k_osd_ids[i], false);
            }
            continue;
        }
        const int x = (int)std::lround((double)g_af_windows.windows[i].x * (double)sx);
        const int y = (int)std::lround((double)g_af_windows.windows[i].y * (double)sy);
        const int w = (int)std::lround((double)g_af_windows.windows[i].w * (double)sx);
        const int h = (int)std::lround((double)g_af_windows.windows[i].h * (double)sy);
        if (w <= 0 || h <= 0)
        {
            continue;
        }
        const int px_w = std::clamp(w, 2, ew);
        const int px_h = std::clamp(h, 2, eh);

        const float nx = (float)x / (float)ew;
        const float ny = (float)y / (float)eh;
        const float nw = (float)px_w / (float)ew;
        const float nh = (float)px_h / (float)eh;

        g_osd_buf[i].assign((size_t)px_w * (size_t)px_h * 4u, 0);
        draw_border_argb(g_osd_buf[i].data(), px_w, px_h, g_af_osd_thickness,
                         g_af_osd_color.a, g_af_osd_color.r, g_af_osd_color.g, g_af_osd_color.b);

        HalOsdCustomOverlay ov{};
        std::snprintf(ov.base.id, sizeof(ov.base.id), "%s", k_osd_ids[i]);
        ov.base.enabled = true;
        ov.base.x = std::clamp(nx, 0.0f, 1.0f);
        ov.base.y = std::clamp(ny, 0.0f, 1.0f);
        ov.base.z_index = 100;
        ov.base.angle = 0;
        ov.base.rotation_policy = HAL_OSD_ROTATION_POLICY_TOP_LEFT;
        ov.base.h_align = HAL_OSD_HALIGN_LEFT;
        ov.base.v_align = HAL_OSD_VALIGN_TOP;
        ov.width = std::clamp(nw, 0.0f, 1.0f);
        ov.height = std::clamp(nh, 0.0f, 1.0f);
        ov.format = HAL_OSD_CUSTOM_FMT_ARGB;
        ov.data = g_osd_buf[i].data();
        ov.data_size = (uint32_t)g_osd_buf[i].size();

        int r = HAL_OK;
        if (!g_osd_added[i])
        {
            r = HAL_OSD_OPS.add_custom_overlay(cc, &ov);
            if (r == HAL_OK)
            {
                g_osd_added[i] = true;
            }
        }
        else
        {
            r = HAL_OSD_OPS.set_custom_overlay(cc, &ov);
        }
        if (r != HAL_OK)
        {
            return r;
        }
        (void)HAL_OSD_OPS.set_overlay_enabled(cc, k_osd_ids[i], true);
    }
    return HAL_OK;
}

static int osd_update_af_curve(int cidx, const std::vector<AfFocusSample> &curve, int peak_pos,
                               double peak_marker_m, double peak_label_m, bool draw_peak)
{
    HalCodecContext *cc = codec_at(cidx);
    if (!cc || curve.size() < 2u)
    {
        return HAL_OK;
    }
    if (!HAL_OSD_OPS.add_custom_overlay || !HAL_OSD_OPS.set_custom_overlay || !HAL_OSD_OPS.set_overlay_enabled)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    if (!g_af_osd_enabled)
    {
        if (g_osd_curve_added)
        {
            (void)HAL_OSD_OPS.set_overlay_enabled(cc, k_osd_curve_id, false);
        }
        return HAL_OK;
    }

    const int ew = (int)cc->config.width;
    const int eh = (int)cc->config.height;
    if (ew <= 0 || eh <= 0)
    {
        return HAL_ERR_INVALID_STATE;
    }
    /* ~800x400 plot area, clamped to encoder resolution; small inset from top-right. */
    constexpr int k_curve_pref_w = 800;
    constexpr int k_curve_pref_h = 400;
    constexpr int k_curve_margin_px = 8;
    const int inset = std::clamp(k_curve_margin_px, 0, std::max(0, std::min(ew, eh) / 8));
    int chart_w = std::min(k_curve_pref_w, std::max(80, ew - inset * 2));
    int chart_h = std::min(k_curve_pref_h, std::max(48, eh - inset * 2));
    chart_w = std::min(chart_w, ew);
    chart_h = std::min(chart_h, eh);
    g_osd_curve_buf.assign((size_t)chart_w * (size_t)chart_h * 4u, 0);

    const int mx = std::clamp(chart_w / 14, 10, chart_w / 3);
    const int my = std::clamp(chart_h / 36, 8, chart_h / 4);
    int pmin = curve[0].pos;
    int pmax = curve[0].pos;
    double vmin = curve[0].m;
    double vmax = curve[0].m;
    for (const auto &s : curve)
    {
        pmin = std::min(pmin, s.pos);
        pmax = std::max(pmax, s.pos);
        vmin = std::min(vmin, s.m);
        vmax = std::max(vmax, s.m);
    }
    if (draw_peak)
    {
        pmin = std::min(pmin, peak_pos);
        pmax = std::max(pmax, peak_pos);
        vmin = std::min(vmin, std::min(peak_marker_m, peak_label_m));
        vmax = std::max(vmax, std::max(peak_marker_m, peak_label_m));
    }
    const double dp = (double)std::max(1, pmax - pmin);
    const double dm = std::max(1e-9, vmax - vmin);

    auto proj = [&](int pos, double met) {
        const int x = mx + (int)std::lround(((double)(pos - pmin) / dp) * (double)(chart_w - 2 * mx));
        const int y = my + (int)std::lround((1.0 - (met - vmin) / dm) * (double)(chart_h - 2 * my));
        return std::pair<int, int>{std::clamp(x, 0, chart_w - 1), std::clamp(y, 0, chart_h - 1)};
    };

    const uint8_t ax_a = 160;
    const uint8_t ax_rgb = 200;
    draw_line_argb(g_osd_curve_buf.data(), chart_w, chart_h, mx, chart_h - my, chart_w - mx, chart_h - my, ax_a,
                   ax_rgb, ax_rgb, ax_rgb);
    draw_line_argb(g_osd_curve_buf.data(), chart_w, chart_h, mx, my, mx, chart_h - my, ax_a, ax_rgb, ax_rgb, ax_rgb);

    for (size_t i = 1; i < curve.size(); i++)
    {
        const auto p0 = proj(curve[i - 1].pos, curve[i - 1].m);
        const auto p1 = proj(curve[i].pos, curve[i].m);
        draw_line_argb(g_osd_curve_buf.data(), chart_w, chart_h, p0.first, p0.second, p1.first, p1.second, 240, 40, 180,
                       255);
    }

    if (draw_peak)
    {
        const auto pk = proj(peak_pos, peak_marker_m);
        const int ppx = pk.first;
        const int ppy = pk.second;
        draw_circle_outline_argb(g_osd_curve_buf.data(), chart_w, chart_h, ppx, ppy, 6, 255, 255, 220, 40);
        draw_line_argb(g_osd_curve_buf.data(), chart_w, chart_h, ppx - 10, ppy, ppx + 10, ppy, 255, 255, 220, 40);
        draw_line_argb(g_osd_curve_buf.data(), chart_w, chart_h, ppx, ppy - 10, ppx, ppy + 10, 255, 255, 220, 40);
        char lbl[128]{};
        const double dmd = std::fabs(peak_marker_m - peak_label_m);
        if (dmd > 0.05 * std::max(std::fabs(peak_marker_m), 1.0) && dmd > 0.5)
        {
            (void)std::snprintf(lbl, sizeof(lbl), "P=%d\nsmpl%.2f\nnow%.2f", peak_pos, peak_marker_m, peak_label_m);
        }
        else
        {
            (void)std::snprintf(lbl, sizeof(lbl), "P=%d m=%.3f", peak_pos, peak_label_m);
        }
        const int scale = std::clamp(chart_w / 200, 1, 3);
        int label_lines = 1;
        for (const char *p = lbl; *p != '\0'; ++p)
        {
            if (*p == '\n')
            {
                ++label_lines;
            }
        }
        const int line_h = label_lines * 8 * scale;
        int tx = ppx + 10;
        int ty = ppy - line_h - 4;
        if (tx + 20 * 6 * scale > chart_w - mx - 2)
        {
            tx = std::max(mx + 2, ppx - 20 * 6 * scale - 10);
        }
        if (ty < my + 2)
        {
            ty = std::min(chart_h - my - line_h - 2, ppy + 12);
        }
        draw_string_argb_glcd(g_osd_curve_buf.data(), chart_w, chart_h, tx, ty, lbl, 255, 255, 255, 60, scale);
    }

    HalOsdCustomOverlay ov{};
    std::snprintf(ov.base.id, sizeof(ov.base.id), "%s", k_osd_curve_id);
    ov.base.enabled = true;
    /* Anchor: top-right corner of encoder frame, with pixel inset (see hal_osd.h alignment). */
    ov.base.x = std::clamp(1.0f - (float)inset / (float)ew, 0.0f, 1.0f);
    ov.base.y = std::clamp((float)inset / (float)eh, 0.0f, 1.0f);
    ov.base.z_index = 110;
    ov.base.angle = 0;
    ov.base.rotation_policy = HAL_OSD_ROTATION_POLICY_TOP_LEFT;
    ov.base.h_align = HAL_OSD_HALIGN_RIGHT;
    ov.base.v_align = HAL_OSD_VALIGN_TOP;
    ov.width = std::clamp((float)chart_w / (float)ew, 0.0f, 1.0f);
    ov.height = std::clamp((float)chart_h / (float)eh, 0.0f, 1.0f);
    ov.format = HAL_OSD_CUSTOM_FMT_ARGB;
    ov.data = g_osd_curve_buf.data();
    ov.data_size = (uint32_t)g_osd_curve_buf.size();

    int r = HAL_OK;
    if (!g_osd_curve_added)
    {
        r = HAL_OSD_OPS.add_custom_overlay(cc, &ov);
        if (r == HAL_OK)
        {
            g_osd_curve_added = true;
        }
    }
    else
    {
        r = HAL_OSD_OPS.set_custom_overlay(cc, &ov);
    }
    if (r != HAL_OK)
    {
        return r;
    }
    (void)HAL_OSD_OPS.set_overlay_enabled(cc, k_osd_curve_id, true);
    return HAL_OK;
}

/* ------------------------------------------------------------------ */
/* AF engine                                                           */
/* ------------------------------------------------------------------ */

static int lens_focus_abs_blocking(int pos)
{
    if (!g_af0832)
    {
        return HAL_ERR_NOT_INITIALIZED;
    }
    std::lock_guard<std::mutex> lk(g_lens_mu);
    HalLensMotion m{};
    m.pps = static_cast<uint16_t>(std::clamp(g_afp.pps, 1, 20000));
    m.value = pos;
    return hal_lens_af0832_focus_abs(g_af0832, &m);
}

static int lens_focus_pos_get(int *out_pos)
{
    if (!out_pos)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (!g_af0832)
    {
        return HAL_ERR_NOT_INITIALIZED;
    }
    HalLensState st{};
    int r = hal_lens_af0832_state_get(g_af0832, &st);
    if (r != HAL_OK)
    {
        return r;
    }
    *out_pos = st.focus_pos;
    return HAL_OK;
}

static int af_set_focus_and_measure(int vidx, int pos, double *out_metric)
{
    if (!out_metric)
    {
        return HAL_ERR_INVALID_ARG;
    }
    int r = lens_focus_abs_blocking(pos);
    if (r != HAL_OK)
    {
        std::printf("af_measure: lens_focus_abs pos=%d ret=%d (%s)\n", pos, r, hal_error_to_string((HalErrorCode)r));
        std::fflush(stdout);
        return r;
    }
    return read_metric_isp_averaged_after_still(vidx, out_metric);
}

static int af_focus_take_sample(int vidx, int pos, std::vector<AfFocusSample> *curve, int *move_count, int *best_pos,
                                double *best_m, const char *phase)
{
    if (!curve || !move_count || !best_pos || !best_m)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (g_af_stop_req.load(std::memory_order_acquire))
    {
        std::printf("af_scan [%s] aborted at pos=%d: af_stop requested\n", phase ? phase : "?", pos);
        std::fflush(stdout);
        return HAL_ERR_INVALID_STATE;
    }
    if (g_af_pause.load(std::memory_order_acquire))
    {
        std::printf("af_scan [%s] aborted at pos=%d: af_pause\n", phase ? phase : "?", pos);
        std::fflush(stdout);
        return HAL_ERR_INVALID_STATE;
    }
    if (++(*move_count) > g_afp.max_total_moves)
    {
        std::printf("af_scan [%s] aborted at pos=%d: max_total_moves=%d exceeded\n", phase ? phase : "?", pos,
                    g_afp.max_total_moves);
        std::fflush(stdout);
        return HAL_ERR_INVALID_STATE;
    }
    double mm = 0.0;
    const int r = af_set_focus_and_measure(vidx, pos, &mm);
    if (r != HAL_OK)
    {
        std::printf("af_scan [%s] pos=%d measure failed ret=%d (%s)\n", phase ? phase : "?", pos, r,
                    hal_error_to_string((HalErrorCode)r));
        std::fflush(stdout);
        return r;
    }
    curve->push_back(AfFocusSample{pos, mm});
    if (mm > *best_m)
    {
        *best_m = mm;
        *best_pos = pos;
    }
    return HAL_OK;
}

static int af_scan_expand_from_center(int vidx, int center, int lo, int hi, int step,
                                      std::vector<AfFocusSample> *curve, int *move_count, int *best_pos, double *best_m,
                                      const char *phase)
{
    if (!curve || !move_count || !best_pos || !best_m)
    {
        return HAL_ERR_INVALID_ARG;
    }
    const char *ph = phase ? phase : "scan";
    const int s = std::max(1, step);
    const int c = std::clamp(center, lo, hi);
    std::printf(
        "af_scan [%s] start center=%d range=[%d,%d] step=%d early_stop_min_spread=%d fit_min_samples=%d moves_so_far=%d\n",
        ph, c, lo, hi, s, g_afp.early_stop_min_spread, g_afp.fit_min_samples, *move_count);
    std::fflush(stdout);

    int r = af_focus_take_sample(vidx, c, curve, move_count, best_pos, best_m, ph);
    if (r != HAL_OK)
    {
        return r;
    }

    int pr = c;
    bool right_fit_stop = false;
    if (pr + s > hi)
    {
        std::printf("af_scan [%s] right: skipped (no room: center=%d hi=%d step=%d)\n", ph, c, hi, s);
        std::fflush(stdout);
    }
    else
    {
        while (pr + s <= hi)
        {
            if (af_fit_stop_extend_right(*curve, s, g_afp.fit_min_samples, g_afp.fit_min_curvature, lo, hi))
            {
                right_fit_stop = true;
                break;
            }
            pr += s;
            r = af_focus_take_sample(vidx, pr, curve, move_count, best_pos, best_m, ph);
            if (r != HAL_OK)
            {
                return r;
            }
        }
        if (right_fit_stop)
        {
            std::printf(
                "af_scan [%s] right: early_stop (global quadratic fit: passed vertex / spread rules) last_pos=%d\n",
                ph, pr);
            std::fflush(stdout);
        }
        else
        {
            std::printf("af_scan [%s] right: boundary hi=%d last_sample_pos=%d (swept to edge)\n", ph, hi, pr);
            std::fflush(stdout);
        }
    }

    bool left_fit_stop = false;
    int pl = c;
    if (pl - s < lo)
    {
        std::printf("af_scan [%s] left: skipped (no room: center=%d lo=%d step=%d)\n", ph, c, lo, s);
        std::fflush(stdout);
    }
    else
    {
        while (pl - s >= lo)
        {
            if (af_fit_stop_extend_left(*curve, s, g_afp.fit_min_samples, g_afp.fit_min_curvature, lo, hi))
            {
                left_fit_stop = true;
                break;
            }
            pl -= s;
            r = af_focus_take_sample(vidx, pl, curve, move_count, best_pos, best_m, ph);
            if (r != HAL_OK)
            {
                return r;
            }
        }
        if (left_fit_stop)
        {
            std::printf(
                "af_scan [%s] left: early_stop (global quadratic fit: passed vertex / spread rules) last_pos=%d\n", ph,
                pl);
            std::fflush(stdout);
        }
        else
        {
            std::printf("af_scan [%s] left: boundary lo=%d last_sample_pos=%d (swept to edge)\n", ph, lo, pl);
            std::fflush(stdout);
        }
    }

    std::printf("af_scan [%s] done samples=%zu best_pos=%d best_m=%.6f\n", ph, curve->size(), *best_pos, *best_m);
    std::fflush(stdout);
    return HAL_OK;
}

static int af_run_once_internal(int vidx)
{
    std::printf("af_run_once: start (vidx=%d range=[%d,%d] coarse=%d fine=%d span=%d sync_frames=%d "
                "metric_avg_frames=%d)\n",
                vidx, g_afp.min_pos, g_afp.max_pos, g_afp.coarse_step, g_afp.fine_step, g_afp.fine_span_steps,
                g_afp.sync_frames, g_afp.metric_avg_frames);
    std::fflush(stdout);

    (void)af_vsync_subscribe(vidx);

    const int minp = std::min(g_afp.min_pos, g_afp.max_pos);
    const int maxp = std::max(g_afp.min_pos, g_afp.max_pos);
    const int coarse = std::max(1, g_afp.coarse_step);
    const int fine = std::max(1, g_afp.fine_step);

    int moves = 0;
    int best_pos = 0;
    double best_m = -1.0;

    std::vector<AfFocusSample> coarse_curve;
    std::vector<AfFocusSample> fine_curve;

    int cur_pos = 0;
    if (lens_focus_pos_get(&cur_pos) != HAL_OK)
    {
        cur_pos = (minp + maxp) / 2;
    }
    const int center_pos = std::clamp(cur_pos, minp, maxp);
    const int coarse_span = std::max(0, g_afp.coarse_span_steps);
    const int c0 = (coarse_span == 0) ? minp : std::max(minp, center_pos - coarse_span);
    const int c1 = (coarse_span == 0) ? maxp : std::min(maxp, center_pos + coarse_span);

    best_pos = center_pos;
    best_m = -1.0;
    const int r_coarse = af_scan_expand_from_center(vidx, center_pos, c0, c1, coarse, &coarse_curve, &moves, &best_pos,
                                                    &best_m, "coarse");
    if (r_coarse != HAL_OK)
    {
        std::printf("af_run_once: coarse failed ret=%d (%s)\n", r_coarse,
                    hal_error_to_string((HalErrorCode)r_coarse));
        std::fflush(stdout);
        af_vsync_unsubscribe(vidx);
        return r_coarse;
    }

    const double coarse_peak_x =
        af_fitted_peak_x(coarse_curve, g_afp.fit_min_samples, g_afp.fit_min_curvature);
    const int coarse_disc_pos = best_pos;
    const double coarse_disc_m = best_m;
    int fine_center = (int)std::lround(std::clamp(coarse_peak_x, (double)minp, (double)maxp));
    /* Quadratic LS can place the vertex far from the sampled discrete peak; trust discrete for window center. */
    if (std::abs(fine_center - coarse_disc_pos) > coarse)
    {
        std::printf("af_run_once: coarse_peak_fit=%.2f (center=%d) vs discrete_best=%d (m=%.6f) — using discrete for "
                    "fine_center\n",
                    coarse_peak_x, fine_center, coarse_disc_pos, coarse_disc_m);
        std::fflush(stdout);
        fine_center = coarse_disc_pos;
    }
    const int span = std::max(fine, g_afp.fine_span_steps);
    const int f0 = std::max(minp, fine_center - span);
    const int f1 = std::min(maxp, fine_center + span);

    int best_f = best_pos;
    double best_fm = best_m;
    const int r_fine =
        af_scan_expand_from_center(vidx, fine_center, f0, f1, fine, &fine_curve, &moves, &best_f, &best_fm, "fine");
    double fine_peak_x = (double)fine_center;
    if (r_fine == HAL_OK)
    {
        fine_peak_x = af_fitted_peak_x(fine_curve, g_afp.fit_min_samples, g_afp.fit_min_curvature);
        const int snap = (int)std::lround(std::clamp(fine_peak_x, (double)minp, (double)maxp));
        /* Fitted vertex can sit on a scan edge with low metric; discrete argmax on fine samples is only local. */
        if (snap != best_f)
        {
            std::printf("af_run_once: fine_peak_fit=%.2f (snap=%d) vs discrete_best=%d (m=%.6f) — merge may override "
                        "with global peak\n",
                        fine_peak_x, snap, best_f, best_fm);
            std::fflush(stdout);
        }
    }

    std::vector<AfFocusSample> plot;
    af_curve_merge_plot(coarse_curve, fine_curve, &plot);
    int global_pos = coarse_disc_pos;
    double global_plot_m = coarse_disc_m;
    if (!plot.empty())
    {
        af_curve_global_discrete_max(plot, &global_pos, &global_plot_m);
    }
    if (r_fine == HAL_OK && global_pos != best_f)
    {
        std::printf("af_run_once: merged global discrete peak=%d m=%.6f (overrides fine-local %d m=%.6f)\n",
                    global_pos, global_plot_m, best_f, best_fm);
        std::fflush(stdout);
    }

    size_t fine2_used = 0;
    if (g_afp.fine_rescan_if_global_outside != 0 && (global_pos < f0 || global_pos > f1))
    {
        const int span2 = std::max(fine, g_afp.fine_span_steps);
        const int fc2 = global_pos;
        const int f02 = std::max(minp, fc2 - span2);
        const int f12 = std::min(maxp, fc2 + span2);
        std::printf("af_run_once: merged global peak=%d outside first fine window [%d,%d] — second fine scan "
                    "[%d,%d] center=%d\n",
                    global_pos, f0, f1, f02, f12, fc2);
        std::fflush(stdout);
        std::vector<AfFocusSample> fine2_curve;
        int best_f2 = fc2;
        double best_fm2 = -1.0;
        const int r_fine2 =
            af_scan_expand_from_center(vidx, fc2, f02, f12, fine, &fine2_curve, &moves, &best_f2, &best_fm2, "fine2");
        if (r_fine2 == HAL_OK && !fine2_curve.empty())
        {
            fine2_used = fine2_curve.size();
            af_curve_merge_plot(plot, fine2_curve, &plot);
            if (!plot.empty())
            {
                af_curve_global_discrete_max(plot, &global_pos, &global_plot_m);
            }
            std::printf("af_run_once: fine2 merged samples=%zu new global peak=%d m=%.6f\n", fine2_used, global_pos,
                        global_plot_m);
            std::fflush(stdout);
        }
        else
        {
            std::printf("af_run_once: fine2 skipped ret=%d (%s)\n", r_fine2, hal_error_to_string((HalErrorCode)r_fine2));
            std::fflush(stdout);
        }
    }

    std::vector<AfFocusSample> ranked = plot;
    std::sort(ranked.begin(), ranked.end(), [](const AfFocusSample &a, const AfFocusSample &b) {
        if (a.m != b.m)
        {
            return a.m > b.m;
        }
        return a.pos < b.pos;
    });
    const double pv_frac = g_afp.peak_verify_min_frac;
    const int pv_cap = std::max(1, g_afp.peak_verify_max_tries);
    int pick_pos = global_pos;
    double pick_curve_m = global_plot_m;
    double best_seen_meas = -1.0;
    int best_seen_pos = global_pos;
    int accepted_i = -1;

    if (pv_frac <= 0.0)
    {
        /* Single settle + read below; no extra candidate moves. */
        accepted_i = 0;
    }
    else
    {
        for (int i = 0; i < (int)ranked.size() && i < pv_cap; ++i)
        {
            const int p = ranked[i].pos;
            const double smpl = ranked[i].m;
            double vm = 0.0;
            const int rv = af_verify_focus_and_measure(vidx, p, &moves, &vm, "peak_verify");
            if (rv != HAL_OK)
            {
                if (rv == HAL_ERR_INVALID_STATE)
                {
                    af_vsync_unsubscribe(vidx);
                    return rv;
                }
                continue;
            }
            if (vm > best_seen_meas)
            {
                best_seen_meas = vm;
                best_seen_pos = p;
            }
            if (smpl <= 1e-12 || vm >= pv_frac * smpl)
            {
                pick_pos = p;
                pick_curve_m = smpl;
                accepted_i = i;
                std::printf("af_run_once: peak_verify accept pos=%d smpl=%.6f now=%.6f (rank=%d frac=%.3g)\n", p, smpl,
                            vm, i, pv_frac);
                std::fflush(stdout);
                break;
            }
            std::printf("af_run_once: peak_verify reject pos=%d smpl=%.6f now=%.6f (need now>=%.3g*smpl)\n", p, smpl, vm,
                        pv_frac);
            std::fflush(stdout);
        }
    }
    if (accepted_i < 0 && pv_frac > 0.0 && best_seen_meas >= 0.0)
    {
        pick_pos = best_seen_pos;
        pick_curve_m = af_curve_m_at_pos(plot, best_seen_pos);
        std::printf("af_run_once: peak_verify fallback to max measured pos=%d now=%.6f (no candidate passed "
                    "smpl/now ratio)\n",
                    pick_pos, best_seen_meas);
        std::fflush(stdout);
    }

    best_pos = pick_pos;
    const double curve_peak_m_at_pick = pick_curve_m;

    (void)lens_focus_abs_blocking(best_pos);
    {
        double final_m = 0.0;
        if (read_metric_isp_averaged_after_still(vidx, &final_m) == HAL_OK)
        {
            best_m = final_m;
        }
        else
        {
            best_m = curve_peak_m_at_pick;
        }
    }

    af_vsync_unsubscribe(vidx);

    if (g_af_osd_enabled && plot.size() >= 2u)
    {
        (void)osd_update_af_curve(g_cidx, plot, best_pos, curve_peak_m_at_pick, best_m, true);
    }

    std::printf("af_run_once: best_pos=%d metric=%.6f coarse_peak_fit=%.2f fine_peak_fit=%.2f samples coarse=%zu "
                "fine=%zu fine2=%zu plot=%zu\n",
                best_pos, best_m, coarse_peak_x, fine_peak_x, coarse_curve.size(), fine_curve.size(), fine2_used,
                plot.size());
    std::fflush(stdout);
    return HAL_OK;
}

static void af_thread_main()
{
    while (!g_af_thread_exit.load(std::memory_order_acquire))
    {
        g_af_heartbeat_ms.store(
            (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch())
                .count(),
            std::memory_order_release);

        std::unique_lock<std::mutex> lk(g_af_mu);
        g_af_cv.wait_for(lk, std::chrono::milliseconds(50), [&] {
            return g_af_thread_exit.load(std::memory_order_acquire) ||
                   g_af_run_once_req.load(std::memory_order_acquire) ||
                   g_af_tracking.load(std::memory_order_acquire);
        });
        if (g_af_thread_exit.load(std::memory_order_acquire))
        {
            break;
        }
        if (g_af_pause.load(std::memory_order_acquire) || !g_af0832)
        {
            continue;
        }

        bool do_once = g_af_run_once_req.exchange(false, std::memory_order_acq_rel);
        bool do_track = g_af_tracking.load(std::memory_order_acquire);
        lk.unlock();

        if (do_once)
        {
            g_af_runs_started.fetch_add(1, std::memory_order_acq_rel);
            g_af_state.store(1, std::memory_order_release);
            g_af_stop_req.store(false, std::memory_order_release);
            const int rr = af_run_once_internal(g_vidx);
            if (rr != HAL_OK)
            {
                std::printf("af_run_once: done ret=%d (%s)\n", rr, hal_error_to_string((HalErrorCode)rr));
                std::fflush(stdout);
            }
            g_af_state.store(0, std::memory_order_release);
        }
        else if (do_track)
        {
            /* Simple tracking: periodically read metric; if it drops significantly, trigger a scan. */
            double m0 = 0.0;
            if (read_metric_isp(g_vidx, &m0) == HAL_OK)
            {
                static double last = 0.0;
                if (last == 0.0)
                {
                    last = m0;
                }
                const double drop = (last > 1e-9) ? (m0 / last) : 1.0;
                last = 0.9 * last + 0.1 * m0;
                if (drop < 0.70) /* threshold */
                {
                    g_af_state.store(1, std::memory_order_release);
                    g_af_stop_req.store(false, std::memory_order_release);
                    (void)af_run_once_internal(g_vidx);
                    g_af_state.store(0, std::memory_order_release);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }
}

static std::thread g_af_thread;

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_versions()
{
    const char *mv = HAL_MEDIA_OPS.get_version ? HAL_MEDIA_OPS.get_version() : "(null)";
    const char *vv = HAL_VIDEO_OPS.get_version ? HAL_VIDEO_OPS.get_version() : "(null)";
    const char *cv = HAL_CODEC_OPS.get_version ? HAL_CODEC_OPS.get_version() : "(null)";
    const char *iv = HAL_ISP_OPS.get_version ? HAL_ISP_OPS.get_version() : "(null)";
    const char *ov = HAL_OSD_OPS.get_version ? HAL_OSD_OPS.get_version() : "(null)";
    const char *mcv = HAL_MCU_OPS.get_hal_version ? HAL_MCU_OPS.get_hal_version() : "(null)";
    std::printf("HAL_MEDIA_OPS  %s\n", mv);
    std::printf("HAL_VIDEO_OPS  %s\n", vv);
    std::printf("HAL_CODEC_OPS  %s\n", cv);
    std::printf("HAL_ISP_OPS    %s\n", iv);
    std::printf("HAL_OSD_OPS    %s\n", ov);
    std::printf("HAL_MCU_OPS    %s\n", mcv);
    std::fflush(stdout);
}

static void cmd_vidx(int argc, char **av)
{
    if (argc < 2)
    {
        std::printf("vidx=%d\n", g_vidx);
        return;
    }
    g_vidx = std::atoi(av[1]);
    std::printf("vidx=%d\n", g_vidx);
}

static void cmd_cidx(int argc, char **av)
{
    if (argc < 2)
    {
        std::printf("cidx=%d\n", g_cidx);
        return;
    }
    g_cidx = std::atoi(av[1]);
    std::printf("cidx=%d\n", g_cidx);
}

static void cmd_isp_af_set(int argc, char **av)
{
    if (argc < 3)
    {
        std::printf("usage: isp_af_set <vidx> <0|1> <x1> <y1> <w1> <h1> [x2 y2 w2 h2] [x3 y3 w3 h3]\n");
        return;
    }
    const int vidx = std::atoi(av[1]);
    HalVideoContext *vc = video_at(vidx);
    if (!vc || !HAL_ISP_OPS.set_af_windows_config)
    {
        std::printf("isp_af_set: bad video index or HAL_ISP_OPS.set_af_windows_config not available\n");
        return;
    }

    HalIspAfWindowsConfig cfg{};
    cfg.enabled = (std::atoi(av[2]) != 0);
    if (!cfg.enabled)
    {
        cfg.window_count = 0;
        const int r = HAL_ISP_OPS.set_af_windows_config(vc, &cfg);
        std::printf("isp_af_set ret=%d\n", r);
        std::fflush(stdout);
        g_af_windows = cfg;
        return;
    }
    if (argc < 7)
    {
        std::printf("usage: isp_af_set <vidx> <0|1> <x1> <y1> <w1> <h1> [x2 y2 w2 h2] [x3 y3 w3 h3]\n");
        return;
    }
    const int remain = argc - 3;
    const int win_n = std::clamp(remain / 4, 1, (int)HAL_ISP_AF_MAX_WINDOWS);
    cfg.window_count = (uint32_t)win_n;
    for (int i = 0; i < win_n; ++i)
    {
        const int base = 3 + i * 4;
        cfg.windows[i].x = std::atoi(av[base + 0]);
        cfg.windows[i].y = std::atoi(av[base + 1]);
        cfg.windows[i].w = std::atoi(av[base + 2]);
        cfg.windows[i].h = std::atoi(av[base + 3]);
    }
    const int r = HAL_ISP_OPS.set_af_windows_config(vc, &cfg);
    std::printf("isp_af_set ret=%d enabled=1 windows=%u\n", r, (unsigned)cfg.window_count);
    std::fflush(stdout);
    if (r == HAL_OK)
    {
        g_af_windows = cfg;
        (void)osd_update_windows(vidx, g_cidx);
    }
}

static void cmd_isp_af_get(int argc, char **av)
{
    if (argc < 2)
    {
        std::printf("usage: isp_af_get <vidx>\n");
        return;
    }
    const int vidx = std::atoi(av[1]);
    HalVideoContext *vc = video_at(vidx);
    if (!vc || !HAL_ISP_OPS.get_af_windows_config)
    {
        std::printf("isp_af_get: bad video index or HAL_ISP_OPS.get_af_windows_config not available\n");
        return;
    }
    HalIspAfWindowsConfig cfg{};
    const int r = HAL_ISP_OPS.get_af_windows_config(vc, &cfg);
    std::printf("isp_af_get ret=%d enabled=%d windows=%u\n", r, (int)cfg.enabled, (unsigned)cfg.window_count);
    for (uint32_t i = 0; i < cfg.window_count && i < HAL_ISP_AF_MAX_WINDOWS; ++i)
    {
        std::printf("  win%u: x=%d y=%d w=%d h=%d\n", (unsigned)(i + 1), cfg.windows[i].x, cfg.windows[i].y,
                    cfg.windows[i].w, cfg.windows[i].h);
    }
    std::fflush(stdout);
    if (r == HAL_OK)
    {
        g_af_windows = cfg;
        (void)osd_update_windows(vidx, g_cidx);
    }
}

static void cmd_isp_af_meas(int argc, char **av)
{
    if (argc < 2)
    {
        std::printf("usage: isp_af_meas <vidx>\n");
        return;
    }
    const int vidx = std::atoi(av[1]);
    HalVideoContext *vc = video_at(vidx);
    if (!vc || !HAL_ISP_OPS.get_af_measurement)
    {
        std::printf("isp_af_meas: bad video index or HAL_ISP_OPS.get_af_measurement not available\n");
        return;
    }
    HalIspAfMeasurement m{};
    const int r = HAL_ISP_OPS.get_af_measurement(vc, &m);
    std::printf("isp_af_meas ret=%d windows=%u sum=[%u,%u,%u] luma=[%u,%u,%u] metric=%.6f\n", r, (unsigned)m.window_count,
                m.sum[0], m.sum[1], m.sum[2], m.luma[0], m.luma[1], m.luma[2], metric_from_meas(m));
    std::fflush(stdout);
}

static void cmd_mcu_init(int argc, char **av)
{
    if (argc < 2)
    {
        std::printf("usage: mcu_init <serial_dev> [baud] [timeout_ms]\n");
        return;
    }
    if (g_mcu_ctx)
    {
        std::printf("mcu already initialized\n");
        return;
    }
    HalMcuConfig cfg{};
    cfg.serial_device = av[1];
    cfg.baud_rate = (argc >= 3) ? (uint32_t)std::atoi(av[2]) : 115200u;
    cfg.timeout_ms = (argc >= 4) ? (uint32_t)std::atoi(av[3]) : 2000u;
    int r = HAL_MCU_OPS.init(&cfg, &g_mcu_ctx);
    std::printf("mcu_init ret=%d ctx=%p\n", r, g_mcu_ctx);
    std::fflush(stdout);
}

static void cmd_mcu_deinit()
{
    if (!g_mcu_ctx)
    {
        std::printf("mcu not initialized\n");
        return;
    }
    (void)HAL_MCU_OPS.deinit(g_mcu_ctx);
    g_mcu_ctx = nullptr;
    std::printf("mcu_deinit ok\n");
}

static void cmd_af_create()
{
    if (!g_mcu_ctx)
    {
        std::printf("af_create: MCU not initialized (use mcu_init)\n");
        return;
    }
    if (g_af0832)
    {
        std::printf("af already created\n");
        return;
    }
    std::lock_guard<std::mutex> lk(g_lens_mu);
    HalLensAf0832Params p{};
    hal_lens_af0832_params_init_defaults(&p);
    int r = hal_lens_af0832_create(g_mcu_ctx, &p, &g_af0832);
    if (r != HAL_OK)
    {
        std::printf("af_create: create ret=%d\n", r);
        return;
    }
    r = hal_lens_af0832_bootstrap(g_af0832);
    std::printf("af_create: bootstrap ret=%d\n", r);
    std::fflush(stdout);

    if (r == HAL_OK)
    {
        /* Default coarse focus positioning via AF0832 tracking table. */
        int gr = hal_lens_af0832_goto_by_ratio_distance(g_af0832, 1.0f, 1.5f);
        std::printf("af_create: default_goto ratio=1.0 dist=1.5m ret=%d\n", gr);
        std::fflush(stdout);
    }
}

static void cmd_af_destroy()
{
    if (!g_af0832)
    {
        std::printf("af not created\n");
        return;
    }
    std::lock_guard<std::mutex> lk(g_lens_mu);
    hal_lens_af0832_destroy(g_af0832);
    g_af0832 = nullptr;
    std::printf("af_destroy ok\n");
}

static void cmd_zoom_goto(int argc, char **av)
{
    if (argc < 3)
    {
        std::printf("usage: zoom_goto <ratio> <distance_m>\n");
        return;
    }
    if (!g_af0832)
    {
        std::printf("zoom_goto: af not created\n");
        return;
    }
    const float ratio = (float)std::atof(av[1]);
    const float dist = (float)std::atof(av[2]);

    /* Pause AF and wait for current scan to stop */
    g_af_pause.store(true, std::memory_order_release);
    for (int i = 0; i < 200; i++)
    {
        if (g_af_state.load(std::memory_order_acquire) == 0)
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    int r = HAL_ERR_INVALID_STATE;
    {
        std::lock_guard<std::mutex> lk(g_lens_mu);
        r = hal_lens_af0832_goto_by_ratio_distance(g_af0832, ratio, dist);
    }
    std::printf("zoom_goto ret=%d\n", r);
    std::fflush(stdout);

    g_af_pause.store(false, std::memory_order_release);
    g_af_cv.notify_all();
}

static void cmd_af_param_get()
{
    std::printf("AF params:\n");
    std::printf("  min_pos=%d max_pos=%d coarse_step=%d fine_step=%d fine_span=%d\n",
                g_afp.min_pos, g_afp.max_pos, g_afp.coarse_step, g_afp.fine_step, g_afp.fine_span_steps);
    std::printf("  sync_frames=%d metric_avg_frames=%d max_moves=%d early_stop_min_spread=%d (0=off) "
                "fit_min_samples=%d fit_min_curvature=%.3g\n",
                g_afp.sync_frames, g_afp.metric_avg_frames, g_afp.max_total_moves, g_afp.early_stop_min_spread,
                g_afp.fit_min_samples, g_afp.fit_min_curvature);
    std::printf("  pps=%d\n", g_afp.pps);
    std::printf("  coarse_span=%d\n", g_afp.coarse_span_steps);
    std::printf("  metric_min_luma=%d metric_ratio_cap=%.3g (0=off; else caps per-window sum/(luma+1), flattens peak)\n",
                g_afp.metric_min_luma, g_afp.metric_ratio_cap);
    std::printf("  fine_rescan_if_global_outside=%d (1=second fine scan if merged peak outside first fine window)\n",
                g_afp.fine_rescan_if_global_outside);
    std::printf("  peak_verify_min_frac=%.3g (0=off; else accept peak only if measured >= frac*curve_sample; rejects "
                "non-reproducible spikes)\n",
                g_afp.peak_verify_min_frac);
    std::printf("  peak_verify_max_tries=%d\n", g_afp.peak_verify_max_tries);
    std::fflush(stdout);
}

static void cmd_af_param_set(int argc, char **av)
{
    if (argc < 3)
    {
        std::printf("usage: af_param_set <name> <value>\n");
        return;
    }
    const char *k = av[1];
    const int v = std::atoi(av[2]);
    if (std::strcmp(k, "pps") == 0) g_afp.pps = v;
    else if (std::strcmp(k, "min_pos") == 0) g_afp.min_pos = v;
    else if (std::strcmp(k, "max_pos") == 0) g_afp.max_pos = v;
    else if (std::strcmp(k, "coarse_span") == 0) g_afp.coarse_span_steps = v;
    else if (std::strcmp(k, "coarse_step") == 0) g_afp.coarse_step = v;
    else if (std::strcmp(k, "fine_step") == 0) g_afp.fine_step = v;
    else if (std::strcmp(k, "fine_span") == 0) g_afp.fine_span_steps = v;
    else if (std::strcmp(k, "sync_frames") == 0) g_afp.sync_frames = v;
    else if (std::strcmp(k, "metric_avg_frames") == 0) g_afp.metric_avg_frames = std::max(1, v);
    else if (std::strcmp(k, "max_moves") == 0) g_afp.max_total_moves = v;
    else if (std::strcmp(k, "metric_min_luma") == 0) g_afp.metric_min_luma = v;
    else if (std::strcmp(k, "metric_ratio_cap") == 0)
    {
        g_afp.metric_ratio_cap = std::atof(av[2]);
        if (g_afp.metric_ratio_cap < 0.0)
        {
            g_afp.metric_ratio_cap = 0.0;
        }
    }
    else if (std::strcmp(k, "early_stop_min_spread") == 0) g_afp.early_stop_min_spread = std::max(0, v);
    else if (std::strcmp(k, "fit_min_samples") == 0) g_afp.fit_min_samples = std::max(3, v);
    else if (std::strcmp(k, "fit_min_curvature") == 0)
    {
        g_afp.fit_min_curvature = std::atof(av[2]);
    }
    else if (std::strcmp(k, "fine_rescan_if_global_outside") == 0)
    {
        g_afp.fine_rescan_if_global_outside = (v != 0) ? 1 : 0;
    }
    else if (std::strcmp(k, "peak_verify_min_frac") == 0)
    {
        g_afp.peak_verify_min_frac = std::atof(av[2]);
        if (g_afp.peak_verify_min_frac < 0.0)
        {
            g_afp.peak_verify_min_frac = 0.0;
        }
        if (g_afp.peak_verify_min_frac > 1.0)
        {
            g_afp.peak_verify_min_frac = 1.0;
        }
    }
    else if (std::strcmp(k, "peak_verify_max_tries") == 0)
    {
        g_afp.peak_verify_max_tries = std::max(1, v);
    }
    else
    {
        std::printf("unknown param name\n");
        return;
    }
    cmd_af_param_get();
}

static void cmd_af_run_once()
{
    if (!g_af0832)
    {
        std::printf("af_run_once: af not created\n");
        return;
    }
    g_af_run_once_req.store(true, std::memory_order_release);
    g_af_cv.notify_all();
    std::printf("af_run_once: queued\n");
}

static void cmd_af_stop()
{
    g_af_stop_req.store(true, std::memory_order_release);
    std::printf("af_stop requested\n");
}

static void cmd_af_track(int argc, char **av)
{
    if (argc < 2)
    {
        std::printf("usage: af_track <0|1>\n");
        return;
    }
    const bool en = (std::atoi(av[1]) != 0);
    g_af_tracking.store(en, std::memory_order_release);
    std::printf("af_track=%d\n", (int)en);
    g_af_cv.notify_all();
}

static void cmd_af_status()
{
    const uint64_t hb = g_af_heartbeat_ms.load(std::memory_order_acquire);
    const uint64_t runs = g_af_runs_started.load(std::memory_order_acquire);
    std::printf("af_state=%s pause=%d tracking=%d\n",
                (g_af_state.load(std::memory_order_acquire) == 0) ? "idle" : "running",
                (int)g_af_pause.load(std::memory_order_acquire),
                (int)g_af_tracking.load(std::memory_order_acquire));
    std::printf("af_thread: heartbeat_ms=%llu runs_started=%llu\n",
                (unsigned long long)hb, (unsigned long long)runs);
}

static void cmd_af_osd(int argc, char **av)
{
    if (argc < 2)
    {
        std::printf("af_osd=%d\n", (int)g_af_osd_enabled);
        return;
    }
    g_af_osd_enabled = (std::atoi(av[1]) != 0);
    std::printf("af_osd=%d\n", (int)g_af_osd_enabled);
    if (g_af_osd_enabled)
    {
        (void)osd_update_windows(g_vidx, g_cidx);
    }
}

static void cmd_af_osd_style(int argc, char **av)
{
    if (argc < 6)
    {
        std::printf("usage: af_osd_style <thickness_px> <a> <r> <g> <b>\n");
        return;
    }
    g_af_osd_thickness = std::max(1, std::atoi(av[1]));
    g_af_osd_color.a = (uint8_t)std::clamp(std::atoi(av[2]), 0, 255);
    g_af_osd_color.r = (uint8_t)std::clamp(std::atoi(av[3]), 0, 255);
    g_af_osd_color.g = (uint8_t)std::clamp(std::atoi(av[4]), 0, 255);
    g_af_osd_color.b = (uint8_t)std::clamp(std::atoi(av[5]), 0, 255);
    std::printf("af_osd_style thickness=%d argb=(%u,%u,%u,%u)\n", g_af_osd_thickness,
                g_af_osd_color.a, g_af_osd_color.r, g_af_osd_color.g, g_af_osd_color.b);
    if (g_af_osd_enabled)
    {
        (void)osd_update_windows(g_vidx, g_cidx);
    }
}

static void dispatch_line(int argc, char **av)
{
    const char *cmd = av[0];
    if (std::strcmp(cmd, "help") == 0)
    {
        print_help();
        return;
    }
    if (std::strcmp(cmd, "streams") == 0)
    {
        (void)refresh_stream_lists();
        print_streams();
        return;
    }
    if (std::strcmp(cmd, "versions") == 0)
    {
        cmd_versions();
        return;
    }
    if (std::strcmp(cmd, "vidx") == 0)
    {
        cmd_vidx(argc, av);
        return;
    }
    if (std::strcmp(cmd, "cidx") == 0)
    {
        cmd_cidx(argc, av);
        return;
    }
    if (std::strcmp(cmd, "isp_af_set") == 0)
    {
        cmd_isp_af_set(argc, av);
        return;
    }
    if (std::strcmp(cmd, "isp_af_get") == 0)
    {
        cmd_isp_af_get(argc, av);
        return;
    }
    if (std::strcmp(cmd, "isp_af_meas") == 0)
    {
        cmd_isp_af_meas(argc, av);
        return;
    }
    if (std::strcmp(cmd, "mcu_init") == 0)
    {
        cmd_mcu_init(argc, av);
        return;
    }
    if (std::strcmp(cmd, "mcu_deinit") == 0)
    {
        cmd_mcu_deinit();
        return;
    }
    if (std::strcmp(cmd, "af_create") == 0)
    {
        cmd_af_create();
        return;
    }
    if (std::strcmp(cmd, "af_destroy") == 0)
    {
        cmd_af_destroy();
        return;
    }
    if (std::strcmp(cmd, "zoom_goto") == 0)
    {
        cmd_zoom_goto(argc, av);
        return;
    }
    if (std::strcmp(cmd, "af_param_get") == 0)
    {
        cmd_af_param_get();
        return;
    }
    if (std::strcmp(cmd, "af_param_set") == 0)
    {
        cmd_af_param_set(argc, av);
        return;
    }
    if (std::strcmp(cmd, "af_run_once") == 0)
    {
        cmd_af_run_once();
        return;
    }
    if (std::strcmp(cmd, "af_stop") == 0)
    {
        cmd_af_stop();
        return;
    }
    if (std::strcmp(cmd, "af_track") == 0)
    {
        cmd_af_track(argc, av);
        return;
    }
    if (std::strcmp(cmd, "af_status") == 0)
    {
        cmd_af_status();
        return;
    }
    if (std::strcmp(cmd, "af_osd") == 0)
    {
        cmd_af_osd(argc, av);
        return;
    }
    if (std::strcmp(cmd, "af_osd_style") == 0)
    {
        cmd_af_osd_style(argc, av);
        return;
    }
    if (std::strcmp(cmd, "udp_stop") == 0)
    {
        udp_stop_internal();
        std::printf("udp stopped\n");
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "udp_stats") == 0)
    {
        if (!g_udp)
        {
            std::printf("udp not running\n");
            return;
        }
        uint64_t b = g_udp->bytes_sent();
        uint64_t p = g_udp->packets_sent();
        CodecUdpStats st{};
        {
            std::lock_guard<std::mutex> lock(g_codec_udp_stats_mu);
            st = g_codec_udp_stats;
        }
        std::printf("udp codec_idx=%d bytes=%llu packets=%llu\n", g_udp_codec_index,
                    (unsigned long long)b, (unsigned long long)p);
        std::printf("  encoded_pkts=%llu ts_strict_increase_ok=%s ts_regress_or_equal=%llu\n",
                    (unsigned long long)st.pkt_count,
                    (st.ts_not_strictly_increasing == 0) ? "yes" : "no",
                    (unsigned long long)st.ts_not_strictly_increasing);
        if (st.idr_interval_count > 0)
        {
            const uint64_t avg_pkts = st.idr_interval_sum_pkts / st.idr_interval_count;
            const uint64_t avg_ns = st.idr_interval_sum_ns / st.idr_interval_count;
            std::printf(
                "  I_frame: idr_units=%llu intervals=%llu pkt[min avg max]=[%llu %llu %llu] ns[min avg max]=[%llu %llu %llu]\n",
                (unsigned long long)st.idr_seen, (unsigned long long)st.idr_interval_count,
                (unsigned long long)st.idr_interval_min_pkts, (unsigned long long)avg_pkts, (unsigned long long)st.idr_interval_max_pkts,
                (unsigned long long)st.idr_interval_min_ns, (unsigned long long)avg_ns, (unsigned long long)st.idr_interval_max_ns);
        }
        else
        {
            std::printf("  I_frame: idr_units=%llu (need >=2 IDR for interval stats; H264/H265 only)\n",
                        (unsigned long long)st.idr_seen);
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "udp_start") == 0)
    {
        const char *h = (argc >= 2) ? av[1] : "127.0.0.1";
        uint16_t port = (argc >= 3) ? (uint16_t)std::atoi(av[2]) : 5004;
        int idx = g_cidx;
        if (idx < 0 || static_cast<uint32_t>(idx) >= g_codec_count)
        {
            idx = (g_codec_count > 0) ? 0 : -1;
        }
        if (idx < 0)
        {
            std::printf("no codec context\n");
            return;
        }
        int rc = udp_start_for_index(idx, h, port);
        std::printf("udp_start ret=%d\n", rc);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "udp_select") == 0 || std::strcmp(cmd, "udp_push") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: udp_select|udp_push <codec_idx> [host] [port]   # encoder index from \"streams\"\n");
            return;
        }
        int idx = std::atoi(av[1]);
        const char *h = (argc >= 3) ? av[2] : "127.0.0.1";
        uint16_t port = (argc >= 4) ? (uint16_t)std::atoi(av[3]) : 5004;
        int rc = udp_start_for_index(idx, h, port);
        std::printf("%s ret=%d\n", cmd, rc);
        std::fflush(stdout);
        return;
    }

    std::printf("unknown command (try help)\n");
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "Usage: %s <medialib_json_path> [--backup <dir>]\n", argv[0]);
        return 1;
    }

    const char *json_path = argv[1];
    const char *backup_dir = nullptr;
    for (int i = 2; i < argc; i++)
    {
        if ((std::strcmp(argv[i], "--backup") == 0) && (i + 1 < argc))
        {
            backup_dir = argv[i + 1];
            i++;
            continue;
        }
        std::fprintf(stderr, "warning: ignoring extra arg: %s\n", argv[i]);
    }

    install_signal_handlers();

    HalMediaConfig mcfg{};
    mcfg.config_path = json_path;
    mcfg.config_json = nullptr;
    mcfg.image_config = {};
    mcfg.backup_folder_path = backup_dir;

    int rc = HAL_MEDIA_OPS.init(&mcfg, &g_media_ctx);
    if (rc != HAL_OK || !g_media_ctx)
    {
        log_err("HAL_MEDIA_OPS.init failed: %d", rc);
        return 1;
    }

    rc = HAL_MEDIA_OPS.start(g_media_ctx);
    if (rc != HAL_OK)
    {
        log_err("HAL_MEDIA_OPS.start failed: %d", rc);
        (void)HAL_MEDIA_OPS.deinit(g_media_ctx);
        return 1;
    }

    if (refresh_stream_lists() != HAL_OK)
    {
        log_err("refresh_stream_lists failed");
        (void)HAL_MEDIA_OPS.stop(g_media_ctx);
        (void)HAL_MEDIA_OPS.deinit(g_media_ctx);
        return 1;
    }

    std::printf("=== auto_af_test (config=%s) ===\n", json_path);
    print_streams();

    /* Start AF worker thread (idle until commanded) */
    g_af_thread = std::thread(af_thread_main);

    print_help();

    CliHistory history{};
    char line[MAX_LINE];
    while (!g_shutdown.load(std::memory_order_acquire))
    {
        if (g_signal_pending != 0)
        {
            g_signal_pending = 0;
            std::printf("signal: exit\n");
            break;
        }

        if (read_line_raw(line, sizeof(line), &history) != 0)
        {
            break;
        }
        if (line[0] == '\0')
        {
            continue;
        }
        history_add(&history, line);

        char *argvv[32] = {0};
        int argcv = 0;
        char *tok = std::strtok(line, " ");
        while (tok && argcv < 32)
        {
            argvv[argcv++] = tok;
            tok = std::strtok(nullptr, " ");
        }
        if (argcv == 0)
        {
            continue;
        }
        if (std::strcmp(argvv[0], "quit") == 0 || std::strcmp(argvv[0], "exit") == 0)
        {
            break;
        }
        dispatch_line(argcv, argvv);
    }

    /* Shutdown */
    udp_stop_internal();
    g_af_thread_exit.store(true, std::memory_order_release);
    g_af_cv.notify_all();
    if (g_af_thread.joinable())
    {
        g_af_thread.join();
    }

    if (g_af0832)
    {
        hal_lens_af0832_destroy(g_af0832);
        g_af0832 = nullptr;
    }
    if (g_mcu_ctx)
    {
        (void)HAL_MCU_OPS.deinit(g_mcu_ctx);
        g_mcu_ctx = nullptr;
    }

    HAL_LOG_INFO("media stop / deinit");
    (void)HAL_MEDIA_OPS.stop(g_media_ctx);
    (void)HAL_MEDIA_OPS.deinit(g_media_ctx);
    g_media_ctx = nullptr;
    return 0;
}

