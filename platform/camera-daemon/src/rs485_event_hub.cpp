/** @file rs485_event_hub.cpp — see rs485_event_hub.h */

#include "rs485_event_hub.h"

#include <algorithm>

namespace aipc {

// ---- Rs485EventHub::Subscription -------------------------------------------

Rs485EventHub::Subscription::~Subscription() {
    if (hub_ && state_) {
        hub_->detach(state_.get());
    }
}

Rs485EventHub::Subscription::Subscription(Subscription&& other) noexcept
    : hub_(other.hub_), state_(std::move(other.state_)) {
    other.hub_ = nullptr;
}

Rs485EventHub::Subscription& Rs485EventHub::Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        if (hub_ && state_) {
            hub_->detach(state_.get());
        }
        hub_ = other.hub_;
        state_ = std::move(other.state_);
        other.hub_ = nullptr;
    }
    return *this;
}

bool Rs485EventHub::Subscription::next(Rs485Frame* out, std::chrono::milliseconds timeout) {
    if (!state_) {
        return false;
    }
    std::unique_lock<std::mutex> lk(state_->mu);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (!state_->queue.empty()) {
            if (out) {
                *out = std::move(state_->queue.front());
            }
            state_->queue.pop_front();
            return true;
        }
        if (state_->closed) {
            return false;
        }
        if (state_->cv.wait_until(lk, deadline) == std::cv_status::timeout) {
            /* Spurious-wakeup guard: re-check once after the timeout. */
            if (!state_->queue.empty()) {
                if (out) {
                    *out = std::move(state_->queue.front());
                }
                state_->queue.pop_front();
                return true;
            }
            return false;
        }
    }
}

uint64_t Rs485EventHub::Subscription::dropped() const {
    if (!state_) {
        return 0;
    }
    std::lock_guard<std::mutex> lk(state_->mu);
    return state_->dropped;
}

bool Rs485EventHub::Subscription::alive() const {
    if (!state_) {
        return false;
    }
    std::lock_guard<std::mutex> lk(state_->mu);
    return !state_->closed;
}

// ---- Rs485EventHub ----------------------------------------------------------

std::shared_ptr<Rs485EventHub::Subscription> Rs485EventHub::subscribe(uint32_t queue_size, bool drop_old) {
    if (queue_size == 0) {
        queue_size = kDefaultQueueSize;
    }
    queue_size = std::min(queue_size, kMaxQueueSize);

    auto state = std::make_shared<detail::Rs485SubState>();
    state->cap = queue_size;
    state->drop_old = drop_old;

    auto sub = std::shared_ptr<Subscription>(new Subscription());
    sub->hub_ = this;
    sub->state_ = state;

    {
        std::lock_guard<std::mutex> lk(mu_);
        subs_.push_back(state);
    }
    return sub;
}

void Rs485EventHub::publish(const uint8_t* data, uint16_t len) {
    if (data == nullptr || len == 0) {
        return;
    }

    std::vector<std::shared_ptr<detail::Rs485SubState>> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        targets.reserve(subs_.size());
        for (auto& s : subs_) {
            targets.push_back(s);
        }
    }

    Rs485Frame frame;
    frame.data.assign(data, data + len);
    frame.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());

    for (auto& st : targets) {
        std::lock_guard<std::mutex> lk(st->mu);
        if (st->closed) {
            continue;
        }
        if (st->queue.size() >= st->cap) {
            if (st->drop_old) {
                st->queue.pop_front();
                st->queue.push_back(frame);
            }
            /* drop_newest: keep the backlog as-is, drop this frame */
            st->dropped++;
        } else {
            st->queue.push_back(frame);
        }
        st->cv.notify_one();
    }
}

void Rs485EventHub::shutdown() {
    std::vector<std::shared_ptr<detail::Rs485SubState>> targets;
    {
        std::lock_guard<std::mutex> lk(mu_);
        targets = std::move(subs_);
        subs_.clear();
    }
    for (auto& st : targets) {
        {
            std::lock_guard<std::mutex> lk(st->mu);
            st->closed = true;
        }
        st->cv.notify_all();
    }
}

size_t Rs485EventHub::subscriber_count() const {
    std::lock_guard<std::mutex> lk(mu_);
    return subs_.size();
}

void Rs485EventHub::detach(detail::Rs485SubState* state) {
    std::lock_guard<std::mutex> lk(mu_);
    subs_.erase(std::remove_if(subs_.begin(), subs_.end(),
                               [state](const std::shared_ptr<detail::Rs485SubState>& s) {
                                   return s.get() == state;
                               }),
                subs_.end());
}

// ---- Rs485HalSubscription ----------------------------------------------------

Rs485HalSubscription::Rs485HalSubscription(SubscribeFn sub, UnsubscribeFn unsub, void* userdata)
    : sub_(sub), unsub_(unsub), ud_(userdata) {}

int Rs485HalSubscription::acquire() {
    std::lock_guard<std::mutex> lk(mu_);
    if (count_ == 0) {
        const int rc = sub_ ? sub_(ud_) : 0;
        if (rc != 0) {
            return rc; /* stay at 0 — nothing held */
        }
    }
    ++count_;
    return 0;
}

int Rs485HalSubscription::release() {
    std::lock_guard<std::mutex> lk(mu_);
    if (count_ == 0) {
        return 0;
    }
    --count_;
    if (count_ == 0) {
        return unsub_ ? unsub_(ud_) : 0;
    }
    return 0;
}

bool Rs485HalSubscription::held() const {
    std::lock_guard<std::mutex> lk(mu_);
    return count_ > 0;
}

} // namespace aipc
