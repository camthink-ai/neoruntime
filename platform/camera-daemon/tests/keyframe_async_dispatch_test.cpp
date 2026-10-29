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
//   3. destroy() drains an in-flight call (bounded) before erasing.

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

    // --- Part 3: destroy() drains an in-flight call before erasing ---
    g_idr_release.store(false);
    std::thread caller2([&] { mgr.force_keyframe("main"); });
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (g_idr_in_calls.load() == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(g_idr_in_calls.load() > 0);
    }
    g_idr_release.store(true);  // let the call return; destroy() drains it
    caller2.join();
    mgr.destroy("main");
    assert(!mgr.has_encoder("main"));
}

}  // namespace

int main() {
    test_publisher_ctrl_does_not_block_on_callback();
    test_manager_does_not_hold_mu_across_force_idr();
    return 0;
}
