/**
 * @file frame_watchdog.h
 * @brief Frame Watchdog - Timeout-based forced frame reclamation
 *
 * Scans outstanding frames and force-reclaims any held longer than threshold.
 * Protects HAL buffer pool from exhaustion by slow/crashed consumers.
 */

#pragma once

#include <cstdint>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>
#include <unordered_map>

struct WatchdogConfig {
    std::chrono::milliseconds scan_interval{50};
    std::chrono::milliseconds frame_timeout{200};
    std::chrono::milliseconds warn_threshold{150};
};

struct WatchdogStats {
    uint64_t total_reclaimed = 0;
    uint64_t total_warnings = 0;
};

/** Callback to force-reclaim a frame by its ID */
using ForceReclaimFn = std::function<void(uint64_t frame_id)>;

class FrameWatchdog {
public:
    explicit FrameWatchdog(WatchdogConfig config);
    ~FrameWatchdog();

    FrameWatchdog(const FrameWatchdog&) = delete;
    FrameWatchdog& operator=(const FrameWatchdog&) = delete;

    /** Start watchdog thread. reclaim_fn is called for timed-out frames. */
    void start(ForceReclaimFn reclaim_fn);
    void stop();

    /** Track/untrack a frame's lend time */
    void track(uint64_t frame_id,
               std::chrono::steady_clock::time_point lend_time);
    void untrack(uint64_t frame_id);

    WatchdogStats get_stats() const;

private:
    WatchdogConfig config_;
    ForceReclaimFn reclaim_fn_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex mu_;
    struct TrackedFrame {
        std::chrono::steady_clock::time_point lend_time;
    };
    std::unordered_map<uint64_t, TrackedFrame> tracked_;

    WatchdogStats stats_{};

    void scan_loop();
};
