#include "fd_receiver.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

using namespace aipc::ai_runtime;
using namespace std::chrono_literals;

namespace {

class FakeFdPublisher {
public:
    explicit FakeFdPublisher(bool send_subscribe_response = true,
                             bool reject_client_writes_after_frame = false,
                             bool send_malformed_frame = false)
        : socket_path_("/tmp/fd-receiver-deadlock-" + std::to_string(getpid()) + ".sock")
        , send_subscribe_response_(send_subscribe_response)
        , reject_client_writes_after_frame_(reject_client_writes_after_frame)
        , send_malformed_frame_(send_malformed_frame) {
        listen_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) fail("socket");

        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
        unlink(socket_path_.c_str());
        if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) fail("bind");
        if (listen(listen_fd_, 1) != 0) fail("listen");

        server_thread_ = std::thread(&FakeFdPublisher::run, this);
    }

    ~FakeFdPublisher() {
        {
            std::lock_guard lock(mu_);
            stopping_ = true;
            cv_.notify_all();
        }
        int client_fd = client_fd_.load(std::memory_order_acquire);
        if (client_fd >= 0) shutdown(client_fd, SHUT_RDWR);
        if (listen_fd_ >= 0) shutdown(listen_fd_, SHUT_RDWR);
        if (server_thread_.joinable()) server_thread_.join();
        client_fd = client_fd_.exchange(-1, std::memory_order_acq_rel);
        if (client_fd >= 0) close(client_fd);
        if (listen_fd_ >= 0) close(listen_fd_);
        unlink(socket_path_.c_str());
    }

    const std::string& socket_path() const { return socket_path_; }

    void send_frame() {
        std::unique_lock lock(mu_);
        if (!cv_.wait_for(lock, 2s, [&] { return subscribed_; })) {
            std::fprintf(stderr, "publisher did not receive SUBSCRIBE\n");
            std::_Exit(1);
        }
        send_frame_ = true;
        cv_.notify_all();
    }

    void disconnect_client() {
        std::unique_lock lock(mu_);
        if (!cv_.wait_for(lock, 2s, [&] { return subscribed_; })) {
            std::fprintf(stderr, "publisher did not receive SUBSCRIBE\n");
            std::_Exit(1);
        }
        drop_connection_ = true;
        cv_.notify_all();
    }

    bool wait_for_release_count(int expected) {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, 2s, [&] {
            return release_count_ >= expected;
        });
    }

    bool wait_for_release() { return wait_for_release_count(1); }

    int release_count() {
        std::lock_guard lock(mu_);
        return release_count_;
    }

    bool wait_for_unsubscribe() {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, 2s, [&] { return unsubscribe_received_; });
    }

