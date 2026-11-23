/* frame_watchdog_in_use_test — verifies the FD_PUB_MSG_FRAME_IN_USE exemption:
 *   1. a normal frame is force-reclaimed at frame_timeout;
 *   2. an in-use frame survives frame_timeout and is only reclaimed at the
 *      longer in-use hard cap;
 *   3. mark_in_use on an unknown/released frame is a no-op;
 *   4. untrack (RELEASE path) prevents any reclaim;
 *   5. in_use_timeout == 0 disables the exemption (in-use ages normally).
 */
#include "frame_watchdog.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>

using Clock = std::chrono::steady_clock;

namespace {
std::mutex g_mu;
std::set<uint64_t> g_reclaimed;

void record_reclaim(uint64_t frame_id) {
    std::lock_guard<std::mutex> lock(g_mu);
    g_reclaimed.insert(frame_id);
}

bool was_reclaimed(uint64_t frame_id) {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_reclaimed.count(frame_id) > 0;
}

// Runs a watchdog with the given config and invokes `scenario` while it scans.
template <typename Fn>
void with_watchdog(WatchdogConfig cfg, Fn&& scenario) {
    g_reclaimed.clear();
    FrameWatchdog wd(cfg);
    wd.start([&](uint64_t frame_id) { record_reclaim(frame_id); });
    scenario(wd);
    wd.stop();
}

bool wait_for(uint64_t frame_id, std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    while (Clock::now() < deadline) {
        if (was_reclaimed(frame_id)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return was_reclaimed(frame_id);
}
}  // namespace

int main() {
    // Loud timeouts are tiny: frame 40ms, in-use cap 160ms, scan 5ms.
    WatchdogConfig cfg;
    cfg.scan_interval = std::chrono::milliseconds(5);
    cfg.frame_timeout = std::chrono::milliseconds(40);
    cfg.warn_threshold = std::chrono::milliseconds(1000);  // silence warns
    cfg.in_use_timeout = std::chrono::milliseconds(160);

    /* 1 + 2: sibling frames, one declared in-use. */
    with_watchdog(cfg, [&](FrameWatchdog& wd) {
        const auto now = Clock::now();
        wd.track(1, now);          // normal frame
        wd.track(2, now);          // will be declared hw-in-use
        assert(wd.mark_in_use(2)); // grant confirmed on a tracked frame

        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        // Past frame_timeout: the normal frame is gone, the in-use one is not.
        assert(was_reclaimed(1));
        assert(!was_reclaimed(2));

        assert(wait_for(2, std::chrono::milliseconds(400)));
    });

    /* 3: mark after untrack (late declaration for a released frame) reports
     *    failure — the caller must learn that NO protection was granted. */
    with_watchdog(cfg, [&](FrameWatchdog& wd) {
        wd.track(3, Clock::now());
        wd.untrack(3);             // RELEASE arrived first
        assert(!wd.mark_in_use(3)); // refused, not silently "accepted"
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        assert(!was_reclaimed(3));
    });

    /* 4: untrack wins even for an in-use frame (final RELEASE path). */
    with_watchdog(cfg, [&](FrameWatchdog& wd) {
        wd.track(4, Clock::now());
        assert(wd.mark_in_use(4));
        wd.untrack(4);
        std::this_thread::sleep_for(std::chrono::milliseconds(220));
        assert(!was_reclaimed(4));
    });

    /* 4b: clear_in_use drops ONE reference — the frame keeps the exemption
     *     while other declarers remain and reverts to the normal deadline
     *     when the last one clears (RELEASE semantics). */
    with_watchdog(cfg, [&](FrameWatchdog& wd) {
        const auto now = Clock::now();
        wd.track(6, now);
        assert(wd.mark_in_use(6));   // declarer A
        assert(wd.mark_in_use(6));   // declarer B
        wd.clear_in_use(6);          // A released
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        assert(!was_reclaimed(6));   // B's exemption still holds
        wd.clear_in_use(6);          // B released → count zero
        assert(wait_for(6, std::chrono::milliseconds(300)));
    });

    /* 4c: clear on an unknown frame is a harmless no-op. */
    with_watchdog(cfg, [&](FrameWatchdog& wd) {
        wd.clear_in_use(999);
        wd.track(7, Clock::now());
        wd.clear_in_use(7);          // underflow guard: count floors at 0
        wd.clear_in_use(7);
        assert(wd.mark_in_use(7));   // still grantable afterwards
        wd.untrack(7);
    });

    /* 5: in_use_timeout == 0 disables the exemption. */
    WatchdogConfig disabled = cfg;
    disabled.in_use_timeout = std::chrono::milliseconds(0);
    with_watchdog(disabled, [&](FrameWatchdog& wd) {
        wd.track(5, Clock::now());
        assert(wd.mark_in_use(5));
        assert(wait_for(5, std::chrono::milliseconds(300)));
    });

    std::printf("frame_watchdog_in_use_test: all assertions passed\n");
    return 0;
}
