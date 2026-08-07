/**
 * @file hal_log.h
 * @brief HAL Logging Interface
 *
 * Unified logging subsystem with console, file, and user-callback outputs.
 * Thread-safe; supports log-level filtering, ANSI color, file rotation,
 * and hex-dump utilities.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Log severity levels (lower value = more verbose) */
#define HAL_LOG_LEVEL_DEBUG     0
#define HAL_LOG_LEVEL_INFO      1
#define HAL_LOG_LEVEL_WARNING   2
#define HAL_LOG_LEVEL_ERROR     3

/* Default log configuration (INFO level, console only, with color & timestamp) */
#define HAL_LOG_DEFAULT_CONFIG { \
    .level = HAL_LOG_LEVEL_INFO, \
    .color_enabled = 1, \
    .timestamp_enabled = 1, \
    .console_enabled = 1, \
    .file_enabled = 0, \
    .file_path = NULL, \
    .file_max_size = 1024 * 1024, \
    .file_max_count = 10, \
    .user_output = NULL \
}

/* Convenience macros - automatically inject source file and line number */
#define HAL_LOG_DEBUG(format, ...)      hal_log_message(HAL_LOG_LEVEL_DEBUG, __LINE__, __FILE__, format, ##__VA_ARGS__)
#define HAL_LOG_INFO(format, ...)       hal_log_message(HAL_LOG_LEVEL_INFO, __LINE__, __FILE__, format, ##__VA_ARGS__)
#define HAL_LOG_WARNING(format, ...)    hal_log_message(HAL_LOG_LEVEL_WARNING, __LINE__, __FILE__, format, ##__VA_ARGS__)
#define HAL_LOG_ERROR(format, ...)      hal_log_message(HAL_LOG_LEVEL_ERROR, __LINE__, __FILE__, format, ##__VA_ARGS__)
#define HAL_DEBUG_HEX(data, size)       hal_log_hexdump(HAL_LOG_LEVEL_DEBUG, data, size)
#define HAL_INFO_HEX(data, size)        hal_log_hexdump(HAL_LOG_LEVEL_INFO, data, size)
#define HAL_WARNING_HEX(data, size)     hal_log_hexdump(HAL_LOG_LEVEL_WARNING, data, size)
#define HAL_ERROR_HEX(data, size)       hal_log_hexdump(HAL_LOG_LEVEL_ERROR, data, size)

/**
 * Logging configuration.
 * All fields can be set individually via setter functions or atomically via
 * hal_log_set_config(). Changes take effect on the next log call.
 */
typedef struct {
    int level;                  /* minimum level to output (HAL_LOG_LEVEL_*) */
    int color_enabled;          /* non-zero: use ANSI color codes on console */
    int timestamp_enabled;      /* non-zero: prepend timestamp to each line */
    int console_enabled;        /* non-zero: write to stdout/stderr */
    int file_enabled;           /* non-zero: write to log file */
    const char* file_path;      /* log file path (NULL = auto-generate in CWD) */
    int file_max_size;          /* max size per log file in bytes before rotation */
    int file_max_count;         /* max number of rotated log files to keep */
    void (*user_output)(int level, int line, const char* file, const char* message);
                                /* optional user callback for custom log sinks */
} HalLogConfig;

/* --- Configuration API --- */
int hal_log_get_config(HalLogConfig* config);
int hal_log_set_config(HalLogConfig* config);
int hal_log_set_level(int level);
int hal_log_set_color(int color_enabled);
int hal_log_set_timestamp(int timestamp_enabled);
int hal_log_set_console(int console_enabled);
int hal_log_set_file(int file_enabled, const char* file_path, int file_max_size, int file_max_count);
int hal_log_set_user_output(void (*user_output)(int level, int line, const char* file, const char* message));

/* --- Output API --- */

/**
 * @brief Emit a formatted log message.
 * @param level   Severity level (HAL_LOG_LEVEL_*).
 * @param line    Source line number (use __LINE__).
 * @param file    Source file name (use __FILE__).
 * @param message printf-style format string.
 * @return 0 on success, negative on error.
 */
int hal_log_message(int level, int line, const char* file, const char* message, ...);

/**
 * @brief Dump a memory region in hex + ASCII format.
 * @param level Severity level.
 * @param data  Pointer to the data to dump.
 * @param size  Number of bytes to dump.
 * @return 0 on success, negative on error.
 */
int hal_log_hexdump(int level, const void* data, int size);

#ifdef __cplusplus
}
#endif
