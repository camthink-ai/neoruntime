/**
 * @file dsp_service.cpp
 * @brief Implementation of the app-facing DSP job service (PLAT-1/4/5).
 *
 * Locking model (independent guards; buffers_mu_ is NEVER held while
 * calling into HAL — allocs/frees run on collected lists after unlock,
 * and unpin_entries takes buffers_mu_ itself):
 *  - buffers_mu_ : registry maps + BufferEntry pin/detach state
 *  - q_mu_       : job deques            - done_mu_  : job done/abandoned
 *  - quota_mu_   : token buckets         - stats_mu_ : counters
 *
 * Nested locking paths are lifecycle_mu_ -> buffers_mu_/q_mu_,
 * buffers_mu_ -> stats_mu_/done_mu_, and done_mu_ -> q_mu_. Async registration
 * uses buffers_mu_ -> done_mu_ to prove its pinned owner is still attached,
 * then done_mu_ -> q_mu_ to make disconnect-versus-enqueue atomic. Disconnect
 * cancellation takes q_mu_ and done_mu_ in separate critical sections.
 *
 * CPU coherency: the daemon never CPU-touches registered buffers, so it
 * does no DMA_BUF_IOCTL_SYNC itself. The DMA_BUF_IOCTL_SYNC discipline
 * (write-fence after CPU fill, read-fence before CPU read) is part of the
 * client-side contract — see docs/proposals/dsp-offload.md (HAL-3).
 */

#include "dsp_service.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <utility>
#include <unistd.h> /* dup, close, read, pread/pwrite */
#include <sys/mman.h> /* mmap/munmap (USERPTR imports) */
#include <sys/random.h> /* getrandom (opaque buffer/job handles) */
#include <sys/stat.h>  /* fstat backing-size check for USERPTR imports */
#include <fcntl.h>     /* fcntl, F_GET_SEALS/F_ADD_SEALS (memfd seals) */
#include <sys/syscall.h> /* SYS_memfd_create (no _GNU_SOURCE in this TU) */
#include <linux/memfd.h> /* MFD_*, F_SEAL_* */
#include <sys/socket.h> /* getsockopt / SO_PEERCRED (quota process identity) */

#include "common/hal_log.h"

namespace {

constexpr uint32_t kMinDim = 16;
constexpr uint32_t kMaxDim = 8192;
/* SCM_RIGHTS wire cap on the UDS alloc response: count*num_planes fds. */
constexpr uint32_t kMaxAllocFds = 64;

template <typename T>
bool cap_exceeded(T current, T added, T cap) {
    return added > cap || current > cap - added;
}

void set_message_noexcept(std::string& message, const char* text) noexcept {
    try {
        message = text;
    } catch (...) {
        message.clear();
    }
}

template <typename Fn>
class ScopeExit {
public:
    explicit ScopeExit(Fn fn) : fn_(std::move(fn)) {}
    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;
    ~ScopeExit() noexcept { if (armed_) fn_(); }
    void disarm() noexcept { armed_ = false; }

private:
    Fn fn_;
    bool armed_ = true;
};

template <typename Fn>
ScopeExit<Fn> make_scope_exit(Fn fn) {
    return ScopeExit<Fn>(std::move(fn));
}

bool read_client_file_identity(int fd, uint64_t& device,
                               uint64_t& inode) noexcept {
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) != 0) return false;
    device = static_cast<uint64_t>(st.st_dev);
    inode = static_cast<uint64_t>(st.st_ino);
    return true;
}

bool read_process_start_time(int pid, uint64_t& start_time_ticks) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    std::ifstream stat_file(path);
    std::string stat_line;
    if (!std::getline(stat_file, stat_line)) return false;

    /* Field 2 (comm) is parenthesized and may contain spaces or ')'. The final
     * ')' closes it; field 3 follows, and starttime is field 22. */
    const size_t comm_end = stat_line.rfind(')');
    if (comm_end == std::string::npos || comm_end + 1 >= stat_line.size())
        return false;

    std::istringstream fields(stat_line.substr(comm_end + 1));
    std::string ignored;
    for (int field = 3; field < 22; ++field) {
        if (!(fields >> ignored)) return false;
    }
    return static_cast<bool>(fields >> start_time_ticks);
}

#ifdef DSP_SERVICE_TESTING
std::atomic<bool> force_random_id_failure{false};
#endif

/* Kernel-CSPRNG draw for opaque buffer/job handles. Return 0 on a persistent
 * getrandom failure; callers translate that to a service allocation error.
 * EINTR is retried, and live-map collisions are re-drawn by the caller. */
uint64_t fresh_random_id() noexcept {
#ifdef DSP_SERVICE_TESTING
    if (force_random_id_failure.load()) return 0;
#endif
    uint64_t id = 0;
    size_t offset = 0;
    while (offset < sizeof(id)) {
        const ssize_t n = ::getrandom(
            reinterpret_cast<unsigned char*>(&id) + offset,
            sizeof(id) - offset, 0);
        if (n > 0) {
            offset += static_cast<size_t>(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return 0;
    }
    return id;
}

bool format_supported(HalPixelFormat f) {
    switch (f) {
    case HAL_PIX_FMT_NV12:
    case HAL_PIX_FMT_RGB24:
    case HAL_PIX_FMT_GRAY8:
    case HAL_PIX_FMT_ARGB32: /* P1: blend overlays */
        return true;
    default:
        return false;
    }
}

/* Plane count for the P0 format set (mirrors hal_pixel_format_plane_count,
 * which lives in hal_v2/common/hal_buffer.c — not linked into the daemon). */
uint32_t plane_count_of(HalPixelFormat f) {
    return (f == HAL_PIX_FMT_NV12) ? 2 : 1;
}

/* Minimum sane stride for plane p of a w*h `format` buffer (no padding). */
uint32_t min_stride_of(HalPixelFormat f, uint32_t w) {
    if (f == HAL_PIX_FMT_RGB24) return w * 3;
    if (f == HAL_PIX_FMT_ARGB32) return w * 4;
    return w; /* NV12: Y row and interleaved-UV row are both w bytes */
}

/* Rows in plane p (NV12 chroma is half height). */
uint32_t plane_rows_of(HalPixelFormat f, uint32_t h, uint32_t plane) {
    if (f == HAL_PIX_FMT_NV12 && plane == 1) return h / 2;
    return h;
}

bool minimum_pool_bytes(uint32_t width, uint32_t height,
                        HalPixelFormat format, uint32_t max_buffers,
                        uint64_t& bytes_out) noexcept {
    uint64_t bytes_per_buffer = 0;
    const uint32_t planes = plane_count_of(format);
    for (uint32_t plane = 0; plane < planes; ++plane) {
        const uint64_t plane_bytes =
            static_cast<uint64_t>(min_stride_of(format, width)) *
            plane_rows_of(format, height, plane);
        if (plane_bytes > UINT64_MAX - bytes_per_buffer) return false;
        bytes_per_buffer += plane_bytes;
    }
    if (max_buffers == 0 || bytes_per_buffer > UINT64_MAX / max_buffers)
        return false;
    bytes_out = bytes_per_buffer * max_buffers;
    return true;
}

/* Imported descriptors are plain daemon-owned allocations, not HAL pool
 * buffers: releasing one is close(dup'd fds) + munmap(mapped planes) +
 * delete. They must never reach fb_ops_->release_frame_buffer. */
void free_imported_fb(HalFrameBuffer* fb) noexcept {
    if (!fb) return;
    for (uint32_t p = 0; p < HAL_MAX_PLANES; ++p) {
        if (fb->mem_type == HAL_MEM_MALLOC && fb->planes[p])
            munmap(fb->planes[p], fb->sizes[p]);
        if (fb->dma_fds[p] >= 0) close(fb->dma_fds[p]);
    }
    delete fb;
}

/* Copies exactly `size` bytes from src_fd into a fresh daemon-owned memfd
 * sealed against shrink/grow/write and returns its fd; -1 on short read or
 * allocation failure. Guards the USERPTR import path: the client keeps a
 * writable handle to its buffer, so a mapping of an unsealed file can be
 * truncated under us — the DSP would then read past EOF and raise a
 * process-wide SIGBUS. Reading through read(2) returns 0 at EOF instead,
 * so the copy fails safely. */
int copy_to_sealed_memfd(int src_fd, uint32_t size) {
    int mfd = static_cast<int>(syscall(SYS_memfd_create, "dsp-import-copy",
                                       MFD_CLOEXEC | MFD_ALLOW_SEALING));
    if (mfd < 0) return -1;
    if (ftruncate(mfd, static_cast<off_t>(size)) != 0) {
        close(mfd);
        return -1;
    }
    char buf[65536];
    off_t off = 0;
    while (off < static_cast<off_t>(size)) {
        const size_t remaining =
            static_cast<size_t>(static_cast<off_t>(size) - off);
        const size_t chunk = std::min(sizeof(buf), remaining);
        ssize_t n = pread(src_fd, buf, chunk, off);
        if (n <= 0) { /* EOF short of the declared size, or read error */
            close(mfd);
            return -1;
        }
        ssize_t written = 0;
        while (written < n) {
            ssize_t m = pwrite(mfd, buf + written,
                               static_cast<size_t>(n - written), off + written);
            if (m <= 0) {
                close(mfd);
                return -1;
            }
            written += m;
        }
        off += n;
    }
    /* Seal after the copy: no writable mapping exists, so shrink (the
     * SIGBUS vector), grow and writes are all locked out for good. */
    if (fcntl(mfd, F_ADD_SEALS,
              F_SEAL_SEAL | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE) != 0) {
        close(mfd);
        return -1;
    }
    return mfd;
}

/* DMA-BUF fdinfo is emitted by the kernel's dma-buf file operations and
 * includes both exporter identity and the retained backing size. Parsing the
 * kernel-owned fields avoids trusting a spoofable symlink/file name. */
bool dma_buf_capacity(int fd, uint64_t& capacity_out) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", fd);
    const int info_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (info_fd < 0) return false;

    char info[4096];
    const ssize_t n = read(info_fd, info, sizeof(info) - 1);
    close(info_fd);
    if (n <= 0) return false;
    info[n] = '\0';

    const char* exp_name = std::strstr(info, "exp_name:");
    const char* size = std::strstr(info, "size:");
    if (!exp_name || !size) return false;
    if (exp_name != info && exp_name[-1] != '\n') return false;
    if (size != info && size[-1] != '\n') return false;

    unsigned long long capacity = 0;
    if (std::sscanf(size, "size:\t%llu", &capacity) != 1 || capacity == 0)
        return false;
    capacity_out = static_cast<uint64_t>(capacity);
    return true;
}

} // namespace

size_t DspService::QuotaKeyHash::operator()(const QuotaKey& key) const noexcept {
    auto combine = [](size_t seed, size_t value) {
        return seed ^ (value + static_cast<size_t>(0x9e3779b9U) +
                       (seed << 6) + (seed >> 2));
    };

    size_t hash = std::hash<uint8_t>{}(static_cast<uint8_t>(key.kind));
    if (key.kind == QuotaKey::Kind::Process) {
        hash = combine(hash, std::hash<int>{}(key.pid));
        return combine(hash,
                       std::hash<uint64_t>{}(key.process_start_time_ticks));
    }
    return combine(hash, std::hash<int>{}(key.legacy_fd));
}

size_t DspService::ClientRegistrationHash::operator()(
    const ClientRegistration& registration) const noexcept {
    size_t hash = std::hash<int>{}(registration.fd);
    return hash ^ (std::hash<uint64_t>{}(registration.generation) +
                   static_cast<size_t>(0x9e3779b9U) + (hash << 6) +
                   (hash >> 2));
}

size_t DspService::BufferPoolKeyHash::operator()(
    const BufferPoolKey& key) const noexcept {
    auto combine = [](size_t seed, size_t value) {
        return seed ^ (value + static_cast<size_t>(0x9e3779b9U) +
                       (seed << 6) + (seed >> 2));
    };
    size_t hash = std::hash<uint32_t>{}(key.width);
    hash = combine(hash, std::hash<uint32_t>{}(key.height));
    hash = combine(hash, std::hash<int>{}(static_cast<int>(key.format)));
    hash = combine(hash, std::hash<uint32_t>{}(key.max_buffers));
    return combine(hash, std::hash<uint64_t>{}(key.bytes_per_buffer));
}

