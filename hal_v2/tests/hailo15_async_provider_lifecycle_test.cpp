#include "platforms/hailo15/model/hailo15_async_provider_lifecycle.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace {

using hal_v2::hailo15::Hailo15AsyncProviderLifecycle;
using Attempt = Hailo15AsyncProviderLifecycle::Attempt;
using Result = Hailo15AsyncProviderLifecycle::SubmissionResult;
using namespace std::chrono_literals;

int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __func__ << ": check failed: " #condition << '\n';  \
            ++failures;                                                        \
        }                                                                      \
    } while (false)

void inline_callback_runs_before_submit_returns() {
    Hailo15AsyncProviderLifecycle lifecycle;
    std::atomic<bool> submit_returned{false};
    std::atomic<int> callbacks{0};

    const int rc = lifecycle.submit(
        [&](Attempt attempt) {
            attempt.complete(HAL_OK);
            return Result::accepted();
        },
        [&](int status) {
            CHECK(status == HAL_OK);
            CHECK(!submit_returned.load(std::memory_order_acquire));
            callbacks.fetch_add(1, std::memory_order_relaxed);
        });
    submit_returned.store(true, std::memory_order_release);

    CHECK(rc == HAL_OK);
    CHECK(callbacks.load() == 1);
    CHECK(lifecycle.pending_count() == 0);
}

void callback_then_vendor_failure_keeps_callback_ownership() {
    for (const bool should_throw : {false, true}) {
        Hailo15AsyncProviderLifecycle lifecycle;
        std::atomic<int> callbacks{0};

        const int rc = lifecycle.submit(
            [&](Attempt attempt) -> Result {
                attempt.complete(HAL_ERR_RESULT);
                if (should_throw)
                    throw std::runtime_error("post-callback vendor failure");
                return Result::rejected(HAL_ERR_RESULT);
            },
            [&](int status) {
                CHECK(status == HAL_ERR_RESULT);
                callbacks.fetch_add(1, std::memory_order_relaxed);
            });

        CHECK(rc == HAL_OK);
        CHECK(callbacks.load() == 1);
        CHECK(lifecycle.pending_count() == 0);
    }
}

void duplicate_completion_is_exactly_once() {
    Hailo15AsyncProviderLifecycle lifecycle;
    std::atomic<int> callbacks{0};
    std::mutex gate_mu;
    std::condition_variable gate_cv;
    int waiting = 0;
    bool release = false;

    const int rc = lifecycle.submit(
        [&](Attempt attempt) {
            auto invoke = [&, attempt] {
                {
                    std::unique_lock lock(gate_mu);
                    ++waiting;
                    gate_cv.notify_all();
                    gate_cv.wait(lock, [&] { return release; });
                }
                attempt.complete(HAL_OK);
            };
            std::thread first(invoke);
            std::thread second(invoke);
            {
                std::unique_lock lock(gate_mu);
                gate_cv.wait(lock, [&] { return waiting == 2; });
                release = true;
                gate_cv.notify_all();
            }
            first.join();
            second.join();
            return Result::accepted();
        },
        [&](int status) {
            CHECK(status == HAL_OK);
            callbacks.fetch_add(1, std::memory_order_relaxed);
        });

    CHECK(rc == HAL_OK);
    CHECK(callbacks.load() == 1);
    CHECK(lifecycle.pending_count() == 0);
}

void close_waits_until_application_callback_returns() {
    Hailo15AsyncProviderLifecycle lifecycle;
    std::mutex callback_mu;
    std::condition_variable callback_cv;
    bool callback_entered = false;
    bool release_callback = false;
    bool callback_exited = false;
    Attempt saved_attempt;

    const int rc = lifecycle.submit(
        [&](Attempt attempt) {
            saved_attempt = attempt;
            attempt.mark_vendor_accepted();
            return Result::accepted();
        },
        [&](int status) {
            CHECK(status == HAL_OK);
            std::unique_lock lock(callback_mu);
            callback_entered = true;
            callback_cv.notify_all();
            callback_cv.wait(lock, [&] { return release_callback; });
            callback_exited = true;
            callback_cv.notify_all();
        });
    CHECK(rc == HAL_OK);
    CHECK(lifecycle.pending_count() == 1);

    std::thread completion([&] { saved_attempt.complete(HAL_OK); });
    {
        std::unique_lock lock(callback_mu);
        CHECK(callback_cv.wait_for(lock, 1s, [&] { return callback_entered; }));
    }

    auto close_future = std::async(std::launch::async, [&] {
        return lifecycle.close_and_wait(2s);
    });
    CHECK(close_future.wait_for(100ms) == std::future_status::timeout);

    {
        std::lock_guard lock(callback_mu);
        release_callback = true;
        callback_cv.notify_all();
    }
    completion.join();
    CHECK(close_future.get());
    CHECK(callback_exited);
    CHECK(lifecycle.pending_count() == 0);

    bool vendor_called = false;
    const int rejected = lifecycle.submit(
        [&](Attempt) {
            vendor_called = true;
            return Result::accepted();
        },
        [](int) {});
    CHECK(rejected == HAL_ERR_INVALID_STATE);
    CHECK(!vendor_called);
}

void rejection_wins_before_late_completion() {
    Hailo15AsyncProviderLifecycle lifecycle;
    Attempt saved_attempt;
    std::atomic<int> callbacks{0};

    const int rc = lifecycle.submit(
        [&](Attempt attempt) {
            saved_attempt = attempt;
            return Result::rejected(HAL_ERR_RESULT);
        },
        [&](int) { callbacks.fetch_add(1, std::memory_order_relaxed); });

    CHECK(rc == HAL_ERR_RESULT);
    CHECK(!saved_attempt.complete(HAL_OK));
    CHECK(callbacks.load() == 0);
    CHECK(lifecycle.pending_count() == 0);
}

