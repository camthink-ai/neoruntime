/**
 * @file crash_handler.h
 * @brief Crash signal handler with backtrace output
 *
 * Installs signal handlers for SIGSEGV, SIGABRT, SIGBUS, SIGFPE.
 * On crash, prints stack trace to stderr and optional crash log file,
 * then re-raises the signal to produce a coredump.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Install crash signal handlers.
 *
 * @param service_name  Identifier printed in crash banner (e.g. "camera-daemon")
 * @param crash_log_dir Directory for crash log files (NULL = stderr only).
 *                      File created as: <dir>/<service_name>.crash.log
 */
void crash_handler_install(const char* service_name, const char* crash_log_dir);

#ifdef __cplusplus
}
#endif