private:
    [[noreturn]] static void fail(const char* operation) {
        std::perror(operation);
        std::_Exit(1);
    }

    void run() {
        int client_fd = accept(listen_fd_, nullptr, nullptr);
        if (client_fd < 0) return;
        client_fd_.store(client_fd, std::memory_order_release);

        FdPubSubscribeMsg subscribe{};
        int fds[FD_PUB_MAX_FDS];
        int num_fds = 0;
        if (fd_pub_recvmsg(client_fd, &subscribe, sizeof(subscribe),
                           fds, &num_fds, FD_PUB_MAX_FDS) <= 0) {
            return;
        }

        if (!send_subscribe_response_) {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [&] { return stopping_; });
            return;
        }

        FdPubResponseMsg response{};
        response.hdr.type = FD_PUB_MSG_OK;
        response.hdr.size = sizeof(response);
        if (fd_pub_sendmsg(client_fd, &response, sizeof(response), nullptr, 0) != 0) return;

        {
            std::unique_lock lock(mu_);
            subscribed_ = true;
            cv_.notify_all();
        }

        // Combined serve loop: control messages (RELEASE / UNSUBSCRIBE) are
        // processed whenever they arrive, and each send_frame() call from a
        // test delivers one more frame over the same connection (frame_id
        // increments so RELEASE matching stays unambiguous in multi-frame
        // scenarios). Poll with a short timeout so both event kinds
        // interleave instead of blocking on one or the other.
        uint64_t next_frame_id = 42;
        for (;;) {
            pollfd poll_fd{client_fd, POLLIN, 0};
            const int poll_rc = ::poll(&poll_fd, 1, 20);
            if (poll_rc < 0) return;
            if (poll_rc > 0 && (poll_fd.revents & POLLIN)) {
                FdPubMsgHeader header{};
                num_fds = 0;
                int received = fd_pub_recvmsg(
                    client_fd, &header, sizeof(header), fds, &num_fds,
                    FD_PUB_MAX_FDS);
                if (received < 0 && (errno == EAGAIN || errno == EINTR)) {
                    // Spurious poll wakeup; nothing was consumed.
                } else if (received <= 0 || num_fds != 0 ||
                           header.size < sizeof(header) ||
                           header.size > sizeof(FdPubReleaseMsg)) {
                    for (int i = 0; i < num_fds; ++i) close(fds[i]);
                    return;
                } else {
                    FdPubReleaseMsg message{};
                    message.hdr = header;
                    const size_t payload_size = header.size - sizeof(header);
                    if (payload_size > 0) {
                        auto* payload =
                            reinterpret_cast<char*>(&message) + sizeof(header);
                        if (recv(client_fd, payload, payload_size, MSG_WAITALL) !=
                            static_cast<ssize_t>(payload_size)) {
                            return;
                        }
                    }

                    if (message.hdr.type == FD_PUB_MSG_RELEASE &&
                        message.hdr.size == sizeof(message)) {
                        std::lock_guard lock(mu_);
                        ++release_count_;
                        cv_.notify_all();
                    } else if (message.hdr.type == FD_PUB_MSG_UNSUBSCRIBE &&
                               message.hdr.size == sizeof(FdPubMsgHeader)) {
                        std::lock_guard lock(mu_);
                        unsubscribe_received_ = true;
                        cv_.notify_all();
                        return;
                    } else {
                        return;
                    }
                }
            }

            bool send_now = false;
            {
                std::lock_guard lock(mu_);
                if (drop_connection_) {
                    shutdown(client_fd, SHUT_RDWR);
                    return;
                }
                if (stopping_) return;
                if (send_frame_) {
                    send_frame_ = false;
                    send_now = true;
                }
            }
            if (!send_now) continue;

            FdPubFrameMsg frame{};
            frame.hdr.type = FD_PUB_MSG_FRAME;
            frame.hdr.size = sizeof(frame);
            frame.frame_id = next_frame_id++;
            frame.width = send_malformed_frame_ ? 0 : 16;
            frame.height = 16;
            frame.num_planes = 1;
            frame.num_fds = 1;
            int frame_fd = dup(listen_fd_);
            if (frame_fd < 0) return;
            const int send_rc = fd_pub_sendmsg(
                client_fd, &frame, sizeof(frame), &frame_fd, 1);
            close(frame_fd);
            if (send_rc != 0) return;
            if (reject_client_writes_after_frame_) {
                shutdown(client_fd, SHUT_RD);
                std::unique_lock lock(mu_);
                cv_.wait(lock, [&] { return stopping_; });
                return;
            }
        }
    }

    std::string socket_path_;
    bool send_subscribe_response_ = true;
    bool reject_client_writes_after_frame_ = false;
    bool send_malformed_frame_ = false;
    int listen_fd_ = -1;
    std::atomic<int> client_fd_{-1};
    std::thread server_thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopping_ = false;
    bool subscribed_ = false;
    bool send_frame_ = false;
    bool drop_connection_ = false;
    int release_count_ = 0;
    bool unsubscribe_received_ = false;
};

}  // namespace

