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

    // Registration identity (model_type + variant exactly as registered).
    // Co-ownership of the same id+path is only valid while these match: a
    // differing re-registration would have the gRPC layer's
    // init_post_process rewire the shared postprocess session to the new
    // configuration, corrupting decode for the incumbent owner(s).
    std::string  model_type;
    std::string  variant;

    // App-bundled model (extracted from an app image by app-manager).
    // Transient models are hidden from the model page: platform-api's
    // syncRuntimeModelsToDB skips them, so they never reach platform.db.
    // Set at first registration; co-ownership re-registrations keep the
    // stored value (a model already loaded under a visibility contract
    // keeps it).
    bool         transient = false;

    HalInferenceSession* infer_session = nullptr;  // HAL v2 inference session
    PostprocessSession   post_session;              // HAL v2 postprocess session

    HalModelInfo model_info{};
    // NPU batch the HAL session was configured with (HalInferenceConfig).
    // 1 = single-frame; >1 requires InferBatch (grouped into one NPU job).
    uint32_t     batch_size = 1;
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
    uint32_t               batch_size    = 1;
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
    /// If the model is already loaded by another owner from the SAME path,
    /// this just adds co-ownership; the same id under a different path is a
    /// collision and is refused (the incumbent's weights must not silently
    /// serve the new registrant). So is the same id+path with a different
    /// registration identity (model_type/variant): re-initializing the
    /// shared postprocess session to the new configuration would corrupt
    /// decode for the incumbent owner(s).
    /// transient marks an app-bundled model (hidden from the model page).
    /// variant is the model's postprocess variant blob; for detections its
    /// backend_function is forwarded to the HAL inference session so NMS output
    /// tensors are named after the selected vendor function, not the file path.
    /// batch_size configures the HAL inference session's NPU batch (>1 is only
    /// valid for batch-compiled HEFs — registration verifies the reported
    /// input byte_size equals batch x single-frame and refuses otherwise;
    /// InferBatch then groups B frames into one NPU job). The batch is part of
    /// the registration identity: a re-registration at a different batch is
    /// refused like any other identity mismatch.
    /// Returns 0 for a fresh registration, 1 when the identical entry was
    /// already loaded and only ownership changed, <0 on error; why (optional)
    /// carries the human-readable refusal reason.
    int register_model(const std::string& model_id, const std::string& model_path,
                       const std::string& owner_id = "",
                       bool transient = false,
                       const std::string& variant = "",
                       const std::string& model_type = "",
                       std::string* why = nullptr,
                       uint32_t batch_size = 1);

    /// Unregister (unload) a model. With owner_id, an absent owner is an
    /// idempotent no-op; other owners keep the model resident; the last owner
    /// is removed atomically with physical unload and is retained when a live
    /// inference ref_count refuses that unload. An empty owner_id requests a
    /// system-level unload but still respects ref_count. Returns 0 only when
    /// the physical model was removed, 1 for an idempotent/co-owner logical
    /// release that leaves it resident, and <0 on refusal.
    int unregister_model(const std::string& model_id, const std::string& owner_id = "");

    /// Unregister all models atomically when none has a live inference ref.
    /// Returns false without changing any registry state if any model is busy.
    /// Used before GenAI session creation to free NPU resources.
    bool force_unregister_all();

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
    /// callback may run before run_async() returns. The caller MUST keep
    /// @p inputs/@p outputs alive until both the callback and this call have
    /// returned. On a failing return or exception, the backend must not start a
    /// callback after unwinding, though a callback may already have completed.
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

    /// HAL inference ops table — for service-layer tensor manipulation that
    /// needs ops beyond the wrapped helpers (e.g. bind_dma_frame on
    /// buffer_id inputs). May be null only before load.
    const HalInferenceOps* infer_ops() const { return infer_ops_; }

    /// Tail-member availability under the ops-table ABI guard: pass
    /// offsetof(HalInferenceOps, <member>). With no loader (ops tables
    /// injected directly in tests) the table is assumed complete.
    bool has_infer_op(size_t member_offset) const {
        return loader_ ? loader_->has_infer_op(member_offset) : true;
    }

    /// Check if HAL supports async inference (run_async != nullptr).
    bool has_async() const;

    /// Update postprocess configuration at runtime (e.g., CLIP zero-shot prompts).
    /// Calls HAL's apply_config_json on the model's postprocess session.
    int update_postprocess_config(const std::string& model_id, const std::string& config_json);

    /// Records a post-process run failure for a model and returns whether the
    /// caller should emit a log line now. Rate-limits the journal: true on the
    /// first failure and every kPostFailLogInterval-th thereafter, false in
    /// between (a broken plugin fails per frame; logging every frame floods).
    /// When count_out is non-null it receives the failure count after this
    /// call (for the "failure #N" log line). The status flip on the response
    /// is NOT rate-limited — only logging is.
    bool note_post_failure(const std::string& model_id, int rc,
                           uint64_t* count_out = nullptr);

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

    // Post-process run failures per model id, for note_post_failure's
    // rate-limited logging. Guarded by mu_ (unique).
    std::unordered_map<std::string, uint64_t> post_fail_counts_;

    // Require mu_ held. Bump/drop the session refcount; destroy via HAL ops
    // only when the count reaches zero.
    void add_infer_locked(HalInferenceSession* s);
    void add_post_locked(HalPostprocessSession* s);
    void release_infer_locked(HalInferenceSession* s);
    void release_post_locked(HalPostprocessSession* s);
};

/// RAII guard prepared before model acquisition and armed only after the
/// acquisition succeeds. Copying the id therefore cannot leak an acquired ref.
struct ModelGuard {
    ModelManager* mgr = nullptr;
    std::string   id;
    bool          armed = false;

    ModelGuard(ModelManager* m, const std::string& i) : mgr(m), id(i) {}
    ~ModelGuard() { if (mgr && armed) mgr->release_model(id); }

    void arm() noexcept { armed = true; }
    void disarm() noexcept { armed = false; }

    ModelGuard(const ModelGuard&) = delete;
    ModelGuard& operator=(const ModelGuard&) = delete;
};

}  // namespace aipc::ai_runtime
