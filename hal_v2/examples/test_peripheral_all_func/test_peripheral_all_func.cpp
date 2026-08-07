/**
 * @file test_peripheral_all_func.cpp
 * @brief Interactive exercise of HAL V2 MCU/IO/peripheral device APIs.
 *
 * Usage:
 *   hal-test-peripheral-all-func <serial_device> [baud]
 *
 * Examples (typical hailo15):
 *   hal-test-peripheral-all-func /dev/ttyS0 115200
 *
 * Commands (type 'help'):
 *   mcu_ping <u32>
 *   mcu_echo <hexbytes>              (e.g. mcu_echo deadbeef)
 *   mcu_ver
 *
 *   rtc_get
 *   rtc_set <yy> <mm> <dd> <wday> <hh> <min> <sec>
 *
 *   led_set <id> <0-100>
 *   led_get <id>
 *   ircut_set <day|night>
 *   ircut_get
 *
 *   pd_get | temp_get | ain_get
 *
 *   fan_set <0|1> | fan_get
 *   heat_set <0|1> | heat_get
 *   radar_set <0|1> | radar_get
 *   reset_soc                      (HOST_LINK_CMD_RESET_SOC → SoC PWR_RST)
 *   reset_mcu [0|1]                (default/0: HOST_LINK_CMD_REBOOT; 1: SoC-side GPIO pulse MCU NRST — ne503 gpio 18)
 *
 *   alarm_out_set <ch> <0|1> | alarm_out_get <ch>
 *   wiegand_out_set <ch> <0|1> | wiegand_out_get <ch>
 *   outputs_get
 *   alarm_sub | alarm_unsub
 *
 *   rs485_init <baud> <cfg3>         (cfg3 like "8N1")
 *   rs485_deinit
 *   rs485_tx <hexbytes>
 *   rs485_sub | rs485_unsub
 *
 *   lens_init | lens_deinit
 *   lens_state
 *   lens_cfg <0|1|2>                 (0=all,1=iris,2=motor)
 *   lens_zoom_rz | lens_focus_rz
 *   lens_zoom_abs <pps> <pos> | lens_focus_abs <pps> <pos>
 *   lens_zoom_run <pps> <steps> | lens_focus_run <pps> <steps>
 *   lens_zf_sync_run <zm_pps> <zm_steps> <fs_pps> <fs_steps>
 *   lens_zoom_focus_run <zm_pps> <zm_steps> <fs_pps> <fs_steps>   (async concurrent)
 *   lens_iris_target <0..1023> | lens_iris_adc
 *   lens_sub | lens_unsub
 *
 *   af_create [pps]                  (default pps=1200; used by goto/zf_sync/abs helpers)
 *   af_zoom_abs <pps> <pos> | af_focus_abs <pps> <pos>
 *   af_zoom_run <pps> <steps> | af_focus_run <pps> <steps>
 *   af_zoom_run_async <pps> <steps> | af_focus_run_async <pps> <steps>
 *   af_zoom_abs_async <pps> <pos> | af_focus_abs_async <pps> <pos>
 *   af_wait_zoom [timeout_ms] | af_wait_focus [timeout_ms]
 *   af_zoom_focus_run_async <zm_pps> <zm_steps> <fs_pps> <fs_steps>
 *   af_zf_sync_run <zm_pps> <zm_steps> <fs_pps> <fs_steps>
 *   af_goto <zoom_ratio> <distance_m>   (distance_m<=0 => INF; uses spec table, 1 table step = 4 HAL steps)
 *   af_rz_force                     (force zoom+focus reset-zero)
 *
 *   gpio_export <num> <in|out> <0|1>   (third: 0=high-active / logic matches pin; 1=kernel active-low, logic vs voltage inverted)
 *   gpio_set <num> <0|1>
 *   gpio_get <num>
 *   gpio_sub <num> <none|rising|falling|both>
 *   gpio_unsub <num>
 *   gpio_unexport <num>              (release line; gpio_unsub first if subscribed)
 */

#include "common/hal_log.h"

extern "C" {
#include "peripheral/hal_mcu.h"
#include "peripheral/hal_io.h"
#include "peripheral/devices/hal_rtc.h"
#include "peripheral/devices/hal_led.h"
#include "peripheral/devices/hal_sensor.h"
#include "peripheral/devices/hal_alarm.h"
#include "peripheral/devices/hal_rs485.h"
#include "peripheral/devices/hal_lens.h"
#include "peripheral/devices/hal_env_ctrl.h"
#include "peripheral/devices/hal_lens_af0832.h"
}

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include <inttypes.h>

#include <termios.h>
#include <unistd.h>

