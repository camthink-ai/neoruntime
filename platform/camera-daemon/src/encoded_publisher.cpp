/**
 * @file encoded_publisher.cpp
 * @brief Async encoded stream publisher
 *
 * Architecture:
 *   Encoder callback -> on_packet() [fast enqueue, never blocks]
 *   Dispatch thread  -> local callbacks + socket broadcast
 */

#include "../include/encoded_publisher.h"

extern "C" {
    #include "hal_log.h"
}

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <chrono>

namespace {
constexpr size_t kMaxClientsPerStream = 16;
constexpr auto kClientProbeInterval = std::chrono::seconds(1);
// Partial-frame drain budget: once bytes of a frame are already in the
// socket, tearing it off would corrupt framing — give the client this long
// to drain, then drop the connection instead of the frame.
constexpr int kPartialDrainMs = 20;
// Minimum spacing between encoder IDR requests for one stream.
constexpr int64_t kKeyframeReqMinIntervalMs = 1000;
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/* ── H.264 / H.265 Annex-B keyframe detection ── */

bool EncodedPublisher::detect_h264_keyframe(const uint8_t* data, size_t size,
                                             const std::string& codec) {
    if (!data || size < 4) return false;

    const bool is_h265 = (codec == "h265");
    size_t i = 0;

    while (i + 3 < size) {
        // Find start code (0x000001 or 0x00000001)
        if (data[i] == 0 && data[i + 1] == 0) {
            size_t sc_len = 0;
            if (i + 2 < size && data[i + 2] == 1) {
                sc_len = 3;
            } else if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                sc_len = 4;
            }

            if (sc_len > 0) {
                size_t nal_offset = i + sc_len;
                if (nal_offset >= size) break;

                uint8_t nal_byte = data[nal_offset];

                if (!is_h265) {
                    // H.264: forbidden_zero_bit (1) | nal_ref_idc (2) | nal_unit_type (5)
                    uint8_t nal_type = nal_byte & 0x1F;
                    if (nal_type == 5) return true;  // IDR
                } else {
                    // H.265: forbidden_zero_bit (1) | nal_unit_type (6) | nuh_layer_id (6) | ...
                    uint8_t nal_type = (nal_byte >> 1) & 0x3F;
                    // IRAP types: BLA_W_LP(16)..RSV_IRAP_VCL23(23)
                    if (nal_type >= 16 && nal_type <= 23) return true;
                }

                i = nal_offset + 1;
                continue;
            }
        }
        i++;
    }
    return false;
}

EncodedPublisher::EncodedPublisher() = default;

EncodedPublisher::~EncodedPublisher() {
    stop();
}

void EncodedPublisher::add_stream(const StreamConfig& cfg, const std::string& base_dir) {
    // Clean up any existing entry for this name (close old socket, clients).
    auto it = streams_.find(cfg.name);
    if (it != streams_.end()) {
        auto& old = it->second;
        if (old->listen_fd >= 0) {
            if (epoll_fd_ >= 0)
                epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, it->second->listen_fd, nullptr);
            ::close(old->listen_fd);
            ::unlink(old->sock_path.c_str());
            old->listen_fd = -1;
        }

        std::lock_guard<std::mutex> lock(old->clients_mu);
        for (auto& c : old->clients) {
            if (c->fd >= 0) ::close(c->fd);
        }
        old->clients.clear();
    }

    auto ss = std::make_unique<StreamState>();
    ss->config = cfg;
    ss->sock_path = base_dir + "/" + cfg.name + ".sock";
    ::mkdir(base_dir.c_str(), 0775);
    ::chown(base_dir.c_str(), -1, 1001);  // aipc group GID

    // If publisher is already running, create the listen socket immediately.
    if (running_.load() && epoll_fd_ >= 0) {
        ::unlink(ss->sock_path.c_str());
        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, ss->sock_path.c_str(), sizeof(addr.sun_path) - 1);
            if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                ::chmod(ss->sock_path.c_str(), 0660);
                ::chown(ss->sock_path.c_str(), -1, 1001);
                if (::listen(fd, 8) == 0) {
                    set_nonblocking(fd);
                    ss->listen_fd = fd;
                    struct epoll_event ev{};
                    ev.events = EPOLLIN;
                    ev.data.fd = fd;
                    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
                    HAL_LOG_INFO("EncodedPublisher: Dynamically added listening socket for stream '%s' on %s",
                                 cfg.name.c_str(), ss->sock_path.c_str());
                } else {
                    HAL_LOG_ERROR("EncodedPublisher: listen() failed for dynamic add '%s': %s",
                                  cfg.name.c_str(), strerror(errno));
                    ::close(fd);
                }
            } else {
                HAL_LOG_ERROR("EncodedPublisher: bind(%s) failed for dynamic add '%s': %s",
                              ss->sock_path.c_str(), cfg.name.c_str(), strerror(errno));
                ::close(fd);
            }
        } else {
            HAL_LOG_ERROR("EncodedPublisher: socket() failed for dynamic add '%s': %s",
                          cfg.name.c_str(), strerror(errno));
        }
    }

    streams_[cfg.name] = std::move(ss);
}

