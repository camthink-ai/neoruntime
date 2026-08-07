#include "grpc_service.h"
#include "log.h"
#include "common.h"

#include "hal_inference.h"
#include "hal_postprocess.h"

#include <cstring>
#include <vector>
#include <sstream>
#include <atomic>
#include <thread>
#include <condition_variable>
#include <future>
#include <unistd.h>

namespace aipc::ai_runtime {

using namespace aipc::inference;
namespace pb = aipc::inference;

AIRuntimeServiceImpl::AIRuntimeServiceImpl(
    const Config& cfg,
    ModelManager* model_mgr,
    SessionManager* session_mgr,
    InferenceScheduler* scheduler,
    FdReceiver* fd_receiver,
    EventBusClient* event_bus,
    PostprocessPool* postprocess_pool,
    const HalClipTextEncoderOps* clip_enc_ops,
    const HalGenaiOps* genai_ops)
    : cfg_(cfg)
    , model_mgr_(model_mgr)
    , session_mgr_(session_mgr)
    , scheduler_(scheduler)
    , fd_receiver_(fd_receiver)
    , event_bus_(event_bus)
    , postprocess_pool_(postprocess_pool)
    , clip_enc_ops_(clip_enc_ops)
    , genai_ops_(genai_ops) {}

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

    LOG_INFO("RegisterModel: model_id=%s path=%s type=%s",
             req->model_id().c_str(), req->model_path().c_str(), req->model_type().c_str());

    if (req->model_id().empty() || req->model_path().empty()) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message("model_id and model_path required");
        return grpc::Status::OK;
    }

    // Extract owner_id, default to "<system>" if not provided
    std::string owner_id = req->owner_id();
    if (owner_id.empty()) {
        owner_id = "<system>";
    }

    int rc = model_mgr_->register_model(req->model_id(), req->model_path(), owner_id);
    if (rc < 0) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message("Failed to register model");
        return grpc::Status::OK;
    }

    // Initialize post-processing if model_type is provided
    if (!req->model_type().empty() && model_mgr_->has_post_ops()) {
        int post_rc = model_mgr_->init_post_process(
            req->model_id(), req->model_type(), req->model_variant());
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
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "Unregistered" : "Failed to unregister");
    return grpc::Status::OK;
}

// ─── ListModels ───────────────────────────────────────────────────────────────

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
        // Include first owner_id if available
        auto owners = model_mgr_->get_owners(m.id);
        if (!owners.empty()) {
            info->set_owner_id(owners[0]);
        }
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

    for (uint32_t i = 0; i < m.model_info.num_inputs; i++) {
        auto* spec = resp->add_inputs();
        spec->set_name(m.model_info.inputs[i].name);
        spec->set_dtype(hal_dtype_to_proto(m.model_info.inputs[i].dtype));
        spec->set_layout(hal_layout_to_string(m.model_info.inputs[i].layout));
        for (int d = 0; d < m.model_info.inputs[i].ndim; d++) {
            spec->add_shape(m.model_info.inputs[i].shape[d]);
        }
    }

    for (uint32_t i = 0; i < m.model_info.num_outputs; i++) {
        auto* spec = resp->add_outputs();
        spec->set_name(m.model_info.outputs[i].name);
        spec->set_dtype(hal_dtype_to_proto(m.model_info.outputs[i].dtype));
        spec->set_layout(hal_layout_to_string(m.model_info.outputs[i].layout));
        for (int d = 0; d < m.model_info.outputs[i].ndim; d++) {
            spec->add_shape(m.model_info.outputs[i].shape[d]);
        }
    }

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

