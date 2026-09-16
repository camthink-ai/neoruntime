/**
 * @file fd_publisher.cpp
 * @brief FD Publisher Implementation - Zero-copy DMA-BUF FD delivery
 */

#include "../include/fd_publisher.h"
#include "../include/fd_protocol.h"
#include "../include/frame_router.h"
#include "../include/dsp_service.h"

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
    // The whole dispatch pass runs under clients_mu_ (sends are non-blocking,
    // so the hold is bounded):
    //   - a client erased by disconnect_client() cannot be freed, and its fd
    //     cannot be closed/recycled, mid-iteration (the old collect-then-
    //     unlock loop was a use-after-free: SIGSEGV under connect churn),
    //   - disconnect's release_all_outstanding() cannot interleave with the
    //     outstanding insert below, which would leak the retained ref.
    std::vector<int> desync_fds;

    {
        std::lock_guard<std::mutex> lock(clients_mu_);

        for (auto& [fd, client] : clients_) {
            if (!client->subscribed || client->stream_name != stream_name) {
                continue;
            }

            // Retain + track BEFORE sendmsg. A RELEASE can only name a frame
            // the client already received — this frame is not on the wire
            // yet, so nothing can release this entry between insert and
            // send. With the old send-then-track order, a RELEASE landing
            // in that gap was discarded as "unknown" and pinned one of the
            // max_outstanding slots until disconnect (permanent delivery
            // stall for that client, no negative ack to detect it).
            bool tracked = false;
            {
                std::lock_guard<std::mutex> ol(client->outstanding_mu);
                if (client->outstanding.size() < config_.max_outstanding_per_client) {
                    router_->retain(mf);   // ref first: never publish an un-retained entry
                    client->outstanding[mf->frame_id] = mf;
                    tracked = true;
                }
            }

            if (!tracked) {
                // Client too slow — drop frame for this client
                std::lock_guard<std::mutex> sl(stats_mu_);
                stats_.frames_dropped++;
                continue;
            }

            switch (send_frame_to_client(client, mf)) {
            case FrameSendResult::kOk: {
                std::lock_guard<std::mutex> sl(stats_mu_);
                stats_.frames_sent++;
                break;
            }

            case FrameSendResult::kSlowClient:
                // EAGAIN: nothing was queued and no fds crossed — the client
                // is behind. Drop this frame, keep the connection.
                erase_outstanding(client, mf->frame_id);
                router_->release(mf);
                {
                    std::lock_guard<std::mutex> sl(stats_mu_);
                    stats_.frames_dropped++;
                }
                break;

            case FrameSendResult::kHardError:
                // Hard error, or a partial send: with SCM_RIGHTS the fds
                // cross with the first byte, so a partial send desyncs the
                // client's stream — drop it after the pass.
                erase_outstanding(client, mf->frame_id);
                router_->release(mf);
                {
                    std::lock_guard<std::mutex> sl(stats_mu_);
                    stats_.send_errors++;
                }
                desync_fds.push_back(fd);
                break;

            case FrameSendResult::kUndeliverable:
                // Frame carries no dma-buf fds — drop it for this client,
                // the connection itself is fine.
                erase_outstanding(client, mf->frame_id);
                router_->release(mf);
                {
                    std::lock_guard<std::mutex> sl(stats_mu_);
                    stats_.send_errors++;
                }
                break;
            }
        }
    }

    // disconnect_client() takes clients_mu_ itself — call it only after the
    // dispatch pass released the lock (we are on the router dispatch thread,
    // so joining the client's recv thread here cannot self-join).
    for (int fd : desync_fds) {
        HAL_LOG_WARNING("FdPublisher: Dropping desynced client fd=%d", fd);
        disconnect_client(fd);
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

void FdPublisher::set_dsp_service(DspService* dsp_service) {
    dsp_service_ = dsp_service;
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

        // Recv stays blocking (client recv threads use blocking recv). Frame
        // delivery on the dispatch thread sends with a per-call MSG_DONTWAIT
        // (see send_frame_to_client): O_NONBLOCK on the fd would break the
        // blocking recv loops, so it must stay per-sendmsg.

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
        /* Read message header first — with recvmsg, never plain recv. On a
         * stream socket SCM_RIGHTS rides with the FIRST byte of the sender's
         * sendmsg, which is the header: a plain recv consuming that byte
         * silently drops the ancillary record (the peer's DSP_IMPORT arrives
         * with fds=0 and the fds leak kernel-side). Every loop iteration
         * owns whatever lands here — only handle_dsp_import consumes (it
         * closes them itself); everything else is closed after the switch. */
        FdPubMsgHeader hdr;
        int msg_fds[FD_PUB_MAX_FDS];
        int num_msg_fds = 0;
        size_t hdr_got = 0;
        bool disconnected = false;
        while (hdr_got < sizeof(hdr)) {
            int chunk_fds[FD_PUB_MAX_FDS];
            int n_chunk_fds = 0;
            ssize_t n = fd_pub_recvmsg(client->fd, (char*)&hdr + hdr_got,
                                       sizeof(hdr) - hdr_got, chunk_fds,
                                       &n_chunk_fds, FD_PUB_MAX_FDS);
            if (n <= 0) {
                /* disconnect — we still own fds captured by earlier chunks */
                for (int i = 0; i < num_msg_fds; ++i) close(msg_fds[i]);
                num_msg_fds = 0;
                disconnected = true;
                break;
            }
            for (int i = 0; i < n_chunk_fds; ++i) {
                if (num_msg_fds < FD_PUB_MAX_FDS) {
                    msg_fds[num_msg_fds++] = chunk_fds[i];
                } else {
                    close(chunk_fds[i]); /* over the wire cap — don't leak */
                }
            }
            hdr_got += (size_t)n;
        }
        if (disconnected) {
            // Client disconnected
            break;
        }

        bool fds_consumed = false;
        ssize_t n = 0; /* payload read length for the switch below */

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

        case FD_PUB_MSG_DSP_ALLOC: {
            if (payload_size != sizeof(FdPubDspAllocMsg) - sizeof(hdr)) break;

            char buf[sizeof(FdPubDspAllocMsg)];
            memcpy(buf, &hdr, sizeof(hdr));
            n = recv(client->fd, buf + sizeof(hdr), payload_size, MSG_WAITALL);
            if (n != (ssize_t)payload_size) break;

            handle_dsp_alloc(client, buf);
            break;
        }

        case FD_PUB_MSG_DSP_BUF_RELEASE: {
            if (payload_size != sizeof(FdPubDspBufReleaseMsg) - sizeof(hdr)) break;

            char buf[sizeof(FdPubDspBufReleaseMsg)];
            memcpy(buf, &hdr, sizeof(hdr));
            n = recv(client->fd, buf + sizeof(hdr), payload_size, MSG_WAITALL);
            if (n != (ssize_t)payload_size) break;

            handle_dsp_buf_release(client, buf);
            break;
        }

        case FD_PUB_MSG_DSP_IMPORT: {
            if (payload_size != sizeof(FdPubDspImportMsg) - sizeof(hdr)) {
                break; /* fds (if any) are closed after the switch */
            }
            /* The dma-buf fds crossed the stream boundary with the header
             * (captured in msg_fds above); the payload itself carries no
             * ancillary data — a plain blocking read is enough. */
            char buf[sizeof(FdPubDspImportMsg)];
            memcpy(buf, &hdr, sizeof(hdr));
            n = recv(client->fd, buf + sizeof(hdr), payload_size, MSG_WAITALL);
            if (n != (ssize_t)payload_size) {
                HAL_LOG_WARNING("FdPublisher: short DSP IMPORT read from fd=%d",
                                client->fd);
                break;
            }

            handle_dsp_import(client, buf, msg_fds, num_msg_fds);
            fds_consumed = true; /* handler closes every fd it was given */
            break;
        }

        case FD_PUB_MSG_UNSUBSCRIBE: {
            // subscribed/stream_name are read on the dispatch thread under
            // clients_mu_ — flip the flag under the same lock.
            {
                std::lock_guard<std::mutex> lock(clients_mu_);
                client->subscribed = false;
            }
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

        /* Ownership fence: fds that arrived with this message but were not
         * handed to handle_dsp_import (any non-IMPORT message, or an IMPORT
         * rejected on payload size) are closed here — a received fd belongs
         * to this process from recvmsg onward, no path may leak it. */
        if (!fds_consumed) {
            for (int i = 0; i < num_msg_fds; ++i) close(msg_fds[i]);
            if (num_msg_fds > 0) {
                HAL_LOG_WARNING(
                    "FdPublisher: closed %d unexpected fd(s) on msg type=%u "
                    "from fd=%d",
                    num_msg_fds, hdr.type, client->fd);
            }
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

    // stream_name is a std::string read concurrently on the dispatch thread
    // (under clients_mu_) — a torn concurrent read is UB, not just a stale
    // value, so the assignment and the flag flip take the same lock.
    {
        std::lock_guard<std::mutex> lock(clients_mu_);
        client->stream_name = name;
        client->subscribed = true;
    }

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

void FdPublisher::handle_dsp_alloc(ClientState* client, const void* msg_data) {
    auto* msg = static_cast<const FdPubDspAllocMsg*>(msg_data);

    FdPubDspAllocRespMsg resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.type = FD_PUB_MSG_DSP_ALLOC_RESP;
    resp.hdr.size = sizeof(resp);

    if (!dsp_service_) {
        resp.code = DSP_SVC_ERR_UNAVAILABLE;
        send(client->fd, &resp, sizeof(resp), MSG_NOSIGNAL);
        return;
    }

    DspService::AllocResult r = dsp_service_->alloc_buffers(
        client->fd, msg->width, msg->height,
        static_cast<HalPixelFormat>(msg->format), msg->count);
    resp.code = r.rc;

    if (r.rc != DSP_SVC_OK) {
        HAL_LOG_WARNING("FdPublisher: DSP alloc fd=%d %ux%u fmt=%u n=%u failed: "
                        "rc=%d (%s)", client->fd, msg->width, msg->height,
                        msg->format, msg->count, r.rc, r.message.c_str());
        send(client->fd, &resp, sizeof(resp), MSG_NOSIGNAL);
        return;
    }

    // count is capped by the service; ids/fds are 1:1 with count*num_planes
    uint32_t count = std::min<uint32_t>(
        static_cast<uint32_t>(r.ids.size()), FD_PUB_DSP_MAX_FDS);
    int num_fds = std::min<int>(static_cast<int>(r.fds.size()),
                                FD_PUB_DSP_MAX_FDS);

    resp.count = count;
    resp.num_planes = r.num_planes;
    for (uint32_t i = 0; i < HAL_MAX_PLANES; i++) {
        resp.strides[i] = r.strides[i];
        resp.sizes[i] = r.sizes[i];
    }
    for (uint32_t i = 0; i < count; i++) {
        resp.buffer_ids[i] = r.ids[i];
    }

    // fds attached via SCM_RIGHTS in buffer-major order (count * num_planes)
    if (fd_pub_sendmsg_capped(client->fd, &resp, sizeof(resp),
                              r.fds.data(), num_fds, FD_PUB_DSP_MAX_FDS) != 0) {
        std::lock_guard<std::mutex> sl(stats_mu_);
        stats_.send_errors++;
        HAL_LOG_ERROR("FdPublisher: DSP alloc resp send failed for fd=%d",
                      client->fd);
    }
}

void FdPublisher::handle_dsp_buf_release(ClientState* client,
                                         const void* msg_data) {
    auto* msg = static_cast<const FdPubDspBufReleaseMsg*>(msg_data);

    if (!dsp_service_) return;

    int rc = dsp_service_->release_buffer(client->fd, msg->buffer_id);
    if (rc != DSP_SVC_OK) {
        HAL_LOG_WARNING("FdPublisher: DSP buf release id=%lu from fd=%d rc=%d",
                        (unsigned long)msg->buffer_id, client->fd, rc);
    }
}

void FdPublisher::handle_dsp_import(ClientState* client, const void* msg_data,
                                    const int* fds, int num_fds) {
    auto* msg = static_cast<const FdPubDspImportMsg*>(msg_data);

    FdPubDspImportRespMsg resp;
    memset(&resp, 0, sizeof(resp));
    resp.hdr.type = FD_PUB_MSG_DSP_IMPORT_RESP;
    resp.hdr.size = sizeof(resp);

    /* The received fds are ours the moment recvmsg returned them — every
     * path below must close them all. import_buffer() dups what it keeps. */
    auto close_received = [&]() {
        for (int i = 0; i < num_fds; ++i) close(fds[i]);
    };

    if (!dsp_service_) {
        resp.code = DSP_SVC_ERR_UNAVAILABLE;
        close_received();
        send(client->fd, &resp, sizeof(resp), MSG_NOSIGNAL);
        return;
    }
    if (msg->num_planes == 0 || msg->num_planes > FD_PUB_MAX_FDS ||
        num_fds != (int)msg->num_planes) {
        resp.code = DSP_SVC_ERR_INVALID;
        HAL_LOG_WARNING("FdPublisher: DSP IMPORT fd=%d plane/fd mismatch "
                        "(planes=%u fds=%d)", client->fd, msg->num_planes,
                        num_fds);
        close_received();
        send(client->fd, &resp, sizeof(resp), MSG_NOSIGNAL);
        return;
    }

    DspService::ImportResult r = dsp_service_->import_buffer(
        client->fd, msg->width, msg->height,
        static_cast<HalPixelFormat>(msg->format), msg->num_planes,
        msg->strides, msg->sizes, fds);
    close_received();

    resp.code = r.rc;
    if (r.rc != DSP_SVC_OK) {
        HAL_LOG_WARNING("FdPublisher: DSP IMPORT fd=%d %ux%u fmt=%u failed: "
                        "rc=%d (%s)", client->fd, msg->width, msg->height,
                        msg->format, r.rc, r.message.c_str());
    } else {
        resp.import_id = r.id;
    }
    send(client->fd, &resp, sizeof(resp), MSG_NOSIGNAL);
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

    // Detach every DSP buffer this client owns. Before close(): the fd number
    // is the registry key and could be reused by a new client after close().
    if (dsp_service_) {
        dsp_service_->release_client_buffers(client_fd);
    }

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

void FdPublisher::erase_outstanding(ClientState* client, uint64_t frame_id) {
    std::lock_guard<std::mutex> ol(client->outstanding_mu);
    client->outstanding.erase(frame_id);
}

FdPublisher::FrameSendResult
FdPublisher::send_frame_to_client(ClientState* client, ManagedFrame* mf) {
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
        return FrameSendResult::kUndeliverable;
    }

    // Non-blocking send: this runs on the FrameRouter dispatch thread and
    // must never stall on a slow client. EAGAIN means nothing was queued
    // (no fds crossed); a partial send sets EMSGSIZE in the helper and has
    // already desynced the client's stream.
    if (fd_pub_sendmsg_flags(client->fd, &msg, sizeof(msg), fds, num_fds,
                             MSG_DONTWAIT) == 0) {
        return FrameSendResult::kOk;
    }
    return (errno == EAGAIN) ? FrameSendResult::kSlowClient
                             : FrameSendResult::kHardError;
}
