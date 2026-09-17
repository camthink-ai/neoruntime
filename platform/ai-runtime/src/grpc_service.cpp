#include "grpc_service.h"
#include "log.h"
#include "common.h"
#include "dsp_client.h"
#include "stream_infer_utils.h"

#include "hal_inference.h"
#include "hal_postprocess.h"
#include "common/hal_common.h"

#include <cstring>
#include <vector>
#include <algorithm>
#include <cmath>
#include <sstream>
#include <atomic>
#include <unordered_set>
#include <thread>
#include <condition_variable>
#include <exception>
#include <future>
#include <limits>
#include <unistd.h>
#include <sys/mman.h>

namespace aipc::ai_runtime {

using namespace aipc::inference;
namespace pb = aipc::inference;

AIRuntimeServiceImpl::AIRuntimeServiceImpl(
    const Config& cfg,
    ModelManager* model_mgr,
    SessionManager* session_mgr,
    InferenceScheduler* scheduler,
    FdReceiver* fd_receiver,
    BufferLookupClient* buffer_lookup,
    EventBusClient* event_bus,
    PostprocessPool* postprocess_pool,
    DspClient* dsp_client,
    const HalClipTextEncoderOps* clip_enc_ops,
    const HalGenaiOps* genai_ops)
    : cfg_(cfg)
    , stream_admission_(
          cfg.stream_max_active_rpcs,
          cfg.stream_max_active_rpcs_per_peer,
          cfg.stream_max_subscribers_per_stream,
          cfg.stream_max_in_flight_total,
          cfg.stream_max_in_flight_per_rpc)
    , model_mgr_(model_mgr)
    , session_mgr_(session_mgr)
    , scheduler_(scheduler)
    , fd_receiver_(fd_receiver)
    , buffer_lookup_(buffer_lookup)
    , event_bus_(event_bus)
    , postprocess_pool_(postprocess_pool)
    , dsp_client_(dsp_client)
    , clip_enc_ops_(clip_enc_ops)
    , genai_ops_(genai_ops) {}

std::shared_ptr<StreamPreprocessPool>
AIRuntimeServiceImpl::acquire_stream_preprocess_pool(uint32_t width,
                                                     uint32_t height) {
    if (!dsp_client_ || width == 0 || height == 0 ||
        cfg_.stream_preprocess_max_pools == 0) {
        return nullptr;
    }

    const uint64_t key = (static_cast<uint64_t>(width) << 32) | height;
    std::shared_future<StreamPoolPtr> pending;
    std::shared_ptr<std::promise<StreamPoolPtr>> initializer;
    {
        std::lock_guard lock(dsp_pool_mu_);
        for (auto it = dsp_pools_.begin(); it != dsp_pools_.end();) {
            if (it->second.expired())
                it = dsp_pools_.erase(it);
            else
                ++it;
        }
        dsp_pool_instances_.erase(
            std::remove_if(dsp_pool_instances_.begin(),
                           dsp_pool_instances_.end(),
                           [](const auto& pool) { return pool.expired(); }),
            dsp_pool_instances_.end());

        auto found = dsp_pools_.find(key);
        if (found != dsp_pools_.end()) {
            auto pool = found->second.lock();
            if (pool && pool->usable()) return pool;
            dsp_pools_.erase(found);
        }

        auto in_progress = dsp_pool_inits_.find(key);
        if (in_progress != dsp_pool_inits_.end()) {
            pending = in_progress->second;
        } else {
            const size_t reserved =
                dsp_pool_instances_.size() + dsp_pool_inits_.size();
            if (reserved >= cfg_.stream_preprocess_max_pools) {
                LOG_WARN("StreamInfer: dsp preprocess geometry limit reached "
                         "(%zu/%u)",
                         reserved,
                         (unsigned)cfg_.stream_preprocess_max_pools);
                return nullptr;
            }
            initializer = std::make_shared<std::promise<StreamPoolPtr>>();
            pending = initializer->get_future().share();
            dsp_pool_inits_.emplace(key, pending);
        }
    }

    if (!initializer) return pending.get();

    StreamPoolPtr pool;
    try {
        auto candidate = std::make_shared<StreamPreprocessPool>(
            *dsp_client_, model_mgr_, cfg_.stream_preprocess_job_ms);
        if (candidate->init(width, height, cfg_.stream_preprocess_slots) == 0 &&
            candidate->usable()) {
            pool = std::move(candidate);
        }
    } catch (const std::exception& e) {
        LOG_WARN("StreamInfer: dsp preprocess pool init failed: %s", e.what());
    } catch (...) {
        LOG_WARN("StreamInfer: dsp preprocess pool init failed");
    }

    {
        std::lock_guard lock(dsp_pool_mu_);
        dsp_pool_inits_.erase(key);
        if (pool) {
            dsp_pools_[key] = pool;
            dsp_pool_instances_.push_back(pool);
        }
    }
    initializer->set_value(pool);
    return pool;
}

// ─── Type conversions ─────────────────────────────────────────────────────────

pb::DataType AIRuntimeServiceImpl::hal_dtype_to_proto(HalDataType dt) {
    switch (dt) {
        case HAL_DTYPE_UINT8:   return pb::UINT8;
        case HAL_DTYPE_INT8:    return pb::INT8;
        case HAL_DTYPE_UINT16:  return pb::UINT16;
        case HAL_DTYPE_INT16:   return pb::INT16;
        case HAL_DTYPE_FLOAT16: return pb::FLOAT16;
        case HAL_DTYPE_FLOAT32: return pb::FLOAT32;
        case HAL_DTYPE_INT32:   return pb::INT32;
        case HAL_DTYPE_UINT32:  return pb::UINT32;
        default:                return pb::FLOAT32;
    }
}

HalDataType AIRuntimeServiceImpl::proto_dtype_to_hal(pb::DataType dt) {
    switch (dt) {
        case pb::UINT8:   return HAL_DTYPE_UINT8;
        case pb::INT8:    return HAL_DTYPE_INT8;
        case pb::UINT16:  return HAL_DTYPE_UINT16;
        case pb::INT16:   return HAL_DTYPE_INT16;
        case pb::FLOAT16: return HAL_DTYPE_FLOAT16;
        case pb::FLOAT32: return HAL_DTYPE_FLOAT32;
        case pb::INT32:   return HAL_DTYPE_INT32;
        case pb::UINT32:  return HAL_DTYPE_UINT32;
        default:          return HAL_DTYPE_FLOAT32;
    }
}

std::string AIRuntimeServiceImpl::hal_layout_to_string(HalTensorLayout layout) {
    switch (layout) {
        case HAL_TENSOR_LAYOUT_NHWC: return "NHWC";
        case HAL_TENSOR_LAYOUT_NCHW: return "NCHW";
        case HAL_TENSOR_LAYOUT_NC:   return "NC";
        case HAL_TENSOR_LAYOUT_NHW:  return "NHW";
        case HAL_TENSOR_LAYOUT_CHW:  return "CHW";
        case HAL_TENSOR_LAYOUT_HWC:  return "HWC";
        default:                     return "";
    }
}

// ─── RegisterModel ────────────────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::RegisterModel(
    grpc::ServerContext* /*ctx*/,
    const pb::ModelRegisterRequest* req,
    pb::ModelRegisterResponse* resp) {

    LOG_INFO("RegisterModel: model_id=%s path=%s type=%s transient=%d batch=%u",
             req->model_id().c_str(), req->model_path().c_str(),
             req->model_type().c_str(), req->transient(), req->batch_size());

    if (req->model_id().empty() || req->model_path().empty()) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message("model_id and model_path required");
        return grpc::Status::OK;
    }

    // App-bundled (transient) models have no platform.db metadata to supply a
    // postprocess type later — this registration is the only chance to get it.
    // An empty model_type means raw-tensor-only output: the model would load
    // fine and then silently return nothing useful to apps expecting
    // structured results (the DPM failure mode). Fail loudly instead — unless
    // the registration explicitly opts into raw output (raw_output_only), which
    // is how bundled packages declare output_mode=raw: the app decodes the
    // tensors itself, so no postprocess session is wanted.
    if (req->transient() && req->model_type().empty() && !req->raw_output_only()) {
        LOG_ERROR("RegisterModel: transient model '%s' registered without "
                  "model_type — post-processing would be unavailable "
                  "(raw tensors only). Rejecting.",
                  req->model_id().c_str());
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message(
            "model_type is required for app-bundled (transient) model '" +
            req->model_id() + "'");
        return grpc::Status::OK;
    }

    // Extract owner_id, default to "<system>" if not provided
    std::string owner_id = req->owner_id();
    if (owner_id.empty()) {
        owner_id = "<system>";
    }

    std::string why;
    int rc = model_mgr_->register_model(req->model_id(), req->model_path(),
                                        owner_id, req->transient(),
                                        req->model_variant(),
                                        req->model_type(), &why,
                                        req->batch_size());
    if (rc < 0) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message(
            "Failed to register model '" + req->model_id() + "': " +
            (why.empty() ? std::string("internal error") : why));
        return grpc::Status::OK;
    }

    // Initialize post-processing only for a fresh entry. rc==1 is an
    // identical same-id/path/config co-owner: its shared postprocess session
    // is already initialized, and replacing it during an active inference is
    // unnecessary even though the requested configuration is equivalent.
    if (rc == 0 && !req->model_type().empty() && model_mgr_->has_post_ops()) {
        int post_rc = model_mgr_->init_post_process(
            req->model_id(), req->model_type(), req->model_variant());
        if (post_rc != 0 && req->transient()) {
            // A transient model that declared a postprocess type but failed to
            // initialize it would answer inference with raw tensors — silent
            // degradation. Roll the registration back and fail loudly.
            LOG_ERROR("RegisterModel: post-process init failed for transient "
                      "model %s (type=%s, rc=%d), rolling back registration",
                      req->model_id().c_str(), req->model_type().c_str(), post_rc);
            model_mgr_->unregister_model(req->model_id(), owner_id);
            resp->mutable_status()->set_success(false);
            resp->mutable_status()->set_message(
                "post-process init failed for app-bundled model '" +
                req->model_id() + "' (type=" + req->model_type() + ")");
            return grpc::Status::OK;
        }
        if (post_rc != 0) {
            LOG_WARN("Post-process init failed for %s: %d (inference will return raw tensors)",
                     req->model_id().c_str(), post_rc);
            // Continue anyway - raw tensors will still be available
        }
    }

    resp->set_model_id(req->model_id());
    resp->mutable_status()->set_success(true);
    resp->mutable_status()->set_message("Model registered");
    return grpc::Status::OK;
}

// ─── UnregisterModel ──────────────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::UnregisterModel(
    grpc::ServerContext* /*ctx*/,
    const pb::ModelInfo* req,
    pb::Status* resp) {

    // Extract owner_id — empty means system-level force unload
    std::string owner_id = req->owner_id();

    LOG_INFO("UnregisterModel: model_id=%s, owner_id=%s", req->model_id().c_str(), owner_id.c_str());

    int rc = model_mgr_->unregister_model(req->model_id(), owner_id);
    if (rc == 0) {
        // unregister_model only returns 0 when ref_count == 0, which means no
        // InferBatch callbacks are in flight (those bump ref_count and would
        // block unload). At this point the HAL session is already destroyed
        // (pending async drained), so the implicit "implicit-{model_id}"
        // sessions are safe to tear down — their Session* can no longer be
        // touched by any late callback.
        session_mgr_->destroy_sessions_by_model(req->model_id());
    }
    resp->set_success(rc >= 0);
    resp->set_message(rc == 0 ? "Unregistered" :
                      rc > 0 ? "Owner released; model remains registered" :
                               "Failed to unregister");
    return grpc::Status::OK;
}

// ─── ListModels ───────────────────────────────────────────────────────────────

// Copies HAL tensor specs onto a protobuf ModelInfo. Shared by ListModels and
// GetModelInfo so the list path carries the same per-tensor facts as the
// detail path (platform-api backfills its DB rows from the list response).
void AIRuntimeServiceImpl::fill_tensor_specs(const HalModelInfo& mi,
                                             pb::ModelInfo* info) {
    for (uint32_t i = 0; i < mi.num_inputs; i++) {
        auto* spec = info->add_inputs();
        spec->set_name(mi.inputs[i].name);
        spec->set_dtype(hal_dtype_to_proto(mi.inputs[i].dtype));
        // NV12/NV21/I420 inputs map to the ambiguous NHW layout in HAL; the
        // pixel format itself lives only in is_nv12. Surface it as "NV12" so
        // clients can tell image-plane tensors from planar RGB without
        // inferring from byte_size (W*H*3/2).
        spec->set_layout(mi.inputs[i].is_nv12 != 0
                             ? "NV12"
                             : hal_layout_to_string(mi.inputs[i].layout));
        spec->set_byte_size(mi.inputs[i].byte_size);
        for (int d = 0; d < mi.inputs[i].ndim; d++) {
            spec->add_shape(mi.inputs[i].shape[d]);
        }
    }

    for (uint32_t i = 0; i < mi.num_outputs; i++) {
        auto* spec = info->add_outputs();
        spec->set_name(mi.outputs[i].name);
        spec->set_dtype(hal_dtype_to_proto(mi.outputs[i].dtype));
        spec->set_layout(hal_layout_to_string(mi.outputs[i].layout));
        spec->set_byte_size(mi.outputs[i].byte_size);
        for (int d = 0; d < mi.outputs[i].ndim; d++) {
            spec->add_shape(mi.outputs[i].shape[d]);
        }
    }
}

grpc::Status AIRuntimeServiceImpl::ListModels(
    grpc::ServerContext* /*ctx*/,
    const pb::Empty* /*req*/,
    pb::ModelListResponse* resp) {

    auto models = model_mgr_->list_models();
    for (auto& m : models) {
        auto* info = resp->add_models();
        info->set_model_id(m.id);
        info->set_name(m.name.empty() ? m.id : m.name);  // Use name or fallback to id
        info->set_model_path(m.path);
        info->set_version(m.model_info.version);
        info->set_load_timestamp(static_cast<uint64_t>(m.load_time));
        info->set_transient(m.transient);
        info->set_batch_size(m.batch_size);
        // Include first owner_id if available
        auto owners = model_mgr_->get_owners(m.id);
        if (!owners.empty()) {
            info->set_owner_id(owners[0]);
        }
        fill_tensor_specs(m.model_info, info);
    }
    return grpc::Status::OK;
}

// ─── GetModelInfo ─────────────────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::GetModelInfo(
    grpc::ServerContext* /*ctx*/,
    const pb::ModelInfo* req,
    pb::ModelInfo* resp) {

    auto m = model_mgr_->get_model_copy(req->model_id());
    if (m.id.empty()) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Model not found");
    }

    resp->set_model_id(m.id);
    resp->set_name(m.name.empty() ? m.id : m.name);
    resp->set_model_path(m.path);
    resp->set_version(m.model_info.version);
    resp->set_load_timestamp(static_cast<uint64_t>(m.load_time));
    resp->set_transient(m.transient);
    resp->set_batch_size(m.batch_size);
    fill_tensor_specs(m.model_info, resp);

    return grpc::Status::OK;
}

// ─── Helper: RLE-encode a binary mask (row-major, 0/1 per pixel) ─────────
// Returns bytes where pairs of bytes represent [start, length] of runs.
// This is a simple byte-run encoding compact enough for event-bus JSON.

static std::string rle_encode_mask(const uint8_t* mask, uint32_t w, uint32_t h, uint8_t target_class) {
    std::string rle;
    const uint32_t total = w * h;
    uint32_t i = 0;
    while (i < total) {
        if (mask[i] == target_class) {
            uint32_t start = i;
            while (i < total && mask[i] == target_class) i++;
            uint32_t len = i - start;
            // Encode start as varint (up to 4 bytes) + len as varint
            auto encode_varint = [&](uint32_t v) {
                while (v > 0x7F) { rle.push_back(uint8_t((v & 0x7F) | 0x80)); v >>= 7; }
                rle.push_back(uint8_t(v));
            };
            encode_varint(start);
            encode_varint(len);
        } else {
            i++;
        }
    }
    return rle;
}

static void compute_mask_bbox(const uint8_t* mask, uint32_t w, uint32_t h,
                               uint8_t target_class,
                               float* out_x, float* out_y, float* out_w, float* out_h) {
    uint32_t x0 = w, y0 = h, x1 = 0, y1 = 0;
    bool found = false;
    for (uint32_t row = 0; row < h; row++) {
        for (uint32_t col = 0; col < w; col++) {
            if (mask[row * w + col] == target_class) {
                found = true;
                if (col < x0) x0 = col;
                if (col > x1) x1 = col;
                if (row < y0) y0 = row;
                if (row > y1) y1 = row;
            }
        }
    }
    if (!found) { *out_x = *out_y = *out_w = *out_h = 0.f; return; }
    *out_x = (float)x0 / w;
    *out_y = (float)y0 / h;
    *out_w = (float)(x1 - x0 + 1) / w;
    *out_h = (float)(y1 - y0 + 1) / h;
}

