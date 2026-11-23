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
    /* Hard cap for hardware-in-use frames (see FD_PUB_MSG_FRAME_IN_USE).
     * In-use frames are exempt from frame_timeout (their buffer backs an
     * async device job that has not completed yet) and are force-reclaimed
     * only after this longer deadline. 0 disables the exemption: in-use
     * frames then follow frame_timeout like everything else. */
    std::chrono::milliseconds in_use_timeout{30000};
};

struct WatchdogStats {
    uint64_t total_reclaimed = 0;
    uint64_t total_warnings = 0;
    uint64_t total_in_use_reclaimed = 0;
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

    /** Declare one hardware-in-use reference on a tracked frame. Returns
     * true iff the frame is still tracked AND the declaration landed
     * atomically (i.e. the scan loop had not queued it for reclaim yet):
     * callers must treat false as "no protection granted". Idempotent per
     * declaring client — multiple declarations stack references; each must
     * be dropped via clear_in_use(). No-op returning false when the frame
     * is not tracked anymore (e.g. declared after a final release). */
    bool mark_in_use(uint64_t frame_id);

    /** Drop one in-use reference taken by mark_in_use(). When the count
     * reaches zero the frame reverts to the normal frame_timeout deadline.
     * No-op for unknown frames. */
    void clear_in_use(uint64_t frame_id);

    WatchdogStats get_stats() const;

private:
    WatchdogConfig config_;
    ForceReclaimFn reclaim_fn_;
    std::thread thread_;
    std::atomic<bool> running_{false};

    mutable std::mutex mu_;
    struct TrackedFrame {
        std::chrono::steady_clock::time_point lend_time;
        /* Number of live hardware-in-use declarations (one per declaring
         * client connection); the exemption holds while > 0. */
        uint32_t in_use = 0;
    };
    std::unordered_map<uint64_t, TrackedFrame> tracked_;

    WatchdogStats stats_{};

    void scan_loop();
};