void EncodedPublisher::remove_stream(const std::string& name) {
    auto it = streams_.find(name);
    if (it == streams_.end()) return;

    auto& ss = it->second;
    if (ss->listen_fd >= 0) {
        if (epoll_fd_ >= 0)
            epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, ss->listen_fd, nullptr);
        ::close(ss->listen_fd);
        ::unlink(ss->sock_path.c_str());
        ss->listen_fd = -1;
    }
    {
        std::lock_guard<std::mutex> lock(ss->clients_mu);
        for (auto& c : ss->clients) {
            if (c->fd >= 0) ::close(c->fd);
        }
        ss->clients.clear();
    }
    streams_.erase(it);
    HAL_LOG_INFO("EncodedPublisher: Removed stream '%s'", name.c_str());
}

void EncodedPublisher::add_local_listener(LocalCallback cb) {
    std::lock_guard<std::mutex> lock(listeners_mu_);
    local_listeners_.push_back(std::move(cb));
}

void EncodedPublisher::clear_local_listeners() {
    std::lock_guard<std::mutex> lock(listeners_mu_);
    local_listeners_.clear();
}

void EncodedPublisher::set_keyframe_request_cb(KeyframeRequestFn fn) {
    keyframe_request_fn_ = std::move(fn);
}

bool EncodedPublisher::start() {
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        HAL_LOG_ERROR("EncodedPublisher: epoll_create1 failed: %s", strerror(errno));
        return false;
    }

    for (auto& [name, ss] : streams_) {
        ::unlink(ss->sock_path.c_str());

        int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            HAL_LOG_ERROR("EncodedPublisher: socket() failed for %s: %s",
                         name.c_str(), strerror(errno));
            continue;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, ss->sock_path.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            HAL_LOG_ERROR("EncodedPublisher: bind(%s) failed: %s",
                         ss->sock_path.c_str(), strerror(errno));
            ::close(fd);
            continue;
        }

        ::chmod(ss->sock_path.c_str(), 0660);
        ::chown(ss->sock_path.c_str(), -1, 1001);  // aipc group GID

        if (::listen(fd, 8) < 0) {
            HAL_LOG_ERROR("EncodedPublisher: listen() failed: %s", strerror(errno));
            ::close(fd);
            continue;
        }

        set_nonblocking(fd);
        ss->listen_fd = fd;

        struct epoll_event ev{};
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);

        HAL_LOG_INFO("EncodedPublisher: Listening on %s for stream '%s' (%s %ux%u)",
                    ss->sock_path.c_str(), name.c_str(), ss->config.codec.c_str(),
                    ss->config.width, ss->config.height);
    }

    running_.store(true);
    accept_thread_ = std::thread(&EncodedPublisher::accept_loop, this);
    dispatch_thread_ = std::thread(&EncodedPublisher::dispatch_loop, this);
    return true;
}