void rejection_race_is_atomic_and_quiescent() {
    for (int iteration = 0; iteration < 200; ++iteration) {
        Hailo15AsyncProviderLifecycle lifecycle;
        std::atomic<bool> release{false};
        std::atomic<int> callbacks{0};
        std::thread completion;

        const int rc = lifecycle.submit(
            [&](Attempt attempt) {
                completion = std::thread([&, attempt] {
                    while (!release.load(std::memory_order_acquire))
                        std::this_thread::yield();
                    attempt.complete(HAL_OK);
                });
                release.store(true, std::memory_order_release);
                return Result::rejected(HAL_ERR_RESULT);
            },
            [&](int status) {
                CHECK(status == HAL_OK);
                callbacks.fetch_add(1, std::memory_order_relaxed);
            });
        completion.join();

        const int callback_count = callbacks.load();
        CHECK((rc == HAL_OK && callback_count == 1) ||
              (rc == HAL_ERR_RESULT && callback_count == 0));
        CHECK(lifecycle.pending_count() == 0);
    }
}

void inline_callback_can_reenter_submit() {
    Hailo15AsyncProviderLifecycle lifecycle;
    std::atomic<int> callbacks{0};
    int nested_rc = HAL_ERR_RESULT;

    const int outer_rc = lifecycle.submit(
        [](Attempt attempt) {
            attempt.complete(HAL_OK);
            return Result::accepted();
        },
        [&](int) {
            callbacks.fetch_add(1, std::memory_order_relaxed);
            nested_rc = lifecycle.submit(
                [](Attempt attempt) {
                    attempt.complete(HAL_OK);
                    return Result::accepted();
                },
                [&](int) {
                    callbacks.fetch_add(1, std::memory_order_relaxed);
                });
        });

    CHECK(outer_rc == HAL_OK);
    CHECK(nested_rc == HAL_OK);
    CHECK(callbacks.load() == 2);
    CHECK(lifecycle.pending_count() == 0);
}

void callback_initiated_close_is_rejected_without_deadlock() {
    Hailo15AsyncProviderLifecycle lifecycle;
    bool close_result = true;

    const int rc = lifecycle.submit(
        [](Attempt attempt) {
            attempt.complete(HAL_OK);
            return Result::accepted();
        },
        [&](int) { close_result = lifecycle.close_and_wait(100ms); });

    CHECK(rc == HAL_OK);
    CHECK(!close_result);
    CHECK(lifecycle.pending_count() == 0);
    CHECK(lifecycle.close_and_wait(100ms));
}

void callback_exception_is_contained_and_retired() {
    Hailo15AsyncProviderLifecycle lifecycle;

    const int rc = lifecycle.submit(
        [](Attempt attempt) {
            attempt.complete(HAL_OK);
            return Result::accepted();
        },
        [](int) { throw std::runtime_error("expected callback failure"); });

    CHECK(rc == HAL_OK);
    CHECK(lifecycle.pending_count() == 0);
    CHECK(lifecycle.close_and_wait(100ms));
}

void nested_callback_close_of_outer_lifecycle_is_rejected() {
    Hailo15AsyncProviderLifecycle outer;
    Hailo15AsyncProviderLifecycle inner;
    bool close_result = true;
    std::chrono::steady_clock::duration close_elapsed = 1s;
    int inner_rc = HAL_ERR_RESULT;

    const int outer_rc = outer.submit(
        [](Attempt attempt) {
            attempt.complete(HAL_OK);
            return Result::accepted();
        },
        [&](int) {
            inner_rc = inner.submit(
                [](Attempt attempt) {
                    attempt.complete(HAL_OK);
                    return Result::accepted();
                },
                [&](int) {
                    const auto started = std::chrono::steady_clock::now();
                    close_result = outer.close_and_wait(500ms);
                    close_elapsed = std::chrono::steady_clock::now() - started;
                });
        });

    CHECK(outer_rc == HAL_OK);
    CHECK(inner_rc == HAL_OK);
    CHECK(!close_result);
    CHECK(close_elapsed < 100ms);
    CHECK(outer.pending_count() == 0);
    CHECK(inner.pending_count() == 0);
    CHECK(outer.close_and_wait(100ms));
    CHECK(inner.close_and_wait(100ms));
}

void rejected_submission_is_callback_quiescent() {
    Hailo15AsyncProviderLifecycle lifecycle;
    std::atomic<int> callbacks{0};

    const int rc = lifecycle.submit(
        [](Attempt) { return Result::rejected(HAL_ERR_RESULT); },
        [&](int) { callbacks.fetch_add(1, std::memory_order_relaxed); });

    CHECK(rc == HAL_ERR_RESULT);
    CHECK(callbacks.load() == 0);
    CHECK(lifecycle.pending_count() == 0);
    CHECK(lifecycle.close_and_wait(100ms));
}

}  // namespace

int main() {
    inline_callback_runs_before_submit_returns();
    callback_then_vendor_failure_keeps_callback_ownership();
    duplicate_completion_is_exactly_once();
    close_waits_until_application_callback_returns();
    rejection_wins_before_late_completion();
    rejection_race_is_atomic_and_quiescent();
    inline_callback_can_reenter_submit();
    callback_initiated_close_is_rejected_without_deadlock();
    callback_exception_is_contained_and_retired();
    nested_callback_close_of_outer_lifecycle_is_rejected();
    rejected_submission_is_callback_quiescent();

    if (failures != 0) {
        std::cerr << failures << " lifecycle check(s) failed\n";
        return 1;
    }
    std::cout << "hailo15 async provider lifecycle tests passed\n";
    return 0;
}
