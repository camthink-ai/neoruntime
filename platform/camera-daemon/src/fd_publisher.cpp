/**
 * @file fd_publisher.cpp
 * @brief FD Publisher Implementation - Zero-copy DMA-BUF FD delivery
 */

#include "../include/fd_publisher.h"
#include "../include/fd_protocol.h"
#include "../include/frame_router.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

extern "C" {
    #include "hal_log.h"
}

FdPublisher::FdPublisher(FrameRouter* router, const FdPublisherConfig& config)
    : router_(router), config_(config) {}

FdPublisher::~FdPublisher() {
    stop();
}

bool FdPublisher::start() {
    if (running_.load()) return true;

    // Create UDS server socket
    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        HAL_LOG_ERROR("FdPublisher: socket() failed: %s", strerror(errno));
        return false;
    }

    // Remove stale socket file
    unlink(config_.sock_path.c_str());

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, config_.sock_path.c_str(),
            sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        HAL_LOG_ERROR("FdPublisher: bind(%s) failed: %s",
                     config_.sock_path.c_str(), strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Permissions: owner + group read/write (App containers run in same group)
    chmod(config_.sock_path.c_str(), 0660);
    chown(config_.sock_path.c_str(), -1, 1001);  // aipc group GID

    if (listen(server_fd_, 8) < 0) {
        HAL_LOG_ERROR("FdPublisher: listen() failed: %s", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_.store(true);
    accept_thread_ = std::thread(&FdPublisher::accept_loop, this);

    HAL_LOG_INFO("FdPublisher: Listening on %s (max_clients=%u, max_outstanding=%u)",
                 config_.sock_path.c_str(), config_.max_clients,
                 config_.max_outstanding_per_client);
    return true;
}

void FdPublisher::stop() {
    if (!running_.exchange(false)) return;

    // Close server socket to unblock accept()
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // Disconnect all clients
    std::vector<int> client_fds;
    {
        std::lock_guard<std::mutex> lock(clients_mu_);
        for (auto& [fd, _] : clients_) {
            client_fds.push_back(fd);
        }
    }
    for (int fd : client_fds) {
        disconnect_client(fd);
    }

    unlink(config_.sock_path.c_str());
    HAL_LOG_INFO("FdPublisher: Stopped");
}

void FdPublisher::on_frame(const std::string& stream_name, ManagedFrame* mf) {
    // Collect clients subscribed to this stream
    std::vector<ClientState*> targets;

    {
        std::lock_guard<std::mutex> lock(clients_mu_);
        for (auto& [fd, client] : clients_) {
            if (client->subscribed && client->stream_name == stream_name) {
                targets.push_back(client);
            }
        }
    }

    if (targets.empty()) {
        // No FD clients for this stream — release our ref immediately
        router_->release(mf);
        return;
    }

    // For each target client:
    //   retain(mf) → bump ref count → send FD → track in outstanding
    int sent_count = 0;

    for (auto* client : targets) {
        // Check outstanding limit
        {
            std::lock_guard<std::mutex> lock(client->outstanding_mu);
            if (client->outstanding.size() >= config_.max_outstanding_per_client) {
                // Client too slow — drop frame for this client
                std::lock_guard<std::mutex> sl(stats_mu_);
                stats_.frames_dropped++;
                continue;
            }
        }

        // Bump ref count before sending
        router_->retain(mf);

        if (send_frame_to_client(client, mf)) {
            // Track in outstanding
            std::lock_guard<std::mutex> lock(client->outstanding_mu);
            client->outstanding[mf->frame_id] = mf;
            sent_count++;

            std::lock_guard<std::mutex> sl(stats_mu_);
            stats_.frames_sent++;
        } else {
            // Send failed — release the retained ref
            router_->release(mf);

            std::lock_guard<std::mutex> sl(stats_mu_);
            stats_.send_errors++;
        }
    }

    // Release our original ref (from FrameRouter subscription)
    router_->release(mf);
}

uint32_t FdPublisher::client_count() const {
    std::lock_guard<std::mutex> lock(clients_mu_);
    return clients_.size();
}

uint32_t FdPublisher::stream_client_count(const std::string& stream_name) const {
    std::lock_guard<std::mutex> lock(clients_mu_);
    uint32_t count = 0;
    for (auto& [fd, client] : clients_) {
        if (client->subscribed && client->stream_name == stream_name) {
            count++;
        }
    }
    return count;
}

FdPublisher::Stats FdPublisher::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mu_);
    return stats_;
}

