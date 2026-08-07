#pragma once

#include <cstdint>
#include <string>
#include <functional>
#include <chrono>
#include <sstream>

#include "fd_receiver.h"

extern "C" {
#include "hal_inference.h"
#include "hal_postprocess.h"
}

namespace aipc::ai_runtime {

using SteadyClock  = std::chrono::steady_clock;
using TimePoint    = SteadyClock::time_point;
using Microseconds = std::chrono::microseconds;
using Milliseconds = std::chrono::milliseconds;

inline uint64_t now_us() {
    return std::chrono::duration_cast<Microseconds>(
        SteadyClock::now().time_since_epoch()).count();
}

inline uint64_t now_ns() {
    auto tp = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        tp.time_since_epoch()).count();
}

/// Build NV12 Y+UV tensor pair from a received DMA-BUF frame.
/// Returns number of input tensors (1 or 2).
inline int build_nv12_tensors(const ReceivedFrame& frame, HalTensor* inputs) {
    int num_inputs = (frame.fd_group && frame.fd_group->fds.size() >= 2) ? 2 : 1;

    inputs[0] = {};
    inputs[0].data      = nullptr;
    inputs[0].dma_fd    = (frame.fd_group && !frame.fd_group->fds.empty())
                          ? frame.fd_group->fds[0] : -1;
    inputs[0].ndim      = 3;
    inputs[0].shape[0]  = static_cast<int32_t>(frame.height);
    inputs[0].shape[1]  = static_cast<int32_t>(frame.width);
    inputs[0].shape[2]  = 1;
    inputs[0].dtype     = HAL_DTYPE_UINT8;
    inputs[0].byte_size = (frame.num_planes > 0) ? frame.sizes[0] : 0;

    if (num_inputs >= 2) {
        inputs[1] = {};
        inputs[1].data      = nullptr;
        inputs[1].dma_fd    = frame.fd_group->fds[1];
        inputs[1].ndim      = 2;
        inputs[1].shape[0]  = static_cast<int32_t>(frame.height / 2);
        inputs[1].shape[1]  = static_cast<int32_t>(frame.width);
        inputs[1].dtype     = HAL_DTYPE_UINT8;
        inputs[1].byte_size = frame.sizes[1] ? frame.sizes[1]
                              : (frame.strides[1] * frame.height / 2);
    }
    return num_inputs;
}

/// Build a single-plane tensor for CLIP / embedding models.
/// Passes the DMA-BUF fd directly — HailoRT handles NV12→RGB conversion
/// internally when the HEF model expects RGB input.
inline int build_clip_tensor(const ReceivedFrame& frame, HalTensor* inputs) {
    inputs[0] = {};
    inputs[0].data      = nullptr;
    inputs[0].dma_fd    = (frame.fd_group && !frame.fd_group->fds.empty())
                          ? frame.fd_group->fds[0] : -1;
    inputs[0].ndim      = 3;
    inputs[0].shape[0]  = static_cast<int32_t>(frame.height);
    inputs[0].shape[1]  = static_cast<int32_t>(frame.width);
    inputs[0].shape[2]  = 1;
    inputs[0].dtype     = HAL_DTYPE_UINT8;
    inputs[0].byte_size = (frame.num_planes > 0) ? frame.sizes[0] : 0;
    return 1;
}

/// Serialize HalPostprocessResult (C struct) to JSON string for Event Bus publishing.
std::string post_result_to_json(const std::string& stream_id,
                                const std::string& model_id,
                                uint64_t frame_seq,
                                uint64_t timestamp_ns,
                                const HalPostprocessResult& result);

}  // namespace aipc::ai_runtime

// Forward-declare protobuf type to avoid pulling inference.pb.h into common.h.
namespace aipc::inference { class PostResult; }

namespace aipc::ai_runtime {

/// Serialize protobuf PostResult to JSON string.
/// Uses the same field names as post_result_to_json so consumers see a uniform schema.
std::string post_result_pb_to_json(const std::string& stream_id,
                                   const std::string& model_id,
                                   uint64_t frame_seq,
                                   uint64_t timestamp_ns,
                                   const aipc::inference::PostResult& result);

}  // namespace aipc::ai_runtime
