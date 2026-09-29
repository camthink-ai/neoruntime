#include "fd_receiver.h"
#include "log.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <chrono>
#include <exception>

namespace aipc::ai_runtime {
namespace {

bool lock_until(
    std::unique_lock<std::timed_mutex>& lock,
    std::chrono::steady_clock::time_point deadline) {
    if (deadline == std::chrono::steady_clock::time_point::max()) {
        lock.lock();
        return true;
    }
    return lock.try_lock_until(deadline);
}

struct ReceivedFdGuard {
    int* fds = nullptr;
    int count = 0;

    ~ReceivedFdGuard() {
        for (int i = 0; i < count; ++i) {
            if (fds[i] >= 0) ::close(fds[i]);
        }
    }
};

}  // namespace

struct FrameAckAggregate {
    std::atomic<size_t> remaining{0};
    std::function<void()> release;

    void acknowledge() noexcept {
        if (remaining.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
        try {
            release();
        } catch (...) {
            LOG_ERROR("FdReceiver: frame release callback threw");
        }
    }
};

struct FrameDeliveryState {
    std::shared_ptr<FrameAckAggregate> aggregate;
    std::function<void()> on_acknowledge;
    std::atomic<bool> registered{false};
    std::atomic<bool> acknowledged{false};

    void acknowledge() noexcept {
        if (acknowledged.exchange(true, std::memory_order_acq_rel)) return;
        auto frame_aggregate = std::move(aggregate);
        auto cleanup = std::move(on_acknowledge);
        const bool was_registered =
            registered.exchange(false, std::memory_order_acq_rel);
        if (frame_aggregate) frame_aggregate->acknowledge();
        try {
            if (was_registered && cleanup) cleanup();
        } catch (...) {
            LOG_ERROR("FdReceiver: delivery acknowledgement cleanup threw");
        }
    }

    ~FrameDeliveryState() { acknowledge(); }
};

void FrameDelivery::acknowledge() const noexcept {
    if (state_) state_->acknowledge();
}

FdReceiver::FdReceiver(const std::string& socket_path)
    : socket_path_(socket_path) {}

FdReceiver::~FdReceiver() {
    stop_all();
}

int FdReceiver::connect_to_server() {
    constexpr int kConnectTimeoutMs = 1000;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        LOG_ERROR("FdReceiver: socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    int rc = ::connect(
        fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS && errno != EAGAIN) {
        LOG_ERROR("FdReceiver: connect(%s) failed: %s",
                  socket_path_.c_str(), strerror(errno));
        ::close(fd);
        return -1;
    }

    if (rc < 0) {
        pollfd pfd{fd, POLLOUT, 0};
        do {
            rc = ::poll(&pfd, 1, kConnectTimeoutMs);
        } while (rc < 0 && errno == EINTR);

        int socket_error = 0;
        socklen_t error_size = sizeof(socket_error);
        if (rc <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR,
                                    &socket_error, &error_size) != 0 ||
            socket_error != 0) {
            LOG_ERROR("FdReceiver: connect(%s) timed out or failed: %s",
                      socket_path_.c_str(),
                      socket_error ? strerror(socket_error) : strerror(errno));
            ::close(fd);
            return -1;
        }
    }

    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) != 0) {
        LOG_ERROR("FdReceiver: failed to restore blocking socket: %s",
                  strerror(errno));
        ::close(fd);
        return -1;
    }

    timeval send_timeout{};
    send_timeout.tv_sec = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                     &send_timeout, sizeof(send_timeout)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                     &send_timeout, sizeof(send_timeout)) != 0) {
        LOG_ERROR("FdReceiver: failed to set socket timeout: %s",
                  strerror(errno));
        ::close(fd);
        return -1;
    }
    return fd;
}

