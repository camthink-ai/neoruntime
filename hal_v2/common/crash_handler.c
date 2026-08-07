/**
 * @file crash_handler.c
 * @brief Crash signal handler — prints backtrace on fatal signals
 *
 * All output in the signal handler uses write() (async-signal-safe).
 * After printing the backtrace, the default handler is restored and
 * the signal re-raised so the OS can generate a coredump.
 */

#define _GNU_SOURCE
#include "crash_handler.h"

#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <stdio.h>
#include <sys/types.h>

#define CRASH_MAX_FRAMES  64
#define CRASH_MAX_PATH    256

static char g_service_name[64];
static char g_crash_log_path[CRASH_MAX_PATH];

/* async-signal-safe write helpers */
static void safe_write(int fd, const char* s)
{
    if (s) write(fd, s, strlen(s));
}

static void safe_write_num(int fd, unsigned long val)
{
    char buf[24];
    int i = (int)sizeof(buf) - 1;
    buf[i] = '\0';
    if (val == 0) {
        buf[--i] = '0';
    } else {
        while (val > 0 && i > 0) {
            buf[--i] = '0' + (char)(val % 10);
            val /= 10;
        }
    }
    safe_write(fd, &buf[i]);
}

static const char* sig_name(int sig)
{
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (Segmentation fault)";
        case SIGABRT: return "SIGABRT (Abort)";
        case SIGBUS:  return "SIGBUS (Bus error)";
        case SIGFPE:  return "SIGFPE (Floating point exception)";
        default:      return "Unknown signal";
    }
}

static void dump_backtrace(int fd, int sig)
{
    safe_write(fd, "\n========== CRASH REPORT ==========\n");
    safe_write(fd, "Service : ");
    safe_write(fd, g_service_name);
    safe_write(fd, "\nSignal  : ");
    safe_write(fd, sig_name(sig));
    safe_write(fd, " (");
    safe_write_num(fd, (unsigned long)sig);
    safe_write(fd, ")\nPID     : ");
    safe_write_num(fd, (unsigned long)getpid());
    safe_write(fd, "\n\n--- Backtrace ---\n");

    void* frames[CRASH_MAX_FRAMES];
    int n = backtrace(frames, CRASH_MAX_FRAMES);
    if (n > 0) {
        backtrace_symbols_fd(frames, n, fd);
    } else {
        safe_write(fd, "(backtrace unavailable)\n");
    }

    safe_write(fd, "===================================\n\n");
}

static void crash_signal_handler(int sig)
{
    dump_backtrace(STDERR_FILENO, sig);

    if (g_crash_log_path[0]) {
        int fd = open(g_crash_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            dump_backtrace(fd, sig);
            close(fd);
        }
    }

    /* Restore default handler and re-raise for coredump generation */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, NULL);
    raise(sig);
}

void crash_handler_install(const char* service_name, const char* crash_log_dir)
{
    if (service_name) {
        strncpy(g_service_name, service_name, sizeof(g_service_name) - 1);
        g_service_name[sizeof(g_service_name) - 1] = '\0';
    } else {
        strcpy(g_service_name, "unknown");
    }

    g_crash_log_path[0] = '\0';
    if (crash_log_dir && crash_log_dir[0]) {
        snprintf(g_crash_log_path, sizeof(g_crash_log_path),
                 "%s/%s.crash.log", crash_log_dir, g_service_name);
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESETHAND;  /* one-shot: avoid recursive crash loops */

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);
    sigaction(SIGFPE,  &sa, NULL);
}