// ─── Helper: PostResult → proto PostResult ──────────────────────────────

static void fill_proto_post_result(pb::PostResult* out, const HalPostprocessResult& src) {
    switch (src.type) {
    case HAL_POST_TYPE_DETECTION: {
        auto& det = src.result.detection;
        for (uint32_t i = 0; i < det.num_detections; i++) {
            auto* d = out->add_detections();
            auto* bbox = d->mutable_bbox();
            bbox->set_x(det.detections[i].bbox.x);
            bbox->set_y(det.detections[i].bbox.y);
            bbox->set_w(det.detections[i].bbox.w);
            bbox->set_h(det.detections[i].bbox.h);
            d->set_confidence(det.detections[i].confidence);
            d->set_class_id(det.detections[i].class_id);
            d->set_label(det.detections[i].label);
        }
        break;
    }
    case HAL_POST_TYPE_CLASSIFICATION:
    case HAL_POST_TYPE_CLIP: {
        auto& cls = src.result.classification;
        for (uint32_t i = 0; i < cls.num_classes; i++) {
            auto* c = out->add_classifications();
            c->set_type(cls.classes[i].type);
            c->set_class_id(cls.classes[i].class_id);
            c->set_label(cls.classes[i].label);
            c->set_confidence(cls.classes[i].confidence);
        }
        // CLIP: also expose image embedding via priv pointer
        if (src.type == HAL_POST_TYPE_CLIP && cls.priv) {
            auto* emb_vec = static_cast<std::vector<float>*>(cls.priv);
            auto* e = out->add_embeddings();
            for (float v : *emb_vec)
                e->add_data(v);
            e->set_dim(static_cast<uint32_t>(emb_vec->size()));
            delete emb_vec;
        }
        break;
    }
    case HAL_POST_TYPE_KEYPOINT: {
        auto& kp = src.result.keypoint;
        for (uint32_t i = 0; i < kp.num_objects; i++) {
            auto& obj = kp.objects[i];
            auto* lm = out->add_landmarks();
            lm->set_type("keypoint");
            for (uint32_t j = 0; j < obj.num_keypoints; j++) {
                auto* pt = lm->add_points();
                pt->set_x(obj.keypoints[j].x);
                pt->set_y(obj.keypoints[j].y);
                pt->set_confidence(obj.keypoints[j].confidence);
            }
        }
        break;
    }
    case HAL_POST_TYPE_SEGMENTATION: {
        auto& seg = src.result.segmentation;
        if (seg.mask_data && seg.width > 0 && seg.height > 0) {
            // Extract per-class masks
            for (uint32_t cls = 0; cls < seg.num_classes; cls++) {
                std::string rle = rle_encode_mask(seg.mask_data, seg.width, seg.height, (uint8_t)cls);
                if (rle.empty()) continue;
                float bx, by, bw, bh;
                compute_mask_bbox(seg.mask_data, seg.width, seg.height, (uint8_t)cls, &bx, &by, &bw, &bh);
                auto* m = out->add_masks();
                m->set_class_id(cls);
                m->set_mask_rle(rle);
                m->set_mask_width(seg.width);
                m->set_mask_height(seg.height);
                auto* bbox = m->mutable_bbox();
                bbox->set_x(bx);
                bbox->set_y(by);
                bbox->set_w(bw);
                bbox->set_h(bh);
            }
        }
        break;
    }
    case HAL_POST_TYPE_OCR_DETECTION: {
        // OCR detection uses HalDetectionResult (result.detection), NOT HalOcrResult
        auto& det = src.result.detection;
        for (uint32_t i = 0; i < det.num_detections; i++) {
            auto* line = out->add_ocr_lines();
            auto* bbox = line->mutable_bbox();
            bbox->set_x(det.detections[i].bbox.x);
            bbox->set_y(det.detections[i].bbox.y);
            bbox->set_w(det.detections[i].bbox.w);
            bbox->set_h(det.detections[i].bbox.h);
            line->set_text(det.detections[i].label);
            line->set_confidence(det.detections[i].confidence);
        }
        break;
    }
    case HAL_POST_TYPE_OCR_RECOGNITION: {
        auto& ocr = src.result.ocr;
        for (uint32_t i = 0; i < ocr.num_lines; i++) {
            auto* line = out->add_ocr_lines();
            auto* bbox = line->mutable_bbox();
            bbox->set_x(ocr.lines[i].bbox.x);
            bbox->set_y(ocr.lines[i].bbox.y);
            bbox->set_w(ocr.lines[i].bbox.w);
            bbox->set_h(ocr.lines[i].bbox.h);
            // Sanitize to valid UTF-8: replace invalid bytes with '?'
            std::string safe_text;
            safe_text.reserve(128);
            const unsigned char* p = reinterpret_cast<const unsigned char*>(ocr.lines[i].text);
            while (*p) {
                if (*p < 0x80) {
                    // ASCII: keep printable chars, replace controls with space
                    safe_text += (*p >= 0x20) ? static_cast<char>(*p) : ' ';
                    ++p;
                } else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) {
                    safe_text += static_cast<char>(p[0]);
                    safe_text += static_cast<char>(p[1]);
                    p += 2;
                } else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) {
                    safe_text += static_cast<char>(p[0]);
                    safe_text += static_cast<char>(p[1]);
                    safe_text += static_cast<char>(p[2]);
                    p += 3;
                } else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) {
                    safe_text += static_cast<char>(p[0]);
                    safe_text += static_cast<char>(p[1]);
                    safe_text += static_cast<char>(p[2]);
                    safe_text += static_cast<char>(p[3]);
                    p += 4;
                } else {
                    safe_text += '?';
                    ++p;
                }
            }
            line->set_text(safe_text);
            line->set_confidence(ocr.lines[i].confidence);
        }
        break;
    }
    case HAL_POST_TYPE_EMBEDDING: {
        auto& emb = src.result.embedding;
        auto* e = out->add_embeddings();
        for (uint32_t i = 0; i < emb.dim; i++) {
            e->add_data(emb.data[i]);
        }
        e->set_dim(emb.dim);
        break;
    }
    case HAL_POST_TYPE_DEPTH: {
        auto& d = src.result.depth;
        if (d.depth_m && d.width > 0 && d.height > 0) {
            auto* dm = out->add_depth_maps();
            dm->set_width(d.width);
            dm->set_height(d.height);
            dm->set_depth_data(d.depth_m, d.width * d.height * sizeof(float));
        }
        break;
    }
    default:
        break;
    }
}

namespace {

constexpr int kMaxInferBatchRequests = 64;
constexpr uint64_t kMaxInferBatchInputBytes = 32ULL * 1024ULL * 1024ULL;

bool validate_infer_inputs(const pb::InferRequest& request,
                           std::string* error) {
    const int count = request.inputs_size();
    if (count <= 0 || count > HAL_MAX_TENSORS) {
        if (error) *error = "input count must be within HAL tensor limits";
        return false;
    }

    for (int i = 0; i < count; ++i) {
        const auto& tensor = request.inputs(i);
        if (tensor.dma_fd() != 0) {
            if (error) *error = "numeric dma_fd cannot cross gRPC";
            return false;
        }
        if (tensor.shape_size() <= 0 ||
            tensor.shape_size() > HAL_MAX_TENSOR_DIMS) {
            if (error) *error = "input rank must be within HAL tensor limits";
            return false;
        }
        for (int d = 0; d < tensor.shape_size(); ++d) {
            if (tensor.shape(d) <= 0) {
                if (error) *error = "input dimensions must be positive";
                return false;
            }
        }
        if (tensor.data().size() > std::numeric_limits<uint32_t>::max()) {
            if (error) *error = "input payload exceeds HAL byte-size limits";
            return false;
        }
    }
    return true;
}

// Repack an NV12 frame received as per-plane dma-buf fds into a tight
// [h*3/2, w] UINT8 CPU buffer — byte-for-byte the layout the bytes path
// sends, so downstream geometry validation is identical. One dma-buf per
// plane, plane data at offset 0 (platform convention: DSP pool allocs,
// imports, and subscribed frames). Returns false with a reason on any
// layout it cannot map.
bool repack_nv12_planes(uint32_t width, uint32_t height, uint32_t format,
                        uint32_t num_planes, const uint32_t* strides,
                        const uint32_t* sizes, const std::vector<int>& fds,
                        std::string& out, std::string& why) {
    constexpr uint32_t kHalPixFmtNv12 = 0;  // HalPixelFormat NV12
    if (format != kHalPixFmtNv12) {
        why = "frame format is not NV12 (repack supports NV12 only)";
        return false;
    }
    if (num_planes != 2) {
        why = "NV12 frame must have 2 planes, got " + std::to_string(num_planes);
        return false;
    }
    const uint32_t w = width, h = height;
    if (w == 0 || h == 0 || (h & 1) != 0) {
        why = "invalid NV12 geometry";
        return false;
    }
    if (fds.size() < 2) {
        why = "NV12 frame must carry 2 plane fds, got "
              + std::to_string(fds.size());
        return false;
    }
    const uint32_t uv_rows = h / 2;
    if (strides[0] < w || sizes[0] < (uint64_t)strides[0] * h) {
        why = "Y plane smaller than stride*height";
        return false;
    }
    if (strides[1] < w || sizes[1] < (uint64_t)strides[1] * uv_rows) {
        why = "UV plane smaller than stride*(height/2)";
        return false;
    }

    out.assign((size_t)w * h * 3 / 2, '\0');

    void* maps[2] = {nullptr, nullptr};
    const uint32_t rows[2] = {h, uv_rows};
    bool ok = true;
    for (int p = 0; p < 2 && ok; ++p) {
        maps[p] = mmap(nullptr, sizes[p], PROT_READ, MAP_SHARED, fds[p], 0);
        if (maps[p] == MAP_FAILED) {
            maps[p] = nullptr;
            why = "mmap of plane " + std::to_string(p) + " failed";
            ok = false;
        }
    }
    if (ok) {
        char* dst = out.data();
        for (int p = 0; p < 2; ++p) {
            const char* src  = static_cast<const char*>(maps[p]);
            uint32_t   stride = strides[p];
            for (uint32_t row = 0; row < rows[p]; ++row) {
                memcpy(dst, src + (size_t)row * stride, w);
                dst += w;
            }
        }
    }
    for (int p = 0; p < 2; ++p) {
        if (maps[p]) munmap(maps[p], sizes[p]);
    }
    return ok;
}

// Frame fetched via Tensor.buffer_id (per-plane dma-buf fds + geometry from
// BufferLookupClient).
bool repack_nv12_planes(const BufferLookupClient::LookupResult& r,
                        std::string& out, std::string& why) {
    return repack_nv12_planes(r.width, r.height, r.format, r.num_planes,
                              r.strides, r.sizes, r.fds, out, why);
}

// ─── P1-2: buffer_id direct-bind ─────────────────────────────────────────────

// Borrowed resources behind a direct-bound input frame. The registry lease
// and the dup'd plane fds must outlive the NPU read, so ownership rides with
// the request (Infer: on_complete lambda capture; InferBatch: the item ctx)
// and this destructor returns everything exactly once.
struct DmaInputLease {
    ModelManager*                  mgr = nullptr;  // free_tensor for bound inputs
    std::vector<HalTensor>         bound;          // canonical priv owners
    std::vector<int>               fds;            // dup'd plane fds from lookup()
    BufferLookupClient*            lookup = nullptr;
    std::vector<uint64_t>          buffer_ids;     // pinned registry ids
    ~DmaInputLease() {
        if (mgr)
            for (auto& t : bound) mgr->free_tensor(&t);
        for (int fd : fds) close(fd);
        if (lookup)
            for (uint64_t id : buffer_ids) lookup->release(id);
    }
};

// Try fd direct-bind for a Tensor.buffer_id input. When the looked-up frame
// is a compact NV12 dmabuf whose geometry matches the model's first input,
// bind_dma_frame() returns a zero-CPU-copy tensor and the mmap+memcpy repack
// is skipped entirely. Any contract mismatch (padding, geometry, format)
// returns false and the caller falls back to the repack unchanged. On
// success `lease` accumulates the borrowed fds + registry pin; the caller
// must keep it alive until the inference has completed.
bool try_bind_dma_input(HalInferenceSession* sess, ModelManager* mgr,
                        const HalModelInfo& mi,
                        const BufferLookupClient::LookupResult& r,
                        uint64_t buffer_id, BufferLookupClient* lookup,
                        HalTensor& ht, std::shared_ptr<DmaInputLease>& lease) {
    constexpr uint32_t kHalPixFmtNv12 = 0;  // HalPixelFormat NV12
    if (!sess || !mgr || r.format != kHalPixFmtNv12 ||
        r.width == 0 || r.height == 0 || r.fds.empty())
        return false;
    // Format/geometry precheck (2026-09-16 remediation #10): the bind
    // contract is exact-geometry NV12. A model whose first input is not an
    // NV12 blob, or an NV12 model of different dimensions, can never bind —
    // skip the HAL call (and its per-frame ERROR log) instead of retrying
    // every frame.
    if (mi.num_inputs == 0 || !mi.inputs[0].is_nv12 ||
        mi.inputs[0].byte_size != (uint64_t)r.width * r.height * 3 / 2)
        return false;
    const HalInferenceOps* ops = mgr->infer_ops();
    // ABI guard: an older HAL's table ends before bind_dma_frame — the slot
    // then reads adjacent rodata, not NULL, so the pointer check alone
    // cannot catch it (R1 SIGILL at the StreamPreprocessPool::bind site).
    if (!ops || !mgr->has_infer_op(offsetof(HalInferenceOps, bind_dma_frame)) ||
        !ops->bind_dma_frame)
        return false;

    HalDmaFrameDesc desc{};
    desc.format   = HAL_PIX_FMT_NV12;
    desc.width    = r.width;
    desc.height   = r.height;
    desc.borrowed = 1;  // fds stay owned by the lease
    const uint32_t rows[HAL_MAX_PLANES] = {r.height, r.height / 2, 0};
    for (uint32_t p = 0; p < r.num_planes && p < HAL_MAX_PLANES; p++) {
        desc.fd[p]         = r.fds[p];
        desc.offset[p]     = 0;  // registry plane fds are whole dmabufs
        desc.stride[p]     = r.strides[p];
        desc.bytes_used[p] = (uint64_t)r.strides[p] * rows[p];
    }
    for (uint32_t p = r.num_planes < HAL_MAX_PLANES ? r.num_planes : HAL_MAX_PLANES;
         p < HAL_MAX_PLANES; p++)
        desc.fd[p] = -1;
    if (r.num_planes == 1)
        desc.bytes_used[0] = (uint64_t)r.width * r.height * 3 / 2;

    HalTensor bound{};
    if (ops->bind_dma_frame(sess, &desc, &bound) != HAL_OK)
        return false;  // off-contract → repack fallback, fds still ours to close

    static std::atomic<bool> s_bind_logged{false};
    if (!s_bind_logged.exchange(true))
        LOG_INFO("buffer_id direct-bind engaged (%ux%u NV12, zero repack)",
                 (unsigned)r.width, (unsigned)r.height);

    ht = bound;
    if (!lease) {
        lease         = std::make_shared<DmaInputLease>();
        lease->mgr    = mgr;
        lease->lookup = lookup;
    }
    lease->bound.push_back(bound);
    // fd ownership moves into the lease: the descriptor copied the fd NUMBERS,
    // the lease closes them after the NPU read completes.
    lease->fds.insert(lease->fds.end(), r.fds.begin(), r.fds.end());
    lease->buffer_ids.push_back(buffer_id);
    return true;
}

// NV12 → model-input conversion fallback (2026-09-16 remediation #10, and
// the R4 attribution correction): a model whose first input is packed RGB
// (yolov5m_vehicles, tiny_yolov4_license_plates, …) can neither direct-bind
// (NV12-only contract) nor consume the tight NV12 repack — its byte_size can
// never match, so run() used to reject every frame with a per-frame HAL
// ERROR while the client saw the inference fail. tensor_from_frame_ex is
// the session-aware path built for exactly this case: BT.601 NV12→RGB plus
// bilinear resize into a model-shaped tensor. The returned tensor owns its
// buffer through priv — release it via ModelManager::free_tensor (the
// DmaInputLease bound-vector already does exactly that).
bool convert_nv12_frame_to_input(HalInferenceSession* sess, ModelManager* mgr,
                                 uint32_t width, uint32_t height,
                                 uint32_t num_planes, const uint32_t* strides,
                                 const uint32_t* sizes, const std::vector<int>& fds,
                                 HalTensor& ht, std::string& why) {
    const HalInferenceOps* ops = mgr->infer_ops();
    if (!ops ||
        !mgr->has_infer_op(offsetof(HalInferenceOps, tensor_from_frame_ex)) ||
        !ops->tensor_from_frame_ex) {
        why = "model input is not NV12 and this HAL has no tensor_from_frame_ex "
              "(deploy ai-runtime and HAL from the same build)";
        return false;
    }
    if (num_planes < 2 || fds.size() < 2) {
        why = "conversion needs a 2-plane NV12 frame, got " +
              std::to_string(num_planes) + " plane(s)";
        return false;
    }

    void* maps[2] = {nullptr, nullptr};
    bool mapped = true;
    for (int p = 0; p < 2 && mapped; ++p) {
        maps[p] = mmap(nullptr, sizes[p], PROT_READ, MAP_SHARED, fds[p], 0);
        if (maps[p] == MAP_FAILED) {
            maps[p] = nullptr;
            mapped = false;
        }
    }
    bool converted = false;
    if (!mapped) {
        why = "mmap of frame planes failed";
    } else {
        HalFrameBuffer fb{};
        fb.width      = width;
        fb.height     = height;
        fb.format     = HAL_PIX_FMT_NV12;
        fb.num_planes = 2;
        for (int p = 0; p < 2; ++p) {
            fb.dma_fds[p] = fds[p];
            fb.planes[p]  = maps[p];
            fb.strides[p] = strides[p];
            fb.sizes[p]   = sizes[p];
        }
        HalTensor out{};
        if (ops->tensor_from_frame_ex(sess, &fb, &out) == HAL_OK) {
            ht = out;
            converted = true;
            static std::atomic<bool> s_conv_logged{false};
            if (!s_conv_logged.exchange(true))
                LOG_INFO("NV12->RGB conversion fallback engaged "
                         "(tensor_from_frame_ex, %ux%u source)",
                         (unsigned)width, (unsigned)height);
        } else {
            why = "tensor_from_frame_ex rejected the frame";
        }
    }
    for (int p = 0; p < 2; ++p)
        if (maps[p]) munmap(maps[p], sizes[p]);
    return converted;
}

}  // namespace