// Establish the physical connection and first subscriber as one transaction
// (called under mu_). The receive thread cannot observe a half-installed stream.
bool FdReceiver::setup_stream_connection(
    const std::string& stream_name,
    const std::shared_ptr<Subscriber>& initial_subscriber) {
    int sock_fd = connect_to_server();
    if (sock_fd < 0) return false;

    FdPubSubscribeMsg sub_msg{};
    sub_msg.hdr.type = FD_PUB_MSG_SUBSCRIBE;
    sub_msg.hdr.size = sizeof(sub_msg);
    sub_msg.version  = FD_PUB_PROTOCOL_VERSION;
    std::strncpy(sub_msg.stream_name, stream_name.c_str(),
                 FD_PUB_MAX_STREAM_NAME - 1);

    if (fd_pub_sendmsg(sock_fd, &sub_msg, sizeof(sub_msg), nullptr, 0) != 0) {
        LOG_ERROR("FdReceiver: failed to send SUBSCRIBE for %s",
                  stream_name.c_str());
        ::close(sock_fd);
        return false;
    }

    constexpr int kSubscribeResponseTimeoutMs = 1000;
    pollfd response_poll{sock_fd, POLLIN, 0};
    int poll_rc;
    do {
        poll_rc = ::poll(&response_poll, 1, kSubscribeResponseTimeoutMs);
    } while (poll_rc < 0 && errno == EINTR);
    if (poll_rc <= 0 || !(response_poll.revents & POLLIN)) {
        LOG_ERROR("FdReceiver: SUBSCRIBE response timed out for %s",
                  stream_name.c_str());
        ::close(sock_fd);
        return false;
    }

    FdPubResponseMsg resp{};
    int recv_fds[FD_PUB_MAX_FDS];
    int num_recv_fds = 0;
    int n = fd_pub_recvmsg(sock_fd, &resp, sizeof(resp), recv_fds,
                           &num_recv_fds, FD_PUB_MAX_FDS);
    const bool valid_response =
        n == static_cast<int>(sizeof(resp)) &&
        resp.hdr.size == sizeof(resp) &&
        resp.hdr.type == FD_PUB_MSG_OK && num_recv_fds == 0;
    if (!valid_response) {
        for (int i = 0; i < num_recv_fds; ++i) ::close(recv_fds[i]);
        LOG_ERROR("FdReceiver: SUBSCRIBE rejected for %s (type=%u code=%d)",
                  stream_name.c_str(), n > 0 ? resp.hdr.type : 0,
                  n > 0 ? resp.code : -1);
        ::close(sock_fd);
        return false;
    }

    timeval no_receive_timeout{};
    if (::setsockopt(sock_fd, SOL_SOCKET, SO_RCVTIMEO,
                     &no_receive_timeout, sizeof(no_receive_timeout)) != 0) {
        LOG_ERROR("FdReceiver: failed to clear receive timeout for %s: %s",
                  stream_name.c_str(), strerror(errno));
        ::close(sock_fd);
        return false;
    }

    try {
        auto conn = std::make_shared<StreamConn>();
        conn->stream_name = stream_name;
        conn->sock_fd = sock_fd;
        conn->subscriber_drains.emplace(
            initial_subscriber->id, initial_subscriber->drain);
        conn->subscribers.push_back(initial_subscriber);
        conn->running = true;

        auto [it, inserted] = streams_.emplace(stream_name, conn);
        if (!inserted) {
            ::close(sock_fd);
            return false;
        }

        try {
            conn->recv_thread =
                std::thread(&FdReceiver::recv_loop, conn);
        } catch (...) {
            streams_.erase(it);
            conn->running = false;
            ::close(sock_fd);
            sock_fd = -1;
            conn->sock_fd = -1;
            throw;
        }
    } catch (const std::exception& e) {
        if (sock_fd >= 0) ::close(sock_fd);
        LOG_ERROR("FdReceiver: setup failed for %s: %s",
                  stream_name.c_str(), e.what());
        return false;
    } catch (...) {
        if (sock_fd >= 0) ::close(sock_fd);
        LOG_ERROR("FdReceiver: setup failed for %s", stream_name.c_str());
        return false;
    }

    LOG_INFO("FdReceiver: stream connection established for %s",
             stream_name.c_str());
    return true;
}

