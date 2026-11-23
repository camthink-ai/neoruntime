/**
 * @file hal_udp_stream.cpp
 */

#include "common/hal_udp_stream.hpp"
#include "common/hal_log.h"

#include <algorithm>
#include <atomic>
#include <arpa/inet.h>
#include <cstdio>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <netdb.h>
#include <thread>

#include <sys/socket.h>
#include <unistd.h>

namespace
{

constexpr size_t kRtpHeaderSize = 12;

struct QueuedAccessUnit
{
    std::vector<uint8_t> data;
    uint64_t timestamp_ns{0};
};

struct RtpState
{
    uint16_t seq{0};
    uint32_t ssrc{0};
};

uint32_t ns_to_rtp_ts90k(uint64_t ns)
{
    return static_cast<uint32_t>((ns * 9ULL) / 100000ULL);
}

uint64_t steady_now_ns()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

/* --------------------------------------------------------------------
 * Cross-instance RTP timestamp continuity (per SSRC + destination port).
 *
 * A rotation/flip change takes the HAL through a full medialib reinit which
 * destroys the encoder pipeline and the UDP pusher with it; the caller then
 * re-arms a NEW HalUdpStream, typically on the same port/SSRC. The new
 * pipeline's media clock restarts from a lower base, so raw timestamps
 * regress; RTP clients (ffmpeg/ffplay/VLC) keep the per-SSRC max timestamp
 * and drop every "older" packet forever ("dropping old packet received too
 * late") until the client is manually restarted.
 *
 * Emitted timestamps are therefore kept monotonic per logical stream, with
 * state that PERSISTS across pusher (re)creations:
 *   - equal raw timestamps pass through unchanged — multi_resize branches
 *     share the source pts, so concurrent streams legitimately carry equal
 *     ticks (bumping on equality would ratchet N interleaved pushers to
 *     N× the real frame rate);
 *   - on a true regression the emission is rebased onto this stream's emitted
 *     history while preserving its own raw cadence:
 *     emitted = last_emitted + (raw - last_raw) when raw advanced, else +1
 *     tick — correct at any frame rate, unlike a fixed per-frame tick.
 *
 * Keyed by (SSRC, destination port), NOT SSRC alone: concurrent pushers share
 * the DEFAULT ssrc 0x12345678 (e.g. video_test_sub_v2's two lanes) while
 * their pts bases differ by pipeline latency — a shared key would have the
 * lanes clamp each other on every AU and inflate both cadences. The port
 * identifies the logical stream: re-arming the same port resumes the state
 * (restart continuity), different ports never interact.
 * -------------------------------------------------------------------- */
struct RtpStreamContState
{
    uint32_t last_emitted_ts{0};
    uint32_t last_raw_ts{0};
    uint16_t last_seq{0};
    bool ts_seen{false};
    bool seq_seen{false};
};
std::mutex g_rtp_ts_mu;
std::unordered_map<uint64_t, RtpStreamContState> g_rtp_ts_by_stream;

uint32_t clamp_rtp_ts_monotonic(uint32_t ssrc, uint16_t port, uint32_t raw)
{
    const std::lock_guard<std::mutex> lock(g_rtp_ts_mu);
    const uint64_t key = (static_cast<uint64_t>(ssrc) << 16) | port;
    RtpStreamContState &st = g_rtp_ts_by_stream[key];
    uint32_t emitted = raw;
    if (st.ts_seen && static_cast<int32_t>(raw - st.last_emitted_ts) < 0)
    {
        const int32_t d_raw = static_cast<int32_t>(raw - st.last_raw_ts);
        emitted = (d_raw > 0) ? static_cast<uint32_t>(static_cast<int64_t>(st.last_emitted_ts) + d_raw)
                              : st.last_emitted_ts + 1U;
        static std::atomic<uint32_t> rebase_logs{0};
        if (rebase_logs.fetch_add(1, std::memory_order_relaxed) < 16)
        {
            HAL_LOG_WARNING("hal_udp_stream: rtp ts rebase ssrc=0x%08x port=%u raw=%u last_raw=%u "
                            "last_emitted=%u -> emitted=%u",
                            ssrc, port, raw, st.last_raw_ts, st.last_emitted_ts, emitted);
        }
    }
    st.ts_seen = true;
    st.last_emitted_ts = emitted;
    st.last_raw_ts = raw;
    return emitted;
}

/** Randomized base counter for FIRST-EVER streams only (RFC 3550 advises a
 *  non-zero start); continuation uses the stream's own emitted history. */
std::atomic<uint32_t> g_rtp_seq_alloc{0x6D2BU};

/** First sequence number of a (re)created pusher: resume from the stream's
 *  actually-emitted tail + a small forward gap (reads as bounded packet loss).
 *  A fixed-size allocation block drifts below the old tail once a pusher
 *  emitted more packets than the block size — high-bitrate streams pass 4096
 *  packets within seconds, which would turn every restart into a large
 *  backward jump (stale/reordered classification at the receiver). */
uint16_t rtp_seq_start(uint32_t ssrc, uint16_t port)
{
    const std::lock_guard<std::mutex> lock(g_rtp_ts_mu);
    const uint64_t key = (static_cast<uint64_t>(ssrc) << 16) | port;
    auto it = g_rtp_ts_by_stream.find(key);
    if (it != g_rtp_ts_by_stream.end() && it->second.seq_seen)
    {
        return static_cast<uint16_t>(it->second.last_seq + 16U);
    }
    return static_cast<uint16_t>(g_rtp_seq_alloc.fetch_add(0x1000U, std::memory_order_relaxed) & 0xFFFFU);
}

/** Record the last emitted sequence number of the logical stream (called once
 *  per access unit; restarts only ever happen between AUs, after the queue
 *  drains, so AU granularity is sufficient). */
void note_emitted_seq(uint32_t ssrc, uint16_t port, uint16_t seq)
{
    const std::lock_guard<std::mutex> lock(g_rtp_ts_mu);
    const uint64_t key = (static_cast<uint64_t>(ssrc) << 16) | port;
    RtpStreamContState &st = g_rtp_ts_by_stream[key];
    st.last_seq = seq;
    st.seq_seen = true;
}

void build_rtp_header(uint8_t *out, uint16_t seq, uint32_t ts, uint32_t ssrc, uint8_t payload_type, bool marker)
{
    out[0] = 0x80;
    const uint8_t mbit = marker ? static_cast<uint8_t>(0x80) : static_cast<uint8_t>(0);
    out[1] = static_cast<uint8_t>(mbit | (payload_type & 0x7FU));
    out[2] = static_cast<uint8_t>((seq >> 8) & 0xff);
    out[3] = static_cast<uint8_t>(seq & 0xff);
    out[4] = static_cast<uint8_t>((ts >> 24) & 0xff);
    out[5] = static_cast<uint8_t>((ts >> 16) & 0xff);
    out[6] = static_cast<uint8_t>((ts >> 8) & 0xff);
    out[7] = static_cast<uint8_t>(ts & 0xff);
    out[8] = static_cast<uint8_t>((ssrc >> 24) & 0xff);
    out[9] = static_cast<uint8_t>((ssrc >> 16) & 0xff);
    out[10] = static_cast<uint8_t>((ssrc >> 8) & 0xff);
    out[11] = static_cast<uint8_t>(ssrc & 0xff);
}

void split_annex_b_extract_nals(const std::vector<uint8_t> &buf, std::vector<std::vector<uint8_t>> *nals)
{
    nals->clear();
    size_t i = 0;
    const size_t n = buf.size();
    while (i < n)
    {
        if (i + 4 <= n && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1)
        {
            i += 4;
        }
        else if (i + 3 <= n && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
        {
            i += 3;
        }
        else
        {
            i++;
            continue;
        }
        const size_t start = i;
        while (i < n)
        {
            if (i + 3 < n && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1)
            {
                break;
            }
            if (i + 4 < n && buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 0 && buf[i + 3] == 1)
            {
                break;
            }
            i++;
        }
        if (i > start)
        {
            nals->emplace_back(buf.begin() + static_cast<std::ptrdiff_t>(start),
                               buf.begin() + static_cast<std::ptrdiff_t>(i));
        }
    }
}

} // namespace

struct HalUdpStream::Impl
{
    HalUdpStreamConfig cfg{};
    int fd{-1};
    sockaddr_storage peer{};
    socklen_t peer_len{0};