// ─── Infer (synchronous single-shot) ─────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::Infer(
    grpc::ServerContext* /*ctx*/,
    const pb::InferRequest* req,
    pb::InferResponse* resp) {

    LOG_DEBUG("Infer: model_id=%s", req->model_id().c_str());

    std::string input_error;
    if (!validate_infer_inputs(*req, &input_error)) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message(std::move(input_error));
        return grpc::Status::OK;
    }

    try {
    // Prepare the guard before acquisition so copying the model id cannot leak
    // a live ref if allocation throws. The scheduler acquires its own ref.
    ModelGuard model_guard{model_mgr_, req->model_id()};
    auto snap = model_mgr_->acquire_model_snapshot(req->model_id());
    if (!snap) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message("Model not found");
        return grpc::Status::OK;
    }
    model_guard.arm();

    // Ensure an implicit session exists for stats tracking on the Infer()
    // path. Created only after the model snapshot resolves so requests for
    // nonexistent model ids cannot accumulate registry entries. A rejected
    // create (invalid id / session caps) returns "" and every infer_session
    // use below is null-checked — stats are skipped, inference proceeds.
    std::string implicit_session_id = "implicit-" + req->model_id();
    session_mgr_->create_named_session(
        implicit_session_id, "implicit", "infer", req->model_id(),
        0 /*fps_limit*/, 0 /*max_qps*/, 5 /*priority*/);
    auto infer_session = session_mgr_->get_session(implicit_session_id);

    // Batch models speak InferBatch only: their session expects B x per-frame
    // input bytes, so a single-frame submission could only fail the HAL
    // size check deep in the backend. Reject with an actionable message.
    if (snap->batch_size > 1) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message(
            "Model '" + req->model_id() + "' is registered with batch=" +
            std::to_string(snap->batch_size) +
            "; single-frame Infer is not available for it. Use InferBatch "
            "with " + std::to_string(snap->batch_size) +
            "-frame groups, or register a batch=1 variant of the model.");
        return grpc::Status::OK;
    }

    int num_inputs = req->inputs_size();
    if (num_inputs != snap->model_info.num_inputs) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message("Input count does not match model");
        return grpc::Status::OK;
    }

    int max_outputs = snap->num_outputs;
    auto post_session = snap->post_session;
    bool enable_post = model_mgr_->has_post_ops() && post_session;
    auto model_id = req->model_id();
    uint32_t priority = req->priority();
    uint32_t timeout_ms = req->timeout_ms() > 0 ? req->timeout_ms() : 5000;
    std::string session_id = implicit_session_id;

    // The on_complete callback does ALL work (fill proto response +
    // post_process + free + release) so it is self-sufficient even if
    // the gRPC thread times out and returns early. The gRPC thread only
    // waits for the response promise and copies it out if ready.
    auto response_ptr = std::make_shared<pb::InferResponse>();
    auto promise = std::make_shared<std::promise<bool>>();  // true = success
    auto future = promise->get_future();

    // Keep input data alive until on_complete finishes. For CPU inputs,
    // copy into a persistent buffer so the protobuf request can be freed
    // after the gRPC thread returns (fixes UAF on timeout).
    // Pre-allocate with resize: emplace_back could reallocate the vector
    // and invalidate previously stored data() pointers.
    auto input_data_holder = std::make_shared<std::vector<std::string>>();
    input_data_holder->resize(num_inputs);
    // P1-2: owns borrowed fds + registry pins for direct-bound buffer_id
    // inputs; captured by on_complete so they outlive the NPU read.
    std::shared_ptr<DmaInputLease> dma_lease;
    auto input_tensors = std::make_shared<std::vector<HalTensor>>(num_inputs);
    for (int i = 0; i < num_inputs; i++) {
        auto& pb_t = req->inputs(i);
        auto& ht   = (*input_tensors)[i];
        std::memset(&ht, 0, sizeof(HalTensor));

        if (pb_t.dma_fd() > 0) {
            // A raw fd number from the caller's process is meaningless here —
            // exactly the cross-process handoff the buffer_id path replaces.
            // Reject loudly instead of binding a random descriptor.
            resp->mutable_status()->set_success(false);
            resp->mutable_status()->set_message(
                "Tensor.dma_fd rejected: a raw fd number cannot cross "
                "processes; pass the frame as bytes or a buffer_id");
            return grpc::Status::OK;
        }

        bool frame_geometry = false;  // dtype/shape taken from the looked-up frame
        if (pb_t.buffer_id() != 0) {
            if (!buffer_lookup_) {
                resp->mutable_status()->set_success(false);
                resp->mutable_status()->set_message(
                    "Tensor.buffer_id rejected: buffer registry not configured");
                return grpc::Status::OK;
            }
            auto lr = buffer_lookup_->lookup(pb_t.buffer_id());
            if (lr.rc != 0) {
                resp->mutable_status()->set_success(false);
                resp->mutable_status()->set_message(
                    "Tensor.buffer_id lookup failed: " + lr.message);
                return grpc::Status::OK;
            }
            std::string why;
            // P1-2: fd direct-bind first — a compact layout matching the
            // model's first input skips the mmap+memcpy repack entirely
            // (try_bind_dma_input prechecks format/geometry; padding /
            // geometry mismatch / non-NV12 falls through to the repack or
            // the conversion below unchanged). First input only:
            // bind_dma_frame validates against input 0.
            const bool rgb_model = snap->model_info.num_inputs > 0 &&
                !snap->model_info.inputs[0].is_nv12;
            const bool bound = i == 0 &&
                try_bind_dma_input(snap->infer_session, model_mgr_,
                                   snap->model_info, lr,
                                   pb_t.buffer_id(), buffer_lookup_, ht,
                                   dma_lease);
            if (!bound) {
                bool prepared = false;
                const char* prep = "repack";
                if (i == 0 && rgb_model) {
                    // Packed-RGB model: convert through the session-aware
                    // preprocess — the NV12 repack's byte_size can never
                    // match and run() would reject every frame.
                    prep = "conversion";
                    prepared = convert_nv12_frame_to_input(
                        snap->infer_session, model_mgr_, lr.width, lr.height,
                        lr.num_planes, lr.strides, lr.sizes, lr.fds, ht, why);
                    if (prepared) {
                        // Tensor owns its buffer; the lease free_tensor's it
                        // once the NPU read completes.
                        if (!dma_lease) {
                            dma_lease      = std::make_shared<DmaInputLease>();
                            dma_lease->mgr = model_mgr_;
                        }
                        dma_lease->bound.push_back(ht);
                    }
                } else {
                    prepared = repack_nv12_planes(lr, (*input_data_holder)[i], why);
                    if (prepared) {
                        const uint32_t w = lr.width, h = lr.height;
                        ht.data      = const_cast<char*>((*input_data_holder)[i].data());
                        ht.byte_size = static_cast<uint32_t>(w) * h * 3 / 2;
                        ht.dma_fd    = -1;
                        // Geometry comes from the daemon's registry, not the wire, so a
                        // mismatched client-declared shape never reaches the NPU.
                        ht.dtype     = HAL_DTYPE_UINT8;
                        ht.ndim      = 2;
                        ht.shape[0]  = static_cast<int32_t>(h * 3 / 2);
                        ht.shape[1]  = static_cast<int32_t>(w);
                    }
                }
                for (int fd : lr.fds) close(fd);
                // Lease dropped once our own copy exists (or cannot be made).
                buffer_lookup_->release(pb_t.buffer_id());
                if (!prepared) {
                    resp->mutable_status()->set_success(false);
                    resp->mutable_status()->set_message(
                        std::string("Tensor.buffer_id ") + prep + " failed: " + why);
                    return grpc::Status::OK;
                }
            }
            // dtype/shape/byte_size of a bound tensor come from
            // bind_dma_frame (desc-derived); both arms carry frame geometry.
            frame_geometry = true;
        } else {
            // Assign to pre-allocated slot — no reallocation
            (*input_data_holder)[i] = pb_t.data();
            ht.data      = const_cast<char*>((*input_data_holder)[i].data());
            ht.byte_size = static_cast<uint32_t>((*input_data_holder)[i].size());
            ht.dma_fd    = -1;
        }

        if (!frame_geometry) {
            ht.dtype = proto_dtype_to_hal(pb_t.dtype());
            ht.ndim  = static_cast<int32_t>(pb_t.shape_size());
            for (int d = 0; d < ht.ndim && d < HAL_MAX_TENSOR_DIMS; d++) {
                ht.shape[d] = pb_t.shape(d);
            }
        }
    }

    auto inf_req = std::make_unique<InferRequest>();
    inf_req->model_id   = model_id;
    inf_req->session_id = session_id;
    inf_req->num_inputs = num_inputs;
    std::memcpy(inf_req->inputs, input_tensors->data(), sizeof(HalTensor) * num_inputs);
    inf_req->priority   = priority;
    inf_req->timeout_ms = timeout_ms;
    inf_req->owns_outputs = true;  // on_complete frees + releases
    inf_req->resource_holder = input_data_holder;  // keep inputs alive

    inf_req->on_complete = [this, promise, response_ptr, post_session,
                            enable_post, model_id, infer_session,
                            input_tensors, session_id, dma_lease](
        int rc, HalTensor* outputs, int num_outputs,
        uint64_t infer_us, uint64_t queue_us,
        bool model_acquired) {

        struct CallbackCleanup {
            ModelManager* mgr;
            HalTensor* outputs;
            int num_outputs;
            std::string model_id;
            bool model_acquired;
            std::shared_ptr<std::promise<bool>> promise;
            bool promise_value = false;

            ~CallbackCleanup() noexcept {
                try {
                    if (outputs) mgr->free_outputs(outputs, num_outputs);
                } catch (...) {
                    LOG_ERROR("Infer: output cleanup threw");
                }
                try {
                    if (model_acquired) mgr->release_model(model_id);
                } catch (...) {
                    LOG_ERROR("Infer: model release threw");
                }
                try {
                    promise->set_value(promise_value);
                } catch (...) {
                    LOG_ERROR("Infer: promise completion threw");
                }
            }
        } cleanup{model_mgr_, outputs, num_outputs, model_id,
                  model_acquired, promise};

        try {
            if (infer_session) {
                session_mgr_->record_inference(infer_session.get(), infer_us);
            }

            if (rc != 0) {
                response_ptr->mutable_status()->set_success(false);
                response_ptr->mutable_status()->set_message(
                    "Inference failed: " + std::to_string(rc));
                return;
            }

            for (int i = 0; i < num_outputs; i++) {
                auto* pt = response_ptr->add_outputs();
                pt->set_dtype(hal_dtype_to_proto(outputs[i].dtype));
                for (int d = 0; d < outputs[i].ndim; d++) {
                    pt->add_shape(outputs[i].shape[d]);
                }
                if (outputs[i].data && outputs[i].byte_size > 0) {
                    pt->set_data(outputs[i].data, outputs[i].byte_size);
                }
            }

            bool pp_failed = false;
            if (enable_post && post_session) {
                HalPostprocessResult post_result{};
                struct PostResultGuard {
                    ModelManager* mgr;
                    HalPostprocessResult* result;
                    ~PostResultGuard() noexcept {
                        try {
                            mgr->free_post_result(result);
                        } catch (...) {
                            LOG_ERROR("Infer: post-result cleanup threw");
                        }
                    }
                } post_guard{model_mgr_, &post_result};

                try {
                    if (model_mgr_->post_process(post_session, outputs,
                                                  num_outputs, &post_result) == 0) {
                        fill_proto_post_result(response_ptr->mutable_post_result(),
                                               post_result);

                        if (event_bus_ && event_bus_->connected() &&
                            cfg_.event_bus_auto_publish) {
                            auto ts_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            publish_result("app-infer", model_id, 0, ts_ns,
                                           response_ptr->post_result());
                        }
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR("Postprocess failed for model '%s': %s",
                              model_id.c_str(), e.what());
                    pp_failed = true;
                    response_ptr->mutable_status()->set_success(false);
                    response_ptr->mutable_status()->set_message(
                        std::string("Postprocess failed: ") + e.what());
                }
            }

            response_ptr->set_infer_time_us(infer_us);
            response_ptr->set_queue_time_us(queue_us);
            if (!pp_failed) response_ptr->mutable_status()->set_success(true);
            cleanup.promise_value = !pp_failed;
        } catch (const std::exception& e) {
            LOG_ERROR("Infer completion failed for model '%s': %s",
                      model_id.c_str(), e.what());
            try {
                response_ptr->mutable_status()->set_success(false);
                response_ptr->mutable_status()->set_message(
                    "Inference completion failed");
            } catch (...) {
            }
        } catch (...) {
            LOG_ERROR("Infer completion failed for model '%s'", model_id.c_str());
            try {
                response_ptr->mutable_status()->set_success(false);
                response_ptr->mutable_status()->set_message(
                    "Inference completion failed");
            } catch (...) {
            }
        }
    };

    if (!scheduler_->submit(std::move(inf_req))) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message("Scheduler queue full");
        // model_guard releases the Infer()-side ref on return
        return grpc::Status::OK;
    }

    // Wait for on_complete to fill the response. On timeout, on_complete
    // will still fire later and do its own free/release — no leak.
    if (future.wait_for(std::chrono::milliseconds(timeout_ms))
        != std::future_status::ready) {
        LOG_ERROR("Infer timeout: model_id=%s timeout_ms=%u",
                  model_id.c_str(), timeout_ms);
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message("Inference timeout");
        // model_guard releases the Infer()-side ref.
        // The scheduler's ref + outputs are freed by on_complete when it fires.
        // input_data_holder (shared_ptr) keeps CPU input alive until on_complete.
        return grpc::Status::OK;
    }

    // on_complete filled response_ptr — copy to the gRPC response
    future.get();  // check for exceptions
    *resp = std::move(*response_ptr);

    return grpc::Status::OK;

    } catch (const std::exception& e) {
        LOG_ERROR("Infer exception: model_id=%s what=%s", req->model_id().c_str(), e.what());
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message(std::string("Exception: ") + e.what());
        return grpc::Status::OK;
    } catch (...) {
        LOG_ERROR("Infer unknown exception: model_id=%s", req->model_id().c_str());
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message("Unknown exception during inference");
        return grpc::Status::OK;
    }
}

// ─── InferBatch async completion helpers (anonymous namespace) ────────────────
namespace {

// Per-item context for an InferBatch request, held via shared_ptr so the HAL
// completion callback (fired from a HailoRT thread) keeps it alive. It owns the
// CPU input payload copies and the HAL-aligned output buffers, so a callback
// firing after the RPC returns never touches freed request memory.
struct InferBatchItemCtx {
    int                            index = 0;
    pb::InferResponse              response;
    std::vector<std::string>       input_data;   // owns CPU input payloads
    std::vector<HalTensor>         inputs;        // inputs[k].data -> input_data[k] or dma_fd
    // P1-2: borrowed fds + registry pins for direct-bound buffer_id inputs;
    // freed when the last ctx reference drops (late callbacks included).
    std::shared_ptr<struct DmaInputLease> dma_lease;
    std::vector<HalTensor>         outputs;       // HAL-align output buffers (priv-owned)
    int                            max_outputs = 0;
    std::optional<ModelSnapshot>   snap;
    std::string                    model_id;
    bool                           acquired = false;  // model ref_count bumped?
    // shared_ptr: this fires from a HailoRT completion thread long after submit,
    // during which another thread can UnregisterModel and erase the implicit
    // session. Holds the Session alive until the callback releases the ctx.
    std::shared_ptr<Session>       infer_session;
    SteadyClock::time_point        start;
    std::atomic<bool>              done{false};
    std::atomic<bool>              finishing{false};
    std::atomic<bool>              released{false};  // model ref released?
    bool                           success = false;

