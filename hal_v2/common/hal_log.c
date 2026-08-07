/**
 * @file hal_log.c
 * @brief Log HAL implementation - unified logging for multiple SoC platforms
 */

#include "common/hal_log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>

/* ANSI color codes for console output */
#define HAL_LOG_COLOR_RESET   "\033[0m"
#define HAL_LOG_COLOR_DEBUG   "\033[36m"  /* cyan */
#define HAL_LOG_COLOR_INFO    "\033[32m"  /* green */
#define HAL_LOG_COLOR_WARNING "\033[33m"  /* yellow */
#define HAL_LOG_COLOR_ERROR   "\033[31m"  /* red */

#define HAL_LOG_MAX_MESSAGE   2048
#define HAL_LOG_MAX_PATH      256
#define HAL_LOG_MAX_ROTATE_PATH (HAL_LOG_MAX_PATH + 16)  /* path + ".%d" + null, avoid format-truncation */
#define HAL_LOG_HEX_BYTES_PER_LINE 16
static const char* const level_names[] = { "DEBUG", "INFO", "WARNING", "ERROR" };

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Runtime config and state. config.file_path always points to file_path (our copy). */
static struct {
    HalLogConfig config;
    FILE* log_file;
    int current_file_size;
    char file_path[HAL_LOG_MAX_PATH];  /* owned full path for open/rotation */
} g_log = {
    .config = HAL_LOG_DEFAULT_CONFIG,
    .log_file = NULL,
    .current_file_size = 0,
    .file_path = { 0 }
};

static void build_full_log_path(const char* path, char* full_path, int cap);
static void create_log_dir_if_needed(const char* full_path);
static void ensure_file_open(void);
static void maybe_rotate_file(int append_size);
static void write_to_file(const char* buf, int len);
static const char* get_color_for_level(int level);
static void format_timestamp(char* buf, int cap);

/* Resolve path: if directory, append default name "hal_yymmdd_hhmmss.log". */
static void build_full_log_path(const char* path, char* full_path, int cap)
{
    if (!full_path || cap <= 0) return;
    full_path[0] = '\0';
    if (!path) path = ".";

    int is_dir = 0;
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] == '/')
        is_dir = 1;
    else {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISDIR(st.st_mode))
            is_dir = 1;
    }

    if (is_dir) {
        time_t t = time(NULL);
        struct tm* tm = localtime(&t);
        if (tm) {
            const char* dir = path;
            if (len > 0 && path[len - 1] == '/')
                snprintf(full_path, (size_t)cap, "%shal_%02d%02d%02d_%02d%02d%02d.log",
                         dir, tm->tm_year % 100, tm->tm_mon + 1, tm->tm_mday,
                         tm->tm_hour, tm->tm_min, tm->tm_sec);
            else
                snprintf(full_path, (size_t)cap, "%s/hal_%02d%02d%02d_%02d%02d%02d.log",
                         dir, tm->tm_year % 100, tm->tm_mon + 1, tm->tm_mday,
                         tm->tm_hour, tm->tm_min, tm->tm_sec);
        } else
            snprintf(full_path, (size_t)cap, "%s/hal_000101_000000.log", path);
    } else {
        strncpy(full_path, path, (size_t)(cap - 1));
        full_path[cap - 1] = '\0';
    }
}

/* Create directory for log file only if parent exists (single-level mkdir, no recursive). */
static void create_log_dir_if_needed(const char* full_path)
{
    if (!full_path || !full_path[0]) return;
    const char* last_slash = strrchr(full_path, '/');
    if (!last_slash || last_slash == full_path) return;  /* no dir part or root "/" */
    size_t dir_len = (size_t)(last_slash - full_path);
    if (dir_len >= HAL_LOG_MAX_PATH) return;
    char dir_buf[HAL_LOG_MAX_PATH];
    memcpy(dir_buf, full_path, dir_len);
    dir_buf[dir_len] = '\0';
    struct stat st;
    if (stat(dir_buf, &st) == 0 && S_ISDIR(st.st_mode)) return;  /* dir exists */
    /* Dir missing. Create only if parent exists (single level, no recursive). */
    char* parent_slash = strrchr(dir_buf, '/');
    if (!parent_slash || parent_slash == dir_buf) return;  /* no parent or parent is "/" */
    *parent_slash = '\0';
    if (stat(dir_buf, &st) != 0 || !S_ISDIR(st.st_mode)) {
        *parent_slash = '/';
        return;  /* parent missing, do not create */
    }
    *parent_slash = '/';
    (void)mkdir(dir_buf, 0755);
}

