#pragma once

/**
 * FdReceiver — connects to camera-daemon's FD Publisher as a client,
 * receives DMA-BUF file descriptors via SCM_RIGHTS for zero-copy inference.
 *
 * Thread model: one background thread per subscribed stream.
 *
 * Multicast: multiple subscribers can subscribe to the same stream.
 * Each receives a copy of every frame. The physical connection to
 * camera-daemon is shared; it is established on first subscribe and
 * torn down when the last subscriber leaves.
 */

#include "fd_protocol.h"
#include "hal_buffer.h"
#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <memory>

namespace aipc::ai_runtime {

/**
 * FdGroup — manages a group of file descriptors (e.g., NV12 planes).
 * Automatically closes all FDs in its destructor.
 */
struct FdGroup {
    std::vector<int> fds;
    ~FdGroup();
};

struct ReceivedFrame {
    uint64_t frame_id;
    uint64_t timestamp_ns;
    uint64_t sequence;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t num_planes;
    uint32_t strides[3];
    uint32_t sizes[3];
    
    std::shared_ptr<FdGroup> fd_group;
};

using FrameCallback = std::function<void(const ReceivedFrame& frame)>;

class FdReceiver {
public:
    explicit FdReceiver(const std::string& socket_path);
    ~FdReceiver();

    /// Subscribe to a stream with a unique subscriber_id.
    /// Multiple subscribers can subscribe to the same stream.
    /// Returns false if this subscriber_id already exists or connection fails.
    bool subscribe(const std::string& stream_name,
                   const std::string& subscriber_id,
                   FrameCallback cb);

    /// Legacy single-subscriber API (uses stream_name as subscriber_id).
    bool subscribe(const std::string& stream_name, FrameCallback cb);

    /// Unsubscribe a specific subscriber. If it was the last for this stream,
    /// the physical connection is torn down.
    void unsubscribe(const std::string& stream_name,
                     const std::string& subscriber_id);

    /// Legacy single-subscriber unsubscribe.
    void unsubscribe(const std::string& stream_name);

    /// Increment reference count for a frame (called internally for each subscriber).
    void ref_frame(const std::string& stream_name, uint64_t frame_id);

    /// Decrement reference count and send RELEASE when it reaches zero.
    void release_frame(const std::string& stream_name, uint64_t frame_id);

    /// Number of active subscribers for a stream.
    int subscriber_count(const std::string& stream_name) const;

    void stop_all();

private:
    struct Subscriber {
        std::string   id;
        FrameCallback callback;
    };

    struct StreamConn {
        std::string              stream_name;
        int                      sock_fd = -1;
        std::thread              recv_thread;
        std::atomic<bool>        running{false};
        std::mutex               sub_mu;          // protects subscribers
        std::vector<Subscriber>  subscribers;
    };

    int  connect_to_server();
    bool setup_stream_connection(const std::string& stream_name);
    void teardown_stream_connection(StreamConn* conn);
    void recv_loop(StreamConn* conn);

    std::string socket_path_;
    mutable std::mutex  mu_;
    std::unordered_map<std::string, std::unique_ptr<StreamConn>> streams_;

    // Per-frame reference counting: key = (stream_name << 32 | frame_id)
    // When count reaches 0, RELEASE is sent to camera-daemon.
    std::mutex frame_ref_mu_;
    std::unordered_map<uint64_t, int> frame_refs_;

    // Simple hash combining stream+frame_id
    static uint64_t frame_ref_key(const std::string& stream, uint64_t fid) {
        // Use a fast hash: mix stream hash with frame_id
        return std::hash<std::string>{}(stream) ^ (fid * 0x9e3779b97f4a7c15ULL);
    }
};

}  // namespace aipc::ai_runtime
