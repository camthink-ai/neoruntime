/** @file rs485_event_hub_test.cpp — host unit tests for the RS-485 RX hub.
 *
 * Covers: fan-out, drop-old/drop-newest policies + counters, capacity
 * clamping, shutdown wakeups, destructor detach, the refcounted HAL
 * subscription guard, and a stub-HAL injection round trip (the same path
 * camera-daemon uses: HAL callback -> hub.publish -> Subscription::next).
 */

#include "rs485_event_hub.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

#include "peripheral/devices/hal_rs485.h"

/* stub HAL (hal_v2/platforms/stub) — real ops table + test injection. */
extern "C" {
extern HalRs485Ops HAL_RS485_OPS;
int stub_rs485_inject_rx(const uint8_t* data, uint16_t len);
}

using aipc::Rs485EventHub;
using aipc::Rs485Frame;
using aipc::Rs485HalSubscription;

static void publish_bytes(Rs485EventHub& hub, const char* s) {
    hub.publish(reinterpret_cast<const uint8_t*>(s),
                static_cast<uint16_t>(std::char_traits<char>::length(s)));
}

static void test_fanout_two_subscribers() {
    Rs485EventHub hub;
    auto a = hub.subscribe(4, true);
    auto b = hub.subscribe(4, true);
    assert(hub.subscriber_count() == 2);

    publish_bytes(hub, "abc");
    Rs485Frame f;
    assert(a->next(&f, std::chrono::milliseconds(100)));
    assert(f.data.size() == 3 && f.data[0] == 'a' && f.timestamp_ns > 0);
    assert(b->next(&f, std::chrono::milliseconds(100)));
    assert(f.data.size() == 3 && f.data[2] == 'c');

    /* Timeout returns false but subscription stays alive. */
    assert(!a->next(&f, std::chrono::milliseconds(50)));
    assert(a->alive());
    std::printf("  fanout: ok\n");
}

static void test_drop_policy() {
    /* drop_old=true keeps the newest frames. */
    {
        Rs485EventHub hub;
        auto s = hub.subscribe(2, true);
        publish_bytes(hub, "1");
        publish_bytes(hub, "2");
        publish_bytes(hub, "3"); /* overflows: "1" dropped */
        Rs485Frame f;
        assert(s->next(&f, std::chrono::milliseconds(100)) && f.data[0] == '2');
        assert(s->next(&f, std::chrono::milliseconds(100)) && f.data[0] == '3');
        assert(s->dropped() == 1);
    }
    /* drop_old=false keeps the backlog as-is (drops the newest). */
    {
        Rs485EventHub hub;
        auto s = hub.subscribe(2, false);
        publish_bytes(hub, "1");
        publish_bytes(hub, "2");
        publish_bytes(hub, "3"); /* dropped entirely */
        publish_bytes(hub, "4"); /* dropped entirely */
        Rs485Frame f;
        assert(s->next(&f, std::chrono::milliseconds(100)) && f.data[0] == '1');
        assert(s->next(&f, std::chrono::milliseconds(100)) && f.data[0] == '2');
        assert(!s->next(&f, std::chrono::milliseconds(50)));
        assert(s->dropped() == 2);
    }
    std::printf("  drop policy: ok\n");
}

static void test_capacity_clamp() {
    Rs485EventHub hub;
    auto s0 = hub.subscribe(0, true);        /* 0 -> kDefaultQueueSize (16) */
    auto sbig = hub.subscribe(99999, true);  /* -> clamped to kMaxQueueSize (128) */
    /* Fill far beyond any plausible internal default: the clamp must bound
     * each backlog with drops counted. */
    for (int i = 0; i < 300; i++) {
        char c = 'a' + (i % 26);
        publish_bytes(hub, &c);
    }
    Rs485Frame f;
    int drained = 0;
    while (s0->next(&f, std::chrono::milliseconds(20))) {
        drained++;
    }
    assert(drained == 16);       /* kDefaultQueueSize */
    assert(s0->dropped() == 284);
    drained = 0;
    while (sbig->next(&f, std::chrono::milliseconds(20))) {
        drained++;
    }
    assert(drained == 128);      /* kMaxQueueSize */
    assert(sbig->dropped() == 172);
    std::printf("  capacity clamp: ok (16-drop=%llu, 128-drop=%llu)\n",
                static_cast<unsigned long long>(s0->dropped()),
                static_cast<unsigned long long>(sbig->dropped()));
}

