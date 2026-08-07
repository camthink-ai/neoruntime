/**
 * @file frame_router.h
 * @brief Frame Router - Async reference-counted frame distribution
 *
 * Receives frames from VideoSource HAL callback. The callback only copies
 * metadata and enqueues — never blocks on subscriber work.
 * A dedicated dispatch thread distributes to subscribers asynchronously.
 *
 * Routing by std::string stream_name.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

extern "C" {
    #include "hal_video.h"
}

class VideoSource;
class FrameWatchdog;

/* ========== Managed Frame ========== */

struct ManagedFrame {
    std::string     stream_name;
    HalFrameBuffer  frame;          // Shallow copy of metadata (dma_fds, sizes, etc.)
    std::atomic<int> ref_count{0};
    std::chrono::steady_clock::time_point lend_time;
    uint64_t        frame_id;       // Globally unique ID
    std::atomic<bool> reclaimed{false};  // Set by force_reclaim; prevents double HAL release
};

/* ========== Subscriber ========== */

using SubscriberId = uint64_t;
using FrameConsumerFn = std::function<void(ManagedFrame* mf)>;

struct Subscriber {
    SubscriberId    id;
    std::string     name;       // e.g. "encoder_main", "shm_ai", "shm_sub"
    std::string     stream_name;// Which stream this subscriber listens to
    FrameConsumerFn on_frame;
    bool            active = true;
};

/* ========== Per-stream stats ========== */

struct RouterStreamStats {
    uint64_t frames_received = 0;
    uint64_t frames_dropped = 0;
    uint64_t force_reclaimed = 0;
};

/* ========== Frame Router ========== */

class FrameRouter {
public:
    FrameRouter(VideoSource* source, FrameWatchdog* watchdog);
    ~FrameRouter();

    FrameRouter(const FrameRouter&) = delete;
    FrameRouter& operator=(const FrameRouter&) = delete;

    /**
     * @brief Start the dispatch thread. Call after all subscribers are registered.
     */
    void start();

    /**
     * @brief Stop the dispatch thread and release all outstanding frames.
     */
    void stop();

    /**
     * @brief Subscribe to a stream's frames
     * @param stream_name Stream name (e.g. "main", "ai")
     * @param subscriber_name Human-readable name for logging
     * @param fn Called with ManagedFrame*; must call release(mf) when done
     * @return Subscriber ID (0 on failure)
     */
    SubscriberId subscribe(const std::string& stream_name,
                           const std::string& subscriber_name,
                           FrameConsumerFn fn);
    void unsubscribe(SubscriberId id);

    /**
     * @brief Frame arrival entry point (called by VideoSource HAL callback)
     *
     * Creates ManagedFrame with shallow metadata copy, enqueues for async
     * dispatch. Releases HAL frame buffer immediately (before subscribers run).
     */
    void on_frame_arrived(const std::string& stream_name, HalFrameBuffer* frame);

    /**
     * @brief Add one reference to a ManagedFrame.
     */
    void retain(ManagedFrame* mf);

    /**
     * @brief Release one reference. When ref_count reaches 0, cleans up.
     */
    void release(ManagedFrame* mf);

    /**
     * @brief Force reclaim a frame by ID (called by watchdog)
     */
    void force_reclaim(uint64_t frame_id);

    RouterStreamStats get_stats(const std::string& stream_name) const;

    /** Wait for fresh frame arrivals without retaining image buffers. */
    bool wait_next_frames(const std::string& stream_name, uint32_t count,
                          std::chrono::milliseconds timeout);
    uint64_t frame_sequence(const std::string& stream_name) const;

private:
    VideoSource*   source_;
    FrameWatchdog* watchdog_;

    // Subscriber registry: stream_name -> list of subscribers
    mutable std::mutex sub_mu_;
    std::unordered_map<std::string, std::vector<Subscriber>> subscribers_;
    std::atomic<SubscriberId> next_sub_id_{1};

    // Outstanding frame tracking (for watchdog)
    mutable std::mutex frames_mu_;
    std::unordered_map<uint64_t, std::shared_ptr<ManagedFrame>> outstanding_;
    std::atomic<uint64_t> next_frame_id_{1};

    // Per-stream stats
    mutable std::mutex stats_mu_;
    std::unordered_map<std::string, RouterStreamStats> stats_;

    mutable std::mutex frame_clock_mu_;
    std::condition_variable frame_clock_cv_;
    std::unordered_map<std::string, uint64_t> frame_sequences_;

    // Async dispatch queue
    struct QueuedFrame {
        std::shared_ptr<ManagedFrame> mf;
        std::vector<FrameConsumerFn>  callbacks;
    };
    static constexpr size_t MAX_DISPATCH_QUEUE = 8;
    std::mutex dispatch_mu_;
    std::condition_variable dispatch_cv_;
    std::deque<QueuedFrame> dispatch_queue_;
    std::thread dispatch_thread_;
    std::atomic<bool> running_{false};

    void dispatch_loop();
    void do_release(ManagedFrame* mf);
    void discard_queued_frame(const std::shared_ptr<ManagedFrame>& mf);
};