size_t DspService::BufferPoolOwnerKeyHash::operator()(
    const BufferPoolOwnerKey& key) const noexcept {
    const size_t owner_hash = QuotaKeyHash{}(key.owner);
    const size_t pool_hash = BufferPoolKeyHash{}(key.pool);
    return owner_hash ^ (pool_hash + static_cast<size_t>(0x9e3779b9U) +
                         (owner_hash << 6) + (owner_hash >> 2));
}

DspService::DspService(HalDspOps* dsp_ops, HalFrameBufferOps* fb_ops,
                       const DspServiceConfig& cfg)
    : dsp_ops_(dsp_ops), fb_ops_(fb_ops), cfg_(cfg) {}

DspService::~DspService() noexcept { stop(); }

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

bool DspService::start() {
    std::unique_lock<std::mutex> lifecycle_lk(lifecycle_mu_);
    if (lifecycle_state_ == LifecycleState::Running) return true;
    if (lifecycle_state_ == LifecycleState::Stopping) return false;
    if (!dsp_ops_ || !fb_ops_) {
        HAL_LOG_ERROR("DspService: null ops table (dsp=%p fb=%p)", dsp_ops_,
                      fb_ops_);
        return false;
    }

    // Same context recipe as dpm_worker (E1 proved contexts coexist; the
    // vendor PriorityQueueSingleton serializes hardware access anyway).
    HalDspConfig dcfg{};
    dcfg.device_priority = 0;
    if (dsp_ops_->init(&dcfg, &dsp_ctx_) != 0 || !dsp_ctx_) {
        HAL_LOG_ERROR("DspService: HAL DSP init failed — SubmitDspJob unavailable");
        dsp_ctx_ = nullptr;
        return false;
    }

    running_ = true;
    try {
        worker_ = std::thread(&DspService::worker_loop, this);
    } catch (...) {
        running_ = false;
        dsp_ops_->deinit(dsp_ctx_);
        dsp_ctx_ = nullptr;
        HAL_LOG_ERROR("DspService: worker thread creation failed");
        return false;
    }
    lifecycle_state_ = LifecycleState::Running;
    HAL_LOG_INFO(
        "DspService: started (max_batch=%u quota=%.0f jobs/s %.0f MPix/s "
        "timeout=%ums)",
        cfg_.max_batch, cfg_.quota_jobs_per_sec, cfg_.quota_mpix_per_sec,
        cfg_.job_timeout_ms);
    return true;
}

void DspService::stop() {
    {
        std::unique_lock<std::mutex> lifecycle_lk(lifecycle_mu_);
        if (lifecycle_state_ == LifecycleState::Stopped) return;
        if (lifecycle_state_ == LifecycleState::Stopping) {
            lifecycle_cv_.wait(lifecycle_lk, [this] {
                return lifecycle_state_ == LifecycleState::Stopped;
            });
            return;
        }

        lifecycle_state_ = LifecycleState::Stopping;
        {
            // q_mu_ is the worker wait predicate's synchronization domain.
            // Publish shutdown under that lock so notify cannot be lost between
            // predicate evaluation and condition-variable sleep.
            std::lock_guard<std::mutex> q_lk(q_mu_);
            running_ = false;
        }
        // Existing wait_job calls are lifecycle users too. Wake them before
        // waiting for the lifecycle count so queued jobs cannot hold stop until
        // their client-supplied timeout expires.
        done_cv_.notify_all();
        lifecycle_cv_.wait(lifecycle_lk, [this] {
            return active_async_submissions_ == 0 &&
                   active_buffer_registrations_ == 0 &&
                   active_buffer_pins_ == 0;
        });
    }

    q_cv_.notify_all();
    if (worker_.joinable()) worker_.join();

    // Drain without staging vectors: stop/destructor cleanup must not allocate.
    for (;;) {
        JobRef job;
        {
            std::lock_guard<std::mutex> lk(q_mu_);
            if (!q_normal_.empty()) {
                job = std::move(q_normal_.front());
                q_normal_.pop_front();
            } else if (!q_background_.empty()) {
                job = std::move(q_background_.front());
                q_background_.pop_front();
            } else {
                break;
            }
        }
        job->result.rc = DSP_SVC_ERR_UNAVAILABLE;
        unpin_entries(job->pinned);
        job->pinned.clear();
        {
            std::lock_guard<std::mutex> lk(done_mu_);
            job->done = true;
        }
        done_cv_.notify_all();
    }

    {
        std::lock_guard<std::mutex> lk(done_mu_);
        jobs_.clear();
        client_async_jobs_.clear();
        total_async_jobs_ = 0;
    }

    for (;;) {
        DetachedFrame frame;
        {
            std::lock_guard<std::mutex> lk(buffers_mu_);
            if (buffers_.empty()) break;
            frame = detach_entry_locked(buffers_.begin()->second);
        }
        release_detached_frame(fb_ops_, frame);
    }
    {
        std::lock_guard<std::mutex> lk(buffers_mu_);
        retained_usage_.clear();
        buffer_pools_.clear();
        buffer_pool_owner_refs_.clear();
        total_buffers_ = 0;
        total_buffer_bytes_ = 0;
        pending_buffers_ = 0;
        pending_buffer_bytes_ = 0;
        total_imports_ = 0;
        total_import_bytes_ = 0;
    }

    if (dsp_ctx_) {
        dsp_ops_->deinit(dsp_ctx_);
        dsp_ctx_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mu_);
        client_sessions_.clear();
        lifecycle_state_ = LifecycleState::Stopped;
    }
    lifecycle_cv_.notify_all();
    HAL_LOG_INFO("DspService: stopped");
}

/* ------------------------------------------------------------------ */
/* Buffer plane                                                        */
/* ------------------------------------------------------------------ */

DspService::QuotaKey DspService::resource_owner_key(
    const QuotaKey& quota_key) const {
    if (quota_key.kind == QuotaKey::Kind::Process) return quota_key;
    // If SO_PEERCRED identity is unavailable, all such connections share one
    // conservative bucket so reconnecting cannot reset retained-resource caps.
    return QuotaKey::for_legacy_fd(-1);
}

int DspService::reserve_buffer_admission(
    const QuotaKey& resource_key, uint32_t width, uint32_t height,
    HalPixelFormat format, uint32_t max_buffers, uint32_t count,
    BufferAdmission& admission, std::string& why) noexcept {
    uint64_t estimated_pool_bytes = 0;
    if (!minimum_pool_bytes(width, height, format, max_buffers,
                            estimated_pool_bytes)) {
        set_message_noexcept(why, "buffer pool admission-byte overflow");
        return DSP_SVC_ERR_LIMIT;
    }

    std::lock_guard<std::mutex> lk(buffers_mu_);
    bool inserted_usage = false;
    try {
        auto usage_result = retained_usage_.try_emplace(resource_key);
        auto usage_it = usage_result.first;
        inserted_usage = usage_result.second;
        RetainedUsage& usage = usage_it->second;

        uint64_t existing_global_bytes = 0;
        uint64_t existing_owner_bytes = 0;
        for (const auto& pool_item : buffer_pools_) {
            const BufferPoolKey& key = pool_item.first;
            if (key.width != width || key.height != height ||
                key.format != format || key.max_buffers != max_buffers)
                continue;
            existing_global_bytes = std::max(
                existing_global_bytes, pool_item.second.retained_bytes);
            const BufferPoolOwnerKey owner_pool{resource_key, key};
            if (buffer_pool_owner_refs_.count(owner_pool) != 0)
                existing_owner_bytes = std::max(
                    existing_owner_bytes, pool_item.second.retained_bytes);
        }

        const uint64_t owner_base =
            usage.buffer_bytes > existing_owner_bytes
                ? usage.buffer_bytes - existing_owner_bytes
                : 0;
        const uint64_t total_base =
            total_buffer_bytes_ > existing_global_bytes
                ? total_buffer_bytes_ - existing_global_bytes
                : 0;
        const uint64_t owner_count =
            static_cast<uint64_t>(usage.buffers) + usage.pending_buffers;
        const uint64_t total_count =
            static_cast<uint64_t>(total_buffers_) + pending_buffers_;

        const char* limit = nullptr;
        if (owner_count + count > cfg_.max_buffers_per_client)
            limit = "per-process buffer count cap exceeded";
        else if (cap_exceeded(owner_base, usage.pending_buffer_bytes,
                              cfg_.max_client_pixels) ||
                 cap_exceeded(owner_base + usage.pending_buffer_bytes,
                              estimated_pool_bytes, cfg_.max_client_pixels))
            limit = "per-process retained buffer byte cap exceeded";
        else if (total_count + count > cfg_.max_total_buffers)
            limit = "service buffer count cap exceeded";
        else if (cap_exceeded(total_base, pending_buffer_bytes_,
                              cfg_.max_total_buffer_pixels) ||
                 cap_exceeded(total_base + pending_buffer_bytes_,
                              estimated_pool_bytes,
                              cfg_.max_total_buffer_pixels))
            limit = "service retained buffer byte cap exceeded";
        if (limit) {
            if (inserted_usage) retained_usage_.erase(usage_it);
            set_message_noexcept(why, limit);
            return DSP_SVC_ERR_LIMIT;
        }

        usage.pending_buffers += count;
        usage.pending_buffer_bytes += estimated_pool_bytes;
        pending_buffers_ += count;
        pending_buffer_bytes_ += estimated_pool_bytes;
        admission.owner = resource_key;
        admission.buffers = count;
        admission.owner_bytes = estimated_pool_bytes;
        admission.total_bytes = estimated_pool_bytes;
        admission.active = true;
        return DSP_SVC_OK;
    } catch (...) {
        if (inserted_usage) retained_usage_.erase(resource_key);
        set_message_noexcept(why, "buffer admission accounting failed");
        return DSP_SVC_ERR_NO_MEM;
    }
}

void DspService::release_buffer_admission_locked(
    BufferAdmission& admission) noexcept {
    if (!admission.active) return;
    auto usage_it = retained_usage_.find(admission.owner);
    if (usage_it != retained_usage_.end()) {
        RetainedUsage& usage = usage_it->second;
        usage.pending_buffers = usage.pending_buffers > admission.buffers
                                    ? usage.pending_buffers - admission.buffers
                                    : 0;
        usage.pending_buffer_bytes =
            usage.pending_buffer_bytes > admission.owner_bytes
                ? usage.pending_buffer_bytes - admission.owner_bytes
                : 0;
        if (usage.buffers == 0 && usage.buffer_bytes == 0 &&
            usage.pending_buffers == 0 && usage.pending_buffer_bytes == 0 &&
            usage.imports == 0 && usage.import_bytes == 0)
            retained_usage_.erase(usage_it);
    }
    pending_buffers_ = pending_buffers_ > admission.buffers
                           ? pending_buffers_ - admission.buffers
                           : 0;
    pending_buffer_bytes_ = pending_buffer_bytes_ > admission.total_bytes
                                ? pending_buffer_bytes_ - admission.total_bytes
                                : 0;
    admission.active = false;
}

void DspService::release_buffer_admission(
    BufferAdmission& admission) noexcept {
    std::lock_guard<std::mutex> lk(buffers_mu_);
    release_buffer_admission_locked(admission);
}