namespace {

constexpr size_t MAX_LINE = 512;
constexpr int HISTORY_MAX = 50;

static uint64_t now_monotonic_us()
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static double elapsed_ms(uint64_t t0_us)
{
    return (double)(now_monotonic_us() - t0_us) / 1000.0;
}

/* ------------------------------------------------------------------ */
/* CLI (raw line, history, tab completion; matches test_media_all_func) */
/* ------------------------------------------------------------------ */

static const char *k_commands[] = {
    "help",
    "quit",
    "exit",
    "mcu_ping",
    "mcu_echo",
    "mcu_ver",
    "rtc_get",
    "rtc_set",
    "led_set",
    "led_get",
    "ircut_set",
    "ircut_get",
    "pd_get",
    "temp_get",
    "ain_get",
    "fan_set",
    "fan_get",
    "heat_set",
    "heat_get",
    "radar_set",
    "radar_get",
    "reset_soc",
    "reset_mcu",
    "alarm_out_set",
    "alarm_out_get",
    "wiegand_out_set",
    "wiegand_out_get",
    "outputs_get",
    "alarm_sub",
    "alarm_unsub",
    "rs485_init",
    "rs485_deinit",
    "rs485_tx",
    "rs485_sub",
    "rs485_unsub",
    "lens_init",
    "lens_deinit",
    "lens_state",
    "lens_cfg",
    "lens_zoom_rz",
    "lens_focus_rz",
    "lens_zoom_abs",
    "lens_focus_abs",
    "lens_zoom_run",
    "lens_focus_run",
    "lens_zf_sync_run",
    "lens_zoom_focus_run",
    "lens_iris_target",
    "lens_iris_adc",
    "lens_sub",
    "lens_unsub",
    "af_create",
    "af_destroy",
    "af_bootstrap",
    "af_zoom_abs",
    "af_focus_abs",
    "af_zoom_run",
    "af_focus_run",
    "af_zoom_run_async",
    "af_focus_run_async",
    "af_zoom_abs_async",
    "af_focus_abs_async",
    "af_wait_zoom",
    "af_wait_focus",
    "af_zoom_focus_run_async",
    "af_zf_sync_run",
    "af_goto",
    "af_rz_force",
    "gpio_export",
    "gpio_set",
    "gpio_get",
    "gpio_sub",
    "gpio_unsub",
    "gpio_unexport",
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
    std::printf("\rio> %s", line);
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
            cursor++;
            len++;
            print_prompt_with_cursor(line, cursor);
            continue;
        }
    }

    (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    line[len] = '\0';
    std::printf("\n");
    return 0;
}

static std::vector<std::string> split_ws(const std::string &s)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace((unsigned char)s[i])) i++;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && !std::isspace((unsigned char)s[j])) j++;
        out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