    // ── Grouped (NPU batch) execution only ─────────────────────────────────
    // Frame slot this item occupies inside its batch job (0..B-1); -1 for
    // plain single-frame jobs.
    int                            batch_frame = -1;
    // Grouped jobs hand each item a per-frame VIEW into the group's batch
    // output tensors; views must not go through free_outputs (the group
    // frees the real tensors once every post task has drained).
    bool                           outputs_are_view = false;
};

// Shared completion signaling across all items in a batch: the last callback to
// finish wakes the waiter. Held by shared_ptr so late callbacks can safely
// decrement it after the RPC returns.
struct InferBatchSyncState {
    std::mutex              mtx;
    std::condition_variable cv;
    int                     remaining;
    explicit InferBatchSyncState(int n) : remaining(n) {}
};

// Bundle threaded through the HAL callback via userdata. The submitter and the
// callback each hold one intrusive reference because a backend may invoke the
// callback before run_async() returns. Model ref_count is released by the
// callback/post_task itself, not by the InferBatch RPC waiter.
struct InferBatchCbState {
    std::atomic<int>                      owners{2};
    AsyncSubmissionGate                   gate;
    int                                   callback_status = HAL_ERROR;
    int                                   callback_num_outputs = 0;
    ModelManager*                         mgr = nullptr;
    SessionManager*                       smgr = nullptr;
    PostprocessPool*                      pool = nullptr;
    InferenceScheduler*                   scheduler = nullptr;
    pb::DataType (*dtype_to_proto)(HalDataType) = nullptr;
    std::shared_ptr<InferBatchItemCtx>    ctx;
    std::shared_ptr<InferBatchSyncState>  sync;

    void release_owner() noexcept {
        if (owners.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this;
    }
};

void finish_batch_item(ModelManager* mgr,
                       InferenceScheduler* scheduler,
                       const std::shared_ptr<InferBatchItemCtx>& ctx,
                       const std::shared_ptr<InferBatchSyncState>& sync,
                       int max_outputs) noexcept {
    if (ctx->finishing.exchange(true, std::memory_order_acq_rel)) return;

    try {
        mgr->free_outputs(ctx->outputs.data(), max_outputs);
    } catch (...) {
        LOG_ERROR("InferBatch: output cleanup threw");
    }
    try {
        if (!ctx->released.exchange(true)) mgr->release_model(ctx->model_id);
    } catch (...) {
        LOG_ERROR("InferBatch: model release threw");
    }

    ctx->done.store(true);
    try {
        std::lock_guard<std::mutex> lock(sync->mtx);
        if (--sync->remaining <= 0) sync->cv.notify_all();
    } catch (...) {
        LOG_ERROR("InferBatch: completion signaling threw");
    }
    if (scheduler) scheduler->complete_external_async();
}

struct BatchFinishGuard {
    ModelManager* mgr;
    InferenceScheduler* scheduler;
    std::shared_ptr<InferBatchItemCtx> ctx;
    std::shared_ptr<InferBatchSyncState> sync;
    int max_outputs;
    bool armed = true;

    void disarm() noexcept { armed = false; }

    ~BatchFinishGuard() noexcept {
        if (armed) finish_batch_item(mgr, scheduler, ctx, sync, max_outputs);
    }
};

struct BatchModelRefGuard {
    ModelManager* mgr;
    std::shared_ptr<InferBatchItemCtx> ctx;
    bool armed = true;

    void disarm() noexcept { armed = false; }

    ~BatchModelRefGuard() noexcept {
        if (!armed || !ctx->acquired || ctx->released.exchange(true)) return;
        try {
            mgr->release_model(ctx->model_id);
        } catch (...) {
            LOG_ERROR("InferBatch: model release threw");
        }
    }
};

// ── NPU batch aggregation ─────────────────────────────────────────────────────
// One HAL job covering B consecutive same-model items (B = the model's
// registered batch_size). The group owns the concatenated input buffers and
// the HAL-allocated batch outputs; items hold per-frame views. The group is
// deleted by the LAST completing per-item post task (or synchronously on the
// failure paths), never by the submitting RPC thread — late callbacks after an
// RPC timeout must stay safe.
struct InferBatchGroupCtx {
    ModelManager*                        mgr = nullptr;
    SessionManager*                      smgr = nullptr;
    PostprocessPool*                     pool = nullptr;
    std::shared_ptr<InferBatchSyncState> sync;
    std::vector<std::shared_ptr<InferBatchItemCtx>> items;  // real (non-pad) items
    std::vector<std::string>             input_data;        // per input idx: B frames concatenated
    std::vector<HalTensor>               inputs;            // point into input_data
    std::vector<HalTensor>               outputs;           // HAL batch outputs (priv-owned)
    int                                  max_outputs = 0;
    uint32_t                             frames = 0;        // B incl. padding frames
    std::vector<uint32_t>                per_frame_out_bytes;
    std::atomic<int>                     pending_posts{0};
};

// Per-item completion work shared by the single-item and grouped InferBatch
// paths: snapshot raw outputs to proto, run post-processing, free tensors
// (unless the item only holds a view), record stats, release the model ref,
// and signal the RPC waiter. Runs on the PostprocessPool so the HailoRT
// completion thread can immediately service the next callback.
static void infer_batch_item_post(InferBatchItemCtx* ctx,
                                  const std::shared_ptr<InferBatchSyncState>& sync,
                                  ModelManager* mgr, SessionManager* smgr,
                                  Microseconds elapsed,
                                  InferBatchGroupCtx* group = nullptr) {
    const int max_out = ctx->max_outputs;

    // Snapshot raw outputs to proto.
    for (int k = 0; k < max_out; k++) {
        auto* pt = ctx->response.add_outputs();
        pt->set_dtype(AIRuntimeServiceImpl::hal_dtype_to_proto(ctx->outputs[k].dtype));
        for (int d = 0; d < ctx->outputs[k].ndim; d++)
            pt->add_shape(ctx->outputs[k].shape[d]);
        if (ctx->outputs[k].data && ctx->outputs[k].byte_size > 0)
            pt->set_data(ctx->outputs[k].data, ctx->outputs[k].byte_size);
    }

    // Post-processing. The Hailo SDK postprocess library may throw
    // std::invalid_argument when the HEF's nms output tensor name does not
    // match the postprocess config (e.g. a misconfigured/renamed
    // model registered under the wrong type). Catch here so a bad model
    // degrades to a failed inference instead of:
    //   • propagating out of the synchronous fallback path (→ terminate
    //     → SIGABRT, core.grpcpp_sync_ser), and
    //   • leaving ctx->done=false on the pool path (→ batch timeout +
    //     model ref leak).
    bool pp_failed = false;
    if (mgr->has_post_ops() && ctx->snap->post_session) {
        HalPostprocessResult post_result{};
        try {
            if (mgr->post_process(ctx->snap->post_session,
                                  ctx->outputs.data(), max_out,
                                  &post_result) == 0) {
                fill_proto_post_result(ctx->response.mutable_post_result(),
                                       post_result);
            }
        } catch (const std::exception& e) {
            LOG_ERROR("Postprocess failed for model '%s': %s",
                      ctx->model_id.c_str(), e.what());
            pp_failed = true;
            ctx->response.mutable_status()->set_success(false);
            ctx->response.mutable_status()->set_message(
                std::string("Postprocess failed: ") + e.what());
        }
        mgr->free_post_result(&post_result);
    }

    // Views (grouped items) borrow the group's buffers: the group frees the
    // real tensors after every post task has drained (below).
    if (!ctx->outputs_are_view)
        mgr->free_outputs(ctx->outputs.data(), max_out);
    if (!pp_failed) {
        ctx->response.mutable_status()->set_success(true);
        ctx->success = true;
    }
    ctx->response.set_infer_time_us(static_cast<uint64_t>(elapsed.count()));

    if (ctx->infer_session)
        smgr->record_inference(ctx->infer_session.get(),
                               static_cast<uint64_t>(elapsed.count()));

    // Release model ref here (self-sufficient: works even if RPC timed out).
    // Atomic exchange prevents double-release if the RPC thread also tries.
    if (!ctx->released.exchange(true)) {
        mgr->release_model(ctx->model_id);
    }

    ctx->done.store(true);
    {
        std::lock_guard<std::mutex> lk(sync->mtx);
        if (--sync->remaining <= 0)
            sync->cv.notify_all();
    }

    // Group teardown: the last post task frees the batch tensors and the
    // group itself. Every other post task has already copied what it needed
    // out of ctx->outputs (views into group->outputs), so the last one out
    // can safely release the real buffers.
    if (group && group->pending_posts.fetch_sub(1) == 1) {
        mgr->free_outputs(group->outputs.data(), group->max_outputs);
        delete group;
    }
}

// HAL completion callback for a grouped batch job: slices the batch outputs
// into per-frame views and runs the shared per-item post work on the pool.
static void infer_batch_group_callback(HalTensor* /*outputs*/, int /*num_outputs*/,
                                       int status, void* userdata) {
    auto* g = static_cast<InferBatchGroupCtx*>(userdata);
    if (!g) return;

    if (status != 0) {
        // Failure path: no post-processing needed, handle synchronously.
        for (auto& c : g->items) {
            if (!c->released.exchange(true)) {
                g->mgr->release_model(c->model_id);
            }
            c->response.mutable_status()->set_success(false);
            c->response.mutable_status()->set_message(
                "Inference failed: " + std::to_string(status));
            c->done.store(true);
            c->success = false;
            {
                std::lock_guard<std::mutex> lk(g->sync->mtx);
                if (--g->sync->remaining <= 0)
                    g->sync->cv.notify_all();
            }
        }
        g->mgr->free_outputs(g->outputs.data(), g->max_outputs);
        delete g;
        return;
    }

    // Success: give every real item its per-frame view of the batch outputs.
    for (auto& c : g->items) {
        c->max_outputs = g->max_outputs;
        c->outputs.assign(g->max_outputs, HalTensor{});
        for (int k = 0; k < g->max_outputs; k++) {
            HalTensor& v = c->outputs[k];
            v = g->outputs[k];  // copy name/dtype/shape/priv metadata
            if (g->outputs[k].data && g->per_frame_out_bytes[k] > 0) {
                v.data = static_cast<uint8_t*>(g->outputs[k].data) +
                         static_cast<uint64_t>(c->batch_frame) *
                             g->per_frame_out_bytes[k];
                v.byte_size = g->per_frame_out_bytes[k];
            }
            v.priv = nullptr;  // view — freed only via the group
        }
        c->outputs_are_view = true;
    }

    // Offload per-item work to the PostprocessPool so this HailoRT completion
    // thread can immediately service the next async inference callback.
    for (auto& c : g->items) {
        auto elapsed = std::chrono::duration_cast<Microseconds>(
            SteadyClock::now() - c->start);
        auto* ctx  = c.get();
        auto  sync = g->sync;
        auto* mgr  = g->mgr;
        auto* smgr = g->smgr;
        auto* grp  = g;
        PostprocessPool::Task post_task = [ctx, sync, mgr, smgr, elapsed, grp]() {
            infer_batch_item_post(ctx, sync, mgr, smgr, elapsed, grp);
        };
        if (!g->pool || !g->pool->submit(post_task)) {
            post_task();  // synchronous fallback (same policy as single items)
        }
    }
    // g is NOT deleted here: the last post task owns the teardown.
}

}  // namespace

static void complete_infer_batch_callback(
    InferBatchCbState* state) noexcept {
    struct CallbackOwnerGuard {
        InferBatchCbState* state;
        ~CallbackOwnerGuard() noexcept { state->release_owner(); }
    } callback_owner{state};

    auto ctx = state->ctx;
    auto sync = state->sync;
    auto* mgr = state->mgr;
    auto* smgr = state->smgr;
    auto* scheduler = state->scheduler;
    const int max_outputs = ctx->max_outputs;
    const int result_outputs = state->callback_num_outputs;
    BatchFinishGuard immediate_finish{
        mgr, scheduler, ctx, sync, max_outputs};

    try {
        if (state->callback_status != 0) {
            ctx->response.mutable_status()->set_success(false);
            ctx->response.mutable_status()->set_message(
                "Inference failed: " +
                std::to_string(state->callback_status));
            ctx->success = false;
            return;
        }

        const auto elapsed = std::chrono::duration_cast<Microseconds>(
            SteadyClock::now() - ctx->start);

        const auto dtype_to_proto = state->dtype_to_proto;
        PostprocessPool::Task post_task =
            [ctx, sync, mgr, smgr, scheduler, dtype_to_proto, max_outputs,
             result_outputs, elapsed]() noexcept {
                BatchFinishGuard finish{
                    mgr, scheduler, ctx, sync, max_outputs};

                try {
                    for (int k = 0; k < result_outputs; k++) {
                        auto* pt = ctx->response.add_outputs();
                        pt->set_dtype(dtype_to_proto(ctx->outputs[k].dtype));
                        for (int d = 0; d < ctx->outputs[k].ndim; d++) {
                            pt->add_shape(ctx->outputs[k].shape[d]);
                        }
                        if (ctx->outputs[k].data &&
                            ctx->outputs[k].byte_size > 0) {
                            pt->set_data(ctx->outputs[k].data,
                                         ctx->outputs[k].byte_size);
                        }
                    }

                    bool post_failed = false;
                    if (mgr->has_post_ops() && ctx->snap->post_session) {
                        HalPostprocessResult post_result{};
                        struct PostResultGuard {
                            ModelManager* mgr;
                            HalPostprocessResult* result;
                            ~PostResultGuard() noexcept {
                                try {
                                    mgr->free_post_result(result);
                                } catch (...) {
                                    LOG_ERROR(
                                        "InferBatch: post-result cleanup threw");
                                }
                            }
                        } post_guard{mgr, &post_result};

                        try {
                            if (mgr->post_process(
                                    ctx->snap->post_session,
                                    ctx->outputs.data(), result_outputs,
                                    &post_result) == 0) {
                                fill_proto_post_result(
                                    ctx->response.mutable_post_result(),
                                    post_result);
                            }
                        } catch (const std::exception& e) {
                            LOG_ERROR(
                                "Postprocess failed for model '%s': %s",
                                ctx->model_id.c_str(), e.what());
                            post_failed = true;
                            ctx->response.mutable_status()->set_success(false);
                            ctx->response.mutable_status()->set_message(
                                std::string("Postprocess failed: ") + e.what());
                        }
                    }

                    if (!post_failed) {
                        ctx->response.mutable_status()->set_success(true);
                        ctx->success = true;
                    }
                    ctx->response.set_infer_time_us(
                        static_cast<uint64_t>(elapsed.count()));
                    if (ctx->infer_session) {
                        smgr->record_inference(
                            ctx->infer_session.get(),
                            static_cast<uint64_t>(elapsed.count()));
                    }
                } catch (const std::exception& e) {
                    LOG_ERROR(
                        "InferBatch completion failed for model '%s': %s",
                        ctx->model_id.c_str(), e.what());
                    ctx->success = false;
                    try {
                        ctx->response.mutable_status()->set_success(false);
                        ctx->response.mutable_status()->set_message(
                            "Inference completion failed");
                    } catch (...) {
                    }
                } catch (...) {
                    LOG_ERROR("InferBatch completion failed for model '%s'",
                              ctx->model_id.c_str());
                    ctx->success = false;
                    try {
                        ctx->response.mutable_status()->set_success(false);
                        ctx->response.mutable_status()->set_message(
                            "Inference completion failed");
                    } catch (...) {
                    }
                }
            };

        // From this point the task owns finish_batch_item(), whether queued or
        // executed synchronously as the bounded-pool fallback.
        immediate_finish.disarm();
        if (!state->pool || !state->pool->submit(post_task)) post_task();
    } catch (const std::exception& e) {
        LOG_ERROR("InferBatch callback setup failed for model '%s': %s",
                  ctx->model_id.c_str(), e.what());
        ctx->success = false;
        try {
            ctx->response.mutable_status()->set_success(false);
            ctx->response.mutable_status()->set_message(
                "Inference completion failed");
        } catch (...) {
        }
    } catch (...) {
        LOG_ERROR("InferBatch callback setup failed for model '%s'",
                  ctx->model_id.c_str());
        ctx->success = false;
        try {
            ctx->response.mutable_status()->set_success(false);
            ctx->response.mutable_status()->set_message(
                "Inference completion failed");
        } catch (...) {
        }
    }
}

void AIRuntimeServiceImpl::InferBatchCallback(
    HalTensor* /*outputs*/, int num_outputs, int status,
    void* userdata) noexcept {
    auto* state = static_cast<InferBatchCbState*>(userdata);
    if (!state) return;

    state->callback_status = status;
    state->callback_num_outputs =
        std::max(0, std::min(num_outputs, state->ctx->max_outputs));
    const auto action = state->gate.on_callback();
    if (action == AsyncCallbackAction::CompleteHere)
        complete_infer_batch_callback(state);
}

// ─── InferBatch (parallel multi-model inference) ──────────────────────────────

grpc::Status AIRuntimeServiceImpl::InferBatch(
    grpc::ServerContext* /*ctx*/,
    const pb::InferBatchRequest* req,
    pb::InferBatchResponse* resp) {

    int num_requests = req->requests_size();
    if (num_requests == 0 || num_requests > kMaxInferBatchRequests) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message(
            num_requests == 0 ? "Empty batch" : "Batch exceeds 64 requests");
        return grpc::Status::OK;
    }

