/**
 * @file frame_router.cpp
 * @brief Frame Router - Async reference-counted frame distribution
 *
 * The HAL video callback runs on a media pipeline thread and must return
 * quickly. on_frame_arrived() only does a shallow metadata copy + enqueue.
 * A dedicated dispatch thread distributes to subscribers, allowing the
 * HAL buffer to be released immediately after enqueue.
 */

#include "../include/frame_router.h"
#include "../include/video_source.h"
#include "../include/frame_watchdog.h"

extern "C" {
    #include "hal_log.h"
}

FrameRouter::FrameRouter(VideoSource* source, FrameWatchdog* watchdog)
    : source_(source), watchdog_(watchdog) {}

FrameRouter::~FrameRouter() {
    stop();

    // Release any orphaned frames
    std::vector<std::shared_ptr<ManagedFrame>> orphans;
    {
        std::lock_guard<std::mutex> lock(frames_mu_);
        for (auto& [fid, mf] : outstanding_) {
            orphans.push_back(mf);
        }
        outstanding_.clear();
    }
    for (auto& mf : orphans) {
        HAL_LOG_WARNING("FrameRouter: Releasing orphaned frame %lu", mf->frame_id);
    }
}

void FrameRouter::start() {
    if (running_.exchange(true)) return;
    dispatch_thread_ = std::thread(&FrameRouter::dispatch_loop, this);
    HAL_LOG_INFO("FrameRouter: Dispatch thread started");
}

void FrameRouter::stop() {
    if (!running_.exchange(false)) return;
    dispatch_cv_.notify_all();
    frame_clock_cv_.notify_all();
    if (dispatch_thread_.joinable()) {
        dispatch_thread_.join();
    }

    // Drain remaining queued frames
    std::deque<QueuedFrame> pending;
    {
        std::lock_guard<std::mutex> lock(dispatch_mu_);
        pending.swap(dispatch_queue_);
    }
    for (auto& qf : pending) {
        discard_queued_frame(qf.mf);
    }
}

SubscriberId FrameRouter::subscribe(const std::string& stream_name,
                                    const std::string& subscriber_name,
                                    FrameConsumerFn fn) {
    SubscriberId id = next_sub_id_.fetch_add(1);

    Subscriber sub;
    sub.id = id;
    sub.name = subscriber_name;
    sub.on_frame = std::move(fn);
    sub.active = true;
    sub.stream_name = stream_name;

    {
        std::lock_guard<std::mutex> lock(sub_mu_);
        subscribers_[stream_name].push_back(std::move(sub));
    }

    HAL_LOG_INFO("FrameRouter: Subscribed [%s] to stream [%s] (id=%lu)",
                 subscriber_name.c_str(), stream_name.c_str(), id);
    return id;
}

void FrameRouter::unsubscribe(SubscriberId id) {
    std::lock_guard<std::mutex> lock(sub_mu_);
    for (auto& [name, subs] : subscribers_) {
        for (auto it = subs.begin(); it != subs.end(); ++it) {
            if (it->id == id) {
                HAL_LOG_INFO("FrameRouter: Unsubscribed [%s] (id=%lu)",
                             it->name.c_str(), id);
                subs.erase(it);
                return;
            }
        }
    }
}

void FrameRouter::on_frame_arrived(const std::string& stream_name, HalFrameBuffer* frame) {
    // --- Fast path: shallow copy metadata, enqueue, release HAL buffer ---
    {
        std::lock_guard<std::mutex> lock(frame_clock_mu_);
        ++frame_sequences_[stream_name];
    }
    frame_clock_cv_.notify_all();

    int active_count = 0;
    std::vector<FrameConsumerFn> callbacks;

    {
        std::lock_guard<std::mutex> lock(sub_mu_);
        auto it = subscribers_.find(stream_name);
        if (it == subscribers_.end() || it->second.empty()) {
            source_->release_frame(stream_name, frame);
            std::lock_guard<std::mutex> sl(stats_mu_);
            stats_[stream_name].frames_dropped++;
            return;
        }

        for (auto& sub : it->second) {
            if (sub.active) {
                callbacks.push_back(sub.on_frame);
                active_count++;
            }
        }
    }

    if (active_count == 0) {
        source_->release_frame(stream_name, frame);
        return;
    }

    // Create ManagedFrame with shallow metadata copy
    auto mf = std::make_shared<ManagedFrame>();
    mf->stream_name = stream_name;
    mf->frame = *frame;            // Shallow copy: dma_fds, metadata only
    mf->ref_count.store(active_count, std::memory_order_relaxed);
    mf->lend_time = std::chrono::steady_clock::now();
    mf->frame_id = next_frame_id_.fetch_add(1);

    if (watchdog_) {
        watchdog_->track(mf->frame_id, mf->lend_time);
    }

    {
        std::lock_guard<std::mutex> lock(frames_mu_);
        outstanding_[mf->frame_id] = mf;
    }

    {
        std::lock_guard<std::mutex> lock(stats_mu_);
        stats_[stream_name].frames_received++;
    }

    // Enqueue for async dispatch
    {
        std::lock_guard<std::mutex> lock(dispatch_mu_);
        if (dispatch_queue_.size() >= MAX_DISPATCH_QUEUE) {
            // Drop oldest — it's already stale
            auto old = std::move(dispatch_queue_.front());
            dispatch_queue_.pop_front();
            discard_queued_frame(old.mf);
        }
        dispatch_queue_.push_back({mf, std::move(callbacks)});
    }
    dispatch_cv_.notify_one();
}