int DspService::reserve_buffer_usage_locked(
    const QuotaKey& resource_key, const BufferPoolKey& pool_key,
    uint32_t count, std::string& why) noexcept {
    if (pool_key.max_buffers == 0 ||
        pool_key.bytes_per_buffer > UINT64_MAX / pool_key.max_buffers) {
        set_message_noexcept(why, "buffer pool retained-byte overflow");
        return DSP_SVC_ERR_LIMIT;
    }
    const uint64_t pool_bytes =
        pool_key.bytes_per_buffer * pool_key.max_buffers;
    const BufferPoolOwnerKey owner_pool{resource_key, pool_key};

    bool inserted_usage = false;
    bool inserted_pool = false;
    bool inserted_owner_pool = false;
    try {
        auto usage_result = retained_usage_.try_emplace(resource_key);
        auto usage_it = usage_result.first;
        inserted_usage = usage_result.second;
        RetainedUsage& usage = usage_it->second;

        const auto pool_it = buffer_pools_.find(pool_key);
        const auto owner_pool_it = buffer_pool_owner_refs_.find(owner_pool);
        const uint64_t owner_added_bytes =
            owner_pool_it == buffer_pool_owner_refs_.end() ? pool_bytes : 0;
        const uint64_t total_added_bytes =
            pool_it == buffer_pools_.end() ? pool_bytes : 0;

        const char* limit = nullptr;
        if (cap_exceeded(usage.buffers, count, cfg_.max_buffers_per_client))
            limit = "per-process buffer count cap exceeded";
        else if (cap_exceeded(usage.buffer_bytes, owner_added_bytes,
                              cfg_.max_client_pixels))
            limit = "per-process retained buffer byte cap exceeded";
        else if (cap_exceeded(total_buffers_, count, cfg_.max_total_buffers))
            limit = "service buffer count cap exceeded";
        else if (cap_exceeded(total_buffer_bytes_, total_added_bytes,
                              cfg_.max_total_buffer_pixels))
            limit = "service retained buffer byte cap exceeded";
        if (limit) {
            if (inserted_usage) retained_usage_.erase(usage_it);
            set_message_noexcept(why, limit);
            return DSP_SVC_ERR_LIMIT;
        }

        auto pool_result = buffer_pools_.try_emplace(
            pool_key, BufferPoolUsage{0, pool_bytes});
        inserted_pool = pool_result.second;
        auto owner_result = buffer_pool_owner_refs_.try_emplace(owner_pool, 0);
        inserted_owner_pool = owner_result.second;

        usage.buffers += count;
        total_buffers_ += count;
        if (inserted_owner_pool) usage.buffer_bytes += pool_bytes;
        if (inserted_pool) total_buffer_bytes_ += pool_bytes;
        pool_result.first->second.refs += count;
        owner_result.first->second += count;
        return DSP_SVC_OK;
    } catch (...) {
        if (inserted_owner_pool) buffer_pool_owner_refs_.erase(owner_pool);
        if (inserted_pool) buffer_pools_.erase(pool_key);
        if (inserted_usage) {
            auto it = retained_usage_.find(resource_key);
            if (it != retained_usage_.end() && it->second.buffers == 0 &&
                it->second.buffer_bytes == 0 &&
                it->second.pending_buffers == 0 &&
                it->second.pending_buffer_bytes == 0 &&
                it->second.imports == 0 && it->second.import_bytes == 0)
                retained_usage_.erase(it);
        }
        set_message_noexcept(why, "resource accounting allocation failed");
        return DSP_SVC_ERR_NO_MEM;
    }
}

int DspService::reserve_import_usage(const QuotaKey& resource_key,
                                     uint64_t bytes,
                                     std::string& why) noexcept {
    std::lock_guard<std::mutex> lk(buffers_mu_);
    try {
        auto [it, inserted] = retained_usage_.try_emplace(resource_key);
        RetainedUsage& usage = it->second;
        const char* limit = nullptr;
        if (cap_exceeded(usage.imports, uint32_t{1},
                         cfg_.max_imports_per_client))
            limit = "per-process import count cap exceeded";
        else if (cap_exceeded(usage.import_bytes, bytes,
                              cfg_.max_import_bytes_per_client))
            limit = "per-process import byte cap exceeded";
        else if (cap_exceeded(total_imports_, uint32_t{1},
                              cfg_.max_total_imports))
            limit = "service import count cap exceeded";
        else if (cap_exceeded(total_import_bytes_, bytes,
                              cfg_.max_total_import_bytes))
            limit = "service import byte cap exceeded";
        if (limit) {
            if (inserted) retained_usage_.erase(it);
            set_message_noexcept(why, limit);
            return DSP_SVC_ERR_LIMIT;
        }
        ++usage.imports;
        usage.import_bytes += bytes;
        ++total_imports_;
        total_import_bytes_ += bytes;
        return DSP_SVC_OK;
    } catch (...) {
        set_message_noexcept(why, "resource accounting allocation failed");
        return DSP_SVC_ERR_NO_MEM;
    }
}

void DspService::release_buffer_usage_locked(
    const QuotaKey& resource_key, const BufferPoolKey& pool_key,
    uint32_t count) noexcept {
    auto usage_it = retained_usage_.find(resource_key);
    auto pool_it = buffer_pools_.find(pool_key);
    const BufferPoolOwnerKey owner_pool{resource_key, pool_key};
    auto owner_it = buffer_pool_owner_refs_.find(owner_pool);
    if (usage_it == retained_usage_.end() || pool_it == buffer_pools_.end() ||
        owner_it == buffer_pool_owner_refs_.end())
        return;

    RetainedUsage& usage = usage_it->second;
    const uint32_t released = std::min(count, owner_it->second);
    usage.buffers = usage.buffers > released ? usage.buffers - released : 0;
    total_buffers_ = total_buffers_ > released ? total_buffers_ - released : 0;
    owner_it->second -= released;
    pool_it->second.refs = pool_it->second.refs > released
                               ? pool_it->second.refs - released
                               : 0;

    if (owner_it->second == 0) {
        usage.buffer_bytes = usage.buffer_bytes > pool_it->second.retained_bytes
                                 ? usage.buffer_bytes -
                                       pool_it->second.retained_bytes
                                 : 0;
        buffer_pool_owner_refs_.erase(owner_it);
    }
    if (pool_it->second.refs == 0) {
        total_buffer_bytes_ =
            total_buffer_bytes_ > pool_it->second.retained_bytes
                ? total_buffer_bytes_ - pool_it->second.retained_bytes
                : 0;
        buffer_pools_.erase(pool_it);
    }
    if (usage.buffers == 0 && usage.buffer_bytes == 0 &&
        usage.pending_buffers == 0 && usage.pending_buffer_bytes == 0 &&
        usage.imports == 0 && usage.import_bytes == 0)
        retained_usage_.erase(usage_it);
}

void DspService::release_import_usage(const QuotaKey& resource_key,
                                      uint32_t count, uint64_t bytes) noexcept {
    std::lock_guard<std::mutex> lk(buffers_mu_);
    auto it = retained_usage_.find(resource_key);
    if (it == retained_usage_.end()) return;
    RetainedUsage& usage = it->second;
    usage.imports = usage.imports > count ? usage.imports - count : 0;
    usage.import_bytes = usage.import_bytes > bytes
                             ? usage.import_bytes - bytes
                             : 0;
    total_imports_ = total_imports_ > count ? total_imports_ - count : 0;
    total_import_bytes_ = total_import_bytes_ > bytes
                              ? total_import_bytes_ - bytes
                              : 0;
    if (usage.buffers == 0 && usage.buffer_bytes == 0 &&
        usage.pending_buffers == 0 && usage.pending_buffer_bytes == 0 &&
        usage.imports == 0 && usage.import_bytes == 0) {
        retained_usage_.erase(it);
    }
}

void DspService::release_entry_usage_locked(const BufferEntry* entry) noexcept {
    auto it = retained_usage_.find(entry->resource_key);
    if (it == retained_usage_.end()) return;
    RetainedUsage& usage = it->second;
    if (entry->imported) {
        usage.imports = usage.imports > 0 ? usage.imports - 1 : 0;
        usage.import_bytes = usage.import_bytes > entry->retained_import_bytes
                                 ? usage.import_bytes -
                                       entry->retained_import_bytes
                                 : 0;
        total_imports_ = total_imports_ > 0 ? total_imports_ - 1 : 0;
        total_import_bytes_ =
            total_import_bytes_ > entry->retained_import_bytes
                ? total_import_bytes_ - entry->retained_import_bytes
                : 0;
    } else {
        release_buffer_usage_locked(entry->resource_key, entry->pool_key, 1);
        return;
    }
    if (usage.buffers == 0 && usage.buffer_bytes == 0 &&
        usage.pending_buffers == 0 && usage.pending_buffer_bytes == 0 &&
        usage.imports == 0 && usage.import_bytes == 0) {
        retained_usage_.erase(it);
    }
}

DspService::AllocResult DspService::alloc_buffers(
    int client_fd, uint32_t width, uint32_t height, HalPixelFormat format,
    uint32_t count) {
    try {
        return alloc_buffers_impl(client_fd, width, height, format, count);
    } catch (...) {
        AllocResult out;
        out.rc = DSP_SVC_ERR_NO_MEM;
        set_message_noexcept(out.message, "buffer allocation failed");
        return out;
    }
}

DspService::AllocResult DspService::alloc_buffers_impl(
    int client_fd, uint32_t width, uint32_t height, HalPixelFormat format,
    uint32_t count) {
    AllocResult out;
    if (!fb_ops_) {
        out.rc = DSP_SVC_ERR_UNAVAILABLE;
        out.message = "service not running";
        return out;
    }
    ClientRegistration registration;
    out.rc = begin_buffer_registration(client_fd, registration);
    if (out.rc != DSP_SVC_OK) {
        out.message = out.rc == DSP_SVC_ERR_NO_BUFFER
                          ? "client session disconnected"
                          : "service not running";
        return out;
    }
    BufferRegistrationGuard registration_guard(this);
    if (count == 0) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "count must be >= 1";
        return out;
    }
    if (width < kMinDim || height < kMinDim || width > kMaxDim ||
        height > kMaxDim) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "width/height out of range [16, 8192]";
        return out;
    }
    if (!format_supported(format)) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "unsupported format (NV12/RGB24/GRAY8/ARGB32)";
        return out;
    }
    if (format == HAL_PIX_FMT_NV12 && ((width & 1U) || (height & 1U))) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "NV12 width and height must be even";
        return out;
    }
    const uint64_t px = pixels_of(width, height);
    if (px > cfg_.max_pixels_per_op) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "buffer exceeds max_pixels_per_op";
        return out;
    }
    uint32_t planes = plane_count_of(format);
    if (static_cast<uint64_t>(count) * planes > kMaxAllocFds) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "count*num_planes exceeds the 64-fd UDS response cap";
        return out;
    }

    // Capture process incarnation once for quota/resource attribution. The
    // stable client registration separately fences disconnect and fd reuse.
    const QuotaKey quota_key = quota_owner_key(client_fd);
    const QuotaKey resource_key = resource_owner_key(quota_key);

    // HAL allocs outside the registry lock (can be slow).
    HalFrameBufferRequest req{};
    req.width = width;
    req.height = height;
    req.format = format;
    // hal_v2's default app pool is 8 buffers/geometry (hailo15_media_impl.cpp
    // kDefaultMaxBuffers) — too small for MULTI_CROP batches. Size the pool to
    // the fd-plane ceiling: 32 two-plane buffers = FD_PUB_DSP_MAX_FDS(64) fds.
    // Pool key includes the size, so this never touches the pipeline's own
    // pool_max_buffers=0 pools.
    req.pool_max_buffers = 32;
    req.mem_type = HAL_MEM_DMABUF;
    req.zero_initialize = false;

    BufferAdmission admission;
    out.rc = reserve_buffer_admission(
        resource_key, width, height, format, req.pool_max_buffers, count,
        admission, out.message);
    if (out.rc != DSP_SVC_OK) return out;
    auto admission_guard = make_scope_exit([&] {
        release_buffer_admission(admission);
    });

    std::vector<HalFrameBuffer*> fbs;
    try {
        fbs.reserve(count);
    } catch (...) {
        out.rc = DSP_SVC_ERR_NO_MEM;
        out.message = "buffer allocation bookkeeping failed";
        return out;
    }
    auto fb_guard = make_scope_exit([&] {
        for (HalFrameBuffer* fb : fbs) fb_ops_->release_frame_buffer(fb);
    });

    for (uint32_t i = 0; i < count; ++i) {
        HalFrameBuffer* fb = nullptr;
        int rc = fb_ops_->request_frame_buffer(&req, &fb);
        if (rc != 0 || !fb) {
            out.rc = DSP_SVC_ERR_NO_MEM;
            char msg[128];
            std::snprintf(msg, sizeof(msg), "HAL alloc failed at %u/%u (rc=%d)", i,
                          count, rc);
            out.message = msg;
            HAL_LOG_WARNING("DspService: %s", msg);
            return out;
        }
        fbs.push_back(fb);
    }

    uint64_t bytes_per_buffer = 0;
    out.num_planes = fbs[0]->num_planes;
    if (out.num_planes == 0 || out.num_planes > HAL_MAX_PLANES) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "HAL returned an invalid plane count";
        return out;
    }
    for (HalFrameBuffer* fb : fbs) {
        if (fb->num_planes != out.num_planes) {
            out.rc = DSP_SVC_ERR_INVALID;
            out.message = "HAL returned inconsistent plane counts";
            return out;
        }
        uint64_t returned_bytes = 0;
        for (uint32_t p = 0; p < fb->num_planes; ++p) {
            if (fb->sizes[p] > UINT64_MAX - returned_bytes) {
                out.rc = DSP_SVC_ERR_LIMIT;
                out.message = "HAL plane byte size overflow";
                return out;
            }
            returned_bytes += fb->sizes[p];
        }
        bytes_per_buffer = std::max(bytes_per_buffer, returned_bytes);
    }
    if (bytes_per_buffer == 0) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "HAL returned zero retained plane bytes";
        return out;
    }
    const BufferPoolKey pool_key{width, height, format,
                                 req.pool_max_buffers, bytes_per_buffer};

    std::vector<std::unique_ptr<BufferEntry>> entries;
    try {
        out.ids.reserve(count);
        out.fds.reserve(static_cast<size_t>(count) * out.num_planes);
        entries.reserve(count);
        for (HalFrameBuffer* fb : fbs) {
            auto entry = std::make_unique<BufferEntry>();
            entry->client_fd = client_fd;
            entry->client_registration = registration;
            entry->quota_key = quota_key;
            entry->resource_key = resource_key;
            entry->pool_key = pool_key;
            entry->fb = fb;
            entries.push_back(std::move(entry));
        }
    } catch (...) {
        out.rc = DSP_SVC_ERR_NO_MEM;
        out.message = "buffer registry bookkeeping failed";
        return out;
    }

    for (uint32_t p = 0; p < HAL_MAX_PLANES; ++p) {
        out.strides[p] = fbs[0]->strides[p];
        out.sizes[p] = fbs[0]->sizes[p];
    }

