/**
 * @file hailo15_feed_health_test.cpp
 * @brief Host unit test for the auto-feed failure streak accounting
 *        (hailo15_feed_health.hpp) used by the FEED_STALL_ALARM escalation.
 *
 * Covers: threshold latching fires exactly once per streak, recovery clears
 * the latch exactly once, sub-window failures do not latch, and the
 * concurrent-recording race (multiple streaming threads) yields a single
 * alarm winner.
 */
#include "hailo15_feed_health.hpp"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                        \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,   \
                         #cond);                                           \
            ++g_failures;                                                  \
        }                                                                  \
    } while (0)

void test_threshold_latches_once()
{
    Hailo15FeedHealth fh;
    constexpr uint32_t kWindow = 2000;

    // Sub-window failures never latch.
    CHECK(!feed_health_record_failure(fh, 5, 100, kWindow));
    CHECK(!feed_health_record_failure(fh, 5, 1500, kWindow));
    CHECK(fh.consecutive_failures.load() == 2);
    CHECK(fh.stall_alarm_latched.load() == false);

    // Crossing the window latches — and exactly once.
    CHECK(feed_health_record_failure(fh, 5, 2100, kWindow));
    CHECK(!feed_health_record_failure(fh, 5, 2200, kWindow));
    CHECK(!feed_health_record_failure(fh, 5, 5000, kWindow));
    CHECK(fh.stall_alarm_latched.load() == true);
    CHECK(fh.total_failures.load() == 5);
    CHECK(fh.last_return_code.load() == 5);
    CHECK(fh.failing_since_ms.load() == 100);
}

void test_success_resets_and_reports_recovery_once()
{
    Hailo15FeedHealth fh;
    constexpr uint32_t kWindow = 2000;

    // A streak must run past the window before the latch (and thus a
    // recovery report) exists.
    CHECK(!feed_health_record_failure(fh, 7, 100, kWindow));
    CHECK(feed_health_record_failure(fh, 7, 2200, kWindow));
    CHECK(fh.stall_alarm_latched.load() == true);

    // First success reports the latched recovery; a second does not.
    CHECK(feed_health_record_success(fh));
    CHECK(!feed_health_record_success(fh));
    CHECK(fh.consecutive_failures.load() == 0);
    CHECK(fh.failing_since_ms.load() == 0);
    CHECK(fh.stall_alarm_latched.load() == false);

    // A fresh streak starts a new window.
    CHECK(!feed_health_record_failure(fh, 7, 9000, kWindow));
    CHECK(fh.failing_since_ms.load() == 9000);
}

void test_second_streak_alarms_again()
{
    Hailo15FeedHealth fh;
    constexpr uint32_t kWindow = 2000;

    // First streak: alarm, recover.
    CHECK(!feed_health_record_failure(fh, 3, 100, kWindow));
    CHECK(feed_health_record_failure(fh, 3, 2200, kWindow));
    CHECK(feed_health_record_success(fh));

    // Second fault in the same process must latch again (and log its
    // early diagnostics): lifetime counters must not suppress it.
    CHECK(!feed_health_record_failure(fh, 3, 10000, kWindow));
    CHECK(feed_health_record_failure(fh, 3, 12500, kWindow));
    CHECK(fh.stall_alarm_latched.load() == true);
    CHECK(fh.total_failures.load() == 4);
}

void test_healthy_stream_stays_silent()
{
    Hailo15FeedHealth fh;
    feed_health_record_success(fh); // no prior failures -> no recovery report
    CHECK(fh.consecutive_failures.load() == 0);
    CHECK(fh.total_failures.load() == 0);
}

void test_concurrent_recorders_single_alarm()
{
    Hailo15FeedHealth fh;
    constexpr uint32_t kWindow = 100;
    std::atomic<int> alarms{0};

    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&fh, &alarms, t]() {
            for (int i = 0; i < 500; ++i) {
                // All threads far past the window: at most one alarm total.
                if (feed_health_record_failure(fh, t, 1000 + i, kWindow)) {
                    alarms.fetch_add(1);
                }
            }
        });
    }
    for (auto& th : threads) th.join();

    CHECK(alarms.load() == 1);
    CHECK(fh.stall_alarm_latched.load() == true);
    CHECK(fh.total_failures.load() == 4 * 500);
}

} // namespace

int main()
{
    test_threshold_latches_once();
    test_success_resets_and_reports_recovery_once();
    test_healthy_stream_stays_silent();
    test_second_streak_alarms_again();
    test_concurrent_recorders_single_alarm();
    if (g_failures == 0) {
        std::printf("hailo15_feed_health_test: all checks passed\n");
        return 0;
    }
    std::printf("hailo15_feed_health_test: %d check(s) failed\n", g_failures);
    return 1;
}