bool test_external_unsubscribe_fences_callback() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());

    std::mutex callback_mu;
    std::condition_variable callback_cv;
    bool callback_entered = false;
    bool allow_release = false;
    bool callback_exited = false;
    std::atomic<bool> unsubscribe_done{false};

    if (!receiver.subscribe("cam0_main", "deadlock-test", [&](const ReceivedFrame& frame) {
            {
                std::unique_lock lock(callback_mu);
                callback_entered = true;
                callback_cv.notify_all();
                callback_cv.wait(lock, [&] { return allow_release; });
            }
            frame.delivery.acknowledge();
            {
                std::lock_guard lock(callback_mu);
                callback_exited = true;
                callback_cv.notify_all();
            }
        })) {
        std::fprintf(stderr, "subscribe failed\n");
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(callback_mu);
        if (!callback_cv.wait_for(lock, 2s, [&] { return callback_entered; })) {
            std::fprintf(stderr, "callback did not start\n");
            return false;
        }
    }

    std::thread unsubscribe_thread([&] {
        receiver.unsubscribe("cam0_main", "deadlock-test");
        unsubscribe_done.store(true, std::memory_order_release);
        callback_cv.notify_all();
    });

    std::this_thread::sleep_for(100ms);
    if (unsubscribe_done.load(std::memory_order_acquire)) {
        std::fprintf(stderr, "unsubscribe returned while callback was active\n");
        std::_Exit(1);
    }

    {
        std::lock_guard lock(callback_mu);
        allow_release = true;
        callback_cv.notify_all();
    }

    {
        std::unique_lock lock(callback_mu);
        if (!callback_cv.wait_for(lock, 2s, [&] {
                return unsubscribe_done.load(std::memory_order_acquire);
            })) {
            std::fprintf(stderr, "unsubscribe deadlocked with callback release_frame\n");
            std::_Exit(1);
        }
    }
    unsubscribe_thread.join();

    if (!callback_exited || !publisher.wait_for_release()) {
        std::fprintf(stderr, "external unsubscribe cleanup failed\n");
        return false;
    }
    return true;
}

bool test_retained_frame_holds_publisher_lease() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    std::unique_ptr<ReceivedFrame> retained;
    bool retained_ready = false;

    if (!receiver.subscribe("cam0_main", "retaining-subscriber",
            [&](const ReceivedFrame& frame) {
                {
                    std::lock_guard lock(mu);
                    retained = std::make_unique<ReceivedFrame>(frame);
                    retained_ready = true;
                }
                cv.notify_all();
            })) {
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return retained_ready; }))
            return false;
    }

    const auto started = std::chrono::steady_clock::now();
    if (receiver.unsubscribe_until(
            "cam0_main", "retaining-subscriber", started + 100ms)) {
        std::fprintf(stderr,
                     "unsubscribe reported quiescence with a retained frame\n");
        return false;
    }
    if (std::chrono::steady_clock::now() - started > 500ms ||
        publisher.release_count() != 0) {
        std::fprintf(stderr,
                     "retained frame deadline or lease handling was incorrect\n");
        return false;
    }

    {
        std::lock_guard lock(mu);
        retained.reset();
    }
    if (!publisher.wait_for_release() || publisher.release_count() != 1) {
        std::fprintf(stderr,
                     "dropping retained frame did not release exactly once\n");
        return false;
    }
    if (!receiver.unsubscribe_until(
            "cam0_main", "retaining-subscriber",
            std::chrono::steady_clock::now() + 2s)) {
        return false;
    }

    return publisher.wait_for_unsubscribe() &&
           publisher.release_count() == 1;
}

bool test_non_last_unsubscribe_waits_for_retained_delivery() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    std::unique_ptr<ReceivedFrame> retained;
    bool retained_ready = false;

    if (!receiver.subscribe("cam0_main", "retaining-subscriber",
            [&](const ReceivedFrame& frame) {
                {
                    std::lock_guard lock(mu);
                    retained = std::make_unique<ReceivedFrame>(frame);
                    retained_ready = true;
                }
                cv.notify_all();
            }) ||
        !receiver.subscribe("cam0_main", "passive-subscriber",
                            [](const ReceivedFrame&) {})) {
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return retained_ready; }))
            return false;
    }

    if (receiver.unsubscribe_until(
            "cam0_main", "retaining-subscriber",
            std::chrono::steady_clock::now() + 100ms)) {
        std::fprintf(stderr,
                     "non-last unsubscribe ignored a retained delivery\n");
        return false;
    }
    if (receiver.unsubscribe_until(
            "cam0_main", "retaining-subscriber",
            std::chrono::steady_clock::now() + 100ms)) {
        std::fprintf(stderr,
                     "retry ignored a draining retained delivery\n");
        return false;
    }
    if (receiver.subscribe("cam0_main", "retaining-subscriber",
                           [](const ReceivedFrame&) {})) {
        std::fprintf(stderr,
                     "subscriber id was reused while its delivery drained\n");
        return false;
    }
    {
        std::lock_guard lock(mu);
        retained.reset();
    }
    if (!publisher.wait_for_release() || publisher.release_count() != 1)
        return false;
    if (!receiver.unsubscribe_until(
            "cam0_main", "retaining-subscriber",
            std::chrono::steady_clock::now() + 2s)) {
        return false;
    }

    receiver.unsubscribe("cam0_main", "passive-subscriber");
    return publisher.wait_for_unsubscribe();
}

