/**
 * @file lens_image_probe.cpp
 * @brief Image-sharpness differential probe implementation.
 */

#include "../include/lens_image_probe.h"
#include "../include/autofocus_controller.h"
#include "../include/frame_router.h"
#include "../include/lens_controller.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <vector>

extern "C" {
#include "hal_common.h"
#include "hal_log.h"
}

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(500);

double median_of(std::vector<double> v) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

/* RAII holder for the lens autofocus-operation lock: while held, user motion
 * RPCs and autofocus jobs are busy-rejected instead of interleaving with
 * the probe's measurement window. */
class AfOpGuard {
public:
    explicit AfOpGuard(LensController* lens) : lens_(lens) {}
    ~AfOpGuard() {
        if (held_) lens_->end_autofocus_operation();
    }
    bool try_acquire() {
        if (lens_->begin_autofocus_operation()) {
            held_ = true;
            return true;
        }
        return false;
    }

private:
    LensController* lens_;
    bool held_ = false;
};

struct StatPoint {
    bool ok = false;
    double metric = 0.0;   // median across frames of the sharpest AF window
    double luma = 0.0;     // median luma of that window (AE-shift guard)
    double spread = 1.0;   // max |frame - median| / median (stability)
};

}  // namespace

const char* lens_image_probe_result_name(LensImageProbeResult result) {
    switch (result) {
        case LensImageProbeResult::Motorized: return "motorized";
        case LensImageProbeResult::FixedLens: return "fixed";
        case LensImageProbeResult::Inconclusive:
        default: return "inconclusive";
    }
}

