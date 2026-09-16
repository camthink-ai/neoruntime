#include "stream_infer_utils.h"
#include "common/hal_common.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aipc::ai_runtime {

namespace pb = aipc::inference;

bool valid_stream_fps(uint32_t fps) {
    return fps <= kMaxStreamFps;
}

bool valid_stream_min_confidence(float confidence) {
    return std::isfinite(confidence) && confidence >= 0.0f &&
           confidence <= 1.0f;
}

bool valid_stream_class_filter_size(int entries) {
    return entries >= 0 && entries <= kMaxStreamClassFilterEntries;
}

bool valid_stream_id(const std::string& stream_id) {
    if (stream_id.empty() || stream_id.size() >= FD_PUB_MAX_STREAM_NAME)
        return false;
    return std::all_of(stream_id.begin(), stream_id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
}

struct StreamAdmissionController::State {
    State(uint32_t active_rpcs, uint32_t rpcs_per_peer,
          uint32_t subscribers_per_stream, uint32_t in_flight_total,
          uint32_t in_flight_per_rpc)
        : max_active_rpcs(active_rpcs),
          max_active_rpcs_per_peer(rpcs_per_peer),
          max_subscribers_per_stream(subscribers_per_stream),
          max_in_flight_total(in_flight_total),
          max_in_flight_per_rpc(in_flight_per_rpc) {}

    std::mutex mu;
    const uint32_t max_active_rpcs;
    const uint32_t max_active_rpcs_per_peer;
    const uint32_t max_subscribers_per_stream;
    const uint32_t max_in_flight_total;
    const uint32_t max_in_flight_per_rpc;
    uint32_t active_rpcs = 0;
    uint32_t in_flight_total = 0;
    std::unordered_map<std::string, uint32_t> active_by_peer;
    std::unordered_map<std::string, uint32_t> active_by_stream;
};

StreamAdmissionController::RpcPermit::RpcPermit(
    std::shared_ptr<State> state, std::string peer, std::string stream)
    : state_(std::move(state)),
      peer_(std::move(peer)),
      stream_(std::move(stream)) {}

StreamAdmissionController::RpcPermit::~RpcPermit() noexcept {
    if (!armed_) return;
    try {
        std::lock_guard lock(state_->mu);
        if (state_->active_rpcs > 0) --state_->active_rpcs;
        auto peer = state_->active_by_peer.find(peer_);
        if (peer != state_->active_by_peer.end() && peer->second > 0 &&
            --peer->second == 0) {
            state_->active_by_peer.erase(peer);
        }
        auto stream = state_->active_by_stream.find(stream_);
        if (stream != state_->active_by_stream.end() && stream->second > 0 &&
            --stream->second == 0) {
            state_->active_by_stream.erase(stream);
        }
    } catch (...) {
    }
}

StreamAdmissionController::WorkPermit::WorkPermit(
    std::shared_ptr<State> state,
    std::shared_ptr<std::atomic<uint32_t>> rpc_in_flight)
    : state_(std::move(state)),
      rpc_in_flight_(std::move(rpc_in_flight)) {}

StreamAdmissionController::WorkPermit::~WorkPermit() noexcept {
    try {
        std::lock_guard lock(state_->mu);
        if (state_->in_flight_total > 0) --state_->in_flight_total;
    } catch (...) {
    }
    if (rpc_in_flight_) rpc_in_flight_->fetch_sub(1);
}

StreamAdmissionController::StreamAdmissionController(
    uint32_t max_active_rpcs,
    uint32_t max_active_rpcs_per_peer,
    uint32_t max_subscribers_per_stream,
    uint32_t max_in_flight_total,
    uint32_t max_in_flight_per_rpc) noexcept {
    try {
        state_ = std::make_shared<State>(
            max_active_rpcs, max_active_rpcs_per_peer,
            max_subscribers_per_stream, max_in_flight_total,
            max_in_flight_per_rpc);
    } catch (...) {
    }
}

std::shared_ptr<StreamAdmissionController::RpcPermit>
StreamAdmissionController::try_acquire_rpc(
    const std::string& peer, const std::string& stream) noexcept {
    const auto state = state_;
    if (!state) return nullptr;

    try {
        std::lock_guard lock(state->mu);
        const auto peer_it = state->active_by_peer.find(peer);
        const auto stream_it = state->active_by_stream.find(stream);
        const uint32_t peer_count = peer_it == state->active_by_peer.end()
            ? 0 : peer_it->second;
        const uint32_t stream_count = stream_it == state->active_by_stream.end()
            ? 0 : stream_it->second;
        if ((state->max_active_rpcs > 0 &&
             state->active_rpcs >= state->max_active_rpcs) ||
            (state->max_active_rpcs_per_peer > 0 &&
             peer_count >= state->max_active_rpcs_per_peer) ||
            (state->max_subscribers_per_stream > 0 &&
             stream_count >= state->max_subscribers_per_stream)) {
            return nullptr;
        }

        auto permit = std::shared_ptr<RpcPermit>(
            new RpcPermit(state, peer, stream));
        auto inserted_peer =
            state->active_by_peer.try_emplace(peer, peer_count).first;
        try {
            auto inserted_stream =
                state->active_by_stream.try_emplace(stream, stream_count).first;
            ++inserted_peer->second;
            ++inserted_stream->second;
            ++state->active_rpcs;
            permit->armed_ = true;
        } catch (...) {
            if (inserted_peer->second == 0)
                state->active_by_peer.erase(inserted_peer);
            throw;
        }
        return permit;
    } catch (...) {
        return nullptr;
    }
}

std::shared_ptr<StreamAdmissionController::WorkPermit>
StreamAdmissionController::try_acquire_work(
    const std::shared_ptr<std::atomic<uint32_t>>& rpc_in_flight) noexcept {
    const auto state = state_;
    if (!state || !rpc_in_flight) return nullptr;

    uint32_t local = rpc_in_flight->load(std::memory_order_acquire);
    for (;;) {
        if (state->max_in_flight_per_rpc > 0 &&
            local >= state->max_in_flight_per_rpc) {
            return nullptr;
        }
        if (rpc_in_flight->compare_exchange_weak(
                local, local + 1, std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            break;
        }
    }

    try {
        std::lock_guard lock(state->mu);
        if (state->max_in_flight_total > 0 &&
            state->in_flight_total >= state->max_in_flight_total) {
            rpc_in_flight->fetch_sub(1, std::memory_order_acq_rel);
            return nullptr;
        }
        auto permit = std::shared_ptr<WorkPermit>(
            new WorkPermit(state, rpc_in_flight));
        ++state->in_flight_total;
        return permit;
    } catch (...) {
        rpc_in_flight->fetch_sub(1, std::memory_order_acq_rel);
        return nullptr;
    }
}

StreamDmaInputLease::~StreamDmaInputLease() {
    if (bound_valid_ && free_tensor_) free_tensor_(&bound_);
}

std::shared_ptr<StreamDmaInputLease> bind_stream_nv12_input(
    HalInferenceSession* session,
    const HalInferenceOps* ops,
    const ReceivedFrame& frame,
    int* bind_status) {
    if (bind_status) *bind_status = HAL_ERR_INVALID_ARG;
    if (!session || !ops || !ops->bind_dma_frame || !ops->free_tensor ||
        !frame.fd_group || frame.fd_group->fds.empty() || frame.width == 0 ||
        frame.height == 0 || frame.format != HAL_PIX_FMT_NV12) {
        return nullptr;
    }

    if ((frame.width & 1u) != 0 || (frame.height & 1u) != 0)
        return nullptr;

    const bool single_fd = frame.num_planes == 1 &&
                           frame.fd_group->fds.size() == 1;
    const bool dual_fd = frame.num_planes == 2 &&
                         frame.fd_group->fds.size() == 2;
    if ((!single_fd && !dual_fd) || frame.fd_group->fds[0] < 0 ||
        (dual_fd && frame.fd_group->fds[1] < 0)) {
        return nullptr;
    }
    if (frame.strides[0] != frame.width ||
        (dual_fd && frame.strides[1] != frame.width)) {
        return nullptr;
    }

    const uint64_t y_bytes =
        static_cast<uint64_t>(frame.width) * frame.height;
    constexpr uint64_t kMaxTensorBytes =
        std::numeric_limits<uint32_t>::max();
    if (y_bytes > kMaxTensorBytes) return nullptr;
    const uint64_t uv_bytes = y_bytes / 2;
    if (uv_bytes > kMaxTensorBytes - y_bytes) return nullptr;
    const uint64_t total_bytes = y_bytes + uv_bytes;

    HalDmaFrameDesc desc{};
    for (int& fd : desc.fd) fd = -1;
    desc.format = HAL_PIX_FMT_NV12;
    desc.width = frame.width;
    desc.height = frame.height;
    desc.borrowed = 1;

    if (dual_fd) {
        if (frame.sizes[0] < y_bytes || frame.sizes[1] < uv_bytes) return nullptr;
        desc.fd[0] = frame.fd_group->fds[0];
        desc.fd[1] = frame.fd_group->fds[1];
        desc.stride[0] = frame.strides[0];
        desc.stride[1] = frame.strides[1];
        desc.bytes_used[0] = static_cast<uint32_t>(y_bytes);
        desc.bytes_used[1] = static_cast<uint32_t>(uv_bytes);
    } else {
        if (frame.sizes[0] < total_bytes) return nullptr;
        desc.fd[0] = frame.fd_group->fds[0];
        desc.stride[0] = frame.strides[0];
        desc.bytes_used[0] = static_cast<uint32_t>(total_bytes);
    }

    auto lease = std::shared_ptr<StreamDmaInputLease>(
        new StreamDmaInputLease());
    const int rc = ops->bind_dma_frame(session, &desc, &lease->bound_);
    if (bind_status) *bind_status = rc;
    if (rc != HAL_OK) return nullptr;

    lease->free_tensor_ = ops->free_tensor;
    lease->fd_group_ = frame.fd_group;
    lease->bound_valid_ = true;
    return lease;
}

FrameRateGate::FrameRateGate(uint32_t fps) {
    if (fps > 0) {
        interval_ = std::chrono::nanoseconds(
            (1000000000ULL + fps - 1) / fps);
    }
}

bool FrameRateGate::allow(TimePoint now) {
    if (interval_ == std::chrono::nanoseconds::zero()) return true;
    if (now < next_allowed_) return false;
    next_allowed_ = now + interval_;
    return true;
}

bool apply_result_filters(
    pb::PostResult* result,
    uint32_t max_results,
    float min_confidence,
    const std::unordered_set<int32_t>& class_filter,
    bool omit_labels) {
    if (result == nullptr) return false;

    const bool has_conf_gate = min_confidence > 0.0f;
    const bool has_class_gate = !class_filter.empty();
    auto class_ok = [&](int32_t class_id) {
        return !has_class_gate || class_filter.count(class_id) != 0;
    };
    auto conf_ok = [&](float confidence) {
        return std::isfinite(confidence) &&
               (!has_conf_gate || confidence >= min_confidence);
    };
    auto by_conf_desc = [](const auto& lhs, const auto& rhs) {
        return lhs.confidence() > rhs.confidence();
    };
    auto truncate_detections = [max_results](auto* values) {
        if (max_results == 0 ||
            static_cast<uint32_t>(values->size()) <= max_results) {
            return;
        }
        values->DeleteSubrange(
            static_cast<int>(max_results),
            static_cast<int>(values->size()) - static_cast<int>(max_results));
    };

    {
        auto* detections = result->mutable_detections();
        detections->erase(
            std::remove_if(detections->begin(), detections->end(),
                           [&](const pb::Detection& detection) {
                               return !conf_ok(detection.confidence()) ||
                                      !class_ok(detection.class_id());
                           }),
            detections->end());
        if (max_results > 0) {
            std::stable_sort(detections->begin(), detections->end(),
                             by_conf_desc);
            truncate_detections(detections);
        }
        if (omit_labels) {
            for (auto& detection : *detections) detection.clear_label();
        }
    }

    {
        auto* classifications = result->mutable_classifications();
        classifications->erase(
            std::remove_if(classifications->begin(), classifications->end(),
                           [&](const pb::Classification& classification) {
                               return !conf_ok(classification.confidence()) ||
                                      !class_ok(classification.class_id());
                           }),
            classifications->end());
        if (omit_labels) {
            for (auto& classification : *classifications) {
                classification.clear_label();
            }
        }
    }

    {
        auto* masks = result->mutable_masks();
        masks->erase(
            std::remove_if(masks->begin(), masks->end(),
                           [&](const pb::SegmentationMask& mask) {
                               return !conf_ok(mask.confidence()) ||
                                      !class_ok(mask.class_id());
                           }),
            masks->end());
        if (omit_labels) {
            for (auto& mask : *masks) mask.clear_label();
        }
    }

    {
        auto* lines = result->mutable_ocr_lines();
        lines->erase(
            std::remove_if(lines->begin(), lines->end(),
                           [&](const pb::OcrLine& line) {
                               return !conf_ok(line.confidence());
                           }),
            lines->end());
    }

    return result->detections_size() > 0 ||
           result->classifications_size() > 0 ||
           result->landmarks_size() > 0 || result->masks_size() > 0 ||
           result->ocr_lines_size() > 0 || result->embeddings_size() > 0 ||
           result->depth_maps_size() > 0;
}

}  // namespace aipc::ai_runtime
