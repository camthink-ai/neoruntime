/**
 * @file motion_test.cpp
 * @brief Deterministic HAL motion-detection test: enable the engine, toggle
 * digital zoom (large frame change) and expect START/STOP transitions.
 */
#include "common/hal_common.h"
#include "media/hal_media.h"

#include <chrono>
#include <cstdio>
#include <thread>

static volatile int g_events = 0;
static void motion_cb(void *ctx, bool detected, uint64_t frame, uint64_t ts, void *user)
{
    (void)ctx; (void)frame; (void)ts; (void)user;
    ++g_events;
    std::printf("[motion] %s\n", detected ? "START" : "STOP");
    std::fflush(stdout);
}

int main()
{
    HalMediaConfig cfg{};
    void *ctx = nullptr;
    int rc = HAL_MEDIA_OPS.init(&cfg, &ctx);
    if (rc != 0 || !ctx) { std::printf("init rc=%d\n", rc); return 1; }
    rc = HAL_MEDIA_OPS.start(ctx);
    std::printf("start rc=%d\n", rc);
    std::this_thread::sleep_for(std::chrono::seconds(4));

    HalMotionConfig mc{};
    mc.enabled = true;
    mc.sensitivity = HAL_MOTION_SENSITIVITY_HIGH;
    mc.threshold = 0.02f;
    rc = HAL_MEDIA_OPS.set_motion_config ? HAL_MEDIA_OPS.set_motion_config(ctx, &mc) : -1;
    std::printf("motion_set rc=%d\n", rc);
    rc = HAL_MEDIA_OPS.subscribe_motion ? HAL_MEDIA_OPS.subscribe_motion(ctx, motion_cb, nullptr) : -1;
    std::printf("motion_sub rc=%d\n", rc);
    std::this_thread::sleep_for(std::chrono::seconds(3));

    for (int i = 0; i < 3; ++i)
    {
        HalMediaImageConfig ic{};
        ic.digital_zoom = true;
        ic.digital_zoom_value = 4;
        rc = HAL_MEDIA_OPS.dynamic_change_image_config(ctx, &ic);
        std::printf("zoom_on rc=%d\n", rc);
        std::this_thread::sleep_for(std::chrono::seconds(4));
        ic.digital_zoom = false;
        rc = HAL_MEDIA_OPS.dynamic_change_image_config(ctx, &ic);
        std::printf("zoom_off rc=%d\n", rc);
        std::this_thread::sleep_for(std::chrono::seconds(4));
    }

    std::printf("events=%d -> %s\n", g_events, g_events >= 2 ? "PASS" : "FAIL");
    (void)HAL_MEDIA_OPS.unsubscribe_motion(ctx);
    (void)HAL_MEDIA_OPS.stop(ctx);
    (void)HAL_MEDIA_OPS.deinit(ctx);
    return g_events >= 2 ? 0 : 1;
}
