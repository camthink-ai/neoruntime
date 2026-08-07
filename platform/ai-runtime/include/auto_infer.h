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

namespace aipc::ai_runtime {

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

private:
    void pipeline_loop(const AutoInferPipeline& pipe);

    void publish_result(const std::string& stream_id,
                        const std::string& model_id,
                        uint64_t frame_seq,
                        uint64_t timestamp_ns,
                        const HalPostprocessResult& result);

    ModelManager*       model_mgr_;
    FdReceiver*         fd_receiver_;
    EventBusClient*     event_bus_;
    InferenceScheduler* scheduler_;
    SessionManager*     session_mgr_;
    PostprocessPool*    postprocess_pool_;
    const Config&       cfg_;

    std::vector<std::thread> threads_;
    std::atomic<bool> running_{false};
};

}  // namespace aipc::ai_runtime
