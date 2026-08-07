/**
 * @file hailo15_mcu_impl.cpp
 * @brief hailo15 implementation of HAL_MCU_OPS using host_link over UART.
 */

#include "hailo15_mcu_priv.hpp"

extern "C" {
#include "peripheral/hal_mcu.h"
#include "common/host_link/host_link_port.h"
}

#include "common/host_link/host_link_proto.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <new>
#include <fcntl.h>
#include <pthread.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

struct Hailo15McuCtx {
    HalMcuConfig cfg{};

    int fd{-1};
    std::atomic<bool> running{false};
    std::atomic<bool> suspend_rx{false};
    pthread_t rx_thread{};
    pthread_t poll_thread{};

    pthread_mutex_t tx_lock{};
    pthread_mutex_t serial_excl_mtx{};

    host_link_handler_t *link{nullptr};

    std::mutex cb_lock;
    struct Sub {
        uint16_t cmd;
        Hailo15McuEventCb cb;
    };
    std::vector<Sub> subs;
};

static speed_t baud_to_speed(uint32_t baud)
{
    switch (baud) {
    case 9600: return B9600;
    case 19200: return B19200;
    case 38400: return B38400;
    case 57600: return B57600;
    case 115200: return B115200;
    case 230400: return B230400;
    case 460800: return B460800;
    case 921600: return B921600;
    case 3000000: return B3000000;
    default: return 0;
    }
}

static int serial_open(const char *path, uint32_t baud)
{
    int fd = open(path, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        return -1;
    }

    struct termios tty;
    if (tcgetattr(fd, &tty) != 0) {
        close(fd);
        return -1;
    }

    speed_t speed = baud_to_speed(baud);
    if (speed == 0) {
        close(fd);
        return -1;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);
    tty.c_cflag &= ~PARENB;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CSIZE;
    tty.c_cflag |= CS8;
    tty.c_cflag &= ~CRTSCTS;
    tty.c_cflag |= CREAD | CLOCAL;

    tty.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXOFF | IXANY);
    tty.c_lflag &= ~(ICANON | ECHO | ECHOE | ISIG);
    tty.c_oflag &= ~OPOST;
    tty.c_cc[VMIN] = 1;
    tty.c_cc[VTIME] = 1;

    if (tcsetattr(fd, TCSANOW, &tty) != 0) {
        close(fd);
        return -1;
    }
    (void)tcflush(fd, TCIOFLUSH);

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    }
    return fd;
}

int hailo15_mcu_map_host_err(int err)
{
    switch (err) {
    case HOST_LINK_OK: return HAL_OK;
    case HOST_LINK_ERR_INVALID_ARG:
    case HOST_LINK_ERR_INVALID_SIZE:
        return HAL_ERR_INVALID_ARG;
    case HOST_LINK_ERR_TIMEOUT:
        return HAL_ERR_TIMEOUT;
    case HOST_LINK_ERR_NO_MEM:
        return HAL_ERR_NO_MEM;
    case HOST_LINK_ERR_INVALID_STATE:
        return HAL_ERR_INVALID_STATE;
    case HOST_LINK_ERR_NO_SLOT:
        return HAL_ERR_NOT_READY;
    default:
        return HAL_ERROR;
    }
}

static int host_send(void *user_ctx, const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    (void)timeout_ms;
    auto *ctx = static_cast<Hailo15McuCtx *>(user_ctx);
    if (ctx == nullptr || ctx->fd < 0 || buf == nullptr || len == 0) {
        return -1;
    }

    pthread_mutex_lock(&ctx->tx_lock);
    uint16_t sent = 0;
    while (sent < len) {
        ssize_t n = write(ctx->fd, buf + sent, (size_t)(len - sent));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            pthread_mutex_unlock(&ctx->tx_lock);
            return -1;
        }
        sent += (uint16_t)n;
    }
    pthread_mutex_unlock(&ctx->tx_lock);
    return 0;
}

static void host_notify(void *user_ctx, host_link_handler_t *h, host_link_frame_t *frame)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(user_ctx);
    if (ctx == nullptr || h == nullptr || frame == nullptr) {
        return;
    }

    if (frame->header.type != HOST_LINK_TYPE_EVENT) {
        return;
    }

    std::vector<Hailo15McuEventCb> cbs;
    {
        std::lock_guard<std::mutex> g(ctx->cb_lock);
        for (const auto &s : ctx->subs) {
            if (s.cmd == frame->header.cmd && s.cb) {
                cbs.push_back(s.cb);
            }
        }
    }
    for (auto &cb : cbs) {
        cb(frame->header.cmd, frame->payload, frame->header.len);
    }

    (void)host_link_event_ack(h, frame);
}

