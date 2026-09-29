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
#include <condition_variable>
#include <unordered_map>
#include <vector>
#include <memory>
#include <chrono>

namespace aipc::ai_runtime {

/**
 * FdGroup — manages a group of file descriptors (e.g., NV12 planes).
 * Automatically closes all FDs in its destructor.
 */
struct FdGroup {
    std::vector<int> fds;
    ~FdGroup();
};

struct FrameDeliveryState;

class FrameDelivery {
public:
    FrameDelivery() = default;

    /// Idempotently acknowledge this subscriber's delivery. The final delivery
    /// acknowledgement sends one RELEASE for the physical frame.
    void acknowledge() const noexcept;
    explicit operator bool() const noexcept { return static_cast<bool>(state_); }

private:
    explicit FrameDelivery(std::shared_ptr<FrameDeliveryState> state)
        : state_(std::move(state)) {}

    std::shared_ptr<FrameDeliveryState> state_;
    friend class FdReceiver;
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
    FrameDelivery delivery;
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

    /// Deadline-aware unsubscribe used by bounded shutdown paths. Returns false
    /// until callbacks and retained frame deliveries have drained, including when
    /// called from the subscriber's own callback (whose removal is deferred).
    bool unsubscribe_until(
        const std::string& stream_name,
        const std::string& subscriber_id,
        std::chrono::steady_clock::time_point deadline);

    /// Legacy single-subscriber unsubscribe.
    void unsubscribe(const std::string& stream_name);

    /// Number of active subscribers for a stream.
    int subscriber_count(const std::string& stream_name) const;

    /// Whether the stream's physical publisher connection is still receiving.
    bool stream_connected(const std::string& stream_name) const;

    /// Stop every stream and wait for receive threads to exit. When invoked by a
    /// receive callback, that callback's own stream is stopped asynchronously
    /// after the callback returns because a thread cannot join itself.
    void stop_all();

private:
    struct SubscriberDrainState {
        size_t active_callbacks = 0;
        size_t outstanding_deliveries = 0;
        bool removed = false;
    };

    struct Subscriber {
        std::string   id;
        FrameCallback callback;
        std::shared_ptr<SubscriberDrainState> drain;
        bool          removed = false;
    };

    struct StreamConn {
        std::string                              stream_name;
        int                                      sock_fd = -1;
        std::thread                              recv_thread;
        std::thread::id                          recv_thread_id;
        std::atomic<bool>                        running{false};
        std::timed_mutex                         send_mu; // serializes control messages and close
        std::timed_mutex                         teardown_mu;
        std::timed_mutex                         sub_mu; // protects subscribers and callback state
        std::condition_variable_any              callback_cv;
        std::timed_mutex                         exit_mu;
        std::condition_variable_any              exit_cv;
        bool                                     exited = false;
        size_t                                   active_callbacks = 0;
        size_t                                   outstanding_frames = 0;
        bool                                     teardown_when_idle = false;
        std::vector<std::shared_ptr<Subscriber>> subscribers;
        std::unordered_map<std::string,
                           std::shared_ptr<SubscriberDrainState>> subscriber_drains;
    };

    int  connect_to_server();
    bool setup_stream_connection(
        const std::string& stream_name,
        const std::shared_ptr<Subscriber>& initial_subscriber);
    bool teardown_stream_connection_until(
        const std::shared_ptr<StreamConn>& conn,
        std::chrono::steady_clock::time_point deadline);
    void teardown_stream_connection(const std::shared_ptr<StreamConn>& conn);
    static void recv_loop(std::shared_ptr<StreamConn> conn) noexcept;

    std::string socket_path_;
    mutable std::timed_mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<StreamConn>> streams_;
};

}  // namespace aipc::ai_runtime
