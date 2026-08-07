#pragma once

#include "inference.grpc.pb.h"
#include "model_manager.h"
#include "session_manager.h"
#include "inference_scheduler.h"
#include "fd_receiver.h"
#include "event_bus_client.h"
#include "postprocess_pool.h"
#include "config.h"
#include "model/hal_clip_text_encoder_ops.h"
#include "model/hal_genai.h"

#include <grpcpp/grpcpp.h>
#include <unordered_map>
#include <mutex>

namespace aipc::ai_runtime {

class AIRuntimeServiceImpl final
    : public aipc::inference::InferenceService::Service {
public:
    AIRuntimeServiceImpl(const Config& cfg,
                         ModelManager* model_mgr,
                         SessionManager* session_mgr,
                         InferenceScheduler* scheduler,
                         FdReceiver* fd_receiver,
                         EventBusClient* event_bus,
                         PostprocessPool* postprocess_pool,
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

    static aipc::inference::DataType hal_dtype_to_proto(HalDataType dt);
    static HalDataType proto_dtype_to_hal(aipc::inference::DataType dt);
    static std::string hal_layout_to_string(HalTensorLayout layout);

    // Completion callback for InferBatch async inference, threaded through the
    // HAL via userdata (an InferBatchCbState* defined in grpc_service.cpp).
    // Static so its address is a plain function pointer fit for
    // HalInferenceOps::run_async.
    static void InferBatchCallback(HalTensor* outputs, int num_outputs,
                                   int status, void* userdata);

    const Config&        cfg_;
    ModelManager*        model_mgr_;
    SessionManager*     session_mgr_;
    InferenceScheduler*  scheduler_;
    FdReceiver*          fd_receiver_;
    EventBusClient*      event_bus_;
    PostprocessPool*     postprocess_pool_;
    const HalClipTextEncoderOps* clip_enc_ops_;
    const HalGenaiOps* genai_ops_;
    std::mutex genai_mu_;
    std::unordered_map<std::string, HalGenaiSession*> genai_sessions_;
};

}  // namespace aipc::ai_runtime