void EncodedPublisher::stop() {
    if (!running_.exchange(false)) return;

    // Wake dispatch thread
    queue_cv_.notify_all();

    for (auto& [name, ss] : streams_) {
        if (ss->listen_fd >= 0) {
            ::close(ss->listen_fd);
            ::unlink(ss->sock_path.c_str());
            ss->listen_fd = -1;
        }
        std::lock_guard<std::mutex> lock(ss->clients_mu);
        for (auto& c : ss->clients) {
            if (c->fd >= 0) ::close(c->fd);
        }
        ss->clients.clear();
    }

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }

    if (accept_thread_.joinable()) accept_thread_.join();
    if (dispatch_thread_.joinable()) dispatch_thread_.join();

    HAL_LOG_INFO("EncodedPublisher: Stopped");
}

void EncodedPublisher::accept_loop() {
    constexpr int MAX_EVENTS = 16;
    struct epoll_event events[MAX_EVENTS];
    auto next_probe = std::chrono::steady_clock::now() + kClientProbeInterval;

    while (running_.load()) {
        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_probe) {
            for (auto& [name, ss] : streams_) {
                probe_clients(name, *ss);
            }
            next_probe = now + kClientProbeInterval;
        }

        for (int i = 0; i < n; i++) {
            int listen_fd = events[i].data.fd;

            for (auto& [name, ss] : streams_) {
                if (ss->listen_fd != listen_fd) continue;

                while (true) {
                    int cfd = ::accept(listen_fd, nullptr, nullptr);
                    if (cfd < 0) break;

                    int sndbuf = 4 * 1024 * 1024;
                    setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
                    set_nonblocking(cfd);

                    auto client = std::make_unique<ClientInfo>();
                    client->fd = cfd;

                    probe_clients(name, *ss);
                    std::lock_guard<std::mutex> lock(ss->clients_mu);
                    if (ss->clients.size() >= kMaxClientsPerStream) {
                        HAL_LOG_WARNING("EncodedPublisher [%s]: Rejecting client fd=%d, total=%zu, max=%zu",
                                        name.c_str(), cfd, ss->clients.size(), kMaxClientsPerStream);
                        ::close(cfd);
                        continue;
                    }
                    ss->clients.push_back(std::move(client));

                    HAL_LOG_INFO("EncodedPublisher [%s]: Client connected (fd=%d, total=%zu)",
                                name.c_str(), cfd, ss->clients.size());
                }
                break;
            }
        }
    }
}

// ────────────────────────────────────────────────────────────
//  on_packet: called from encoder callback — must be FAST
// ────────────────────────────────────────────────────────────

void EncodedPublisher::on_packet(const std::string& stream_name, const HalPacketBuffer* packet) {
    if (!running_.load()) return;
    if (!packet || !packet->data || packet->size == 0) return;

    auto it = streams_.find(stream_name);
    if (it == streams_.end()) return;

    // Minimal work in encoder callback: copy raw data + enqueue.
    // All parsing (keyframe detection) and framing happens in dispatch_loop.
    auto qp = std::make_unique<QueuedPacket>();
    qp->stream_name = stream_name;
    qp->timestamp_ns = packet->timestamp_ns;
    qp->dts_ns = packet->timestamp_ns;
    qp->raw_size = packet->size;
    qp->raw_data.assign(packet->data, packet->data + packet->size);

    {
        std::lock_guard<std::mutex> lock(queue_mu_);
        // Seq assigned at enqueue: a queue-overflow eviction of an earlier
        // packet leaves a permanent, client-visible hole in the sequence.
        uint64_t seq = ++next_seq_[stream_name];
        qp->packet_seq = seq;
        if (queue_.size() >= MAX_QUEUE_SIZE) {
            queue_.pop_front();
            it->second->queue_overflow_drops.fetch_add(1, std::memory_order_relaxed);
        }
        queue_.push_back(std::move(qp));
        it->second->last_packet_seq.store(seq, std::memory_order_relaxed);
        it->second->packets_published.fetch_add(1, std::memory_order_relaxed);
    }
    queue_cv_.notify_one();
}

// ────────────────────────────────────────────────────────────
//  dispatch_loop: runs on dedicated thread, handles all I/O
//  Generates continuous timestamps to avoid timestamp disorder
// ────────────────────────────────────────────────────────────