    std::mutex mu;
    std::condition_variable cv;
    std::deque<QueuedAccessUnit> queue;
    std::thread sender;
    std::atomic<bool> running{true};
    std::atomic<bool> accepting{true};

    std::atomic<uint64_t> *bytes_parent{nullptr};
    std::atomic<uint64_t> *packets_parent{nullptr};
    uint64_t dropped_aus{0};

    void add_sent(uint64_t bytes, uint64_t packets)
    {
        if (bytes_parent)
        {
            bytes_parent->fetch_add(bytes, std::memory_order_relaxed);
        }
        if (packets_parent)
        {
            packets_parent->fetch_add(packets, std::memory_order_relaxed);
        }
    }

    /** RFC 6184: single NAL or FU-A. */
    bool send_rtp_h264_nal(int sock, const sockaddr *addr, socklen_t addrlen, RtpState *rtp, const uint8_t *nal,
                           size_t nal_len, uint32_t rtp_ts, bool marker)
    {
        const size_t max_payload = cfg.max_payload;
        if (nal_len == 0)
        {
            return true;
        }
        if (nal_len <= max_payload)
        {
            std::vector<uint8_t> pkt(kRtpHeaderSize + nal_len);
            build_rtp_header(pkt.data(), ++rtp->seq, rtp_ts, rtp->ssrc, cfg.rtp_payload_type, marker);
            std::memcpy(pkt.data() + kRtpHeaderSize, nal, nal_len);
            const ssize_t n = ::sendto(sock, pkt.data(), pkt.size(), 0, addr, addrlen);
            if (n < 0)
            {
                return false;
            }
            add_sent(static_cast<uint64_t>(n), 1);
            return true;
        }
        const uint8_t nal_hdr = nal[0];
        const uint8_t nri = static_cast<uint8_t>(nal_hdr & 0x60);
        const uint8_t nal_type = static_cast<uint8_t>(nal_hdr & 0x1F);
        size_t offset = 1;
        bool first = true;
        while (offset < nal_len)
        {
            const size_t chunk = std::min(max_payload - 2, nal_len - offset);
            const bool last = (offset + chunk >= nal_len);
            std::vector<uint8_t> pkt(kRtpHeaderSize + 2 + chunk);
            build_rtp_header(pkt.data(), ++rtp->seq, rtp_ts, rtp->ssrc, cfg.rtp_payload_type, last ? marker : false);
            uint8_t *p = pkt.data() + kRtpHeaderSize;
            p[0] = static_cast<uint8_t>(nri | 28);
            p[1] = static_cast<uint8_t>((first ? 0x80 : 0) | (last ? 0x40 : 0) | nal_type);
            std::memcpy(p + 2, nal + offset, chunk);
            const ssize_t n = ::sendto(sock, pkt.data(), pkt.size(), 0, addr, addrlen);
            if (n < 0)
            {
                return false;
            }
            add_sent(static_cast<uint64_t>(n), 1);
            offset += chunk;
            first = false;
        }
        return true;
    }