/* ========== Private methods ========== */

void FdPublisher::accept_loop() {
    while (running_.load()) {
        int client_fd = accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (running_.load()) {
                HAL_LOG_ERROR("FdPublisher: accept() failed: %s", strerror(errno));
            }
            break;
        }

        // Check max clients
        {
            std::lock_guard<std::mutex> lock(clients_mu_);
            if (clients_.size() >= config_.max_clients) {
                HAL_LOG_WARNING("FdPublisher: Max clients reached, rejecting");
                close(client_fd);
                continue;
            }
        }

        // Set send non-blocking (frame delivery must not block HAL callback)
        int flags = fcntl(client_fd, F_GETFL, 0);
        // Keep recv blocking (client recv thread blocks on recv)
        // Send will use MSG_NOSIGNAL | MSG_DONTWAIT in sendmsg

        auto* client = new ClientState();
        client->fd = client_fd;

        {
            std::lock_guard<std::mutex> lock(clients_mu_);
            clients_[client_fd] = client;
        }

        // Start recv thread for this client
        client->recv_thread = std::thread(&FdPublisher::client_recv_loop,
                                           this, client);

        {
            std::lock_guard<std::mutex> lock(stats_mu_);
            stats_.clients_connected++;
        }

        HAL_LOG_INFO("FdPublisher: Client connected (fd=%d, total=%u)",
                     client_fd, client_count());
    }
}

void FdPublisher::client_recv_loop(ClientState* client) {
    while (running_.load()) {
        // Read message header first
        FdPubMsgHeader hdr;
        ssize_t n = recv(client->fd, &hdr, sizeof(hdr), MSG_WAITALL);
        if (n <= 0) {
            // Client disconnected
            break;
        }

        if (n != sizeof(hdr)) {
            HAL_LOG_WARNING("FdPublisher: Partial header from client fd=%d",
                           client->fd);
            break;
        }

        // Read remaining payload
        size_t payload_size = hdr.size - sizeof(hdr);

        switch (hdr.type) {
        case FD_PUB_MSG_SUBSCRIBE: {
            if (payload_size != sizeof(FdPubSubscribeMsg) - sizeof(hdr)) {
                HAL_LOG_WARNING("FdPublisher: Bad SUBSCRIBE size from fd=%d",
                               client->fd);
                break;
            }
            // Read the rest of the subscribe message
            char buf[sizeof(FdPubSubscribeMsg)];
            memcpy(buf, &hdr, sizeof(hdr));
            n = recv(client->fd, buf + sizeof(hdr), payload_size, MSG_WAITALL);
            if (n != (ssize_t)payload_size) break;

            handle_subscribe(client, buf);
            break;
        }

        case FD_PUB_MSG_RELEASE: {
            if (payload_size != sizeof(FdPubReleaseMsg) - sizeof(hdr)) break;

            char buf[sizeof(FdPubReleaseMsg)];
            memcpy(buf, &hdr, sizeof(hdr));
            n = recv(client->fd, buf + sizeof(hdr), payload_size, MSG_WAITALL);
            if (n != (ssize_t)payload_size) break;

            handle_release(client, buf);
            break;
        }

        case FD_PUB_MSG_UNSUBSCRIBE: {
            client->subscribed = false;
            HAL_LOG_INFO("FdPublisher: Client fd=%d unsubscribed from %s",
                        client->fd, client->stream_name.c_str());
            // Send OK
            FdPubResponseMsg resp;
            resp.hdr.type = FD_PUB_MSG_OK;
            resp.hdr.size = sizeof(resp);
            resp.code = 0;
            send(client->fd, &resp, sizeof(resp), MSG_NOSIGNAL);
            break;
        }

        default:
            HAL_LOG_WARNING("FdPublisher: Unknown msg type %u from fd=%d",
                           hdr.type, client->fd);
            // Drain unknown payload
            if (payload_size > 0 && payload_size < 4096) {
                char drain[4096];
                recv(client->fd, drain, payload_size, MSG_WAITALL);
            }
            break;
        }
    }

    // Client disconnected — clean up
    HAL_LOG_INFO("FdPublisher: Client fd=%d disconnected", client->fd);
    disconnect_client(client->fd);
}