#ifdef DSP_SERVICE_TESTING
    try {
        if (before_buffer_register_hook_) before_buffer_register_hook_();
    } catch (...) {
        out.rc = DSP_SVC_ERR_NO_MEM;
        out.message = "buffer registration bookkeeping failed";
        return out;
    }
#endif

    size_t inserted_count = 0;
    {
        std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mu_);
        if (!client_registration_active_locked(registration) ||
            lifecycle_state_ != LifecycleState::Running) {
            out.rc = lifecycle_state_ == LifecycleState::Running
                         ? DSP_SVC_ERR_NO_BUFFER
                         : DSP_SVC_ERR_UNAVAILABLE;
            out.message = lifecycle_state_ == LifecycleState::Running
                              ? "client disconnected during allocation"
                              : "service stopping";
            return out;
        }

        std::lock_guard<std::mutex> buffers_lk(buffers_mu_);
        try {
            buffers_.reserve(buffers_.size() + entries.size());
        } catch (...) {
            out.rc = DSP_SVC_ERR_NO_MEM;
            out.message = "buffer registry insertion failed";
            return out;
        }
        // Replace the pre-HAL conservative admission atomically with the
        // actual returned plane-byte pool charge.
        release_buffer_admission_locked(admission);
        out.rc = reserve_buffer_usage_locked(resource_key, pool_key, count,
                                             out.message);
        if (out.rc != DSP_SVC_OK) return out;

        try {
            for (size_t i = 0; i < entries.size(); ++i) {
                auto& entry = entries[i];
                do {
                    entry->id = fresh_random_id();
                    if (entry->id == 0) throw std::bad_alloc();
                } while (buffers_.count(entry->id));
                buffers_.emplace(entry->id, entry.get());
                ++inserted_count;
                out.ids.push_back(entry->id);
                for (uint32_t p = 0; p < out.num_planes; ++p)
                    out.fds.push_back(entry->fb->dma_fds[p]);
            }
        } catch (...) {
            for (size_t i = 0; i < inserted_count; ++i)
                buffers_.erase(entries[i]->id);
            release_buffer_usage_locked(resource_key, pool_key, count);
            out.ids.clear();
            out.fds.clear();
            out.rc = DSP_SVC_ERR_NO_MEM;
            out.message = "buffer registry insertion failed";
            return out;
        }
    }

    for (auto& entry : entries) entry.release();
    fb_guard.disarm();

    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.buffers_allocated += count;
        stats_.buffers_in_registry += count;
    }
    return out;
}

DspService::ImportResult DspService::import_buffer(
    int client_fd, uint32_t width, uint32_t height, HalPixelFormat format,
    uint32_t num_planes, const uint32_t* strides, const uint32_t* sizes,
    const int* fds) {
    try {
        return import_buffer_impl(client_fd, width, height, format, num_planes,
                                  strides, sizes, fds);
    } catch (...) {
        ImportResult out;
        out.rc = DSP_SVC_ERR_NO_MEM;
        set_message_noexcept(out.message, "buffer import failed");
        return out;
    }
}

DspService::ImportResult DspService::import_buffer_impl(
    int client_fd, uint32_t width, uint32_t height, HalPixelFormat format,
    uint32_t num_planes, const uint32_t* strides, const uint32_t* sizes,
    const int* fds) {
    ImportResult out;
    if (!fb_ops_) {
        out.rc = DSP_SVC_ERR_UNAVAILABLE;
        out.message = "service not running";
        return out;
    }
    ClientRegistration registration;
    out.rc = begin_buffer_registration(client_fd, registration);
    if (out.rc != DSP_SVC_OK) {
        out.message = out.rc == DSP_SVC_ERR_NO_BUFFER
                          ? "client session disconnected"
                          : "service not running";
        return out;
    }
    BufferRegistrationGuard registration_guard(this);
    if (!strides || !sizes || !fds) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "missing plane descriptors";
        return out;
    }
    if (width < kMinDim || height < kMinDim || width > kMaxDim ||
        height > kMaxDim) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "width/height out of range [16, 8192]";
        return out;
    }
    if (!format_supported(format)) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "unsupported format (NV12/RGB24/GRAY8/ARGB32)";
        return out;
    }
    if (format == HAL_PIX_FMT_NV12 && ((width & 1U) || (height & 1U))) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "NV12 width and height must be even";
        return out;
    }
    if (pixels_of(width, height) > cfg_.max_pixels_per_op) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "buffer exceeds max_pixels_per_op";
        return out;
    }
    const uint32_t planes = plane_count_of(format);
    if (num_planes != planes) {
        out.rc = DSP_SVC_ERR_INVALID;
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "num_planes %u does not match format (%u expected)",
                      num_planes, planes);
        out.message = msg;
        return out;
    }
    uint64_t import_bytes = 0;
    bool all_dma = true;
    bool any_dma = false;
    bool copy_userptr[HAL_MAX_PLANES] = {false, false, false};
    constexpr int kStableSeals =
        F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    for (uint32_t p = 0; p < planes; ++p) {
        const uint32_t rows = plane_rows_of(format, height, p);
        const uint64_t minimum_size =
            static_cast<uint64_t>(strides[p]) * rows;
        if (strides[p] < min_stride_of(format, width) ||
            sizes[p] < minimum_size) {
            out.rc = DSP_SVC_ERR_INVALID;
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "plane %u geometry implausible (stride %u, size %u)",
                          p, strides[p], sizes[p]);
            out.message = msg;
            return out;
        }

        uint64_t retained_bytes = 0;
        uint64_t dma_capacity = 0;
        const bool is_dma = dma_buf_capacity(fds[p], dma_capacity);
        all_dma = all_dma && is_dma;
        any_dma = any_dma || is_dma;
        if (is_dma) {
            if (sizes[p] == 0 || sizes[p] > dma_capacity) {
                out.rc = DSP_SVC_ERR_INVALID;
                out.message = "dma-buf plane shorter than declared size";
                return out;
            }
            retained_bytes = dma_capacity;
        } else {
            struct stat st{};
            if (sizes[p] == 0 || fstat(fds[p], &st) != 0 ||
                !S_ISREG(st.st_mode) || st.st_size < 0 ||
                static_cast<uint64_t>(st.st_size) < sizes[p]) {
                out.rc = DSP_SVC_ERR_INVALID;
                out.message = "import plane shorter than declared size";
                return out;
            }
            const int seals = fcntl(fds[p], F_GET_SEALS);
            copy_userptr[p] =
                seals == -1 || (seals & kStableSeals) != kStableSeals;
            retained_bytes = copy_userptr[p]
                                 ? sizes[p]
                                 : static_cast<uint64_t>(st.st_size);
        }
        if (retained_bytes > UINT64_MAX - import_bytes) {
            out.rc = DSP_SVC_ERR_LIMIT;
            out.message = "import backing size overflow";
            return out;
        }
        import_bytes += retained_bytes;
    }
    if (any_dma && !all_dma) {
        out.rc = DSP_SVC_ERR_INVALID;
        out.message = "mixed dma-buf and non-dma-buf planes";
        return out;
    }

    // Reserve the actual retained backing before descriptor dup, USERPTR copy,
    // mmap, or heap allocation. Unstable files are copied, so only the sealed
    // copy's declared size is charged; stable files and dma-bufs charge their
    // kernel-reported backing size.
    const QuotaKey quota_key = quota_owner_key(client_fd);
    const QuotaKey resource_key = resource_owner_key(quota_key);
    out.rc = reserve_import_usage(resource_key, import_bytes, out.message);
    if (out.rc != DSP_SVC_OK) return out;
    auto usage_guard = make_scope_exit([&] {
        release_import_usage(resource_key, 1, import_bytes);
    });

    // Dup outside the registry lock (syscalls). The daemon keeps its own fd
    // copies, so the client may close theirs immediately if it wants.
    int dup_fds[HAL_MAX_PLANES] = {-1, -1, -1};
    for (uint32_t p = 0; p < planes; ++p) {
        dup_fds[p] = dup(fds[p]);
        if (dup_fds[p] < 0) {
            for (uint32_t q = 0; q < p; ++q) close(dup_fds[q]);
            out.rc = DSP_SVC_ERR_NO_MEM;
            out.message = "dup of client buffer fd failed";
            return out;
        }
    }

    // A plain descriptor the DSP HAL reads like any pool buffer: geometry +
    // fds or mapped planes + strides. hal_frame_to_dsp_image() only consumes
    // these fields (hailo15_dsp_impl.cpp) — refcounts/priv belong to HAL
    // pool buffers and are deliberately left zero.
    HalFrameBuffer* fb = nullptr;
    try {
        fb = new HalFrameBuffer();
    } catch (...) {
        for (uint32_t p = 0; p < planes; ++p) close(dup_fds[p]);
        out.rc = DSP_SVC_ERR_NO_MEM;
        out.message = "import descriptor allocation failed";
        return out;
    }
    fb->width = width;
    fb->height = height;
    fb->format = format;
    fb->num_planes = planes;
    if (all_dma) {
        fb->mem_type = HAL_MEM_DMABUF;
    } else {
        fb->mem_type = HAL_MEM_MALLOC; /* USERPTR planes (see above) */
        for (uint32_t p = 0; p < planes; ++p) {
            /* Unstable files are copied into a fully sealed daemon memfd before
             * mmap, closing the truncate/write race. Stable sealed files map
             * directly. In either case the mapping holds the backing reference,
             * so no original USERPTR descriptor remains pinned afterward. */
            int map_fd = dup_fds[p];
            if (copy_userptr[p]) {
                map_fd = copy_to_sealed_memfd(dup_fds[p], sizes[p]);
                if (map_fd < 0) {
                    for (uint32_t q = 0; q < p; ++q)
                        munmap(fb->planes[q], sizes[q]);
                    for (uint32_t q = 0; q < planes; ++q) close(dup_fds[q]);
                    delete fb;
                    out.rc = DSP_SVC_ERR_INVALID;
                    out.message =
                        "import plane unreadable or shorter than declared size";
                    return out;
                }
            }
            void* addr = mmap(nullptr, sizes[p], PROT_READ, MAP_SHARED,
                              map_fd, 0);
            if (addr == MAP_FAILED) {
                if (copy_userptr[p]) close(map_fd);
                for (uint32_t q = 0; q < p; ++q)
                    munmap(fb->planes[q], sizes[q]);
                for (uint32_t q = 0; q < planes; ++q) close(dup_fds[q]);
                delete fb;
                out.rc = DSP_SVC_ERR_INVALID;
                out.message = "import plane not mappable";
                return out;
            }
            if (copy_userptr[p]) close(map_fd);
            close(dup_fds[p]);
            dup_fds[p] = -1;
            fb->planes[p] = addr;
        }
    }
    for (uint32_t p = 0; p < HAL_MAX_PLANES; ++p) {
        fb->dma_fds[p] = (p < planes) ? dup_fds[p] : -1;
        fb->strides[p] = (p < planes) ? strides[p] : 0;
        fb->sizes[p] = (p < planes) ? sizes[p] : 0;
    }

    std::unique_ptr<BufferEntry> entry;
    try {
        entry = std::make_unique<BufferEntry>();
        entry->client_fd = client_fd;
        entry->client_registration = registration;
        entry->quota_key = quota_key;
        entry->resource_key = resource_key;
        entry->fb = fb;
        entry->retained_import_bytes = import_bytes;
        entry->imported = true;
    } catch (...) {
        free_imported_fb(fb);
        out.rc = DSP_SVC_ERR_NO_MEM;
        out.message = "import descriptor bookkeeping failed";
        return out;
    }

