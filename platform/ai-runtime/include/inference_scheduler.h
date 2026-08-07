#pragma once

#include "model_manager.h"
#include "session_manager.h"
#include "common.h"
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <string>
#include <climits>

namespace aipc::ai_runtime {

class InferenceScheduler;  // forward declaration for WorkerCallbackState

struct InferRequest {
    std::string session_id;
    std::string model_id;
    HalTensor   inputs[HAL_MAX_TENSORS];
    int         num_inputs;
    uint32_t         priority;
    uint32_t         timeout_ms;
    TimePoint        enqueue_time;

    // Optional: hold onto resources (like FdGroup) until request completes
    std::shared_ptr<void> resource_holder;

    // Result callback: invoked from worker thread (sync mode) or HAL
    // completion thread (async mode).
    //   model_acquired: true if the worker successfully called
    //   acquire_model_snapshot (ref was bumped). The callback MUST call
    //   release_model() only when model_acquired is true.
    std::function<void(int rc, HalTensor* outputs, int num_outputs,
                       uint64_t infer_us, uint64_t queue_us,
                       bool model_acquired)> on_complete;

    // When true, on_complete takes ownership of the HAL output buffers: it
    // must call free_outputs() and release_model() when done (possibly
    // asynchronously via PostprocessPool). It must NOT delete the callback's
    // HalTensor* storage; that pointer may be owned by the HAL async context.
    // When false (default), the scheduler worker frees outputs and releases
    // the model ref after on_complete returns — the legacy synchronous path.
    bool owns_outputs = false;
};

/// State threaded through the HAL async callback via userdata.
/// Allocated on the heap per-submission; deleted in the callback.
struct WorkerCallbackState {
    InferenceScheduler*         scheduler;
    ModelManager*              mgr;
    SessionManager*            smgr;
    std::unique_ptr<InferRequest> req;
    HalTensor*                 outputs;
    int                        max_outputs;
    TimePoint                  infer_start;
    uint64_t                   queue_time_us;
    std::shared_ptr<Session>   session;
};

class InferenceScheduler {
public:
    InferenceScheduler(ModelManager* model_mgr,
                       SessionManager* session_mgr,
                       int num_workers,
                       int queue_capacity);
    ~InferenceScheduler();

    void start();
    /// Stop scheduler: sets running_=false, joins workers (they drain the
    /// queue before exiting), then drops any remaining queued requests
    /// without calling on_complete (they were never acquired by a worker,
    /// so calling on_complete would risk release_model on a non-existent
    /// ref). Callers handle dropped requests via their own timeout.
    /// Async HAL callbacks for in-flight run_async jobs are NOT waited
    /// for here — call drain_async() afterwards.
    void stop();

    /// Bounded drain of in-flight async callbacks (submitted via run_async
    /// but not yet completed). Call after stop() and before destroying
    /// dependent objects (ModelManager, PostprocessPool). Returns the number
    /// of orphaned jobs that did not complete within the timeout.
    int drain_async(int timeout_ms = 5000);

    /// Submit an inference request. Returns false if queue is full.
    bool submit(std::unique_ptr<InferRequest> req);

    int queue_depth() const;

private:
    void worker_loop(int worker_id);

    /// HAL async completion callback (static, fits HalInferenceAsyncCallback).
    /// Owns and deletes the WorkerCallbackState passed via userdata.
    static void on_hw_complete(HalTensor* outputs, int num_outputs,
                               int status, void* userdata);

    /// Common completion logic shared by sync and async paths.
    void handle_completion(int rc, HalTensor* outputs, int num_outputs,
                          uint64_t infer_us, uint64_t queue_us,
                          const std::string& model_id,
                          const std::string& session_id,
                          bool owns_outputs,
                          const std::shared_ptr<Session>& session);

    /// Called by on_hw_complete to decrement async_in_flight_ and notify drain.
    void notify_async_complete();

    ModelManager*    model_mgr_;
    SessionManager*  session_mgr_;
    int              num_workers_;
    int              queue_capacity_;

    mutable std::mutex          mu_;
    std::condition_variable  cv_;

    // Session-keyed fair queues. Selection uses weighted-deficit
    // round-robin: higher-priority sessions accumulate deficit faster,
    // but no session starves.
    std::unordered_map<std::string, std::queue<std::unique_ptr<InferRequest>>> session_queues_;
    std::unordered_map<std::string, int64_t> deficits_;  // per-session WDRR deficit
    int                      total_queued_{0};

    std::vector<std::thread> workers_;
    std::atomic<bool>        running_{false};

    // Track async jobs submitted via run_async whose HAL callbacks haven't
    // fired yet. Incremented on submit, decremented in on_hw_complete.
    // drain_async() waits for this to reach zero (bounded) so late callbacks
    // can safely access model_mgr_/session_mgr_ during shutdown.
    std::atomic<int>        async_in_flight_{0};
    std::condition_variable  async_drain_cv_;
    std::mutex              async_drain_mu_;
};

}  // namespace aipc::ai_runtime
