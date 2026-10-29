// Regression test for the 10-29 field deadlock: a HAL force_idr blocked
// inside medialib (gst_pad_send_event on a stalled encode pipeline) while
// callers held EncoderManager::mu_ and StreamState::clients_mu, wedging
// every status/probe/reconfigure path daemon-wide.
//
// Contract under test:
//   1. EncodedPublisher never invokes the keyframe callback inline on the
//      CTRL/probe path — a blocked callback must not block stats/probe.
//   2. EncoderManager::force_keyframe does not hold mu_ across the HAL
//      call — has_encoder() stays responsive while force_idr is stuck.
//   3. destroy() erases the slot and drains an in-flight call in ONE
//      critical section: while a stuck call occupies the ctx, destroy is
//      blocked, no new caller can capture the ctx, and once the call
//      returns the teardown completes.
//   4. When the drain times out, destroy() returns false and aborts the
//      HAL teardown — no stop/unsubscribe/deinit on a ctx the stuck call
//      still occupies (use-after-free). The slot is parked as a closing
//      tombstone; a retry after the call returns reclaims the ctx.
//   5. Callbacks arriving for a parked (closing) ctx are dropped but their
//      packet buffers are RELEASED — every packet owns a shared buffer
//      reference; leaking them starves the pool (black screen).
//   6. Creating a same-name encoder while a teardown is parked is rejected
//      (it would overwrite and permanently leak the old ctx); after the
//      tombstone is reclaimed, creation works again.
//   7. A parked/claimed teardown is owned by exactly ONE destroy() call:
//      a concurrent destroy() of the same name refuses (false) instead of
//      running stop/unsubscribe/deinit a second time on the same ctx.
//   8. Two concurrent same-name creates produce exactly one registration:
//      the loser fails at name reservation BEFORE touching the HAL, so no
//      subscription or ctx is leaked by the overwrite.
//   9. destroy() of a name whose creation is mid-HAL (reservation held,
//      init/subscribe lock-free) waits bounded for the creator to publish,
//      then tears the published encoder down — never "nothing registered".
//  10. destroy_all() rejects creations arriving inside its window and
//      waits out in-flight ones before reporting all-clear.
//  11. A RUNNING FROM_MEDIA encoder delivering inside subscribe() — before
//      the promotion critical section writes the routing maps — hits the
//      release-only path: the packet is dropped but RELEASED, not leaked.

#include "encoded_publisher.h"
#include "encoder_manager.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

