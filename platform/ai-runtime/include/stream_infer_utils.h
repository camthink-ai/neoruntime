#pragma once

#include "inference.pb.h"
#include "fd_receiver.h"
#include "hal_inference.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace aipc::ai_runtime {

constexpr int kMaxStreamClassFilterEntries = 256;
constexpr uint32_t kMaxStreamFps = 120;

bool valid_stream_fps(uint32_t fps);
bool valid_stream_min_confidence(float confidence);
bool valid_stream_class_filter_size(int entries);
bool valid_stream_id(const std::string& stream_id);

class StreamAdmissionController final {
private:
    struct State;

public:
    class RpcPermit final {
    public:
        ~RpcPermit() noexcept;
        RpcPermit(const RpcPermit&) = delete;
        RpcPermit& operator=(const RpcPermit&) = delete;

    private:
        friend class StreamAdmissionController;
        RpcPermit(std::shared_ptr<State> state, std::string peer,
                  std::string stream);

        std::shared_ptr<State> state_;
        std::string peer_;
        std::string stream_;
        bool armed_ = false;
    };

    class WorkPermit final {
    public:
        ~WorkPermit() noexcept;
        WorkPermit(const WorkPermit&) = delete;
        WorkPermit& operator=(const WorkPermit&) = delete;

    private:
        friend class StreamAdmissionController;
        WorkPermit(std::shared_ptr<State> state,
                   std::shared_ptr<std::atomic<uint32_t>> rpc_in_flight);

        std::shared_ptr<State> state_;
        std::shared_ptr<std::atomic<uint32_t>> rpc_in_flight_;
    };

    StreamAdmissionController(
        uint32_t max_active_rpcs,
        uint32_t max_active_rpcs_per_peer,
        uint32_t max_subscribers_per_stream,
        uint32_t max_in_flight_total,
        uint32_t max_in_flight_per_rpc) noexcept;

    std::shared_ptr<RpcPermit> try_acquire_rpc(
        const std::string& peer, const std::string& stream) noexcept;
    std::shared_ptr<WorkPermit> try_acquire_work(
        const std::shared_ptr<std::atomic<uint32_t>>& rpc_in_flight) noexcept;

private:
    std::shared_ptr<State> state_;
};

class StreamDmaInputLease final {
public:
    ~StreamDmaInputLease();
    StreamDmaInputLease(const StreamDmaInputLease&) = delete;
    StreamDmaInputLease& operator=(const StreamDmaInputLease&) = delete;

    const HalTensor& tensor() const noexcept { return bound_; }

private:
    friend std::shared_ptr<StreamDmaInputLease> bind_stream_nv12_input(
        HalInferenceSession* session,
        const HalInferenceOps* ops,
        const ReceivedFrame& frame,
        int* bind_status);

    StreamDmaInputLease() = default;

    HalTensor bound_{};
    void (*free_tensor_)(HalTensor*) = nullptr;
    std::shared_ptr<FdGroup> fd_group_;
    bool bound_valid_ = false;
};

std::shared_ptr<StreamDmaInputLease> bind_stream_nv12_input(
    HalInferenceSession* session,
    const HalInferenceOps* ops,
    const ReceivedFrame& frame,
    int* bind_status = nullptr);

class FrameRateGate {
public:
    using Clock = std::chrono::steady_clock;
    using TimePoint = Clock::time_point;

    explicit FrameRateGate(uint32_t fps);

    bool allow(TimePoint now);

private:
    // Ratio (credit-bucket) pacing: credits accrue at fps_limit per second,
    // a frame is submitted the moment it arrives with >=1 credit banked, and
    // accrual is capped at kMaxCredits so a source stall never banks more
    // than that much catch-up. Sustained submit rate converges to
    // min(fps_limit, source rate) regardless of divisibility — a hard
    // time-threshold ceiling locks to source/k subharmonics when k frames
    // per interval isn't integral (15fps capped at 10 submitted 7.5fps).
    std::chrono::nanoseconds interval_{};
    double credits_ = 1.0;  // first frame passes
    TimePoint last_{};
    static constexpr double kMaxCredits = 2.0;
};

bool apply_result_filters(
    aipc::inference::PostResult* result,
    uint32_t max_results,
    float min_confidence,
    const std::unordered_set<int32_t>& class_filter,
    bool omit_labels);

}  // namespace aipc::ai_runtime