    uint64_t aggregate_input_bytes = 0;
    for (const auto& item : req->requests()) {
        std::string input_error;
        if (!validate_infer_inputs(item, &input_error)) {
            resp->mutable_status()->set_success(false);
            resp->mutable_status()->set_message(std::move(input_error));
            return grpc::Status::OK;
        }
        for (const auto& tensor : item.inputs()) {
            aggregate_input_bytes += tensor.data().size();
            if (aggregate_input_bytes > kMaxInferBatchInputBytes) {
                resp->mutable_status()->set_success(false);
                resp->mutable_status()->set_message(
                    "Batch input payload exceeds 32 MiB");
                return grpc::Status::OK;
            }
        }
    }

    LOG_DEBUG("InferBatch: %d requests", num_requests);

    auto batch_start = SteadyClock::now();

    auto sync = std::make_shared<InferBatchSyncState>(num_requests);

    std::vector<std::shared_ptr<InferBatchItemCtx>> ctxs(num_requests);
    for (int i = 0; i < num_requests; i++)
        ctxs[i] = std::make_shared<InferBatchItemCtx>();

    // Decrement + notify for paths that complete without an async callback
    // (model-not-found, run_async submission failure).
    auto complete_now = [&sync]() {
        std::lock_guard<std::mutex> lk(sync->mtx);
        if (--sync->remaining <= 0)
            sync->cv.notify_all();
    };

    // Submit phase: launch every job from this thread. The shared NPU scheduler
    // (a single ROUND_ROBIN VDevice) interleaves them — no OS thread per item.
    for (int i = 0; i < num_requests; i++) {
        const auto& infer_req = req->requests(i);
        auto& c = ctxs[i];
        c->index    = i;
        c->model_id = infer_req.model_id();
        c->start    = SteadyClock::now();

        c->snap = model_mgr_->acquire_model_snapshot(infer_req.model_id());
        if (!c->snap) {
            c->response.mutable_status()->set_success(false);
            c->response.mutable_status()->set_message("Model not found");
            c->done.store(true);
            complete_now();
            continue;
        }
        // Idempotent implicit session for stats tracking (one per model),
        // created only after the model snapshot resolves (same ordering and
        // ""-tolerance as the Infer() path above).
        std::string implicit_session_id = "implicit-" + infer_req.model_id();
        session_mgr_->create_named_session(
            implicit_session_id, "implicit", "infer", infer_req.model_id(),
            0, 0, 5);
        c->infer_session = session_mgr_->get_session(implicit_session_id);
        c->acquired = true;  // ref_count bumped; released after the callback fires
        BatchModelRefGuard model_ref_guard{model_mgr_, c};
        if (infer_req.inputs_size() != c->snap->model_info.num_inputs) {
            c->response.mutable_status()->set_success(false);
            c->response.mutable_status()->set_message(
                "Input count does not match model");
            if (!c->released.exchange(true)) {
                model_mgr_->release_model(c->model_id);
            }
            c->done.store(true);
            complete_now();
            continue;
        }

        // Convert proto tensors → HalTensor. CPU payloads are copied into
        // ctx-owned storage so the buffers outlive the RPC for late callbacks.
        const int num_inputs = infer_req.inputs_size();
        c->input_data.resize(num_inputs);
        c->inputs.resize(num_inputs);
        bool item_failed = false;
        std::string fail_msg;
        for (int j = 0; j < num_inputs; j++) {
            const auto& pb_t = infer_req.inputs(j);
            HalTensor&  ht   = c->inputs[j];
            std::memset(&ht, 0, sizeof(HalTensor));
            if (pb_t.dma_fd() > 0) {
                // Cross-process raw fd — same red line as single Infer.
                item_failed = true;
                fail_msg = "Tensor.dma_fd rejected: a raw fd number cannot "
                           "cross processes; pass the frame as bytes or a "
                           "buffer_id";
                break;
            }
            bool frame_geometry = false;  // dtype/shape from the looked-up frame
            if (pb_t.buffer_id() != 0) {
                if (!buffer_lookup_) {
                    item_failed = true;
                    fail_msg = "Tensor.buffer_id rejected: buffer registry not configured";
                    break;
                }
                auto lr = buffer_lookup_->lookup(pb_t.buffer_id());
                if (lr.rc != 0) {
                    item_failed = true;
                    fail_msg = "Tensor.buffer_id lookup failed: " + lr.message;
                    break;
                }
                std::string why;
                // P1-2: fd direct-bind first (first input, single-frame
                // models only — NPU-batch grouping needs contiguous CPU
                // frames). try_bind_dma_input prechecks format/geometry;
                // off-contract frames repack or convert as before.
                const bool rgb_model = c->snap->model_info.num_inputs > 0 &&
                    !c->snap->model_info.inputs[0].is_nv12;
                const bool bound = j == 0 && c->snap->batch_size == 1 &&
                    try_bind_dma_input(c->snap->infer_session, model_mgr_,
                                       c->snap->model_info, lr,
                                       pb_t.buffer_id(), buffer_lookup_, ht,
                                       c->dma_lease);
                if (!bound) {
                    bool prepared = false;
                    const char* prep = "repack";
                    if (j == 0 && c->snap->batch_size == 1 && rgb_model) {
                        // Packed-RGB model: convert through the session-aware
                        // preprocess — the NV12 repack's byte_size can never
                        // match and run() would reject every frame.
                        prep = "conversion";
                        prepared = convert_nv12_frame_to_input(
                            c->snap->infer_session, model_mgr_, lr.width,
                            lr.height, lr.num_planes, lr.strides, lr.sizes,
                            lr.fds, ht, why);
                        if (prepared) {
                            // Tensor owns its buffer; the lease free_tensor's
                            // it once the NPU read completes.
                            if (!c->dma_lease) {
                                c->dma_lease      = std::make_shared<DmaInputLease>();
                                c->dma_lease->mgr = model_mgr_;
                            }
                            c->dma_lease->bound.push_back(ht);
                        }
                    } else {
                        prepared = repack_nv12_planes(lr, c->input_data[j], why);
                        if (prepared) {
                            const uint32_t w = lr.width, h = lr.height;
                            ht.data      = const_cast<char*>(c->input_data[j].data());
                            ht.byte_size = static_cast<uint32_t>(w) * h * 3 / 2;
                            ht.dma_fd    = -1;
                            // Geometry comes from the daemon's registry, not the wire.
                            ht.dtype     = HAL_DTYPE_UINT8;
                            ht.ndim      = 2;
                            ht.shape[0]  = static_cast<int32_t>(h * 3 / 2);
                            ht.shape[1]  = static_cast<int32_t>(w);
                        }
                    }
                    for (int fd : lr.fds) close(fd);
                    // Lease dropped once our own copy exists (or cannot be made).
                    buffer_lookup_->release(pb_t.buffer_id());
                    if (!prepared) {
                        item_failed = true;
                        fail_msg = std::string("Tensor.buffer_id ") + prep +
                                   " failed: " + why;
                        break;
                    }
                }
                // dtype/shape/byte_size of a bound tensor come from
                // bind_dma_frame; both arms carry frame geometry.
                frame_geometry = true;
            } else {
                c->input_data[j].assign(pb_t.data().data(), pb_t.data().size());
                ht.data      = const_cast<char*>(c->input_data[j].data());
                ht.byte_size = static_cast<uint32_t>(c->input_data[j].size());
                ht.dma_fd    = -1;
            }
            if (!frame_geometry) {
                ht.dtype = proto_dtype_to_hal(pb_t.dtype());
                ht.ndim  = static_cast<int32_t>(pb_t.shape_size());
                for (int d = 0; d < ht.ndim && d < HAL_MAX_TENSOR_DIMS; d++)
                    ht.shape[d] = pb_t.shape(d);
            }
        }
        if (item_failed) {
            // Release the model ref taken above — no async callback will fire.
            if (!c->released.exchange(true)) {
                model_mgr_->release_model(c->model_id);
            }
            c->response.mutable_status()->set_success(false);
            c->response.mutable_status()->set_message(fail_msg);
            c->done.store(true);
            complete_now();
            continue;
        }

        // NPU-batch models (batch>1) do not submit inline: consecutive
        // same-model frames are grouped into one NPU job by the grouped
        // submit phase after the per-frame validation pass. Disarm the
        // guard — the model ref rides with the item until the group's
        // post tasks release it.
        if (c->snap->batch_size > 1) {
            model_ref_guard.disarm();
            continue;
        }

        c->max_outputs = c->snap->num_outputs;
        c->outputs.assign(c->max_outputs, HalTensor{});

        InferBatchCbState* cb_state = nullptr;
        try {
            auto setup_state = std::make_unique<InferBatchCbState>();
            setup_state->mgr = model_mgr_;
            setup_state->smgr = session_mgr_;
            setup_state->pool = postprocess_pool_;
            setup_state->scheduler = scheduler_;
            setup_state->dtype_to_proto =
                &AIRuntimeServiceImpl::hal_dtype_to_proto;
            setup_state->ctx = c;
            setup_state->sync = sync;
            cb_state = setup_state.release();
        } catch (const std::exception& e) {
            LOG_ERROR("InferBatch callback-state allocation failed: %s",
                      e.what());
            c->response.mutable_status()->set_success(false);
            c->response.mutable_status()->set_message(
                "Inference submission setup failed");
            c->done.store(true);
            complete_now();
            continue;
        } catch (...) {
            LOG_ERROR("InferBatch callback-state allocation failed");
            c->response.mutable_status()->set_success(false);
            c->response.mutable_status()->set_message(
                "Inference submission setup failed");
            c->done.store(true);
            complete_now();
            continue;
        }

        scheduler_->begin_external_async();
        int rc = HAL_ERROR;
        try {
            rc = model_mgr_->run_async(
                c->snap->infer_session, c->inputs.data(), num_inputs,
                c->outputs.data(), c->max_outputs,
                &AIRuntimeServiceImpl::InferBatchCallback, cb_state);
        } catch (const std::exception& e) {
            LOG_ERROR("InferBatch submission threw for model '%s': %s",
                      c->model_id.c_str(), e.what());
        } catch (...) {
            LOG_ERROR("InferBatch submission threw for model '%s'",
                      c->model_id.c_str());
        }

        const auto action = cb_state->gate.on_return(rc == HAL_OK);
        if (action == AsyncReturnAction::AwaitCallback) {
            model_ref_guard.disarm();
            cb_state->release_owner();  // submitter owner
            continue;
        }
        if (action == AsyncReturnAction::CompleteHere) {
            model_ref_guard.disarm();
            complete_infer_batch_callback(cb_state);
            cb_state->release_owner();  // submitter owner
            continue;
        }

        try {
            c->response.mutable_status()->set_success(false);
            c->response.mutable_status()->set_message(
                "Inference submission failed: " + std::to_string(rc));
        } catch (...) {
            LOG_ERROR("InferBatch: failed to build submission error");
        }
        c->success = false;
        finish_batch_item(model_mgr_, scheduler_, c, sync, c->max_outputs);
        cb_state->release_owner();  // unused callback owner
        cb_state->release_owner();  // submitter owner
    }

    // ── Per-frame size validation for batch models (NPU-batch) ────────────
    // A batch>1 session expects each item to carry exactly the per-frame
    // byte count (model_info sizes are B x per-frame). Fail mismatched items
    // up front so grouping never builds a torn batch buffer. Inline-submitted
    // items (batch<=1) skip this pass; deferred batch items were prepped in
    // the materialization loop above.
    for (int i = 0; i < num_requests; i++) {
        auto& c = ctxs[i];
        if (!c->acquired || !c->snap || c->snap->batch_size <= 1) continue;
        const uint32_t  B  = c->snap->batch_size;
        const auto&     mi = c->snap->model_info;
        const int      nin = static_cast<int>(c->inputs.size());
        bool bad = false;
        std::string why;
        if (nin != static_cast<int>(mi.num_inputs)) {
            bad = true;
            why = "expects " + std::to_string(mi.num_inputs) +
                  " inputs, got " + std::to_string(nin);
        } else {
            for (int k = 0; k < nin && !bad; k++) {
                const uint32_t per_frame = mi.inputs[k].byte_size / B;
                if (per_frame == 0 || c->inputs[k].byte_size != per_frame) {
                    bad = true;
                    why = "input[" + std::to_string(k) + "] byte_size " +
                          std::to_string(c->inputs[k].byte_size) +
                          " != per-frame " + std::to_string(per_frame) +
                          " (model batch=" + std::to_string(B) + ")";
                }
            }
        }
        if (bad) {
            if (!c->released.exchange(true)) {
                model_mgr_->release_model(c->model_id);
            }
            c->acquired = false;
            c->response.mutable_status()->set_success(false);
            c->response.mutable_status()->set_message("Batch model input mismatch: " + why);
            c->done.store(true);
            complete_now();
        }
    }

    // ── Grouped NPU-batch submit phase (batch>1 models) ────────────────────
    // Single-frame items were submitted inline in the materialization loop
    // above. Here, consecutive same-model items of a batch>1 model collapse
    // into ONE NPU job per B frames: the array runs all B frames in parallel
    // inside a single job instead of B serialized jobs. Grouped jobs predate
    // the external-async accounting and stay outside it — their teardown is
    // self-sufficient (sync->remaining + pending_posts).
    int i = 0;
    while (i < num_requests) {
        auto& c = ctxs[i];
        if (!c->acquired) { i++; continue; }  // failed during materialization
        if (c->snap->batch_size <= 1) { i++; continue; }  // submitted inline
        const uint32_t B = c->snap->batch_size;

        // ── Grouped batch job ──
        // Collect the maximal run [i, j) of consecutive acquired items that
        // share this model and batch size, then submit ceil(run/B) jobs.
        int j = i + 1;
        while (j < num_requests && ctxs[j]->acquired &&
               ctxs[j]->model_id == c->model_id &&
               ctxs[j]->snap && ctxs[j]->snap->batch_size == B) {
            j++;
        }

        const auto& mi   = c->snap->model_info;
        const int   nin  = static_cast<int>(c->inputs.size());
        const int   nout = c->snap->num_outputs;

        std::vector<uint32_t> pf_in(nin), pf_out(nout);
        bool geo_ok = true;
        for (int k = 0; k < nin && geo_ok; k++)
            if ((pf_in[k] = mi.inputs[k].byte_size / B) == 0) geo_ok = false;
        for (int k = 0; k < nout && geo_ok; k++)
            if ((pf_out[k] = mi.outputs[k].byte_size / B) == 0) geo_ok = false;
        if (!geo_ok) {
            // Degenerate model_info (zero per-frame size): fail the whole run.
            for (int q = i; q < j; q++) {
                auto& qc = ctxs[q];
                if (!qc->released.exchange(true)) {
                    model_mgr_->release_model(qc->model_id);
                }
                qc->response.mutable_status()->set_success(false);
                qc->response.mutable_status()->set_message(
                    "Batch model geometry unavailable (zero per-frame size)");
                qc->done.store(true);
                complete_now();
            }
            i = j;
            continue;
        }

        for (int off = i; off < j; off += (int)B) {
            const int n = std::min<int>((int)B, j - off);  // real items in this chunk

            auto* g = new InferBatchGroupCtx;
            g->mgr  = model_mgr_;
            g->smgr = session_mgr_;
            g->pool = postprocess_pool_;
            g->sync = sync;
            g->frames        = B;
            g->max_outputs   = nout;
            g->per_frame_out_bytes = pf_out;
            g->pending_posts.store(n);
            g->items.reserve(n);

            // Concatenate B frames per input; padding frames duplicate the
            // last real item's pixels (their results are discarded — only the
            // NPU needs a full batch).
            g->input_data.resize(nin);
            for (int r = 0; r < (int)B; r++) {
                const bool pad = (r >= n);
                auto& src = pad ? ctxs[off + n - 1] : ctxs[off + r];
                if (!pad) {
                    src->batch_frame = r;
                    g->items.push_back(src);
                }
                for (int k = 0; k < nin; k++) {
                    g->input_data[k].append(
                        static_cast<const char*>(src->inputs[k].data), pf_in[k]);
                }
            }

            g->inputs.resize(nin);
            for (int k = 0; k < nin; k++) {
                HalTensor& ht = g->inputs[k];
                std::memset(&ht, 0, sizeof(HalTensor));
                ht.data      = const_cast<char*>(g->input_data[k].data());
                ht.byte_size = static_cast<uint32_t>(g->input_data[k].size());
                ht.dma_fd    = -1;
                ht.dtype     = mi.inputs[k].dtype;
                ht.ndim      = mi.inputs[k].ndim;
                for (int d = 0; d < ht.ndim && d < HAL_MAX_TENSOR_DIMS; d++)
                    ht.shape[d] = mi.inputs[k].shape[d];
                if (ht.ndim >= 1)
                    ht.shape[0] = static_cast<int32_t>(B);
            }
            g->outputs.assign(nout, HalTensor{});

            int rc = model_mgr_->run_async(c->snap->infer_session,
                                           g->inputs.data(), nin,
                                           g->outputs.data(), nout,
                                           infer_batch_group_callback, g);
            if (rc != 0) {
                // Submission failed: no callback will fire. Clean up + signal.
                for (auto& ic : g->items) {
                    if (!ic->released.exchange(true)) {
                        model_mgr_->release_model(ic->model_id);
                    }
                    ic->response.mutable_status()->set_success(false);
                    ic->response.mutable_status()->set_message(
                        "Inference submission failed: " + std::to_string(rc));
                    ic->done.store(true);
                    complete_now();
                }
                model_mgr_->free_outputs(g->outputs.data(), g->max_outputs);
                delete g;
            }
        }
        i = j;
    }