bool test_callback_owned_retained_frame_does_not_cycle() {
    struct CallbackState {
        std::unique_ptr<ReceivedFrame> retained;
    };

    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    bool retained_ready = false;
    auto callback_state = std::make_shared<CallbackState>();

    if (!receiver.subscribe(
            "cam0_main", "callback-cycle-test",
            [callback_state, &mu, &cv, &retained_ready](
                const ReceivedFrame& frame) {
                callback_state->retained =
                    std::make_unique<ReceivedFrame>(frame);
                {
                    std::lock_guard lock(mu);
                    retained_ready = true;
                }
                cv.notify_all();
            })) {
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return retained_ready; }))
            return false;
    }

    callback_state.reset();
    if (!receiver.unsubscribe_until(
            "cam0_main", "callback-cycle-test",
            std::chrono::steady_clock::now() + 2s)) {
        std::fprintf(stderr,
                     "callback-owned retained frame formed a shutdown cycle\n");
        return false;
    }
    return publisher.wait_for_release() &&
           publisher.wait_for_unsubscribe() &&
           publisher.release_count() == 1;
}

bool test_stop_all_waits_for_retained_frame() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    std::unique_ptr<ReceivedFrame> retained;
    bool retained_ready = false;
    std::atomic<bool> stop_done{false};

    if (!receiver.subscribe("cam0_main", "stop-retained-test",
            [&](const ReceivedFrame& frame) {
                {
                    std::lock_guard lock(mu);
                    retained = std::make_unique<ReceivedFrame>(frame);
                    retained_ready = true;
                }
                cv.notify_all();
            })) {
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return retained_ready; }))
            return false;
    }

    std::thread stop_thread([&] {
        receiver.stop_all();
        stop_done.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(100ms);
    if (stop_done.load(std::memory_order_acquire)) {
        std::fprintf(stderr,
                     "stop_all returned with a retained frame outstanding\n");
        std::_Exit(1);
    }

    {
        std::lock_guard lock(mu);
        retained.reset();
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!stop_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    if (!stop_done.load(std::memory_order_acquire)) std::_Exit(1);
    stop_thread.join();

    return publisher.wait_for_release() &&
           publisher.wait_for_unsubscribe() &&
           publisher.release_count() == 1;
}

bool test_throwing_callback_does_not_wedge_unsubscribe() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    bool callback_entered = false;

    if (!receiver.subscribe("cam0_main", "throw-test", [&](const ReceivedFrame&) {
            {
                std::lock_guard lock(mu);
                callback_entered = true;
                cv.notify_all();
            }
            throw std::runtime_error("expected callback failure");
        })) {
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return callback_entered; })) return false;
    }

    receiver.unsubscribe("cam0_main", "throw-test");
    return publisher.wait_for_unsubscribe() &&
           publisher.release_count() == 1;
}

bool test_release_then_throw_is_idempotent() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    bool callback_entered = false;

    if (!receiver.subscribe("cam0_main", "release-throw-test",
            [&](const ReceivedFrame& frame) {
                frame.delivery.acknowledge();
                {
                    std::lock_guard lock(mu);
                    callback_entered = true;
                    cv.notify_all();
                }
                throw std::runtime_error("expected callback failure");
            })) {
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return callback_entered; }))
            return false;
    }

    receiver.unsubscribe("cam0_main", "release-throw-test");
    return publisher.wait_for_unsubscribe() &&
           publisher.release_count() == 1;
}

bool test_callback_can_stop_all() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    bool callback_done = false;

    if (!receiver.subscribe("cam0_main", "stop-all-test",
            [&](const ReceivedFrame& frame) {
                frame.delivery.acknowledge();
                receiver.stop_all();
                {
                    std::lock_guard lock(mu);
                    callback_done = true;
                    cv.notify_all();
                }
            })) {
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return callback_done; })) {
            std::fprintf(stderr, "callback stop_all did not return\n");
            return false;
        }
    }

    receiver.stop_all();  // external reaper joins the retired receive thread
    return publisher.wait_for_unsubscribe() &&
           publisher.release_count() == 1;
}

