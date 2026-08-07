#include "postprocess_pool.h"
#include "log.h"

namespace aipc::ai_runtime {

PostprocessPool::PostprocessPool(int num_workers, int queue_capacity)
    : num_workers_(num_workers > 0 ? num_workers : 1)
    , queue_capacity_(queue_capacity > 0 ? queue_capacity : 32) {}

PostprocessPool::~PostprocessPool() {
    stop();
}

void PostprocessPool::start() {
    if (running_.exchange(true)) return;

    LOG_INFO("Starting postprocess pool: %d workers, queue=%d",
             num_workers_, queue_capacity_);

    for (int i = 0; i < num_workers_; i++) {
        workers_.emplace_back(&PostprocessPool::worker_loop, this);
    }
}

void PostprocessPool::stop() {
    if (!running_.exchange(false)) return;

    LOG_INFO("Stopping postprocess pool");
    cv_.notify_all();

    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();
}

bool PostprocessPool::submit(Task& task) {
    if (!running_) return false;

    std::lock_guard lock(mu_);
    if (static_cast<int>(tasks_.size()) >= queue_capacity_) {
        LOG_WARN("Postprocess queue full (%d), falling back to sync",
                 queue_capacity_);
        return false;
    }
    tasks_.push(std::move(task));
    cv_.notify_one();
    return true;
}

int PostprocessPool::queue_depth() const {
    std::lock_guard lock(mu_);
    return static_cast<int>(tasks_.size());
}

void PostprocessPool::worker_loop() {
    // Drain on shutdown: keep processing until both running_ is false AND
    // the queue is empty. This ensures all in-flight post-process tasks
    // (which may hold HAL output refs and model refcounts) complete before
    // the pool stops, preventing resource leaks during shutdown.
    while (true) {
        Task task;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this] { return !tasks_.empty() || !running_; });
            if (tasks_.empty() && !running_) break;
            if (tasks_.empty()) continue;

            task = std::move(tasks_.front());
            tasks_.pop();
        }

        try {
            task();
        } catch (const std::exception& e) {
            LOG_ERROR("Postprocess task exception: %s", e.what());
        } catch (...) {
            LOG_ERROR("Postprocess task unknown exception");
        }
    }

    LOG_DEBUG("Postprocess worker stopped");
}

}  // namespace aipc::ai_runtime
