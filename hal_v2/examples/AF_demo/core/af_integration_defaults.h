#pragma once

#include "af_follow.h"
#include "af_metric.h"

#include <algorithm>
#include <cmath>

namespace hal_auto_af
{

/** Fixed product-integration baseline copied from the current auto_af_test. */
struct IntegrationDefaults
{
    MetricConfig metric{};
    FollowConfig follow{};

    int focus_min = -844;
    int focus_max = 592;
    int pps = 1600;

    int coarse_span = 160;
    int coarse_step = 40;
    int fine_span = 24;
    int fine_step = 8;
    int max_moves = 200;

    int fit_min_samples = 6;
    double fit_min_curvature = 1e-12;
    int early_stop_min_spread = 64;
    int fine_rescan_if_global_outside = 1;

    int exploration_sync_frames = 1;
    int exploration_metric_frames = 1;
    int verification_sync_frames = 1;
    int verification_metric_frames = 3;
    int vsync_timeout_ms = 900;

    int startup_af_enable = 1;
    int startup_af_settle_frames = 10;
    int startup_af_max_tries = 1;

    double peak_verify_min_frac = 0.55;
    int peak_verify_max_tries = 2;
    double peak_min_prominence = 0.10;
    double peak_noise_floor = 0.03;
    double peak_fast_noise_floor = 0.05;
    double peak_plateau_ratio = 0.03;
    int peak_open_boundary_margin_steps = 16;
    double peak_luma_mad_limit = 0.08;

    int symmetric_scan = 1;
    int peak_closure_shadow = 1;
    int peak_closure_recovery = 0;
    int confidence_v2_enable = 1;
    double confidence_accept = 0.80;
    double confidence_recovery = 0.65;

    int follow_path_auto_reanchor = 1;
    int follow_path_auto_reanchor_max_tries = 1;
};

/** Default product three-window layout in video-stream coordinates. */
inline HalIspAfWindowsConfig make_default_af_windows(int video_width, int video_height)
{
    HalIspAfWindowsConfig config{};
    if (video_width <= 0 || video_height <= 0)
    {
        return config;
    }

    config.enabled = true;
    config.window_count = 3;
    const double x[] = {0.10, 0.40, 0.70};
    /* 4K reference: y=900, keeping the window centers slightly below image center. */
    constexpr double y = 900.0 / 2160.0;
    constexpr double width = 0.20;
    constexpr double height = 0.240740741;
    const int min_width = std::max(32, video_width / 32);
    const int min_height = std::max(32, video_height / 32);

    for (uint32_t i = 0; i < config.window_count; ++i)
    {
        int px = static_cast<int>(std::lround(video_width * x[i]));
        int py = static_cast<int>(std::lround(video_height * y));
        const int window_width = std::max(min_width, static_cast<int>(std::lround(video_width * width)));
        const int window_height = std::max(min_height, static_cast<int>(std::lround(video_height * height)));
        px = std::min(px, video_width - window_width);
        py = std::min(py, video_height - window_height);
        config.windows[i].x = std::max(0, px);
        config.windows[i].y = std::max(0, py);
        config.windows[i].w = std::min(window_width, video_width - config.windows[i].x);
        config.windows[i].h = std::min(window_height, video_height - config.windows[i].y);
    }
    return config;
}

} // namespace hal_auto_af