static bool parse_u32(const std::string &s, uint32_t *out)
{
    if (!out) return false;
    char *end = nullptr;
    unsigned long v = std::strtoul(s.c_str(), &end, 0);
    if (end == s.c_str() || *end != 0) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_i32(const std::string &s, int32_t *out)
{
    if (!out) return false;
    char *end = nullptr;
    long v = std::strtol(s.c_str(), &end, 0);
    if (end == s.c_str() || *end != 0) return false;
    *out = (int32_t)v;
    return true;
}

static bool parse_u16(const std::string &s, uint16_t *out)
{
    uint32_t v = 0;
    if (!parse_u32(s, &v) || v > 0xFFFFu) return false;
    *out = (uint16_t)v;
    return true;
}

static bool parse_u8(const std::string &s, uint8_t *out)
{
    uint32_t v = 0;
    if (!parse_u32(s, &v) || v > 0xFFu) return false;
    *out = (uint8_t)v;
    return true;
}

static bool parse_f32(const std::string &s, float *out)
{
    if (!out) return false;
    char *end = nullptr;
    double v = std::strtod(s.c_str(), &end);
    if (end == s.c_str() || *end != 0) return false;
    *out = (float)v;
    return true;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

static bool parse_hex_bytes(const std::string &hex, std::vector<uint8_t> *out)
{
    if (!out) return false;
    out->clear();
    std::string s;
    s.reserve(hex.size());
    for (char c : hex) {
        if (!std::isspace((unsigned char)c)) s.push_back(c);
    }
    if ((s.size() % 2) != 0) return false;
    for (size_t i = 0; i < s.size(); i += 2) {
        int hi = hex_nibble(s[i]);
        int lo = hex_nibble(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out->push_back((uint8_t)((hi << 4) | lo));
    }
    return true;
}

static void print_help()
{
    std::puts("Commands:");
    std::puts("  help | quit | exit");
    std::puts("  mcu_ver");
    std::puts("  mcu_ping <u32>");
    std::puts("  mcu_echo <hexbytes>              (e.g. mcu_echo deadbeef)");
    std::puts("  rtc_get");
    std::puts("  rtc_set <yy> <mm> <dd> <wday> <hh> <min> <sec>");
    std::puts("  led_set <id> <0-100> | led_get <id>");
    std::puts("  ircut_set <day|night> | ircut_get");
    std::puts("  pd_get | temp_get | ain_get");
    std::puts("  fan_set <0|1> | fan_get");
    std::puts("  heat_set <0|1> | heat_get");
    std::puts("  radar_set <0|1> | radar_get");
    std::puts("  reset_soc                      (MCU cmd RESET_SOC → SoC PWR_RST)");
    std::puts("  reset_mcu [0|1]                (0/default: cmd REBOOT; 1: host NRST GPIO — ne503 cfg gpio 18)");
    std::puts("  alarm_out_set <ch> <0|1> | alarm_out_get <ch>");
    std::puts("  wiegand_out_set <ch> <0|1> | wiegand_out_get <ch>");
    std::puts("  outputs_get");
    std::puts("  alarm_sub | alarm_unsub");
    std::puts("  rs485_init <baud> <cfg3>         (cfg3 like \"8N1\")");
    std::puts("  rs485_deinit");
    std::puts("  rs485_tx <hexbytes>");
    std::puts("  rs485_sub | rs485_unsub");
    std::puts("  lens_init | lens_deinit");
    std::puts("  lens_state | lens_cfg <0|1|2>    (0=all,1=iris,2=motor)");
    std::puts("  lens_zoom_rz | lens_focus_rz");
    std::puts("  lens_zoom_abs <pps> <pos> | lens_focus_abs <pps> <pos>");
    std::puts("  lens_zoom_run <pps> <steps> | lens_focus_run <pps> <steps>");
    std::puts("  lens_zf_sync_run <zm_pps> <zm_steps> <fs_pps> <fs_steps>");
    std::puts("  lens_zoom_focus_run <zm_pps> <zm_steps> <fs_pps> <fs_steps>");
    std::puts("  lens_iris_target <0..1023> | lens_iris_adc");
    std::puts("  lens_sub | lens_unsub");
    std::puts("  af_create [pps]                        (default pps=1200)");
    std::puts("  af_zoom_abs <pps> <pos> | af_focus_abs <pps> <pos>");
    std::puts("  af_zoom_run <pps> <steps> | af_focus_run <pps> <steps>");
    std::puts("  af_zoom_run_async <pps> <steps> | af_focus_run_async <pps> <steps>");
    std::puts("  af_zoom_abs_async <pps> <pos> | af_focus_abs_async <pps> <pos>");
    std::puts("  af_wait_zoom [timeout_ms] | af_wait_focus [timeout_ms]");
    std::puts("  af_zoom_focus_run_async <zm_pps> <zm_steps> <fs_pps> <fs_steps>");
    std::puts("  af_zf_sync_run <zm_pps> <zm_steps> <fs_pps> <fs_steps>");
    std::puts("  af_goto <zoom_ratio> <distance_m>      (distance_m<=0 => INF; uses spec table)");
    std::puts("  af_rz_force                            (force zoom+focus reset-zero)");
    std::puts("  gpio_export <num> <in|out> <0|1>");
    std::puts("              third: 0 = normal (logic 1 -> pin high); 1 = ACTIVE_LOW (logic 1 -> pin low)");
    std::puts("  gpio_unexport <num>                    (release; gpio_unsub first if subscribed)");
    std::puts("  gpio_set <num> <0|1> | gpio_get <num>");
    std::puts("  gpio_sub <num> <none|rising|falling|both> | gpio_unsub <num>");
}

static void on_alarm_evt(void *mcu_ctx, uint8_t ch, bool level, void *ud)
{
    (void)mcu_ctx;
    (void)ud;
    std::printf("[ALARM_IN] ch=%u level=%u\n", (unsigned)ch, level ? 1u : 0u);
}

static void on_rs485_rx(void *mcu_ctx, const uint8_t *data, uint16_t len, void *ud)
{
    (void)mcu_ctx;
    (void)ud;
    std::printf("[RS485_RX] len=%u data=", (unsigned)len);
    for (uint16_t i = 0; i < len; i++) {
        std::printf("%02x", data ? data[i] : 0);
    }
    std::printf("\n");
}

static void on_lens_evt(void *mcu_ctx, uint32_t event, int32_t result, int32_t zoom_pos, int32_t focus_pos, void *ud)
{
    (void)mcu_ctx;
    (void)ud;
    std::printf("[LENS_EVT] event=0x%08x result=%d zoom=%d focus=%d\n",
                (unsigned)event, (int)result, (int)zoom_pos, (int)focus_pos);
}

static void on_af_evt(HalLensAf0832 *dev, uint32_t event, int32_t result, int32_t zoom_pos, int32_t focus_pos, void *userdata)
{
    (void)dev;
    (void)userdata;
    std::printf("[AF_EVT] event=0x%08x result=%d zoom=%d focus=%d\n",
                (unsigned)event, (int)result, (int)zoom_pos, (int)focus_pos);
}

static HalGpioEdge parse_edge(const std::string &s)
{
    if (s == "none") return HAL_GPIO_EDGE_NONE;
    if (s == "rising") return HAL_GPIO_EDGE_RISING;
    if (s == "falling") return HAL_GPIO_EDGE_FALLING;
    if (s == "both") return HAL_GPIO_EDGE_BOTH;
    return HAL_GPIO_EDGE_NONE;
}

static void on_gpio_evt(void *io_ctx, uint32_t gpio_num, bool value, void *userdata)
{
    (void)io_ctx;
    (void)userdata;
    std::printf("[GPIO_EVT] gpio=%" PRIu32 " value=%u\n", gpio_num, value ? 1u : 0u);
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "Usage: %s <serial_device> [baud]\n", argv[0]);
        return 2;
    }
    const char *serial = argv[1];
    uint32_t baud = 115200;
    if (argc >= 3) {
        (void)parse_u32(argv[2], &baud);
    }

    void *mcu = nullptr;
    HalMcuConfig mcucfg{};
    mcucfg.serial_device = serial;
    mcucfg.baud_rate = baud;
    mcucfg.timeout_ms = 500;
    /* ne503: gpiochip1 line 2 → HAL 18 (MCU NRST from Linux); used by reset_mcu only. */
    mcucfg.host_mcu_reset_gpio = 18u;
    mcucfg.host_mcu_reset_active_low = 0u;
    mcucfg.host_mcu_reset_pulse_ms = 200u;
    if (HAL_MCU_OPS.init(&mcucfg, &mcu) != HAL_OK) {
        std::fprintf(stderr, "HAL_MCU_OPS.init failed\n");
        return 1;
    }

    void *io = nullptr;
    (void)HAL_IO_OPS.init(&io);

    HalLensAf0832 *af = nullptr;

    std::printf("hal_v2 peripheral cli started, dev=%s baud=%" PRIu32 " (reset_mcu: host_mcu_reset_gpio=%u)\n",
                serial, baud, (unsigned)mcucfg.host_mcu_reset_gpio);
    print_help();

    char line[MAX_LINE];
    CliHistory history{};
    while (true) {
        if (read_line_raw(line, sizeof(line), &history) != 0) {
            break;
        }
        history_add(&history, line);
        std::string cmdline(line);
        auto args = split_ws(cmdline);
        if (args.empty()) continue;

        const std::string &cmd = args[0];
        if (cmd == "help") {
            print_help();
            continue;
        }
        if (cmd == "quit" || cmd == "exit") {
            break;
        }

        auto ok = [&](int r) {
            std::printf("ret=%d (%s)\n", r, hal_error_to_string((HalErrorCode)r));
        };
        auto ok_dt = [&](int r, uint64_t t0_us) {
            std::printf("ret=%d (%s) dt_ms=%.3f\n", r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0_us));
        };

        if (cmd == "mcu_ver") {
            HalMcuVersion v{};
            uint64_t t0 = now_monotonic_us();
            int r = HAL_MCU_OPS.get_version(mcu, &v);
            ok_dt(r, t0);
            std::printf("MCU ver: %d.%d.%d.%d \"%s\"\n", v.major, v.minor, v.patch, v.build, v.version_str);
            continue;
        }
        if (cmd == "mcu_ping" && args.size() >= 2) {
            uint32_t val = 0, echo = 0;
            if (!parse_u32(args[1], &val)) { std::puts("bad arg"); continue; }
            uint64_t t0 = now_monotonic_us();
            int r = HAL_MCU_OPS.ping(mcu, val, &echo);
            std::printf("ret=%d (%s) dt_ms=%.3f echo=%" PRIu32 "\n",
                        r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0), echo);
            continue;
        }
        if (cmd == "mcu_echo" && args.size() >= 2) {
            std::vector<uint8_t> in;
            if (!parse_hex_bytes(args[1], &in)) { std::puts("bad hex"); continue; }
            std::vector<uint8_t> out(512);
            uint16_t out_len = 0;
            uint64_t t0 = now_monotonic_us();
            int r = HAL_MCU_OPS.echo(mcu, in.data(), (uint16_t)in.size(), out.data(), (uint16_t)out.size(), &out_len);
            ok_dt(r, t0);
            std::printf("out_len=%u out=", (unsigned)out_len);
            for (uint16_t i = 0; i < out_len; i++) std::printf("%02x", out[i]);
            std::printf("\n");
            continue;
        }

        if (cmd == "rtc_get") {
            HalRtcTime t{};
            uint64_t t0 = now_monotonic_us();
            int r = HAL_RTC_OPS.get_time(mcu, &t);
            std::printf("ret=%d (%s) dt_ms=%.3f RTC=%02u-%02u-%02u wday=%u %02u:%02u:%02u\n",
                        r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0),
                        t.year, t.month, t.day, t.weekday, t.hour, t.minute, t.second);
            continue;
        }
        if (cmd == "rtc_set" && args.size() >= 8) {
            HalRtcTime t{};
            if (!parse_u8(args[1], &t.year) || !parse_u8(args[2], &t.month) || !parse_u8(args[3], &t.day) ||
                !parse_u8(args[4], &t.weekday) || !parse_u8(args[5], &t.hour) || !parse_u8(args[6], &t.minute) ||
                !parse_u8(args[7], &t.second)) {
                std::puts("bad arg");
                continue;
            }
            uint64_t t0 = now_monotonic_us();
            ok_dt(HAL_RTC_OPS.set_time(mcu, &t), t0);
            continue;
        }

        if (cmd == "led_set" && args.size() >= 3) {
            uint8_t id = 0, duty = 0;
            if (!parse_u8(args[1], &id) || !parse_u8(args[2], &duty)) { std::puts("bad arg"); continue; }
            uint64_t t0 = now_monotonic_us();
            ok_dt(HAL_LED_OPS.led_set_duty(mcu, id, duty), t0);
            continue;
        }
        if (cmd == "led_get" && args.size() >= 2) {
            uint8_t id = 0, duty = 0;
            if (!parse_u8(args[1], &id)) { std::puts("bad arg"); continue; }
            uint64_t t0 = now_monotonic_us();
            int r = HAL_LED_OPS.led_get_duty(mcu, id, &duty);
            std::printf("ret=%d (%s) dt_ms=%.3f duty=%u\n",
                        r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0), (unsigned)duty);
            continue;
        }
        if (cmd == "ircut_set" && args.size() >= 2) {
            HalIrCutMode m = (args[1] == "night") ? HAL_IRCUT_NIGHT : HAL_IRCUT_DAY;
            uint64_t t0 = now_monotonic_us();
            ok_dt(HAL_LED_OPS.ircut_set_mode(mcu, m), t0);
            continue;
        }
        if (cmd == "ircut_get") {
            HalIrCutMode m{};
            uint64_t t0 = now_monotonic_us();
            int r = HAL_LED_OPS.ircut_get_mode(mcu, &m);
            std::printf("ret=%d (%s) dt_ms=%.3f mode=%s\n",
                        r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0),
                        (m == HAL_IRCUT_NIGHT) ? "night" : "day");
            continue;
        }

        if (cmd == "pd_get" || cmd == "temp_get" || cmd == "ain_get") {
            HalAdcValue v{};
            int r = HAL_OK;
            uint64_t t0 = now_monotonic_us();
            if (cmd == "pd_get") r = HAL_SENSOR_OPS.pd_get(mcu, &v);
            if (cmd == "temp_get") r = HAL_SENSOR_OPS.temp_get(mcu, &v);
            if (cmd == "ain_get") r = HAL_SENSOR_OPS.ain_get(mcu, &v);
            std::printf("ret=%d (%s) dt_ms=%.3f mv=%u milli=%d\n",
                        r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0), (unsigned)v.mv, (int)v.milli);
            continue;
        }

        if (cmd == "fan_set" && args.size() >= 2) { uint64_t t0=now_monotonic_us(); ok_dt(HAL_ENV_CTRL_OPS.fan_set(mcu, args[1] != "0"), t0); continue; }
        if (cmd == "fan_get") { bool en=false; uint64_t t0=now_monotonic_us(); int r=HAL_ENV_CTRL_OPS.fan_get(mcu,&en); std::printf("ret=%d (%s) dt_ms=%.3f enable=%u\n", r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0), en?1u:0u); continue; }
        if (cmd == "heat_set" && args.size() >= 2) { uint64_t t0=now_monotonic_us(); ok_dt(HAL_ENV_CTRL_OPS.heat_set(mcu, args[1] != "0"), t0); continue; }
        if (cmd == "heat_get") { bool en=false; uint64_t t0=now_monotonic_us(); int r=HAL_ENV_CTRL_OPS.heat_get(mcu,&en); std::printf("ret=%d (%s) dt_ms=%.3f enable=%u\n", r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0), en?1u:0u); continue; }
        if (cmd == "radar_set" && args.size() >= 2) { uint64_t t0=now_monotonic_us(); ok_dt(HAL_ENV_CTRL_OPS.radar_set(mcu, args[1] != "0"), t0); continue; }
        if (cmd == "radar_get") { bool en=false; uint64_t t0=now_monotonic_us(); int r=HAL_ENV_CTRL_OPS.radar_get(mcu,&en); std::printf("ret=%d (%s) dt_ms=%.3f enable=%u\n", r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0), en?1u:0u); continue; }
        if (cmd == "reset_soc") {
            uint64_t t0 = now_monotonic_us();
            ok_dt(HAL_ENV_CTRL_OPS.reset_soc(mcu), t0);
            continue;
        }
        if (cmd == "reset_mcu") {
            const bool force_gpio = (args.size() >= 2 && args[1] != "0");
            uint64_t t0 = now_monotonic_us();
            ok_dt(HAL_ENV_CTRL_OPS.reset_mcu(mcu, force_gpio), t0);
            continue;
        }

        if (cmd == "alarm_out_set" && args.size() >= 3) {
            uint8_t ch=0; if (!parse_u8(args[1], &ch)) { std::puts("bad arg"); continue; }
            ok(HAL_ALARM_OPS.alarm_out_set(mcu, ch, args[2] != "0")); continue;
        }
        if (cmd == "alarm_out_get" && args.size() >= 2) {
            uint8_t ch=0; if (!parse_u8(args[1], &ch)) { std::puts("bad arg"); continue; }
            bool en=false; ok(HAL_ALARM_OPS.alarm_out_get(mcu, ch, &en)); std::printf("%u\n", en?1u:0u); continue;
        }
        if (cmd == "wiegand_out_set" && args.size() >= 3) {
            uint8_t ch=0; if (!parse_u8(args[1], &ch)) { std::puts("bad arg"); continue; }
            ok(HAL_ALARM_OPS.wiegand_out_set(mcu, ch, args[2] != "0")); continue;
        }
        if (cmd == "wiegand_out_get" && args.size() >= 2) {
            uint8_t ch=0; if (!parse_u8(args[1], &ch)) { std::puts("bad arg"); continue; }
            bool en=false; ok(HAL_ALARM_OPS.wiegand_out_get(mcu, ch, &en)); std::printf("%u\n", en?1u:0u); continue;
        }
        if (cmd == "outputs_get") {
            HalAlarmOutputsState st{}; ok(HAL_ALARM_OPS.outputs_get(mcu,&st));
            std::printf("aout0=%u aout1=%u w0=%u w1=%u\n", st.alarm_out0?1u:0u, st.alarm_out1?1u:0u, st.wiegand0?1u:0u, st.wiegand1?1u:0u);
            continue;
        }
        if (cmd == "alarm_sub") { uint64_t t0=now_monotonic_us(); ok_dt(HAL_ALARM_OPS.subscribe(mcu, on_alarm_evt, nullptr), t0); continue; }
        if (cmd == "alarm_unsub") { uint64_t t0=now_monotonic_us(); ok_dt(HAL_ALARM_OPS.unsubscribe(mcu), t0); continue; }

        if (cmd == "rs485_init" && args.size() >= 3) {
            uint32_t b=0; if (!parse_u32(args[1], &b) || args[2].size()!=3) { std::puts("bad arg"); continue; }
            char cfg[HAL_RS485_CONFIG_LEN]{args[2][0],args[2][1],args[2][2]};
            uint64_t t0=now_monotonic_us(); ok_dt(HAL_RS485_OPS.rs485_init(mcu, b, cfg), t0); continue;
        }
        if (cmd == "rs485_deinit") { uint64_t t0=now_monotonic_us(); ok_dt(HAL_RS485_OPS.rs485_deinit(mcu), t0); continue; }
        if (cmd == "rs485_tx" && args.size() >= 2) {
            std::vector<uint8_t> data; if (!parse_hex_bytes(args[1], &data)) { std::puts("bad hex"); continue; }
            uint64_t t0=now_monotonic_us(); ok_dt(HAL_RS485_OPS.rs485_tx(mcu, data.data(), (uint16_t)data.size()), t0); continue;
        }
        if (cmd == "rs485_sub") { uint64_t t0=now_monotonic_us(); ok_dt(HAL_RS485_OPS.subscribe(mcu, on_rs485_rx, nullptr), t0); continue; }
        if (cmd == "rs485_unsub") { uint64_t t0=now_monotonic_us(); ok_dt(HAL_RS485_OPS.unsubscribe(mcu), t0); continue; }

        if (cmd == "lens_init") { uint64_t t0=now_monotonic_us(); ok_dt(HAL_LENS_OPS.lens_init(mcu), t0); continue; }
        if (cmd == "lens_deinit") { uint64_t t0=now_monotonic_us(); ok_dt(HAL_LENS_OPS.lens_deinit(mcu), t0); continue; }
        if (cmd == "lens_state") { HalLensState st{}; uint64_t t0=now_monotonic_us(); int r=HAL_LENS_OPS.state_get(mcu,&st); std::printf("ret=%d (%s) dt_ms=%.3f zoom=%d focus=%d zpos=%d fpos=%d zrz=%u frz=%u\n", r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0), (int)st.zoom_state,(int)st.focus_state,(int)st.zoom_pos,(int)st.focus_pos,st.zoom_rz_done?1u:0u,st.focus_rz_done?1u:0u); continue; }
        if (cmd == "lens_cfg" && args.size() >= 2) { uint32_t m=0; if (!parse_u32(args[1], &m)) { std::puts("bad arg"); continue; } uint64_t t0=now_monotonic_us(); ok_dt(HAL_LENS_OPS.lens_config(mcu,(HalLensMode)m), t0); continue; }
        if (cmd == "lens_zoom_rz") { uint64_t t0=now_monotonic_us(); ok_dt(HAL_LENS_OPS.zoom_rz(mcu), t0); continue; }
        if (cmd == "lens_focus_rz") { uint64_t t0=now_monotonic_us(); ok_dt(HAL_LENS_OPS.focus_rz(mcu), t0); continue; }
        if ((cmd == "lens_zoom_abs" || cmd == "lens_focus_abs" || cmd == "lens_zoom_run" || cmd == "lens_focus_run") && args.size() >= 3) {
            uint16_t pps=0; int32_t v=0;
            if (!parse_u16(args[1], &pps) || !parse_i32(args[2], &v)) { std::puts("bad arg"); continue; }
            HalLensMotion m{.pps=pps,.value=v};
            uint64_t t0=now_monotonic_us();
            if (cmd == "lens_zoom_abs") ok_dt(HAL_LENS_OPS.zoom_abs(mcu,&m), t0);
            if (cmd == "lens_focus_abs") ok_dt(HAL_LENS_OPS.focus_abs(mcu,&m), t0);
            if (cmd == "lens_zoom_run") ok_dt(HAL_LENS_OPS.zoom_run(mcu,&m), t0);
            if (cmd == "lens_focus_run") ok_dt(HAL_LENS_OPS.focus_run(mcu,&m), t0);
            continue;
        }
        if (cmd == "lens_zf_sync_run" && args.size() >= 5) {
            uint16_t zm_pps=0, fs_pps=0; int32_t zm_steps=0, fs_steps=0;
            if (!parse_u16(args[1], &zm_pps) || !parse_i32(args[2], &zm_steps) ||
                !parse_u16(args[3], &fs_pps) || !parse_i32(args[4], &fs_steps)) { std::puts("bad arg"); continue; }
            HalLensZfSync p{.zm_pps=zm_pps, .zm_micro_steps=zm_steps, .fs_pps=fs_pps, .fs_micro_steps=fs_steps};
            uint64_t t0=now_monotonic_us();
            ok_dt(HAL_LENS_OPS.zf_sync_run(mcu, &p), t0);
            continue;
        }
        if (cmd == "lens_zoom_focus_run" && args.size() >= 5) {
            /* Async concurrent: issue zoom_run and focus_run back-to-back.
             * With new MCU firmware both axes can run simultaneously —
             * the driver coordinates VD_FZ pulses internally. */
            uint16_t zm_pps=0, fs_pps=0; int32_t zm_steps=0, fs_steps=0;
            if (!parse_u16(args[1], &zm_pps) || !parse_i32(args[2], &zm_steps) ||
                !parse_u16(args[3], &fs_pps) || !parse_i32(args[4], &fs_steps)) { std::puts("bad arg"); continue; }
            uint64_t t0=now_monotonic_us();
            int rz = HAL_OK, rf = HAL_OK;
            if (zm_steps != 0) {
                HalLensMotion mz{.pps=zm_pps, .value=zm_steps};
                rz = HAL_LENS_OPS.zoom_run(mcu, &mz);
            }
            if (fs_steps != 0) {
                HalLensMotion mf{.pps=fs_pps, .value=fs_steps};
                rf = HAL_LENS_OPS.focus_run(mcu, &mf);
            }
            std::printf("zoom_run ret=%d (%s) focus_run ret=%d (%s) dt_ms=%.3f\n",
                        rz, hal_error_to_string((HalErrorCode)rz),
                        rf, hal_error_to_string((HalErrorCode)rf), elapsed_ms(t0));
            continue;
        }
        if (cmd == "lens_iris_target" && args.size() >= 2) { uint16_t t=0; if(!parse_u16(args[1],&t)) { std::puts("bad arg"); continue; } uint64_t t0=now_monotonic_us(); ok_dt(HAL_LENS_OPS.iris_target_set(mcu,t), t0); continue; }
        if (cmd == "lens_iris_adc") { uint16_t adc=0; uint64_t t0=now_monotonic_us(); int r=HAL_LENS_OPS.iris_adc_get(mcu,&adc); std::printf("ret=%d (%s) dt_ms=%.3f adc=%u\n", r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0), (unsigned)adc); continue; }
        if (cmd == "lens_sub") { uint64_t t0=now_monotonic_us(); ok_dt(HAL_LENS_OPS.subscribe(mcu, on_lens_evt, nullptr), t0); continue; }
        if (cmd == "lens_unsub") { uint64_t t0=now_monotonic_us(); ok_dt(HAL_LENS_OPS.unsubscribe(mcu), t0); continue; }

        if (cmd == "af_create") {
            if (af) { std::puts("af already created"); continue; }
            HalLensAf0832Params p; hal_lens_af0832_params_init_defaults(&p);
            if (args.size() >= 2) {
                uint16_t pps = 0;
                if (parse_u16(args[1], &pps) && pps >= 24 && pps <= 4000) {
                    p.default_pps = pps;
                } else {
                    std::puts("bad pps (valid: 24..4000)");
                    continue;
                }
            }
            uint64_t t0=now_monotonic_us();
            int r = hal_lens_af0832_create(mcu, &p, &af);
            ok_dt(r, t0);
            if (af) { uint64_t t1=now_monotonic_us(); ok_dt(hal_lens_af0832_set_event_callback(af, on_af_evt, nullptr), t1); }
            if (r == HAL_OK) std::printf("af0832 created: pps=%u zoom=[%d,%d] focus=[%d,%d]\n",
                (unsigned)p.default_pps, (int)p.zoom_limit.min_pos, (int)p.zoom_limit.max_pos,
                (int)p.focus_limit.min_pos, (int)p.focus_limit.max_pos);
            continue;
        }
        if (cmd == "af_destroy") { if (!af) { std::puts("af not created"); continue; } uint64_t t0=now_monotonic_us(); hal_lens_af0832_destroy(af); af=nullptr; std::printf("ret=%d (%s) dt_ms=%.3f\n", HAL_OK, hal_error_to_string((HalErrorCode)HAL_OK), elapsed_ms(t0)); continue; }
        if (cmd == "af_bootstrap") { if (!af) { std::puts("af not created"); continue; } uint64_t t0=now_monotonic_us(); ok_dt(hal_lens_af0832_bootstrap(af), t0); continue; }
        if ((cmd == "af_zoom_abs" || cmd == "af_focus_abs") && args.size() >= 3) {
            if (!af) { std::puts("af not created"); continue; }
            uint16_t pps=0; int32_t pos=0;
            if (!parse_u16(args[1], &pps) || !parse_i32(args[2], &pos)) { std::puts("bad arg"); continue; }
            HalLensMotion m{.pps=pps,.value=pos};
            uint64_t t0=now_monotonic_us();
            if (cmd == "af_zoom_abs") ok_dt(hal_lens_af0832_zoom_abs(af, &m), t0);
            if (cmd == "af_focus_abs") ok_dt(hal_lens_af0832_focus_abs(af, &m), t0);
            continue;
        }
        if ((cmd == "af_zoom_run" || cmd == "af_focus_run") && args.size() >= 3) {
            if (!af) { std::puts("af not created"); continue; }
            uint16_t pps=0; int32_t steps=0;
            if (!parse_u16(args[1], &pps) || !parse_i32(args[2], &steps)) { std::puts("bad arg"); continue; }
            HalLensMotion m{.pps=pps,.value=steps};
            uint64_t t0=now_monotonic_us();
            if (cmd == "af_zoom_run") ok_dt(hal_lens_af0832_zoom_run(af, &m), t0);
            if (cmd == "af_focus_run") ok_dt(hal_lens_af0832_focus_run(af, &m), t0);
            continue;
        }
        if ((cmd == "af_zoom_run_async" || cmd == "af_focus_run_async" || cmd == "af_zoom_abs_async" || cmd == "af_focus_abs_async") && args.size() >= 3) {
            if (!af) { std::puts("af not created"); continue; }
            uint16_t pps=0; int32_t v=0;
            if (!parse_u16(args[1], &pps) || !parse_i32(args[2], &v)) { std::puts("bad arg"); continue; }
            HalLensMotion m{.pps=pps,.value=v};
            uint64_t t0=now_monotonic_us();
            if (cmd == "af_zoom_run_async") ok_dt(hal_lens_af0832_zoom_run_async(af, &m), t0);
            if (cmd == "af_focus_run_async") ok_dt(hal_lens_af0832_focus_run_async(af, &m), t0);
            if (cmd == "af_zoom_abs_async") ok_dt(hal_lens_af0832_zoom_abs_async(af, &m), t0);
            if (cmd == "af_focus_abs_async") ok_dt(hal_lens_af0832_focus_abs_async(af, &m), t0);
            continue;
        }
        if (cmd == "af_wait_zoom" || cmd == "af_wait_focus") {
            if (!af) { std::puts("af not created"); continue; }
            uint32_t to_ms = 60000;
            if (args.size() >= 2) { uint32_t v=0; if (parse_u32(args[1], &v)) to_ms = v; }
            uint64_t t0=now_monotonic_us();
            int32_t result = 0;
            int r;
            if (cmd == "af_wait_zoom") r = hal_lens_af0832_wait_zoom(af, to_ms, &result);
            else                      r = hal_lens_af0832_wait_focus(af, to_ms, &result);
            std::printf("ret=%d (%s) result=%d dt_ms=%.3f\n", r, hal_error_to_string((HalErrorCode)r), (int)result, elapsed_ms(t0));
            continue;
        }
        if (cmd == "af_zoom_focus_run_async" && args.size() >= 5) {
            if (!af) { std::puts("af not created"); continue; }
            uint16_t zm_pps=0, fs_pps=0; int32_t zm_steps=0, fs_steps=0;
            if (!parse_u16(args[1], &zm_pps) || !parse_i32(args[2], &zm_steps) ||
                !parse_u16(args[3], &fs_pps) || !parse_i32(args[4], &fs_steps)) { std::puts("bad arg"); continue; }
            uint64_t t0=now_monotonic_us();
            int rz = HAL_OK, rf = HAL_OK;
            if (zm_steps != 0) {
                HalLensMotion mz{.pps=zm_pps, .value=zm_steps};
                rz = hal_lens_af0832_zoom_run_async(af, &mz);
            }
            if (fs_steps != 0) {
                HalLensMotion mf{.pps=fs_pps, .value=fs_steps};
                rf = hal_lens_af0832_focus_run_async(af, &mf);
            }
            std::printf("zoom_async ret=%d (%s) focus_async ret=%d (%s) --> both axes running concurrently, use af_wait_zoom/af_wait_focus to synchronise, dt_ms=%.3f\n",
                        rz, hal_error_to_string((HalErrorCode)rz),
                        rf, hal_error_to_string((HalErrorCode)rf), elapsed_ms(t0));
            continue;
        }
        if (cmd == "af_zf_sync_run" && args.size() >= 5) {
            if (!af) { std::puts("af not created"); continue; }
            uint16_t zm_pps=0, fs_pps=0; int32_t zm_steps=0, fs_steps=0;
            if (!parse_u16(args[1], &zm_pps) || !parse_i32(args[2], &zm_steps) ||
                !parse_u16(args[3], &fs_pps) || !parse_i32(args[4], &fs_steps)) { std::puts("bad arg"); continue; }
            HalLensZfSync p{.zm_pps=zm_pps, .zm_micro_steps=zm_steps, .fs_pps=fs_pps, .fs_micro_steps=fs_steps};
            uint64_t t0=now_monotonic_us();
            ok_dt(hal_lens_af0832_zf_sync_run(af, &p), t0);
            continue;
        }
        if (cmd == "af_goto" && args.size() >= 3) {
            if (!af) { std::puts("af not created"); continue; }
            float ratio = 0.0f;
            float dist_m = 0.0f;
            if (!parse_f32(args[1], &ratio) || !parse_f32(args[2], &dist_m)) { std::puts("bad arg"); continue; }
            uint64_t t0 = now_monotonic_us();
            ok_dt(hal_lens_af0832_goto_by_ratio_distance(af, ratio, dist_m), t0);
            continue;
        }
        if (cmd == "af_rz_force") {
            if (!af) { std::puts("af not created"); continue; }
            uint64_t t0 = now_monotonic_us();
            ok_dt(hal_lens_af0832_force_reset_zero(af), t0);
            continue;
        }

        if (cmd == "gpio_export" && args.size() >= 4) {
            uint32_t num=0; if (!parse_u32(args[1], &num)) { std::puts("bad arg"); continue; }
            HalGpioConfig c{}; c.gpio_num = num;
            c.direction = (args[2] == "out") ? HAL_GPIO_DIR_OUTPUT : HAL_GPIO_DIR_INPUT;
            /* "0" -> false: logic tracks pin voltage. Non-"0" -> ACTIVE_LOW in gpiod (inverted). */
            c.active_low = (args[3] != "0");
            uint64_t t0=now_monotonic_us();
            ok_dt(HAL_IO_OPS.gpio_export(io, &c), t0);
            continue;
        }
        if (cmd == "gpio_set" && args.size() >= 3) { uint32_t num=0; if(!parse_u32(args[1],&num)) { std::puts("bad arg"); continue; } uint64_t t0=now_monotonic_us(); ok_dt(HAL_IO_OPS.gpio_set_value(io,num,args[2]!="0"), t0); continue; }
        if (cmd == "gpio_get" && args.size() >= 2) { uint32_t num=0; if(!parse_u32(args[1],&num)) { std::puts("bad arg"); continue; } bool v=false; uint64_t t0=now_monotonic_us(); int r=HAL_IO_OPS.gpio_get_value(io,num,&v); std::printf("ret=%d (%s) dt_ms=%.3f value=%u\n", r, hal_error_to_string((HalErrorCode)r), elapsed_ms(t0), v?1u:0u); continue; }
        if (cmd == "gpio_sub" && args.size() >= 3) { uint32_t num=0; if(!parse_u32(args[1],&num)) { std::puts("bad arg"); continue; } uint64_t t0=now_monotonic_us(); ok_dt(HAL_IO_OPS.gpio_subscribe(io,num,parse_edge(args[2]),on_gpio_evt,nullptr), t0); continue; }
        if (cmd == "gpio_unsub" && args.size() >= 2) { uint32_t num=0; if(!parse_u32(args[1],&num)) { std::puts("bad arg"); continue; } uint64_t t0=now_monotonic_us(); ok_dt(HAL_IO_OPS.gpio_unsubscribe(io,num), t0); continue; }
        if (cmd == "gpio_unexport" && args.size() >= 2) {
            uint32_t num = 0;
            if (!parse_u32(args[1], &num)) {
                std::puts("bad arg");
                continue;
            }
            uint64_t t0 = now_monotonic_us();
            ok_dt(HAL_IO_OPS.gpio_unexport(io, num), t0);
            continue;
        }

        std::puts("unknown or invalid command, try: help");
    }

    if (af) {
        hal_lens_af0832_destroy(af);
        af = nullptr;
    }
    if (io) {
        (void)HAL_IO_OPS.deinit(io);
        io = nullptr;
    }
    (void)HAL_MCU_OPS.deinit(mcu);
    return 0;
}

