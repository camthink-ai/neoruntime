/**
 * @file frame_watchdog.cpp
 * @brief Frame Watchdog Implementation
 */

#include "../include/frame_watchdog.h"
#include <unordered_map>
#include <vector>

extern "C" {
    #include "hal_log.h"
}

FrameWatchdog::FrameWatchdog(WatchdogConfig config)
    : config_(config) {}

FrameWatchdog::~FrameWatchdog() {
    stop();
}

void FrameWatchdog::start(ForceReclaimFn reclaim_fn) {
    if (running_.load()) return;

    reclaim_fn_ = std::move(reclaim_fn);
    running_.store(true);
    thread_ = std::thread(&FrameWatchdog::scan_loop, this);
    HAL_LOG_INFO("FrameWatchdog: Started (timeout=%ldms, scan=%ldms)",
                 config_.frame_timeout.count(),
                 config_.scan_interval.count());
}

void FrameWatchdog::stop() {
    if (!running_.load()) return;

    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
    HAL_LOG_INFO("FrameWatchdog: Stopped (reclaimed=%lu, warnings=%lu)",
                 stats_.total_reclaimed, stats_.total_warnings);
}

void FrameWatchdog::track(uint64_t frame_id,
                          std::chrono::steady_clock::time_point lend_time) {
    std::lock_guard<std::mutex> lock(mu_);
    tracked_[frame_id] = {lend_time};
}

void FrameWatchdog::untrack(uint64_t frame_id) {
    std::lock_guard<std::mutex> lock(mu_);
    tracked_.erase(frame_id);
}

WatchdogStats FrameWatchdog::get_stats() const {
    return stats_;
}

void FrameWatchdog::scan_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(config_.scan_interval);

        auto now = std::chrono::steady_clock::now();
        std::vector<uint64_t> to_reclaim;

        {
            std::lock_guard<std::mutex> lock(mu_);
            for (auto& [fid, tf] : tracked_) {
                auto elapsed = std::chrono::duration_cast<
                    std::chrono::milliseconds>(now - tf.lend_time);

                if (elapsed >= config_.frame_timeout) {
                    HAL_LOG_ERROR("Watchdog: Frame %lu held %ldms, force reclaim",
                                 fid, elapsed.count());
                    to_reclaim.push_back(fid);
                } else if (elapsed >= config_.warn_threshold) {
                    HAL_LOG_WARNING("Watchdog: Frame %lu held %ldms",
                                   fid, elapsed.count());
                    stats_.total_warnings++;
                }
            }

            // Remove from tracking (reclaim_fn will do the actual release)
            for (auto fid : to_reclaim) {
                tracked_.erase(fid);
                stats_.total_reclaimed++;
            }
        }

        // Execute reclaim outside the lock
        for (auto fid : to_reclaim) {
            if (reclaim_fn_) {
                reclaim_fn_(fid);
            }
        }
    }
}
