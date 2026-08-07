/**
 * @file hal_udp_stream.hpp
 * @brief Generic UDP push stream: raw datagrams, H.264 RTP (RFC 6184), or H.265 RTP (RFC 7798).
 *
 * Thread-safe enqueue from codec callbacks; a dedicated sender thread performs sendto().
 * Copying inbound data avoids blocking the media path longer than a memcpy + queue push.
 */
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

enum class HalUdpStreamMode
{
    /** Send payload as UDP packets, splitting at max_payload bytes per datagram (no RTP). */
    Raw,
    /** Parse Annex B, pack NAL units into RTP (H.264, dynamic PT, FU-A for large NALs). */
    RtpH264AnnexB,
    /** Parse Annex B, pack NAL units into RTP (H.265/HEVC, dynamic PT, FU for large NALs, RFC 7798). */
    RtpH265AnnexB,
};

struct HalUdpStreamConfig
{
    const char *host{nullptr};
    uint16_t port{0};
    /** Max RTP/UDP payload per datagram (typical MTU-safe: 1200). */
    size_t max_payload{1200};
    /** Drop oldest access units when the queue grows past this size. */
    size_t queue_depth{64};
    HalUdpStreamMode mode{HalUdpStreamMode::RtpH264AnnexB};
    uint32_t rtp_ssrc{0x12345678U};
    /** Dynamic RTP payload type (e.g. 96 for H.264, 97 for H.265 in typical SDP). */
    uint8_t rtp_payload_type{96U};
};

class HalUdpStream
{
public:
    explicit HalUdpStream(const HalUdpStreamConfig &cfg);
    HalUdpStream(const HalUdpStream &) = delete;
    HalUdpStream &operator=(const HalUdpStream &) = delete;
    HalUdpStream(HalUdpStream &&) = delete;
    HalUdpStream &operator=(HalUdpStream &&) = delete;
    ~HalUdpStream();

    /** True if the UDP socket was created and the sender thread was started. */
    bool ok() const;

    /** When false, push_* return immediately without enqueueing (for graceful shutdown). */
    void stop_accepting();

    /**
     * Stop sender, join thread, close socket. Idempotent.
     * Call after unsubscribe so the queue can drain without new packets.
     */
    void shutdown();

    void push_annex_b(std::vector<uint8_t> &&annex_b, uint64_t timestamp_ns);
    void push_annex_b(const uint8_t *data, size_t len, uint64_t timestamp_ns);

    /** Raw mode: one UDP datagram per chunk of up to max_payload bytes. */
    void push_raw(const uint8_t *data, size_t len);

    uint64_t bytes_sent() const { return bytes_sent_.load(std::memory_order_relaxed); }
    uint64_t packets_sent() const { return packets_sent_.load(std::memory_order_relaxed); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> packets_sent_{0};
};
