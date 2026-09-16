#pragma once

/**
 * @file dsp_client.h
 * @brief Stream preprocessing client for camera-daemon DSP resize jobs.
 *
 * C++ counterpart of the SDK's DspClient, trimmed to what StreamInfer
 * needs. Two planes, matching the daemon's split:
 *   - buffer plane: camera.sock UDS — DSP_ALLOC / DSP_IMPORT / DSP_BUF_RELEASE
 *     (fd_protocol.h structs; fds cross via SCM_RIGHTS)
 *   - job plane: camera-control.sock gRPC — CameraControl.SubmitDspJob
 *     (synchronous form: the RPC returns only after the DSP worker executed
 *     the op, so a successful resize means the dst buffer is safe to read)
 *
 * Ownership rules honored here (daemon-side contract):
 *   - buffer ids are owned by the UDS connection that allocated/imported
 *     them; DSP_BUF_RELEASE must go over THAT connection (-2 otherwise)
 *   - closing the UDS connection releases every buffer of this client
 *     (used as the safety net on process death)
 *   - jobs may reference ids from any connection (global registry), but
 *     quota (jobs/s + MPix/s) is charged to the SRC buffer's owner process
 *   - a RESIZE dst must be a pool-allocated buffer (imports are src-only)
 *
 * StreamPreprocessPool layers a shared model-geometry pool on top:
 * DSP_IMPORT the incoming frame, RESIZE it into a free private slot, then
 * fd-bind the slot (bind_dma_frame) as the NPU input — zero CPU pixel
 * copies. Slots cycle free → bound (riding an InferRequest) → free.
 */

