#pragma once

#include "hal_ml_loader.h"
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <shared_mutex>
#include <optional>
#include <vector>
#include <cstdint>
#include <ctime>

namespace aipc::ai_runtime {

// HAL v2 session types
struct PostprocessSession {
    HalPostprocessSession* session = nullptr;
    HalPostprocessType     type    = HAL_POST_TYPE_NONE;
};

struct ModelEntry {
    std::string  id;
    std::string  name;                  // Display name for the model
    std::string  path;

    HalInferenceSession* infer_session = nullptr;  // HAL v2 inference session
    PostprocessSession   post_session;              // HAL v2 postprocess session

    HalModelInfo model_info{};
    int          ref_count  = 0;
    int64_t      load_time  = 0;        // Unix timestamp
};

/// Lightweight snapshot of model fields needed for inference.
/// Safe to hold after lock release (value copy, no pointers into map).
struct ModelSnapshot {
    HalInferenceSession*   infer_session = nullptr;
    HalPostprocessSession* post_session  = nullptr;
    HalPostprocessType     post_type     = HAL_POST_TYPE_NONE;
    HalModelInfo           model_info{};
    int                    num_outputs   = 0;
};

/// Thread-safe model lifecycle manager backed by HAL ops.
class ModelManager {
public:
    ModelManager(const HalInferenceOps* infer_ops,
                 const HalPostprocessOps* post_ops = nullptr,
                 const HalDrawOps* draw_ops = nullptr,
                 const HalMlLoader* loader = nullptr,
                 const std::string& hal_platform_config = "");
    ~ModelManager();

    /// Register (load) a model. If owner_id is non-empty, it tracks ownership.
    /// If the model is already loaded by another owner, this just adds co-ownership.
    /// Returns 0 on success, <0 on error.
    int register_model(const std::string& model_id, const std::string& model_path,
                       const std::string& owner_id = "");

    /// Unregister (unload) a model. If owner_id is given, only removes that owner.
    /// The model is physically unloaded only when no owners remain AND ref_count == 0.
    int unregister_model(const std::string& model_id, const std::string& owner_id = "");

    /// Force unregister all models, ignoring ref_count.
    /// Destroys all HAL sessions and clears model registry.
    /// Used before GenAI session creation to free NPU resources.
    void force_unregister_all();

    /// Check if a specific owner has ownership of a model.
    bool is_owner(const std::string& model_id, const std::string& owner_id) const;

    /// Get all owners of a model.
    std::vector<std::string> get_owners(const std::string& model_id) const;

    /// Acquire a model (bump refcount). Returns nullptr if not found.
    /// WARNING: returned pointer may be invalidated by concurrent register_model (map rehash).
    /// Prefer acquire_model_snapshot() for lock-free usage.
    ModelEntry* acquire_model(const std::string& model_id);

    /// Acquire model snapshot (bump refcount + copy needed fields atomically).
    /// Safe to use after lock release. Call release_model() when done.
    std::optional<ModelSnapshot> acquire_model_snapshot(const std::string& model_id);

    /// Release (decrement refcount).
    void release_model(const std::string& model_id);

    /// Free HAL-allocated output tensor buffers after inference.
    void free_outputs(HalTensor* outputs, int num_outputs);

    /// Get a snapshot copy of a model entry (safe, no dangling pointer).
    /// Returns empty ModelEntry (id.empty()) if not found.
    ModelEntry get_model_copy(const std::string& model_id) const;

    /// List all models.
    std::vector<ModelEntry> list_models() const;

    /// Run synchronous inference.
    int infer(HalInferenceSession* session,
              const HalTensor* inputs, int num_inputs,
              HalTensor* outputs, int max_outputs);

    /// Submit asynchronous inference. The NPU scheduler runs the job and
    /// invokes @p callback (from a HailoRT thread) once outputs are ready; the
    /// caller MUST keep @p inputs/@p outputs alive until the callback fires.
    /// Returns HAL_OK (0) on submission success, <0 if the HAL has no async
    /// path or submission failed.
    int run_async(HalInferenceSession* session,
                  const HalTensor* inputs, int num_inputs,
                  HalTensor* outputs, int num_outputs,
                  HalInferenceAsyncCallback callback, void* userdata);

