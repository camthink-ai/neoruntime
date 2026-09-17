#pragma once

#include "inference.grpc.pb.h"
#include "model_manager.h"
#include "session_manager.h"
#include "inference_scheduler.h"
#include "fd_receiver.h"
#include "buffer_lookup.h"
#include "event_bus_client.h"
#include "postprocess_pool.h"
#include "stream_infer_utils.h"
#include "config.h"
#include "model/hal_clip_text_encoder_ops.h"
#include "model/hal_genai.h"

#include <grpcpp/grpcpp.h>
#include <future>
#include <memory>
#include <unordered_map>
#include <vector>
#include <mutex>

namespace aipc::ai_runtime {

class DspClient;
class StreamPreprocessPool;

class AIRuntimeServiceImpl final
    : public aipc::inference::InferenceService::Service {
public:
    AIRuntimeServiceImpl(const Config& cfg,
                         ModelManager* model_mgr,
                         SessionManager* session_mgr,
                         InferenceScheduler* scheduler,
                         FdReceiver* fd_receiver,
                         BufferLookupClient* buffer_lookup,
                         EventBusClient* event_bus,
                         PostprocessPool* postprocess_pool,
                         DspClient* dsp_client,
                         const HalClipTextEncoderOps* clip_enc_ops,
                         const HalGenaiOps* genai_ops);

    grpc::Status RegisterModel(
        grpc::ServerContext* ctx,
        const aipc::inference::ModelRegisterRequest* req,
        aipc::inference::ModelRegisterResponse* resp) override;

    grpc::Status UnregisterModel(
        grpc::ServerContext* ctx,
        const aipc::inference::ModelInfo* req,
        aipc::inference::Status* resp) override;

    grpc::Status ListModels(
        grpc::ServerContext* ctx,
        const aipc::inference::Empty* req,
        aipc::inference::ModelListResponse* resp) override;

    grpc::Status GetModelInfo(
        grpc::ServerContext* ctx,
        const aipc::inference::ModelInfo* req,
        aipc::inference::ModelInfo* resp) override;

    grpc::Status Infer(
        grpc::ServerContext* ctx,
        const aipc::inference::InferRequest* req,
        aipc::inference::InferResponse* resp) override;

    grpc::Status InferBatch(
        grpc::ServerContext* ctx,
        const aipc::inference::InferBatchRequest* req,
        aipc::inference::InferBatchResponse* resp) override;

    grpc::Status StreamInfer(
        grpc::ServerContext* ctx,
        const aipc::inference::StreamInferRequest* req,
        grpc::ServerWriter<aipc::inference::StreamInferResponse>* writer) override;

    grpc::Status CreateSession(
        grpc::ServerContext* ctx,
        const aipc::inference::SessionConfig* req,
        aipc::inference::SessionCreateResponse* resp) override;

    grpc::Status DestroySession(
        grpc::ServerContext* ctx,
        const aipc::inference::SessionConfig* req,
        aipc::inference::Status* resp) override;

    grpc::Status GetStats(
        grpc::ServerContext* ctx,
        const aipc::inference::Empty* req,
        aipc::inference::SystemStats* resp) override;

    grpc::Status UpdatePostprocessConfig(
        grpc::ServerContext* ctx,
        const aipc::inference::UpdatePostprocessConfigRequest* req,
        aipc::inference::UpdatePostprocessConfigResponse* resp) override;

    grpc::Status EncodeText(
        grpc::ServerContext* ctx,
        const aipc::inference::EncodeTextRequest* req,
        aipc::inference::EncodeTextResponse* resp) override;

    // GenAI (LLM/VLM)
    grpc::Status GenaiCreateSession(
        grpc::ServerContext* ctx,
        const aipc::inference::GenaiCreateSessionRequest* req,
        aipc::inference::GenaiCreateSessionResponse* resp) override;

    grpc::Status GenaiDestroySession(
        grpc::ServerContext* ctx,
        const aipc::inference::GenaiCreateSessionRequest* req,
        aipc::inference::Status* resp) override;

    grpc::Status GenaiGenerate(
        grpc::ServerContext* ctx,
        const aipc::inference::GenaiGenerateRequest* req,
        grpc::ServerWriter<aipc::inference::GenaiGenerateResponse>* writer) override;

    grpc::Status GenaiAbort(
        grpc::ServerContext* ctx,
        const aipc::inference::GenaiAbortRequest* req,
        aipc::inference::Status* resp) override;

private:
    void publish_result(const std::string& stream_id,
                        const std::string& model_id,
                        uint64_t frame_seq,
                        uint64_t timestamp_ns,
                        const aipc::inference::PostResult& post_result);

    // P2-13 lifecycle sessions: broadcast "<result-topic-prefix>session/end"
    // with metadata {"session_id": client_session_id}. The camera-daemon
    // overlay subscriber exact-matches that topic and sweeps every polygon
    // sidecar tagged with the session. No-op for an empty tag (untagged
    // sessions have nothing tagged to sweep) or when the bus is down.
    void publish_session_end(const std::string& client_session_id);

    static aipc::inference::DataType hal_dtype_to_proto(HalDataType dt);
    static HalDataType proto_dtype_to_hal(aipc::inference::DataType dt);
    static std::string hal_layout_to_string(HalTensorLayout layout);

    // Copies HAL tensor specs onto a protobuf ModelInfo. Shared by
    // ListModels and GetModelInfo so the list path carries the same
    // per-tensor facts as the detail path.
    static void fill_tensor_specs(const HalModelInfo& mi,
                                  aipc::inference::ModelInfo* info);

    // Completion callback for InferBatch async inference, threaded through the
    // HAL via userdata (an InferBatchCbState* defined in grpc_service.cpp).
    // Static so its address is a plain function pointer fit for
    // HalInferenceOps::run_async.
    static void InferBatchCallback(HalTensor* outputs, int num_outputs,
                                   int status, void* userdata) noexcept;

    std::shared_ptr<StreamPreprocessPool> acquire_stream_preprocess_pool(
        uint32_t width, uint32_t height);

    const Config&        cfg_;
    StreamAdmissionController stream_admission_;
    ModelManager*        model_mgr_;
    SessionManager*     session_mgr_;
    InferenceScheduler*  scheduler_;
    FdReceiver*          fd_receiver_;
    BufferLookupClient*  buffer_lookup_;
    EventBusClient*      event_bus_;
    PostprocessPool*     postprocess_pool_;
    DspClient*           dsp_client_;      // stream DSP preprocess
    std::mutex            dsp_pool_mu_;
    using StreamPoolPtr = std::shared_ptr<StreamPreprocessPool>;
    std::unordered_map<uint64_t, std::weak_ptr<StreamPreprocessPool>>
        dsp_pools_;                         // current pool per geometry
    std::vector<std::weak_ptr<StreamPreprocessPool>>
        dsp_pool_instances_;                // counts retired pools until destroyed
    std::unordered_map<uint64_t, std::shared_future<StreamPoolPtr>>
        dsp_pool_inits_;                    // one allocator per geometry
    const HalClipTextEncoderOps* clip_enc_ops_;
    const HalGenaiOps* genai_ops_;
    std::mutex genai_mu_;
    std::unordered_map<std::string, HalGenaiSession*> genai_sessions_;
};

}  // namespace aipc::ai_runtime