// Tear down the physical connection for a stream without exceeding the caller's
// deadline. The receive loop owns final socket close. A callback-side stop cannot
// join its own thread, so it detaches that stream and completes asynchronously
// after the callback returns; external callers wait for receive-loop quiescence.
bool FdReceiver::teardown_stream_connection_until(
    const std::shared_ptr<StreamConn>& conn,
    std::chrono::steady_clock::time_point deadline) {
    std::unique_lock<std::timed_mutex> teardown_lock(
        conn->teardown_mu, std::defer_lock);
    if (!lock_until(teardown_lock, deadline)) return false;

    conn->running = false;
    {
        std::unique_lock<std::timed_mutex> send_lock(
            conn->send_mu, std::defer_lock);
        if (!lock_until(send_lock, deadline)) return false;
        if (conn->sock_fd >= 0) {
            FdPubMsgHeader unsub{};
            unsub.type = FD_PUB_MSG_UNSUBSCRIBE;
            unsub.size = sizeof(unsub);
            (void)fd_pub_sendmsg_flags(
                conn->sock_fd, &unsub, sizeof(unsub), nullptr, 0,
                MSG_DONTWAIT);
            shutdown(conn->sock_fd, SHUT_RDWR);
        }
    }

    if (conn->recv_thread.joinable() &&
        conn->recv_thread.get_id() == std::this_thread::get_id()) {
        conn->recv_thread.detach();
        return true;
    }

    {
        std::unique_lock<std::timed_mutex> exit_lock(
            conn->exit_mu, std::defer_lock);
        if (!lock_until(exit_lock, deadline)) return false;

        const auto exited = [&] { return conn->exited; };
        if (deadline == std::chrono::steady_clock::time_point::max()) {
            conn->exit_cv.wait(exit_lock, exited);
        } else if (!conn->exit_cv.wait_until(exit_lock, deadline, exited)) {
            return false;
        }
    }

    if (deadline != std::chrono::steady_clock::time_point::max() &&
        std::chrono::steady_clock::now() >= deadline) {
        return false;
    }
    if (conn->recv_thread.joinable()) conn->recv_thread.join();
    return true;
}

void FdReceiver::teardown_stream_connection(
    const std::shared_ptr<StreamConn>& conn) {
    (void)teardown_stream_connection_until(
        conn, std::chrono::steady_clock::time_point::max());
}

