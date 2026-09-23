#pragma once

#include "common/hal_common.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <utility>
#include <vector>

namespace hal_v2::hailo15 {

class Hailo15AsyncProviderLifecycle {
private:
    struct SharedState;
    struct Context;

public:
    struct SubmissionResult {
        static SubmissionResult accepted() noexcept {
            return {true, HAL_OK};
        }

        static SubmissionResult rejected(int error) noexcept {
            return {false, error == HAL_OK ? HAL_ERR_RESULT : error};
        }

        bool is_accepted = false;
        int error = HAL_ERR_RESULT;
    };

    class Attempt {
    public:
        Attempt() = default;

        void mark_vendor_accepted() const noexcept {
            const auto ctx = ctx_;
            if (!ctx)
                return;
            std::lock_guard lock(ctx->mu);
            if (!ctx->rejected && !ctx->retired)
                ctx->vendor_accepted = true;
        }

        bool complete(int status) const noexcept {
            const auto ctx = ctx_;
            if (!ctx)
                return false;

            bool invoke = false;
            {
                std::lock_guard lock(ctx->mu);
                if (ctx->completion_claimed || ctx->rejected || ctx->retired)
                    return false;
                ctx->completion_claimed = true;
                ctx->completion_status = status;
                if (!ctx->submitting) {
                    ctx->callback_running = true;
                    invoke = true;
                }
            }

            if (invoke)
                invoke_and_retire(ctx);
            return true;
        }

    private:
        explicit Attempt(std::shared_ptr<Context> ctx)
            : ctx_(std::move(ctx)) {}

        static void invoke_and_retire(
            const std::shared_ptr<Context>& ctx) noexcept {
            Hailo15AsyncProviderLifecycle::invoke_and_retire(ctx);
        }

        std::shared_ptr<Context> ctx_;
        friend class Hailo15AsyncProviderLifecycle;
    };

    Hailo15AsyncProviderLifecycle()
        : state_(std::make_shared<SharedState>()) {}

    template <typename VendorSubmit, typename Completion>
    int submit(VendorSubmit&& vendor_submit, Completion&& completion) noexcept {
        std::shared_ptr<Context> ctx;
        try {
            ctx = std::make_shared<Context>(
                state_, std::function<void(int)>(
                            std::forward<Completion>(completion)));
        } catch (const std::bad_alloc&) {
            return HAL_ERR_NO_MEM;
        } catch (...) {
            return HAL_ERR_RESULT;
        }

        {
            std::lock_guard lock(state_->mu);
            if (state_->closing)
                return HAL_ERR_INVALID_STATE;
            try {
                state_->pending.push_back(ctx);
            } catch (const std::bad_alloc&) {
                return HAL_ERR_NO_MEM;
            } catch (...) {
                return HAL_ERR_RESULT;
            }
            ++state_->active_submissions;
        }

        SubmissionResult result = SubmissionResult::rejected(HAL_ERR_RESULT);
        try {
            std::unique_lock submit_lock(state_->vendor_submit_mu);
            try {
                result = std::forward<VendorSubmit>(vendor_submit)(Attempt(ctx));
            } catch (const std::bad_alloc&) {
                result = SubmissionResult::rejected(HAL_ERR_NO_MEM);
            } catch (...) {
                result = SubmissionResult::rejected(HAL_ERR_RESULT);
            }
        } catch (...) {
            result = SubmissionResult::rejected(HAL_ERR_MUTEX);
        }

        finish_active_submission(state_);

        bool invoke = false;
        bool retire_rejected = false;
        bool callback_owned = false;
        {
            std::lock_guard lock(ctx->mu);
            ctx->submitting = false;
            callback_owned = result.is_accepted || ctx->vendor_accepted ||
                             ctx->completion_claimed;
            if (ctx->completion_claimed && !ctx->callback_running) {
                ctx->callback_running = true;
                invoke = true;
            } else if (!callback_owned) {
                ctx->rejected = true;
                retire_rejected = true;
            }
        }

        if (invoke)
            invoke_and_retire(ctx);
        else if (retire_rejected)
            retire_context(ctx);

        return callback_owned ? HAL_OK : result.error;
    }

