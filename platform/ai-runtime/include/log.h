#pragma once

#include <cstdio>
#include <cstdarg>

namespace aipc::ai_runtime {

enum class LogLevel { kDebug = 0, kInfo, kWarn, kError, kFatal };

inline LogLevel g_log_level = LogLevel::kInfo;

inline void set_log_level(const std::string& level) {
    if (level == "debug") g_log_level = LogLevel::kDebug;
    else if (level == "info")  g_log_level = LogLevel::kInfo;
    else if (level == "warn")  g_log_level = LogLevel::kWarn;
    else if (level == "error") g_log_level = LogLevel::kError;
}

inline void log_msg(LogLevel level, const char* fmt, ...) {
    if (level < g_log_level) return;
    static const char* tags[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL"};
    fprintf(stderr, "[ai-runtime][%s] ", tags[static_cast<int>(level)]);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

}  // namespace aipc::ai_runtime

#define LOG_DEBUG(fmt, ...) ::aipc::ai_runtime::log_msg(::aipc::ai_runtime::LogLevel::kDebug, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...)  ::aipc::ai_runtime::log_msg(::aipc::ai_runtime::LogLevel::kInfo,  fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  ::aipc::ai_runtime::log_msg(::aipc::ai_runtime::LogLevel::kWarn,  fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) ::aipc::ai_runtime::log_msg(::aipc::ai_runtime::LogLevel::kError, fmt, ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...) ::aipc::ai_runtime::log_msg(::aipc::ai_runtime::LogLevel::kFatal, fmt, ##__VA_ARGS__)