bool test_callback_can_unsubscribe_itself() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    bool callback_done = false;
    bool self_unsubscribe_reported_quiesced = true;

    if (!receiver.subscribe("cam0_main", "self-test", [&](const ReceivedFrame& frame) {
            const bool quiesced = receiver.unsubscribe_until(
                "cam0_main", "self-test",
                std::chrono::steady_clock::now() + 1s);
            frame.delivery.acknowledge();
            {
                std::lock_guard lock(mu);
                self_unsubscribe_reported_quiesced = quiesced;
                callback_done = true;
                cv.notify_all();
            }
        })) {
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return callback_done; })) {
            std::fprintf(stderr, "self-unsubscribe callback deadlocked\n");
            return false;
        }
    }

    receiver.stop_all();
    return !self_unsubscribe_reported_quiesced &&
           receiver.subscriber_count("cam0_main") == 0 &&
           publisher.wait_for_release();
}

bool test_subscribe_response_timeout_is_bounded() {
    FakeFdPublisher publisher(false);
    FdReceiver receiver(publisher.socket_path());

    const auto started = std::chrono::steady_clock::now();
    const bool subscribed = receiver.subscribe(
        "cam0_main", "subscribe-timeout-test", [](const ReceivedFrame&) {});
    const auto elapsed = std::chrono::steady_clock::now() - started;

    if (subscribed || elapsed < 800ms || elapsed > 2500ms) {
        std::fprintf(stderr,
                     "SUBSCRIBE response timeout was not bounded (%lld ms)\n",
                     static_cast<long long>(
                         std::chrono::duration_cast<std::chrono::milliseconds>(
                             elapsed).count()));
        return false;
    }
    return true;
}

bool test_idle_stream_stays_connected() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    bool callback_done = false;

    if (!receiver.subscribe(
            "cam0_main", "idle-test", [&](const ReceivedFrame& frame) {
                frame.delivery.acknowledge();
                {
                    std::lock_guard lock(mu);
                    callback_done = true;
                }
                cv.notify_all();
            })) {
        return false;
    }

    std::this_thread::sleep_for(1200ms);
    if (!receiver.stream_connected("cam0_main")) {
        std::fprintf(stderr, "idle stream disconnected on receive timeout\n");
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return callback_done; }))
            return false;
    }
    receiver.unsubscribe("cam0_main", "idle-test");
    return publisher.wait_for_release() && publisher.wait_for_unsubscribe();
}

bool test_unsubscribe_deadline_bounds_active_callback() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    bool callback_entered = false;
    bool release_callback = false;
    bool callback_exited = false;

    if (!receiver.subscribe("cam0_main", "deadline-test",
            [&](const ReceivedFrame& frame) {
                std::unique_lock lock(mu);
                callback_entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release_callback; });
                lock.unlock();
                frame.delivery.acknowledge();
                lock.lock();
                callback_exited = true;
                cv.notify_all();
            })) {
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return callback_entered; }))
            return false;
    }

    const auto started = std::chrono::steady_clock::now();
    const bool quiesced = receiver.unsubscribe_until(
        "cam0_main", "deadline-test", started + 100ms);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    if (quiesced || elapsed > 500ms) {
        std::fprintf(stderr, "deadline-aware unsubscribe was not bounded\n");
        return false;
    }

    {
        std::lock_guard lock(mu);
        release_callback = true;
        cv.notify_all();
    }
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return callback_exited; }))
            return false;
    }
    receiver.unsubscribe("cam0_main", "deadline-test");
    return publisher.wait_for_unsubscribe() && publisher.wait_for_release() &&
           receiver.subscriber_count("cam0_main") == 0;
}

