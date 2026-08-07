/**
 * @file hailo15_io_impl.cpp
 * @brief hailo15 SoC GPIO via libgpiod (/dev/gpiochip0,1). SoC PWM not supported (stub).
 *
 * GPIO numbering (Hailo-15 ne503): @c gpio_num 0..15 = gpiochip0 line offset 0..15;
 * 16..31 = gpiochip1 line offset 0..15. Matches @c gpiodetect / @c gpioinfo.
 */

extern "C" {
#include "peripheral/hal_io.h"
}

#include <gpiod.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <pthread.h>
#include <string>
#include <sys/epoll.h>
#include <sys/types.h>
#include <unistd.h>

namespace {

static constexpr unsigned kLinesPerChip = 16u;
static constexpr unsigned kChipCount = 2u;
static constexpr unsigned kHalGpioMax = kLinesPerChip * kChipCount; /* exclusive */

static bool hal_gpio_decode(uint32_t gpio_num, unsigned *chip_idx, unsigned *offset)
{
    if (gpio_num >= kHalGpioMax) {
        return false;
    }
    *chip_idx = gpio_num / kLinesPerChip;
    *offset = gpio_num % kLinesPerChip;
    return true;
}

static const char *consumer_name(const HalGpioConfig *config)
{
    if (config != nullptr && config->label != nullptr && config->label[0] != '\0') {
        return config->label;
    }
    return "aipc_hal";
}

static int request_flags_from_cfg(bool active_low)
{
    return active_low ? GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW : 0;
}

struct GpioSub {
    int fd{-1};
    gpiod_line *line{nullptr};
    bool active_low{false};
    HalGpioEventCallback cb{nullptr};
    void *userdata{nullptr};
};

struct Hailo15IoCtx {
    pthread_mutex_t lock{};
    gpiod_chip *chips[kChipCount]{nullptr, nullptr};
    std::map<uint32_t, gpiod_line *> exported; /* gpio_num -> line (owned request) */
    std::map<uint32_t, bool> exported_active_low;
    int epfd{-1};
    std::atomic<bool> running{false};
    pthread_t evt_thread{};
    std::map<uint32_t, GpioSub> subs;
};

static void *evt_thread_main(void *arg)
{
    auto *ctx = static_cast<Hailo15IoCtx *>(arg);
    epoll_event evs[8];
    while (ctx->running.load()) {
        const int n = epoll_wait(ctx->epfd, evs, 8, 200);
        if (!ctx->running.load()) {
            break;
        }
        if (n <= 0) {
            continue;
        }

        for (int i = 0; i < n; i++) {
            const uint32_t gpio = (uint32_t)(uintptr_t)evs[i].data.ptr;
            HalGpioEventCallback cb = nullptr;
            void *ud = nullptr;
            gpiod_line *line = nullptr;
            pthread_mutex_lock(&ctx->lock);
            const auto it = ctx->subs.find(gpio);
            if (it != ctx->subs.end()) {
                cb = it->second.cb;
                ud = it->second.userdata;
                line = it->second.line;
            }
            pthread_mutex_unlock(&ctx->lock);
            if (line == nullptr || cb == nullptr) {
                continue;
            }

            struct gpiod_line_event ev {};
            const int efd = gpiod_line_event_get_fd(line);
            if (efd < 0) {
                continue;
            }
            for (;;) {
                const int rr = gpiod_line_event_read_fd(efd, &ev);
                if (rr < 0) {
                    break;
                }
                if (rr == 0) {
                    break;
                }
                const int v = gpiod_line_get_value(line);
                if (v < 0) {
                    continue;
                }
                /* With GPIOD_LINE_REQUEST_FLAG_ACTIVE_LOW, gpiod reports logical levels. */
                const bool logical = (v != 0);
                cb(ctx, gpio, logical, ud);
            }
        }
    }
    return nullptr;
}

static int io_init(void **io_ctx_return)
{
    if (io_ctx_return == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    auto *ctx = new (std::nothrow) Hailo15IoCtx();
    if (ctx == nullptr) {
        return HAL_ERR_NO_MEM;
    }
    pthread_mutex_init(&ctx->lock, nullptr);
    for (unsigned c = 0; c < kChipCount; c++) {
        ctx->chips[c] = gpiod_chip_open_by_number(c);
        if (ctx->chips[c] == nullptr) {
            for (unsigned j = 0; j < c; j++) {
                gpiod_chip_close(ctx->chips[j]);
                ctx->chips[j] = nullptr;
            }
            pthread_mutex_destroy(&ctx->lock);
            delete ctx;
            return HAL_ERROR;
        }
        if (gpiod_chip_num_lines(ctx->chips[c]) < kLinesPerChip) {
            for (unsigned j = 0; j <= c; j++) {
                gpiod_chip_close(ctx->chips[j]);
                ctx->chips[j] = nullptr;
            }
            pthread_mutex_destroy(&ctx->lock);
            delete ctx;
            return HAL_ERROR;
        }
    }

    ctx->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (ctx->epfd < 0) {
        gpiod_chip_close(ctx->chips[0]);
        gpiod_chip_close(ctx->chips[1]);
        ctx->chips[0] = ctx->chips[1] = nullptr;
        pthread_mutex_destroy(&ctx->lock);
        delete ctx;
        return HAL_ERROR;
    }
    ctx->running.store(true);
    if (pthread_create(&ctx->evt_thread, nullptr, evt_thread_main, ctx) != 0) {
        ctx->running.store(false);
        close(ctx->epfd);
        gpiod_chip_close(ctx->chips[0]);
        gpiod_chip_close(ctx->chips[1]);
        ctx->chips[0] = ctx->chips[1] = nullptr;
        pthread_mutex_destroy(&ctx->lock);
        delete ctx;
        return HAL_ERROR;
    }
    *io_ctx_return = ctx;
    return HAL_OK;
}

static void close_all_exported_and_subs(Hailo15IoCtx *ctx)
{
    pthread_mutex_lock(&ctx->lock);
    for (auto &kv : ctx->subs) {
        if (kv.second.fd >= 0) {
            (void)epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, kv.second.fd, nullptr);
        }
        if (kv.second.line != nullptr) {
            gpiod_line_release(kv.second.line);
            kv.second.line = nullptr;
        }
        kv.second.fd = -1;
    }
    ctx->subs.clear();

    for (auto &kv : ctx->exported) {
        if (kv.second != nullptr) {
            gpiod_line_release(kv.second);
        }
    }
    ctx->exported.clear();
    ctx->exported_active_low.clear();
    pthread_mutex_unlock(&ctx->lock);
}

static int io_deinit(void *io_ctx)
{
    auto *ctx = static_cast<Hailo15IoCtx *>(io_ctx);
    if (ctx == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }

    ctx->running.store(false);
    pthread_join(ctx->evt_thread, nullptr);

    close_all_exported_and_subs(ctx);

    close(ctx->epfd);
    ctx->epfd = -1;
    if (ctx->chips[0] != nullptr) {
        gpiod_chip_close(ctx->chips[0]);
        ctx->chips[0] = nullptr;
    }
    if (ctx->chips[1] != nullptr) {
        gpiod_chip_close(ctx->chips[1]);
        ctx->chips[1] = nullptr;
    }
    pthread_mutex_destroy(&ctx->lock);
    delete ctx;
    return HAL_OK;
}

static int gpio_export(void *io_ctx, const HalGpioConfig *config)
{
    auto *ctx = static_cast<Hailo15IoCtx *>(io_ctx);
    if (ctx == nullptr || config == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    unsigned chip_idx = 0;
    unsigned offset = 0;
    if (!hal_gpio_decode(config->gpio_num, &chip_idx, &offset)) {
        return HAL_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&ctx->lock);
    if (ctx->exported.find(config->gpio_num) != ctx->exported.end()) {
        pthread_mutex_unlock(&ctx->lock);
        return HAL_ERR_INVALID_STATE;
    }
    pthread_mutex_unlock(&ctx->lock);

    gpiod_chip *chip = ctx->chips[chip_idx];
    gpiod_line *line = gpiod_chip_get_line(chip, offset);
    if (line == nullptr) {
        return HAL_ERROR;
    }

    const char *cons = consumer_name(config);
    const int flags = request_flags_from_cfg(config->active_low);
    int rc = -1;
    if (config->direction == HAL_GPIO_DIR_OUTPUT) {
        rc = gpiod_line_request_output_flags(line, cons, flags, 0);
    } else {
        rc = gpiod_line_request_input_flags(line, cons, flags);
    }
    if (rc != 0) {
        return HAL_ERROR;
    }

    pthread_mutex_lock(&ctx->lock);
    ctx->exported[config->gpio_num] = line;
    ctx->exported_active_low[config->gpio_num] = config->active_low;
    pthread_mutex_unlock(&ctx->lock);
    return HAL_OK;
}

static int gpio_unexport(void *io_ctx, uint32_t gpio_num)
{
    auto *ctx = static_cast<Hailo15IoCtx *>(io_ctx);
    if (ctx == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    unsigned chip_idx = 0;
    unsigned offset = 0;
    if (!hal_gpio_decode(gpio_num, &chip_idx, &offset)) {
        (void)chip_idx;
        (void)offset;
        return HAL_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&ctx->lock);
    if (ctx->subs.find(gpio_num) != ctx->subs.end()) {
        pthread_mutex_unlock(&ctx->lock);
        return HAL_ERR_INVALID_STATE;
    }
    const auto it = ctx->exported.find(gpio_num);
    if (it == ctx->exported.end()) {
        pthread_mutex_unlock(&ctx->lock);
        return HAL_ERR_NOT_FOUND;
    }
    gpiod_line *line = it->second;
    ctx->exported.erase(it);
    ctx->exported_active_low.erase(gpio_num);
    pthread_mutex_unlock(&ctx->lock);

    gpiod_line_release(line);
    return HAL_OK;
}

static gpiod_line *get_exported_line(Hailo15IoCtx *ctx, uint32_t gpio_num)
{
    pthread_mutex_lock(&ctx->lock);
    const auto it = ctx->exported.find(gpio_num);
    gpiod_line *line = (it != ctx->exported.end()) ? it->second : nullptr;
    pthread_mutex_unlock(&ctx->lock);
    return line;
}

static int gpio_set_value(void *io_ctx, uint32_t gpio_num, bool value)
{
    auto *ctx = static_cast<Hailo15IoCtx *>(io_ctx);
    gpiod_line *line = get_exported_line(ctx, gpio_num);
    if (line == nullptr) {
        return HAL_ERR_NOT_FOUND;
    }
    const int v = value ? 1 : 0;
    return (gpiod_line_set_value(line, v) == 0) ? HAL_OK : HAL_ERROR;
}

static int gpio_get_value(void *io_ctx, uint32_t gpio_num, bool *value)
{
    auto *ctx = static_cast<Hailo15IoCtx *>(io_ctx);
    if (value == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    gpiod_line *line = get_exported_line(ctx, gpio_num);
    if (line == nullptr) {
        return HAL_ERR_NOT_FOUND;
    }
    const int v = gpiod_line_get_value(line);
    if (v < 0) {
        return HAL_ERROR;
    }
    *value = (v != 0);
    return HAL_OK;
}

static int gpio_set_direction(void *io_ctx, uint32_t gpio_num, HalGpioDirection dir)
{
    auto *ctx = static_cast<Hailo15IoCtx *>(io_ctx);
    if (ctx == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&ctx->lock);
    if (ctx->subs.find(gpio_num) != ctx->subs.end()) {
        pthread_mutex_unlock(&ctx->lock);
        return HAL_ERR_INVALID_STATE;
    }
    const auto it = ctx->exported.find(gpio_num);
    if (it == ctx->exported.end()) {
        pthread_mutex_unlock(&ctx->lock);
        return HAL_ERR_NOT_FOUND;
    }
    gpiod_line *line = it->second;
    const bool active_low = ctx->exported_active_low[gpio_num];
    pthread_mutex_unlock(&ctx->lock);

    gpiod_line_release(line);
    const char *cons = "aipc_hal";
    int rc = -1;
    if (dir == HAL_GPIO_DIR_OUTPUT) {
        rc = gpiod_line_request_output_flags(line, cons, request_flags_from_cfg(active_low), 0);
    } else {
        rc = gpiod_line_request_input_flags(line, cons, request_flags_from_cfg(active_low));
    }
    if (rc != 0) {
        return HAL_ERROR;
    }
    return HAL_OK;
}

static int gpio_subscribe(void *io_ctx, uint32_t gpio_num, HalGpioEdge edge, HalGpioEventCallback callback,
                          void *userdata)
{
    auto *ctx = static_cast<Hailo15IoCtx *>(io_ctx);
    if (ctx == nullptr || callback == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }
    if (edge == HAL_GPIO_EDGE_NONE) {
        return HAL_ERR_INVALID_ARG;
    }

    gpiod_line *line = nullptr;
    bool active_low = false;
    pthread_mutex_lock(&ctx->lock);
    const auto ex = ctx->exported.find(gpio_num);
    if (ex == ctx->exported.end()) {
        pthread_mutex_unlock(&ctx->lock);
        return HAL_ERR_NOT_FOUND;
    }
    if (ctx->subs.find(gpio_num) != ctx->subs.end()) {
        pthread_mutex_unlock(&ctx->lock);
        return HAL_ERR_INVALID_STATE;
    }
    line = ex->second;
    const auto al_it = ctx->exported_active_low.find(gpio_num);
    active_low = (al_it != ctx->exported_active_low.end()) ? al_it->second : false;
    ctx->exported.erase(ex);
    ctx->exported_active_low.erase(gpio_num);
    pthread_mutex_unlock(&ctx->lock);

    gpiod_line_release(line);
    const char *cons = "aipc_hal";
    const int flags = request_flags_from_cfg(active_low);
    int rc = -1;
    switch (edge) {
    case HAL_GPIO_EDGE_RISING:
        rc = gpiod_line_request_rising_edge_events_flags(line, cons, flags);
        break;
    case HAL_GPIO_EDGE_FALLING:
        rc = gpiod_line_request_falling_edge_events_flags(line, cons, flags);
        break;
    case HAL_GPIO_EDGE_BOTH:
        rc = gpiod_line_request_both_edges_events_flags(line, cons, flags);
        break;
    default:
        return HAL_ERR_INVALID_ARG;
    }
    if (rc != 0) {
        (void)gpiod_line_request_input_flags(line, cons, flags);
        pthread_mutex_lock(&ctx->lock);
        ctx->exported[gpio_num] = line;
        ctx->exported_active_low[gpio_num] = active_low;
        pthread_mutex_unlock(&ctx->lock);
        return HAL_ERROR;
    }

    const int efd = gpiod_line_event_get_fd(line);
    if (efd < 0) {
        (void)gpiod_line_request_input_flags(line, cons, flags);
        pthread_mutex_lock(&ctx->lock);
        ctx->exported[gpio_num] = line;
        ctx->exported_active_low[gpio_num] = active_low;
        pthread_mutex_unlock(&ctx->lock);
        return HAL_ERROR;
    }

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.ptr = (void *)(uintptr_t)gpio_num;
    if (epoll_ctl(ctx->epfd, EPOLL_CTL_ADD, efd, &ev) != 0) {
        (void)gpiod_line_request_input_flags(line, cons, flags);
        pthread_mutex_lock(&ctx->lock);
        ctx->exported[gpio_num] = line;
        ctx->exported_active_low[gpio_num] = active_low;
        pthread_mutex_unlock(&ctx->lock);
        return HAL_ERROR;
    }

    pthread_mutex_lock(&ctx->lock);
    GpioSub &sub = ctx->subs[gpio_num];
    sub.fd = efd;
    sub.line = line;
    sub.active_low = active_low;
    sub.cb = callback;
    sub.userdata = userdata;
    pthread_mutex_unlock(&ctx->lock);
    return HAL_OK;
}

static int gpio_unsubscribe(void *io_ctx, uint32_t gpio_num)
{
    auto *ctx = static_cast<Hailo15IoCtx *>(io_ctx);
    if (ctx == nullptr) {
        return HAL_ERR_INVALID_ARG;
    }

    pthread_mutex_lock(&ctx->lock);
    const auto it = ctx->subs.find(gpio_num);
    if (it == ctx->subs.end()) {
        pthread_mutex_unlock(&ctx->lock);
        return HAL_ERR_NOT_FOUND;
    }
    const int efd = it->second.fd;
    gpiod_line *line = it->second.line;
    const bool active_low = it->second.active_low;
    ctx->subs.erase(it);
    pthread_mutex_unlock(&ctx->lock);

    if (efd >= 0) {
        (void)epoll_ctl(ctx->epfd, EPOLL_CTL_DEL, efd, nullptr);
    }
    if (line != nullptr) {
        gpiod_line_release(line);
        (void)gpiod_line_request_input_flags(line, "aipc_hal", request_flags_from_cfg(active_low));
    }

    pthread_mutex_lock(&ctx->lock);
    ctx->exported[gpio_num] = line;
    ctx->exported_active_low[gpio_num] = active_low;
    pthread_mutex_unlock(&ctx->lock);
    return HAL_OK;
}

static int pwm_configure(void *io_ctx, const HalPwmConfig *config)
{
    (void)io_ctx;
    (void)config;
    return HAL_ERR_NOT_SUPPORTED;
}

static int pwm_set_duty(void *io_ctx, uint32_t pwm_chip, uint32_t pwm_channel, uint32_t duty_ns)
{
    (void)io_ctx;
    (void)pwm_chip;
    (void)pwm_channel;
    (void)duty_ns;
    return HAL_ERR_NOT_SUPPORTED;
}

static int pwm_enable(void *io_ctx, uint32_t pwm_chip, uint32_t pwm_channel, bool enable)
{
    (void)io_ctx;
    (void)pwm_chip;
    (void)pwm_channel;
    (void)enable;
    return HAL_ERR_NOT_SUPPORTED;
}

static const char *io_get_version(void)
{
    return "Hailo15 HAL-IO 2.1.1 (GPIO=libgpiod; PWM=unsupported)";
}

} // namespace

extern "C" {
HalIoOps HAL_IO_OPS = {
    .init = io_init,
    .deinit = io_deinit,
    .gpio_export = gpio_export,
    .gpio_unexport = gpio_unexport,
    .gpio_set_value = gpio_set_value,
    .gpio_get_value = gpio_get_value,
    .gpio_set_direction = gpio_set_direction,
    .gpio_subscribe = gpio_subscribe,
    .gpio_unsubscribe = gpio_unsubscribe,
    .pwm_configure = pwm_configure,
    .pwm_set_duty = pwm_set_duty,
    .pwm_enable = pwm_enable,
    .get_version = io_get_version,
};
}
