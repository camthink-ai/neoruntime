/**
 * @file encoded_publisher.h
 * @brief Async encoded stream publisher.
 *
 * Encoder callback → lock-free enqueue (never blocks encoder thread)
 * Dispatch thread  → local callbacks + socket broadcast
 *
 * Protocol V3 (38-byte header):
 *   [4 bytes]  total_size (uint32 LE, includes header)
 *   [1 byte]  codec      (0=h264, 1=h265)
 *   [1 byte]  flags      (bit0=keyframe, bit1=packet_seq present)
 *   [8 bytes] timestamp_ns (uint64 LE) - PTS
 *   [4 bytes] width      (uint32 LE)
 *   [4 bytes] height     (uint32 LE)
 *   [8 bytes] dts_ns     (uint64 LE) - DTS
 *   [8 bytes] packet_seq (uint64 LE) - per-stream monotonic, assigned at
 *               enqueue so a queue-overflow drop leaves a visible hole
 *   [N bytes] data       (raw Annex-B bitstream)
 *
 * A V3 writer is always readable by a V3 reader; a pre-V3 reader must not be
 * paired with a V3 writer (deploy the daemon and its SDK clients together).
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>
#include <memory>
#include <functional>
#include <condition_variable>
#include <deque>

extern "C" {
    #include "hal_buffer.h"
}

class EncodedPublisher {
public:
    static constexpr size_t HEADER_SIZE = 38;         // V3 (seq at offset 30)
    static constexpr size_t LEGACY_HEADER_SIZE = 30;  // V2 (no seq), pre-V3 peers
    static constexpr uint8_t FLAG_KEYFRAME = 0x01;
    static constexpr uint8_t FLAG_SEQ_PRESENT = 0x02;

    struct StreamConfig {
        std::string name;
        std::string codec;  // "h264" or "h265"
        uint32_t width;
        uint32_t height;
    };

    using LocalCallback = std::function<void(const std::string& stream_name,
                                             const HalPacketBuffer* packet)>;

    using KeyframeRequestFn = std::function<void(const std::string& stream_name)>;

    EncodedPublisher();
    ~EncodedPublisher();

    EncodedPublisher(const EncodedPublisher&) = delete;
    EncodedPublisher& operator=(const EncodedPublisher&) = delete;

    void add_stream(const StreamConfig& cfg, const std::string& base_dir = "/run/aipc/encoded");
    void remove_stream(const std::string& name);
    void add_local_listener(LocalCallback cb);
    void clear_local_listeners();
    void set_keyframe_request_cb(KeyframeRequestFn fn);

    bool start();
    void stop();

    void on_packet(const std::string& stream_name, const HalPacketBuffer* packet);

    // Per-stream drop/throughput snapshot for the unified observability
    // surface (GetStreamStatus). Returns false if the stream is unknown.
    struct StreamDropStats {
        uint64_t packets_published;     // seq assignments (== last_packet_seq)
        uint64_t queue_overflow_drops;  // newest-kept overflow evictions
        uint64_t client_send_drops;     // per-client skips (full / awaiting IDR)
        uint64_t client_send_failures;  // per-client hard errors (disconnect)
        uint64_t client_disconnects;    // peers lost via control-poll EOF/ERR
        uint64_t last_packet_seq;       // 0 = no packet yet
        uint32_t clients;               // live socket subscribers
    };
    bool get_stream_stats(const std::string& name, StreamDropStats* out);

private:
    struct ClientInfo {
        int fd = -1;
        bool alive = true;
        uint64_t frames_dropped = 0;
        // We dropped a frame on this client mid-stream: skip P-frames (they
        // reference the dropped frame) until the next keyframe resyncs it.
        bool needs_keyframe = false;
    };

    struct StreamState {
        StreamConfig config;
        std::string sock_path;
        int listen_fd = -1;

        std::mutex clients_mu;
        std::vector<std::unique_ptr<ClientInfo>> clients;

        // Observability counters (relaxed atomics; snapshot without locks).
        std::atomic<uint64_t> packets_published{0};
        std::atomic<uint64_t> queue_overflow_drops{0};
        std::atomic<uint64_t> client_send_drops{0};
        std::atomic<uint64_t> client_send_failures{0};
        std::atomic<uint64_t> client_disconnects{0};
        std::atomic<uint64_t> last_packet_seq{0};
        std::atomic<int64_t> last_keyframe_req_ms{0};  // steady-clock ms, IDR rate limit
    };

    struct QueuedPacket {
        std::string stream_name;
        std::vector<uint8_t> data;
        bool is_keyframe;
        uint64_t timestamp_ns;
        uint64_t dts_ns;
        uint64_t packet_seq;  // per-stream monotonic, assigned at enqueue
        uint32_t raw_size;
        std::vector<uint8_t> raw_data;
    };

    static constexpr size_t MAX_QUEUE_SIZE = 120;

    std::atomic<bool> running_{false};
    std::unordered_map<std::string, std::unique_ptr<StreamState>> streams_;
    int epoll_fd_ = -1;
    std::thread accept_thread_;
    std::thread dispatch_thread_;

    std::mutex queue_mu_;
    std::condition_variable queue_cv_;
    std::deque<std::unique_ptr<QueuedPacket>> queue_;
    std::unordered_map<std::string, uint64_t> next_seq_;  // per-stream seq, under queue_mu_

    std::mutex listeners_mu_;
    std::vector<LocalCallback> local_listeners_;

    KeyframeRequestFn keyframe_request_fn_;

    static constexpr uint8_t CTRL_REQUEST_KEYFRAME = 0x4B;

    void accept_loop();
    void dispatch_loop();
    void broadcast(StreamState& ss, const uint8_t* buf, size_t len, bool is_keyframe);
    void check_client_control(StreamState& ss, ClientInfo& c);
    void probe_clients(const std::string& stream_name, StreamState& ss);
    void reap_dead_clients_locked(const std::string& stream_name, StreamState& ss);

    // Non-blocking frame send for one client. Never blocks the dispatch
    // thread longer than the bounded partial-drain window.
    enum class SendOutcome { SENT, SKIP_AGAIN, SKIP_WAIT_KEYFRAME, FAIL };
    static SendOutcome send_frame_nb(ClientInfo& c, const uint8_t* buf, size_t len,
                                     bool is_keyframe);

    // Rate-limited (<=1/s per stream) encoder IDR request so a client that
    // lost frames can resync at the next keyframe.
    void maybe_request_keyframe(StreamState& ss);

    /**
     * Parse H.264/H.265 Annex-B bitstream to detect keyframes.
     * Returns true if an IDR NAL (H.264 type 5) or IRAP NAL (H.265 types 16-23) is found.
     */
    static bool detect_h264_keyframe(const uint8_t* data, size_t size, const std::string& codec);
};
