#pragma once

#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <functional>
#include <atomic>
#include <vector>

namespace aipc::ai_runtime {

/// Generic thread pool for offloading post-processing work (post_process,
/// result serialization, event-bus publishing) from inference threads.
///
/// Tasks are std::function<void()> lambdas. The pool does NOT own HAL
/// resources — each task is responsible for acquiring/releasing model
/// references and freeing HAL tensors as needed.
class PostprocessPool {
public:
    using Task = std::function<void()>;

    PostprocessPool(int num_workers, int queue_capacity);
    ~PostprocessPool();

    PostprocessPool(const PostprocessPool&) = delete;
    PostprocessPool& operator=(const PostprocessPool&) = delete;
    PostprocessPool(PostprocessPool&&) = delete;
    PostprocessPool& operator=(PostprocessPool&&) = delete;

    void start();
    void stop();

    /// Submit a post-processing task. Returns false if the queue is full
    /// or the pool is stopped. On false, @p task is NOT moved-from and the
    /// caller can execute it synchronously as a fallback.
    bool submit(Task& task);

    int queue_depth() const;

private:
    void worker_loop();

    int                          num_workers_;
    int                          queue_capacity_;

    mutable std::mutex           mu_;
    std::condition_variable      cv_;
    std::queue<Task>             tasks_;
    std::vector<std::thread>     workers_;
    std::atomic<bool>            running_{false};
};

}  // namespace aipc::ai_runtime