static void *rx_thread_main(void *arg)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(arg);
    uint8_t buf[512];
    while (ctx->running.load()) {
        if (ctx->suspend_rx.load()) {
            usleep(2000);
            continue;
        }
        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(ctx->fd, &rfds);
        tv.tv_sec = 0;
        tv.tv_usec = 200000;

        int sr = select(ctx->fd + 1, &rfds, NULL, NULL, &tv);
        if (!ctx->running.load()) {
            break;
        }
        if (sr < 0) {
            if (errno == EINTR) {
                continue;
            }
            usleep(1000);
            continue;
        }
        if (sr == 0 || !FD_ISSET(ctx->fd, &rfds)) {
            continue;
        }
        ssize_t n = read(ctx->fd, buf, sizeof(buf));
        if (n > 0 && ctx->link != nullptr) {
            host_link_feed(ctx->link, buf, (uint16_t)n);
        } else if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
    }
    return nullptr;
}

static void *poll_thread_main(void *arg)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(arg);
    while (ctx->running.load()) {
        host_link_poll(ctx->link);
    }
    return nullptr;
}

int hailo15_mcu_register_event_cb(Hailo15McuCtx *ctx, uint16_t cmd, Hailo15McuEventCb cb)
{
    if (ctx == nullptr || cb == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> g(ctx->cb_lock);
    ctx->subs.push_back(Hailo15McuCtx::Sub{cmd, std::move(cb)});
    return HAL_OK;
}

int hailo15_mcu_unregister_all_event_cbs(Hailo15McuCtx *ctx, uint16_t cmd)
{
    if (ctx == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> g(ctx->cb_lock);
    ctx->subs.erase(std::remove_if(ctx->subs.begin(), ctx->subs.end(),
                                   [cmd](const Hailo15McuCtx::Sub &s) { return s.cmd == cmd; }),
                    ctx->subs.end());
    return HAL_OK;
}

static int mcu_init(const HalMcuConfig *config, void **mcu_ctx_return)
{
    if (mcu_ctx_return == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    if (config == nullptr || config->serial_device == nullptr || config->baud_rate == 0) {
        return HAL_ERR_INVALID_ARG;
    }
    auto *ctx = new (std::nothrow) Hailo15McuCtx();
    if (ctx == nullptr) {
        return HAL_ERR_NO_MEM;
    }
    ctx->cfg = *config;
    ctx->fd = serial_open(config->serial_device, config->baud_rate);
    if (ctx->fd < 0) {
        delete ctx;
        return HAL_ERROR;
    }

    pthread_mutex_init(&ctx->tx_lock, nullptr);
    pthread_mutex_init(&ctx->serial_excl_mtx, nullptr);

    ctx->link = host_link_init(host_send, host_notify, ctx);
    if (ctx->link == nullptr) {
        close(ctx->fd);
        ctx->fd = -1;
        pthread_mutex_destroy(&ctx->serial_excl_mtx);
        pthread_mutex_destroy(&ctx->tx_lock);
        delete ctx;
        return HAL_ERR_NO_MEM;
    }

    ctx->running.store(true);
    if (pthread_create(&ctx->rx_thread, nullptr, rx_thread_main, ctx) != 0 || pthread_create(&ctx->poll_thread, nullptr, poll_thread_main, ctx) != 0) {
        ctx->running.store(false);
        if (ctx->fd >= 0) {
            close(ctx->fd);
            ctx->fd = -1;
        }
        host_link_deinit(ctx->link);
        ctx->link = nullptr;
        pthread_mutex_destroy(&ctx->serial_excl_mtx);
        pthread_mutex_destroy(&ctx->tx_lock);
        delete ctx;
        return HAL_ERROR;
    }

    *mcu_ctx_return = ctx;
    return HAL_OK;
}

static int mcu_deinit(void *mcu_ctx)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    if (ctx == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }

    ctx->running.store(false);
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }

    pthread_join(ctx->rx_thread, nullptr);
    pthread_join(ctx->poll_thread, nullptr);

    host_link_deinit(ctx->link);
    ctx->link = nullptr;

    pthread_mutex_destroy(&ctx->serial_excl_mtx);
    pthread_mutex_destroy(&ctx->tx_lock);
    delete ctx;
    return HAL_OK;
}

static int mcu_get_version(void *mcu_ctx, HalMcuVersion *version)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    if (ctx == nullptr || version == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    void *payload = nullptr;
    uint16_t payload_len = 0;
    int err = host_link_request(ctx->link, HOST_LINK_CMD_GET_VERSION, nullptr, 0, &payload, &payload_len);
    if (err != HOST_LINK_OK) {
        return hailo15_mcu_map_host_err(err);
    }
    if (payload == nullptr || payload_len != sizeof(host_link_version_t)) {
        if (payload != nullptr) {
            host_link_free(payload);
        }
        return HAL_ERR_INVALID_SIZE;
    }
    const auto *wire = static_cast<const host_link_version_t *>(payload);
    memset(version, 0, sizeof(*version));
    version->major = wire->major;
    version->minor = wire->minor;
    version->patch = wire->patch;
    version->build = wire->build;
    (void)snprintf(version->version_str, sizeof(version->version_str), "%d.%d.%d.%d",
                   wire->major, wire->minor, wire->patch, wire->build);
    host_link_free(payload);
    return HAL_OK;
}

static int mcu_ping(void *mcu_ctx, uint32_t value, uint32_t *echo)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    if (ctx == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    void *payload = nullptr;
    uint16_t payload_len = 0;
    int err = host_link_request(ctx->link, HOST_LINK_CMD_PING, &value, sizeof(value), &payload, &payload_len);
    if (err != HOST_LINK_OK) {
        return hailo15_mcu_map_host_err(err);
    }
    if (payload == nullptr || payload_len != sizeof(uint32_t)) {
        if (payload != nullptr) {
            host_link_free(payload);
        }
        return HAL_ERR_INVALID_SIZE;
    }
    uint32_t out = 0;
    memcpy(&out, payload, sizeof(out));
    host_link_free(payload);
    if (echo != nullptr) {
        *echo = out;
    }
    return HAL_OK;
}

static int mcu_echo(void *mcu_ctx, const uint8_t *data, uint16_t len, uint8_t *out, uint16_t out_size, uint16_t *out_len)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    if (ctx == nullptr || out_len == nullptr || (len > 0 && data == nullptr)) {
        return HAL_ERR_INVALID_ARG;
    }
    void *payload = nullptr;
    uint16_t payload_len = 0;
    int err = host_link_request(ctx->link, HOST_LINK_CMD_ECHO, data, len, &payload, &payload_len);
    if (err != HOST_LINK_OK) {
        return hailo15_mcu_map_host_err(err);
    }
    uint16_t copy_len = payload_len;
    if (copy_len > out_size) {
        copy_len = out_size;
    }
    if (copy_len > 0 && out != nullptr && payload != nullptr) {
        memcpy(out, payload, copy_len);
    }
    *out_len = copy_len;
    if (payload != nullptr) {
        host_link_free(payload);
    }
    return HAL_OK;
}

