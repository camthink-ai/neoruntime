/**
 * @file encoded_publisher.h
 * @brief Async encoded stream publisher.
 *
 * Encoder callback → lock-free enqueue (never blocks encoder thread)
 * Dispatch thread  → local callbacks + socket broadcast
 *
 * Protocol V2 (30-byte header):
 *   [4 bytes]  total_size (uint32 LE, includes header)
 *   [1 byte]  codec      (0=h264, 1=h265)
 *   [1 byte]  flags      (bit0=keyframe)
 *   [8 bytes] timestamp_ns (uint64 LE) - PTS
 *   [4 bytes] width      (uint32 LE)
 *   [4 bytes] height     (uint32 LE)
 *   [8 bytes] dts_ns     (uint64 LE) - DTS
 *   [N bytes] data       (raw Annex-B bitstream)
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
    static constexpr size_t HEADER_SIZE = 30;

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

private:
    struct ClientInfo {
        int fd = -1;
        bool alive = true;
        uint64_t frames_dropped = 0;
    };

    struct StreamState {
        StreamConfig config;
        std::string sock_path;
        int listen_fd = -1;

        std::mutex clients_mu;
        std::vector<std::unique_ptr<ClientInfo>> clients;
    };

    struct QueuedPacket {
        std::string stream_name;
        std::vector<uint8_t> data;
        bool is_keyframe;
        uint64_t timestamp_ns;
        uint64_t dts_ns;
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

    std::mutex listeners_mu_;
    std::vector<LocalCallback> local_listeners_;

    KeyframeRequestFn keyframe_request_fn_;

    static constexpr uint8_t CTRL_REQUEST_KEYFRAME = 0x4B;

    void accept_loop();
    void dispatch_loop();
    void broadcast(StreamState& ss, const uint8_t* buf, size_t len);
    void check_client_control(const std::string& stream_name, ClientInfo& c);
    void probe_clients(const std::string& stream_name, StreamState& ss);
    void reap_dead_clients_locked(const std::string& stream_name, StreamState& ss);

    /**
     * Parse H.264/H.265 Annex-B bitstream to detect keyframes.
     * Returns true if an IDR NAL (H.264 type 5) or IRAP NAL (H.265 types 16-23) is found.
     */
    static bool detect_h264_keyframe(const uint8_t* data, size_t size, const std::string& codec);
};