void FdPublisher::handle_subscribe(ClientState* client, const void* msg_data) {
    auto* msg = static_cast<const FdPubSubscribeMsg*>(msg_data);

    char name[FD_PUB_MAX_STREAM_NAME + 1] = {};
    memcpy(name, msg->stream_name, FD_PUB_MAX_STREAM_NAME);

    client->stream_name = name;
    client->subscribed = true;

    FdPubResponseMsg resp;
    resp.hdr.type = FD_PUB_MSG_OK;
    resp.hdr.size = sizeof(resp);
    resp.code = 0;
    send(client->fd, &resp, sizeof(resp), MSG_NOSIGNAL);

    HAL_LOG_INFO("FdPublisher: Client fd=%d subscribed to stream [%s]",
                 client->fd, name);
}

void FdPublisher::handle_release(ClientState* client, const void* msg_data) {
    auto* msg = static_cast<const FdPubReleaseMsg*>(msg_data);
    uint64_t frame_id = msg->frame_id;

    ManagedFrame* mf = nullptr;
    {
        std::lock_guard<std::mutex> lock(client->outstanding_mu);
        auto it = client->outstanding.find(frame_id);
        if (it == client->outstanding.end()) {
            HAL_LOG_WARNING("FdPublisher: RELEASE for unknown frame_id=%lu from fd=%d",
                           frame_id, client->fd);
            return;
        }
        mf = it->second;
        client->outstanding.erase(it);
    }

    // Release the ref we retained for this client
    if (mf && router_) {
        router_->release(mf);
    }
}

void FdPublisher::disconnect_client(int client_fd) {
    ClientState* client = nullptr;

    {
        std::lock_guard<std::mutex> lock(clients_mu_);
        auto it = clients_.find(client_fd);
        if (it == clients_.end()) return;
        client = it->second;
        clients_.erase(it);
    }

    if (!client) return;

    // Release all outstanding frames
    release_all_outstanding(client);

    // Close socket (will unblock recv in client_recv_loop)
    shutdown(client->fd, SHUT_RDWR);
    close(client->fd);

    // Join recv thread if it's not the current thread
    if (client->recv_thread.joinable() &&
        client->recv_thread.get_id() != std::this_thread::get_id()) {
        client->recv_thread.join();
    } else if (client->recv_thread.joinable()) {
        client->recv_thread.detach();
    }

    {
        std::lock_guard<std::mutex> lock(stats_mu_);
        stats_.clients_disconnected++;
    }

    delete client;
}

void FdPublisher::release_all_outstanding(ClientState* client) {
    std::lock_guard<std::mutex> lock(client->outstanding_mu);

    if (!client->outstanding.empty()) {
        HAL_LOG_WARNING("FdPublisher: Releasing %zu outstanding frames for fd=%d",
                       client->outstanding.size(), client->fd);
    }

    for (auto& [frame_id, mf] : client->outstanding) {
        if (mf && router_) {
            router_->release(mf);
        }
    }
    client->outstanding.clear();
}

bool FdPublisher::send_frame_to_client(ClientState* client, ManagedFrame* mf) {
    const HalFrameBuffer& frame = mf->frame;

    // Build frame message
    FdPubFrameMsg msg;
    memset(&msg, 0, sizeof(msg));
    msg.hdr.type = FD_PUB_MSG_FRAME;
    msg.hdr.size = sizeof(msg);
    msg.frame_id = mf->frame_id;
    msg.timestamp_ns = frame.timestamp_ns;
    msg.sequence = frame.sequence;
    msg.width = frame.width;
    msg.height = frame.height;
    msg.format = frame.format;
    msg.num_planes = frame.num_planes;

    for (uint32_t i = 0; i < frame.num_planes && i < 3; i++) {
        msg.strides[i] = frame.strides[i];
        msg.sizes[i] = frame.sizes[i];
    }

    // Collect valid DMA-BUF fds
    int fds[FD_PUB_MAX_FDS];
    int num_fds = 0;

    if (frame.mem_type == HAL_MEM_DMABUF) {
        for (uint32_t i = 0; i < frame.num_planes && i < 3; i++) {
            if (frame.dma_fds[i] >= 0) {
                fds[num_fds++] = frame.dma_fds[i];
            }
        }
    }

    msg.num_fds = num_fds;

    if (num_fds == 0) {
        // No DMA-BUF fds — this frame type doesn't support FD passing
        HAL_LOG_WARNING("FdPublisher: Frame has no DMA-BUF fds, cannot send to fd=%d",
                       client->fd);
        return false;
    }

    // Send via SCM_RIGHTS (non-blocking via MSG_NOSIGNAL in fd_pub_sendmsg)
    return fd_pub_sendmsg(client->fd, &msg, sizeof(msg), fds, num_fds) == 0;
}