    /// Map frame buffer for models requiring CPU bound data (single tensor models like CLIP).
    int tensor_from_frame(const HalFrameBuffer* frame, HalTensor* tensor);
    
    /// Free a tensor mapped via tensor_from_frame.
    void free_tensor(HalTensor* tensor);

    /// Initialize post-processing for a model (must be called after register_model).
    /// model_type: "detection", "landmarks", "segmentation", "classification"
    /// variant: e.g. "yolov8n", "yolov8s" (for detection only, may be empty)
    int init_post_process(const std::string& model_id,
                          const std::string& model_type,
                          const std::string& variant = "");

    /// Run post-processing on inference outputs.
    /// Returns 0 on success. Fills result with detections/classifications/landmarks.
    int post_process(HalPostprocessSession* session,
                     const HalTensor* outputs, int num_outputs,
                     HalPostprocessResult* result);

    /// Free malloc-owned fields in a postprocess result (e.g. depth_m, mask_data).
    /// MUST be called once per result returned by post_process() after the caller
    /// has copied out any data it needs (e.g. fill_proto_post_result). Safe on a
    /// zero-initialized result. Without this, depth/segmentation backends leak
    /// their per-inference allocations (~3.3GB/h for per-frame depth inference).
    void free_post_result(HalPostprocessResult* result);

    bool has_post_ops() const;

    /// Check if HAL supports async inference (run_async != nullptr).
    bool has_async() const;

    /// Update postprocess configuration at runtime (e.g., CLIP zero-shot prompts).
    /// Calls HAL's apply_config_json on the model's postprocess session.
    int update_postprocess_config(const std::string& model_id, const std::string& config_json);

    /// Query system-level NPU/CPU performance stats from HAL.
    /// Returns 0 on success, <0 if unavailable.
    int query_performance_stats(uint32_t sampling_period_ms, HalInferencePerfStats* out);

    /// Query per-session hardware performance stats (FPS, hw latency) from HAL.
    /// Returns 0 on success, <0 if unavailable.
    int query_session_stats(const std::string& model_id, uint32_t sampling_ms,
                            HalInferenceSessionPerfStats* out);

private:
    const HalInferenceOps*   infer_ops_;
    const HalPostprocessOps* post_ops_;
    const HalDrawOps*        draw_ops_;
    const HalMlLoader*       loader_;
    std::string              hal_platform_config_;
    mutable std::shared_mutex mu_;
    std::unordered_map<std::string, ModelEntry> models_;
    std::unordered_map<std::string, std::unordered_set<std::string>> owners_; // model_id -> {owner_ids}

    // Physical HAL sessions can be shared by multiple model entries: the
    // path-alias branch in register_model() copies a ModelEntry, so two ids
    // end up pointing at the same infer_session / post_session. Without
    // refcounting, unregister_model and the destructor/force_unregister_all
    // (which visit every entry) double-destroy the shared session -> UAF.
    // These maps track one refcount per live session pointer so it is
    // physically destroyed exactly once, when the last entry releases it.
    std::unordered_map<HalInferenceSession*, int>   infer_refs_;  // infer_session -> refcount
    std::unordered_map<HalPostprocessSession*, int> post_refs_;   // post_session  -> refcount

    // Require mu_ held. Bump/drop the session refcount; destroy via HAL ops
    // only when the count reaches zero.
    void add_infer_locked(HalInferenceSession* s);
    void add_post_locked(HalPostprocessSession* s);
    void release_infer_locked(HalInferenceSession* s);
    void release_post_locked(HalPostprocessSession* s);
};

/// RAII guard: releases model ref_count on destruction.
struct ModelGuard {
    ModelManager* mgr = nullptr;
    std::string   id;
    ModelGuard(ModelManager* m, const std::string& i) : mgr(m), id(i) {}
    ~ModelGuard() { if (mgr) mgr->release_model(id); }
    ModelGuard(const ModelGuard&) = delete;
    ModelGuard& operator=(const ModelGuard&) = delete;
};

}  // namespace aipc::ai_runtime
