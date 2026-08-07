#ifndef __WIC_LOG_H__
#define __WIC_LOG_H__

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_IS_USE_FREERTOS     (0)
#define LOG_LEVEL_IS_STR        (0)

#define LOG_LEVEL_TRACE         (0) 
#define LOG_LEVEL_DEBUG         (1)
#define LOG_LEVEL_INFO          (2)
#define LOG_LEVEL_WARN          (3)
#define LOG_LEVEL_ERROR         (4)
#define LOG_LEVEL_FATAL         (5)
#define LOG_LEVEL_NONE          (6)

typedef int (*log_save_fun_t)(const char *file, const int line, const int level, char *info);

typedef enum {
    LOG_OK = 0,
    LOG_ERR_INVALID_ARG = -0X0F,
    LOG_ERR_NO_MEM,
    LOG_ERR_FMT_STR,
    LOG_ERR_SAVE,
    LOG_ERR_LOCK,
} log_err_t;

#if LOG_LEVEL_IS_STR == 1
#define WIC_LOG_INFO_FMT        "%s [%s:%d]-> %s\r\n"
#define WIC_LOG_HEX_FMT         "%s Output %u data(hex):\r\n"
#else
#define WIC_LOG_INFO_FMT        "%c [%s:%d]-> %s\r\n"
#define WIC_LOG_HEX_FMT         "%s Output %u data(hex):\r\n"
#endif
#define WIC_LOG_LEVEL           LOG_LEVEL_DEBUG
#define WIC_LOG_INFO_MAX_LEN    512
#define WIC_LOG_FILE_NAME(x)    strrchr(x, '\\') ? (strrchr(x, '\\') + 1) : (strrchr(x, '/') ? (strrchr(x, '/') + 1) : x)

const char *get_level_str(const int level);
const char get_level_char(const int level);
void set_log_save_fun(log_save_fun_t fun);
void set_log_level(const int level);
int wic_log_hex(const char *file, const int line, const int level, const char *tag, unsigned char *data, unsigned int len);
int wic_log(const char *file, const int line, const int level, const char *fmt, ...);

// Define formatted log output
#if WIC_LOG_LEVEL > LOG_LEVEL_FATAL
    #define WIC_LOGF(fmt, ...)
#else
    #define WIC_LOGF(fmt, ...)  wic_log(__FILE__, __LINE__, LOG_LEVEL_FATAL, fmt, ##__VA_ARGS__)
#endif
#if WIC_LOG_LEVEL > LOG_LEVEL_ERROR
    #define WIC_LOGE(fmt, ...)
#else
    #define WIC_LOGE(fmt, ...)  wic_log(__FILE__, __LINE__, LOG_LEVEL_ERROR, fmt, ##__VA_ARGS__)
#endif
#if WIC_LOG_LEVEL > LOG_LEVEL_WARN
    #define WIC_LOGW(fmt, ...)
#else
    #define WIC_LOGW(fmt, ...)  wic_log(__FILE__, __LINE__, LOG_LEVEL_WARN, fmt, ##__VA_ARGS__)
#endif
#if WIC_LOG_LEVEL > LOG_LEVEL_INFO
    #define WIC_LOGI(fmt, ...)
#else
    #define WIC_LOGI(fmt, ...)  wic_log(__FILE__, __LINE__, LOG_LEVEL_INFO, fmt, ##__VA_ARGS__)
#endif
#if WIC_LOG_LEVEL > LOG_LEVEL_DEBUG
    #define WIC_LOGD(fmt, ...)
#else
    #define WIC_LOGD(fmt, ...)  wic_log(__FILE__, __LINE__, LOG_LEVEL_DEBUG, fmt, ##__VA_ARGS__)
#endif
#if WIC_LOG_LEVEL > LOG_LEVEL_TRACE
    #define WIC_LOGT(fmt, ...)
#else
    #define WIC_LOGT(fmt, ...)  wic_log(__FILE__, __LINE__, LOG_LEVEL_TRACE, fmt, ##__VA_ARGS__)
#endif
// Define array output
#if WIC_LOG_LEVEL > LOG_LEVEL_FATAL
    #define WIC_LOGF_HEX(tag, datas, len)
#else
    #define WIC_LOGF_HEX(tag, datas, len)   wic_log_hex(__FILE__, __LINE__, LOG_LEVEL_FATAL, tag, datas, len)
#endif
#if WIC_LOG_LEVEL > LOG_LEVEL_ERROR
    #define WIC_LOGE_HEX(tag, datas, len)
#else
    #define WIC_LOGE_HEX(tag, datas, len)   wic_log_hex(__FILE__, __LINE__, LOG_LEVEL_ERROR, tag, datas, len)
#endif
#if WIC_LOG_LEVEL > LOG_LEVEL_WARN
    #define WIC_LOGW_HEX(tag, datas, len)
#else
    #define WIC_LOGW_HEX(tag, datas, len)   wic_log_hex(__FILE__, __LINE__, LOG_LEVEL_WARN, tag, datas, len)
#endif
#if WIC_LOG_LEVEL > LOG_LEVEL_INFO
    #define WIC_LOGI_HEX(tag, datas, len)
#else
    #define WIC_LOGI_HEX(tag, datas, len)   wic_log_hex(__FILE__, __LINE__, LOG_LEVEL_INFO, tag, datas, len)
#endif
#if WIC_LOG_LEVEL > LOG_LEVEL_DEBUG
    #define WIC_LOGD_HEX(tag, datas, len)
#else
    #define WIC_LOGD_HEX(tag, datas, len)   wic_log_hex(__FILE__, __LINE__, LOG_LEVEL_DEBUG, tag, datas, len)
#endif
#if WIC_LOG_LEVEL > LOG_LEVEL_TRACE
    #define WIC_LOGT_HEX(tag, datas, len)
#else
    #define WIC_LOGT_HEX(tag, datas, len)   wic_log_hex(__FILE__, __LINE__, LOG_LEVEL_TRACE, tag, datas, len)
#endif

#ifdef __cplusplus
}
#endif

#endif /* __WIC_LOG_H__ */