static int mcu_get_status(void *mcu_ctx)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    if (ctx == nullptr) {
        return (int)HAL_STATUS_UNINITIALIZED;
    }
    if (!ctx->running.load()) {
        return (int)HAL_STATUS_STOPPED;
    }
    if (ctx->fd < 0 || ctx->link == nullptr) {
        return (int)HAL_STATUS_ERROR;
    }
    return (int)HAL_STATUS_RUNNING;
}

static int mcu_raw_request(void *mcu_ctx, uint16_t cmd, const uint8_t *payload, uint16_t payload_len,
                           uint8_t *response, uint16_t response_size, uint16_t *response_len)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    if (ctx == nullptr || (payload_len > 0 && payload == nullptr)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (payload_len > HAL_MCU_MAX_PAYLOAD) {
        return HAL_ERR_INVALID_SIZE;
    }
    void *resp = nullptr;
    uint16_t resp_len = 0;
    int err = host_link_request(ctx->link, cmd, payload, payload_len, &resp, &resp_len);
    if (err != HOST_LINK_OK) {
        return hailo15_mcu_map_host_err(err);
    }
    if (response_len != nullptr) {
        *response_len = resp_len;
    }
    if (response == nullptr || response_size == 0) {
        if (resp != nullptr) {
            host_link_free(resp);
        }
        return HAL_OK;
    }
    if (resp_len > response_size) {
        if (resp != nullptr) {
            host_link_free(resp);
        }
        return HAL_ERR_INSUFFICIENT_BUFFER;
    }
    if (resp_len > 0 && resp != nullptr) {
        memcpy(response, resp, resp_len);
    }
    if (resp != nullptr) {
        host_link_free(resp);
    }
    return HAL_OK;
}