bool test_malformed_frame_forces_disconnect() {
    FakeFdPublisher publisher(true, false, true);
    FdReceiver receiver(publisher.socket_path());
    std::atomic<bool> callback_called{false};

    if (!receiver.subscribe(
            "cam0_main", "malformed-test", [&](const ReceivedFrame&) {
                callback_called.store(true, std::memory_order_release);
            })) {
        return false;
    }
    publisher.send_frame();

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (receiver.stream_connected("cam0_main") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    return !receiver.stream_connected("cam0_main") &&
           !callback_called.load(std::memory_order_acquire);
}

bool test_release_send_failure_disconnects_without_deadlock() {
    FakeFdPublisher publisher(true, true);
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    std::unique_ptr<ReceivedFrame> retained;
    bool retained_ready = false;

    if (!receiver.subscribe(
            "cam0_main", "release-failure-test",
            [&](const ReceivedFrame& frame) {
                {
                    std::lock_guard lock(mu);
                    retained = std::make_unique<ReceivedFrame>(frame);
                    retained_ready = true;
                }
                cv.notify_all();
            })) {
        return false;
    }
    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return retained_ready; }))
            return false;
    }

    {
        std::lock_guard lock(mu);
        retained->delivery.acknowledge();
        retained->delivery.acknowledge();
        retained.reset();
    }
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (receiver.stream_connected("cam0_main") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    if (receiver.stream_connected("cam0_main")) return false;

    return receiver.unsubscribe_until(
               "cam0_main", "release-failure-test",
               std::chrono::steady_clock::now() + 2s) &&
           publisher.release_count() == 0;
}

bool test_publisher_disconnect_marks_stream_disconnected() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());

    if (!receiver.subscribe(
            "cam0_main", "disconnect-test", [](const ReceivedFrame&) {})) {
        return false;
    }
    if (!receiver.stream_connected("cam0_main")) return false;

    publisher.disconnect_client();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (receiver.stream_connected("cam0_main") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
    }
    return !receiver.stream_connected("cam0_main");
}

bool test_receiver_can_be_destroyed_after_callback_stop() {
    FakeFdPublisher publisher;
    std::unique_ptr<FdReceiver> receiver =
        std::make_unique<FdReceiver>(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    bool callback_done = false;

    if (!receiver->subscribe(
            "cam0_main", "destroy-test", [&](const ReceivedFrame& frame) {
                frame.delivery.acknowledge();
                receiver->stop_all();
                receiver.reset();
                {
                    std::lock_guard lock(mu);
                    callback_done = true;
                    cv.notify_all();
                }
            })) {
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return callback_done; }))
            return false;
    }
    return !receiver && publisher.wait_for_release();
}

// Race regression: a new subscriber arriving while the last subscriber's
// unsubscribe is still parked inside its callback-drain wait must keep the
// stream connection alive — the parked unsubscribe must not tear down a
// connection the newcomer just joined, and the newcomer must receive frames
// afterwards over the same connection.
bool test_new_subscriber_during_last_unsubscribe_keeps_connection() {
    FakeFdPublisher publisher;
    FdReceiver receiver(publisher.socket_path());
    std::mutex mu;
    std::condition_variable cv;
    bool callback_entered = false;
    bool release_callback = false;
    bool callback_exited = false;

    if (!receiver.subscribe("cam0_main", "blocked-sub",
            [&](const ReceivedFrame& frame) {
                std::unique_lock lock(mu);
                callback_entered = true;
                cv.notify_all();
                cv.wait(lock, [&] { return release_callback; });
                lock.unlock();
                frame.delivery.acknowledge();
                lock.lock();
                callback_exited = true;
                cv.notify_all();
            })) {
        return false;
    }

    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return callback_entered; }))
            return false;
    }

    // Bounded unsubscribe of the (blocked) last subscriber on its own thread:
    // it parks inside the drain wait while the callback is active.
    std::atomic<bool> unsubscribe_done{false};
    std::atomic<bool> unsubscribe_ok{false};
    std::thread unsub_thread([&] {
        unsubscribe_ok = receiver.unsubscribe_until(
            "cam0_main", "blocked-sub",
            std::chrono::steady_clock::now() + 10s);
        unsubscribe_done = true;
    });

    // Give the unsubscribe thread time to park inside its drain wait.
    std::this_thread::sleep_for(150ms);
    if (unsubscribe_done.load()) {
        // Only a teardown of the parked path could finish this early — that
        // would drop the connection before the newcomer arrives.
        unsub_thread.join();
        return false;
    }

    // The newcomer joins the same stream while the drain wait is parked.
    std::atomic<bool> newcomer_got_frame{false};
    if (!receiver.subscribe("cam0_main", "newcomer-sub",
            [&](const ReceivedFrame& frame) {
                frame.delivery.acknowledge();
                newcomer_got_frame = true;
            })) {
        {
            std::lock_guard lock(mu);
            release_callback = true;
            cv.notify_all();
        }
        unsub_thread.join();
        return false;
    }

    // Release the blocked callback; the parked unsubscribe must complete
    // successfully without tearing the connection down.
    {
        std::lock_guard lock(mu);
        release_callback = true;
        cv.notify_all();
    }
    unsub_thread.join();
    if (!unsubscribe_ok.load()) return false;

    // The connection survived: the newcomer still receives a frame.
    if (!receiver.stream_connected("cam0_main")) return false;
    publisher.send_frame();
    {
        std::unique_lock lock(mu);
        if (!cv.wait_for(lock, 2s, [&] { return newcomer_got_frame.load(); }))
            return false;
    }

    receiver.stop_all();
    // Frame 42 was acked by the blocked subscriber, frame 43 by the newcomer.
    return publisher.wait_for_unsubscribe() &&
           publisher.wait_for_release_count(2) &&
           receiver.subscriber_count("cam0_main") == 0;
}