// ─── Infer (synchronous single-shot) ─────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::Infer(
    grpc::ServerContext* /*ctx*/,
    const pb::InferRequest* req,
    pb::InferResponse* resp) {

    LOG_DEBUG("Infer: model_id=%s", req->model_id().c_str());

    // Ensure an implicit session exists for stats tracking on the Infer() path.
    std::string implicit_session_id = "implicit-" + req->model_id();
    session_mgr_->create_named_session(
        implicit_session_id, "implicit", "infer", req->model_id(),
        0 /*fps_limit*/, 0 /*max_qps*/, 5 /*priority*/);
    auto infer_session = session_mgr_->get_session(implicit_session_id);

    try {
    // Acquire model snapshot for post_session/num_outputs.
    // ModelGuard releases this ref when Infer() returns. The scheduler's
    // own acquire (inside worker_loop) is released by on_complete.
    auto snap = model_mgr_->acquire_model_snapshot(req->model_id());
    if (!snap) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message("Model not found");
        return grpc::Status::OK;
    }
    ModelGuard model_guard{model_mgr_, req->model_id()};

    int num_inputs = req->inputs_size();

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
    auto input_tensors = std::make_shared<std::vector<HalTensor>>(num_inputs);
    for (int i = 0; i < num_inputs; i++) {
        auto& pb_t = req->inputs(i);
        auto& ht   = (*input_tensors)[i];
        std::memset(&ht, 0, sizeof(HalTensor));

        if (pb_t.dma_fd() > 0) {
            ht.dma_fd = pb_t.dma_fd();
            ht.data   = nullptr;
        } else {
            // Assign to pre-allocated slot — no reallocation
            (*input_data_holder)[i] = pb_t.data();
            ht.data      = const_cast<char*>((*input_data_holder)[i].data());
            ht.byte_size = static_cast<uint32_t>((*input_data_holder)[i].size());
            ht.dma_fd    = -1;
        }

        ht.dtype = proto_dtype_to_hal(pb_t.dtype());
        ht.ndim  = static_cast<int32_t>(pb_t.shape_size());
        for (int d = 0; d < ht.ndim && d < HAL_MAX_TENSOR_DIMS; d++) {
            ht.shape[d] = pb_t.shape(d);
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
                            input_tensors, session_id](
        int rc, HalTensor* outputs, int num_outputs,
        uint64_t infer_us, uint64_t queue_us,
        bool model_acquired) {

        // Record stats (single source of truth)
        if (infer_session) {
            session_mgr_->record_inference(infer_session.get(), infer_us);
        }

        if (rc != 0) {
            response_ptr->mutable_status()->set_success(false);
            response_ptr->mutable_status()->set_message(
                "Inference failed: " + std::to_string(rc));
            if (outputs) model_mgr_->free_outputs(outputs, num_outputs);
            if (model_acquired) model_mgr_->release_model(model_id);
            promise->set_value(false);
            return;
        }

        // Fill raw outputs into the shared response
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

        // Post-processing. Wrapped in try/catch: a throwing backend (e.g.
        // an invalid config_json / HEF tensor-name mismatch) must NOT
        // terminate the process via the single-Infer on_complete thread —
        // mirrors the InferBatch post_task guard at line ~735.
        bool pp_failed = false;
        if (enable_post && post_session) {
            HalPostprocessResult post_result{};
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
            model_mgr_->free_post_result(&post_result);
        }

        response_ptr->set_infer_time_us(infer_us);
        response_ptr->set_queue_time_us(queue_us);
        if (!pp_failed) {
            response_ptr->mutable_status()->set_success(true);
        }

        // Free outputs + release scheduler's model ref
        model_mgr_->free_outputs(outputs, num_outputs);
        model_mgr_->release_model(model_id);

        promise->set_value(true);
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
    std::atomic<bool>              released{false};  // model ref released?
    bool                           success = false;
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

// Bundle threaded through the HAL callback via userdata. The callback owns it
// (new/delete) and releases the shared_ptrs when it returns. Model ref_count
// is released by the callback/post_task itself (self-sufficient), NOT by the
// InferBatch RPC thread — this avoids UAF if the RPC times out and the model
// is unregistered while a late post_task is still pending.
struct InferBatchCbState {
    ModelManager*                         mgr = nullptr;
    SessionManager*                       smgr = nullptr;
    PostprocessPool*                      pool = nullptr;
    std::shared_ptr<InferBatchItemCtx>    ctx;
    std::shared_ptr<InferBatchSyncState>  sync;
};

}  // namespace

void AIRuntimeServiceImpl::InferBatchCallback(HalTensor* /*outputs*/, int /*num_outputs*/,
                                              int status, void* userdata) {
    auto* st = static_cast<InferBatchCbState*>(userdata);
    if (!st) return;
    auto& c = st->ctx;

    if (status != 0) {
        // Failure path: no post-processing needed, handle synchronously.
        st->mgr->free_outputs(c->outputs.data(), c->max_outputs);
        // Release model ref (self-sufficient, same as success post_task path)
        if (!c->released.exchange(true)) {
            st->mgr->release_model(c->model_id);
        }
        c->response.mutable_status()->set_success(false);
        c->response.mutable_status()->set_message(
            "Inference failed: " + std::to_string(status));
        c->done.store(true);
        c->success = false;
        {
            std::lock_guard<std::mutex> lk(st->sync->mtx);
            if (--st->sync->remaining <= 0)
                st->sync->cv.notify_all();
        }
        delete st;
        return;
    }

    // Success: offload output serialization + post-processing to the
    // PostprocessPool so the HailoRT completion thread can immediately
    // service the next async inference callback.
    auto elapsed = std::chrono::duration_cast<Microseconds>(
        SteadyClock::now() - c->start);

    // Capture shared_ptrs so ctx/sync outlive st (deleted below).
    auto ctx  = c;
    auto sync = st->sync;
    auto* mgr  = st->mgr;
    auto* smgr = st->smgr;
    int  max_out = c->max_outputs;

    PostprocessPool::Task post_task = [ctx, sync, mgr, smgr, max_out, elapsed]() {
        // Snapshot raw outputs to proto.
        for (int k = 0; k < max_out; k++) {
            auto* pt = ctx->response.add_outputs();
            pt->set_dtype(hal_dtype_to_proto(ctx->outputs[k].dtype));
            for (int d = 0; d < ctx->outputs[k].ndim; d++)
                pt->add_shape(ctx->outputs[k].shape[d]);
            if (ctx->outputs[k].data && ctx->outputs[k].byte_size > 0)
                pt->set_data(ctx->outputs[k].data, ctx->outputs[k].byte_size);
        }

        // Post-processing. The Hailo SDK postprocess library may throw
        // std::invalid_argument when the HEF's nms output tensor name does
        // not match the postprocess config (e.g. a misconfigured/renamed
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
    };

    if (!st->pool || !st->pool->submit(post_task)) {
        // Queue full or no pool — run synchronously (still off the NPU thread)
        // post_task is NOT moved-from because submit takes Task&
        post_task();
    }

    delete st;  // releases this callback's ctx/sync shared_ptrs
    // (ctx/sync kept alive by the lambda's captures)
}

// ─── InferBatch (parallel multi-model inference) ──────────────────────────────

grpc::Status AIRuntimeServiceImpl::InferBatch(
    grpc::ServerContext* /*ctx*/,
    const pb::InferBatchRequest* req,
    pb::InferBatchResponse* resp) {

    int num_requests = req->requests_size();
    if (num_requests == 0) {
        resp->mutable_status()->set_success(false);
        resp->mutable_status()->set_message("Empty batch");
        return grpc::Status::OK;
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

        // Idempotent implicit session for stats tracking (one per model).
        std::string implicit_session_id = "implicit-" + infer_req.model_id();
        session_mgr_->create_named_session(
            implicit_session_id, "implicit", "infer", infer_req.model_id(),
            0, 0, 5);
        c->infer_session = session_mgr_->get_session(implicit_session_id);

        c->snap = model_mgr_->acquire_model_snapshot(infer_req.model_id());
        if (!c->snap) {
            c->response.mutable_status()->set_success(false);
            c->response.mutable_status()->set_message("Model not found");
            c->done.store(true);
            complete_now();
            continue;
        }
        c->acquired = true;  // ref_count bumped; released after the callback fires

        // Convert proto tensors → HalTensor. CPU payloads are copied into
        // ctx-owned storage so the buffers outlive the RPC for late callbacks.
        const int num_inputs = infer_req.inputs_size();
        c->input_data.resize(num_inputs);
        c->inputs.resize(num_inputs);
        for (int j = 0; j < num_inputs; j++) {
            const auto& pb_t = infer_req.inputs(j);
            HalTensor&  ht   = c->inputs[j];
            std::memset(&ht, 0, sizeof(HalTensor));
            if (pb_t.dma_fd() > 0) {
                ht.dma_fd = pb_t.dma_fd();
                ht.data   = nullptr;
            } else {
                c->input_data[j].assign(pb_t.data().data(), pb_t.data().size());
                ht.data      = const_cast<char*>(c->input_data[j].data());
                ht.byte_size = static_cast<uint32_t>(c->input_data[j].size());
                ht.dma_fd    = -1;
            }
            ht.dtype = proto_dtype_to_hal(pb_t.dtype());
            ht.ndim  = static_cast<int32_t>(pb_t.shape_size());
            for (int d = 0; d < ht.ndim && d < HAL_MAX_TENSOR_DIMS; d++)
                ht.shape[d] = pb_t.shape(d);
        }

        c->max_outputs = c->snap->num_outputs;
        c->outputs.assign(c->max_outputs, HalTensor{});

        // Bundle per-item state for the HAL callback (threaded via userdata).
        // The callback owns it and deletes it once it has fired.
        auto* cb_state = new InferBatchCbState{model_mgr_, session_mgr_,
                                              postprocess_pool_, c, sync};

        int rc = model_mgr_->run_async(c->snap->infer_session,
                                       c->inputs.data(), num_inputs,
                                       c->outputs.data(), c->max_outputs,
                                       &AIRuntimeServiceImpl::InferBatchCallback,
                                       cb_state);
        if (rc != 0) {
            // Submission failed: no callback will fire. Clean up + signal.
            delete cb_state;
            model_mgr_->free_outputs(c->outputs.data(), c->max_outputs);
            // Release model ref (no callback will release it)
            if (!c->released.exchange(true)) {
                model_mgr_->release_model(c->model_id);
            }
            c->response.mutable_status()->set_success(false);
            c->response.mutable_status()->set_message(
                "Inference submission failed: " + std::to_string(rc));
            c->done.store(true);
            complete_now();
        }
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

    LOG_INFO("StreamInfer: stream_id=%s model_id=%s fps_limit=%u",
             req->stream_id().c_str(), req->model_id().c_str(), req->fps_limit());

    // Acquire model snapshot — safe to use after lock release
    auto snap = model_mgr_->acquire_model_snapshot(req->model_id());
    if (!snap) {
        return grpc::Status(grpc::StatusCode::NOT_FOUND, "Model not found");
    }

    ModelGuard model_guard{model_mgr_, req->model_id()};

    // Create session
    std::string session_id = session_mgr_->create_session(
        req->session_id(), req->stream_id(), req->model_id(),
        req->fps_limit(), 0, 5);

    struct SessionGuard {
        SessionManager* mgr; std::string id;
        ~SessionGuard() { mgr->destroy_session(id); }
    } guard{session_mgr_, session_id};

    auto session = session_mgr_->get_session(session_id);
    if (!session) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Session creation failed");
    }

    HalPostprocessSession* pp_session = snap->post_session;
    int max_outputs = snap->num_outputs;
    bool enable_post = model_mgr_->has_post_ops() && !req->raw_output_only();
    auto model_id = req->model_id();
    auto stream_id = req->stream_id();

    // Subscribe to FdReceiver for zero-copy DMA-BUF frames
    std::mutex frame_mu;
    std::condition_variable frame_cv;
    ReceivedFrame latest_frame{};
    bool has_frame = false;

    bool fd_path = fd_receiver_->subscribe(
        stream_id,
        session_id,  // unique per gRPC call — enables multicast
        [&](const ReceivedFrame& frame) {
            std::lock_guard lock(frame_mu);
            // Release previously buffered frame if unconsumed (backpressure)
            if (has_frame && latest_frame.frame_id != 0) {
                fd_receiver_->release_frame(stream_id, latest_frame.frame_id);
            }
            latest_frame = frame;
            has_frame = true;
            frame_cv.notify_one();
        });

    if (!fd_path) {
        LOG_WARN("StreamInfer: FdReceiver unavailable for %s, zero-copy disabled",
                 stream_id.c_str());
    }

    // FPS interval
    auto fps = req->fps_limit() > 0 ? req->fps_limit() : 30;
    auto frame_interval = Milliseconds(1000 / fps);
    uint64_t last_seq = 0;

    // Backpressure: limit outstanding inferences to prevent camera-daemon
    // buffer pool exhaustion (shared pool ~15 buffers, 3 per stream leaves headroom)
    constexpr int MAX_IN_FLIGHT = 3;
    auto in_flight = std::make_shared<std::atomic<int>>(0);

    while (!ctx->IsCancelled()) {
        if (!session_mgr_->check_fps_limit(session.get())) {
            std::this_thread::sleep_for(Milliseconds(1));
            continue;
        }

        pb::StreamInferResponse resp;

        if (fd_path) {
            ReceivedFrame frame{};
            {
                std::unique_lock lock(frame_mu);
                if (!frame_cv.wait_for(lock, frame_interval,
                                       [&] { return has_frame || ctx->IsCancelled(); })) {
                    continue;
                }
                if (ctx->IsCancelled()) break;
                frame = latest_frame;
                has_frame = false;
            }

            if (frame.sequence == last_seq) continue;
            last_seq = frame.sequence;

            // Backpressure: drop frame if too many in-flight
            if (in_flight->load() >= MAX_IN_FLIGHT) {
                fd_receiver_->release_frame(stream_id, frame.frame_id);
                LOG_DEBUG("StreamInfer: backpressure drop (in_flight=%d)",
                          in_flight->load());
                continue;
            }

            // Build HalTensor from DMA-BUF fd — zero copy
            HalTensor inputs[2] = {};
            int num_inputs = build_nv12_tensors(frame, inputs);

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

            auto inf_req = std::make_unique<InferRequest>();
            inf_req->model_id   = model_id;
            inf_req->session_id = session_id;
            inf_req->num_inputs = num_inputs;
            std::memcpy(inf_req->inputs, inputs, sizeof(HalTensor) * num_inputs);
            inf_req->priority   = 5;
            inf_req->timeout_ms = 1000;
            inf_req->resource_holder = frame.fd_group;
            inf_req->owns_outputs = true;

            auto frame_id = frame.frame_id;
            auto frame_seq = frame.sequence;
            auto ts_ns = frame.timestamp_ns;

            inf_req->on_complete = [this, promise, stream_resp, pp_session,
                                    enable_post, model_id, stream_id,
                                    frame_id, frame_seq, ts_ns, in_flight,
                                    fd_receiver = fd_receiver_, session](
                int rc, HalTensor* outputs, int num_outputs,
                uint64_t infer_us, uint64_t queue_us,
                bool model_acquired) {

                // Release frame back to camera-daemon
                fd_receiver->release_frame(stream_id, frame_id);

                // Record stats (single source of truth)
                session_mgr_->record_inference(session.get(), infer_us);

                if (rc != 0) {
                    stream_resp->mutable_status()->set_success(false);
                    stream_resp->mutable_status()->set_message(
                        "Inference failed: " + std::to_string(rc));
                    if (outputs) model_mgr_->free_outputs(outputs, num_outputs);
                    if (model_acquired) model_mgr_->release_model(model_id);
                    in_flight->fetch_sub(1);
                    promise->set_value(false);
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
                    model_mgr_->free_post_result(&post_result);
                }

                if (!pp_failed) {
                    stream_resp->mutable_status()->set_success(true);
                }

                // Publish to event-bus
                if (cfg_.event_bus_auto_publish
                    && stream_resp->has_post_result()) {
                    publish_result(stream_id, model_id,
                                   frame_seq, ts_ns,
                                   stream_resp->post_result());
                }

                // Free outputs + release scheduler's model ref
                model_mgr_->free_outputs(outputs, num_outputs);
                model_mgr_->release_model(model_id);
                in_flight->fetch_sub(1);

                promise->set_value(true);
            };

            in_flight->fetch_add(1);

            if (!scheduler_->submit(std::move(inf_req))) {
                fd_receiver_->release_frame(stream_id, frame_id);
                in_flight->fetch_sub(1);
                resp.mutable_status()->set_success(false);
                resp.mutable_status()->set_message("Scheduler queue full");
                if (!writer->Write(resp)) break;
                continue;
            }

            // Wait with timeout. On timeout, on_complete still fires
            // later and does its own free/release — no leak, and
            // in_flight is decremented by on_complete (not here).
            uint32_t stream_timeout_ms = 5000;
            if (future.wait_for(std::chrono::milliseconds(stream_timeout_ms))
                != std::future_status::ready) {
                LOG_WARN("StreamInfer: inference timeout, skipping frame");
                // Do NOT decrement in_flight here — on_complete will do it.
                // Do NOT touch stream_resp — on_complete owns it.
                continue;
            }

            future.get();  // check for exceptions
            resp = std::move(*stream_resp);
        } else {
            // Simulation mode (no frame source)
            std::this_thread::sleep_for(frame_interval);
            resp.set_frame_sequence(++last_seq);
            resp.set_timestamp_ns(now_ns());
            resp.mutable_status()->set_success(true);
            resp.mutable_status()->set_message("simulation");
        }

        if (!writer->Write(resp)) {
            LOG_INFO("StreamInfer: client disconnected");
            break;
        }
    }

    if (fd_path) {
        fd_receiver_->unsubscribe(stream_id, session_id);
    }

    // Bounded drain: wait for in-flight tasks to complete, but don't
    // block forever if a HAL job is stuck. Late callbacks self-clean
    // (free + release + in_flight--) so it's safe to return.
    {
        auto deadline = SteadyClock::now() + Milliseconds(5000);
        while (in_flight->load() > 0) {
            if (SteadyClock::now() >= deadline) {
                LOG_WARN("StreamInfer: drain timeout, %d orphan job(s) — "
                         "late callbacks will self-clean",
                         in_flight->load());
                break;
            }
            std::this_thread::sleep_for(Milliseconds(1));
        }
    }

    LOG_INFO("StreamInfer: ended for %s (subscriber=%s)",
             stream_id.c_str(), session_id.c_str());
    return grpc::Status::OK;
}

// ─── CreateSession ────────────────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::CreateSession(
    grpc::ServerContext* /*ctx*/,
    const pb::SessionConfig* req,
    pb::SessionCreateResponse* resp) {

    auto sid = session_mgr_->create_session(
        req->app_id(), "", "",
        0, req->max_qps(), req->priority());

    resp->set_session_id(sid);
    resp->mutable_status()->set_success(true);
    return grpc::Status::OK;
}

// ─── DestroySession ───────────────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::DestroySession(
    grpc::ServerContext* /*ctx*/,
    const pb::SessionConfig* req,
    pb::Status* resp) {

    bool ok = session_mgr_->destroy_session(req->session_id());
    resp->set_success(ok);
    resp->set_message(ok ? "Destroyed" : "Session not found");
    return grpc::Status::OK;
}