    // Wait for callbacks with a single timeout. Items not done by
    // then are reported as "Batch timeout". Late callbacks still fire
    // and release their own refs (via PostprocessPool).
    const uint32_t batch_timeout_ms = req->timeout_ms() > 0 ? req->timeout_ms() : 10000;
    {
        std::unique_lock<std::mutex> lk(sync->mtx);
        sync->cv.wait_for(lk, std::chrono::milliseconds(batch_timeout_ms),
                          [&] { return sync->remaining <= 0; });
    }

    // Collect responses in original order. For items whose callback has not
    // fired, emit a fresh timeout response without touching c->response — a
    // late callback may still write to it (safely, into the shared_ptr ctx).
    int success_count = 0;
    for (int i = 0; i < num_requests; i++) {
        auto& c = ctxs[i];
        if (c->done.load()) {
            if (c->success) success_count++;
            *resp->add_responses() = std::move(c->response);
        } else {
            auto* out = resp->add_responses();
            out->mutable_status()->set_success(false);
            out->mutable_status()->set_message("Batch timeout");
        }
    }

    // post_task is self-sufficient: it releases its own model ref when
    // it completes. The RPC thread does NOT release any refs here —
    // releasing a ref for an item whose post_task hasn't run yet would
    // allow the model to be unregistered, destroying snap->post_session
    // that the late post_task still needs (use-after-free).
    //
    // For items whose callback never fired (done == false), we keep
    // the ref (leak) to protect snap->post_session. The late post_task
    // will release it when it eventually runs. If the callback truly
    // never fires, the ref leaks permanently — this is the safe
    // trade-off (leak vs UAF).
    for (int i = 0; i < num_requests; i++) {
        auto& c = ctxs[i];
        if (!c->done.load() && !c->released.load()) {
            LOG_ERROR("InferBatch item %d: callback/post_task never fired "
                      "(model '%s' ref leaked for safety)",
                      c->index, c->model_id.c_str());
        }
    }

    auto batch_elapsed = std::chrono::duration_cast<Microseconds>(
        SteadyClock::now() - batch_start);
    resp->set_total_time_us(static_cast<uint64_t>(batch_elapsed.count()));

    resp->mutable_status()->set_success(success_count == num_requests);
    if (success_count < num_requests) {
        resp->mutable_status()->set_message(
            std::to_string(success_count) + "/" + std::to_string(num_requests) + " succeeded");
    } else {
        resp->mutable_status()->set_message("All inferences completed");
    }

    LOG_DEBUG("InferBatch: %d/%d succeeded in %llu us",
              success_count, num_requests,
              (unsigned long long)batch_elapsed.count());

    return grpc::Status::OK;
}

// ─── StreamInfer (zero-copy via FdReceiver) ──────────────────────────────────