void EncodedPublisher::dispatch_loop() {
    HAL_LOG_INFO("EncodedPublisher: Dispatch thread started");

    uint64_t pkt_count = 0;
    uint64_t keyframe_count = 0;
    uint64_t slow_broadcast_count = 0;  // >5ms broadcast calls
    uint64_t max_broadcast_us = 0;

    while (running_.load()) {
        std::unique_ptr<QueuedPacket> pkt;

        {
            std::unique_lock<std::mutex> lock(queue_mu_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(100),
                              [this] { return !queue_.empty() || !running_.load(); });
            if (queue_.empty()) continue;
            pkt = std::move(queue_.front());
            queue_.pop_front();
        }

        if (!pkt) continue;

        pkt_count++;

        // --- Keyframe detection (NAL parsing, safe to do here) ---
        auto it = streams_.find(pkt->stream_name);
        if (it != streams_.end()) {
            pkt->is_keyframe = detect_h264_keyframe(
                pkt->raw_data.data(), pkt->raw_data.size(),
                it->second->config.codec);
        }

        if (pkt->is_keyframe) keyframe_count++;

        // Log every 300 packets (~10s at 30fps)
        if (pkt_count % 300 == 0) {
            HAL_LOG_INFO("EncodedPublisher: dispatched %lu pkts (%lu keyframes), stream=%s, slow_broadcast=%lu, max_broadcast_us=%lu",
                        (unsigned long)pkt_count, (unsigned long)keyframe_count,
                        pkt->stream_name.c_str(), (unsigned long)slow_broadcast_count,
                        (unsigned long)max_broadcast_us);
            slow_broadcast_count = 0;
            max_broadcast_us = 0;
        }

        // --- Timing: start of dispatch work ---
        auto dispatch_t0 = std::chrono::steady_clock::now();

        // 1) Call local listeners (built-in RTSP etc.)
        {
            HalPacketBuffer local_pkt{};
            local_pkt.data = pkt->raw_data.data();
            local_pkt.size = pkt->raw_size;
            local_pkt.timestamp_ns = pkt->timestamp_ns;

            std::lock_guard<std::mutex> lock(listeners_mu_);
            for (auto& cb : local_listeners_) {
                cb(pkt->stream_name, &local_pkt);
            }
        }

        // 2) Build framed message and broadcast to socket clients
        if (it != streams_.end()) {
            auto& ss = *it->second;
            uint32_t total_size = HEADER_SIZE + pkt->raw_size;

            pkt->data.resize(total_size);
            uint8_t* p = pkt->data.data();

            // total_size (LE)
            p[0] = total_size & 0xFF;
            p[1] = (total_size >> 8) & 0xFF;
            p[2] = (total_size >> 16) & 0xFF;
            p[3] = (total_size >> 24) & 0xFF;

            // codec
            p[4] = (ss.config.codec == "h265") ? 1 : 0;

            // flags — bit0 = keyframe, bit1 = packet_seq present (V3)
            p[5] = (pkt->is_keyframe ? FLAG_KEYFRAME : 0x00) | FLAG_SEQ_PRESENT;

            // timestamp_ns (LE) - PTS
            uint64_t pts = pkt->timestamp_ns;
            for (int i = 0; i < 8; i++) {
                p[6 + i] = (pts >> (i * 8)) & 0xFF;
            }

            // width (LE)
            uint32_t w = ss.config.width;
            p[14] = w & 0xFF;
            p[15] = (w >> 8) & 0xFF;
            p[16] = (w >> 16) & 0xFF;
            p[17] = (w >> 24) & 0xFF;

            // height (LE)
            uint32_t h = ss.config.height;
            p[18] = h & 0xFF;
            p[19] = (h >> 8) & 0xFF;
            p[20] = (h >> 16) & 0xFF;
            p[21] = (h >> 24) & 0xFF;

            // dts_ns (LE)
            uint64_t dts = pkt->dts_ns;
            for (int i = 0; i < 8; i++) {
                p[22 + i] = (dts >> (i * 8)) & 0xFF;
            }

            // packet_seq (LE) — V3 extension at offset 30
            uint64_t seq = pkt->packet_seq;
            for (int i = 0; i < 8; i++) {
                p[30 + i] = (seq >> (i * 8)) & 0xFF;
            }

            // payload
            memcpy(p + HEADER_SIZE, pkt->raw_data.data(), pkt->raw_size);

            auto bcast_t0 = std::chrono::steady_clock::now();
            broadcast(ss, pkt->data.data(), pkt->data.size(), pkt->is_keyframe);
            auto bcast_t1 = std::chrono::steady_clock::now();
            auto bcast_us = std::chrono::duration_cast<std::chrono::microseconds>(bcast_t1 - bcast_t0).count();
            if ((uint64_t)bcast_us > max_broadcast_us) max_broadcast_us = bcast_us;
            if (bcast_us > 5000) {
                slow_broadcast_count++;
                HAL_LOG_WARNING("EncodedPublisher: SLOW broadcast %ld us (%zu bytes, %d clients), stream=%s",
                               (long)bcast_us, pkt->data.size(),
                               (int)ss.clients.size(), pkt->stream_name.c_str());
            }
        }

        auto dispatch_t1 = std::chrono::steady_clock::now();
        auto total_us = std::chrono::duration_cast<std::chrono::microseconds>(dispatch_t1 - dispatch_t0).count();
        if (total_us > 10000) {
            HAL_LOG_WARNING("EncodedPublisher: SLOW dispatch total %ld us, pkt=%zu bytes, stream=%s",
                           (long)total_us, pkt->raw_size, pkt->stream_name.c_str());
        }
    }

    HAL_LOG_INFO("EncodedPublisher: Dispatch thread stopped");
}