#ifdef DSP_SERVICE_TESTING
    try {
        if (before_buffer_register_hook_) before_buffer_register_hook_();
    } catch (...) {
        free_imported_fb(fb);
        out.rc = DSP_SVC_ERR_NO_MEM;
        out.message = "import registration bookkeeping failed";
        return out;
    }
#endif

    {
        std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mu_);
        if (!client_registration_active_locked(registration) ||
            lifecycle_state_ != LifecycleState::Running) {
            free_imported_fb(fb);
            out.rc = lifecycle_state_ == LifecycleState::Running
                         ? DSP_SVC_ERR_NO_BUFFER
                         : DSP_SVC_ERR_UNAVAILABLE;
            out.message = lifecycle_state_ == LifecycleState::Running
                              ? "client disconnected during import"
                              : "service stopping";
            return out;
        }
        std::lock_guard<std::mutex> buffers_lk(buffers_mu_);
        try {
            do {
                entry->id = fresh_random_id();
                if (entry->id == 0) throw std::bad_alloc();
            } while (buffers_.count(entry->id));
            buffers_.emplace(entry->id, entry.get());
            out.id = entry->id;
        } catch (...) {
            free_imported_fb(fb);
            out.rc = DSP_SVC_ERR_NO_MEM;
            out.message = "import registry insertion failed";
            return out;
        }
    }
    entry.release();
    usage_guard.disarm();

    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.buffers_allocated++;
        stats_.buffers_in_registry++;
    }
    return out;
}

DspService::DetachedFrame DspService::detach_entry_locked(
    BufferEntry* entry) noexcept {
    DetachedFrame frame;
    if (!entry || entry->detached) return frame;
    entry->detached = true;
    buffers_.erase(entry->id);
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.buffers_released++;
        if (stats_.buffers_in_registry > 0) stats_.buffers_in_registry--;
    }
    if (entry->pins != 0) return frame;

    release_entry_usage_locked(entry);
    frame.fb = entry->fb;
    frame.imported = entry->imported;
    delete entry;
    return frame;
}

void DspService::release_detached_frame(HalFrameBufferOps* fb_ops,
                                        DetachedFrame frame) noexcept {
    if (!frame.fb) return;
    if (frame.imported) free_imported_fb(frame.fb);
    else if (fb_ops && fb_ops->release_frame_buffer)
        fb_ops->release_frame_buffer(frame.fb);
}

int DspService::release_buffer(int client_fd, uint64_t buffer_id) {
    ClientRegistration registration;
    {
        std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mu_);
        auto session = client_sessions_.find(client_fd);
        if (session == client_sessions_.end() || !session->second.connected)
            return DSP_SVC_ERR_NO_BUFFER;
        registration = {client_fd, session->second.generation};
    }

    DetachedFrame frame;
    {
        std::lock_guard<std::mutex> lk(buffers_mu_);
        auto it = buffers_.find(buffer_id);
        if (it == buffers_.end()) return DSP_SVC_ERR_NO_BUFFER;
        if (!(it->second->client_registration == registration))
            return DSP_SVC_ERR_NO_BUFFER;
        frame = detach_entry_locked(it->second);
    }
    release_detached_frame(fb_ops_, frame);
    return DSP_SVC_OK;
}

void DspService::release_async_job_slot_locked(const JobRef& job) noexcept {
    if (!job || job->async_slot_released) return;
    auto count_it = client_async_jobs_.find(job->resource_key);
    if (count_it != client_async_jobs_.end()) {
        if (count_it->second > 1) --count_it->second;
        else client_async_jobs_.erase(count_it);
    }
    if (total_async_jobs_ > 0) --total_async_jobs_;
    job->async_slot_released = true;
}

void DspService::cancel_queued_jobs(
    const ClientRegistration& registration) noexcept {
    for (;;) {
        JobRef canceled;
        {
            std::lock_guard<std::mutex> q_lk(q_mu_);
            auto find_and_erase = [&](std::deque<JobRef>& queue) {
                for (auto it = queue.begin(); it != queue.end(); ++it) {
                    if ((*it)->owner_registration == registration) {
                        canceled = std::move(*it);
                        queue.erase(it);
                        return true;
                    }
                }
                return false;
            };
            if (!find_and_erase(q_normal_)) find_and_erase(q_background_);
        }
        if (!canceled) return;
        unpin_entries(canceled->pinned);
        canceled->pinned.clear();
        std::lock_guard<std::mutex> done_lk(done_mu_);
        canceled->result.rc = DSP_SVC_ERR_UNAVAILABLE;
        canceled->result.message.swap(canceled->cancellation_message);
        canceled->done = true;
        release_async_job_slot_locked(canceled);
    }
}

void DspService::release_client_buffers(int client_fd) {
    ClientRegistration registration;
    {
        std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mu_);
        auto session = client_sessions_.find(client_fd);
        if (session == client_sessions_.end() || !session->second.connected)
            return;
        session->second.connected = false;
        registration = {client_fd, session->second.generation};
    }

    size_t freed = 0;
    for (;;) {
        DetachedFrame frame;
        bool found = false;
        {
            std::lock_guard<std::mutex> buffers_lk(buffers_mu_);
            for (auto it = buffers_.begin(); it != buffers_.end(); ++it) {
                if (it->second->client_registration == registration) {
                    frame = detach_entry_locked(it->second);
                    found = true;
                    break;
                }
            }
        }
        if (!found) break;
        if (frame.fb) ++freed;
        release_detached_frame(fb_ops_, frame);
    }

    size_t reaped = 0;
    {
        std::lock_guard<std::mutex> done_lk(done_mu_);
        for (auto it = jobs_.begin(); it != jobs_.end();) {
            JobRef job = it->second;
            if (job->owner_registration == registration) {
                job->abandoned = true;
                if (job->done) release_async_job_slot_locked(job);
                it = jobs_.erase(it);
                ++reaped;
            } else {
                ++it;
            }
        }
    }
    cancel_queued_jobs(registration);
    done_cv_.notify_all();
    if (freed > 0)
        HAL_LOG_INFO("DspService: client %d disconnected, freed %zu buffer(s)",
                     client_fd, freed);
    if (reaped > 0)
        HAL_LOG_INFO("DspService: client %d disconnected, reaped %zu async job(s)",
                     client_fd, reaped);
    quota_forget(client_fd);
}

/* ------------------------------------------------------------------ */
/* One-shot ops plane (daemon-internal)                                */
/* ------------------------------------------------------------------ */

void DspService::BufferPin::release_pin() noexcept {
    if (!svc_ || !entry_) return;
    DspService* service = svc_;
    BufferEntry* entry = static_cast<BufferEntry*>(entry_);
    svc_ = nullptr;
    entry_ = nullptr;
    fb_ = nullptr;
    service->unpin_entry(entry);
    service->end_buffer_pin();
}

DspService::BufferPin::~BufferPin() noexcept { release_pin(); }

DspService::BufferPin::BufferPin(BufferPin&& other) noexcept
    : svc_(other.svc_), entry_(other.entry_), fb_(other.fb_),
      owner_fd_(other.owner_fd_), rc_(other.rc_) {
    other.svc_ = nullptr;
    other.entry_ = nullptr;
    other.fb_ = nullptr;
    other.owner_fd_ = -1;
    other.rc_ = DSP_SVC_ERR_NO_BUFFER;
}

DspService::BufferPin& DspService::BufferPin::operator=(BufferPin&& other) noexcept {
    if (this != &other) {
        release_pin();
        svc_ = other.svc_;
        entry_ = other.entry_;
        fb_ = other.fb_;
        owner_fd_ = other.owner_fd_;
        rc_ = other.rc_;
        other.svc_ = nullptr;
        other.entry_ = nullptr;
        other.fb_ = nullptr;
        other.owner_fd_ = -1;
        other.rc_ = DSP_SVC_ERR_NO_BUFFER;
    }
    return *this;
}

DspService::BufferPin DspService::pin_buffer(uint64_t buffer_id) {
    BufferPin pin;
    if (!begin_buffer_pin()) {
        pin.rc_ = DSP_SVC_ERR_UNAVAILABLE;
        return pin;
    }

    BufferEntry* entry = nullptr;
    int owner = -1;
    {
        std::lock_guard<std::mutex> lk(buffers_mu_);
        if (!resolve_pin_buffer(buffer_id, owner, entry)) {
            end_buffer_pin();
            return pin;
        }
    }
    pin.svc_ = this;
    pin.entry_ = entry;
    pin.fb_ = entry->fb;
    pin.owner_fd_ = owner;
    pin.rc_ = DSP_SVC_OK;
    return pin;
}

/* ------------------------------------------------------------------ */
/* Job plane                                                           */
/* ------------------------------------------------------------------ */

bool DspService::resolve_pin_buffer(uint64_t id, int& owner_fd_out,
                                    BufferEntry*& entry) {
    // Caller holds buffers_mu_.
    auto it = buffers_.find(id);
    if (it == buffers_.end()) return false;
    entry = it->second;
    owner_fd_out = entry->client_fd;
    entry->pins++;
    return true;
}

void DspService::unpin_entry(BufferEntry* entry) noexcept {
    if (!entry) return;
    DetachedFrame frame;
    {
        std::lock_guard<std::mutex> lk(buffers_mu_);
        if (entry->pins > 0) --entry->pins;
        if (entry->detached && entry->pins == 0) {
            release_entry_usage_locked(entry);
            frame.fb = entry->fb;
            frame.imported = entry->imported;
            delete entry;
        }
    }
    release_detached_frame(fb_ops_, frame);
}

void DspService::unpin_entries(
    const std::vector<BufferEntry*>& entries) noexcept {
    for (BufferEntry* entry : entries) unpin_entry(entry);
}