int hal_log_get_config(HalLogConfig* config)
{
    if (!config) return -1;
    pthread_mutex_lock(&g_mutex);
    memcpy(config, &g_log.config, sizeof(HalLogConfig));
    config->file_path = g_log.file_path[0] ? g_log.file_path : NULL;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

static int validate_and_apply_config(const HalLogConfig* config)
{
    if (config->level < HAL_LOG_LEVEL_DEBUG || config->level > HAL_LOG_LEVEL_ERROR)
        return -1;
    if (config->file_max_size < 0 || config->file_max_count < 0)
        return -1;

    g_log.config.level = config->level;
    g_log.config.color_enabled = config->color_enabled ? 1 : 0;
    g_log.config.timestamp_enabled = config->timestamp_enabled ? 1 : 0;
    g_log.config.console_enabled = config->console_enabled ? 1 : 0;
    g_log.config.file_enabled = config->file_enabled ? 1 : 0;
    g_log.config.file_max_size = config->file_max_size > 0 ? config->file_max_size : 1024 * 1024;
    g_log.config.file_max_count = config->file_max_count > 0 ? config->file_max_count : 10;
    g_log.config.user_output = config->user_output;

    /* Never store external pointer: resolve path and keep our own copy */
    g_log.config.file_path = NULL;
    g_log.file_path[0] = '\0';
    if (g_log.config.file_enabled) {
        const char* path = config->file_path && config->file_path[0] ? config->file_path : ".";
        build_full_log_path(path, g_log.file_path, HAL_LOG_MAX_PATH);
        g_log.config.file_path = g_log.file_path;
    }
    return 0;
}

int hal_log_set_config(HalLogConfig* config)
{
    if (!config) return -1;
    pthread_mutex_lock(&g_mutex);
    if (validate_and_apply_config(config) != 0) {
        pthread_mutex_unlock(&g_mutex);
        return -1;
    }
    if (g_log.log_file) {
        fclose(g_log.log_file);
        g_log.log_file = NULL;
    }
    g_log.current_file_size = 0;
    if (g_log.config.file_enabled && g_log.file_path[0])
        ensure_file_open();
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

int hal_log_set_level(int level)
{
    if (level < HAL_LOG_LEVEL_DEBUG || level > HAL_LOG_LEVEL_ERROR)
        return -1;
    pthread_mutex_lock(&g_mutex);
    g_log.config.level = level;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

int hal_log_set_color(int color_enabled)
{
    pthread_mutex_lock(&g_mutex);
    g_log.config.color_enabled = color_enabled ? 1 : 0;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

int hal_log_set_timestamp(int timestamp_enabled)
{
    pthread_mutex_lock(&g_mutex);
    g_log.config.timestamp_enabled = timestamp_enabled ? 1 : 0;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

int hal_log_set_console(int console_enabled)
{
    pthread_mutex_lock(&g_mutex);
    g_log.config.console_enabled = console_enabled ? 1 : 0;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

int hal_log_set_file(int file_enabled, const char* file_path, int file_max_size, int file_max_count)
{
    pthread_mutex_lock(&g_mutex);
    g_log.config.file_enabled = file_enabled ? 1 : 0;
    g_log.config.file_max_size = file_max_size > 0 ? file_max_size : 1024 * 1024;
    g_log.config.file_max_count = file_max_count > 0 ? file_max_count : 10;

    g_log.config.file_path = NULL;
    g_log.file_path[0] = '\0';
    if (g_log.log_file) {
        fclose(g_log.log_file);
        g_log.log_file = NULL;
    }
    g_log.current_file_size = 0;

    if (file_enabled) {
        const char* path = (file_path && file_path[0]) ? file_path : ".";
        build_full_log_path(path, g_log.file_path, HAL_LOG_MAX_PATH);
        g_log.config.file_path = g_log.file_path;
        ensure_file_open();
    }
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

int hal_log_set_user_output(void (*user_output)(int level, int line, const char* file, const char* message))
{
    pthread_mutex_lock(&g_mutex);
    g_log.config.user_output = user_output;
    pthread_mutex_unlock(&g_mutex);
    return 0;
}

static void ensure_file_open(void)
{
    if (g_log.log_file || !g_log.config.file_enabled || !g_log.file_path[0])
        return;
    create_log_dir_if_needed(g_log.file_path);
    g_log.log_file = fopen(g_log.file_path, "a");
    if (g_log.log_file) {
        fseek(g_log.log_file, 0, SEEK_END);
        g_log.current_file_size = (int)ftell(g_log.log_file);
    }
}

/* Rotate: current -> .1, .1 -> .2, ... discard .max_count. Caller holds g_mutex. */
static void maybe_rotate_file(int append_size)
{
    int max_size = g_log.config.file_max_size;
    int max_count = g_log.config.file_max_count;
    if (max_size <= 0 || g_log.current_file_size + append_size <= max_size)
        return;
    if (!g_log.log_file)
        return;

    fclose(g_log.log_file);
    g_log.log_file = NULL;
    g_log.current_file_size = 0;

    char old_name[HAL_LOG_MAX_ROTATE_PATH];
    char new_name[HAL_LOG_MAX_ROTATE_PATH];

    for (int i = max_count - 1; i >= 1; i--) {
        (void)snprintf(old_name, sizeof(old_name), "%s.%d", g_log.file_path, i);
        (void)snprintf(new_name, sizeof(new_name), "%s.%d", g_log.file_path, i + 1);
        rename(old_name, new_name);
    }
    (void)snprintf(new_name, sizeof(new_name), "%s.1", g_log.file_path);
    rename(g_log.file_path, new_name);

    g_log.log_file = fopen(g_log.file_path, "a");
}

static void write_to_file(const char* buf, int len)
{
    if (!g_log.log_file || len <= 0)
        return;
    maybe_rotate_file(len);
    if (g_log.log_file) {
        size_t n = fwrite(buf, 1, (size_t)len, g_log.log_file);
        if (n > 0) {
            g_log.current_file_size += (int)n;
            fflush(g_log.log_file);
        }
    }
}

static const char* get_color_for_level(int level)
{
    switch (level) {
        case HAL_LOG_LEVEL_DEBUG:   return HAL_LOG_COLOR_DEBUG;
        case HAL_LOG_LEVEL_INFO:    return HAL_LOG_COLOR_INFO;
        case HAL_LOG_LEVEL_WARNING: return HAL_LOG_COLOR_WARNING;
        case HAL_LOG_LEVEL_ERROR:   return HAL_LOG_COLOR_ERROR;
        default:                    return HAL_LOG_COLOR_RESET;
    }
}

static void format_timestamp(char* buf, int cap)
{
    time_t t = time(NULL);
    struct tm* tm = localtime(&t);
    if (tm)
        strftime(buf, (size_t)cap, "%Y-%m-%d %H:%M:%S", tm);
    else
        buf[0] = '\0';
}

int hal_log_message(int level, int line, const char* file, const char* message, ...)
{
    pthread_mutex_lock(&g_mutex);
    if (level < g_log.config.level) {
        pthread_mutex_unlock(&g_mutex);
        return 0;
    }

    char msg_buf[HAL_LOG_MAX_MESSAGE];
    char ts_buf[32];
    char out_buf[HAL_LOG_MAX_MESSAGE + 128];
    va_list ap;
    va_start(ap, message);
    vsnprintf(msg_buf, sizeof(msg_buf), message, ap);
    va_end(ap);

    const char* level_str = level_names[level <= HAL_LOG_LEVEL_ERROR ? level : HAL_LOG_LEVEL_ERROR];
    const char* base_file = file ? strrchr(file, '/') : NULL;
    if (base_file)
        base_file++;
    else
        base_file = file ? file : "";

    if (g_log.config.timestamp_enabled) {
        format_timestamp(ts_buf, sizeof(ts_buf));
        snprintf(out_buf, sizeof(out_buf), "[%s] [%s] [%s:%d] %s\n", ts_buf, level_str, base_file, line, msg_buf);
    } else {
        snprintf(out_buf, sizeof(out_buf), "[%s] [%s:%d] %s\n", level_str, base_file, line, msg_buf);
    }

    int out_len = (int)strlen(out_buf);

    if (g_log.config.console_enabled) {
        if (g_log.config.color_enabled)
            fprintf(stderr, "%s%s%s", get_color_for_level(level), out_buf, HAL_LOG_COLOR_RESET);
        else
            fputs(out_buf, stderr);
    }

    if (g_log.config.file_enabled)
        write_to_file(out_buf, out_len);

    void (*user_output)(int, int, const char*, const char*) = g_log.config.user_output;
    pthread_mutex_unlock(&g_mutex);

    if (user_output)
        user_output(level, line, file, msg_buf);

    return 0;
}

int hal_log_hexdump(int level, const void* data, int size)
{
    pthread_mutex_lock(&g_mutex);
    int min_level = g_log.config.level;
    pthread_mutex_unlock(&g_mutex);

    if (level < min_level || !data || size <= 0)
        return 0;

    const unsigned char* p = (const unsigned char*)data;
    char line_buf[80];
    char msg_buf[HAL_LOG_MAX_MESSAGE];
    int offset = 0;

    while (size > 0) {
        int chunk = size < HAL_LOG_HEX_BYTES_PER_LINE ? size : HAL_LOG_HEX_BYTES_PER_LINE;
        int pos = 0;
        pos += snprintf(line_buf + pos, sizeof(line_buf) - pos, "%04X  ", offset);
        for (int i = 0; i < HAL_LOG_HEX_BYTES_PER_LINE; i++) {
            if (i < chunk)
                pos += snprintf(line_buf + pos, sizeof(line_buf) - pos, "%02X ", p[i]);
            else
                pos += snprintf(line_buf + pos, sizeof(line_buf) - pos, "   ");
        }
        pos += snprintf(line_buf + pos, sizeof(line_buf) - pos, " |");
        for (int i = 0; i < chunk; i++)
            pos += snprintf(line_buf + pos, sizeof(line_buf) - pos, "%c", p[i] >= 32 && p[i] < 127 ? p[i] : '.');
        pos += snprintf(line_buf + pos, sizeof(line_buf) - pos, "|");

        snprintf(msg_buf, sizeof(msg_buf), "%s", line_buf);
        hal_log_message(level, 0, "", "%s", msg_buf);

        p += chunk;
        offset += chunk;
        size -= chunk;
    }
    return 0;
}