void EncodedPublisher::check_client_control(StreamState& ss, ClientInfo& c) {
    // Non-blocking read of control bytes from client
    uint8_t ctrl;
    while (true) {
        ssize_t n = ::recv(c.fd, &ctrl, 1, MSG_DONTWAIT);
        if (n == 0) {
            // Peer gone (close/SIGKILL): EOF on the control poll is how the
            // publisher normally notices a dead subscriber — usually before
            // any send would EPIPE. Count it so the loss shows up on the
            // observability surface instead of only the reap log.
            ss.client_disconnects.fetch_add(1, std::memory_order_relaxed);
            c.alive = false;
            break;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            ss.client_disconnects.fetch_add(1, std::memory_order_relaxed);
            c.alive = false;
            break;
        }
        if (ctrl == CTRL_REQUEST_KEYFRAME && keyframe_request_fn_) {
            // HAL_LOG_INFO("EncodedPublisher [%s]: Keyframe request from client fd=%d",
            //            ss.config.name.c_str(), c.fd);
            try {
                keyframe_request_fn_(ss.config.name);
            } catch (const std::exception& e) {
                HAL_LOG_ERROR("EncodedPublisher: keyframe request callback failed: %s", e.what());
            } catch (...) {
                HAL_LOG_ERROR("EncodedPublisher: keyframe request callback failed (unknown)");
            }
        }
    }
}

void EncodedPublisher::probe_clients(const std::string& stream_name, StreamState& ss) {
    std::lock_guard<std::mutex> lock(ss.clients_mu);
    for (auto& c : ss.clients) {
        if (!c->alive) continue;
        check_client_control(ss, *c);
    }
    reap_dead_clients_locked(stream_name, ss);
}

void EncodedPublisher::reap_dead_clients_locked(const std::string& stream_name, StreamState& ss) {
    ss.clients.erase(
        std::remove_if(ss.clients.begin(), ss.clients.end(),
            [&](const std::unique_ptr<ClientInfo>& c) {
                if (!c->alive) {
                    HAL_LOG_INFO("EncodedPublisher [%s]: Client disconnected (fd=%d, dropped=%lu)",
                                stream_name.c_str(), c->fd, (unsigned long)c->frames_dropped);
                    if (c->fd >= 0) ::close(c->fd);
                    return true;
                }
                return false;
            }),
        ss.clients.end());
}

/**
 * Non-blocking frame send for one client. One MSG_DONTWAIT attempt: a full
 * socket buffer with nothing sent drops THIS frame for THIS client (the
 * frame stays intact for everyone else); a partial send gets a bounded
 * POLLOUT drain because tearing a frame would corrupt the client's framing
 * — past the deadline the client is dropped instead.
 */
