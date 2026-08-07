#include "model_manager.h"
#include "log.h"
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <algorithm>

namespace aipc::ai_runtime {

// ============================================================
// Constructor / Destructor
// ============================================================

ModelManager::ModelManager(const HalInferenceOps* infer_ops,
                           const HalPostprocessOps* post_ops,
                           const HalDrawOps* draw_ops,
                           const HalMlLoader* loader,
                           const std::string& hal_platform_config)
    : infer_ops_(infer_ops), post_ops_(post_ops), draw_ops_(draw_ops),
      loader_(loader), hal_platform_config_(hal_platform_config) {}

ModelManager::~ModelManager() {
    std::unique_lock lock(mu_);
    for (auto& [id, entry] : models_) {
        release_post_locked(entry.post_session.session);
        release_infer_locked(entry.infer_session);
    }
    models_.clear();
    infer_refs_.clear();
    post_refs_.clear();
}

// session refcount helpers — require mu_ held.
void ModelManager::add_infer_locked(HalInferenceSession* s) {
    if (s) ++infer_refs_[s];
}

void ModelManager::add_post_locked(HalPostprocessSession* s) {
    if (s) ++post_refs_[s];
}

void ModelManager::release_infer_locked(HalInferenceSession* s) {
    if (!s || !infer_ops_ || !infer_ops_->destroy) return;
    auto it = infer_refs_.find(s);
    if (it == infer_refs_.end()) return;
    if (--it->second <= 0) {
        infer_ops_->destroy(s);
        infer_refs_.erase(it);
    }
}

void ModelManager::release_post_locked(HalPostprocessSession* s) {
    if (!s || !post_ops_ || !post_ops_->destroy) return;
    auto it = post_refs_.find(s);
    if (it == post_refs_.end()) return;
    if (--it->second <= 0) {
        post_ops_->destroy(s);
        post_refs_.erase(it);
    }
}

bool ModelManager::has_post_ops() const {
    return post_ops_ != nullptr;
}

bool ModelManager::has_async() const {
    return infer_ops_ && infer_ops_->run_async;
}

// ============================================================
// register_model
// ============================================================

int ModelManager::register_model(const std::string& model_id,
                                 const std::string& model_path,
                                 const std::string& owner_id) {
    std::unique_lock lock(mu_);

    if (models_.count(model_id)) {
        // Model already loaded — add co-ownership if owner_id is provided
        if (!owner_id.empty()) {
            owners_[model_id].insert(owner_id);
            LOG_INFO("Model %s: added co-owner '%s' (total owners: %zu)",
                     model_id.c_str(), owner_id.c_str(), owners_[model_id].size());
            return 0;
        }

        LOG_INFO("Model %s already loaded, skipping", model_id.c_str());
        return 0;
    }

    // Check if the same file is already loaded under a different model_id
    for (const auto& [existing_id, entry] : models_) {
        if (entry.path == model_path) {
            LOG_INFO("Model file %s already loaded as '%s', aliasing as '%s'",
                     model_path.c_str(), existing_id.c_str(), model_id.c_str());
            // Create an alias entry under the new model_id. The alias shares the
            // original's HAL sessions (shallow copy), so bump the shared
            // refcounts — the physical session must outlive both ids.
            HalInferenceSession*   shared_infer = entry.infer_session;
            HalPostprocessSession* shared_post  = entry.post_session.session;
            ModelEntry alias = entry;
            alias.id   = model_id;   // alias must carry its own id, not the original's
            alias.name = model_id;   // display name must match the alias id
            models_.emplace(model_id, alias);
            add_infer_locked(shared_infer);
            add_post_locked(shared_post);
            if (!owner_id.empty()) {
                owners_[model_id].insert(owner_id);
            }
            return 0;
        }
    }

    // HAL v2: session-based inference
    HalInferenceConfig infer_cfg{};
    std::strncpy(infer_cfg.model_path, model_path.c_str(), HAL_MAX_MODEL_PATH - 1);
    infer_cfg.batch_size = 1;
    infer_cfg.timeout_ms = 5000;
    infer_cfg.use_dma = true;

    // Pass scheduler config for shared VDevice + round-robin scheduling
    if (!hal_platform_config_.empty()) {
        infer_cfg.platform_config = hal_platform_config_.c_str();
    }

    HalInferenceSession* session = infer_ops_->create(&infer_cfg);
    if (!session) {
        LOG_ERROR("HAL Inference create failed for %s", model_id.c_str());
        return -1;
    }

    // Get model info and create entry
    ModelEntry entry;
    entry.id        = model_id;
    entry.name      = model_id;
    entry.path      = model_path;
    entry.ref_count = 0;
    entry.load_time = std::time(nullptr);

    entry.infer_session = session;
    if (infer_ops_->get_model_info) {
        infer_ops_->get_model_info(session, &entry.model_info);
    }
    LOG_INFO("Model registered: %s (session=%p, owner=%s)",
             model_id.c_str(), (void*)session,
             owner_id.empty() ? "<system>" : owner_id.c_str());

    models_.emplace(model_id, std::move(entry));
    add_infer_locked(session);  // first reference to the freshly created session

    // Track ownership
    if (!owner_id.empty()) {
        owners_[model_id].insert(owner_id);
    }

    return 0;
}

// ============================================================
// init_post_process
// ============================================================

int ModelManager::init_post_process(const std::string& model_id,
                                    const std::string& model_type,
                                    const std::string& variant) {
    std::string t = model_type;
    std::transform(t.begin(), t.end(), t.begin(), ::tolower);

    // HAL v2: create a postprocess session (decoupled from inference)
    if (!post_ops_ || !post_ops_->create) {
        LOG_WARN("Post-process ops not available, skipping init for %s", model_id.c_str());
        return -1;
    }

    HalPostprocessConfig pp_cfg{};

    if (t == "detection" || t == "yolo") {
        pp_cfg.type = HAL_POST_TYPE_DETECTION;
        hal_detection_config_init(&pp_cfg.config.detection);
        pp_cfg.config.detection.confidence_threshold = 0.25f;
        pp_cfg.config.detection.nms_threshold = 0.45f;
        pp_cfg.config.detection.max_detections = 64;
    } else if (t == "landmarks" || t == "keypoint") {
        pp_cfg.type = HAL_POST_TYPE_KEYPOINT;
        hal_keypoint_config_init(&pp_cfg.config.keypoint);
        pp_cfg.config.keypoint.confidence_threshold = 0.25f;
    } else if (t == "segmentation") {
        pp_cfg.type = HAL_POST_TYPE_SEGMENTATION;
        hal_segmentation_config_init(&pp_cfg.config.segmentation);
        pp_cfg.config.segmentation.confidence_threshold = 0.25f;
    } else if (t == "classification") {
        pp_cfg.type = HAL_POST_TYPE_CLASSIFICATION;
        hal_classification_config_init(&pp_cfg.config.classification);
        pp_cfg.config.classification.confidence_threshold = 0.25f;
        pp_cfg.config.classification.top_k = 5;
    } else if (t == "clip") {
        pp_cfg.type = HAL_POST_TYPE_CLIP;
        hal_clip_postprocess_config_init(&pp_cfg.config.clip);
    } else if (t == "embedding") {
        pp_cfg.type = HAL_POST_TYPE_EMBEDDING;
        hal_embedding_config_init(&pp_cfg.config.embedding);
    } else if (t == "depth" || t == "monocular_depth" || t == "scdepth") {
        pp_cfg.type = HAL_POST_TYPE_DEPTH;
        hal_depth_config_init(&pp_cfg.config.depth);
    } else if (t == "ocr_detection") {
        pp_cfg.type = HAL_POST_TYPE_OCR_DETECTION;
        hal_ocr_detection_post_config_init(&pp_cfg.config.ocr_detection);
        pp_cfg.config.ocr_detection.det_bin_thresh = 0.15f;
        pp_cfg.config.ocr_detection.det_box_thresh = 0.06f;
        pp_cfg.config.ocr_detection.det_unclip_ratio = 2.0f;
        pp_cfg.config.ocr_detection.det_max_candidates = 64;
        pp_cfg.config.ocr_detection.min_confidence = 0.3f;
    } else if (t == "ocr_recognition") {
        pp_cfg.type = HAL_POST_TYPE_OCR_RECOGNITION;
        hal_ocr_recognition_post_config_init(&pp_cfg.config.ocr_recognition);
        {
            const char *default_charset = "/data/models/ocr/ocr_dict.txt";
            struct stat st;
            if (stat(default_charset, &st) == 0) {
                strncpy(pp_cfg.config.ocr_recognition.charset_path,
                        default_charset,
                        sizeof(pp_cfg.config.ocr_recognition.charset_path) - 1);
                // Charset file has no blank entry at index 0, so model class 1
                // maps to charset[0]. Without this offset, all chars shift by 1.
                pp_cfg.config.ocr_recognition.charset_index_offset = 1;
                LOG_INFO("OCR recognition charset: %s (offset=%d)",
                         default_charset, pp_cfg.config.ocr_recognition.charset_index_offset);
            } else {
                LOG_WARN("OCR charset not found at %s, using default 99-char set", default_charset);
            }
        }
    } else {
        LOG_WARN("Unknown model type '%s', defaulting to detection", model_type.c_str());
        pp_cfg.type = HAL_POST_TYPE_DETECTION;
        hal_detection_config_init(&pp_cfg.config.detection);
        pp_cfg.config.detection.confidence_threshold = 0.25f;
        pp_cfg.config.detection.nms_threshold = 0.45f;
        pp_cfg.config.detection.max_detections = 64;
    }

    // Detection variant → vendor backend_function. The Hailo postprocess .so
    // exports one function per model family (e.g. "yolov5m_vehicles",
    // "hailo_yolov8n"). The default chosen in hailo15_postprocess_impl.cpp
    // ("hailo_yolov8n") only matches yolov8n HEFs: a yolov5 HEF's nms output
    // tensor has a different name and the default function throws
    // std::invalid_argument (caught in grpc_service.cpp on_complete + post_task
    // now, but the inference still fails). When the caller supplies a variant
    // naming the matching backend function, route it through config_json so
    // HAL's create picks it up (see hailo15_postprocess_impl.cpp:1228) instead
    // of the hailo_yolov8n default.
    //
    // IMPORTANT: a bare {"backend_function":"<name>} is SCHEMA-INVALID for the
    // YOLO postprocess plugin. HAL strips the backend_* loader keys
    // (json_strip_hailo_postprocess_loader_keys) and writes the remainder to a
    // temp file the plugin validates with validate_json_with_schema — an empty
    // remainder {} fails ("Invalid keyword: required"), so the plugin falls
    // back to the default hailo_yolov8n and the tensor mismatch persists.
    // Therefore a caller MUST supply a FULL config_json blob (backend_function
    // + the YOLO schema fields: iou_threshold, detection_threshold,
    // output_activation, label_offset, max_boxes, labels — see
    // /home/root/apps/shared/resources/configs/yolov8.json). Two accepted
    // shapes for `variant`:
    //   1. Full JSON blob (first char '{') → used verbatim as config_json.
    //   2. Bare backend_function name → legacy {"backend_function":"<name>"}
    //      (schema-invalid; kept for backward compat, logs a warning).
    // The local string only needs to outlive the create() call below, which
    // copies what it needs into merged_vendor_json.
    std::string detection_cfg_json;
    if (pp_cfg.type == HAL_POST_TYPE_DETECTION && !variant.empty()) {
        if (variant.front() == '{') {
            detection_cfg_json = variant;
        } else {
            LOG_WARN("init_post_process: variant='%s' is a bare backend_function "
                     "name — config_json {\"backend_function\":\"%s\"} is "
                     "schema-invalid for the YOLO plugin (will fall back to "
                     "hailo_yolov8n). Pass a full config_json blob instead.",
                     variant.c_str(), variant.c_str());
            detection_cfg_json = std::string("{\"backend_function\":\"") +
                                 variant + "\"}";
        }
        pp_cfg.config.detection.config_json = detection_cfg_json.c_str();
    }
    // Keypoint variant → native YOLOv8-Pose config_json. The built-in keypoint
    // decoder has no "backend_function"; selection is driven by the
    // native_yolov8_pose flag inside a full config_json blob (see
    // hal_v2/examples/ai_example_v2/data/yolov8_pose_native_post.example.json).
    // HAL's create() copies config_json into merged_vendor_json
    // (hailo15_postprocess_impl.cpp:1067,1079) where the create-time
    // native_yolov8_pose check (ibid. 1127) flips yolov8_pose_builtin, selecting
    // the built-in COCO-17 decoder instead of the facial_landmarks_nv12
    // default. update_postprocess_config (apply_config_json) does NOT handle
    // native_yolov8_pose and does not re-run backend selection, so the flag
    // MUST be supplied here at create time. A bare variant name carries no
    // config and keeps the facial_landmarks_nv12 default — which cannot parse
    // COCO-17 pose HEFs (silent postprocess failure → no results). The app
    // therefore MUST pass a JSON blob as the variant for pose models.
    std::string keypoint_cfg_json;
    if (pp_cfg.type == HAL_POST_TYPE_KEYPOINT && !variant.empty()) {
        if (variant.front() == '{') {
            keypoint_cfg_json = variant;
        } else {
            LOG_WARN("init_post_process: keypoint variant='%s' is a bare name — "
                     "native_yolov8_pose cannot be set without a full config_json "
                     "blob; facial_landmarks_nv12 default will be used (fails on "
                     "COCO-17 pose HEFs). Pass a JSON blob instead.",
                     variant.c_str());
        }
        if (!keypoint_cfg_json.empty())
            pp_cfg.config.keypoint.config_json = keypoint_cfg_json.c_str();
    }
    LOG_INFO("init_post_process: model_id=%s type=%s variant='%s' cfg_json='%s' kp_cfg_json='%s'",
             model_id.c_str(), model_type.c_str(), variant.c_str(),
             detection_cfg_json.c_str(), keypoint_cfg_json.c_str());

    HalPostprocessSession* pp_session = post_ops_->create(&pp_cfg);
    if (!pp_session) {
        LOG_ERROR("HAL POSTPROCESS create failed for %s", model_id.c_str());
        return -1;
    }

    {
        std::unique_lock lock(mu_);
        auto it = models_.find(model_id);
        if (it == models_.end()) {
            post_ops_->destroy(pp_session);
            return -1;
        }
        // Replace any existing postprocess session: release the shared refcount
        // (destroyed only when no entry references it) and track the new one.
        release_post_locked(it->second.post_session.session);
        it->second.post_session.session = pp_session;
        it->second.post_session.type = pp_cfg.type;
        add_post_locked(pp_session);
    }

    LOG_INFO("Post-process initialized: %s (type=%s)", model_id.c_str(), model_type.c_str());
    return 0;
}

// ============================================================
// update_postprocess_config
// ============================================================

int ModelManager::update_postprocess_config(const std::string& model_id,
                                             const std::string& config_json) {
    std::unique_lock lock(mu_);
    auto it = models_.find(model_id);
    if (it == models_.end()) return -1;
    if (!it->second.post_session.session) return -2;
    if (!post_ops_ || !post_ops_->apply_config_json) return -3;
    int rc = post_ops_->apply_config_json(it->second.post_session.session, config_json.c_str());
    if (rc == 0) {
        LOG_INFO("Postprocess config updated for %s", model_id.c_str());
    } else {
        LOG_WARN("Postprocess config update failed for %s: rc=%d", model_id.c_str(), rc);
    }
    return rc;
}

// ============================================================
// unregister_model
// ============================================================

int ModelManager::unregister_model(const std::string& model_id,
                                   const std::string& owner_id) {
    std::unique_lock lock(mu_);

    auto it = models_.find(model_id);
    if (it == models_.end()) return -1;

    // If owner_id is provided, remove only that owner
    if (!owner_id.empty()) {
        auto oit = owners_.find(model_id);
        if (oit != owners_.end()) {
            oit->second.erase(owner_id);
            LOG_INFO("Model %s: removed owner '%s' (remaining owners: %zu)",
                     model_id.c_str(), owner_id.c_str(), oit->second.size());

            // If other owners remain, don't unload
            if (!oit->second.empty()) {
                return 0;
            }
            // Clean up empty owner set
            owners_.erase(oit);
        }
    }

    // Check ref_count before physical unload
    if (it->second.ref_count > 0) {
        LOG_ERROR("Cannot unregister %s: ref_count=%d (still in use by active sessions)",
                  model_id.c_str(), it->second.ref_count);
        return -1;
    }

    // HAL v2: release shared sessions (physically destroyed at refcount 0, so
    // aliases of the same model file keep working after one id is removed).
    release_post_locked(it->second.post_session.session);
    release_infer_locked(it->second.infer_session);

    models_.erase(it);
    owners_.erase(model_id);
    LOG_INFO("Model unregistered: %s", model_id.c_str());
    return 0;
}

// ============================================================
// Force unregister all
// ============================================================

void ModelManager::force_unregister_all() {
    std::unique_lock lock(mu_);
    for (auto& [id, entry] : models_) {
        release_post_locked(entry.post_session.session);
        release_infer_locked(entry.infer_session);
        LOG_INFO("Force unloaded model %s (ref_count was %d)",
                 id.c_str(), entry.ref_count);
    }
    models_.clear();
    owners_.clear();
    infer_refs_.clear();
    post_refs_.clear();
}

// ============================================================
// Ownership helpers
// ============================================================

bool ModelManager::is_owner(const std::string& model_id,
                            const std::string& owner_id) const {
    std::shared_lock lock(mu_);
    auto it = owners_.find(model_id);
    if (it == owners_.end()) return false;
    return it->second.count(owner_id) > 0;
}

std::vector<std::string> ModelManager::get_owners(const std::string& model_id) const {
    std::shared_lock lock(mu_);
    auto it = owners_.find(model_id);
    if (it == owners_.end()) return {};
    return {it->second.begin(), it->second.end()};
}

ModelEntry* ModelManager::acquire_model(const std::string& model_id) {
    std::unique_lock lock(mu_);
    auto it = models_.find(model_id);
    if (it == models_.end()) return nullptr;
    it->second.ref_count++;
    return &it->second;
}

std::optional<ModelSnapshot> ModelManager::acquire_model_snapshot(const std::string& model_id) {
    std::unique_lock lock(mu_);
    auto it = models_.find(model_id);
    if (it == models_.end()) return std::nullopt;
    it->second.ref_count++;
    ModelSnapshot snap;
    snap.infer_session = it->second.infer_session;
    snap.post_session  = it->second.post_session.session;
    snap.post_type     = it->second.post_session.type;
    snap.model_info    = it->second.model_info;
    snap.num_outputs   = static_cast<int>(it->second.model_info.num_outputs);
    if (snap.num_outputs <= 0) snap.num_outputs = 1;
    return snap;
}

void ModelManager::release_model(const std::string& model_id) {
    std::unique_lock lock(mu_);
    auto it = models_.find(model_id);
    if (it != models_.end() && it->second.ref_count > 0) {
        it->second.ref_count--;
    }
}

ModelEntry ModelManager::get_model_copy(const std::string& model_id) const {
    std::shared_lock lock(mu_);
    auto it = models_.find(model_id);
    if (it != models_.end()) return it->second;
    return {};
}

std::vector<ModelEntry> ModelManager::list_models() const {
    std::shared_lock lock(mu_);
    std::vector<ModelEntry> result;
    result.reserve(models_.size());
    for (auto& [id, entry] : models_) {
        result.push_back(entry);
    }
    return result;
}

// ============================================================
// Inference
// ============================================================

int ModelManager::infer(HalInferenceSession* session,
                        const HalTensor* inputs, int num_inputs,
                        HalTensor* outputs, int max_outputs) {
    if (!infer_ops_ || !infer_ops_->run) return -1;
    return infer_ops_->run(session, inputs, num_inputs, outputs, max_outputs);
}

int ModelManager::run_async(HalInferenceSession* session,
                            const HalTensor* inputs, int num_inputs,
                            HalTensor* outputs, int num_outputs,
                            HalInferenceAsyncCallback callback, void* userdata) {
    if (!infer_ops_ || !infer_ops_->run_async) return -1;
    return infer_ops_->run_async(session, inputs, num_inputs, outputs, num_outputs,
                                 callback, userdata);
}

int ModelManager::tensor_from_frame(const HalFrameBuffer* frame, HalTensor* tensor) {
    if (!infer_ops_ || !infer_ops_->tensor_from_frame) return -1;
    return infer_ops_->tensor_from_frame(frame, tensor);
}

void ModelManager::free_tensor(HalTensor* tensor) {
    if (!infer_ops_ || !infer_ops_->free_tensor) return;
    if (tensor && tensor->data) {
        infer_ops_->free_tensor(tensor);
        tensor->data = nullptr;
    }
}

void ModelManager::free_outputs(HalTensor* outputs, int num_outputs) {
    if (!infer_ops_ || !infer_ops_->free_tensor) return;
    for (int i = 0; i < num_outputs; i++) {
        if (outputs[i].data != nullptr) {
            infer_ops_->free_tensor(&outputs[i]);
            outputs[i].data = nullptr;
        }
    }
}

// ============================================================
// Post-processing
// ============================================================

int ModelManager::post_process(HalPostprocessSession* session,
                               const HalTensor* outputs, int num_outputs,
                               HalPostprocessResult* result) {
    if (!post_ops_ || !post_ops_->run) return -1;
    return post_ops_->run(session, outputs, num_outputs, result);
}

void ModelManager::free_post_result(HalPostprocessResult* result) {
    if (!post_ops_ || !post_ops_->free_result || !result) return;
    post_ops_->free_result(result);
}

// ============================================================
// System performance stats (NPU utilization)
// ============================================================

int ModelManager::query_performance_stats(uint32_t sampling_period_ms, HalInferencePerfStats* out) {
    if (!infer_ops_ || !infer_ops_->query_system_performance_stats || !out) return -1;
    return infer_ops_->query_system_performance_stats(nullptr, sampling_period_ms, out);
}

// ============================================================
// Per-session hardware performance stats
// ============================================================

int ModelManager::query_session_stats(const std::string& model_id,
                                       uint32_t sampling_ms,
                                       HalInferenceSessionPerfStats* out) {
    if (!infer_ops_ || !infer_ops_->query_session_performance_stats || !out)
        return -1;
    auto snap = acquire_model_snapshot(model_id);
    if (!snap) return -1;
    ModelGuard guard(this, model_id);
    return infer_ops_->query_session_performance_stats(
        snap->infer_session, sampling_ms, out);
}

}  // namespace aipc::ai_runtime