void FrameRouter::dispatch_loop() {
    while (running_.load()) {
        QueuedFrame qf;

        {
            std::unique_lock<std::mutex> lock(dispatch_mu_);
            dispatch_cv_.wait_for(lock, std::chrono::milliseconds(50),
                [this] { return !dispatch_queue_.empty() || !running_.load(); });
            if (dispatch_queue_.empty()) continue;
            qf = std::move(dispatch_queue_.front());
            dispatch_queue_.pop_front();
        }

        if (!qf.mf) continue;

        // Distribute to subscribers
        for (auto& fn : qf.callbacks) {
            fn(qf.mf.get());
        }
    }
}

void FrameRouter::retain(ManagedFrame* mf) {
    if (!mf) return;
    mf->ref_count.fetch_add(1, std::memory_order_relaxed);
}

void FrameRouter::release(ManagedFrame* mf) {
    if (!mf) return;

    int prev = mf->ref_count.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1) {
        do_release(mf);
    }
}

void FrameRouter::force_reclaim(uint64_t frame_id) {
    std::shared_ptr<ManagedFrame> mf;

    {
        std::lock_guard<std::mutex> lock(frames_mu_);
        auto it = outstanding_.find(frame_id);
        if (it == outstanding_.end()) return;
        mf = it->second;
        // Keep shared_ptr in outstanding_ — fd_publisher clients still hold raw pointers.
        // do_release() will erase it when ref_count reaches 0.
    }

    if (mf) {
        // Use compare_exchange to prevent double HAL release (race with do_release)
        bool expected = false;
        if (mf->reclaimed.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            HAL_LOG_WARNING("FrameRouter: Force reclaiming frame %lu (ref_count=%d)",
                           frame_id, mf->ref_count.load(std::memory_order_relaxed));

            source_->release_frame(mf->stream_name, &mf->frame);
            // Note: watchdog already untracked in scan_loop before calling reclaim_fn
        }

        {
            std::lock_guard<std::mutex> lock(stats_mu_);
            stats_[mf->stream_name].force_reclaimed++;
        }
    }
}

RouterStreamStats FrameRouter::get_stats(const std::string& stream_name) const {
    std::lock_guard<std::mutex> lock(stats_mu_);
    auto it = stats_.find(stream_name);
    if (it != stats_.end()) return it->second;
    return {};
}

bool FrameRouter::wait_next_frames(const std::string& stream_name, uint32_t count,
                                   std::chrono::milliseconds timeout) {
    const uint64_t requested = count == 0 ? 1 : count;
    std::unique_lock<std::mutex> lock(frame_clock_mu_);
    const uint64_t target = frame_sequences_[stream_name] + requested;
    const bool reached = frame_clock_cv_.wait_for(lock, timeout, [&] {
        return !running_.load() || frame_sequences_[stream_name] >= target;
    });
    return reached && running_.load() && frame_sequences_[stream_name] >= target;
}

uint64_t FrameRouter::frame_sequence(const std::string& stream_name) const {
    std::lock_guard<std::mutex> lock(frame_clock_mu_);
    const auto it = frame_sequences_.find(stream_name);
    return it == frame_sequences_.end() ? 0 : it->second;
}

void FrameRouter::do_release(ManagedFrame* mf) {
    // Use compare_exchange to prevent double HAL release (race with force_reclaim)
    bool expected = false;
    if (mf->reclaimed.compare_exchange_strong(expected, true,
            std::memory_order_acq_rel)) {
        // Normal path: first one to release this frame
        if (watchdog_) {
            watchdog_->untrack(mf->frame_id);
        }
        source_->release_frame(mf->stream_name, &mf->frame);
    }
    // If compare_exchange failed: force_reclaim already released the HAL buffer.

    // Always erase from outstanding_ when last ref drops.
    // This keeps the ManagedFrame alive until all subscribers release their refs.
    {
        std::lock_guard<std::mutex> lock(frames_mu_);
        outstanding_.erase(mf->frame_id);
    }
}

void FrameRouter::discard_queued_frame(const std::shared_ptr<ManagedFrame>& mf) {
    if (!mf) return;

    // A queued frame has not reached any subscriber, so retire every reference
    // assigned at enqueue time and release its underlying MediaLibrary buffer.
    int remaining = mf->ref_count.exchange(0, std::memory_order_acq_rel);
    if (remaining > 0) {
        bool expected = false;
        if (mf->reclaimed.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel)) {
            if (watchdog_) watchdog_->untrack(mf->frame_id);
            source_->release_frame(mf->stream_name, &mf->frame);
        }
    }

    std::lock_guard<std::mutex> lock(frames_mu_);
    outstanding_.erase(mf->frame_id);
}
