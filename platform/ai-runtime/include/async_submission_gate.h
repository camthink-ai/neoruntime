#pragma once

#include <atomic>
#include <cstdint>

namespace aipc::ai_runtime {

enum class AsyncSubmissionPhase : uint8_t {
    Submitting,
    Submitted,
    CallbackDeferred,
    Completing,
    Cancelled,
};

enum class AsyncCallbackAction : uint8_t {
    DeferToSubmitter,
    CompleteHere,
    Ignore,
};

enum class AsyncReturnAction : uint8_t {
    AwaitCallback,
    CompleteHere,
    Rollback,
};

/// Coordinates an async backend that may invoke its callback before
/// run_async() returns. Inline callbacks publish their result and return
/// without application cleanup; the submitter completes them only after the
/// backend call has returned or unwound.
class AsyncSubmissionGate {
public:
    AsyncCallbackAction on_callback() noexcept {
        auto expected = AsyncSubmissionPhase::Submitting;
        if (phase_.compare_exchange_strong(
                expected, AsyncSubmissionPhase::CallbackDeferred,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return AsyncCallbackAction::DeferToSubmitter;
        }

        expected = AsyncSubmissionPhase::Submitted;
        if (phase_.compare_exchange_strong(
                expected, AsyncSubmissionPhase::Completing,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return AsyncCallbackAction::CompleteHere;
        }
        return AsyncCallbackAction::Ignore;
    }

    AsyncReturnAction on_return(bool accepted) noexcept {
        auto expected = AsyncSubmissionPhase::Submitting;
        const auto target = accepted ? AsyncSubmissionPhase::Submitted
                                     : AsyncSubmissionPhase::Cancelled;
        if (phase_.compare_exchange_strong(
                expected, target, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return accepted ? AsyncReturnAction::AwaitCallback
                            : AsyncReturnAction::Rollback;
        }

        expected = AsyncSubmissionPhase::CallbackDeferred;
        if (phase_.compare_exchange_strong(
                expected, AsyncSubmissionPhase::Completing,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return AsyncReturnAction::CompleteHere;
        }

        // A callback that observed Submitted may already be completing on its
        // own thread. Callback-wins also applies if the backend subsequently
        // reports an error or throws.
        return AsyncReturnAction::AwaitCallback;
    }

private:
    std::atomic<AsyncSubmissionPhase> phase_{
        AsyncSubmissionPhase::Submitting};
};

}  // namespace aipc::ai_runtime
