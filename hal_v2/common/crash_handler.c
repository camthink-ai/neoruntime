/**
 * @file crash_handler.c
 * @brief Crash signal handler — prints backtrace on fatal signals
 *
 * Signal-path output uses only async-signal-safe calls (write/open/read/
 * close/sigaction/sigprocmask/raise + manual number formatting); the
 * snprintf at install time runs in a normal context, never in a handler.
 * The handler runs on a dedicated altstack (SA_ONSTACK) so even a stack
 * overflow produces a report. The persistent crash log is written FIRST
 * and the unwind runs ONCE: if the unwinder itself faults on a corrupt
 * stack, the header (fault addr/code, thread tid+name) is already on disk.
 * After the report, the default handler is restored and the signal
 * re-raised (unblocked first) so the OS can generate a coredump.
 */

#define _GNU_SOURCE
#include "crash_handler.h"

#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/syscall.h>

#define CRASH_MAX_FRAMES   64
#define CRASH_MAX_PATH     256
/* Altstack big enough for the handler + libgcc unwind of CRASH_MAX_FRAMES
 * (SIGSTKSZ alone is not). Static, not a runtime sysconf: stays a link-time
 * constant on every glibc. */
#define CRASH_ALTSTACK_SIZE (64 * 1024)

static char g_service_name[64];
static char g_crash_log_path[CRASH_MAX_PATH];
static char g_crash_altstack[CRASH_ALTSTACK_SIZE];

/* async-signal-safe write helpers */
static void safe_write(int fd, const char* s)
{
    if (s) {
        size_t len = strlen(s);
        while (len > 0) {
            ssize_t n = write(fd, s, len);
            if (n <= 0) break; /* EINTR retries are pointless post-crash */
            s += n;
            len -= (size_t)n;
        }
    }
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

static void safe_write_hex(int fd, unsigned long val)
{
    static const char hex[] = "0123456789abcdef";
    char buf[20];
    int i = (int)sizeof(buf) - 1;
    buf[i] = '\0';
    if (val == 0) {
        buf[--i] = '0';
    } else {
        while (val > 0 && i > 0) {
            buf[--i] = hex[val & 0xF];
            val >>= 4;
        }
    }
    safe_write(fd, "0x");
    safe_write(fd, &buf[i]);
}

static const char* sig_name(int sig)
{
    switch (sig) {
        case SIGSEGV: return "SIGSEGV (Segmentation fault)";
        case SIGABRT: return "SIGABRT (Abort)";
        case SIGBUS:  return "SIGBUS (Bus error)";
        case SIGFPE:  return "SIGFPE (Floating point exception)";
        case SIGILL:  return "SIGILL (Illegal instruction)";
        default:      return "Unknown signal";
    }
}

/* async-signal-safe string builders (no snprintf in the handler path) */
static size_t buf_append(char* buf, size_t cap, size_t pos, const char* s)
{
    while (*s != '\0' && pos + 1 < cap) {
        buf[pos++] = *s++;
    }
    buf[pos] = '\0';
    return pos;
}

static size_t buf_append_num(char* buf, size_t cap, size_t pos, unsigned long val)
{
    char tmp[24];
    int i = (int)sizeof(tmp) - 1;
    tmp[i] = '\0';
    if (val == 0) {
        tmp[--i] = '0';
    } else {
        while (val > 0 && i > 0) {
            tmp[--i] = '0' + (char)(val % 10);
            val /= 10;
        }
    }
    return buf_append(buf, cap, pos, &tmp[i]);
}

/* Header (everything except the backtrace). Written to the persistent log
 * BEFORE the unwind runs: if the unwinder faults on a corrupt stack, the
 * fault address and thread identity still survive on disk. */
static void write_report_header(int fd, int sig, const siginfo_t* info)
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

    /* Fault address + code: distinguishes a wild pointer deref (SEGV_MAPERR at
     * a garbage address — e.g. a lock taken on freed+reused memory) from a
     * stack overflow (address near the guard page) without needing a core. */
    if (info) {
        safe_write(fd, "\nFault   : addr=");
        safe_write_hex(fd, (unsigned long)info->si_addr);
        safe_write(fd, " code=");
        safe_write_num(fd, (unsigned long)info->si_code);
    }

    /* Faulting thread: name + tid — the faulting thread is often a GStreamer
     * streaming thread, and journal output alone doesn't say which thread's
     * stack the backtrace below came from. open/read/close are async-signal-
     * safe; /proc/self/task/<tid>/comm is a small kernel-provided text file. */
    {
        long tid = syscall(SYS_gettid);
        char comm_path[64];
        char comm[32];
        ssize_t n = -1;
        size_t pos = buf_append(comm_path, sizeof(comm_path), 0, "/proc/self/task/");
        pos = buf_append_num(comm_path, sizeof(comm_path), pos, (unsigned long)tid);
        buf_append(comm_path, sizeof(comm_path), pos, "/comm");
        int cfd = open(comm_path, O_RDONLY);
        if (cfd >= 0) {
            n = read(cfd, comm, sizeof(comm) - 1);
            close(cfd);
        }
        safe_write(fd, "\nThread  : tid=");
        safe_write_num(fd, (unsigned long)tid);
        safe_write(fd, " name=");
        if (n > 0) {
            comm[n] = '\0';
            if (comm[n - 1] == '\n') comm[n - 1] = '\0';
            safe_write(fd, comm);
        } else {
            safe_write(fd, "?");
        }
    }
    safe_write(fd, "\n");
}