// Race regression: the publisher dropping the connection (the receive
// thread's local failure path) while stop_all() is concurrently parked in
// its callback-drain wait must not deadlock — stop_all returns, the receive
// thread exits, and the receiver is destroyable. After a hard disconnect no
// RELEASE/UNSUBSCRIBE can reach the publisher, so the assertions are the
// absence of a hang plus a clean post-state.
bool test_publisher_disconnect_during_stop_all_does_not_deadlock() {
    FakeFdPublisher publisher;
    {
        FdReceiver receiver(publisher.socket_path());
        std::mutex mu;
        std::condition_variable cv;
        bool callback_entered = false;
        bool release_callback = false;

        if (!receiver.subscribe("cam0_main", "stop-race-sub",
                [&](const ReceivedFrame& frame) {
                    std::unique_lock lock(mu);
                    callback_entered = true;
                    cv.notify_all();
                    cv.wait(lock, [&] { return release_callback; });
                    lock.unlock();
                    frame.delivery.acknowledge();
                })) {
            return false;
        }

        publisher.send_frame();
        {
            std::unique_lock lock(mu);
            if (!cv.wait_for(lock, 2s, [&] { return callback_entered; }))
                return false;
        }

        // stop_all() on another thread parks waiting for the active callback.
        std::thread stop_thread([&] { receiver.stop_all(); });
        std::this_thread::sleep_for(100ms);

        // Concurrent local failure: the publisher drops the connection out
        // from under the parked stop_all().
        publisher.disconnect_client();
        std::this_thread::sleep_for(50ms);

        // Let the callback return so both the receive thread and stop_all()
        // can proceed; neither may wedge on the other's teardown.
        {
            std::lock_guard lock(mu);
            release_callback = true;
            cv.notify_all();
        }
        stop_thread.join();

        if (receiver.subscriber_count("cam0_main") != 0) return false;
    }
    return true;
}

int main() {
    if (!test_external_unsubscribe_fences_callback()) return 1;
    if (!test_retained_frame_holds_publisher_lease()) return 1;
    if (!test_non_last_unsubscribe_waits_for_retained_delivery()) return 1;
    if (!test_callback_owned_retained_frame_does_not_cycle()) return 1;
    if (!test_stop_all_waits_for_retained_frame()) return 1;
    if (!test_throwing_callback_does_not_wedge_unsubscribe()) return 1;
    if (!test_release_then_throw_is_idempotent()) return 1;
    if (!test_callback_can_stop_all()) return 1;
    if (!test_callback_can_unsubscribe_itself()) return 1;
    if (!test_subscribe_response_timeout_is_bounded()) return 1;
    if (!test_idle_stream_stays_connected()) return 1;
    if (!test_unsubscribe_deadline_bounds_active_callback()) return 1;
    if (!test_malformed_frame_forces_disconnect()) return 1;
    if (!test_release_send_failure_disconnects_without_deadlock()) return 1;
    if (!test_publisher_disconnect_marks_stream_disconnected()) return 1;
    if (!test_receiver_can_be_destroyed_after_callback_stop()) return 1;
    if (!test_new_subscriber_during_last_unsubscribe_keeps_connection()) return 1;
    if (!test_publisher_disconnect_during_stop_all_does_not_deadlock()) return 1;

    std::fprintf(stderr, "FdReceiver lifecycle regressions: PASS\n");
    return 0;
}