grpc::Status AIRuntimeServiceImpl::StreamInfer(
    grpc::ServerContext* ctx,
    const pb::StreamInferRequest* req,
    grpc::ServerWriter<pb::StreamInferResponse>* writer) {

    try {
    if (!valid_stream_id(req->stream_id())) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "stream_id must be 1-63 alphanumeric, '_' or '-'");
    }

    LOG_INFO("StreamInfer: stream_id=%s model_id=%s fps_limit=%u",
             req->stream_id().c_str(), req->model_id().c_str(), req->fps_limit());

    if (!valid_stream_class_filter_size(req->class_filter_size())) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "class_filter exceeds 256 entries");
    }
    if (!valid_stream_min_confidence(req->min_confidence())) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "min_confidence must be finite and within [0,1]");
    }
    if (!valid_stream_fps(req->fps_limit())) {
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                            "fps_limit exceeds 120");
    }

    std::string peer = ctx->peer();
    if (peer.empty()) peer = "<unknown>";
    auto rpc_permit = stream_admission_.try_acquire_rpc(
        peer, req->stream_id());
    if (!rpc_permit) {
        return grpc::Status(grpc::StatusCode::RESOURCE_EXHAUSTED,
                            "stream admission limit reached");
    }

    // Prepare the guard before acquisition so copying the model id cannot leak
    // a live ref if allocation throws.
    ModelGuard model_guard{model_mgr_, req->model_id()};
    auto snap = model_mgr_->acquire_model_snapshot(req->model_id());
    if (!snap) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Model not found");
    }
    model_guard.arm();

    // Create session
    std::string session_id = session_mgr_->create_session(
        req->session_id(), req->stream_id(), req->model_id(),
        req->fps_limit(), 0, 5);

    // The guard fires on every StreamInfer exit — clean finish, error
    // return, or client disconnect (ctx cancellation breaks the loop): the
    // session is destroyed and a tagged session additionally broadcasts
    // session/end so the daemon-side overlay sidecars are swept (P2-13).
    // The sweep tag is req->session_id() — the client-facing string the
    // SDK stamps on its annotate events — NOT the internal session id
    // (app-stream-model-ts) the daemon would never have seen.
    struct SessionGuard {
        SessionManager* mgr; std::string id;
        AIRuntimeServiceImpl* svc = nullptr;
        std::string client_tag;
        ~SessionGuard() {
            mgr->destroy_session(id);
            if (svc) svc->publish_session_end(client_tag);
        }
    } guard{session_mgr_, session_id, this, req->session_id()};

    auto session = session_mgr_->get_session(session_id);
    if (!session) {
        return grpc::Status(
            grpc::StatusCode::RESOURCE_EXHAUSTED,
            "session creation failed (invalid id or session limit reached)");
    }

    HalPostprocessSession* pp_session = snap->post_session;
    int max_outputs = snap->num_outputs;
    bool enable_post = model_mgr_->has_post_ops() && !req->raw_output_only();
    auto model_id = req->model_id();
    auto stream_id = req->stream_id();

    // ── On-demand result shaping (P1-5): by-value captures for the
    // per-frame on_complete. Absent fields default to "no filtering" —
    // wire-compatible with older clients that never set them.
    const uint32_t flt_max_results = req->max_detections();
    const float flt_min_confidence = req->min_confidence();
    const auto flt_class_filter =
        std::make_shared<const std::unordered_set<int32_t>>(
            req->class_filter().begin(), req->class_filter().end());
    const bool flt_omit_labels = req->omit_labels();
    const bool flt_only_nonempty = req->only_nonempty();
    uint32_t suppressed_responses = 0;  // only_nonempty drops (logged at end)

    // Subscribe to FdReceiver for zero-copy DMA-BUF frames. The buffer state is
    // shared with the receive callback: on close it is quiesced (reject new
    // frames + acknowledge the buffered one) BEFORE the bounded unsubscribe, so
    // an in-flight callback after the deadline only holds state that
    // acknowledges instead of buffering — teardown never races the receiver.
    struct StreamFrameState {
        std::mutex mu;
        std::condition_variable cv;
        ReceivedFrame latest_frame{};
        bool has_frame = false;
        bool accepting_frames = true;
    };
    auto frame_state = std::make_shared<StreamFrameState>();

    bool fd_path = fd_receiver_->subscribe(
        stream_id,
        session_id,  // unique per gRPC call — enables multicast
        [frame_state](const ReceivedFrame& frame) {
            std::lock_guard lock(frame_state->mu);
            if (!frame_state->accepting_frames) {
                frame.delivery.acknowledge();
                return;
            }
            // Release previously buffered frame if unconsumed (backpressure)
            if (frame_state->has_frame &&
                frame_state->latest_frame.frame_id != 0) {
                frame_state->latest_frame.delivery.acknowledge();
            }
            frame_state->latest_frame = frame;
            frame_state->has_frame = true;
            frame_state->cv.notify_one();
        });

    struct SubscriptionGuard {
        FdReceiver* receiver;
        std::string stream_id;
        std::string subscriber_id;
        std::shared_ptr<StreamFrameState> state;
        bool active;

        void close() noexcept {
            if (!active) return;
            active = false;
            // 1. Quiesce callback state: reject new frames, release the
            //    buffered delivery. After this, an in-flight callback only
            //    acknowledges; it cannot extend the drain.
            {
                std::lock_guard lock(state->mu);
                state->accepting_frames = false;
                if (state->has_frame &&
                    state->latest_frame.frame_id != 0) {
                    state->latest_frame.delivery.acknowledge();
                }
                state->latest_frame = {};
                state->has_frame = false;
                state->cv.notify_all();
            }
            // 2. Bounded unsubscribe. The subscriber is removed from the live
            //    set first; a false return means a delivery is still retained
            //    elsewhere (e.g. a completion lambda). Those holders release
            //    through their own acknowledgement, and the shared state
            //    above keeps any late callback safe without blocking this RPC
            //    (or the process shutdown's own bounded drain) indefinitely.
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::seconds(5);
            if (!receiver->unsubscribe_until(stream_id, subscriber_id,
                                             deadline)) {
                LOG_ERROR("StreamInfer: unsubscribe for stream '%s' did not "
                          "quiesce before the deadline",
                          stream_id.c_str());
            }
        }
        ~SubscriptionGuard() { close(); }
    } subscription{fd_receiver_, stream_id, session_id, frame_state,
                   fd_path};

    if (!fd_path) {
        if (!cfg_.stream_simulation_enabled) {
            LOG_WARN("StreamInfer: FdReceiver unavailable for %s",
                     stream_id.c_str());
            return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                "camera frame source unavailable");
        }
        LOG_WARN("StreamInfer: FdReceiver unavailable for %s; "
                 "test-only simulation enabled",
                 stream_id.c_str());
    }

    // ── DSP preprocess pool (opt-in via cfg.stream_dsp_preprocess) ──────────
    // Record trusted model geometry now, but allocate no daemon buffers until
    // the first mismatched frame. Pools are shared by geometry across streams,
    // so matching/idle RPCs reserve nothing and concurrent models with the same
    // input shape draw from one bounded slot set.
    uint32_t model_in_w = 0;
    uint32_t model_in_h = 0;
    std::shared_ptr<StreamPreprocessPool> dsp_pool;
    auto next_dsp_pool_retry = SteadyClock::time_point::min();
    constexpr auto kDspPoolRetryDelay = std::chrono::seconds(1);
    if (fd_path) {
        const auto& mi = snap->model_info;
        // Model input geometry is only trusted when the NV12 reconciliation
        // in the HAL left one logical input with unambiguous shape[1]/shape[2]
        // that multiplies out to byte_size.
        if (mi.num_inputs == 1 && mi.inputs[0].is_nv12 &&
            mi.inputs[0].ndim >= 4 &&
            mi.inputs[0].shape[1] > 0 && mi.inputs[0].shape[2] > 0 &&
            static_cast<uint64_t>(static_cast<uint64_t>(mi.inputs[0].shape[2]) *
                                  static_cast<uint64_t>(mi.inputs[0].shape[1]) *
                                  3 / 2) ==
                static_cast<uint64_t>(mi.inputs[0].byte_size)) {
            model_in_w = static_cast<uint32_t>(mi.inputs[0].shape[2]);
            model_in_h = static_cast<uint32_t>(mi.inputs[0].shape[1]);
        }
    }

    // A zero fps_limit preserves the legacy unlimited fd-frame path. Positive
    // limits use a ceiling nanosecond interval so the gate never overshoots.
    const uint32_t fps = req->fps_limit();
    const bool has_fps_limit = fps > 0;
    const auto frame_interval = has_fps_limit
        ? std::chrono::nanoseconds((1000000000ULL + fps - 1) / fps)
        : std::chrono::nanoseconds::zero();
    const auto simulation_interval = has_fps_limit
        ? frame_interval : std::chrono::milliseconds(33);
    FrameRateGate fps_gate(fps);
    // Sequence-zero is a valid first frame; track "seen" separately from the
    // value so it is never dropped as a phantom duplicate.
    uint64_t last_seq = 0;
    bool has_last_seq = false;

    // Per-RPC count is paired with the service-wide admission controller. A
    // permit remains held through late completion cleanup, not just submission.
    auto in_flight = std::make_shared<std::atomic<uint32_t>>(0);

    // Cost of the previous response's Write(), reported one response late
    // (perf fields are finalized before the Write that would carry them).
    uint64_t last_write_us = 0;

    while (!ctx->IsCancelled()) {
        pb::StreamInferResponse resp;

        if (fd_path) {
            ReceivedFrame frame{};
            {
                std::unique_lock lock(frame_state->mu);
                // Bounded purely to poll cancellation; frame arrivals wake
                // this immediately.
                frame_state->cv.wait_for(
                    lock, Milliseconds(50),
                    [&] { return frame_state->has_frame || ctx->IsCancelled(); });
                if (ctx->IsCancelled()) break;
                if (!frame_state->has_frame) {
                    if (!fd_receiver_->stream_connected(stream_id)) {
                        LOG_WARN("StreamInfer: publisher connection lost for %s",
                                 stream_id.c_str());
                        break;
                    }
                    continue;
                }
                frame = frame_state->latest_frame;
                frame_state->latest_frame = {};
                frame_state->has_frame = false;
            }

            if (has_last_seq && frame.sequence == last_seq) {
                frame.delivery.acknowledge();
                continue;
            }

            // Frame-driven fps gate: submit a frame the moment it arrives,
            // provided one interval passed since the previous SUBMIT; a
            // frame landing inside the interval is released right away.
            // Buffering it until a slot opens (the old scheme: 1ms-spin on
            // check_fps_limit, then newest-buffered) aged every submitted
            // frame by up to one arrival period — at fps=10 on a 15fps
            // stream that alone was p50 ~33ms of skew against a ~7ms
            // compute pipeline. The cap stays a hard ceiling; undershoot
            // only occurs when the source rate doesn't divide the cap
            // (15fps capped at 10 submits 7.5fps) — freshness wins.
            // Re-anchoring after an arrival stall prevents burst credit.
            const auto submit_now = SteadyClock::now();
            if (!fps_gate.allow(submit_now)) {
                frame.delivery.acknowledge();
                continue;
            }
            last_seq = frame.sequence;
            has_last_seq = true;

            auto work_permit =
                stream_admission_.try_acquire_work(in_flight);
            if (!work_permit) {
                frame.delivery.acknowledge();
                LOG_DEBUG("StreamInfer: admission drop (rpc_in_flight=%u)",
                          in_flight->load());
                continue;
            }

            // One NV12 model input is one logical HAL tensor even when the
            // camera supplies Y and UV in separate dma-bufs. Build that tensor
            // through bind_dma_frame(); never submit the two physical planes as
            // two model inputs. Mismatched frames first try a private DSP slot.
            HalTensor inputs[HAL_MAX_TENSORS] = {};
            int num_inputs = 0;
            std::shared_ptr<void> input_owner;
            std::shared_ptr<StreamPreprocessPool::SlotLease> pp_lease;
            std::shared_ptr<StreamDmaInputLease> direct_lease;
            bool early_released = false;
            uint64_t dsp_us = 0;
            const uint64_t repack_us = 0;

            const bool nv12_model = model_in_w > 0 && model_in_h > 0;
            const bool nv12_frame = frame.format == HAL_PIX_FMT_NV12;
            const bool geometry_mismatch = nv12_model && nv12_frame &&
                (frame.width != model_in_w || frame.height != model_in_h);

            if (geometry_mismatch && cfg_.stream_dsp_preprocess && dsp_client_) {
                const auto pool_now = SteadyClock::now();
                if (dsp_pool && !dsp_pool->usable()) {
                    dsp_pool.reset();
                    next_dsp_pool_retry = pool_now + kDspPoolRetryDelay;
                }
                if (!dsp_pool && pool_now >= next_dsp_pool_retry) {
                    dsp_pool = acquire_stream_preprocess_pool(model_in_w,
                                                              model_in_h);
                    next_dsp_pool_retry = pool_now + kDspPoolRetryDelay;
                    if (dsp_pool) {
                        LOG_INFO("StreamInfer: shared dsp preprocess armed "
                                 "(model %ux%u NV12, %u slot(s), timeout %ums)",
                                 (unsigned)model_in_w, (unsigned)model_in_h,
                                 (unsigned)cfg_.stream_preprocess_slots,
                                 (unsigned)cfg_.stream_preprocess_job_ms);
                    } else {
                        LOG_WARN("StreamInfer: dsp preprocess pool unavailable; "
                                 "validating mismatched frame through direct DMA");
                    }
                }
                if (dsp_pool) {
                    pp_lease = dsp_pool->prepare(frame, &dsp_us);
                    if (!pp_lease && !dsp_pool->usable()) {
                        dsp_pool.reset();
                        next_dsp_pool_retry =
                            SteadyClock::now() + kDspPoolRetryDelay;
                    }
                }
                if (pp_lease &&
                    dsp_pool->bind(snap->infer_session, pp_lease.get(),
                                   &inputs[0])) {
                    num_inputs = 1;
                    input_owner = pp_lease;
                    // The resized pixels now live in the shared DSP slot.
                    frame.delivery.acknowledge();
                    early_released = true;
                } else {
                    pp_lease.reset();
                }
            }

            if (num_inputs == 0 && nv12_model && nv12_frame) {
                int bind_status = HAL_ERR_INVALID_ARG;
                direct_lease = bind_stream_nv12_input(
                    snap->infer_session, model_mgr_->infer_ops(), frame,
                    &bind_status);

                // On a mismatch, bind_dma_frame is the direct-DMA geometry
                // validation fallback. It must reject the source geometry; do
                // not run a mismatched frame even if an older backend accepts
                // equal-byte-size dimensions.
                if (!direct_lease || geometry_mismatch) {
                    if (direct_lease) {
                        direct_lease.reset();
                        bind_status = HAL_ERR_INVALID_SIZE;
                    }
                    frame.delivery.acknowledge();
                    resp.set_frame_sequence(frame.sequence);
                    resp.set_timestamp_ns(frame.timestamp_ns);
                    resp.mutable_status()->set_success(false);
                    resp.mutable_status()->set_message(
                        "DMA input rejected: " + std::to_string(bind_status));
                    resp.mutable_perf()->set_repack_us(repack_us);
                    resp.mutable_perf()->set_dsp_us(dsp_us);
                    if (!writer->Write(resp)) break;
                    continue;
                }

                inputs[0] = direct_lease->tensor();
                num_inputs = 1;
                input_owner = direct_lease;
            } else if (num_inputs == 0) {
                // Preserve the pre-existing non-NV12/unknown-model fallback.
                num_inputs = build_nv12_tensors(frame, inputs);
                input_owner = frame.fd_group;
            }

            resp.set_frame_sequence(frame.sequence);
            resp.set_timestamp_ns(frame.timestamp_ns);

            // on_complete does ALL work (fill response + post_process +
            // free + release) so it is self-sufficient on timeout.
            // The gRPC thread only waits for the response promise.
            auto stream_resp = std::make_shared<pb::StreamInferResponse>();
            stream_resp->set_frame_sequence(frame.sequence);
            stream_resp->set_timestamp_ns(frame.timestamp_ns);
            auto promise = std::make_shared<std::promise<bool>>();
            auto future = promise->get_future();
            // only_nonempty decision crosses from on_complete to the writer
            // (the promise only says "response built", not "send it").
            auto resp_suppressed = std::make_shared<std::atomic<bool>>(false);

            auto inf_req = std::make_unique<InferRequest>();
            inf_req->model_id   = model_id;
            inf_req->session_id = session_id;
            inf_req->num_inputs = num_inputs;
            std::memcpy(inf_req->inputs, inputs, sizeof(HalTensor) * num_inputs);
            inf_req->priority   = 5;
            inf_req->timeout_ms = 1000;
            // Keep either the bound direct-DMA tensor (and its borrowed source
            // FDs) or the DSP slot lease alive until the NPU read completes.
            inf_req->resource_holder = std::move(input_owner);
            inf_req->owns_outputs = true;

            auto frame_id = frame.frame_id;
            auto frame_delivery = frame.delivery;
            auto frame_seq = frame.sequence;
            auto ts_ns = frame.timestamp_ns;

            inf_req->on_complete = [this, promise, stream_resp, pp_session,
                                    enable_post, model_id, stream_id,
                                    frame_delivery, frame_seq, ts_ns,
                                    work_permit, session,
                                    repack_us, dsp_us, early_released,
                                    flt_max_results, flt_min_confidence,
                                    flt_class_filter, flt_omit_labels,
                                    flt_only_nonempty, resp_suppressed](
                int rc, HalTensor* outputs, int num_outputs,
                uint64_t infer_us, uint64_t queue_us,
                bool model_acquired) {

                struct CallbackCleanup {
                    ModelManager* model_mgr;
                    FrameDelivery frame_delivery;
                    HalTensor* outputs;
                    int num_outputs;
                    std::string model_id;
                    bool model_acquired;
                    bool release_frame;
                    std::shared_ptr<StreamAdmissionController::WorkPermit>
                        work_permit;
                    std::shared_ptr<std::promise<bool>> promise;
                    bool promise_value = false;

                    ~CallbackCleanup() noexcept {
                        if (release_frame)
                            frame_delivery.acknowledge();
                        try {
                            if (outputs)
                                model_mgr->free_outputs(outputs, num_outputs);
                        } catch (...) {
                            LOG_ERROR("StreamInfer: output cleanup threw");
                        }
                        try {
                            if (model_acquired)
                                model_mgr->release_model(model_id);
                        } catch (...) {
                            LOG_ERROR("StreamInfer: model release threw");
                        }
                        work_permit.reset();
                        try {
                            promise->set_value(promise_value);
                        } catch (...) {
                            LOG_ERROR("StreamInfer: completion promise failed");
                        }
                    }
                } cleanup{model_mgr_, frame_delivery, outputs, num_outputs,
                          model_id, model_acquired, !early_released,
                          work_permit, promise};

                try {
                // Release frame back to camera-daemon — unless the DSP
                // preprocess arm already returned it right after the resize
                // (exactly-once).
                if (cleanup.release_frame) {
                    cleanup.frame_delivery.acknowledge();
                    cleanup.release_frame = false;
                }

                // Record stats (single source of truth)
                session_mgr_->record_inference(session.get(), infer_us);

                // Perf segments known at completion (also on failure —
                // partial data beats none when diagnosing a dropped frame).
                // dsp_us is the stream DSP resize wall time (0 on the direct
                // DMA arm); hw_infer_us stays 0 (HAL does not expose NPU-only
                // latency; NV12 models skip the measurement flag);
                // post_us set at the post branch, write_us lags one
                // response (set by the writer below).
                auto* perf = stream_resp->mutable_perf();
                perf->set_repack_us(repack_us);
                perf->set_dsp_us(dsp_us);
                perf->set_queue_us(queue_us);
                perf->set_infer_us(infer_us);

                if (rc != 0) {
                    stream_resp->mutable_status()->set_success(false);
                    stream_resp->mutable_status()->set_message(
                        "Inference failed: " + std::to_string(rc));
                    return;
                }

                bool pp_failed = false;
                // Fill raw outputs if post-process disabled
                if (!enable_post) {
                    for (int i = 0; i < num_outputs; i++) {
                        auto* pt = stream_resp->add_outputs();
                        pt->set_dtype(hal_dtype_to_proto(outputs[i].dtype));
                        for (int d = 0; d < outputs[i].ndim; d++)
                            pt->add_shape(outputs[i].shape[d]);
                        if (outputs[i].data && outputs[i].byte_size > 0)
                            pt->set_data(outputs[i].data,
                                        outputs[i].byte_size);
                    }
                } else if (pp_session) {
                    // Post-processing inline in on_complete (single owner).
                    // Wrapped in try/catch: a throwing backend must NOT
                    // terminate the process via the sync on_complete thread
                    // — mirrors the InferBatch post_task guard at line ~735.
                    HalPostprocessResult post_result{};
                    struct PostResultGuard {
                        ModelManager* model_mgr;
                        HalPostprocessResult* result;
                        ~PostResultGuard() noexcept {
                            try {
                                model_mgr->free_post_result(result);
                            } catch (...) {
                                LOG_ERROR("StreamInfer: post-result cleanup threw");
                            }
                        }
                    } post_guard{model_mgr_, &post_result};
                    const uint64_t post_t0 = now_us();
                    try {
                        if (model_mgr_->post_process(pp_session, outputs,
                                    num_outputs, &post_result) == 0) {
                            fill_proto_post_result(stream_resp->mutable_post_result(),
                                                  post_result);
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("Postprocess failed for model '%s': %s",
                                  model_id.c_str(), e.what());
                        pp_failed = true;
                        stream_resp->mutable_status()->set_success(false);
                        stream_resp->mutable_status()->set_message(
                            std::string("Postprocess failed: ") + e.what());
                    }
                    perf->set_post_us(now_us() - post_t0);
                }

                if (!pp_failed) {
                    stream_resp->mutable_status()->set_success(true);
                }

                // On-demand shaping (P1-5): threshold / top-k / strip labels
                // on the filled post-result, then decide whole-response
                // suppression. Applied before the event-bus publish so the
                // bus sees the same shaped view as this subscriber.
                if (enable_post && stream_resp->has_post_result()) {
                    const bool any_result = apply_result_filters(
                        stream_resp->mutable_post_result(), flt_max_results,
                        flt_min_confidence, *flt_class_filter,
                        flt_omit_labels);
                    if (!any_result && flt_only_nonempty)
                        resp_suppressed->store(true);
                }

                // Publish to event-bus (a suppressed response publishes
                // nothing — the shaping is this subscription's semantics)
                if (cfg_.event_bus_auto_publish
                    && stream_resp->has_post_result()
                    && !resp_suppressed->load()) {
                    publish_result(stream_id, model_id,
                                   frame_seq, ts_ns,
                                   stream_resp->post_result());
                }

                // Skew (success only): result-ready (here — response fully
                // built, post-process done) minus frame capture. Both stamps
                // are CLOCK_MONOTONIC: the daemon's frame stamp comes from
                // the media pipeline and now_us() is steady_clock. Failures
                // keep skew_us = 0 ("not measured").
                if (!pp_failed) {
                    uint64_t ready_us = now_us();
                    uint64_t capture_us = ts_ns / 1000;
                    if (ready_us > capture_us) {
                        uint64_t skew_us = ready_us - capture_us;
                        stream_resp->set_skew_us(
                            static_cast<int64_t>(skew_us));
                        session_mgr_->record_skew(session.get(), skew_us);
                    }
                }

                cleanup.promise_value = true;
                } catch (const std::exception& e) {
                    LOG_ERROR("StreamInfer completion failed for model '%s': %s",
                              model_id.c_str(), e.what());
                    try {
                        stream_resp->mutable_status()->set_success(false);
                        stream_resp->mutable_status()->set_message(
                            "stream completion failed");
                    } catch (...) {
                        LOG_ERROR("StreamInfer: failed to record completion error");
                    }
                } catch (...) {
                    LOG_ERROR("StreamInfer completion failed for model '%s'",
                              model_id.c_str());
                    try {
                        stream_resp->mutable_status()->set_success(false);
                        stream_resp->mutable_status()->set_message(
                            "stream completion failed");
                    } catch (...) {
                        LOG_ERROR("StreamInfer: failed to record completion error");
                    }
                }
            };

            if (!scheduler_->submit(std::move(inf_req))) {
                // (early_released guard: the dsp arm already returned the
                // source; inf_req destruction releases the slot lease.)
                if (!early_released)
                    frame_delivery.acknowledge();
                resp.mutable_status()->set_success(false);
                resp.mutable_status()->set_message("Scheduler queue full");
                if (!writer->Write(resp)) break;
                continue;
            }

            // Wait with timeout. On timeout, on_complete still fires
            // later and does its own free/release — no leak, and its
            // admission permit is released by on_complete (not here).
            uint32_t stream_timeout_ms = 5000;
            if (future.wait_for(std::chrono::milliseconds(stream_timeout_ms))
                != std::future_status::ready) {
                LOG_WARN("StreamInfer: inference timeout, skipping frame");
                // Do not release admission here; on_complete still owns it.
                // Do NOT touch stream_resp — on_complete owns it.
                continue;
            }

            future.get();  // check for exceptions
            resp = std::move(*stream_resp);
            // only_nonempty: response intentionally not sent — the frame
            // still went through the full pipeline (perf recorded),
            // only the wire write is dropped.
            if (resp_suppressed->load()) {
                suppressed_responses++;
                continue;
            }
        } else {
            // Simulation mode (no frame source)
            std::this_thread::sleep_for(simulation_interval);
            resp.set_frame_sequence(++last_seq);
            resp.set_timestamp_ns(now_ns());
            resp.mutable_status()->set_success(true);
            resp.mutable_status()->set_message("simulation");
        }

        if (resp.has_perf()) {
            resp.mutable_perf()->set_write_us(last_write_us);
        }
        const uint64_t write_t0 = now_us();
        if (!writer->Write(resp)) {
            LOG_INFO("StreamInfer: client disconnected");
            break;
        }
        last_write_us = now_us() - write_t0;
    }

    subscription.close();

    // Bounded drain: wait for in-flight tasks to complete, but don't
    // block forever if a HAL job is stuck. Late callbacks self-clean
    // (free + release + in_flight--) so it's safe to return.
    {
        auto deadline = SteadyClock::now() + Milliseconds(5000);
        while (in_flight->load() > 0) {
            if (SteadyClock::now() >= deadline) {
                LOG_WARN("StreamInfer: drain timeout, %u orphan job(s) — "
                         "late callbacks will self-clean",
                         in_flight->load());
                break;
            }
            std::this_thread::sleep_for(Milliseconds(1));
        }
    }

    if (suppressed_responses > 0) {
        LOG_INFO("StreamInfer: %s suppressed %u empty response(s) "
                 "(only_nonempty)", stream_id.c_str(), suppressed_responses);
    }
    LOG_INFO("StreamInfer: ended for %s (subscriber=%s)",
             stream_id.c_str(), session_id.c_str());
    return grpc::Status::OK;
    } catch (const std::exception& e) {
        LOG_ERROR("StreamInfer exception: stream_id=%s model_id=%s what=%s",
                  req->stream_id().c_str(), req->model_id().c_str(), e.what());
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "stream inference failed");
    } catch (...) {
        LOG_ERROR("StreamInfer unknown exception: stream_id=%s model_id=%s",
                  req->stream_id().c_str(), req->model_id().c_str());
        return grpc::Status(grpc::StatusCode::INTERNAL,
                            "stream inference failed");
    }
}

// ─── CreateSession ────────────────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::CreateSession(
    grpc::ServerContext* /*ctx*/,
    const pb::SessionConfig* req,
    pb::SessionCreateResponse* resp) {

    auto sid = session_mgr_->create_session(
        req->app_id(), "", "",
        0, req->max_qps(), req->priority());
    if (sid.empty()) {
        resp->set_session_id("");
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message(
            "session creation failed (invalid app id or session limit reached)");
        return grpc::Status::OK;
    }

    resp->set_session_id(sid);
    resp->mutable_status()->set_success(true);
    return grpc::Status::OK;
}

// ─── DestroySession ───────────────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::DestroySession(
    grpc::ServerContext* /*ctx*/,
    const pb::SessionConfig* req,
    pb::Status* resp) {

    // P2-13: capture the client-facing tag before the session object is
    // destroyed — app_id at creation time (for StreamInfer sessions that
    // is the SDK's session_id) is what overlay sidecars were tagged with.
    // The broadcast goes out after the destroy so a downstream observer
    // reacting to session/end sees a fully torn-down runtime session.
    std::string client_tag;
    if (auto s = session_mgr_->get_session(req->session_id()))
        client_tag = s->app_id;

    bool ok = session_mgr_->destroy_session(req->session_id());
    publish_session_end(client_tag);
    resp->set_success(ok);
    resp->set_message(ok ? "Destroyed" : "Session not found");
    return grpc::Status::OK;
}