    template <typename Function>
    decltype(auto) serialize_vendor_submission(Function&& function) {
        std::lock_guard lock(state_->vendor_submit_mu);
        return std::forward<Function>(function)();
    }

    template <typename Rep, typename Period>
    bool close_and_wait(
        const std::chrono::duration<Rep, Period>& timeout) noexcept {
        if (is_callback_active(state_.get()))
            return false;
        try {
            std::unique_lock lock(state_->mu);
            state_->closing = true;
            return state_->cv.wait_for(lock, timeout, [&] {
                return state_->active_submissions == 0 &&
                       state_->pending.empty();
            });
        } catch (...) {
            return false;
        }
    }

    bool close_and_wait() noexcept {
        if (is_callback_active(state_.get()))
            return false;
        try {
            std::unique_lock lock(state_->mu);
            state_->closing = true;
            state_->cv.wait(lock, [&] {
                return state_->active_submissions == 0 &&
                       state_->pending.empty();
            });
            return true;
        } catch (...) {
            return false;
        }
    }

    size_t pending_count() const noexcept {
        std::lock_guard lock(state_->mu);
        return state_->pending.size();
    }

private:
    struct SharedState {
        std::mutex mu;
        std::condition_variable cv;
        std::mutex vendor_submit_mu;
        bool closing = false;
        size_t active_submissions = 0;
        std::vector<std::shared_ptr<Context>> pending;
    };

    struct CallbackFrame {
        const SharedState* state = nullptr;
        const CallbackFrame* previous = nullptr;
    };

    struct Context {
        Context(std::shared_ptr<SharedState> state_in,
                std::function<void(int)> completion_in)
            : state(std::move(state_in)),
              completion(std::move(completion_in)) {}

        std::shared_ptr<SharedState> state;
        std::mutex mu;
        std::function<void(int)> completion;
        bool submitting = true;
        bool vendor_accepted = false;
        bool completion_claimed = false;
        bool callback_running = false;
        bool rejected = false;
        bool retired = false;
        int completion_status = HAL_ERR_RESULT;
    };

    static void finish_active_submission(
        const std::shared_ptr<SharedState>& state) noexcept {
        std::lock_guard lock(state->mu);
        if (state->active_submissions > 0)
            --state->active_submissions;
        state->cv.notify_all();
    }

    static void retire_context(
        const std::shared_ptr<Context>& ctx) noexcept {
        {
            std::lock_guard lock(ctx->mu);
            if (ctx->retired)
                return;
            ctx->retired = true;
            ctx->callback_running = false;
        }

        const auto state = ctx->state;
        std::lock_guard lock(state->mu);
        auto& pending = state->pending;
        pending.erase(std::remove(pending.begin(), pending.end(), ctx),
                      pending.end());
        state->cv.notify_all();
    }

    static bool is_callback_active(const SharedState* state) noexcept {
        for (auto* frame = current_callback_frame_; frame;
             frame = frame->previous) {
            if (frame->state == state)
                return true;
        }
        return false;
    }

    static void invoke_and_retire(
        const std::shared_ptr<Context>& ctx) noexcept {
        const auto state = ctx->state;
        const CallbackFrame frame{state.get(), current_callback_frame_};
        current_callback_frame_ = &frame;
        try {
            ctx->completion(ctx->completion_status);
        } catch (...) {
        }
        current_callback_frame_ = frame.previous;
        retire_context(ctx);
    }

    std::shared_ptr<SharedState> state_;
    inline static thread_local const CallbackFrame* current_callback_frame_ =
        nullptr;
};

}  // namespace hal_v2::hailo15