EncodedPublisher::SendOutcome EncodedPublisher::send_frame_nb(ClientInfo& c,
                                                              const uint8_t* buf,
                                                              size_t len,
                                                              bool is_keyframe) {
    // A frame we dropped earlier broke this client's reference chain: only a
    // keyframe can resync it, everything else would decode as garbage.
    if (c.needs_keyframe && !is_keyframe) return SendOutcome::SKIP_WAIT_KEYFRAME;

    size_t total = 0;
    while (total < len) {
        ssize_t n = ::send(c.fd, buf + total, len - total, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (total == 0) return SendOutcome::SKIP_AGAIN;  // frame never started
            struct pollfd pfd = { c.fd, POLLOUT, 0 };
            int r = ::poll(&pfd, 1, kPartialDrainMs);
            if (r > 0 && (pfd.revents & POLLOUT)) continue;
            return SendOutcome::FAIL;  // mid-frame stall: framing at risk
        }
        return SendOutcome::FAIL;  // EPIPE / ECONNRESET / ...
    }
    return SendOutcome::SENT;
}

void EncodedPublisher::maybe_request_keyframe(StreamState& ss) {
    if (!keyframe_request_fn_) return;
    int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    int64_t last = ss.last_keyframe_req_ms.load(std::memory_order_relaxed);
    if (now_ms - last < kKeyframeReqMinIntervalMs) return;
    if (!ss.last_keyframe_req_ms.compare_exchange_strong(last, now_ms)) return;
    try {
        keyframe_request_fn_(ss.config.name);
    } catch (const std::exception& e) {
        HAL_LOG_ERROR("EncodedPublisher: keyframe request callback failed: %s", e.what());
    } catch (...) {
        HAL_LOG_ERROR("EncodedPublisher: keyframe request callback failed (unknown)");
    }
}

void EncodedPublisher::broadcast(StreamState& ss, const uint8_t* buf, size_t len,
                                 bool is_keyframe) {
    std::lock_guard<std::mutex> lock(ss.clients_mu);

    for (auto& c : ss.clients) {
        if (!c->alive) continue;

        // Check for reverse control messages (keyframe request etc.)
        check_client_control(ss, *c);
        if (!c->alive) continue;

        auto send_t0 = std::chrono::steady_clock::now();
        SendOutcome oc = send_frame_nb(*c, buf, len, is_keyframe);
        auto send_t1 = std::chrono::steady_clock::now();
        auto send_us = std::chrono::duration_cast<std::chrono::microseconds>(send_t1 - send_t0).count();
        if (send_us > 5000) {
            HAL_LOG_WARNING("EncodedPublisher: SLOW send fd=%d %ld us (%zu bytes), stream=%s",
                           c->fd, (long)send_us, len, ss.config.name.c_str());
        }

        switch (oc) {
        case SendOutcome::SENT:
            c->needs_keyframe = false;
            break;
        case SendOutcome::SKIP_AGAIN:
        case SendOutcome::SKIP_WAIT_KEYFRAME:
            // Frame dropped for this client only; it resyncs at the next
            // keyframe (requested, rate-limited) instead of decoding garbage.
            c->frames_dropped++;
            c->needs_keyframe = true;
            ss.client_send_drops.fetch_add(1, std::memory_order_relaxed);
            maybe_request_keyframe(ss);
            break;
        case SendOutcome::FAIL:
            c->alive = false;
            ss.client_send_failures.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }

    reap_dead_clients_locked(ss.config.name, ss);
}

bool EncodedPublisher::get_stream_stats(const std::string& name, StreamDropStats* out) {
    if (!out) return false;
    auto it = streams_.find(name);
    if (it == streams_.end()) return false;
    auto& ss = *it->second;
    out->packets_published = ss.packets_published.load(std::memory_order_relaxed);
    out->queue_overflow_drops = ss.queue_overflow_drops.load(std::memory_order_relaxed);
    out->client_send_drops = ss.client_send_drops.load(std::memory_order_relaxed);
    out->client_send_failures = ss.client_send_failures.load(std::memory_order_relaxed);
    out->client_disconnects = ss.client_disconnects.load(std::memory_order_relaxed);
    out->last_packet_seq = ss.last_packet_seq.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(ss.clients_mu);
        out->clients = (uint32_t)ss.clients.size();
    }
    return true;
}