// ─── GetStats ─────────────────────────────────────────────────────────────────

grpc::Status AIRuntimeServiceImpl::GetStats(
    grpc::ServerContext* /*ctx*/,
    const pb::Empty* /*req*/,
    pb::SystemStats* resp) {

    auto models   = model_mgr_->list_models();
    auto sessions = session_mgr_->list_sessions();

    for (auto& m : models) {
        auto* stat = resp->add_model_stats();
        stat->set_model_id(m.id);
        stat->set_queue_depth(static_cast<uint32_t>(scheduler_->queue_depth()));

        // Aggregate from sessions
        uint64_t total_latency = 0;
        uint64_t total_inferences = 0;
        for (auto& s : sessions) {
            if (s->model_id == m.id) {
                uint64_t count = s->infer_count.load(std::memory_order_relaxed);
                total_inferences += count;
                total_latency += s->total_latency_us.load(std::memory_order_relaxed);

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

        // Query HAL for per-session hardware FPS
        HalInferenceSessionPerfStats hw_perf{};
        if (model_mgr_->query_session_stats(m.id, 500, &hw_perf) == 0) {
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
    if (model_mgr_->query_performance_stats(500, &perf) == 0) {
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

    // Destroy any existing GenAI sessions first to release KV-Cache.
    // This handles the case where the app container was restarted and lost
    // track of its previous session_id, but ai-runtime still holds the session.
    {
        std::lock_guard<std::mutex> lock(genai_mu_);
        for (auto& [sid, session] : genai_sessions_) {
            if (genai_ops_ && genai_ops_->destroy && session) {
                genai_ops_->destroy(session);
            }
        }
        genai_sessions_.clear();
    }

    // Force-release all loaded models to free NPU memory for the GenAI model.
    // GenAI models (especially VLMs like Qwen3-VL) are too large to coexist
    // with regular inference models on the NPU.
    model_mgr_->force_unregister_all();
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