int hailo15_mcu_raw_request_timeout(void *mcu_ctx, uint16_t cmd,
                                   const uint8_t *payload, uint16_t payload_len,
                                   uint8_t *response, uint16_t response_size,
                                   uint16_t *response_len, uint32_t timeout_ms)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    if (ctx == nullptr || (payload_len > 0 && payload == nullptr)) {
        return HAL_ERR_INVALID_ARG;
    }
    if (payload_len > HAL_MCU_MAX_PAYLOAD) {
        return HAL_ERR_INVALID_SIZE;
    }

    void *resp = nullptr;
    uint16_t resp_len = 0;
    int err = host_link_request_timeout(ctx->link, cmd, payload, payload_len, &resp, &resp_len, timeout_ms);
    if (err != HOST_LINK_OK) {
        return hailo15_mcu_map_host_err(err);
    }

    if (response_len != nullptr) {
        *response_len = resp_len;
    }
    if (response == nullptr || response_size == 0) {
        if (resp != nullptr) {
            host_link_free(resp);
        }
        return HAL_OK;
    }
    if (resp_len > response_size) {
        if (resp != nullptr) {
            host_link_free(resp);
        }
        return HAL_ERR_INSUFFICIENT_BUFFER;
    }
    if (resp_len > 0 && resp != nullptr) {
        memcpy(response, resp, resp_len);
    }
    if (resp != nullptr) {
        host_link_free(resp);
    }
    return HAL_OK;
}

extern "C" const HalMcuConfig *hailo15_mcu_get_config(void *mcu_ctx)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    if (ctx == nullptr) {
        return nullptr;
    }
    return &ctx->cfg;
}

extern "C" int hailo15_mcu_raw_write_all(void *mcu_ctx, const uint8_t *data, size_t len)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    if (ctx == nullptr || ctx->fd < 0 || (len > 0 && data == nullptr)) {
        return HAL_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&ctx->tx_lock);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(ctx->fd, data + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            pthread_mutex_unlock(&ctx->tx_lock);
            return HAL_ERROR;
        }
        sent += (size_t)n;
    }
    pthread_mutex_unlock(&ctx->tx_lock);
    return HAL_OK;
}

extern "C" ssize_t hailo15_mcu_raw_read(void *mcu_ctx, void *buf, size_t cap, uint32_t timeout_ms)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    if (ctx == nullptr || buf == nullptr || cap == 0u || ctx->fd < 0) {
        return (ssize_t)HAL_ERR_INVALID_ARG;
    }
    fd_set rfds;
    struct timeval tv {};
    struct timeval *tvp = nullptr;
    if (timeout_ms > 0u) {
        tv.tv_sec = (long)(timeout_ms / 1000u);
        tv.tv_usec = (long)((timeout_ms % 1000u) * 1000u);
        tvp = &tv;
    }
    FD_ZERO(&rfds);
    FD_SET(ctx->fd, &rfds);
    int r = select(ctx->fd + 1, &rfds, nullptr, nullptr, tvp);
    if (r < 0) {
        return (ssize_t)-errno;
    }
    if (r == 0) {
        return 0;
    }
    ssize_t n = read(ctx->fd, buf, cap);
    if (n < 0) {
        return (ssize_t)-errno;
    }
    return n;
}

extern "C" int hailo15_mcu_with_rx_suspended(void *mcu_ctx, int (*fn)(void *mcu_ctx2, void *user), void *user)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    if (ctx == nullptr || fn == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&ctx->serial_excl_mtx);
    ctx->suspend_rx.store(true);
    /* Allow RX thread to leave read/select before we flush. */
    usleep(250 * 1000);
    if (ctx->fd >= 0) {
        (void)tcflush(ctx->fd, TCIOFLUSH);
    }
    const int rc = (ctx->fd < 0) ? HAL_ERR_INVALID_STATE : fn(mcu_ctx, user);
    if (ctx->fd >= 0) {
        (void)tcflush(ctx->fd, TCIFLUSH);
    }
    ctx->suspend_rx.store(false);
    pthread_mutex_unlock(&ctx->serial_excl_mtx);
    return rc;
}

extern "C" int hailo15_mcu_serial_exclusive(void *mcu_ctx, int (*fn)(void *mcu_ctx2, void *user), void *user)
{
    return hailo15_mcu_with_rx_suspended(mcu_ctx, fn, user);
}

static const char *mcu_get_hal_version(void)
{
    return "Hailo15 HAL-MCU 2.0.0";
}

extern "C" {
HalMcuOps HAL_MCU_OPS = {
    .init = mcu_init,
    .deinit = mcu_deinit,
    .get_version = mcu_get_version,
    .ping = mcu_ping,
    .echo = mcu_echo,
    .get_status = mcu_get_status,
    .raw_request = mcu_raw_request,
    .get_hal_version = mcu_get_hal_version,
};
}

