#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "wic_log.h"
#if LOG_IS_USE_FREERTOS == 1
#include "FreeRTOS.h"
#endif

#if LOG_IS_USE_FREERTOS == 0
static char global_log_info[WIC_LOG_INFO_MAX_LEN] = {0};
static unsigned char log_lock = 0;
#endif
static const char *LOG_LEVEL_STR[] = {"TRACE", "DEBUG", "INFO", "WARN", "ERROR", "FATAL", "NONE"};
static const char LOG_LEVEL_CHAR[] = {'T', 'D', 'I', 'W', 'E', 'F', 'N'};
static log_save_fun_t log_save_fun = NULL;
static int log_level = LOG_LEVEL_TRACE;

const char *get_level_str(const int level)
{
    if (level < LOG_LEVEL_TRACE || level >= LOG_LEVEL_NONE) return LOG_LEVEL_STR[LOG_LEVEL_NONE];
    else return LOG_LEVEL_STR[level];
}

const char get_level_char(const int level)
{
    if (level < LOG_LEVEL_TRACE || level >= LOG_LEVEL_NONE) return LOG_LEVEL_CHAR[LOG_LEVEL_NONE];
    else return LOG_LEVEL_CHAR[level];
}

void set_log_save_fun(log_save_fun_t fun)
{
    log_save_fun = fun;
}

void set_log_level(const int level)
{
    if (level < LOG_LEVEL_TRACE || level > LOG_LEVEL_NONE) return;
    log_level = level;
}

int wic_log_hex(const char *file, const int line, const int level, const char *tag, unsigned char *datas, unsigned int len)
{
    int recode = LOG_OK, i = 0;
    char hex_str[8] = {0};
    char *log_info = NULL;
    const char *file_name = NULL;

    file_name = WIC_LOG_FILE_NAME(file);
    if (file_name == NULL || tag == NULL || datas == NULL || (len + 32 + strlen(tag)) > (WIC_LOG_INFO_MAX_LEN / 3)) return LOG_ERR_INVALID_ARG;
    if (level < log_level || level >= LOG_LEVEL_NONE) return LOG_OK;
#if LOG_IS_USE_FREERTOS == 0
    if (log_lock) return LOG_ERR_LOCK;
    log_lock = 1;
#endif
#if LOG_IS_USE_FREERTOS == 1
    log_info = (char *)pvPortMalloc(WIC_LOG_INFO_MAX_LEN);
    if (log_info == NULL) return LOG_ERR_NO_MEM;
#else
    log_info = global_log_info;
#endif
    memset(log_info, 0x00, WIC_LOG_INFO_MAX_LEN);

    if (snprintf(log_info, WIC_LOG_INFO_MAX_LEN, WIC_LOG_HEX_FMT, tag, len) < 0) recode = LOG_ERR_FMT_STR;
    else {
        recode = LOG_OK;
        for (i = 0; i < len; i++) {
            if (snprintf(hex_str, 8, ((i + 1) % 16) ? "%02X " : "%02X\r\n", datas[i]) < 0) {
                recode = LOG_ERR_FMT_STR;
                break;
            }
            strncat(log_info, hex_str, 4);
        }
    }
    if (recode == LOG_OK) {
        if (len % 16) strncat(log_info, "\r\n", 3);
        if (log_save_fun != NULL) {
            if (log_save_fun(file_name, line, level, log_info) < 0) recode = LOG_ERR_SAVE;
        }
        if (log_save_fun == NULL || recode == LOG_ERR_SAVE) {
        #if LOG_LEVEL_IS_STR == 1
            printf(WIC_LOG_INFO_FMT, LOG_LEVEL_STR[level], file_name, line, log_info);
        #else
            printf(WIC_LOG_INFO_FMT, LOG_LEVEL_CHAR[level], file_name, line, log_info);
        #endif
        }
    }

#if LOG_IS_USE_FREERTOS == 1
    if (log_info != NULL) vPortFree(log_info);
#else
    log_lock = 0;
#endif
    return recode;
}

int wic_log(const char *file, const int line, const int level, const char *fmt, ...)
{
    int recode = LOG_OK;
    char *log_info = NULL;
    const char *file_name = NULL;
    va_list args;

    file_name = WIC_LOG_FILE_NAME(file);
    if (file_name == NULL || fmt == NULL) return LOG_ERR_INVALID_ARG;
    if (level < log_level || level >= LOG_LEVEL_NONE) return LOG_OK;
#if LOG_IS_USE_FREERTOS == 0
    if (log_lock) return LOG_ERR_LOCK;
    log_lock = 1;
#endif
#if LOG_IS_USE_FREERTOS == 1
    log_info = (char *)pvPortMalloc(WIC_LOG_INFO_MAX_LEN);
    if (log_info == NULL) return LOG_ERR_NO_MEM;
#else
    log_info = global_log_info;
#endif
    memset(log_info, 0x00, WIC_LOG_INFO_MAX_LEN);
    
    va_start(args, fmt);
    if (vsnprintf(log_info, WIC_LOG_INFO_MAX_LEN, fmt, args) < 0) recode = LOG_ERR_FMT_STR;
    else {
        recode = LOG_OK;
        if (log_save_fun != NULL) {
            if (log_save_fun(file_name, line, level, log_info) < 0) recode = LOG_ERR_SAVE;
        }
        if (log_save_fun == NULL || recode == LOG_ERR_SAVE) {
        #if LOG_LEVEL_IS_STR == 1
            printf(WIC_LOG_INFO_FMT, LOG_LEVEL_STR[level], file_name, line, log_info);
        #else
            printf(WIC_LOG_INFO_FMT, LOG_LEVEL_CHAR[level], file_name, line, log_info);
        #endif
        }
    }
    va_end(args);

#if LOG_IS_USE_FREERTOS == 1
    if (log_info != NULL) vPortFree(log_info);
#else
    log_lock = 0;
#endif
    return recode;
}