int DspService::validate_and_pin(const DspJobDesc& desc, JobRef& job_out,
                                 std::string& why) {
    if (desc.interpolation < 0 ||
        desc.interpolation >= HAL_DSP_INTERPOLATION_MAX) {
        why = "invalid interpolation";
        return DSP_SVC_ERR_INVALID;
    }
    if (desc.scaling_mode < 0 || desc.scaling_mode >= HAL_DSP_SCALING_MAX) {
        why = "invalid scaling_mode";
        return DSP_SVC_ERR_INVALID;
    }

    const bool wants_rects = desc.op == HAL_DSP_OP_CROP_RESIZE ||
                             desc.op == HAL_DSP_OP_MULTI_CROP_RESIZE ||
                             desc.op == HAL_DSP_OP_BLEND;
    if (wants_rects && desc.rects.empty()) {
        why = "op requires >= 1 rect";
        return DSP_SVC_ERR_INVALID;
    }
    if (!wants_rects && !desc.rects.empty()) {
        why = "op does not take rects";
        return DSP_SVC_ERR_INVALID;
    }
    if (desc.dst_ids.empty()) {
        why = "no dst buffers";
        return DSP_SVC_ERR_INVALID;
    }
    const bool multi_dst = desc.op == HAL_DSP_OP_MULTI_CROP_RESIZE ||
                           desc.op == HAL_DSP_OP_BLEND;
    if (multi_dst && (desc.dst_ids.size() > cfg_.max_batch ||
                      desc.rects.size() != desc.dst_ids.size())) {
        why = "op requires dst_ids.size() == rects.size() <= max_batch";
        return DSP_SVC_ERR_INVALID;
    }
    if (!multi_dst && desc.dst_ids.size() != 1) {
        why = "op requires exactly 1 dst buffer";
        return DSP_SVC_ERR_INVALID;
    }
    if (desc.op != HAL_DSP_OP_RESIZE && desc.op != HAL_DSP_OP_CROP_RESIZE &&
        desc.op != HAL_DSP_OP_MULTI_CROP_RESIZE &&
        desc.op != HAL_DSP_OP_CONVERT_FORMAT && desc.op != HAL_DSP_OP_BLEND) {
        why = "op not available";
        return DSP_SVC_ERR_INVALID;
    }
    if (desc.op == HAL_DSP_OP_BLEND && desc.src_id == 0) {
        why = "BLEND needs a base buffer";
        return DSP_SVC_ERR_INVALID;
    }

    JobRef job;
    try {
#ifdef DSP_SERVICE_TESTING
        if (before_pin_bookkeeping_hook_) before_pin_bookkeeping_hook_();
#endif
        job = std::make_shared<JobItem>();
        job->desc = desc;
        job->priority = job->desc.priority;
        job->result.message = "service stopping";
        job->cancellation_message = "job canceled";
        // Reserve before the first pin increment. Every subsequent push is then
        // non-allocating, so pin counts and bookkeeping stay transactional.
        job->pinned.reserve(1 + job->desc.dst_ids.size());
    } catch (...) {
        set_message_noexcept(why, "job pin bookkeeping allocation failed");
        return DSP_SVC_ERR_NO_MEM;
    }
    auto pin_guard = make_scope_exit([&] { unpin_entries(job->pinned); });

    // Locked section: resolve + pin every referenced buffer. On failure the
    // caller unpins AFTER the lock is gone (unpin_entries takes buffers_mu_).
    int vrc = DSP_SVC_OK;
    int owner_fd = -1;
    {
        std::lock_guard<std::mutex> lk(buffers_mu_);

        BufferEntry* src = nullptr;
        int src_owner = -1;
        if (!resolve_pin_buffer(job->desc.src_id, src_owner, src)) {
            vrc = DSP_SVC_ERR_NO_BUFFER;
            why = "src buffer not found";
        } else {
            owner_fd = src_owner;
            job->owner_registration = src->client_registration;
            job->quota_key = src->quota_key;
            job->resource_key = src->resource_key;
            job->pinned.push_back(src);
            const HalFrameBuffer* sfb = src->fb;
            uint64_t dst_px_sum = 0;

            if (job->desc.op == HAL_DSP_OP_BLEND) {
                if (sfb->format != HAL_PIX_FMT_NV12) {
                    vrc = DSP_SVC_ERR_INVALID;
                    why = "BLEND base must be NV12 (composited in place)";
                } else if (src->imported) {
                    /* the vendor op writes the base — an imported frame
                     * belongs to the app, mutating it corrupts the source */
                    vrc = DSP_SVC_ERR_INVALID;
                    why = "BLEND composites the base in place; imported "
                          "frames cannot be the base";
                }
            }

            for (size_t i = 0; vrc == DSP_SVC_OK && i < job->desc.dst_ids.size();
                 ++i) {
                int dst_owner = -1;
                BufferEntry* dst = nullptr;
                if (!resolve_pin_buffer(job->desc.dst_ids[i], dst_owner, dst)) {
                    vrc = DSP_SVC_ERR_NO_BUFFER;
                    why = "dst buffer not found";
                    break;
                }
                job->pinned.push_back(dst);
                const HalFrameBuffer* dfb = dst->fb;

                if (!(dst->client_registration == job->owner_registration)) {
                    vrc = DSP_SVC_ERR_NO_BUFFER;
                    why = "src/dst buffers belong to different client sessions";
                    break;
                }
                if (dst->imported && job->desc.op != HAL_DSP_OP_BLEND) {
                    /* written outputs must be daemon pool buffers; the one
                     * exception is BLEND, whose dst slots are the ARGB32
                     * overlays — read-only inputs (the SDK ships them as
                     * memfd imports because the deployed HAL refuses
                     * ARGB32 pool allocation on some devices) */
                    vrc = DSP_SVC_ERR_INVALID;
                    why = "imported buffers are source-only";
                    break;
                }
                if (dfb->format != sfb->format &&
                    job->desc.op != HAL_DSP_OP_CONVERT_FORMAT &&
                    job->desc.op != HAL_DSP_OP_BLEND) {
                    vrc = DSP_SVC_ERR_INVALID;
                    why = "src/dst format mismatch (only CONVERT_FORMAT and "
                          "BLEND allow it)";
                    break;
                }
                if (dfb->format == sfb->format &&
                    job->desc.op == HAL_DSP_OP_CONVERT_FORMAT) {
                    vrc = DSP_SVC_ERR_INVALID;
                    why = "CONVERT_FORMAT requires differing formats";
                    break;
                }
                if (job->desc.op == HAL_DSP_OP_BLEND &&
                    dfb->format != HAL_PIX_FMT_ARGB32) {
                    vrc = DSP_SVC_ERR_INVALID;
                    why = "BLEND overlays must be ARGB32";
                    break;
                }

                if (wants_rects) {
                    const DspRect& r = job->desc.rects[i];
                    if (r.width == 0 || r.height == 0 || r.dst_width == 0 ||
                        r.dst_height == 0) {
                        vrc = DSP_SVC_ERR_INVALID;
                        why = "rect has zero dimension";
                        break;
                    }
                    if (r.x > sfb->width || r.y > sfb->height ||
                        r.width > sfb->width - r.x ||
                        r.height > sfb->height - r.y) {
                        vrc = DSP_SVC_ERR_INVALID;
                        why = "rect exceeds source bounds";
                        break;
                    }
                    if (r.dst_width != dfb->width || r.dst_height != dfb->height) {
                        vrc = DSP_SVC_ERR_INVALID;
                        why = "rect dst dims must match dst buffer dims";
                        break;
                    }
                    if (job->desc.op == HAL_DSP_OP_BLEND &&
                        (r.width != dfb->width || r.height != dfb->height)) {
                        vrc = DSP_SVC_ERR_INVALID;
                        why = "BLEND pastes overlays 1:1 — rect w/h must "
                              "equal the overlay dims";
                        break;
                    }
                } else if (job->desc.op == HAL_DSP_OP_CONVERT_FORMAT &&
                           (dfb->width != sfb->width ||
                            dfb->height != sfb->height)) {
                    vrc = DSP_SVC_ERR_INVALID;
                    why = "CONVERT_FORMAT requires equal dims (P0)";
                    break;
                }
                dst_px_sum += pixels_of(dfb->width, dfb->height);
            }

            if (vrc == DSP_SVC_OK) {
                const uint64_t src_px = pixels_of(sfb->width, sfb->height);
                if (src_px > cfg_.max_pixels_per_op ||
                    dst_px_sum > cfg_.max_pixels_per_op) {
                    vrc = DSP_SVC_ERR_INVALID;
                    why = "op exceeds max_pixels_per_op";
                } else {
                    job->charge_mpix =
                        static_cast<double>(src_px + dst_px_sum) / 1e6;
                }
            }
        }
    } // buffers_mu_ released

    if (vrc != DSP_SVC_OK) return vrc;
    job->owner_fd = owner_fd;
    job_out = std::move(job);
    pin_guard.disarm();
    return DSP_SVC_OK;
}

DspService::QuotaKey DspService::quota_owner_key(int owner_fd) {
    /* SO_PEERCRED is available only while this connected UDS fd is alive, so
     * capture the process incarnation once at buffer registration. The local
     * 3-field layout avoids _GNU_SOURCE churn for struct ucred. */
    struct {
        int32_t pid;
        uint32_t uid;
        uint32_t gid;
    } cred{};
    socklen_t len = sizeof(cred);
    uint64_t start_time_ticks = 0;
    if (owner_fd >= 0 &&
        ::getsockopt(owner_fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0 &&
        len == sizeof(cred) && cred.pid > 0 &&
        read_process_start_time(cred.pid, start_time_ticks)) {
        return QuotaKey::for_process(cred.pid, start_time_ticks);
    }
    return QuotaKey::for_legacy_fd(owner_fd);
}

bool DspService::quota_try_consume(const QuotaKey& quota_key, double mpix,
                                   std::string& why) {
    using clock = std::chrono::steady_clock;
    std::lock_guard<std::mutex> lk(quota_mu_);
    auto bucket_result = quotas_.try_emplace(quota_key);
    QuotaBucket& b = bucket_result.first->second;
    const auto discard_new_bucket = [&]() noexcept {
        if (bucket_result.second) quotas_.erase(bucket_result.first);
    };
    const auto now = clock::now();
    const auto refill = [&](QuotaBucket& bucket, double jobs_per_sec,
                            double mpix_per_sec) {
        const bool first_use =
            bucket.last.time_since_epoch().count() == 0;
        double dt = first_use
                        ? 0.0
                        : std::chrono::duration<double>(now - bucket.last)
                              .count();
        if (dt < 0) dt = 0;
        bucket.last = now;
        if (first_use) {
            /* Grant the full 1 s burst up front so the first job is accepted. */
            bucket.jobs = jobs_per_sec;
            bucket.mpix = mpix_per_sec;
        } else {
            bucket.jobs =
                std::min(bucket.jobs + dt * jobs_per_sec, jobs_per_sec);
            bucket.mpix =
                std::min(bucket.mpix + dt * mpix_per_sec, mpix_per_sec);
        }
        return first_use;
    };
    const bool first_use = refill(
        b, cfg_.quota_jobs_per_sec, cfg_.quota_mpix_per_sec);
    refill(global_quota_, cfg_.quota_total_jobs_per_sec,
           cfg_.quota_total_mpix_per_sec);
    if (first_use && quota_key.kind == QuotaKey::Kind::Process) {
        HAL_LOG_INFO(
            "DspService: quota bucket created for process pid=%d start=%llu",
            quota_key.pid, static_cast<unsigned long long>(
                               quota_key.process_start_time_ticks));
    }
    if (b.jobs < 1.0) {
        char msg[96];
        std::snprintf(msg, sizeof(msg), "quota: jobs/s budget exhausted (%.0f/s)",
                      cfg_.quota_jobs_per_sec);
        discard_new_bucket();
        why = msg;
        return false;
    }
    if (b.mpix < mpix) {
        char msg[96];
        std::snprintf(msg, sizeof(msg),
                      "quota: MPix/s budget exhausted (need %.2f, have %.2f)", mpix,
                      b.mpix);
        discard_new_bucket();
        why = msg;
        return false;
    }
    if (global_quota_.jobs < 1.0) {
        char msg[112];
        std::snprintf(msg, sizeof(msg),
                      "quota: service jobs/s budget exhausted (%.0f/s)",
                      cfg_.quota_total_jobs_per_sec);
        discard_new_bucket();
        why = msg;
        return false;
    }
    if (global_quota_.mpix < mpix) {
        char msg[112];
        std::snprintf(
            msg, sizeof(msg),
            "quota: service MPix/s budget exhausted (need %.2f, have %.2f)",
            mpix, global_quota_.mpix);
        discard_new_bucket();
        why = msg;
        return false;
    }
    b.jobs -= 1.0;
    b.mpix -= mpix;
    global_quota_.jobs -= 1.0;
    global_quota_.mpix -= mpix;
    /* Process-keyed buckets are NOT erased on disconnect (a process may
     * hold several connections; the token bucket refills anyway), so bound
     * the map: past 256 buckets, drop ones idle for over a minute. */
    if (quotas_.size() > 256) {
        for (auto it = quotas_.begin(); it != quotas_.end();) {
            if (!(it->first == quota_key) &&
                std::chrono::duration<double>(now - it->second.last).count() > 60.0)
                it = quotas_.erase(it);
            else
                ++it;
        }
    }
    return true;
}

void DspService::quota_forget(int owner_fd) {
    /* Only a legacy per-fd bucket is dropped here. Process-incarnation buckets
     * outlive any one connection because another connection from that process
     * may still be active; the idle sweep reclaims abandoned buckets. */
    if (owner_fd < 0)
        return; /* daemon-internal shared fallback bucket: keep */
    std::lock_guard<std::mutex> lk(quota_mu_);
    quotas_.erase(QuotaKey::for_legacy_fd(owner_fd));
}

int DspService::begin_buffer_registration(
    int client_fd, ClientRegistration& registration) noexcept {
    uint64_t device = 0;
    uint64_t inode = 0;
    const bool has_identity =
        read_client_file_identity(client_fd, device, inode);
    if (!has_identity) {
        device = UINT64_MAX;
        inode = static_cast<uint64_t>(static_cast<int64_t>(client_fd));
    }

    std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mu_);
    if (lifecycle_state_ != LifecycleState::Running || !dsp_ctx_)
        return DSP_SVC_ERR_UNAVAILABLE;
    try {
        auto it = client_sessions_.find(client_fd);
        if (it == client_sessions_.end()) {
            ClientSession session;
            session.generation = next_client_generation_++;
            if (session.generation == 0)
                session.generation = next_client_generation_++;
            session.device = device;
            session.inode = inode;
            session.connected = true;
            it = client_sessions_.emplace(client_fd, session).first;
        } else if (it->second.device != device || it->second.inode != inode) {
            it->second.generation = next_client_generation_++;
            if (it->second.generation == 0)
                it->second.generation = next_client_generation_++;
            it->second.device = device;
            it->second.inode = inode;
            it->second.connected = true;
        } else if (!it->second.connected) {
            return DSP_SVC_ERR_NO_BUFFER;
        }
        registration = {client_fd, it->second.generation};
        ++active_buffer_registrations_;
        return DSP_SVC_OK;
    } catch (...) {
        return DSP_SVC_ERR_NO_MEM;
    }
}