void test_publisher_ctrl_does_not_block_on_callback() {
    char dir_template[] = "/tmp/encoded-publisher-async-kf.XXXXXX";
    char* dir = ::mkdtemp(dir_template);
    assert(dir != nullptr);

    std::promise<void> release_cb;
    std::future<void> cb_gate = release_cb.get_future();
    std::atomic<int> calls{0};

    EncodedPublisher publisher;
    EncodedPublisher::StreamConfig config;
    config.name = "main";
    config.codec = "h264";
    config.width = 1920;
    config.height = 1080;
    publisher.add_stream(config, dir);
    publisher.set_keyframe_request_cb([&](const std::string& name) {
        assert(name == "main");
        ++calls;
        cb_gate.wait();  // simulate force_idr stuck inside medialib
    });
    assert(publisher.start());

    const std::string socket_path = std::string(dir) + "/main.sock";
    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    assert(client >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    assert(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    // Bounded poll for the accept loop to install the client.
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        EncodedPublisher::StreamDropStats stats{};
        bool installed = false;
        while (!installed && std::chrono::steady_clock::now() < deadline) {
            installed = publisher.get_stream_stats("main", &stats) && stats.clients == 1;
            if (!installed) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        assert(installed);
    }

    // Client asks for a keyframe; the callback will hang on cb_gate.
    const uint8_t ctrl = 0x4B;
    assert(::send(client, &ctrl, 1, 0) == 1);

    // The worker must pick it up (bounded)...
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(calls.load() > 0);  // callback now blocked inside the worker
    }

    // ...and the locked paths must STILL answer while the callback hangs.
    // Old code: CTRL ran the callback inline under clients_mu → this call
    // blocked until the (never-returning) HAL call released it.
    const auto t0 = std::chrono::steady_clock::now();
    EncodedPublisher::StreamDropStats stats{};
    assert(publisher.get_stream_stats("main", &stats));
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    assert(elapsed < 500);

    release_cb.set_value();  // unstick the worker so stop() can join it
    ::close(client);
    publisher.stop();
    ::unlink(socket_path.c_str());
    ::rmdir(dir);
}

std::atomic<bool> g_idr_release{false};
std::atomic<int> g_idr_in_calls{0};

int mock_force_idr(void*) {
    ++g_idr_in_calls;
    while (!g_idr_release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}

void test_manager_does_not_hold_mu_across_force_idr() {
    HalCodecOps ops{};
    ops.force_idr = mock_force_idr;
    EncoderManager mgr(&ops);
    assert(mgr.create_from_context("main", reinterpret_cast<void*>(0x1234)));

    // --- Part 2: mu_ free while force_idr is stuck inside the HAL ---
    g_idr_release.store(false);
    std::thread caller([&] { mgr.force_keyframe("main"); });
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_idr_in_calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(g_idr_in_calls.load() > 0);  // mock is inside the "HAL"
    }

    // Old code held mu_ across the call: the first has_encoder() here would
    // block until the mock was released. It must answer promptly now.
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 100; ++i) {
        assert(mgr.has_encoder("main"));
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
    assert(elapsed < 500);

    g_idr_release.store(true);
    caller.join();

    // --- Part 3: destroy() races a stuck in-flight call, atomically ---
    // The drain-then-erase order let a new call capture the ctx between the
    // wait and the erase; the erase now happens first (inside mu_), so the
    // slot is gone the moment destroy enters its critical section.
    g_idr_release.store(false);
    g_idr_in_calls.store(0);
    std::thread caller2([&] { mgr.force_keyframe("main"); });
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_idr_in_calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(g_idr_in_calls.load() > 0);  // call is inside the "HAL"
    }
    std::atomic<bool> destroy_done{false};
    std::atomic<bool> destroy_ok{false};
    std::thread destroyer([&] {
        destroy_ok.store(mgr.destroy("main"));
        destroy_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    // destroy() must still be waiting in its bounded drain: the stuck call
    // still occupies the ctx, and freeing it under the call is a UAF.
    assert(!destroy_done.load());
    // ...but the slot is already erased — a NEW caller cannot capture the
    // ctx anymore (this is the atomicity the old order missed).
    assert(!mgr.has_encoder("main"));
    assert(mgr.force_keyframe("main") == -1);

    g_idr_release.store(true);
    caller2.join();
    destroyer.join();
    assert(destroy_done.load());
    assert(destroy_ok.load());  // drained in time → teardown ran, rc surfaced
    assert(!mgr.has_encoder("main"));
}

std::atomic<int> g_unsubscribes{0};

int mock_unsubscribe(void*, HalCodecFrameCallback) {
    ++g_unsubscribes;
    return 0;
}

std::atomic<int> g_subscribes{0};

int mock_subscribe(void*, HalCodecFrameCallback, void*) {
    ++g_subscribes;
    return 0;
}

std::atomic<bool> g_subscribe_release{false};
std::atomic<int> g_subscribe_in{0};
std::atomic<bool> g_subscribe_deliver{false};

int mock_subscribe_gated(void* codec_ctx, HalCodecFrameCallback cb, void* ud) {
    ++g_subscribe_in;
    while (!g_subscribe_release.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (g_subscribe_deliver.load() && cb) {
        // Simulate a running FROM_MEDIA encoder delivering the moment the
        // subscription installs — BEFORE create_from_context() writes the
        // routing maps. This is the pending-window packet.
        HalPacketBuffer pkt{};
        pkt.size = 4;
        cb(codec_ctx, &pkt, ud);
    }
    ++g_subscribes;
    return 0;
}

std::atomic<int> g_releases{0};

int mock_release_packet(void*, HalPacketBuffer*) {
    ++g_releases;
    return 0;
}

void test_destroy_aborts_hal_teardown_on_drain_timeout() {
    // Contract 4: on drain timeout destroy() returns WITHOUT running the
    // HAL teardown (stop/unsubscribe/deinit would race the stuck call).
    HalCodecOps ops{};
    ops.force_idr = mock_force_idr;
    ops.unsubscribe = mock_unsubscribe;  // FROM_MEDIA destroy path would call this
    EncoderManager mgr(&ops);
    assert(mgr.create_from_context("aux", reinterpret_cast<void*>(0x5678)));

    g_unsubscribes.store(0);
    g_idr_in_calls.store(0);
    g_idr_release.store(false);
    std::thread caller([&] { mgr.force_keyframe("aux"); });
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_idr_in_calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(g_idr_in_calls.load() > 0);
    }

    // Blocks ~2s in the drain, then aborts: no unsubscribe, slot parked.
    const bool destroyed = mgr.destroy("aux");
    assert(!destroyed);                  // timeout surfaced to the caller
    assert(g_unsubscribes.load() == 0);  // teardown aborted, not raced
    assert(!mgr.has_encoder("aux"));

    g_idr_release.store(true);
    caller.join();

    // Tombstone retry: the wedged call has returned by now, so a later
    // destroy() of the same name reclaims the parked ctx and runs the
    // deferred teardown instead of leaving a permanent leak.
    const bool reclaimed = mgr.destroy("aux");
    assert(reclaimed);
    assert(g_unsubscribes.load() == 1);  // deferred unsubscribe finally ran
    assert(!mgr.has_encoder("aux"));
}

void test_closing_packets_released_and_create_gated() {
    HalCodecOps ops{};
    ops.force_idr = mock_force_idr;
    ops.unsubscribe = mock_unsubscribe;
    ops.release_packet = mock_release_packet;
    EncoderManager mgr(&ops);
    void* const aux_ctx = reinterpret_cast<void*>(0x5678);
    assert(mgr.create_from_context("aux", aux_ctx));

    g_unsubscribes.store(0);
    g_releases.store(0);
    g_idr_in_calls.store(0);
    g_idr_release.store(false);
    std::thread caller([&] { mgr.force_keyframe("aux"); });
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_idr_in_calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(g_idr_in_calls.load() > 0);
    }

    // Timed-out destroy parks the ctx; the subscription stays live.
    assert(!mgr.destroy("aux"));
    assert(g_unsubscribes.load() == 0);

    // Contract 5: packets for the parked ctx are dropped but RELEASED —
    // the buffer reference must go back or the pool starves.
    HalPacketBuffer pkt{};
    pkt.size = 4;
    mgr.on_packet(aux_ctx, &pkt);
    assert(g_releases.load() == 1);
    // A ctx that was never registered keeps the old plain-drop behavior.
    mgr.on_packet(reinterpret_cast<void*>(0x9999), &pkt);
    assert(g_releases.load() == 1);

    // Contract 6: same-name creation while the teardown is parked must be
    // rejected — accepting it would overwrite (and leak) the parked ctx.
    assert(!mgr.create_from_context("aux", reinterpret_cast<void*>(0xAAAA)));
    assert(!mgr.has_encoder("aux"));

    g_idr_release.store(true);
    caller.join();

    // After the wedged call returns: tombstone retry reclaims the ctx...
    assert(mgr.destroy("aux"));
    assert(g_unsubscribes.load() == 1);
    // ...and creation under the same name is allowed again.
    assert(mgr.create_from_context("aux", reinterpret_cast<void*>(0xBBBB)));
    assert(mgr.has_encoder("aux"));
}

void test_concurrent_destroy_claims_tombstone_once() {
    // Contract 7: while one destroy() owns a parked teardown, a concurrent
    // destroy() of the same name must refuse — double stop/unsubscribe/
    // deinit on one ctx is a double-free for standalone encoders.
    HalCodecOps ops{};
    ops.force_idr = mock_force_idr;
    ops.unsubscribe = mock_unsubscribe;
    EncoderManager mgr(&ops);
    assert(mgr.create_from_context("aux", reinterpret_cast<void*>(0xC0DE)));

    g_unsubscribes.store(0);
    g_idr_in_calls.store(0);
    g_idr_release.store(false);
    std::thread caller([&] { mgr.force_keyframe("aux"); });
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_idr_in_calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(g_idr_in_calls.load() > 0);
    }

    // First destroy parks the slot (claimed) and blocks in the bounded drain.
    std::atomic<bool> destroy_done{false};
    std::atomic<bool> destroy_ok{false};
    std::thread destroyer([&] {
        destroy_ok.store(mgr.destroy("aux"));
        destroy_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(!destroy_done.load());  // parked and draining, teardown owned

    // The concurrent destroy must refuse promptly (claim observed), NOT
    // spend its own 2s drain or run a second teardown after a timeout.
    const auto t0 = std::chrono::steady_clock::now();
    const bool second = mgr.destroy("aux");
    const auto refused_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
    assert(!second);
    assert(refused_ms < 500);
    assert(!destroy_done.load());  // first destroy still owns the teardown

    g_idr_release.store(true);
    caller.join();
    destroyer.join();
    assert(destroy_ok.load());             // owner completed the teardown
    assert(g_unsubscribes.load() == 1);    // exactly ONE teardown ran
    assert(!mgr.has_encoder("aux"));
}

void test_concurrent_same_name_create_single_winner() {
    // Contract 8: two concurrent create_from_context() calls for one name
    // must produce exactly one registration; the loser fails at the name
    // reservation BEFORE touching the HAL (no orphan subscription).
    HalCodecOps ops{};
    ops.subscribe = mock_subscribe;
    ops.force_idr = mock_force_idr;
    ops.unsubscribe = mock_unsubscribe;
    EncoderManager mgr(&ops);

    g_subscribes.store(0);
    g_unsubscribes.store(0);
    std::atomic<int> wins{0};
    auto make = [&](void* ctx) {
        if (mgr.create_from_context("aux", ctx)) wins.fetch_add(1);
    };
    std::thread a([&] { make(reinterpret_cast<void*>(0xAAAA)); });
    std::thread b([&] { make(reinterpret_cast<void*>(0xBBBB)); });
    a.join();
    b.join();

    // Exactly one HAL subscription ever happens, whichever way the race
    // resolves: the reservation loser fails fast with false; a caller that
    // only arrives AFTER the winner registered gets the pre-existing
    // idempotent "already exists" == true. Both must end with exactly one
    // destroyable registration and no orphan subscription.
    assert(wins.load() >= 1);
    assert(g_subscribes.load() == 1);   // never two subscribes for one name
    assert(mgr.has_encoder("aux"));

    // The winner's teardown is clean and unambiguous.
    assert(mgr.destroy("aux"));
    assert(g_unsubscribes.load() == 1);
    assert(!mgr.has_encoder("aux"));
}

void test_destroy_waits_out_inflight_creation() {
    // Contract 9: destroy() of a name whose creation is mid-HAL must not
    // report "nothing registered" — it waits for the creator to publish,
    // then tears the published encoder down.
    HalCodecOps ops{};
    ops.subscribe = mock_subscribe_gated;
    ops.force_idr = mock_force_idr;
    ops.unsubscribe = mock_unsubscribe;
    EncoderManager mgr(&ops);
    void* const aux_ctx = reinterpret_cast<void*>(0x7777);

    g_unsubscribes.store(0);
    g_subscribe_in.store(0);
    g_subscribe_release.store(false);
    g_subscribe_deliver.store(false);

    std::thread creator([&] { assert(mgr.create_from_context("aux", aux_ctx)); });
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_subscribe_in.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(g_subscribe_in.load() > 0);  // creator blocked inside subscribe
    }

    std::atomic<bool> destroy_done{false};
    std::atomic<bool> destroy_ok{false};
    std::thread destroyer([&] {
        destroy_ok.store(mgr.destroy("aux"));
        destroy_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(!destroy_done.load());  // waiting out the creation, not lying

    g_subscribe_release.store(true);
    creator.join();
    destroyer.join();
    assert(destroy_ok.load());             // found + tore down the published encoder
    assert(g_unsubscribes.load() == 1);
    assert(!mgr.has_encoder("aux"));
}

void test_destroy_all_bars_new_creates_and_waits() {
    // Contract 10: destroy_all() rejects creations arriving inside its
    // window and waits out in-flight ones before reporting all-clear.
    HalCodecOps ops{};
    ops.subscribe = mock_subscribe_gated;
    ops.force_idr = mock_force_idr;
    ops.unsubscribe = mock_unsubscribe;
    ops.release_packet = mock_release_packet;
    EncoderManager mgr(&ops);
    void* const aux_ctx = reinterpret_cast<void*>(0x7777);

    g_unsubscribes.store(0);
    g_subscribe_in.store(0);
    g_subscribe_release.store(false);
    g_subscribe_deliver.store(false);

    std::thread creator([&] { assert(mgr.create_from_context("aux", aux_ctx)); });
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_subscribe_in.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(g_subscribe_in.load() > 0);
    }

    std::atomic<bool> all_done{false};
    std::atomic<bool> all_ok{false};
    std::thread caller([&] {
        all_ok.store(mgr.destroy_all());
        all_done.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    assert(!all_done.load());  // waiting for the in-flight creation

    // A creation arriving inside the window is rejected outright.
    assert(!mgr.create_from_context("other", reinterpret_cast<void*>(0x8888)));
    assert(!mgr.has_encoder("other"));

    g_subscribe_release.store(true);
    creator.join();
    caller.join();
    assert(all_ok.load());
    assert(g_unsubscribes.load() == 1);    // the published aux got torn down
    assert(!mgr.has_encoder("aux"));

    // Window closed: creations work again.
    assert(mgr.create_from_context("other", reinterpret_cast<void*>(0x8888)));
    assert(mgr.has_encoder("other"));
    assert(mgr.destroy_all());
}

void test_pending_creation_packets_released() {
    // Contract 11: a FROM_MEDIA encoder delivering inside subscribe() —
    // before the promotion critical section — hits the release-only path:
    // dropped but RELEASED, not leaked.
    HalCodecOps ops{};
    ops.subscribe = mock_subscribe_gated;
    ops.force_idr = mock_force_idr;
    ops.unsubscribe = mock_unsubscribe;
    ops.release_packet = mock_release_packet;
    EncoderManager mgr(&ops);
    void* const aux_ctx = reinterpret_cast<void*>(0x7777);

    g_subscribes.store(0);
    g_releases.store(0);
    g_subscribe_in.store(0);
    g_subscribe_release.store(true);   // no gating — deliver and return
    g_subscribe_deliver.store(true);

    assert(mgr.create_from_context("aux", aux_ctx));
    assert(g_releases.load() == 1);    // pending-window packet was released

    // Post-promotion packets route normally (released at the end of the
    // normal path).
    HalPacketBuffer pkt{};
    pkt.size = 4;
    mgr.on_packet(aux_ctx, &pkt);
    assert(g_releases.load() == 2);
    assert(mgr.has_encoder("aux"));

    assert(mgr.destroy("aux"));
    assert(!mgr.has_encoder("aux"));
}

}  // namespace

int main() {
    test_publisher_ctrl_does_not_block_on_callback();
    test_manager_does_not_hold_mu_across_force_idr();
    test_destroy_aborts_hal_teardown_on_drain_timeout();
    test_closing_packets_released_and_create_gated();
    test_concurrent_destroy_claims_tombstone_once();
    test_concurrent_same_name_create_single_winner();
    test_destroy_waits_out_inflight_creation();
    test_destroy_all_bars_new_creates_and_waits();
    test_pending_creation_packets_released();
    return 0;
}
