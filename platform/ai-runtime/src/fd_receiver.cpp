#include "fd_receiver.h"
#include "log.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

namespace aipc::ai_runtime {

FdReceiver::FdReceiver(const std::string& socket_path)
    : socket_path_(socket_path) {}

FdReceiver::~FdReceiver() {
    stop_all();
}

int FdReceiver::connect_to_server() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("FdReceiver: socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("FdReceiver: connect(%s) failed: %s",
                  socket_path_.c_str(), strerror(errno));
        ::close(fd);
        return -1;
    }

    return fd;
}

// Establish the physical connection + recv thread for a stream (called under mu_).
bool FdReceiver::setup_stream_connection(const std::string& stream_name) {
    int sock_fd = connect_to_server();
    if (sock_fd < 0) return false;

    // Send SUBSCRIBE message
    FdPubSubscribeMsg sub_msg{};
    sub_msg.hdr.type = FD_PUB_MSG_SUBSCRIBE;
    sub_msg.hdr.size = sizeof(sub_msg);
    sub_msg.version  = FD_PUB_PROTOCOL_VERSION;
    std::strncpy(sub_msg.stream_name, stream_name.c_str(), FD_PUB_MAX_STREAM_NAME - 1);

    if (fd_pub_sendmsg(sock_fd, &sub_msg, sizeof(sub_msg), nullptr, 0) != 0) {
        LOG_ERROR("FdReceiver: failed to send SUBSCRIBE for %s", stream_name.c_str());
        ::close(sock_fd);
        return false;
    }

    // Wait for OK response
    FdPubResponseMsg resp{};
    int recv_fds[FD_PUB_MAX_FDS];
    int num_recv_fds = 0;
    int n = fd_pub_recvmsg(sock_fd, &resp, sizeof(resp), recv_fds, &num_recv_fds, FD_PUB_MAX_FDS);
    if (n <= 0 || resp.hdr.type != FD_PUB_MSG_OK) {
        LOG_ERROR("FdReceiver: SUBSCRIBE rejected for %s (type=%u code=%d)",
                  stream_name.c_str(),
                  n > 0 ? resp.hdr.type : 0,
                  n > 0 ? resp.code : -1);
        ::close(sock_fd);
        return false;
    }

    auto conn = std::make_unique<StreamConn>();
    conn->stream_name = stream_name;
    conn->sock_fd     = sock_fd;
    conn->running     = true;

    auto* conn_ptr = conn.get();
    conn->recv_thread = std::thread(&FdReceiver::recv_loop, this, conn_ptr);

    streams_.emplace(stream_name, std::move(conn));

    LOG_INFO("FdReceiver: stream connection established for %s", stream_name.c_str());
    return true;
}

// Tear down the physical connection for a stream.
void FdReceiver::teardown_stream_connection(StreamConn* conn) {
    conn->running = false;

    // Send UNSUBSCRIBE
    FdPubMsgHeader unsub{};
    unsub.type = FD_PUB_MSG_UNSUBSCRIBE;
    unsub.size = sizeof(unsub);
    fd_pub_sendmsg(conn->sock_fd, &unsub, sizeof(unsub), nullptr, 0);

    shutdown(conn->sock_fd, SHUT_RDWR);
    if (conn->recv_thread.joinable()) conn->recv_thread.join();
    ::close(conn->sock_fd);
}

bool FdReceiver::subscribe(const std::string& stream_name,
                           const std::string& subscriber_id,
                           FrameCallback cb) {
    std::lock_guard lock(mu_);

    auto it = streams_.find(stream_name);
    if (it != streams_.end()) {
        auto* conn = it->second.get();
        if (conn->running) {
            // Stream already has a physical connection — just add a subscriber
            std::lock_guard sub_lock(conn->sub_mu);

            // Check for duplicate subscriber_id
            for (const auto& sub : conn->subscribers) {
                if (sub.id == subscriber_id) {
                    LOG_WARN("FdReceiver: subscriber '%s' already exists for stream '%s'",
                             subscriber_id.c_str(), stream_name.c_str());
                    return false;
                }
            }

            conn->subscribers.push_back({subscriber_id, std::move(cb)});
            LOG_INFO("FdReceiver: added subscriber '%s' to stream '%s' (total: %zu)",
                     subscriber_id.c_str(), stream_name.c_str(), conn->subscribers.size());
            return true;
        } else {
            // Physical connection is dead, remove it so we can re-establish
            LOG_WARN("FdReceiver: found dead connection for %s, re-establishing", 
                     stream_name.c_str());
            teardown_stream_connection(conn); // Join thread if needed
            streams_.erase(it);
        }
    }

    // No existing connection — establish one
    if (!setup_stream_connection(stream_name)) {
        return false;
    }

    // Add the first subscriber
    auto* conn = streams_[stream_name].get();
    std::lock_guard sub_lock(conn->sub_mu);
    conn->subscribers.push_back({subscriber_id, std::move(cb)});

    LOG_INFO("FdReceiver: subscriber '%s' subscribed to new stream '%s'",
             subscriber_id.c_str(), stream_name.c_str());
    return true;
}

// Legacy API: uses stream_name as subscriber_id (backward compatible)
bool FdReceiver::subscribe(const std::string& stream_name, FrameCallback cb) {
    return subscribe(stream_name, stream_name, std::move(cb));
}

