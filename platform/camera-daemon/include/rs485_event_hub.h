#pragma once

/**
 * @file rs485_event_hub.h
 * @brief Fan-out hub for RS-485 RX frames (transport-agnostic, no proto types).
 *
 * publish() is non-blocking and safe from the HAL mcu event thread: each
 * subscriber owns a bounded deque with an explicit drop policy, so a slow
 * consumer never stalls the producer or other subscribers. Also provides a
 * refcounted guard around the HAL subscribe/unsubscribe pair: the hailo15
 * HAL *appends* a callback per subscribe() call and its unsubscribe() removes
 * ALL callbacks for the cmd, so the daemon must hold exactly one HAL
 * subscription while any stream is active — transitions (including the HAL
 * call itself) happen under the guard's mutex.
 */

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <vector>

namespace aipc {

struct Rs485Frame {
    std::vector<uint8_t> data;
    uint64_t timestamp_ns = 0;
};

namespace detail {

struct Rs485SubState {
    std::mutex mu;
    std::condition_variable cv;
    std::deque<Rs485Frame> queue;
    uint32_t cap = 16;
    bool drop_old = true;
    uint64_t dropped = 0;
    bool closed = false;
};

} // namespace detail

class Rs485EventHub {
public:
    static constexpr uint32_t kDefaultQueueSize = 16;
    static constexpr uint32_t kMaxQueueSize = 128;

    class Subscription {
    public:
        ~Subscription();                       // detaches from the hub
        Subscription(Subscription&&) noexcept;
        Subscription& operator=(Subscription&&) noexcept;
        Subscription(const Subscription&) = delete;
        Subscription& operator=(const Subscription&) = delete;

        /** Blocks up to @p timeout. true = frame delivered in *out;
         *  false = timeout, hub shut down, or subscription detached. */
        bool next(Rs485Frame* out, std::chrono::milliseconds timeout);

        /** Frames dropped for this subscriber (queue full, per policy). */
        uint64_t dropped() const;

        /** false once the hub is shut down or the subscription detached. */
        bool alive() const;

    private:
        friend class Rs485EventHub;
        Subscription() = default;
        Rs485EventHub* hub_ = nullptr;
        std::shared_ptr<detail::Rs485SubState> state_;
    };

    /* Lifetime: subscriptions must not outlive the hub — ~Subscription()
     * detaches through the hub pointer. In the daemon the hub is a member of
     * the gRPC service object, and the server joins every handler (so
     * destroying every subscription) before that object goes away. */
    Rs485EventHub() = default;
    Rs485EventHub(const Rs485EventHub&) = delete;
    Rs485EventHub& operator=(const Rs485EventHub&) = delete;

    /** queue_size is clamped to [1, kMaxQueueSize]; 0 = kDefaultQueueSize. */
    std::shared_ptr<Subscription> subscribe(uint32_t queue_size, bool drop_old);

    /** Non-blocking fan-out; timestamps with the host clock. */
    void publish(const uint8_t* data, uint16_t len);

    /** Closes every subscription and wakes all waiters. Idempotent, and
     *  final: subscriptions created afterwards start out closed. */
    void shutdown();

    size_t subscriber_count() const;

private:
    void detach(detail::Rs485SubState* state);

    mutable std::mutex mu_;
    std::vector<std::shared_ptr<detail::Rs485SubState>> subs_;
    bool shutdown_ = false;
};

/**
 * Refcounted acquire/release around a HAL subscribe/unsubscribe function
 * pair. The HAL calls run inside the critical section so exactly one HAL
 * subscription exists while >=1 holders are active.
 */
class Rs485HalSubscription {
public:
    using SubscribeFn = int (*)(void* userdata);
    using UnsubscribeFn = int (*)(void* userdata);

    Rs485HalSubscription(SubscribeFn sub, UnsubscribeFn unsub, void* userdata);

    /** +1; performs the HAL subscribe on 0->1; on failure rolls the count
     *  back and returns the HAL error code. */
    int acquire();

    /** -1; performs the HAL unsubscribe on ->0. */
    int release();

    bool held() const;

private:
    mutable std::mutex mu_;
    SubscribeFn sub_;
    UnsubscribeFn unsub_;
    void* ud_;
    uint32_t count_ = 0;
};

} // namespace aipc