static void crash_signal_handler(int sig, siginfo_t* info, void* uctx)
{
    (void)uctx;

    /* Persistent log first — it is the artifact that survives a headless
     * reboot; stderr (journald) second. */
    int persist_fd = -1;
    if (g_crash_log_path[0]) {
        persist_fd = open(g_crash_log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (persist_fd >= 0) {
            write_report_header(persist_fd, sig, info);
        }
    }
    write_report_header(STDERR_FILENO, sig, info);

    /* Unwind ONCE, then emit symbols to both sinks — halves the exposure to
     * the unwinder faulting on a corrupt stack (observed in the field: a
     * queue src-pad task crashed with a smashed frame chain and the second
     * backtrace() faulted inside libgcc). With the header already written
     * above, a fault here still leaves the key diagnostics on disk. */
    {
        void* frames[CRASH_MAX_FRAMES];
        int n = backtrace(frames, CRASH_MAX_FRAMES);
        if (n > 0) {
            if (persist_fd >= 0) {
                safe_write(persist_fd, "\n--- Backtrace ---\n");
                backtrace_symbols_fd(frames, n, persist_fd);
            }
            safe_write(STDERR_FILENO, "\n--- Backtrace ---\n");
            backtrace_symbols_fd(frames, n, STDERR_FILENO);
        } else {
            safe_write(STDERR_FILENO, "\n(backtrace unavailable)\n");
        }
    }
    safe_write(persist_fd >= 0 ? persist_fd : STDERR_FILENO,
               "\n===================================\n\n");
    if (persist_fd >= 0) {
        safe_write(STDERR_FILENO, "===================================\n\n");
        close(persist_fd);
    }

    /* Restore default handler and re-raise for coredump generation.
     * SA_RESETHAND already reset the disposition at handler entry; this
     * sigaction is defensive (e.g. against a nested handler on another
     * crash signal having re-armed it). The signal is blocked in this
     * thread's mask during handler execution — unblock explicitly so the
     * re-raise terminates immediately regardless of the return path. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = SIG_DFL;
    sigaction(sig, &sa, NULL);

    sigset_t unblock;
    sigemptyset(&unblock);
    sigaddset(&unblock, sig);
    sigprocmask(SIG_UNBLOCK, &unblock, NULL);
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
        int n = snprintf(g_crash_log_path, sizeof(g_crash_log_path),
                         "%s/%s.crash.log", crash_log_dir, g_service_name);
        if (n < 0 || (size_t)n >= sizeof(g_crash_log_path)) {
            /* Truncated path would write somewhere unintended — disable the
             * persistent copy rather than corrupt another file. */
            g_crash_log_path[0] = '\0';
            safe_write(STDERR_FILENO, "crash_handler: crash log path truncated; persistent copy disabled\n");
        }
    }

    /* Run on a dedicated altstack so a stack-overflow SIGSEGV can still run
     * the handler (on the exhausted stack it would instantly re-fault and,
     * with SA_RESETHAND, die with no report at all). */
    stack_t ss;
    memset(&ss, 0, sizeof(ss));
    ss.ss_sp = g_crash_altstack;
    ss.ss_size = sizeof(g_crash_altstack);
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) != 0) {
        safe_write(STDERR_FILENO, "crash_handler: sigaltstack failed; overflow crashes will be silent\n");
    }

    /* Prime the unwinder now, in a safe context: the first backtrace() call
     * can lazily dlopen libgcc_s, which deadlocks if it first happens inside
     * the handler while the loader lock is held by the crasher. */
    {
        void* prime[2];
        (void)backtrace(prime, 2);
    }

    /* Block every crash signal while handling one: without this, a SIGABRT
     * arriving mid-SIGSEGV-report would nest handlers on the altstack. They
     * pend until the re-raise below consumes them. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sigaddset(&sa.sa_mask, SIGSEGV);
    sigaddset(&sa.sa_mask, SIGABRT);
    sigaddset(&sa.sa_mask, SIGBUS);
    sigaddset(&sa.sa_mask, SIGFPE);
    sigaddset(&sa.sa_mask, SIGILL);
    sa.sa_flags = SA_RESETHAND | SA_SIGINFO | SA_ONSTACK;  /* one-shot: no recursive crash loops */

    int installed = 0;
    installed |= sigaction(SIGSEGV, &sa, NULL);
    installed |= sigaction(SIGABRT, &sa, NULL);
    installed |= sigaction(SIGBUS,  &sa, NULL);
    installed |= sigaction(SIGFPE,  &sa, NULL);
    installed |= sigaction(SIGILL,  &sa, NULL);
    if (installed != 0) {
        safe_write(STDERR_FILENO, "crash_handler: sigaction failed for at least one signal\n");
    }
}
