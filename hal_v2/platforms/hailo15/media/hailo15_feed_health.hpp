/**
 * @file hailo15_feed_health.hpp
 * @brief Per-stream auto-feed failure streak accounting for the frontend bridge.
 *
 * Standalone on purpose (only <atomic>/<cstdint>): the accounting logic is
 * unit-testable on the host without the vendor MediaLibrary headers, while
 * the production users (frontend bridge, media HAL ops) include it via
 * hailo15_media_priv.hpp.
 */
#pragma once

#include <atomic>
#include <cstdint>

/**
 * Failure-streak state for one frontend stream's encoder auto-feed path.
 *
 * Updated from GStreamer streaming threads inside the frontend callback:
 * every field is an std::atomic and this path never takes a lock. (The
 * previous std::map<std::string,size_t> counter was itself raced by
 * concurrent operator[] inserts from multiple streaming threads.) The map
 * holding these entries is built once at connect time (single-threaded);
 * afterwards only the atomics inside the prebuilt entries change.
 */
struct Hailo15FeedHealth
{
    /** steady-clock ms of the first failure of the current streak; 0 = healthy. */
    std::atomic<uint64_t> failing_since_ms{0};
    std::atomic<uint32_t> consecutive_failures{0};
    std::atomic<uint32_t> total_failures{0};
    /** vendor media_library_return of the most recent failure (or a sentinel
     *  documented at the call site, e.g. -1 for "no encoder object"). */
    std::atomic<int32_t> last_return_code{0};
    /** Latched once an uninterrupted failure run outlived the window. */
    std::atomic<bool> stall_alarm_latched{false};
};

/**
 * Record one add_buffer failure. Returns true when THIS call is the one that
 * crossed the alarm threshold (exactly one caller logs the alarm; the CAS
 * makes the latch race-free across streaming threads).
 */
inline bool feed_health_record_failure(Hailo15FeedHealth &fh, int32_t vendor_ret,
                                       uint64_t now_ms, uint32_t window_ms)
{
    uint64_t first = fh.failing_since_ms.load(std::memory_order_relaxed);
    if (first == 0)
    {
        fh.failing_since_ms.store(now_ms, std::memory_order_relaxed);
        first = now_ms;
    }
    fh.consecutive_failures.fetch_add(1, std::memory_order_relaxed);
    fh.total_failures.fetch_add(1, std::memory_order_relaxed);
    fh.last_return_code.store(vendor_ret, std::memory_order_relaxed);
    if (!fh.stall_alarm_latched.load(std::memory_order_acquire) &&
        now_ms - first >= window_ms)
    {
        bool expected = false;
        if (fh.stall_alarm_latched.compare_exchange_strong(
                expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
        {
            return true;
        }
    }
    return false;
}

/**
 * Record one successful add_buffer: clears the streak. Returns the previous
 * latched state so the caller can log recovery exactly once.
 */
inline bool feed_health_record_success(Hailo15FeedHealth &fh)
{
    if (fh.consecutive_failures.load(std::memory_order_relaxed) == 0)
    {
        return false;
    }
    fh.consecutive_failures.store(0, std::memory_order_relaxed);
    fh.failing_since_ms.store(0, std::memory_order_relaxed);
    return fh.stall_alarm_latched.exchange(false, std::memory_order_acq_rel);
}