// ─── GetStats ─────────────────────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::GetStats(
    grpc::ServerContext* /*ctx*/,
    const pb::GetStatsRequest* req,
    pb::SystemStats* resp) {

    // Sampling window for the blocking HAL queries below (device/CPU/DSP
    // utilization is measured, not read). 0 = server default 500ms — the
    // pre-parameterization behavior, also what an Empty-sending legacy
    // client gets. Clamped to [1,5000].
    uint32_t window_ms = req ? req->sampling_window_ms() : 0;
    if (window_ms == 0) {
        window_ms = 500;
    } else if (window_ms > 5000) {
        window_ms = 5000;
    }

    auto models   = model_mgr_->list_models();
    auto sessions = session_mgr_->list_sessions();

    for (auto& m : models) {
        auto* stat = resp->add_model_stats();
        stat->set_model_id(m.id);
        stat->set_queue_depth(static_cast<uint32_t>(scheduler_->queue_depth()));

        // Aggregate from sessions
        uint64_t total_latency = 0;
        uint64_t total_inferences = 0;
        uint64_t total_skew = 0;
        uint64_t skew_samples = 0;
        uint64_t max_skew = 0;
        for (auto& s : sessions) {
            if (s->model_id == m.id) {
                uint64_t count = s->infer_count.load(std::memory_order_relaxed);
                total_inferences += count;
                total_latency += s->total_latency_us.load(std::memory_order_relaxed);

                // Stream-infer skew aggregation (see Session::total_skew_us)
                skew_samples += s->skew_count.load(std::memory_order_relaxed);
                total_skew += s->total_skew_us.load(std::memory_order_relaxed);
                uint64_t sk_max = s->max_skew_us.load(std::memory_order_relaxed);
                if (sk_max > max_skew) max_skew = sk_max;

                // QPS from sliding window
                auto now_ms = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count());
                auto window_start = s->window_start_ms.load(std::memory_order_relaxed);
                auto window_count = s->window_infer_count.load(std::memory_order_relaxed);
                if (window_start > 0 && now_ms > window_start) {
                    double window_elapsed_s = static_cast<double>(now_ms - window_start) / 1000.0;
                    if (window_elapsed_s > 0) {
                        float existing_qps = stat->current_qps();
                        float session_qps = static_cast<float>(
                            window_count / window_elapsed_s);
                        stat->set_current_qps(existing_qps + session_qps);
                    }
                }
            }
        }
        stat->set_total_inferences(total_inferences);

        // Skew aggregates — only set when samples exist so old-server
        // semantics ("all 0") are preserved for stream-less models.
        if (skew_samples > 0) {
            stat->set_avg_skew_us(total_skew / skew_samples);
            stat->set_max_skew_us(max_skew);
            stat->set_skew_samples(skew_samples);
        }

        // Query HAL for per-session hardware FPS
        HalInferenceSessionPerfStats hw_perf{};
        if (model_mgr_->query_session_stats(m.id, window_ms, &hw_perf) == 0) {
            if (hw_perf.fps > 0)
                stat->set_hw_fps(hw_perf.fps);
        }

        // Average latency
        if (total_inferences > 0 && total_latency > 0) {
            stat->set_avg_latency_us(
                static_cast<uint64_t>(total_latency / total_inferences));
        }
    }

    // Query HAL for performance stats
    HalInferencePerfStats perf{};
    if (model_mgr_->query_performance_stats(window_ms, &perf) == 0) {
        if (perf.npu_utilization >= 0) {
            resp->set_device_utilization(perf.npu_utilization / 100.0f);
        }
        if (perf.cpu_utilization >= 0) {
            resp->set_cpu_utilization(perf.cpu_utilization / 100.0f);
        }
        if (perf.dsp_utilization >= 0) {
            resp->set_dsp_utilization(perf.dsp_utilization / 100.0f);
        }
        if (perf.ram_total_kib >= 0) {
            resp->set_ram_total_kib(perf.ram_total_kib);
        }
        if (perf.ram_used_kib >= 0) {
            resp->set_ram_used_kib(perf.ram_used_kib);
        }
    }

    return grpc::Status::OK;
}

// ─── UpdatePostprocessConfig ─────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::UpdatePostprocessConfig(
    grpc::ServerContext* /*ctx*/,
    const pb::UpdatePostprocessConfigRequest* req,
    pb::UpdatePostprocessConfigResponse* resp) {

    LOG_INFO("UpdatePostprocessConfig: model_id=%s", req->model_id().c_str());

    if (req->model_id().empty() || req->config_json().empty()) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message("model_id and config_json required");
        return grpc::Status::OK;
    }

    int rc = model_mgr_->update_postprocess_config(req->model_id(), req->config_json());
    resp->mutable_status()->set_success(rc == 0);
    if (rc != 0) {
        resp->mutable_status()->set_message("Failed to update config: " + std::to_string(rc));
    }

    return grpc::Status::OK;
}

// ─── Event Bus publishing ─────────────────────────────────────────────────────

void AIRuntimeServiceImpl::publish_result(const std::string& stream_id,
                                          const std::string& model_id,
                                          uint64_t frame_seq,
                                          uint64_t timestamp_ns,
                                          const pb::PostResult& post_result) {
    if (!event_bus_ || !event_bus_->connected()) return;

    std::string topic = cfg_.event_bus_result_topic_prefix + model_id + "/" + stream_id;
    std::string payload = post_result_pb_to_json(stream_id, model_id, frame_seq, timestamp_ns, post_result);
    std::string event_id = "inf-" + std::to_string(frame_seq) + "-" + std::to_string(timestamp_ns);

    event_bus_->publish(topic, "ai-runtime", timestamp_ns, event_id, payload,
                        {{"stream_id", stream_id}, {"model_id", model_id}});
}

void AIRuntimeServiceImpl::publish_session_end(
    const std::string& client_session_id) {
    // Daemon contract (ai_overlay_subscriber.h:104): the sweep trigger is
    // the exact topic "<prefix>session/end" with the session riding in
    // metadata["session_id"]. The prefix is the configured result prefix —
    // the daemon's ai_overlay.topic_prefix must agree with it (both
    // default "inference/"), the same coupling result publishing already
    // has. Payload is unused by the daemon; "{}" keeps bus snoopers happy.
    // Reserved-name rule: no result may publish under a model+stream that
    // literally spells "session/end" — this is the sole sanctioned user.
    if (client_session_id.empty()) return;  // untagged: nothing to sweep
    if (!event_bus_ || !event_bus_->connected()) return;

    const std::string topic =
        cfg_.event_bus_result_topic_prefix + "session/end";
    event_bus_->publish(topic, "ai-runtime", now_ns(),
                        "session-end-" + client_session_id,
                        "{}", {{"session_id", client_session_id}});
}

// ─── CLIP text encoding ──────────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::EncodeText(
    grpc::ServerContext* ctx,
    const aipc::inference::EncodeTextRequest* req,
    aipc::inference::EncodeTextResponse* resp) {
    if (!clip_enc_ops_ || !clip_enc_ops_->create || !clip_enc_ops_->encode || !clip_enc_ops_->destroy) {
        resp->mutable_status()->set_code(3);
        resp->mutable_status()->set_message("CLIP text encoder not available");
        return grpc::Status::OK;
    }
    const std::string& text = req->text();
    if (text.empty()) {
        resp->mutable_status()->set_code(1);
        resp->mutable_status()->set_message("empty text");
        return grpc::Status::OK;
    }

    // Create a short-lived encoder instance per request.
    // The encoder caches model loading internally so repeated calls are fast.
    auto* enc = clip_enc_ops_->create();
    if (!enc) {
        resp->mutable_status()->set_code(3);
        resp->mutable_status()->set_message("CLIP text encoder init failed");
        return grpc::Status::OK;
    }

    uint32_t dim = 512;
    std::vector<float> emb_vec(dim);
    int rc = clip_enc_ops_->encode(enc, text.c_str(), emb_vec.data(), &dim);
    clip_enc_ops_->destroy(enc);

    if (rc != 0) {
        resp->mutable_status()->set_code(2);
        resp->mutable_status()->set_message("text encoding failed");
        return grpc::Status::OK;
    }
    emb_vec.resize(dim);

    auto* out = resp->mutable_embedding();
    for (float v : emb_vec)
        out->add_data(v);
    out->set_dim(dim);
    resp->mutable_status()->set_code(0);
    return grpc::Status::OK;
}

// ─── GenAI (LLM/VLM) ──────────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::GenaiCreateSession(
    grpc::ServerContext* /*ctx*/,
    const aipc::inference::GenaiCreateSessionRequest* req,
    aipc::inference::GenaiCreateSessionResponse* resp) {
    if (!genai_ops_ || !genai_ops_->create) {
        resp->mutable_status()->set_code(3);
        resp->mutable_status()->set_message("GenAI not available");
        return grpc::Status::OK;
    }

    // Regular inference and GenAI cannot share the NPU. Refuse atomically when
    // any model is still in use; the registry and existing GenAI sessions remain
    // untouched so active work is never invalidated to satisfy this request.
    if (!model_mgr_->force_unregister_all()) {
        resp->mutable_status()->set_code(9);
        resp->mutable_status()->set_message(
            "Regular inference is still active; retry GenAI session creation");
        return grpc::Status::OK;
    }

    // Destroy stale GenAI sessions only after regular-model unload succeeded.
    // This handles an app restart that lost its previous session id.
    {
        std::lock_guard<std::mutex> lock(genai_mu_);
        for (auto& [sid, session] : genai_sessions_) {
            if (genai_ops_ && genai_ops_->destroy && session)
                genai_ops_->destroy(session);
        }
        genai_sessions_.clear();
    }

    // force_unregister_all drains every pending InferBatch callback (via HAL
    // destroy's wait_pending_async) before returning, so the implicit
    // "implicit-{model_id}" sessions can no longer be touched by a late
    // callback. Drop them now so a VLM load doesn't orphan prior sessions.
    session_mgr_->destroy_sessions_by_app("implicit");
    // Allow NPU time to fully release VDevice and KV-Cache resources
    // after destroying HAL inference sessions. GenAI VLM (e.g. Qwen3-VL)
    // requires exclusive KV-Cache access; HailoRT VDevice cleanup is async.
    std::this_thread::sleep_for(std::chrono::seconds(6));

    HalGenaiCreateParams params{};
    strncpy(params.hef_path, req->hef_path().c_str(), sizeof(params.hef_path) - 1);
    params.kind = req->kind() == aipc::inference::GENAI_KIND_VLM
                     ? HAL_GENAI_KIND_VLM
                     : HAL_GENAI_KIND_LLM;
    if (!req->lora_name().empty()) {
        params.lora_name = req->lora_name().c_str();
    }
    params.optimize_memory_on_device = req->optimize_memory();

    auto* session = genai_ops_->create(&params);
    if (!session) {
        resp->mutable_status()->set_code(3);
        resp->mutable_status()->set_message("GenAI session creation failed");
        return grpc::Status::OK;
    }

    // Generate a session ID
    std::string session_id = "genai_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());

    {
        std::lock_guard<std::mutex> lock(genai_mu_);
        genai_sessions_[session_id] = session;
    }

    resp->set_session_id(session_id);
    resp->mutable_status()->set_code(0);
    return grpc::Status::OK;
}

grpc::Status AIRuntimeServiceImpl::GenaiDestroySession(
    grpc::ServerContext* /*ctx*/,
    const aipc::inference::GenaiCreateSessionRequest* req,
    aipc::inference::Status* resp) {
    // Reuse hef_path field as session_id for destroy
    const std::string& sid = req->hef_path();
    HalGenaiSession* session = nullptr;
    {
        std::lock_guard<std::mutex> lock(genai_mu_);
        auto it = genai_sessions_.find(sid);
        if (it != genai_sessions_.end()) {
            session = it->second;
            genai_sessions_.erase(it);
        }
    }
    if (session && genai_ops_ && genai_ops_->destroy) {
        genai_ops_->destroy(session);
    }
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status AIRuntimeServiceImpl::GenaiGenerate(
    grpc::ServerContext* ctx,
    const aipc::inference::GenaiGenerateRequest* req,
    grpc::ServerWriter<aipc::inference::GenaiGenerateResponse>* writer) {
    if (!genai_ops_ || !genai_ops_->generate_stream) {
        return grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "GenAI not available");
    }

    HalGenaiSession* session = nullptr;
    {
        std::lock_guard<std::mutex> lock(genai_mu_);
        auto it = genai_sessions_.find(req->session_id());
        if (it == genai_sessions_.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, "session not found");
        }
        session = it->second;
    }

    // Set generator params if provided
    if (req->has_params()) {
        HalGenaiGeneratorParams gparams{};
        gparams.temperature = req->params().temperature();
        gparams.top_p = req->params().top_p();
        gparams.top_k = req->params().top_k();
        gparams.frequency_penalty = req->params().frequency_penalty();
        gparams.max_generated_tokens = req->params().max_generated_tokens();
        gparams.do_sample = req->params().do_sample();
        if (genai_ops_->set_generator_params) {
            genai_ops_->set_generator_params(session, &gparams);
        }
    }

    // Set stop tokens if provided
    if (req->stop_tokens_size() > 0 && genai_ops_->set_stop_tokens) {
        std::vector<const char*> stops;
        for (const auto& s : req->stop_tokens()) {
            stops.push_back(s.c_str());
        }
        genai_ops_->set_stop_tokens(session, stops.data(), stops.size());
    }

    // Prepare messages as C-string array
    std::vector<std::string> msgs(req->messages_json().begin(), req->messages_json().end());
    std::vector<const char*> msg_ptrs;
    for (const auto& m : msgs) {
        msg_ptrs.push_back(m.c_str());
    }

    LOG_INFO("GenAI generate: session=%s, msgs=%d, images=%d, has_params=%d",
             req->session_id().c_str(), (int)msg_ptrs.size(),
             (int)req->image_frames().size(), req->has_params());

    // Prepare image frames for VLM
    std::vector<HalGenaiImageFrame> img_frames;
    for (const auto& img : req->image_frames()) {
        HalGenaiImageFrame f{};
        f.data = reinterpret_cast<const uint8_t*>(img.data());
        f.byte_size = img.size();
        img_frames.push_back(f);
    }

    // Streaming generation with callbacks
    auto* writer_ptr = writer;
    const HalGenaiGeneratorParams* gen_params_ptr = nullptr;

    // Build generator params if provided
    HalGenaiGeneratorParams gen_params{};
    if (req->has_params()) {
        gen_params.temperature = req->params().temperature();
        gen_params.top_p = req->params().top_p();
        gen_params.top_k = req->params().top_k();
        gen_params.frequency_penalty = req->params().frequency_penalty();
        gen_params.max_generated_tokens = req->params().max_generated_tokens();
        gen_params.do_sample = req->params().do_sample();
        gen_params_ptr = &gen_params;
    }

    LOG_INFO("GenAI generate_stream starting...");

    // Watchdog: if the client disconnects mid-generation, abort NPU work.
    // abort_generation is "safe from another thread" (hal_genai.h) and MUST NOT
    // be called from the token/finish callbacks (they run on this generate_stream
    // thread). A short-lived watchdog polls ctx->IsCancelled() and aborts from a
    // separate thread, so a disconnected client no longer burns NPU cycles to the
    // end of generation. Joined before return so ctx/session stay valid.
    std::atomic<bool> gen_done{false};
    std::thread watchdog;
    if (genai_ops_->abort_generation) {
        auto* abort_ops = genai_ops_;
        watchdog = std::thread([ctx, session, abort_ops, &gen_done]() {
            while (!gen_done.load(std::memory_order_acquire)) {
                if (ctx->IsCancelled()) {
                    abort_ops->abort_generation(session);
                    LOG_INFO("GenAI generate aborted: client disconnected");
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
    }

    int rc = genai_ops_->generate_stream(
        session,
        msg_ptrs.data(), static_cast<int>(msg_ptrs.size()),
        img_frames.empty() ? nullptr : img_frames.data(), static_cast<int>(img_frames.size()),
        gen_params_ptr,
        // on_token callback
        [](const char* token, void* user) {
            auto* w = static_cast<grpc::ServerWriter<aipc::inference::GenaiGenerateResponse>*>(user);
            aipc::inference::GenaiGenerateResponse resp;
            resp.set_token(token);
            w->Write(resp);
        }, writer_ptr,
        // on_finish callback
        [](HalGenaiFinishReason reason, int /*error_code*/, void* user) {
            auto* w = static_cast<grpc::ServerWriter<aipc::inference::GenaiGenerateResponse>*>(user);
            aipc::inference::GenaiGenerateResponse resp;
            resp.set_finish(static_cast<aipc::inference::GenaiFinishReason>(reason));
            w->Write(resp);
        }, writer_ptr);

    gen_done.store(true, std::memory_order_release);
    if (watchdog.joinable()) watchdog.join();

    LOG_INFO("GenAI generate_stream done, rc=%d", rc);
    if (rc != 0) {
        aipc::inference::GenaiGenerateResponse resp;
        resp.set_finish(aipc::inference::GENAI_FINISH_ERROR);
        writer->Write(resp);
    }

    return grpc::Status::OK;
}

grpc::Status AIRuntimeServiceImpl::GenaiAbort(
    grpc::ServerContext* /*ctx*/,
    const aipc::inference::GenaiAbortRequest* req,
    aipc::inference::Status* resp) {
    if (!genai_ops_ || !genai_ops_->abort_generation) {
        resp->set_success(false);
        resp->set_message("GenAI abort not available");
        return grpc::Status::OK;
    }

    HalGenaiSession* session = nullptr;
    {
        std::lock_guard<std::mutex> lock(genai_mu_);
        auto it = genai_sessions_.find(req->session_id());
        if (it != genai_sessions_.end()) {
            session = it->second;
        }
    }

    if (session) {
        genai_ops_->abort_generation(session);
        resp->set_success(true);
    } else {
        resp->set_success(false);
        resp->set_message("session not found");
    }
    return grpc::Status::OK;
}

}  // namespace aipc::ai_runtime