    /** RFC 7798: single NAL unit packets or Fragmentation Unit (FU). */
    bool send_rtp_h265_nal(int sock, const sockaddr *addr, socklen_t addrlen, RtpState *rtp, const uint8_t *nal,
                          size_t nal_len, uint32_t rtp_ts, bool marker)
    {
        constexpr unsigned kFuType = 49U;
        const size_t max_payload = cfg.max_payload;
        if (nal_len == 0)
        {
            return true;
        }
        if (nal_len < 2)
        {
            return true;
        }
        if (nal_len <= max_payload)
        {
            std::vector<uint8_t> pkt(kRtpHeaderSize + nal_len);
            build_rtp_header(pkt.data(), ++rtp->seq, rtp_ts, rtp->ssrc, cfg.rtp_payload_type, marker);
            std::memcpy(pkt.data() + kRtpHeaderSize, nal, nal_len);
            const ssize_t n = ::sendto(sock, pkt.data(), pkt.size(), 0, addr, addrlen);
            if (n < 0)
            {
                return false;
            }
            add_sent(static_cast<uint64_t>(n), 1);
            return true;
        }
        const uint8_t orig_nal_type = static_cast<uint8_t>((nal[0] >> 1) & 0x3FU);
        const uint8_t payload_hdr0 =
            static_cast<uint8_t>((nal[0] & 0x81U) | static_cast<uint8_t>((kFuType & 0x3FU) << 1));
        const uint8_t payload_hdr1 = nal[1];
        size_t offset = 2;
        bool first = true;
        while (offset < nal_len)
        {
            const size_t chunk = std::min(max_payload - 3, nal_len - offset);
            const bool last = (offset + chunk >= nal_len);
            std::vector<uint8_t> pkt(kRtpHeaderSize + 3 + chunk);
            build_rtp_header(pkt.data(), ++rtp->seq, rtp_ts, rtp->ssrc, cfg.rtp_payload_type, last ? marker : false);
            uint8_t *p = pkt.data() + kRtpHeaderSize;
            p[0] = payload_hdr0;
            p[1] = payload_hdr1;
            p[2] = static_cast<uint8_t>((first ? 0x80U : 0U) | (last ? 0x40U : 0U) | (orig_nal_type & 0x3FU));
            std::memcpy(p + 3, nal + offset, chunk);
            const ssize_t n = ::sendto(sock, pkt.data(), pkt.size(), 0, addr, addrlen);
            if (n < 0)
            {
                return false;
            }
            add_sent(static_cast<uint64_t>(n), 1);
            offset += chunk;
            first = false;
        }
        return true;
    }