void FdReceiver::unsubscribe(const std::string& stream_name,
                             const std::string& subscriber_id) {
    std::unique_lock lock(mu_);

    auto it = streams_.find(stream_name);
    if (it == streams_.end()) return;

    auto* conn = it->second.get();
    bool should_teardown = false;

    {
        std::lock_guard sub_lock(conn->sub_mu);

        // Remove the subscriber
        auto& subs = conn->subscribers;
        subs.erase(std::remove_if(subs.begin(), subs.end(),
                                  [&](const Subscriber& s) { return s.id == subscriber_id; }),
                   subs.end());

        LOG_INFO("FdReceiver: removed subscriber '%s' from stream '%s' (remaining: %zu)",
                 subscriber_id.c_str(), stream_name.c_str(), subs.size());

        // If no subscribers left, tear down the physical connection
        should_teardown = subs.empty();
    }

    if (should_teardown) {
        auto owned_conn = std::move(it->second);
        streams_.erase(it);
        lock.unlock();  // release mu_ before blocking on thread join

        teardown_stream_connection(owned_conn.get());
        LOG_INFO("FdReceiver: stream '%s' disconnected (last subscriber left)",
                 stream_name.c_str());
    }
}

// Legacy API
void FdReceiver::unsubscribe(const std::string& stream_name) {
    unsubscribe(stream_name, stream_name);
}

void FdReceiver::ref_frame(const std::string& stream_name, uint64_t frame_id) {
    std::lock_guard lock(frame_ref_mu_);
    uint64_t key = frame_ref_key(stream_name, frame_id);
    frame_refs_[key]++;
}

void FdReceiver::release_frame(const std::string& stream_name, uint64_t frame_id) {
    bool should_release = false;
    {
        std::lock_guard lock(frame_ref_mu_);
        uint64_t key = frame_ref_key(stream_name, frame_id);
        auto it = frame_refs_.find(key);
        if (it != frame_refs_.end()) {
            it->second--;
            if (it->second <= 0) {
                frame_refs_.erase(it);
                should_release = true;
            }
        } else {
            // No refcount entry — single subscriber or legacy path
            should_release = true;
        }
    }

    if (should_release) {
        std::lock_guard lock(mu_);
        auto it = streams_.find(stream_name);
        if (it == streams_.end()) return;

        FdPubReleaseMsg rel{};
        rel.hdr.type = FD_PUB_MSG_RELEASE;
        rel.hdr.size = sizeof(rel);
        rel.frame_id = frame_id;

        fd_pub_sendmsg(it->second->sock_fd, &rel, sizeof(rel), nullptr, 0);
    }
}

int FdReceiver::subscriber_count(const std::string& stream_name) const {
    std::lock_guard lock(mu_);
    auto it = streams_.find(stream_name);
    if (it == streams_.end()) return 0;

    std::lock_guard sub_lock(it->second->sub_mu);
    return static_cast<int>(it->second->subscribers.size());
}

void FdReceiver::stop_all() {
    std::vector<std::string> names;
    {
        std::lock_guard lock(mu_);
        for (auto& [name, _] : streams_) {
            names.push_back(name);
        }
    }
    // Collect all subscriber IDs and unsubscribe each
    for (auto& name : names) {
        std::vector<std::string> sub_ids;
        {
            std::lock_guard lock(mu_);
            auto it = streams_.find(name);
            if (it == streams_.end()) continue;
            std::lock_guard sub_lock(it->second->sub_mu);
            for (const auto& s : it->second->subscribers) {
                sub_ids.push_back(s.id);
            }
        }
        for (const auto& sid : sub_ids) {
            unsubscribe(name, sid);
        }
    }
}

FdGroup::~FdGroup() {
    for (int fd : fds) {
        if (fd >= 0) {
            ::close(fd);
        }
    }
}

void FdReceiver::recv_loop(StreamConn* conn) {
    LOG_DEBUG("FdReceiver: recv_loop started for %s", conn->stream_name.c_str());

    while (conn->running) {
        FdPubFrameMsg frame_msg{};
        int fds[FD_PUB_MAX_FDS] = {-1, -1, -1};
        int num_fds = 0;

        int n = fd_pub_recvmsg(conn->sock_fd, &frame_msg, sizeof(frame_msg),
                               fds, &num_fds, FD_PUB_MAX_FDS);
        if (n <= 0) {
            if (conn->running) {
                LOG_WARN("FdReceiver: connection lost for %s", conn->stream_name.c_str());
            }
            break;
        }

        if (frame_msg.hdr.type != FD_PUB_MSG_FRAME) continue;

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
        for (int i = 0; i < num_fds && i < FD_PUB_MAX_FDS; i++) {
            rf.fd_group->fds.push_back(fds[i]);
        }

        // Fan-out: deliver frame to ALL subscribers
        {
            std::lock_guard sub_lock(conn->sub_mu);
            size_t num_subs = conn->subscribers.size();
            if (num_subs > 1) {
                // Ref-count the frame for N subscribers so release_frame
                // only sends RELEASE to camera-daemon when the last one calls it.
                std::lock_guard ref_lock(frame_ref_mu_);
                uint64_t key = frame_ref_key(conn->stream_name, rf.frame_id);
                frame_refs_[key] = static_cast<int>(num_subs);
            }
            for (const auto& sub : conn->subscribers) {
                sub.callback(rf);
            }
        }
    }

    // Cleanup: remove from streams_ map so it can be re-established
    {
        std::lock_guard lock(mu_);
        auto it = streams_.find(conn->stream_name);
        if (it != streams_.end() && it->second.get() == conn) {
             // In a real system we'd need to be careful about joining here,
             // but recv_loop is the one ending, and the map erase will
             // eventually trigger teardown if not already done.
             // For now, just mark it as not running so subscribe() can detect it.
             conn->running = false;
             // streams_.erase(it); // Dangerous while in recv_loop
        }
    }

    LOG_DEBUG("FdReceiver: recv_loop ended for %s", conn->stream_name.c_str());
}

}  // namespace aipc::ai_runtime