void DspService::end_buffer_registration() noexcept {
    std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mu_);
    if (active_buffer_registrations_ > 0) --active_buffer_registrations_;
    lifecycle_cv_.notify_all();
}

bool DspService::client_registration_active_locked(
    const ClientRegistration& registration) const noexcept {
    const auto it = client_sessions_.find(registration.fd);
    return it != client_sessions_.end() && it->second.connected &&
           it->second.generation == registration.generation;
}

bool DspService::begin_buffer_pin() noexcept {
    std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mu_);
    if (lifecycle_state_ != LifecycleState::Running || !dsp_ctx_)
        return false;
    ++active_buffer_pins_;
    return true;
}

void DspService::end_buffer_pin() noexcept {
    std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mu_);
    if (active_buffer_pins_ > 0) --active_buffer_pins_;
    lifecycle_cv_.notify_all();
}

bool DspService::begin_async_submission() {
    std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mu_);
    if (lifecycle_state_ != LifecycleState::Running || !dsp_ctx_)
        return false;
    ++active_async_submissions_;
    return true;
}

void DspService::end_async_submission() noexcept {
    std::lock_guard<std::mutex> lifecycle_lk(lifecycle_mu_);
    if (active_async_submissions_ == 0) {
        HAL_LOG_ERROR("DspService: async submission tracker underflow");
        return;
    }
    --active_async_submissions_;
    lifecycle_cv_.notify_all();
}

DspJobResult DspService::submit_job(const DspJobDesc& desc) {
    try {
        return submit_job_impl(desc);
    } catch (...) {
        DspJobResult result;
        result.rc = DSP_SVC_ERR_NO_MEM;
        set_message_noexcept(result.message, "DSP service allocation failed");
        return result;
    }
}

DspJobResult DspService::submit_job_impl(const DspJobDesc& desc) {
    /* P2: the synchronous form is the async form plus one bounded wait —
     * one code path for validation, quota and queueing. */
    uint64_t job_id = 0;
    DspJobResult res = submit_job_async(desc, job_id);
    if (res.rc != DSP_SVC_OK) return res;

    bool done = false;
    res = wait_job(job_id, cfg_.job_timeout_ms, done);
    if (done) return res;

    // Watchdog: a vendor op in flight cannot be cancelled — the worker
    // finishes it and discards the result (jobs_timed_out). Pins already
    // dropped at the execute_job tail; reap the registry entry here.
    {
        std::lock_guard<std::mutex> lk(done_mu_);
        auto it = jobs_.find(job_id);
        if (it != jobs_.end()) {
            JobRef job = it->second;
            job->abandoned = true;
            if (job->done) release_async_job_slot_locked(job);
            jobs_.erase(it);
        }
    }
    done_cv_.notify_all();
    res.rc = DSP_SVC_ERR_TIMEOUT;
    char msg[128];
    std::snprintf(msg, sizeof(msg), "job timed out after %ums (dst undefined)",
                  cfg_.job_timeout_ms);
    res.message = msg;
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.jobs_timed_out++;
    }
    HAL_LOG_WARNING("DspService: %s", msg);
    return res;
}

DspJobResult DspService::submit_job_async(const DspJobDesc& desc,
                                          uint64_t& job_id_out) {
    try {
        return submit_job_async_impl(desc, job_id_out);
    } catch (...) {
        job_id_out = 0;
        DspJobResult result;
        result.rc = DSP_SVC_ERR_NO_MEM;
        set_message_noexcept(result.message, "DSP service allocation failed");
        return result;
    }
}

DspJobResult DspService::submit_job_async_impl(const DspJobDesc& desc,
                                               uint64_t& job_id_out) {
    job_id_out = 0;
    DspJobResult res;
    if (!begin_async_submission()) {
        res.rc = DSP_SVC_ERR_UNAVAILABLE;
        res.message = "DspService not running";
        return res;
    }
    AsyncSubmissionGuard submission_guard(this);

    JobRef job;
    uint64_t local_job_id = 0;
    bool registered = false;
    bool queued = false;
    auto rollback = [&]() noexcept {
        if (registered) {
            std::lock_guard<std::mutex> lk(done_mu_);
            auto it = jobs_.find(local_job_id);
            if (it != jobs_.end() && it->second == job) jobs_.erase(it);
            release_async_job_slot_locked(job);
            registered = false;
        }
        if (job && !queued) {
            unpin_entries(job->pinned);
            job->pinned.clear();
        }
    };

    try {
        std::string why;
        const int vrc = validate_and_pin(desc, job, why);
        if (vrc != DSP_SVC_OK) {
            res.rc = vrc;
            res.message = why;
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.jobs_rejected++;
            return res;
        }

#ifdef DSP_SERVICE_TESTING
        if (after_async_pin_hook_) after_async_pin_hook_();
#endif

        std::string quota_why;
        if (!quota_try_consume(job->quota_key, job->charge_mpix, quota_why)) {
            rollback();
            res.rc = DSP_SVC_ERR_QUOTA;
            res.message = quota_why;
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.jobs_rejected++;
            return res;
        }

        {
            std::lock_guard<std::mutex> buffers_lk(buffers_mu_);
            BufferEntry* owner_entry =
                job->pinned.empty() ? nullptr : job->pinned[0];
            const auto owner_it = owner_entry
                                      ? buffers_.find(owner_entry->id)
                                      : buffers_.end();
            if (!owner_entry || owner_entry->detached ||
                owner_it == buffers_.end() || owner_it->second != owner_entry ||
                !(owner_entry->client_registration ==
                  job->owner_registration)) {
                res.rc = DSP_SVC_ERR_NO_BUFFER;
                res.message = "buffer owner disconnected during submission";
            } else {
                std::lock_guard<std::mutex> done_lk(done_mu_);
                auto count_it = client_async_jobs_.find(job->resource_key);
                const uint32_t count = count_it == client_async_jobs_.end()
                                           ? 0
                                           : count_it->second;
                if (total_async_jobs_ >= cfg_.max_total_async_jobs) {
                    res.rc = DSP_SVC_ERR_QUOTA;
                    res.message = "too many outstanding jobs for this service";
                } else if (count >= cfg_.max_async_jobs_per_client) {
                    res.rc = DSP_SVC_ERR_QUOTA;
                    res.message = "too many outstanding jobs for this client";
                } else {
                    auto count_result = client_async_jobs_.try_emplace(
                        job->resource_key, 0);
                    try {
                        do {
                            local_job_id = fresh_random_id();
                            if (local_job_id == 0) throw std::bad_alloc();
                        } while (jobs_.count(local_job_id));
                        jobs_.emplace(local_job_id, job);
                    } catch (...) {
                        if (count_result.second)
                            client_async_jobs_.erase(count_result.first);
                        throw;
                    }
                    ++count_result.first->second;
                    ++total_async_jobs_;
                    registered = true;
                }
            }
        }
        if (res.rc != DSP_SVC_OK) {
            rollback();
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.jobs_rejected++;
            return res;
        }

#ifdef DSP_SERVICE_TESTING
        if (after_async_register_hook_) after_async_register_hook_();
#endif

        res.rc = DSP_SVC_OK;
        res.message = "submitted";
        {
            std::lock_guard<std::mutex> done_lk(done_mu_);
            const auto registered_it = jobs_.find(local_job_id);
            if (registered_it == jobs_.end() ||
                registered_it->second != job || job->abandoned.load()) {
                res.rc = DSP_SVC_ERR_NO_BUFFER;
                res.message = "buffer owner disconnected before enqueue";
            } else {
                std::lock_guard<std::mutex> q_lk(q_mu_);
                (job->priority == DspPriority::Background ? q_background_
                                                          : q_normal_)
                    .push_back(job);
                queued = true;
            }
        }
        if (!queued) {
            rollback();
            std::lock_guard<std::mutex> stats_lk(stats_mu_);
            stats_.jobs_rejected++;
            return res;
        }
        job_id_out = local_job_id;
        q_cv_.notify_one();
        return res;
    } catch (const std::bad_alloc&) {
        rollback();
        res.rc = DSP_SVC_ERR_NO_MEM;
        set_message_noexcept(res.message, "DSP service allocation failed");
    } catch (...) {
        rollback();
        res.rc = DSP_SVC_ERR_NO_MEM;
        set_message_noexcept(res.message, "DSP service bookkeeping failed");
    }
    {
        std::lock_guard<std::mutex> lk(stats_mu_);
        stats_.jobs_rejected++;
    }
    return res;
}

DspJobResult DspService::wait_job(uint64_t job_id, uint32_t timeout_ms,
                                  bool& done_out) {
    done_out = false;
    if (!begin_async_submission()) {
        DspJobResult result;
        result.rc = DSP_SVC_ERR_UNAVAILABLE;
        set_message_noexcept(result.message, "DspService not running");
        return result;
    }
    AsyncSubmissionGuard submission_guard(this);
    try {
        return wait_job_impl(job_id, timeout_ms, done_out);
    } catch (...) {
        done_out = false;
        DspJobResult result;
        result.rc = DSP_SVC_ERR_NO_MEM;
        set_message_noexcept(result.message, "DSP service allocation failed");
        return result;
    }
}