    bool send_raw_chunks(int sock, const sockaddr *addr, socklen_t addrlen, const uint8_t *data, size_t len)
    {
        const size_t max_payload = cfg.max_payload;
        if (len == 0)
        {
            return true;
        }
        size_t offset = 0;
        while (offset < len)
        {
            const size_t chunk = std::min(max_payload, len - offset);
            const ssize_t n = ::sendto(sock, data + offset, chunk, 0, addr, addrlen);
            if (n < 0)
            {
                return false;
            }
            add_sent(static_cast<uint64_t>(n), 1);
            offset += chunk;
        }
        return true;
    }

    void sender_loop()
    {
        RtpState rtp{};
        rtp.ssrc = cfg.rtp_ssrc;
        rtp.seq = static_cast<uint16_t>((rtp_seq_start(rtp.ssrc, cfg.port) - 1U) & 0xFFFFU); /* first ++seq = start */
        sockaddr *addr = reinterpret_cast<sockaddr *>(&peer);

        while (running.load(std::memory_order_acquire) || !queue.empty())
        {
            QueuedAccessUnit au;
            {
                std::unique_lock<std::mutex> lock(mu);
                cv.wait_for(lock, std::chrono::milliseconds(200),
                            [this] { return !queue.empty() || !running.load(std::memory_order_acquire); });
                if (queue.empty())
                {
                    continue;
                }
                au = std::move(queue.front());
                queue.pop_front();
            }

            if (cfg.mode == HalUdpStreamMode::Raw)
            {
                if (!send_raw_chunks(fd, addr, peer_len, au.data.data(), au.data.size()))
                {
                    HAL_LOG_ERROR("hal_udp_stream: sendto failed (raw)");
                }
                continue;
            }

            std::vector<std::vector<uint8_t>> nals;
            split_annex_b_extract_nals(au.data, &nals);
            uint64_t ts_ns = au.timestamp_ns;
            if (ts_ns == 0 || ts_ns == UINT64_MAX)
            {
                ts_ns = steady_now_ns();
            }
            const uint32_t rtp_ts = clamp_rtp_ts_monotonic(rtp.ssrc, cfg.port, ns_to_rtp_ts90k(ts_ns));
            const bool h265 = (cfg.mode == HalUdpStreamMode::RtpH265AnnexB);
            for (size_t k = 0; k < nals.size(); k++)
            {
                const bool mbit = (k + 1 == nals.size());
                const uint8_t *nal_data = nals[k].data();
                const size_t nal_sz = nals[k].size();
                bool ok_send = false;
                if (h265)
                {
                    ok_send = send_rtp_h265_nal(fd, addr, peer_len, &rtp, nal_data, nal_sz, rtp_ts, mbit);
                }
                else
                {
                    ok_send = send_rtp_h264_nal(fd, addr, peer_len, &rtp, nal_data, nal_sz, rtp_ts, mbit);
                }
                if (!ok_send)
                {
                    HAL_LOG_ERROR("hal_udp_stream: sendto failed (rtp)");
                }
            }
            note_emitted_seq(rtp.ssrc, cfg.port, rtp.seq);
        }
    }
};

HalUdpStream::HalUdpStream(const HalUdpStreamConfig &cfg)
{
    impl_ = std::make_unique<Impl>();
    impl_->cfg = cfg;
    impl_->bytes_parent = &bytes_sent_;
    impl_->packets_parent = &packets_sent_;

    if (!cfg.host || cfg.port == 0)
    {
        HAL_LOG_ERROR("hal_udp_stream: invalid host or port");
        return;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *res = nullptr;
    char port_str[16];
    (void)std::snprintf(port_str, sizeof(port_str), "%u", static_cast<unsigned>(cfg.port));
    if (getaddrinfo(cfg.host, port_str, &hints, &res) != 0 || !res)
    {
        HAL_LOG_ERROR("hal_udp_stream: getaddrinfo failed for %s", cfg.host);
        return;
    }

    impl_->fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (impl_->fd < 0)
    {
        HAL_LOG_ERROR("hal_udp_stream: socket failed");
        freeaddrinfo(res);
        return;
    }

    std::memcpy(&impl_->peer, res->ai_addr, res->ai_addrlen);
    impl_->peer_len = static_cast<socklen_t>(res->ai_addrlen);
    freeaddrinfo(res);

    impl_->sender = std::thread([this]() { impl_->sender_loop(); });
}

HalUdpStream::~HalUdpStream()
{
    shutdown();
}

bool HalUdpStream::ok() const
{
    return impl_ && impl_->fd >= 0;
}

void HalUdpStream::stop_accepting()
{
    if (!impl_)
    {
        return;
    }
    impl_->accepting.store(false, std::memory_order_release);
}

void HalUdpStream::shutdown()
{
    if (!impl_)
    {
        return;
    }
    impl_->running.store(false, std::memory_order_release);
    impl_->cv.notify_all();
    if (impl_->sender.joinable())
    {
        impl_->sender.join();
    }
    if (impl_->fd >= 0)
    {
        (void)::close(impl_->fd);
        impl_->fd = -1;
    }
}

void HalUdpStream::push_annex_b(std::vector<uint8_t> &&annex_b, uint64_t timestamp_ns)
{
    if (!impl_ || !impl_->accepting.load(std::memory_order_acquire) || impl_->fd < 0)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        while (impl_->queue.size() >= impl_->cfg.queue_depth)
        {
            impl_->queue.pop_front();
            impl_->dropped_aus++;
        }
        QueuedAccessUnit au;
        au.data = std::move(annex_b);
        au.timestamp_ns = timestamp_ns;
        impl_->queue.push_back(std::move(au));
    }
    if (impl_->dropped_aus != 0 && (impl_->dropped_aus % 64) == 0)
    {
        HAL_LOG_WARNING("udp_stream: dropped access units due to queue full (dropped=%llu, depth=%zu)",
                        static_cast<unsigned long long>(impl_->dropped_aus), impl_->cfg.queue_depth);
    }
    impl_->cv.notify_one();
}

void HalUdpStream::push_annex_b(const uint8_t *data, size_t len, uint64_t timestamp_ns)
{
    if (!data || len == 0)
    {
        return;
    }
    std::vector<uint8_t> copy(data, data + len);
    push_annex_b(std::move(copy), timestamp_ns);
}

void HalUdpStream::push_raw(const uint8_t *data, size_t len)
{
    if (!impl_ || !impl_->accepting.load(std::memory_order_acquire) || impl_->fd < 0)
    {
        return;
    }
    if (!data || len == 0)
    {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        while (impl_->queue.size() >= impl_->cfg.queue_depth)
        {
            impl_->queue.pop_front();
            impl_->dropped_aus++;
        }
        QueuedAccessUnit au;
        au.data.assign(data, data + len);
        au.timestamp_ns = 0;
        impl_->queue.push_back(std::move(au));
    }
    if (impl_->dropped_aus != 0 && (impl_->dropped_aus % 64) == 0)
    {
        HAL_LOG_WARNING("udp_stream: dropped chunks due to queue full (dropped=%llu, depth=%zu)",
                        static_cast<unsigned long long>(impl_->dropped_aus), impl_->cfg.queue_depth);
    }
    impl_->cv.notify_one();
}
