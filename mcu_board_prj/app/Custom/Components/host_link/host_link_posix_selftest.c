/**
 * @file host_link_posix_selftest.c
 * @brief Loopback test on Linux: gcc -std=c11 -Wall -O2 -DHOST_LINK_BUILD_SELFTEST \
 *        -I. host_link_posix_selftest.c host_link_core.c host_link_port_posix.c \
 *        -o host_link_selftest -pthread
 *
 * Excluded from STM32 CubeIDE build (see .cproject excluding list).
 */
#if defined(__linux__) && defined(HOST_LINK_BUILD_SELFTEST)

#include "host_link.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd;
} sock_ctx_t;

static int send_sock(void *user_ctx, const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    sock_ctx_t *c = (sock_ctx_t *)user_ctx;
    ssize_t w = write(c->fd, buf, len);
    return (w == (ssize_t)len) ? 0 : -1;
}

static void notify_b(void *user_ctx, host_link_handler_t *h, host_link_frame_t *f)
{
    (void)user_ctx;
    if (f->header.type == HOST_LINK_TYPE_REQUEST && f->header.cmd == HOST_LINK_CMD_PING) {
        uint32_t v = 0x11223344u;
        (void)host_link_response(h, f, &v, sizeof(v));
    } else if (f->header.type == HOST_LINK_TYPE_EVENT) {
        (void)host_link_event_ack(h, f);
    }
}

static void notify_a(void *user_ctx, host_link_handler_t *h, host_link_frame_t *f)
{
    (void)user_ctx;
    (void)h;
    (void)f;
}

typedef struct {
    host_link_handler_t *h;
    int fd;
} rx_arg_t;

static void *rx_thread(void *arg)
{
    rx_arg_t *p = (rx_arg_t *)arg;
    uint8_t b[256];
    for (;;) {
        ssize_t n = read(p->fd, b, sizeof(b));
        if (n <= 0) {
            break;
        }
        host_link_feed(p->h, b, (uint16_t)n);
    }
    return NULL;
}

static volatile int g_stop_poll;

static void *poll_thread(void *arg)
{
    host_link_handler_t *h = (host_link_handler_t *)arg;
    while (!g_stop_poll) {
        host_link_poll(h);
    }
    return NULL;
}

int main(void)
{
    g_stop_poll = 0;
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sp) != 0) {
        perror("socketpair");
        return 1;
    }

    sock_ctx_t ca = {.fd = sp[0]};
    sock_ctx_t cb = {.fd = sp[1]};

    host_link_handler_t *ha = host_link_init(send_sock, notify_a, &ca);
    host_link_handler_t *hb = host_link_init(send_sock, notify_b, &cb);
    if (ha == NULL || hb == NULL) {
        return 2;
    }

    rx_arg_t ra = {ha, sp[0]};
    rx_arg_t rb = {hb, sp[1]};

    pthread_t ta, tb, pa, pb;
    pthread_create(&ta, NULL, rx_thread, &ra);
    pthread_create(&tb, NULL, rx_thread, &rb);
    pthread_create(&pa, NULL, poll_thread, ha);
    pthread_create(&pb, NULL, poll_thread, hb);

    void *resp = NULL;
    uint16_t rlen = 0;
    int r = host_link_request(ha, HOST_LINK_CMD_PING, NULL, 0, &resp, &rlen);
    if (r != HOST_LINK_OK || rlen != sizeof(uint32_t) || *(uint32_t *)resp != 0x11223344u) {
        fprintf(stderr, "request ping fail r=%d len=%u\n", r, (unsigned)rlen);
        return 3;
    }
    host_link_free(resp);

    r = host_link_send_event(ha, HOST_LINK_CMD_ECHO, "x", 1);
    if (r != HOST_LINK_OK) {
        fprintf(stderr, "send_event fail %d\n", r);
        return 4;
    }

    g_stop_poll = 1;
    pthread_join(pa, NULL);
    pthread_join(pb, NULL);

    close(sp[0]);
    close(sp[1]);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);

    host_link_deinit(ha);
    host_link_deinit(hb);
    printf("host_link posix selftest OK\n");
    return 0;
}

#endif /* __linux__ && HOST_LINK_BUILD_SELFTEST */

#if !defined(__linux__) || !defined(HOST_LINK_BUILD_SELFTEST)
typedef int host_link_posix_selftest_placeholder;
#endif