#include "fd_protocol.h"
#include "fd_receiver.h"
#include "model_manager.h"

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace aipc::ai_runtime {

class StreamPreprocessPool;

class DspClient {
public:
    // Proto enum values (camera.proto DspInterpolation / DspPriority);
    // mirrored here so the header does not need the generated proto.
    enum Interp   { INTERP_NEAREST = 0, INTERP_BILINEAR = 1 };
    enum Priority { PRIO_BACKGROUND = 0, PRIO_NORMAL = 1 };

    // One DSP_ALLOC round: geometry + daemon ids + our plane fd dups.
    // fds is buffer-major, ids.size() * num_planes entries; the CALLEE
    // closes them via release_pool / process exit (whichever comes first).
    struct PoolInfo {
        uint32_t width = 0, height = 0, num_planes = 0;
        uint32_t strides[3] = {0, 0, 0};
        uint32_t sizes[3]   = {0, 0, 0};
        uint64_t uds_generation = 0;  // connection that owns every id
        std::vector<uint64_t> ids;
        std::vector<int>      fds;
    };

    DspClient(const std::string& uds_path, const std::string& grpc_endpoint);
    ~DspClient();  // closes the UDS: daemon reaps this client's ids/jobs

    DspClient(const DspClient&) = delete;
    DspClient& operator=(const DspClient&) = delete;

    /// DSP_ALLOC count NV12 buffers at w×h. 0 on success (PoolInfo filled),
    /// <0 on transport/daemon failure (nothing allocated).
    int alloc_pool(PoolInfo* out, uint32_t w, uint32_t h, uint32_t count);

    /// DSP_BUF_RELEASE for every id of one pool (fire-and-forget, like the
    /// SDK). Plane fds stay with the caller — close them separately.
    void release_pool(PoolInfo* pool);

    /// DSP_IMPORT a received frame's plane fds (daemon dups them, so our
    /// copies stay ours to close). 0 on success with *id and the owning UDS
    /// connection generation set.
    int import_frame(const ReceivedFrame& frame, uint64_t* id,
                     uint64_t* uds_generation);

    enum ReleaseResult {
        RELEASE_OK = 0,
        RELEASE_TRANSPORT_ERROR = -1,
        RELEASE_STALE_GENERATION = -2,
    };

    /// DSP_BUF_RELEASE one id on the connection generation that owns it.
    /// Returns RELEASE_OK only when the release message was sent; a missing or
    /// broken connection is reported instead of silently reconnecting, because
    /// disconnect cleanup has already revoked every id from that generation.
    int release_buffer(uint64_t id, uint64_t uds_generation);

    /// Synchronous SubmitDspJob RESIZE src→dst (single dst, bilinear,
    /// stretch). Returns 0 on success, <0 otherwise with *err_code (daemon
    /// DspService error code on -2, grpc status code on -1) and *message.
    int resize(uint64_t src_id, uint64_t dst_id, Interp interp, Priority prio,
               uint32_t timeout_ms, int* err_code, std::string* message);

private:
    int ensure_uds_locked();
    void close_uds_locked();
    std::shared_ptr<grpc::Channel> ensure_channel_locked();

    std::string uds_path_;
    std::string grpc_endpoint_;

    std::mutex uds_mu_;
    int        uds_fd_ = -1;
    uint64_t   uds_generation_ = 0;

    std::mutex                  grpc_mu_;
    std::shared_ptr<grpc::Channel> channel_;
};

/**
 * Shared preprocess pool for one model input geometry. Frames whose geometry
 * does NOT match the model are resized
 * into pool slots on the DSP and fd-bound — the path that today fails at
 * the HAL input size check (-2811).
 *
 * Lifetime: acquired lazily by mismatched streams and shared while the same
 * geometry remains active. A lease that outlives the pool (late callback after
 * a stream drain timeout)
 * still self-cleans: the pool releases daemon ids before DspClient shutdown,
 * while shared slot state lets the last lease close its plane fds safely.
 */
class StreamPreprocessPool
    : public std::enable_shared_from_this<StreamPreprocessPool> {
public:
    StreamPreprocessPool(DspClient& dsp, ModelManager* mgr,
                         uint32_t job_timeout_ms);
    ~StreamPreprocessPool();

    StreamPreprocessPool(const StreamPreprocessPool&) = delete;
    StreamPreprocessPool& operator=(const StreamPreprocessPool&) = delete;

    /// Allocate the pool. 0 on success; usable() then tells whether the
    /// layout is direct-bindable (compact strides).
    int init(uint32_t model_w, uint32_t model_h, uint32_t slots);

    bool     usable() const { return usable_.load(); }
    uint32_t width()  const { return width_; }
    uint32_t height() const { return height_; }

    /// A pool slot pinned for one frame. Destruction frees the bound HAL
    /// tensor (after the NPU read completed) and returns the slot; this is
    /// what rides InferRequest.resource_holder. The lease also pins its owning
    /// pool so daemon buffer ids and local plane fds cannot be retired while
    /// the NPU is still reading the slot.
    struct SlotLease {
        struct Impl;                      // slot + shared pool state
        std::shared_ptr<Impl> impl;
        std::shared_ptr<StreamPreprocessPool> pool_owner;

        SlotLease() = default;
        SlotLease(const SlotLease&) = delete;
        SlotLease& operator=(const SlotLease&) = delete;
        ~SlotLease();
    };

    /// Import + resize the frame into a free slot. nullptr when any step
    /// fails or no slot is free (caller retains the direct DMA path and still
    /// holds the source frame). On success the source frame may be released
    /// immediately: the resized pixels live in the private slot. *dsp_us gets
    /// the resize job wall time.
    std::shared_ptr<SlotLease> prepare(const ReceivedFrame& frame,
                                       uint64_t* dsp_us);

    /// fd-bind the slot's resized dmabuf as the NPU input tensor. The
    /// lease takes ownership of the bound tensor (freed at lease
    /// destruction, i.e. after the NPU read).
    bool bind(HalInferenceSession* sess, SlotLease* lease, HalTensor* out);

private:
    struct Shared;                        // slot table outliving leases

    DspClient&     dsp_;
    ModelManager*  mgr_;
    uint32_t       job_timeout_ms_;
    uint32_t       width_ = 0, height_ = 0;
    std::atomic<bool> usable_{false};
    DspClient::PoolInfo pool_;
    std::shared_ptr<Shared> shared_;
};

}  // namespace aipc::ai_runtime