DspJobResult DspService::wait_job_impl(uint64_t job_id, uint32_t timeout_ms,
                                       bool& done_out) {
    done_out = false;
    DspJobResult res;
    std::unique_lock<std::mutex> lk(done_mu_);
    auto it = jobs_.find(job_id);
    if (it == jobs_.end()) {
        res.rc = DSP_SVC_ERR_NO_BUFFER;
        res.message = "unknown or reaped job id";
        return res;
    }
    JobRef job = it->second;
    if (job->active_waiters >= cfg_.max_waiters_per_job) {
        res.rc = DSP_SVC_ERR_QUOTA;
        res.message = "too many waiters for this job";
        return res;
    }
    if (total_waiters_ >= cfg_.max_total_waiters) {
        res.rc = DSP_SVC_ERR_QUOTA;
        res.message = "too many waiters for this service";
        return res;
    }
    ++job->active_waiters;
    ++total_waiters_;
    auto waiter_guard = make_scope_exit([&]() noexcept {
        if (job->active_waiters > 0) --job->active_waiters;
        if (total_waiters_ > 0) --total_waiters_;
    });
#ifdef DSP_SERVICE_TESTING
    if (after_wait_job_lookup_hook_) {
        lk.unlock();
        try {
            after_wait_job_lookup_hook_();
        } catch (...) {
            lk.lock();
            throw;
        }
        lk.lock();
    }
#endif
    const uint32_t effective_timeout_ms =
        std::min(timeout_ms, cfg_.max_wait_job_timeout_ms);
    if (effective_timeout_ms > 0) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(effective_timeout_ms);
        done_cv_.wait_until(lk, deadline, [&] {
            return job->done || !running_.load();
        });
    } /* timeout_ms == 0: non-blocking poll (HAL wait() convention) */
    if (!job->done) {
        // Entry stays valid — the caller may re-wait later unless stop owns
        // registry teardown, in which case report the lifecycle transition.
        const bool service_stopping = !running_.load();
        res.rc = service_stopping ? DSP_SVC_ERR_UNAVAILABLE
                                  : DSP_SVC_ERR_TIMEOUT;
        if (service_stopping) {
            res.message = "DSP service stopping";
        } else {
            char msg[96];
            std::snprintf(msg, sizeof(msg), "job %lu not done after %ums",
                          static_cast<unsigned long>(job_id),
                          effective_timeout_ms);
            res.message = msg;
        }
        return res;
    }
    res = job->result;
    /* The wait above released done_mu_: release_client_buffers may have
     * reaped this entry meanwhile (client disconnect). The JobRef keeps the
     * job object alive, but the map node is gone — erasing through the
     * pre-wait iterator is UB. Re-find; an absent entry was already
     * reaped (its async-job slot too), so only the found case unwinds. */
    it = jobs_.find(job_id);
    if (it != jobs_.end()) {
        jobs_.erase(it);
        release_async_job_slot_locked(job);
    }
    done_out = true;
    return res;
}

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */

void DspService::worker_loop() {
    while (running_.load()) {
        JobRef job;
        {
            std::unique_lock<std::mutex> lk(q_mu_);
            q_cv_.wait(lk, [&] {
                return !q_normal_.empty() || !q_background_.empty() ||
                       !running_.load();
            });
            if (!running_.load()) break;
            if (!q_normal_.empty()) {
                job = q_normal_.front();
                q_normal_.pop_front();
            } else {
                job = q_background_.front();
                q_background_.pop_front();
            }
        }
        try {
            execute_job(job);
        } catch (const std::bad_alloc&) {
            job->result.rc = DSP_SVC_ERR_NO_MEM;
            set_message_noexcept(job->result.message,
                                 "DSP worker allocation failed");
            std::lock_guard<std::mutex> stats_lk(stats_mu_);
            stats_.jobs_failed++;
        } catch (...) {
            job->result.rc = DSP_SVC_ERR_UNAVAILABLE;
            set_message_noexcept(job->result.message,
                                 "DSP worker execution failed");
            std::lock_guard<std::mutex> stats_lk(stats_mu_);
            stats_.jobs_failed++;
        }
        {
            std::lock_guard<std::mutex> lk(done_mu_);
            if (job->abandoned.load()) {
                job->result.rc = DSP_SVC_ERR_UNAVAILABLE;
                job->result.message.swap(job->cancellation_message);
                release_async_job_slot_locked(job);
            }
            job->done = true;
        }
        done_cv_.notify_all();
    }
}

int DspService::build_resize(const JobRef& job, HalDspResizeParams& p) {
    p.src = job->pinned[0]->fb;
    p.dst = job->pinned[1]->fb;
    p.interpolation = job->desc.interpolation;
    return DSP_SVC_OK;
}

int DspService::build_crop_resize(const JobRef& job, HalDspCropResizeParams& p) {
    const DspRect& r = job->desc.rects[0];
    p.src = job->pinned[0]->fb;
    p.dst = job->pinned[1]->fb;
    p.crop.start_x = r.x;
    p.crop.start_y = r.y;
    p.crop.end_x = r.x + r.width;
    p.crop.end_y = r.y + r.height;
    p.interpolation = job->desc.interpolation;
    p.scaling_mode = job->desc.scaling_mode;
    p.letterbox_alignment =
        (job->desc.scaling_mode == HAL_DSP_SCALING_LETTERBOX_MIDDLE)
            ? HAL_DSP_LETTERBOX_MIDDLE
            : (job->desc.scaling_mode == HAL_DSP_SCALING_LETTERBOX_UP_LEFT)
                  ? HAL_DSP_LETTERBOX_UP_LEFT
                  : HAL_DSP_LETTERBOX_NONE;
    p.letterbox_color = HalDspColor{}; /* black */
    return DSP_SVC_OK;
}

int DspService::build_multi_crop(const JobRef& job,
                                 std::vector<HalDspMultiCropOutput>& outputs,
                                 HalDspMultiCropResizeParams& p) {
    p.src = job->pinned[0]->fb;
    p.interpolation = job->desc.interpolation;
    outputs.resize(job->desc.rects.size());
    for (size_t i = 0; i < job->desc.rects.size(); ++i) {
        const DspRect& r = job->desc.rects[i];
        outputs[i].crop.start_x = r.x;
        outputs[i].crop.start_y = r.y;
        outputs[i].crop.end_x = r.x + r.width;
        outputs[i].crop.end_y = r.y + r.height;
        outputs[i].dst = job->pinned[1 + i]->fb;
        outputs[i].scaling_mode = job->desc.scaling_mode;
        outputs[i].letterbox_color = HalDspColor{}; /* black */
    }
    p.outputs = outputs.data();
    p.output_count = static_cast<uint32_t>(outputs.size());
    return DSP_SVC_OK;
}

int DspService::build_convert(const JobRef& job, HalDspConvertFormatParams& p) {
    p.src = job->pinned[0]->fb;
    p.dst = job->pinned[1]->fb;
    return DSP_SVC_OK;
}

int DspService::build_blend(const JobRef& job,
                            std::vector<HalDspOverlay>& overlays,
                            HalDspBlendParams& p) {
    /* pinned[0] is the NV12 base (composited in place); pinned[1+i] are
     * the ARGB32 overlays, placed by rects[i]. Vector storage per call —
     * a shared static was the aliasing bug fixed in 4c65a595. */
    p.base = job->pinned[0]->fb;
    overlays.resize(job->desc.rects.size());
    for (size_t i = 0; i < job->desc.rects.size(); ++i) {
        const DspRect& r = job->desc.rects[i];
        overlays[i].overlay = job->pinned[1 + i]->fb;
        overlays[i].x_offset = static_cast<int32_t>(r.x);
        overlays[i].y_offset = static_cast<int32_t>(r.y);
    }
    p.overlays = overlays.data();
    p.overlay_count = static_cast<uint32_t>(overlays.size());
    return DSP_SVC_OK;
}

void DspService::execute_job(const JobRef& job) {
    auto pin_guard = make_scope_exit([&] {
        unpin_entries(job->pinned);
        job->pinned.clear();
    });
    if (job->abandoned.load()) {
        job->result.rc = DSP_SVC_ERR_UNAVAILABLE;
        return;
    }
    const auto t0 = std::chrono::steady_clock::now();
    int rc = DSP_SVC_ERR_INVALID;
    const char* what = "unhandled op";

    HalDspResizeParams rp{};
    HalDspCropResizeParams crp{};
    HalDspConvertFormatParams cfp{};
    std::vector<HalDspMultiCropOutput> outs;
    HalDspMultiCropResizeParams mcp{};
    std::vector<HalDspOverlay> ovs;
    HalDspBlendParams blp{};

    switch (job->desc.op) {
    case HAL_DSP_OP_RESIZE:
        if (build_resize(job, rp) == DSP_SVC_OK) {
            what = "resize";
            rc = dsp_ops_->resize(dsp_ctx_, &rp);
        }
        break;
    case HAL_DSP_OP_CROP_RESIZE:
        if (build_crop_resize(job, crp) == DSP_SVC_OK) {
            what = "crop_and_resize";
            rc = dsp_ops_->crop_and_resize(dsp_ctx_, &crp);
        }
        break;
    case HAL_DSP_OP_MULTI_CROP_RESIZE:
        if (build_multi_crop(job, outs, mcp) == DSP_SVC_OK) {
            what = "multi_crop_and_resize";
            rc = dsp_ops_->multi_crop_and_resize(dsp_ctx_, &mcp);
        }
        break;
    case HAL_DSP_OP_CONVERT_FORMAT:
        if (build_convert(job, cfp) == DSP_SVC_OK) {
            what = "convert_format";
            rc = dsp_ops_->convert_format(dsp_ctx_, &cfp);
        }
        break;
    case HAL_DSP_OP_BLEND:
        if (build_blend(job, ovs, blp) == DSP_SVC_OK) {
            what = "blend";
            rc = dsp_ops_->blend(dsp_ctx_, &blp);
        }
        break;
    default:
        break; /* unreachable — validate_and_pin gates the op set */
    }

    const auto t1 = std::chrono::steady_clock::now();
    job->result.elapsed_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    if (!job->abandoned) {
        // job->abandoned is written under done_mu_ from another thread
        // (submitter timeout / owner disconnect); it is atomic, so this
        // unlocked read is race-free (worst case a result that was about
        // to be discarded is also counted as failed).
        if (rc == 0) {
            job->result.rc = DSP_SVC_OK;
            job->result.message = what;
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.jobs_ok++;
        } else {
            job->result.rc = rc; /* pass the HAL error through */
            char msg[128];
            std::snprintf(msg, sizeof(msg), "%s failed (HAL rc=%d)", what, rc);
            job->result.message = msg;
            std::lock_guard<std::mutex> lk(stats_mu_);
            stats_.jobs_failed++;
            HAL_LOG_WARNING("DspService: %s", msg);
        }
    }
}

DspServiceStats DspService::stats() const {
    std::lock_guard<std::mutex> lk(stats_mu_);
    return stats_;
}

#ifdef DSP_SERVICE_TESTING
void DspService::set_after_async_pin_hook(std::function<void()> hook) {
    after_async_pin_hook_ = std::move(hook);
}

void DspService::set_after_async_register_hook(std::function<void()> hook) {
    after_async_register_hook_ = std::move(hook);
}

void DspService::set_after_wait_job_lookup_hook(std::function<void()> hook) {
    after_wait_job_lookup_hook_ = std::move(hook);
}

void DspService::set_before_buffer_register_hook(std::function<void()> hook) {
    before_buffer_register_hook_ = std::move(hook);
}

void DspService::set_before_pin_bookkeeping_hook(std::function<void()> hook) {
    before_pin_bookkeeping_hook_ = std::move(hook);
}

void DspService::set_random_id_failure_for_test(bool fail) {
    force_random_id_failure.store(fail);
}

size_t DspService::async_job_count_for_test() {
    std::lock_guard<std::mutex> lk(done_mu_);
    return jobs_.size();
}

size_t DspService::async_owner_slot_count_for_test() {
    std::lock_guard<std::mutex> lk(done_mu_);
    return client_async_jobs_.size();
}

uint32_t DspService::client_async_job_count_for_test(int client_fd) {
    const QuotaKey key = resource_owner_key(quota_owner_key(client_fd));
    std::lock_guard<std::mutex> lk(done_mu_);
    const auto it = client_async_jobs_.find(key);
    return it == client_async_jobs_.end() ? 0 : it->second;
}

bool DspService::quota_try_consume_process_for_test(
    int pid, uint64_t process_start_time_ticks) {
    std::string why;
    return quota_try_consume(
        QuotaKey::for_process(pid, process_start_time_ticks), 0.0, why);
}

bool DspService::quota_try_consume_legacy_fd_for_test(int fd) {
    std::string why;
    return quota_try_consume(QuotaKey::for_legacy_fd(fd), 0.0, why);
}

uint64_t DspService::retained_buffer_bytes_for_test() {
    std::lock_guard<std::mutex> lk(buffers_mu_);
    return total_buffer_bytes_;
}
#endif
