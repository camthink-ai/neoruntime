#pragma once

#include "config.h"
#include "model_manager.h"
#include "fd_receiver.h"
#include "event_bus_client.h"
#include "inference_scheduler.h"
#include "session_manager.h"
#include "postprocess_pool.h"

#include <thread>
#include <vector>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <cstdint>

namespace aipc::ai_runtime {

struct AutoInferOutstandingWorkState {
    std::mutex mu;
    std::condition_variable cv;
    size_t count = 0;
};

class AutoInfer {
public:
    AutoInfer(ModelManager* model_mgr,
              FdReceiver* fd_receiver,
              EventBusClient* event_bus,
              InferenceScheduler* scheduler,
              SessionManager* session_mgr,
              PostprocessPool* postprocess_pool,
              const Config& cfg);
    ~AutoInfer();

    bool start();
    void stop();

    /// True when a started pipeline exited unexpectedly after startup and the
    /// runtime should shut down in a controlled, ordered fashion.
    bool failed() const noexcept {
        return pipeline_failed_.load(std::memory_order_acquire);
    }

private:
    enum class LifecycleState : uint8_t { Stopped, Starting, Running, Stopping };
    struct PipelineFrameState;
    struct PipelineRunState;

    void pipeline_thread_main(
        AutoInferPipeline pipe,
        std::shared_ptr<PipelineRunState> run_state) noexcept;
    void pipeline_loop(
        const AutoInferPipeline& pipe,
        const std::shared_ptr<PipelineRunState>& run_state,
        bool& startup_succeeded);
    void stop_locked(std::unique_lock<std::mutex>& lifecycle_lock);

    ModelManager*       model_mgr_;
    FdReceiver*         fd_receiver_;
    EventBusClient*     event_bus_;
    InferenceScheduler* scheduler_;
    SessionManager*     session_mgr_;
    PostprocessPool*    postprocess_pool_;
    const Config&       cfg_;

    std::mutex lifecycle_mu_;
    LifecycleState lifecycle_state_ = LifecycleState::Stopped;
    std::vector<std::thread> threads_;
    std::mutex pipeline_states_mu_;
    std::vector<std::weak_ptr<PipelineFrameState>> pipeline_states_;
    std::shared_ptr<PipelineRunState> pipeline_run_state_;
    std::shared_ptr<AutoInferOutstandingWorkState> outstanding_work_;
    std::atomic<bool> running_{false};
    std::atomic<bool> pipeline_failed_{false};
};

}  // namespace aipc::ai_runtime
