#include "dsp_client.h"
#include "log.h"

#include <grpcpp/create_channel_posix.h>

#include "camera.grpc.pb.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <chrono>

namespace aipc::ai_runtime {

namespace {

// One UDS round-trip must not hang a StreamInfer frame forever if the daemon
// wedges.
const timeval kIoTimeout{2, 0};

constexpr uint32_t kHalPixFmtNv12 = 0;  // HalPixelFormat NV12

const char* dsp_svc_error_text(int code) {
    switch (code) {
        case -1: return "invalid buffer descriptor";
        case -2: return "unknown or foreign buffer id";
        case -3: return "quota exceeded";
        case -4: return "timeout";
        case -5: return "dsp service unavailable";
        case -6: return "out of memory";
        case -7: return "per-client buffer limit";
        default:  return "daemon error";
    }
}

uint64_t steady_now_us() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

// recvmsg with an ancillary buffer sized for the DSP alloc response (up to
// FD_PUB_DSP_MAX_FDS fds). fd_protocol.h's fd_pub_recvmsg only has room for
// FD_PUB_MAX_FDS(3) — using it for a multi-buffer alloc would silently lose
// the excess fds to kernel-side MSG_CTRUNC. Returns bytes received (<0 on
// error); fds land in fds[]. MSG_CTRUNC reports -1 (excess fds were closed
// by the kernel; the stream is suspect anyway).
int recv_dsp_msg(int fd, void* data, size_t data_len,
                 int* fds, int* num_fds, int max_fds) {
    struct msghdr msg{};
    struct iovec iov;
    iov.iov_base = data;
    iov.iov_len = data_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int) * FD_PUB_DSP_MAX_FDS)];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    *num_fds = 0;
    ssize_t n = ::recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
    if (n <= 0) return -1;

    bool ancillary_error = (msg.msg_flags & MSG_CTRUNC) != 0;
    for (struct cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        if (c->cmsg_len < CMSG_LEN(0)) {
            ancillary_error = true;
            continue;
        }

        const int nfds = static_cast<int>(
            (c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        const int* received = reinterpret_cast<const int*>(CMSG_DATA(c));
        for (int i = 0; i < nfds; ++i) {
            if (!ancillary_error && *num_fds < max_fds &&
                *num_fds < FD_PUB_DSP_MAX_FDS) {
                fds[(*num_fds)++] = received[i];
            } else {
                ::close(received[i]);
                ancillary_error = true;
            }
        }
    }

    if (ancillary_error) {
        for (int i = 0; i < *num_fds; ++i) ::close(fds[i]);
        *num_fds = 0;
        return -1;
    }
    return static_cast<int>(n);
}

// Drain the remainder of a struct that arrived fragmented (SOCK_STREAM has no
// message boundaries). Any SCM_RIGHTS fds rode with the first byte and were
// already extracted — the rest is plain bytes.
bool recv_rest(int fd, void* data, size_t have, size_t want) {
    char* p = static_cast<char*>(data) + have;
    size_t left = want - have;
    while (left > 0) {
        ssize_t n = ::recv(fd, p, left, MSG_WAITALL);
        if (n <= 0) return false;
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

// "unix:///run/aipc/x.sock" → "/run/aipc/x.sock" ("" when not a unix path).
std::string extract_unix_path(const std::string& ep) {
    if (ep.rfind("unix:///", 0) == 0) return ep.substr(7);
    if (ep.rfind("unix:", 0) == 0)    return ep.substr(5);
    if (!ep.empty() && ep[0] == '/')  return ep;
    return {};
}

}  // namespace

// ─── DspClient ───────────────────────────────────────────────────────────────

DspClient::DspClient(const std::string& uds_path, const std::string& grpc_endpoint)
    : uds_path_(uds_path), grpc_endpoint_(grpc_endpoint) {}

DspClient::~DspClient() {
    // Closing the UDS releases every buffer this process allocated/imported
    // and reaps its async jobs (daemon disconnect cleanup).
    std::lock_guard lock(uds_mu_);
    close_uds_locked();
}

void DspClient::close_uds_locked() {
    if (uds_fd_ >= 0) ::close(uds_fd_);
    uds_fd_ = -1;
}

int DspClient::ensure_uds_locked() {
    if (uds_fd_ >= 0) return uds_fd_;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("DspClient: socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, uds_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("DspClient: connect(%s) failed: %s",
                  uds_path_.c_str(), strerror(errno));
        ::close(fd);
        return -1;
    }

    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &kIoTimeout, sizeof(kIoTimeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &kIoTimeout, sizeof(kIoTimeout));

    uds_fd_ = fd;
    ++uds_generation_;
    if (uds_generation_ == 0) ++uds_generation_;  // reserve 0 for "no owner"
    LOG_INFO("DspClient: connected to %s (dsp buffer plane, generation=%lu)",
             uds_path_.c_str(), (unsigned long)uds_generation_);
    return uds_fd_;
}

std::shared_ptr<grpc::Channel> DspClient::ensure_channel_locked() {
    if (channel_) return channel_;

    std::string path = extract_unix_path(grpc_endpoint_);
    if (path.empty()) {
        LOG_ERROR("DspClient: job endpoint '%s' is not a unix socket path",
                  grpc_endpoint_.c_str());
        return nullptr;
    }
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;
    struct sockaddr_un sa{};
    sa.sun_family = AF_UNIX;
    std::strncpy(sa.sun_path, path.c_str(), sizeof(sa.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) != 0) {
        LOG_ERROR("DspClient: connect(%s) failed: %s", path.c_str(), strerror(errno));
        ::close(fd);
        return nullptr;
    }
    // CreateInsecureChannelFromFd takes fd ownership (musl resolver bypass,
    // same as EventBusClient). The channel is cached until an RPC transport
    // failure, then the next call reconnects after a daemon restart.
    channel_ = grpc::CreateInsecureChannelFromFd("camera-control", fd);
    if (!channel_) {
        LOG_ERROR("DspClient: CreateInsecureChannelFromFd(%s) failed", path.c_str());
        return nullptr;
    }
    LOG_INFO("DspClient: job plane connected to %s", path.c_str());
    return channel_;
}

int DspClient::alloc_pool(PoolInfo* out, uint32_t w, uint32_t h, uint32_t count) {
    std::lock_guard lock(uds_mu_);
    if (!out || w == 0 || h == 0 || count == 0 ||
        count * 2u > FD_PUB_DSP_MAX_FDS)  // NV12: 2 planes per buffer
        return -1;

    int fd = ensure_uds_locked();
    if (fd < 0) return -1;

    FdPubDspAllocMsg msg{};
    msg.hdr.type = FD_PUB_MSG_DSP_ALLOC;
    msg.hdr.size = sizeof(msg);
    msg.width  = w;
    msg.height = h;
    msg.format = kHalPixFmtNv12;
    msg.count  = count;

    if (fd_pub_sendmsg(fd, &msg, sizeof(msg), nullptr, 0) != 0) {
        LOG_ERROR("DspClient: ALLOC send failed (%ux%u x%u): %s",
                  (unsigned)w, (unsigned)h, (unsigned)count, strerror(errno));
        close_uds_locked();
        return -1;
    }

    FdPubDspAllocRespMsg resp{};
    int fds[FD_PUB_DSP_MAX_FDS];
    int num_fds = 0;
    int n = recv_dsp_msg(fd, &resp, sizeof(resp), fds, &num_fds, FD_PUB_DSP_MAX_FDS);
    if (n <= 0 ||
        (static_cast<size_t>(n) < sizeof(resp) &&
         !recv_rest(fd, &resp, static_cast<size_t>(n), sizeof(resp))) ||
        resp.hdr.type != FD_PUB_MSG_DSP_ALLOC_RESP) {
        LOG_ERROR("DspClient: ALLOC resp broken (n=%d type=%u): %s",
                  n, resp.hdr.type, n <= 0 ? strerror(errno) : "desync");
        for (int i = 0; i < num_fds; ++i) ::close(fds[i]);
        close_uds_locked();
        return -1;
    }

    if (resp.code != 0) {
        LOG_ERROR("DspClient: ALLOC rejected (%ux%u x%u): %d (%s)",
                  (unsigned)w, (unsigned)h, (unsigned)count, resp.code,
                  dsp_svc_error_text(resp.code));
        for (int i = 0; i < num_fds; ++i) ::close(fds[i]);  // none expected
        return -1;
    }

    const uint32_t got = resp.count;
    if (got == 0 || got > count ||
        num_fds != static_cast<int>(got * resp.num_planes)) {
        LOG_ERROR("DspClient: ALLOC plane/fd mismatch (count=%u planes=%u fds=%d)",
                  got, resp.num_planes, num_fds);
        for (int i = 0; i < num_fds; ++i) ::close(fds[i]);
        close_uds_locked();
        return -1;
    }

    out->width          = w;
    out->height         = h;
    out->num_planes     = resp.num_planes;
    out->uds_generation = uds_generation_;
    for (uint32_t i = 0; i < 3; ++i) {
        out->strides[i] = resp.strides[i];
        out->sizes[i]   = resp.sizes[i];
    }
    out->ids.assign(resp.buffer_ids, resp.buffer_ids + got);
    out->fds.assign(fds, fds + num_fds);
    LOG_INFO("DspClient: ALLOC %ux%u NV12 x%u (planes=%u stride=%u/%u)",
             (unsigned)w, (unsigned)h, (unsigned)got,
             resp.num_planes, resp.strides[0],
             resp.num_planes > 1 ? resp.strides[1] : 0);
    return 0;
}

void DspClient::release_pool(PoolInfo* pool) {
    if (!pool) return;
    for (uint64_t id : pool->ids)
        release_buffer(id, pool->uds_generation);
    for (int fd : pool->fds) ::close(fd);
    pool->ids.clear();
    pool->fds.clear();
    pool->uds_generation = 0;
}

int DspClient::import_frame(const ReceivedFrame& frame, uint64_t* id,
                            uint64_t* uds_generation) {
    std::lock_guard lock(uds_mu_);
    if (id) *id = 0;
    if (uds_generation) *uds_generation = 0;
    if (!frame.fd_group || frame.fd_group->fds.empty()) return -1;

    int fd = ensure_uds_locked();
    if (fd < 0) return -1;

    FdPubDspImportMsg msg{};
    msg.hdr.type   = FD_PUB_MSG_DSP_IMPORT;
    msg.hdr.size   = sizeof(msg);
    msg.width      = frame.width;
    msg.height     = frame.height;
    msg.format     = frame.format;
    msg.num_planes = frame.num_planes;
    for (uint32_t i = 0; i < 3; ++i) {
        msg.strides[i] = frame.strides[i];
        msg.sizes[i]   = frame.sizes[i];
    }

    // Partial SCM_RIGHTS send = fds already leaked to the daemon and the
    // stream desynced — hard failure, never retry (fd_protocol.h contract).
    if (fd_pub_sendmsg(fd, &msg, sizeof(msg),
                       frame.fd_group->fds.data(),
                       static_cast<int>(frame.fd_group->fds.size())) != 0) {
        LOG_ERROR("DspClient: IMPORT send failed (frame %ux%u): %s",
                  (unsigned)frame.width, (unsigned)frame.height,
                  strerror(errno));
        close_uds_locked();
        return -1;
    }

    FdPubDspImportRespMsg resp{};
    int n = ::recv(fd, &resp, sizeof(resp), MSG_WAITALL);
    if (n != static_cast<int>(sizeof(resp)) ||
        resp.hdr.type != FD_PUB_MSG_DSP_IMPORT_RESP) {
        LOG_ERROR("DspClient: IMPORT resp broken (n=%d type=%u)", n, resp.hdr.type);
        close_uds_locked();
        return -1;
    }
    if (resp.code != 0) {
        LOG_WARN("DspClient: IMPORT rejected (frame %ux%u): %d (%s)",
                 (unsigned)frame.width, (unsigned)frame.height,
                 resp.code, dsp_svc_error_text(resp.code));
        return -1;
    }
    if (id) *id = resp.import_id;
    if (uds_generation) *uds_generation = uds_generation_;
    return 0;
}

int DspClient::release_buffer(uint64_t id, uint64_t uds_generation) {
    std::lock_guard lock(uds_mu_);
    if (id == 0) return RELEASE_OK;

    if (uds_fd_ < 0) {
        LOG_WARN("DspClient: RELEASE id=%lu generation=%lu failed: connection "
                 "is closed (daemon disconnect cleanup revoked its buffers)",
                 (unsigned long)id, (unsigned long)uds_generation);
        return RELEASE_TRANSPORT_ERROR;
    }
    if (uds_generation == 0 || uds_generation != uds_generation_) {
        LOG_WARN("DspClient: RELEASE id=%lu generation=%lu skipped: active "
                 "generation is %lu (old buffers were revoked on disconnect)",
                 (unsigned long)id, (unsigned long)uds_generation,
                 (unsigned long)uds_generation_);
        return RELEASE_STALE_GENERATION;
    }

    FdPubDspBufReleaseMsg msg{};
    msg.hdr.type  = FD_PUB_MSG_DSP_BUF_RELEASE;
    msg.hdr.size  = sizeof(msg);
    msg.buffer_id = id;

    if (fd_pub_sendmsg(uds_fd_, &msg, sizeof(msg), nullptr, 0) != 0) {
        LOG_WARN("DspClient: RELEASE id=%lu generation=%lu send failed: %s "
                 "(daemon disconnect cleanup revokes all buffers in the pool)",
                 (unsigned long)id, (unsigned long)uds_generation,
                 strerror(errno));
        close_uds_locked();
        return RELEASE_TRANSPORT_ERROR;
    }
    return RELEASE_OK;
}

int DspClient::resize(uint64_t src_id, uint64_t dst_id, Interp interp,
                      Priority prio, uint32_t timeout_ms, int* err_code,
                      std::string* message) {
    if (err_code) *err_code = 0;
    if (message)  message->clear();

    std::shared_ptr<grpc::Channel> ch;
    {
        std::lock_guard lock(grpc_mu_);
        ch = ensure_channel_locked();
    }
    if (!ch) {
        if (err_code) *err_code = -1;
        if (message)  *message = "cannot reach camera-daemon control plane";
        return -1;
    }

    // Stub creation is a few small allocations; per-call keeps the header
    // free of generated-proto types.
    auto stub = aipc::camera::CameraControl::NewStub(ch);

    aipc::camera::DspJobRequest job;
    job.set_op(aipc::camera::DSP_OP_RESIZE);
    job.set_src_buffer_id(src_id);
    job.add_dst_buffer_ids(dst_id);
    job.set_interpolation(static_cast<aipc::camera::DspInterpolation>(interp));
    job.set_scaling_mode(aipc::camera::DSP_SCALING_STRETCH);
    job.set_priority(static_cast<aipc::camera::DspPriority>(prio));

    grpc::ClientContext ctx;
    // Client deadline rides above the intended job wait so a wedged daemon
    // cannot pin a StreamInfer frame forever. On deadline expiry the dst may
    // still be written by a late daemon-side completion — callers must treat
    // the slot as poisoned, never reused.
    if (timeout_ms > 0) {
        ctx.set_deadline(std::chrono::system_clock::now() +
                         std::chrono::milliseconds(timeout_ms + 50));
    }

    aipc::camera::DspJobResponse resp;
    grpc::Status st = stub->SubmitDspJob(&ctx, job, &resp);
    if (!st.ok()) {
        // A channel created from an already-connected Unix fd cannot recover
        // after daemon restart. Drop only the generation this call used so a
        // later resize reconnects without disturbing a newer channel.
        {
            std::lock_guard lock(grpc_mu_);
            if (channel_ == ch) channel_.reset();
        }
        if (err_code) *err_code = -1;
        if (message)  *message = "SubmitDspJob: " + st.error_message();
        return -1;
    }
    if (!resp.success()) {
        if (err_code) *err_code = resp.error_code();
        if (message)  *message = resp.message();
        return -2;
    }
    return 0;
}

// ─── StreamPreprocessPool ────────────────────────────────────────────────────

struct StreamPreprocessPool::Shared {
    struct Slot {
        uint64_t id = 0;
        std::vector<int> fds;      // plane fd dups for THIS slot
        bool in_use    = false;    // pinned by a live lease
        bool poisoned  = false;    // retired: possible late DSP write
        bool reclaimed = false;    // plane fds closed (once)
    };
    std::mutex mu;
    std::vector<Slot> slots;
    uint64_t uds_generation = 0;   // generation that owns every slot id
    size_t healthy_slots = 0;      // slots not retired by timeout/disconnect
    bool destroyed = false;        // pool dtor ran
    bool engaged_logged = false;   // one "engaged" line per shared pool
};

struct StreamPreprocessPool::SlotLease::Impl {
    std::shared_ptr<Shared> shared;
    uint32_t slot = 0;
    // Bind descriptor inputs (copied at prepare; pool may die before us).
    std::vector<int> fds;
    uint32_t num_planes = 0;
    uint32_t strides[3] = {0, 0, 0};
    uint32_t src_w = 0, src_h = 0;  // prepared frame geometry (logging)
    // Bound tensor (valid after a successful bind()); freed by OUR dtor,
    // i.e. after the NPU read completed.
    HalTensor bound{};
    void (*free_tensor)(HalTensor*) = nullptr;
    bool bound_valid = false;
};

StreamPreprocessPool::StreamPreprocessPool(DspClient& dsp, ModelManager* mgr,
                                           uint32_t job_timeout_ms)
    : dsp_(dsp), mgr_(mgr), job_timeout_ms_(job_timeout_ms) {}

StreamPreprocessPool::~StreamPreprocessPool() {
    if (shared_) {
        std::lock_guard lock(shared_->mu);
        shared_->destroyed = true;
        for (auto& s : shared_->slots) {
            // Release daemon ownership while DspClient is still guaranteed alive.
            // A live lease only needs our local fd dups for the pending NPU read.
            if (s.id != 0) {
                dsp_.release_buffer(s.id, shared_->uds_generation);
                s.id = 0;
            }
            if (s.in_use || s.reclaimed) continue;  // live lease closes its fds
            s.reclaimed = true;
            for (int fd : s.fds) ::close(fd);
            s.fds.clear();
        }
    }

    // Before init() commits slot ownership to shared_, PoolInfo remains the
    // sole owner. This also covers exceptions during Shared allocation/fill.
    dsp_.release_pool(&pool_);
}

int StreamPreprocessPool::init(uint32_t model_w, uint32_t model_h,
                               uint32_t slots) {
    if (model_w == 0 || model_h == 0 || slots == 0) return -1;

    int rc = dsp_.alloc_pool(&pool_, model_w, model_h, slots);
    if (rc != 0) return rc;
    width_  = model_w;
    height_ = model_h;

    // The slot buffers must be direct-bindable: compact NV12 layout
    // (stride == width on every plane, exact plane sizes). Padded strides
    // would be rejected by bind_dma_frame per-frame — detect once here and
    // leave mismatched frames on the existing direct DMA path instead.
    const uint32_t expect_sizes[2] = {model_w * model_h,
                                      model_w * (model_h / 2)};
    usable_ = (pool_.num_planes == 2);
    for (uint32_t p = 0; usable_ && p < pool_.num_planes; ++p) {
        if (pool_.strides[p] != model_w || pool_.sizes[p] != expect_sizes[p])
            usable_ = false;
    }
    if (!usable_) {
        LOG_WARN("StreamPreprocess: pool %ux%u NV12 layout not direct-bindable "
                 "(planes=%u stride=%u/%u size=%u/%u) — direct DMA fallback",
                 (unsigned)model_w, (unsigned)model_h,
                 pool_.num_planes, pool_.strides[0],
                 pool_.num_planes > 1 ? pool_.strides[1] : 0,
                 pool_.sizes[0], pool_.num_planes > 1 ? pool_.sizes[1] : 0);
        dsp_.release_pool(&pool_);  // ids/fds are not slot-managed in this mode
        return 0;  // allocated but not usable; caller checks usable()
    }

    auto new_shared = std::make_shared<Shared>();
    new_shared->uds_generation = pool_.uds_generation;
    new_shared->healthy_slots = pool_.ids.size();
    new_shared->slots.resize(pool_.ids.size());
    for (size_t i = 0; i < pool_.ids.size(); ++i) {
        auto& s = new_shared->slots[i];
        s.id = pool_.ids[i];
        s.fds.assign(
            pool_.fds.begin() + static_cast<long>(i * pool_.num_planes),
            pool_.fds.begin() + static_cast<long>((i + 1) * pool_.num_planes));
    }
    // Commit ownership only after every allocation/copy succeeds. Before this
    // point PoolInfo remains the sole owner, so destructor cleanup is exact.
    pool_.ids.clear();
    pool_.fds.clear();
    shared_ = std::move(new_shared);
    LOG_INFO("StreamPreprocess: pool %ux%u NV12, %zu slot(s) ready",
             (unsigned)width_, (unsigned)height_, shared_->slots.size());
    return 0;
}

std::shared_ptr<StreamPreprocessPool::SlotLease>
StreamPreprocessPool::prepare(const ReceivedFrame& frame, uint64_t* dsp_us) {
    if (dsp_us) *dsp_us = 0;
    if (!usable_ || !shared_) return nullptr;
    auto pool_owner = weak_from_this().lock();
    if (!pool_owner) return nullptr;

    // Allocate all lease state before reserving a slot. In particular, copying
    // the FD vector can throw; the slot must not become permanently busy if it
    // does.
    auto impl = std::make_shared<SlotLease::Impl>();
    auto lease = std::make_shared<SlotLease>();
    impl->shared     = shared_;
    impl->num_planes = pool_.num_planes;
    for (uint32_t p = 0; p < 3; ++p) impl->strides[p] = pool_.strides[p];
    impl->src_w      = frame.width;
    impl->src_h      = frame.height;

    uint32_t idx = 0;
    {
        std::lock_guard lock(shared_->mu);
        if (shared_->destroyed) return nullptr;
        bool found = false;
        for (size_t i = 0; i < shared_->slots.size(); ++i) {
            auto& slot = shared_->slots[i];
            if (!slot.in_use && !slot.poisoned) {
                impl->fds = slot.fds;  // borrowed (never closed by the lease)
                slot.in_use = true;
                idx = static_cast<uint32_t>(i);
                found = true;
                break;
            }
        }
        if (!found) return nullptr;  // all busy → keep direct DMA input
    }

    impl->slot = idx;
    lease->impl = std::move(impl);
    lease->pool_owner = std::move(pool_owner);

    auto poison_slot = [&](uint32_t slot) {
        std::lock_guard lock(shared_->mu);
        auto& s = shared_->slots[slot];
        if (!s.poisoned) {
            s.poisoned = true;
            if (shared_->healthy_slots > 0) --shared_->healthy_slots;
        }
        if (shared_->healthy_slots == 0) usable_ = false;
    };
    auto poison_all = [&]() {
        std::lock_guard lock(shared_->mu);
        for (auto& s : shared_->slots) s.poisoned = true;
        shared_->healthy_slots = 0;
        usable_ = false;
    };

    uint64_t import_id = 0;
    uint64_t import_generation = 0;
    if (dsp_.import_frame(frame, &import_id, &import_generation) != 0) {
        lease.reset();  // ~SlotLease frees the slot
        return nullptr;
    }
    // A reconnect reaps every destination id from the allocation generation.
    // Do not submit a job against, or later return, such a revoked slot.
    if (import_generation != shared_->uds_generation) {
        LOG_WARN("StreamPreprocess: UDS generation changed (pool=%lu import=%lu); "
                 "retiring all destination slots",
                 (unsigned long)shared_->uds_generation,
                 (unsigned long)import_generation);
        dsp_.release_buffer(import_id, import_generation);
        poison_all();
        lease.reset();
        return nullptr;
    }
    // The daemon dup'd the frame fds inside the import; from here on the
    // source frame is free to be returned to the camera pool by the caller.

    const uint64_t t0 = steady_now_us();
    int err = 0;
    std::string msg;
    const uint64_t dst_id = shared_->slots[idx].id;  // this lease owns the slot
    const int rc = dsp_.resize(import_id, dst_id, DspClient::INTERP_BILINEAR,
                               DspClient::PRIO_BACKGROUND, job_timeout_ms_,
                               &err, &msg);
    const uint64_t us = steady_now_us() - t0;
    const int release_rc =
        dsp_.release_buffer(import_id, import_generation);

    // A failed release closes (or observes replacement of) the owning UDS.
    // Daemon disconnect cleanup revokes the whole destination pool generation,
    // including a slot whose resize RPC just succeeded.
    if (release_rc != DspClient::RELEASE_OK) {
        LOG_WARN("StreamPreprocess: import release failed (id=%lu generation=%lu "
                 "rc=%d); retiring all destination slots",
                 (unsigned long)import_id, (unsigned long)import_generation,
                 release_rc);
        poison_all();
        lease.reset();
        return nullptr;
    }

    if (rc != 0) {
        LOG_WARN("StreamPreprocess: resize failed (frame %ux%u -> %ux%u): "
                 "rc=%d err=%d (%s)",
                 (unsigned)frame.width, (unsigned)frame.height,
                 (unsigned)width_, (unsigned)height_, rc, err, msg.c_str());
        // err==-2 unknown id: pool ids are stale (daemon restarted) — every
        //      future job would fail too, degrade to the direct DMA path.
        // err==-4 job timeout: dst undefined, a straggler write is still
        //      possible — retire the slot.
        // rc==-1 transport: the job may reach the daemon after our deadline
        //      — retire the slot and degrade (channel suspect).
        if (err == -2) {
            poison_all();
        } else if (err == -4 || rc == -1) {
            poison_slot(idx);
        }
        if (rc == -1) usable_ = false;
        lease.reset();  // ~SlotLease frees the slot (poisoned slots stay out)
        return nullptr;
    }

    if (dsp_us) *dsp_us = us;
    return lease;
}

bool StreamPreprocessPool::bind(HalInferenceSession* sess, SlotLease* lease,
                                HalTensor* out) {
    if (!sess || !lease || !lease->impl || !lease->impl->shared || !out)
        return false;
    const HalInferenceOps* ops = mgr_->infer_ops();
    if (!ops || !ops->bind_dma_frame || !ops->free_tensor) return false;
    if (lease->impl->fds.size() < lease->impl->num_planes) return false;

    HalDmaFrameDesc desc{};
    desc.format   = HAL_PIX_FMT_NV12;
    desc.width    = width_;
    desc.height   = height_;
    desc.borrowed = 1;  // fds stay owned by the pool/Shared
    const uint32_t rows[HAL_MAX_PLANES] = {height_, height_ / 2, 0};
    for (uint32_t p = 0; p < lease->impl->num_planes && p < HAL_MAX_PLANES; p++) {
        desc.fd[p]         = lease->impl->fds[p];
        desc.offset[p]     = 0;
        desc.stride[p]     = lease->impl->strides[p];
        desc.bytes_used[p] =
            static_cast<uint64_t>(lease->impl->strides[p]) * rows[p];
    }
    for (uint32_t p = lease->impl->num_planes < HAL_MAX_PLANES
                            ? lease->impl->num_planes : HAL_MAX_PLANES;
         p < HAL_MAX_PLANES; p++)
        desc.fd[p] = -1;

    if (ops->bind_dma_frame(sess, &desc, &lease->impl->bound) != HAL_OK)
        return false;  // off-contract: direct DMA fallback; slot remains valid

    lease->impl->free_tensor = ops->free_tensor;
    lease->impl->bound_valid = true;
    *out = lease->impl->bound;

    bool log_engaged = false;
    {
        std::lock_guard lock(lease->impl->shared->mu);
        if (!lease->impl->shared->engaged_logged) {
            lease->impl->shared->engaged_logged = true;
            log_engaged = true;
        }
    }
    if (log_engaged) {
        LOG_INFO("stream dsp preprocess engaged (%ux%u -> %ux%u via %zu-slot "
                 "pool, zero repack)",
                 (unsigned)lease->impl->src_w, (unsigned)lease->impl->src_h,
                 (unsigned)width_, (unsigned)height_,
                 lease->impl->shared->slots.size());
    }
    return true;
}

StreamPreprocessPool::SlotLease::~SlotLease() {
    if (!impl || !impl->shared) return;
    if (impl->bound_valid && impl->free_tensor)
        impl->free_tensor(&impl->bound);
    auto sh = impl->shared;
    std::lock_guard lock(sh->mu);
    auto& s = sh->slots[impl->slot];
    s.in_use = false;
    if (sh->destroyed && !s.reclaimed) {
        // Pool destruction already released the daemon id while DspClient was
        // alive. The late lease owns only these local plane fd dups.
        s.reclaimed = true;
        for (int fd : s.fds) ::close(fd);
        s.fds.clear();
    }
}

}  // namespace aipc::ai_runtime