LensImageProbeResult run_lens_image_probe(HalIspOps* isp_ops, void* video_ctx,
                                          FrameRouter* frame_router,
                                          LensController* lens,
                                          AutofocusController* autofocus,
                                          const LensImageProbeConfig& cfg,
                                          const std::atomic<bool>* stop_flag,
                                          double* return_dev_out) {
    if (return_dev_out) *return_dev_out = 1.0;
    if (!isp_ops || !isp_ops->get_af_measurement || !video_ctx ||
        !frame_router || !lens) {
        HAL_LOG_WARNING("Lens image probe: missing ISP/video/lens deps; skipped");
        return LensImageProbeResult::Inconclusive;
    }

    /* Readiness: lens parked, autofocus idle for two consecutive polls, and
     * the AF statistics path answering. The boot one-shot autofocus pass
     * (and an archived-position restore) runs first; the probe observes it
     * through the busy flag and only then takes over. The operation lock is
     * taken inside the same loop: a job that is queued but not yet running
     * reads idle here, wins the lock at start-up, and would otherwise make
     * the probe give up (or worse, make the queued job fail) — so a lost
     * race simply resets the streak and waits the scan out. */
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(cfg.ready_timeout_ms);
    AfOpGuard af_guard(lens);
    bool af_lock_held = false;
    int idle_streak = 0;
    bool stats_ok = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (stop_flag && stop_flag->load()) {
            HAL_LOG_INFO("Lens image probe: cancelled before start");
            return LensImageProbeResult::Inconclusive;
        }
        if (lens->initialized() &&
            (!autofocus || !autofocus->status().busy)) {
            ++idle_streak;
        } else {
            idle_streak = 0;
        }
        if (idle_streak >= 2) {
            HalIspAfMeasurement probe_meas{};
            if (!stats_ok &&
                frame_router->wait_next_frames(cfg.stream_name, 1,
                    std::chrono::milliseconds(cfg.frame_wait_timeout_ms)) &&
                isp_ops->get_af_measurement(video_ctx, &probe_meas) == HAL_OK) {
                stats_ok = true;
            }
            /* Competing for the lock only after the streak and stats: once
             * held, no autofocus job can start mid-measurement (its
             * begin_autofocus_operation fails and the job errors out). */
            if (stats_ok && af_guard.try_acquire()) {
                af_lock_held = true;
                break;
            }
            if (stats_ok) idle_streak = 0;  // a job took the lock; wait it out
        }
        std::this_thread::sleep_for(kPollInterval);
    }
    if (!af_lock_held) {
        HAL_LOG_WARNING("Lens image probe: lens/AF/stats not ready within %d ms; "
                        "identity stays fg2009", cfg.ready_timeout_ms);
        return LensImageProbeResult::Inconclusive;
    }

    /* One measurement point: sync to fresh frames, then take cfg.frames
     * measurements one frame apart. Per frame the metric is the sharpest AF
     * window's focus energy — the same signal the autofocus scanner rides. */
    auto sample = [&](StatPoint* out) -> bool {
        std::vector<double> metrics, lumas;
        if (!frame_router->wait_next_frames(cfg.stream_name,
                                            std::max(1, cfg.frames),
                                            std::chrono::milliseconds(
                                                cfg.frame_wait_timeout_ms))) {
            return false;
        }
        for (int i = 0; i < std::max(1, cfg.frames); ++i) {
            HalIspAfMeasurement m{};
            if (isp_ops->get_af_measurement(video_ctx, &m) != HAL_OK) return false;
            uint32_t best_sum = 0, best_luma = 0;
            for (int w = 0; w < HAL_ISP_AF_MAX_WINDOWS; ++w) {
                if (m.sum[w] > best_sum) {
                    best_sum = m.sum[w];
                    best_luma = m.luma[w];
                }
            }
            metrics.push_back(static_cast<double>(best_sum));
            lumas.push_back(static_cast<double>(best_luma));
            if (i + 1 < cfg.frames &&
                !frame_router->wait_next_frames(cfg.stream_name, 1,
                    std::chrono::milliseconds(cfg.frame_wait_timeout_ms))) {
                return false;
            }
        }
        const double med = median_of(metrics);
        if (med <= 0.0) return false;
        double max_dev = 0.0;
        for (const double v : metrics) {
            max_dev = std::max(max_dev, std::abs(v - med) / med);
        }
        *out = StatPoint{true, med, median_of(lumas), max_dev};
        return true;
    };

    LensControllerState st{};
    if (lens->state_get(&st) != HAL_OK) {
        HAL_LOG_WARNING("Lens image probe: lens state unreadable; identity stays fg2009");
        return LensImageProbeResult::Inconclusive;
    }
    const int32_t p0 = st.focus_pos;
    /* Jog away from the nearer end so the delta never rides a limit; the
     * FG2009 travel is 0..2453 with the park resting near the far end. */
    const int32_t target = p0 >= 1226 ? p0 - cfg.steps : p0 + cfg.steps;

    StatPoint s0{}, s1{}, s2{};
    if (!sample(&s0)) {
        HAL_LOG_WARNING("Lens image probe: baseline sampling failed; identity stays fg2009");
        return LensImageProbeResult::Inconclusive;
    }

    if (lens->focus_abs_wait(cfg.pps, target,
                             static_cast<uint32_t>(cfg.move_timeout_ms)) != HAL_OK) {
        HAL_LOG_WARNING("Lens image probe: focus jog rejected (ret=%d-style failure); "
                        "identity stays fg2009", 0);
        return LensImageProbeResult::Inconclusive;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.settle_ms));
    const bool jog_sampled = sample(&s1);
    if (lens->focus_abs_wait(cfg.pps, p0,
                             static_cast<uint32_t>(cfg.move_timeout_ms)) != HAL_OK) {
        HAL_LOG_WARNING("Lens image probe: focus return move failed; identity stays fg2009");
        return LensImageProbeResult::Inconclusive;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(cfg.settle_ms));
    const bool ret_sampled = sample(&s2);
    if (!jog_sampled || !ret_sampled) {
        HAL_LOG_WARNING("Lens image probe: post-jog sampling failed; identity stays fg2009");
        return LensImageProbeResult::Inconclusive;
    }

    /* Verdict gates — a "fixed" verdict requires a textured, stable scene:
     * absence of change is only evidence of absence when the scene could
     * have shown it. */
    const double dev_jog = std::abs(s1.metric - s0.metric) / s0.metric;
    const double dev_ret = std::abs(s2.metric - s0.metric) / s0.metric;
    const double span = std::max(dev_jog, std::abs(s1.metric - s2.metric) / s0.metric);
    HAL_LOG_INFO("Lens image probe: base=%.0f jog=%.0f ret=%.0f "
                 "(dev_jog=%.1f%% dev_ret=%.1f%% spread0=%.1f%% luma %.0f->%.0f)",
                 s0.metric, s1.metric, s2.metric,
                 dev_jog * 100.0, dev_ret * 100.0, s0.spread * 100.0,
                 s0.luma, s2.luma);

    if (s0.metric < static_cast<double>(cfg.texture_floor)) {
        HAL_LOG_INFO("Lens image probe: below texture floor (%.0f < %u); inconclusive",
                     s0.metric, cfg.texture_floor);
        return LensImageProbeResult::Inconclusive;
    }
    if (s0.spread > cfg.flat_ratio) {
        HAL_LOG_INFO("Lens image probe: baseline unstable (spread %.1f%%); inconclusive",
                     s0.spread * 100.0);
        return LensImageProbeResult::Inconclusive;
    }
    const double luma_shift = s0.luma > 0.0
        ? std::abs(s2.luma - s0.luma) / s0.luma : 1.0;
    if (luma_shift > cfg.luma_guard_ratio) {
        HAL_LOG_INFO("Lens image probe: exposure/scene shifted (luma %.1f%%); inconclusive",
                     luma_shift * 100.0);
        return LensImageProbeResult::Inconclusive;
    }

    if (span >= cfg.motor_ratio) {
        // A dip this deep is one-way evidence: only a motor answers. The
        // return quality is reported but not required — open-loop gears
        // have hysteresis, so the return jog lands 20-40% short of the
        // baseline sharpness on a perfectly healthy motorized lens.
        HAL_LOG_INFO("Lens image probe: %s (statistics tracked the %d-step jog)",
                     lens_image_probe_result_name(LensImageProbeResult::Motorized),
                     cfg.steps);
        if (return_dev_out) *return_dev_out = dev_ret;
        return LensImageProbeResult::Motorized;
    }
    if (span <= cfg.flat_ratio) {
        HAL_LOG_INFO("Lens image probe: %s (textured scene, statistics flat)",
                     lens_image_probe_result_name(LensImageProbeResult::FixedLens));
        if (return_dev_out) *return_dev_out = dev_ret;
        return LensImageProbeResult::FixedLens;
    }
    HAL_LOG_INFO("Lens image probe: response between flat and motor bands; inconclusive");
    if (return_dev_out) *return_dev_out = dev_ret;
    return LensImageProbeResult::Inconclusive;
}