bool FdReceiver::subscribe(const std::string& stream_name,
                           const std::string& subscriber_id,
                           FrameCallback cb) {
    std::shared_ptr<Subscriber> new_subscriber;
    try {
        new_subscriber = std::make_shared<Subscriber>();
        new_subscriber->id = subscriber_id;
        new_subscriber->callback = std::move(cb);
        new_subscriber->drain = std::make_shared<SubscriberDrainState>();
    } catch (const std::exception& e) {
        LOG_ERROR("FdReceiver: subscriber allocation failed: %s", e.what());
        return false;
    } catch (...) {
        LOG_ERROR("FdReceiver: subscriber allocation failed");
        return false;
    }

    for (;;) {
        std::shared_ptr<StreamConn> dead_conn;
        {
            std::lock_guard lock(mu_);
            auto it = streams_.find(stream_name);
            if (it != streams_.end()) {
                auto conn = it->second;
                if (conn->running) {
                    std::lock_guard sub_lock(conn->sub_mu);
                    if (conn->subscriber_drains.find(subscriber_id) !=
                        conn->subscriber_drains.end()) {
                        LOG_WARN("FdReceiver: subscriber '%s' already exists or is draining for stream '%s'",
                                 subscriber_id.c_str(), stream_name.c_str());
                        return false;
                    }

                    try {
                        conn->subscriber_drains.emplace(
                            subscriber_id, new_subscriber->drain);
                        try {
                            conn->subscribers.push_back(new_subscriber);
                        } catch (...) {
                            conn->subscriber_drains.erase(subscriber_id);
                            throw;
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("FdReceiver: subscriber insertion failed: %s",
                                  e.what());
                        return false;
                    }
                    conn->teardown_when_idle = false;
                    conn->callback_cv.notify_all();
                    LOG_INFO("FdReceiver: added subscriber '%s' to stream '%s' (total: %zu)",
                             subscriber_id.c_str(), stream_name.c_str(),
                             conn->subscribers.size());
                    return true;
                }

                LOG_WARN("FdReceiver: found dead connection for %s, re-establishing",
                         stream_name.c_str());
                dead_conn = std::move(it->second);
                streams_.erase(it);
            } else {
                if (!setup_stream_connection(stream_name, new_subscriber))
                    return false;
                LOG_INFO("FdReceiver: subscriber '%s' subscribed to new stream '%s'",
                         subscriber_id.c_str(), stream_name.c_str());
                return true;
            }
        }

        // Joining while holding mu_ can deadlock an active callback that calls
        // release_frame(). Reap the dead connection after removing it from the
        // registry, then retry in case another subscriber connected meanwhile.
        teardown_stream_connection(dead_conn);
    }
}

// Legacy API: uses stream_name as subscriber_id (backward compatible)
bool FdReceiver::subscribe(const std::string& stream_name, FrameCallback cb) {
    return subscribe(stream_name, stream_name, std::move(cb));
}

void FdReceiver::unsubscribe(const std::string& stream_name,
                             const std::string& subscriber_id) {
    (void)unsubscribe_until(
        stream_name, subscriber_id,
        std::chrono::steady_clock::time_point::max());
}

bool FdReceiver::unsubscribe_until(
    const std::string& stream_name,
    const std::string& subscriber_id,
    std::chrono::steady_clock::time_point deadline) {
    std::shared_ptr<StreamConn> conn;
    std::shared_ptr<Subscriber> retired_sub;
    std::shared_ptr<SubscriberDrainState> removed_drain;
    bool was_last_subscriber = false;
    bool called_from_recv_thread = false;

    {
        std::unique_lock<std::timed_mutex> lock(mu_, std::defer_lock);
        if (!lock_until(lock, deadline)) return false;
        auto it = streams_.find(stream_name);
        if (it == streams_.end()) return true;

        conn = it->second;
        std::unique_lock<std::timed_mutex> sub_lock(
            conn->sub_mu, std::defer_lock);
        if (!lock_until(sub_lock, deadline)) return false;
        auto& subs = conn->subscribers;
        auto sub_it = std::find_if(subs.begin(), subs.end(),
                                   [&](const auto& sub) { return sub->id == subscriber_id; });
        if (sub_it == subs.end()) {
            auto drain_it = conn->subscriber_drains.find(subscriber_id);
            if (drain_it == conn->subscriber_drains.end()) {
                if (!subs.empty() || !conn->teardown_when_idle) return true;
            } else {
                removed_drain = drain_it->second;
            }
            was_last_subscriber =
                subs.empty() && conn->teardown_when_idle;
        } else {
            retired_sub = *sub_it;
            retired_sub->removed = true;
            removed_drain = retired_sub->drain;
            removed_drain->removed = true;
            subs.erase(sub_it);
            was_last_subscriber = subs.empty();
            if (was_last_subscriber) conn->teardown_when_idle = true;

            LOG_INFO("FdReceiver: removed subscriber '%s' from stream '%s' (remaining: %zu)",
                     subscriber_id.c_str(), stream_name.c_str(), subs.size());
        }
        called_from_recv_thread =
            conn->recv_thread_id == std::this_thread::get_id();
    }
    // Destroy the callback-bearing subscriber only after releasing sub_mu. Its
    // captured state may drop a retained FrameDelivery whose cleanup takes sub_mu.
    retired_sub.reset();

    // A callback cannot wait for itself or join its own receive thread. Removal
    // is deferred and the receive loop tears down after callback/delivery drain,
    // but the deadline-aware API must not report quiescence while still inside it.
    if (called_from_recv_thread) return false;

    {
        std::unique_lock<std::timed_mutex> sub_lock(
            conn->sub_mu, std::defer_lock);
        if (!lock_until(sub_lock, deadline)) return false;

        const auto wait_for_callback = [&](auto&& predicate) {
            if (deadline == std::chrono::steady_clock::time_point::max()) {
                conn->callback_cv.wait(sub_lock, predicate);
                return true;
            }
            return conn->callback_cv.wait_until(
                sub_lock, deadline, predicate);
        };
        if (removed_drain && !wait_for_callback([&] {
                return removed_drain->active_callbacks == 0 &&
                       removed_drain->outstanding_deliveries == 0;
            })) {
            return false;
        }
        if (removed_drain) {
            auto drain_it = conn->subscriber_drains.find(subscriber_id);
            if (drain_it != conn->subscriber_drains.end() &&
                drain_it->second == removed_drain) {
                conn->subscriber_drains.erase(drain_it);
            }
        }

        if (!was_last_subscriber || !conn->teardown_when_idle ||
            !conn->subscribers.empty()) {
            return true;
        }
        if (!wait_for_callback([&] {
                return !conn->teardown_when_idle ||
                       !conn->subscribers.empty() ||
                       (conn->active_callbacks == 0 &&
                        conn->outstanding_frames == 0);
            })) {
            return false;
        }
        if (!conn->teardown_when_idle || !conn->subscribers.empty())
            return true;
    }

    bool should_teardown = false;
    {
        std::unique_lock<std::timed_mutex> lock(mu_, std::defer_lock);
        if (!lock_until(lock, deadline)) return false;
        auto it = streams_.find(stream_name);
        if (it != streams_.end() && it->second == conn) {
            std::unique_lock<std::timed_mutex> sub_lock(
                conn->sub_mu, std::defer_lock);
            if (!lock_until(sub_lock, deadline)) return false;
            if (conn->subscribers.empty() &&
                conn->active_callbacks == 0 &&
                conn->outstanding_frames == 0) {
                conn->running = false;
                should_teardown = true;
            }
        }
    }

    if (!should_teardown) return true;
    if (!teardown_stream_connection_until(conn, deadline)) return false;

    {
        std::unique_lock<std::timed_mutex> lock(mu_, std::defer_lock);
        if (!lock_until(lock, deadline)) return false;
        auto it = streams_.find(stream_name);
        if (it != streams_.end() && it->second == conn)
            streams_.erase(it);
    }
    LOG_INFO("FdReceiver: stream '%s' disconnected (last subscriber left)",
             stream_name.c_str());
    return true;
}

// Legacy API
void FdReceiver::unsubscribe(const std::string& stream_name) {
    unsubscribe(stream_name, stream_name);
}

int FdReceiver::subscriber_count(const std::string& stream_name) const {
    std::lock_guard lock(mu_);
    auto it = streams_.find(stream_name);
    if (it == streams_.end()) return 0;

    std::lock_guard sub_lock(it->second->sub_mu);
    return static_cast<int>(it->second->subscribers.size());
}

bool FdReceiver::stream_connected(const std::string& stream_name) const {
    std::lock_guard lock(mu_);
    auto it = streams_.find(stream_name);
    return it != streams_.end() &&
           it->second->running.load(std::memory_order_acquire);
}

void FdReceiver::stop_all() {
    std::vector<std::shared_ptr<StreamConn>> connections;
    {
        std::lock_guard lock(mu_);
        connections.reserve(streams_.size());
        for (auto& [_, conn] : streams_) {
            connections.push_back(std::move(conn));
        }
        streams_.clear();
    }

    for (const auto& conn : connections) {
        bool called_from_recv_thread = false;
        std::vector<std::shared_ptr<Subscriber>> retired_subscribers;
        {
            std::lock_guard sub_lock(conn->sub_mu);
            for (const auto& sub : conn->subscribers) {
                sub->removed = true;
                sub->drain->removed = true;
            }
            retired_subscribers.swap(conn->subscribers);
            conn->teardown_when_idle = true;
            called_from_recv_thread =
                conn->recv_thread_id == std::this_thread::get_id();
        }
        // Callback destruction may release retained deliveries and re-enter sub_mu.
        retired_subscribers.clear();

        if (!called_from_recv_thread) {
            std::unique_lock sub_lock(conn->sub_mu);
            conn->callback_cv.wait(sub_lock, [&] {
                return conn->active_callbacks == 0 &&
                       conn->outstanding_frames == 0;
            });
            conn->running = false;
        }

        if (called_from_recv_thread) {
            std::lock_guard teardown_lock(conn->teardown_mu);
            if (conn->recv_thread.joinable()) conn->recv_thread.detach();
            continue;
        }
        teardown_stream_connection(conn);
    }
}

FdGroup::~FdGroup() {
    for (int fd : fds) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
}

void FdReceiver::recv_loop(std::shared_ptr<StreamConn> conn) noexcept {
    bool local_failure = false;
    try {
        {
            std::lock_guard sub_lock(conn->sub_mu);
            conn->recv_thread_id = std::this_thread::get_id();
        }
        LOG_DEBUG("FdReceiver: recv_loop started for %s", conn->stream_name.c_str());

        while (conn->running) {
        FdPubFrameMsg frame_msg{};
        int fds[FD_PUB_MAX_FDS] = {-1, -1, -1};
        int num_fds = 0;

        int n = fd_pub_recvmsg(conn->sock_fd, &frame_msg, sizeof(frame_msg),
                               fds, &num_fds, FD_PUB_MAX_FDS);
        if (n <= 0) {
            if (conn->running.exchange(false, std::memory_order_acq_rel)) {
                LOG_WARN("FdReceiver: connection lost for %s", conn->stream_name.c_str());
            }
            break;
        }
        ReceivedFdGuard fd_guard{fds, num_fds};
        if (!conn->running.load(std::memory_order_acquire)) break;

        const bool valid_frame =
            n == static_cast<int>(sizeof(frame_msg)) &&
            frame_msg.hdr.type == FD_PUB_MSG_FRAME &&
            frame_msg.hdr.size == sizeof(frame_msg) &&
            frame_msg.width > 0 && frame_msg.height > 0 &&
            frame_msg.num_planes > 0 &&
            frame_msg.num_planes <= FD_PUB_MAX_FDS &&
            frame_msg.num_fds > 0 &&
            frame_msg.num_fds <= frame_msg.num_planes &&
            frame_msg.num_fds == static_cast<uint32_t>(num_fds);
        if (!valid_frame) {
            LOG_WARN("FdReceiver: rejected malformed frame for %s; disconnecting",
                     conn->stream_name.c_str());
            conn->running = false;
            shutdown(conn->sock_fd, SHUT_RDWR);
            break;
        }

        ReceivedFrame rf{};
        rf.frame_id     = frame_msg.frame_id;
        rf.timestamp_ns = frame_msg.timestamp_ns;
        rf.sequence     = frame_msg.sequence;
        rf.width        = frame_msg.width;
        rf.height       = frame_msg.height;
        rf.format       = frame_msg.format;
        rf.num_planes   = frame_msg.num_planes;

        for (uint32_t i = 0; i < 3; i++) {
            rf.strides[i] = frame_msg.strides[i];
            rf.sizes[i]   = frame_msg.sizes[i];
        }

        // Create reference-counted FD group
        rf.fd_group = std::make_shared<FdGroup>();
        rf.fd_group->fds.reserve(static_cast<size_t>(num_fds));
        for (int i = 0; i < num_fds && i < FD_PUB_MAX_FDS; i++) {
            rf.fd_group->fds.push_back(fds[i]);
            fds[i] = -1;
        }

        auto aggregate = std::make_shared<FrameAckAggregate>();
        aggregate->release = [conn, frame_id = rf.frame_id]() noexcept {
            {
                std::lock_guard send_lock(conn->send_mu);
                if (conn->sock_fd >= 0) {
                    FdPubReleaseMsg rel{};
                    rel.hdr.type = FD_PUB_MSG_RELEASE;
                    rel.hdr.size = sizeof(rel);
                    rel.frame_id = frame_id;
                    if (fd_pub_sendmsg_flags(
                            conn->sock_fd, &rel, sizeof(rel), nullptr, 0,
                            MSG_DONTWAIT) != 0) {
                        conn->running = false;
                        shutdown(conn->sock_fd, SHUT_RDWR);
                        LOG_WARN("FdReceiver: failed to release frame %lu for %s; disconnecting",
                                 (unsigned long)frame_id,
                                 conn->stream_name.c_str());
                    }
                }
            }

            bool stop_when_idle = false;
            {
                std::lock_guard sub_lock(conn->sub_mu);
                if (conn->outstanding_frames > 0)
                    --conn->outstanding_frames;
                stop_when_idle = conn->teardown_when_idle &&
                                 conn->subscribers.empty() &&
                                 conn->active_callbacks == 0 &&
                                 conn->outstanding_frames == 0;
                if (stop_when_idle) conn->running = false;
            }
            conn->callback_cv.notify_all();

            if (stop_when_idle) {
                std::lock_guard send_lock(conn->send_mu);
                if (conn->sock_fd >= 0) {
                    FdPubMsgHeader unsub{};
                    unsub.type = FD_PUB_MSG_UNSUBSCRIBE;
                    unsub.size = sizeof(unsub);
                    (void)fd_pub_sendmsg_flags(
                        conn->sock_fd, &unsub, sizeof(unsub), nullptr, 0,
                        MSG_DONTWAIT);
                    shutdown(conn->sock_fd, SHUT_RDWR);
                }
            }
        };

        // Snapshot subscriber ownership and preallocate every delivery state while
        // holding sub_mu. Registering the physical frame in the same critical
        // section prevents last-subscriber teardown from racing ahead of a frame
        // that has already arrived but has not reached its callback yet.
        std::vector<std::shared_ptr<Subscriber>> subscribers;
        std::vector<std::shared_ptr<FrameDeliveryState>> delivery_states;
        {
            std::lock_guard sub_lock(conn->sub_mu);
            subscribers = conn->subscribers;
            delivery_states.reserve(subscribers.size());
            for (const auto& sub : subscribers) {
                auto state = std::make_shared<FrameDeliveryState>();
                std::weak_ptr<SubscriberDrainState> weak_drain = sub->drain;
                const std::string subscriber_id = sub->id;
                state->on_acknowledge =
                    [conn, weak_drain, subscriber_id] {
                        {
                            std::lock_guard sub_lock(conn->sub_mu);
                            if (auto drain = weak_drain.lock()) {
                                if (drain->outstanding_deliveries > 0)
                                    --drain->outstanding_deliveries;
                                if (drain->removed &&
                                    drain->active_callbacks == 0 &&
                                    drain->outstanding_deliveries == 0) {
                                    auto it = conn->subscriber_drains.find(
                                        subscriber_id);
                                    if (it != conn->subscriber_drains.end() &&
                                        it->second == drain) {
                                        conn->subscriber_drains.erase(it);
                                    }
                                }
                            }
                        }
                        conn->callback_cv.notify_all();
                    };
                delivery_states.push_back(std::move(state));
            }

            ++conn->outstanding_frames;
            aggregate->remaining.store(
                subscribers.empty() ? 1 : subscribers.size(),
                std::memory_order_release);
            for (size_t i = 0; i < delivery_states.size(); ++i) {
                ++subscribers[i]->drain->outstanding_deliveries;
                delivery_states[i]->aggregate = aggregate;
                delivery_states[i]->registered.store(
                    true, std::memory_order_release);
            }
        }

        if (subscribers.empty()) {
            aggregate->acknowledge();
            continue;
        }

        for (size_t i = 0; i < subscribers.size(); ++i) {
            const auto& sub = subscribers[i];
            ReceivedFrame delivered = rf;
            delivered.delivery = FrameDelivery(delivery_states[i]);

            bool should_invoke = false;
            {
                std::lock_guard sub_lock(conn->sub_mu);
                if (!sub->removed) {
                    ++sub->drain->active_callbacks;
                    ++conn->active_callbacks;
                    should_invoke = true;
                }
            }

            if (!should_invoke) {
                delivered.delivery.acknowledge();
                continue;
            }

            try {
                sub->callback(delivered);
            } catch (const std::exception& e) {
                LOG_ERROR("FdReceiver: callback '%s' for stream '%s' threw: %s",
                          sub->id.c_str(), conn->stream_name.c_str(), e.what());
            } catch (...) {
                LOG_ERROR("FdReceiver: callback '%s' for stream '%s' threw",
                          sub->id.c_str(), conn->stream_name.c_str());
            }
            // Drop only this callback's delivery copy. A subscriber may have
            // retained another copy for asynchronous DMA access; the shared
            // state acknowledges only when that final copy is released.
            delivered.delivery = FrameDelivery{};

            bool stop_when_idle = false;
            {
                std::lock_guard sub_lock(conn->sub_mu);
                --sub->drain->active_callbacks;
                --conn->active_callbacks;
                if (sub->drain->removed &&
                    sub->drain->active_callbacks == 0 &&
                    sub->drain->outstanding_deliveries == 0) {
                    auto it = conn->subscriber_drains.find(sub->id);
                    if (it != conn->subscriber_drains.end() &&
                        it->second == sub->drain) {
                        conn->subscriber_drains.erase(it);
                    }
                }
                stop_when_idle = conn->teardown_when_idle &&
                                 conn->subscribers.empty() &&
                                 conn->active_callbacks == 0 &&
                                 conn->outstanding_frames == 0;
                if (stop_when_idle) conn->running = false;
                conn->callback_cv.notify_all();
            }

            if (stop_when_idle) {
                std::lock_guard send_lock(conn->send_mu);
                if (conn->sock_fd >= 0) {
                    FdPubMsgHeader unsub{};
                    unsub.type = FD_PUB_MSG_UNSUBSCRIBE;
                    unsub.size = sizeof(unsub);
                    (void)fd_pub_sendmsg_flags(
                        conn->sock_fd, &unsub, sizeof(unsub), nullptr, 0,
                        MSG_DONTWAIT);
                    shutdown(conn->sock_fd, SHUT_RDWR);
                }
            }
        }
    }
    } catch (const std::exception& e) {
        local_failure = true;
        LOG_ERROR("FdReceiver: receive loop for %s failed: %s",
                  conn->stream_name.c_str(), e.what());
    } catch (...) {
        local_failure = true;
        LOG_ERROR("FdReceiver: receive loop for %s failed",
                  conn->stream_name.c_str());
    }

    if (local_failure) {
        conn->running = false;
        std::vector<std::shared_ptr<Subscriber>> retired_subscribers;
        {
            std::lock_guard sub_lock(conn->sub_mu);
            for (const auto& sub : conn->subscribers) {
                sub->removed = true;
                sub->drain->removed = true;
            }
            retired_subscribers.swap(conn->subscribers);
            conn->teardown_when_idle = true;
        }
        retired_subscribers.clear();

        std::unique_lock sub_lock(conn->sub_mu);
        conn->callback_cv.wait(sub_lock, [&] {
            return conn->active_callbacks == 0 &&
                   conn->outstanding_frames == 0;
        });
    }

    conn->running = false;
    conn->callback_cv.notify_all();
    try {
        std::lock_guard send_lock(conn->send_mu);
        if (conn->sock_fd >= 0) {
            ::close(conn->sock_fd);
            conn->sock_fd = -1;
        }
    } catch (...) {
        LOG_ERROR("FdReceiver: socket cleanup failed for %s",
                  conn->stream_name.c_str());
    }
    LOG_DEBUG("FdReceiver: recv_loop ended for %s", conn->stream_name.c_str());
    {
        std::lock_guard exit_lock(conn->exit_mu);
        conn->exited = true;
    }
    conn->exit_cv.notify_all();
}

}  // namespace aipc::ai_runtime