static void test_shutdown_wakes_blocked_next() {
    Rs485EventHub hub;
    auto s = hub.subscribe(4, true);
    std::thread closer([&hub] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        hub.shutdown();
    });
    Rs485Frame f;
    const auto t0 = std::chrono::steady_clock::now();
    const bool got = s->next(&f, std::chrono::milliseconds(5000));
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);
    closer.join();
    assert(!got);
    assert(!s->alive());
    assert(waited.count() < 2000); /* woken by shutdown, not by the timeout */
    std::printf("  shutdown wake: ok (waited %lldms)\n",
                static_cast<long long>(waited.count()));
}

static void test_destructor_detach() {
    Rs485EventHub hub;
    {
        auto s = hub.subscribe(4, true);
        assert(hub.subscriber_count() == 1);
    } /* Subscription dtor detaches */
    assert(hub.subscriber_count() == 0);
    publish_bytes(hub, "x"); /* no subscribers: must not crash/block */
    std::printf("  dtor detach: ok\n");
}

static void test_hal_subscription_refcount() {
    static int sub_calls;
    static int unsub_calls;
    static int sub_rc;
    sub_calls = unsub_calls = 0;
    sub_rc = 0;

    Rs485HalSubscription guard(
        [](void*) -> int { sub_calls++; return sub_rc; },
        [](void*) -> int { unsub_calls++; return 0; },
        nullptr);

    assert(!guard.held());
    assert(guard.acquire() == 0 && sub_calls == 1 && guard.held());
    assert(guard.acquire() == 0 && sub_calls == 1); /* refcount: no second HAL call */
    assert(guard.release() == 0 && unsub_calls == 0);
    assert(guard.held());
    assert(guard.release() == 0 && unsub_calls == 1);
    assert(!guard.held());
    assert(guard.release() == 0 && unsub_calls == 1); /* under-release is a no-op */

    /* Failure path: subscribe error leaves the count at 0 (retryable). */
    sub_rc = -7;
    assert(guard.acquire() == -7 && !guard.held());
    sub_rc = 0;
    assert(guard.acquire() == 0 && sub_calls == 3 && guard.held());
    assert(guard.release() == 0 && unsub_calls == 2);
    std::printf("  hal refcount: ok\n");
}

/* End-to-end parity with the daemon wiring: Rs485HalSubscription around the
 * real stub HAL ops table (subscribe -> cb -> hub.publish), frames injected
 * through the stub exactly like the mcu event thread would deliver them. */
static void test_stub_hal_injection_roundtrip() {
    static Rs485EventHub* hub_ptr;
    hub_ptr = nullptr;

    Rs485EventHub hub;
    hub_ptr = &hub;

    Rs485HalSubscription guard(
        [](void*) -> int {
            return HAL_RS485_OPS.subscribe(
                nullptr,
                [](void* ctx, const uint8_t* data, uint16_t len, void* ud) {
                    (void)ctx;
                    (void)ud;
                    hub_ptr->publish(data, len);
                },
                nullptr);
        },
        [](void*) -> int { return HAL_RS485_OPS.unsubscribe(nullptr); },
        nullptr);

    assert(guard.acquire() == 0);
    auto s = hub.subscribe(8, true);
    assert(stub_rs485_inject_rx(reinterpret_cast<const uint8_t*>("\x01\x02\x03"), 3) == 0);
    Rs485Frame f;
    assert(s->next(&f, std::chrono::milliseconds(200)));
    assert(f.data.size() == 3 && f.data[0] == 0x01 && f.data[2] == 0x03);
    /* Injection without a subscription registered is rejected. */
    assert(guard.release() == 0);
    assert(stub_rs485_inject_rx(reinterpret_cast<const uint8_t*>("x"), 1) != 0);
    std::printf("  stub injection roundtrip: ok\n");
}

int main() {
    test_fanout_two_subscribers();
    test_drop_policy();
    test_capacity_clamp();
    test_shutdown_wakes_blocked_next();
    test_destructor_detach();
    test_hal_subscription_refcount();
    test_stub_hal_injection_roundtrip();
    std::printf("rs485-event-hub-test: all passed\n");
    return 0;
}
