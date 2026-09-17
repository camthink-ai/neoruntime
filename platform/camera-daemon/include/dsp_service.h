/**
 * @file dsp_service.h
 * @brief App-facing DSP job service (PLAT-1/4/5 of the DSP-offload roadmap).
 *
 * Owns the HAL DSP contexts used for app-submitted jobs and the
 * dma-buf buffer registry those jobs reference. P1-9 lane split: one
 * context per priority lane (NORMAL at cfg.device_priority, BACKGROUND
 * at 0) — the vendor executes higher-priority QUEUED work first
 * (dsp_set_priority), so normal-lane app jobs stop queueing behind
 * 30fps stream-side DSP tasks. Still context model D1 from
 * docs/proposals/dsp-offload.md: the extra context buys ordering, not
 * parallelism (measured speedup 1.00x — the vendor PriorityQueueSingleton
 * serializes all DSP work process-wide), so this service never inits
 * per-client contexts. dpm_worker keeps its own context (priority 0);
 * P0 deliberately does not touch dpm.
 *
 * Transport split:
 *  - Buffer plane (FdPublisher UDS, fds via SCM_RIGHTS):
 *      alloc_buffers / release_buffer / release_client_buffers
 *  - Job plane (gRPC SubmitDspJob[/Async/WaitDspJob], buffers by id):
 *      submit_job / submit_job_async / wait_job
 *
 * Scheduling (PLAT-4): single serialized worker thread; NORMAL jobs drain
 * before BACKGROUND; per-owner token-bucket quota (jobs/s + MPix/s); size
 * caps at validation; watchdog timeout on the caller side (an in-flight
 * vendor op cannot be cancelled — its result is discarded and logged).
 *
 * Buffer ids are process-unique, monotonically increasing and never
 * reused. Buffers are refcount-pinned by queued/running jobs: a release
 * detaches the id from the registry immediately (new jobs fail to resolve
 * it); the underlying HAL buffer is then parked in the retention cache
 * (cfg.pool_retention_*) — the next same-geometry alloc reuses it — or,
 * with retention disabled/full, freed when the last pin drops. A parked
 * buffer keeps its dma fds alive: a client writing through fds it kept
 * past release() can corrupt the buffer's next owner.
 */

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/hal_buffer.h"
#include "common/hal_common.h"
#include "dsp/hal_dsp.h"

/** Service-level error codes (HAL ops keep their own negative codes). */
enum DspServiceError {
    DSP_SVC_OK = 0,
    DSP_SVC_ERR_INVALID = -1,      /* validation failed (see message)   */
    DSP_SVC_ERR_NO_BUFFER = -2,    /* unknown or foreign buffer id      */
    DSP_SVC_ERR_QUOTA = -3,        /* per-app jobs/s or MPix/s budget   */
    DSP_SVC_ERR_TIMEOUT = -4,      /* job exceeded job_timeout_ms       */
    DSP_SVC_ERR_UNAVAILABLE = -5,  /* service not started / no ctx      */
    DSP_SVC_ERR_NO_MEM = -6,       /* buffer allocation failed          */
    DSP_SVC_ERR_LIMIT = -7,        /* per-client buffer count/pixel cap */
};

struct DspServiceConfig {
    /* PLAT-3: batch cap. 128 verified all-written on device, 260 silently
     * truncates — 64 keeps sync-RPC latency bounded under load. */
    uint32_t max_batch = 64;
    /* Max pixels per op: source plane and (summed) destination planes. */
    uint64_t max_pixels_per_op = 8294400; /* 3840*2160 */
    /* PLAT-4 quota anchors (dma-buf figures, per owning client):
     * single-op resize ~1500 ops/s, multi-crop N=7 ~6500 rects/s.
     * MPix/s: sized for a 4K@30 derived pipeline (8.3 MPix × 30 ≈
     * 249) — the 120 of the 720p era starved any sustained 4K flow
     * (per-call pool reuse made 4K resize ~42ms ≈ 246 MPix/s and the
     * bucket rejected half the calls). Total stays 480: two saturated
     * 4K clients exactly fill it, a third gets throttled globally. */
    double quota_jobs_per_sec = 100.0;
    double quota_mpix_per_sec = 240.0;
    double quota_total_jobs_per_sec = 400.0;
    double quota_total_mpix_per_sec = 480.0;
    uint32_t job_timeout_ms = 2000;
    /* Registry caps per owning process incarnation. The legacy `pixels`
     * config names are ABI/config compatibility aliases for retained bytes.
     * Allocation accounting uses the actual returned plane sizes and charges
     * the HAL geometry pool capacity, not only the requested buffer count. */
    uint32_t max_buffers_per_client = 128;
    uint64_t max_client_pixels = 2147483648ULL; /* 2 GiB retained pool capacity */
    uint32_t max_total_buffers = 512;
    uint64_t max_total_buffer_pixels = 4294967296ULL; /* 4 GiB service-wide */
    /* Imports pin descriptors and may copy unsealed USERPTR planes into sealed
     * daemon-owned memfds. Bound both count and declared bytes before any dup,
     * copy, mmap, or allocation. */
    uint32_t max_imports_per_client = 64;
    uint64_t max_import_bytes_per_client = 67108864; /* 64 MiB */
    uint32_t max_total_imports = 256;
    uint64_t max_total_import_bytes = 268435456; /* 256 MiB */
    /* Cross-process read leases (DSP_LOOKUP) per connection. A lease pins
     * the buffer and holds dup'd fds until released; a stuck borrower must
     * not be able to pin the registry unboundedly. */
    uint32_t max_lookups_per_client = 16;
    /* P2: outstanding SubmitDspJobAsync jobs per owner and service. Bounds
     * the registry/queues (each entry holds a JobItem + pins until retired). */
    uint32_t max_async_jobs_per_client = 32;
    uint32_t max_total_async_jobs = 128;
    /* Bound synchronous WaitDspJob handlers independently of job count. */
    uint32_t max_waiters_per_job = 4;
    uint32_t max_total_waiters = 64;
    uint32_t max_wait_job_timeout_ms = 30000;
    /* P1-9: vendor device priority of the NORMAL lane's HAL context
     * (dsp_set_priority — the vendor runs higher-priority queued work
     * first). 1 puts app jobs ahead of same-priority stream-side DSP work
     * (dpm resizes, encoder); 0 restores the pre-split flat ordering.
     * The BACKGROUND lane always inits at 0 — a bulk lane must never
     * preempt the encoder. */
    int device_priority = 1;
    /* P1-9 tail fix: released pool buffers are parked by geometry for this
     * grace period instead of freed to HAL, and alloc_buffers reuses them
     * first — each skip avoids a whole HAL pool-chunk destroy/create round
     * (the 43ms resize tail). Parking ONE buffer keeps the vendor pool
     * chunk alive, so footprint is accounted per chunk (32 buffers), not
     * per parked buffer. Either field 0 disables retention entirely
     * (rollback knob: free-on-last-pin, the pre-fix behavior). */
    uint32_t pool_retention_ms = 3000;
    uint64_t pool_retention_max_bytes = 192ULL << 20;
};

/** Job priority. P0 has two levels; platform (daemon-internal) jobs are
 *  expected to bypass this service entirely until PLAT-1's full merge. */
enum class DspPriority { Background = 0, Normal = 1 };

struct DspRect {
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t width = 0;  /* ROI size on the source (pixels)         */
    uint32_t height = 0;
    uint32_t dst_width = 0;  /* expected dst buffer dims (validated) */
    uint32_t dst_height = 0;
};

/** Plain-struct mirror of the proto request — keeps HAL/proto decoupled. */
struct DspJobDesc {
    HalDspOpType op = HAL_DSP_OP_RESIZE;
    uint64_t src_id = 0;
    std::vector<uint64_t> dst_ids;
    std::vector<DspRect> rects;
    HalDspInterpolation interpolation = HAL_DSP_INTERPOLATION_BILINEAR;
    HalDspScalingMode scaling_mode = HAL_DSP_SCALING_STRETCH;
    DspPriority priority = DspPriority::Normal;
};

struct DspJobResult {
    int rc = DSP_SVC_OK;       /* DspServiceError or pass-through HAL rc */
    uint32_t elapsed_ms = 0;   /* time spent inside execute (worker)     */
    std::string message;
};

struct DspServiceStats {
    uint64_t jobs_ok = 0;
    uint64_t jobs_rejected = 0; /* validation + quota failures           */
    uint64_t jobs_failed = 0;   /* HAL op returned non-zero              */
    uint64_t jobs_timed_out = 0;
    uint64_t buffers_allocated = 0;
    uint64_t buffers_released = 0;
    uint64_t buffers_in_registry = 0;
    /* P1-9 pool retention (cfg.pool_retention_*). */
    uint64_t retention_reuses = 0; /* alloc served from a parked buffer   */
    uint64_t retention_releases = 0; /* parks dropped to HAL: expired,
                                      * cap-evicted, or stop-flushed      */
    uint64_t retention_parked = 0; /* gauge: buffers parked right now     */
};

class DspService {
public:
    DspService(HalDspOps* dsp_ops, HalFrameBufferOps* fb_ops,
               const DspServiceConfig& cfg = DspServiceConfig());
    ~DspService() noexcept;

    DspService(const DspService&) = delete;
    DspService& operator=(const DspService&) = delete;

    /** Init the HAL DSP context and start the worker thread. */
    bool start();
    /**
     * Stop workers and synchronously drain lifecycle users. This blocks until
     * every BufferPin is released; callers must release pins held by the
     * calling thread before invoking stop() or destroying the service.
     */
    void stop();
    bool is_running() const { return running_.load(); }

    /* ---------------- Buffer plane (FdPublisher client thread) -------- */

    struct AllocResult {
        int rc = DSP_SVC_OK;
        std::string message;
        std::vector<uint64_t> ids;   /* count ids, in order               */
        std::vector<int> fds;        /* count * num_planes fds, buffer-major */
        uint32_t num_planes = 0;
        uint32_t strides[HAL_MAX_PLANES] = {0, 0, 0};
        uint32_t sizes[HAL_MAX_PLANES] = {0, 0, 0};
    };

    /** Allocate `count` dma-buf HalFrameBuffers owned by `client_fd`. */
    AllocResult alloc_buffers(int client_fd, uint32_t width, uint32_t height,
                              HalPixelFormat format, uint32_t count);

    struct ImportResult {
        int rc = DSP_SVC_OK;
        std::string message;
        uint64_t id = 0;
    };

    /** Zero-copy source: register client-supplied dma-buf fds as a job-usable
     *  buffer. The fds are dup'd (caller keeps ownership of its copies); the
     *  returned id lives in the same namespace as alloc ids — passable as
     *  src_buffer_id, freed via release_buffer / release_client_buffers. */
    ImportResult import_buffer(int client_fd, uint32_t width, uint32_t height,
                               HalPixelFormat format, uint32_t num_planes,
                               const uint32_t* strides, const uint32_t* sizes,
                               const int* fds);

    /** Detach one buffer id; HAL buffer freed when the last pin drops. */
    int release_buffer(int client_fd, uint64_t buffer_id);

    /** UDS disconnect hook: detach every buffer owned by the client. */
    void release_client_buffers(int client_fd);

    /* ---------------- Cross-process lookup plane (DSP_LOOKUP) --------- */

    /** Result of lookup_buffer: geometry + dup'd dma-buf plane fds. */
    struct LookupResult {
        int rc = DSP_SVC_OK;
        std::string message;
        uint32_t width = 0;
        uint32_t height = 0;
        HalPixelFormat format = HAL_PIX_FMT_NV12;
        uint32_t num_planes = 0;
        uint32_t strides[HAL_MAX_PLANES] = {0, 0, 0};
        uint32_t sizes[HAL_MAX_PLANES] = {0, 0, 0};
        std::vector<int> fds;       /* num_planes dup'd fds; caller closes  */
    };

    /**
     * Resolve a registry buffer_id for a NON-owning connection and pin it
     * (lease) for that connection: the underlying frame stays alive even if
     * the owning client disconnects mid-lease. Serves HAL_MEM_DMABUF buffers
     * only — pool buffers and imported dma-buf frames; a memfd/USERPTR
     * import is refused with DSP_SVC_ERR_INVALID. The returned fds are
     * fresh dup()s of the frame's dma-buf planes; closing them does not end
     * the lease (lookup_release does). Caps at cfg.max_lookups_per_client
     * outstanding leases per connection.
     */
    LookupResult lookup_buffer(int client_fd, uint64_t buffer_id);

    /** Drop one lease taken by this connection (unknown id: no-op error). */
    int lookup_release(int client_fd, uint64_t buffer_id);

    /** UDS disconnect hook: drop every lease this connection took. */
    void lookup_release_all(int client_fd);

    /* ---------------- One-shot ops plane (daemon-internal) ----------- */

    /**
     * RAII pin of one registered buffer, for daemon-internal one-shot ops
     * (EncodeImage). Same lifecycle semantics as job pins: a concurrent
     * release detaches the id; the HAL buffer stays alive until this pin
     * drops. Move-only. A pin must not be retained by a thread that calls
     * stop() or destroys the owning DspService, because shutdown waits for it.
     */
    class BufferPin {
    public:
        BufferPin() = default;
        ~BufferPin() noexcept;
        BufferPin(BufferPin&& other) noexcept;
        BufferPin& operator=(BufferPin&& other) noexcept;
        BufferPin(const BufferPin&) = delete;
        BufferPin& operator=(const BufferPin&) = delete;

        bool ok() const { return rc_ == DSP_SVC_OK; }
        int rc() const { return rc_; }             /* DspServiceError */
        HalFrameBuffer* fb() const { return fb_; } /* valid while held */
        int owner_fd() const { return owner_fd_; } /* quota owner     */

    private:
        friend class DspService;
        void release_pin() noexcept;

        DspService* svc_ = nullptr;
        void* entry_ = nullptr; /* BufferEntry* — opaque outside the .cpp */
        HalFrameBuffer* fb_ = nullptr;
        int owner_fd_ = -1;
        int rc_ = DSP_SVC_ERR_NO_BUFFER;
    };

    /**
     * Pin one buffer id for a daemon-internal op. Returns a handle whose
     * fb() is the registered HalFrameBuffer (valid until the handle is
     * destroyed) or rc() == DSP_SVC_ERR_NO_BUFFER.
     */
    BufferPin pin_buffer(uint64_t buffer_id);

    /* ---------------- Job plane (gRPC worker thread) ------------------ */

    /**
     * Validate, enqueue and wait for one job (up to cfg.job_timeout_ms).
     * On timeout the job may still be executing — destination buffers
     * must be considered undefined until a later successful job.
     */
    DspJobResult submit_job(const DspJobDesc& desc);

    /**
     * P2 async form: validate, enqueue and return immediately. On success
     * `job_id_out` receives the registry id for wait_job(). The job pins
     * its buffers until it completes AND is waited (or is reaped below);
     * the per-owner outstanding count is capped by
     * cfg.max_async_jobs_per_client.
     */
    DspJobResult submit_job_async(const DspJobDesc& desc, uint64_t& job_id_out);

    /**
     * Wait for an async job. timeout_ms 0 = non-blocking poll. On done the
     * result is returned and the registry entry reaped; on timeout the
     * entry stays valid (re-wait later) and rc is DSP_SVC_ERR_TIMEOUT with
     * *done_out = false. Unknown/reaped ids return DSP_SVC_ERR_NO_BUFFER.
     */
    DspJobResult wait_job(uint64_t job_id, uint32_t timeout_ms, bool& done_out);
    uint32_t max_wait_job_timeout_ms() const noexcept {
        return cfg_.max_wait_job_timeout_ms;
    }

    DspServiceStats stats() const;

#ifdef DSP_SERVICE_TESTING
    /** Test-only fences around async registration. */
    void set_after_async_pin_hook(std::function<void()> hook);
    void set_after_async_register_hook(std::function<void()> hook);
    void set_after_wait_job_lookup_hook(std::function<void()> hook);
    void set_before_buffer_register_hook(std::function<void()> hook);
    void set_before_pin_bookkeeping_hook(std::function<void()> hook);
    void set_random_id_failure_for_test(bool fail);
    size_t async_job_count_for_test();
    size_t async_owner_slot_count_for_test();
    uint32_t client_async_job_count_for_test(int client_fd);
    bool quota_try_consume_process_for_test(int pid,
                                            uint64_t process_start_time_ticks);
    bool quota_try_consume_legacy_fd_for_test(int fd);
    uint64_t retained_buffer_bytes_for_test();
#endif

private:
    enum class LifecycleState : uint8_t { Stopped, Running, Stopping };

    AllocResult alloc_buffers_impl(int client_fd, uint32_t width,
                                   uint32_t height, HalPixelFormat format,
                                   uint32_t count);
    ImportResult import_buffer_impl(int client_fd, uint32_t width,
                                    uint32_t height, HalPixelFormat format,
                                    uint32_t num_planes,
                                    const uint32_t* strides,
                                    const uint32_t* sizes, const int* fds);
    DspJobResult submit_job_impl(const DspJobDesc& desc);
    DspJobResult submit_job_async_impl(const DspJobDesc& desc,
                                       uint64_t& job_id_out);
    DspJobResult wait_job_impl(uint64_t job_id, uint32_t timeout_ms,
                               bool& done_out);

    struct ClientRegistration {
        int fd = -1;
        uint64_t generation = 0;

        bool operator==(const ClientRegistration& other) const noexcept {
            return fd == other.fd && generation == other.generation;
        }
    };

    struct ClientRegistrationHash {
        size_t operator()(const ClientRegistration& registration) const noexcept;
    };

    struct ClientSession {
        uint64_t generation = 0;
        uint64_t device = 0;
        uint64_t inode = 0;
        bool connected = false;
    };

    bool begin_async_submission();
    void end_async_submission() noexcept;
    int begin_buffer_registration(int client_fd,
                                  ClientRegistration& registration) noexcept;
    void end_buffer_registration() noexcept;
    bool client_registration_active_locked(
        const ClientRegistration& registration) const noexcept;
    bool begin_buffer_pin() noexcept;
    void end_buffer_pin() noexcept;

    struct AsyncSubmissionGuard {
        explicit AsyncSubmissionGuard(DspService* service_in)
            : service(service_in) {}
        AsyncSubmissionGuard(const AsyncSubmissionGuard&) = delete;
        AsyncSubmissionGuard& operator=(const AsyncSubmissionGuard&) = delete;
        ~AsyncSubmissionGuard() noexcept {
            if (service) service->end_async_submission();
        }
        DspService* service;
    };

    struct BufferRegistrationGuard {
        explicit BufferRegistrationGuard(DspService* service_in)
            : service(service_in) {}
        BufferRegistrationGuard(const BufferRegistrationGuard&) = delete;
        BufferRegistrationGuard& operator=(const BufferRegistrationGuard&) = delete;
        ~BufferRegistrationGuard() noexcept {
            if (service) service->end_buffer_registration();
        }
        DspService* service;
    };

    struct QuotaKey {
        enum class Kind : uint8_t { LegacyFd, Process };

        Kind kind = Kind::LegacyFd;
        int legacy_fd = -1;
        int pid = 0;
        uint64_t process_start_time_ticks = 0;

        static QuotaKey for_legacy_fd(int fd) {
            return {Kind::LegacyFd, fd, 0, 0};
        }
        static QuotaKey for_process(int process_pid, uint64_t start_time_ticks) {
            return {Kind::Process, -1, process_pid, start_time_ticks};
        }
        bool operator==(const QuotaKey& other) const {
            return kind == other.kind && legacy_fd == other.legacy_fd &&
                   pid == other.pid &&
                   process_start_time_ticks == other.process_start_time_ticks;
        }
    };

    struct QuotaKeyHash {
        size_t operator()(const QuotaKey& key) const noexcept;
    };

    struct BufferPoolKey {
        uint32_t width = 0;
        uint32_t height = 0;
        HalPixelFormat format = HAL_PIX_FMT_NV12;
        uint32_t max_buffers = 0;
        uint64_t bytes_per_buffer = 0;

        bool operator==(const BufferPoolKey& other) const noexcept {
            return width == other.width && height == other.height &&
                   format == other.format && max_buffers == other.max_buffers &&
                   bytes_per_buffer == other.bytes_per_buffer;
        }
    };

    struct BufferPoolKeyHash {
        size_t operator()(const BufferPoolKey& key) const noexcept;
    };

    struct BufferPoolOwnerKey {
        QuotaKey owner = QuotaKey::for_legacy_fd(-1);
        BufferPoolKey pool;

        bool operator==(const BufferPoolOwnerKey& other) const noexcept {
            return owner == other.owner && pool == other.pool;
        }
    };

    struct BufferPoolOwnerKeyHash {
        size_t operator()(const BufferPoolOwnerKey& key) const noexcept;
    };

    struct BufferEntry {
        uint64_t id = 0;
        int client_fd = -1;
        ClientRegistration client_registration;
        QuotaKey quota_key = QuotaKey::for_legacy_fd(-1);
        QuotaKey resource_key = QuotaKey::for_legacy_fd(-1);
        BufferPoolKey pool_key;
        HalFrameBuffer* fb = nullptr;
        uint64_t retained_import_bytes = 0;
        uint32_t pins = 0;      /* held by queued/running jobs            */
        bool detached = false;  /* removed from registry, pending free    */
        bool imported = false;  /* DSP_IMPORT descriptor, not a HAL pool buffer */
    };

    struct JobItem {
        DspJobDesc desc;
        DspPriority priority = DspPriority::Normal;
        int owner_fd = -1;         /* lifecycle owner (src buffer client) */
        ClientRegistration owner_registration;
        QuotaKey quota_key = QuotaKey::for_legacy_fd(-1);
        QuotaKey resource_key = QuotaKey::for_legacy_fd(-1);
        double charge_mpix = 0.0;
        std::vector<BufferEntry*> pinned; /* resolved at validation       */
        DspJobResult result;
        std::string cancellation_message;
        bool done = false;
        uint32_t active_waiters = 0;       /* guarded by done_mu_          */
        bool async_slot_released = false; /* guarded by done_mu_          */
        /* Set by another thread (submitter timeout / owner disconnect)
         * while the worker may be reading it at the execute_job tail —
         * atomic because a plain bool read there is a data race, however
         * "benign" the outcome looks. */
        std::atomic<bool> abandoned{false};
    };
    using JobRef = std::shared_ptr<JobItem>;

    struct QuotaBucket {
        double jobs = 0.0;
        double mpix = 0.0;
        std::chrono::steady_clock::time_point last;
    };

    struct RetainedUsage {
        uint32_t buffers = 0;
        uint64_t buffer_bytes = 0;
        uint32_t pending_buffers = 0;
        uint64_t pending_buffer_bytes = 0;
        uint32_t imports = 0;
        uint64_t import_bytes = 0;
    };

    struct BufferAdmission {
        QuotaKey owner = QuotaKey::for_legacy_fd(-1);
        uint32_t buffers = 0;
        uint64_t owner_bytes = 0;
        uint64_t total_bytes = 0;
        bool active = false;
    };

    struct BufferPoolUsage {
        uint32_t refs = 0;
        uint64_t retained_bytes = 0;
    };

    // Validation + resolution (caller: any thread; takes registry lock).
    int validate_and_pin(const DspJobDesc& desc, JobRef& job_out,
                         std::string& why);
    bool resolve_pin_buffer(uint64_t id, int& owner_fd_out, BufferEntry*& entry);
    void unpin_entry(BufferEntry* entry) noexcept;
    void unpin_entries(const std::vector<BufferEntry*>& entries) noexcept;
    struct DetachedFrame {
        HalFrameBuffer* fb = nullptr;
        bool imported = false;
    };
    /* caller holds buffers_mu_; returned frame is released after unlock */
    DetachedFrame detach_entry_locked(BufferEntry* entry) noexcept;
    static void release_detached_frame(HalFrameBufferOps* fb_ops,
                                       DetachedFrame frame) noexcept;
    QuotaKey resource_owner_key(const QuotaKey& quota_key) const;
    int reserve_buffer_admission(const QuotaKey& resource_key,
                                 uint32_t width, uint32_t height,
                                 HalPixelFormat format, uint32_t max_buffers,
                                 uint32_t count, BufferAdmission& admission,
                                 std::string& why) noexcept;
    void release_buffer_admission(BufferAdmission& admission) noexcept;
    void release_buffer_admission_locked(BufferAdmission& admission) noexcept;
    int reserve_buffer_usage_locked(const QuotaKey& resource_key,
                                    const BufferPoolKey& pool_key,
                                    uint32_t count, std::string& why) noexcept;
    void release_buffer_usage_locked(const QuotaKey& resource_key,
                                     const BufferPoolKey& pool_key,
                                     uint32_t count) noexcept;
    int reserve_import_usage(const QuotaKey& resource_key, uint64_t bytes,
                             std::string& why) noexcept;
    void release_import_usage(const QuotaKey& resource_key, uint32_t count,
                              uint64_t bytes) noexcept;
    void release_entry_usage_locked(const BufferEntry* entry) noexcept;

    bool quota_try_consume(const QuotaKey& quota_key, double mpix,
                           std::string& why);
    void quota_forget(int owner_fd);
    /* Resolve a live UDS peer once at buffer registration. A process key uses
     * both SO_PEERCRED PID and /proc/<pid>/stat start time, so connections from
     * one process share quota while a later PID incarnation cannot inherit it.
     * If either identity component is unavailable, preserve legacy per-fd
     * behavior in a separately tagged namespace. */
    QuotaKey quota_owner_key(int owner_fd);

    void release_async_job_slot_locked(const JobRef& job) noexcept;
    void cancel_queued_jobs(const ClientRegistration& registration) noexcept;
    void worker_loop();
    void execute_job(const JobRef& job);

    // Fill `params` from a pinned job; returns 0 or DSP_SVC_ERR_INVALID.
    // Params point straight at pinned HalFrameBuffers — only called on the
    // worker thread while pins are held.
    int build_resize(const JobRef& job, HalDspResizeParams& p);
    int build_crop_resize(const JobRef& job, HalDspCropResizeParams& p);
    int build_multi_crop(const JobRef& job,
                         std::vector<HalDspMultiCropOutput>& outputs,
                         HalDspMultiCropResizeParams& p);
    int build_convert(const JobRef& job, HalDspConvertFormatParams& p);
    int build_blend(const JobRef& job,
                    std::vector<HalDspOverlay>& overlays,
                    HalDspBlendParams& p);

    static uint64_t pixels_of(uint32_t w, uint32_t h) {
        return static_cast<uint64_t>(w) * static_cast<uint64_t>(h);
    }

    HalDspOps* dsp_ops_ = nullptr;
    HalFrameBufferOps* fb_ops_ = nullptr;
    DspServiceConfig cfg_;

    /* P1-9 per-lane HAL contexts — vendor-level queue PRIORITY, not
     * parallelism: the vendor PriorityQueueSingleton still serializes all
     * DSP execution process-wide; ordering of queued work is the lever.
     * dsp_ctx_normal_ inits at cfg_.device_priority, dsp_ctx_background_
     * at 0. dsp_ctx_background_ may stay null (init failure → BACKGROUND
     * jobs degrade onto the normal lane; service still starts). */
    void* dsp_ctx_normal_ = nullptr;
    void* dsp_ctx_background_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::mutex lifecycle_mu_;
    std::condition_variable lifecycle_cv_;
    LifecycleState lifecycle_state_ = LifecycleState::Stopped;
    size_t active_async_submissions_ = 0;
    size_t active_buffer_registrations_ = 0;
    size_t active_buffer_pins_ = 0;
    uint64_t next_client_generation_ = 1;
    std::unordered_map<int, ClientSession> client_sessions_;

    // Job queue (FIFO; NORMAL drains before BACKGROUND).
    std::mutex q_mu_;
    std::condition_variable q_cv_;
    std::deque<JobRef> q_normal_;
    std::deque<JobRef> q_background_;

    // Completion signalling for in-flight submit_job callers and the P2
    // async job registry (jobs_ keyed by unpredictable random job ids — see
    // fresh_random_id in dsp_service.cpp; entries hold pins until
    // waited-to-completion or reaped on owner disconnect / stop).
    std::mutex done_mu_;
    std::condition_variable done_cv_;
    std::unordered_map<uint64_t, JobRef> jobs_;
    std::unordered_map<QuotaKey, uint32_t, QuotaKeyHash> client_async_jobs_;
    uint32_t total_async_jobs_ = 0;
    uint32_t total_waiters_ = 0;
#ifdef DSP_SERVICE_TESTING
    std::function<void()> after_async_pin_hook_;
    std::function<void()> after_async_register_hook_;
    std::function<void()> after_wait_job_lookup_hook_;
    std::function<void()> before_buffer_register_hook_;
    std::function<void()> before_pin_bookkeeping_hook_;
#endif

    // Buffer registry (keys are unpredictable random ids; 0 is never valid).
    std::mutex buffers_mu_;
    std::unordered_map<uint64_t, BufferEntry*> buffers_;
    std::unordered_map<QuotaKey, RetainedUsage, QuotaKeyHash> retained_usage_;
    std::unordered_map<BufferPoolKey, BufferPoolUsage, BufferPoolKeyHash>
        buffer_pools_;
    std::unordered_map<BufferPoolOwnerKey, uint32_t,
                       BufferPoolOwnerKeyHash>
        buffer_pool_owner_refs_;
    uint32_t total_buffers_ = 0;
    uint64_t total_buffer_bytes_ = 0;
    uint32_t pending_buffers_ = 0;
    uint64_t pending_buffer_bytes_ = 0;
    uint32_t total_imports_ = 0;
    uint64_t total_import_bytes_ = 0;

    // P1-9 pool retention: released NON-imported pool buffers parked by
    // geometry {width, height, format} for cfg.pool_retention_ms, reused
    // by same-geometry allocs. All state below lives under buffers_mu_;
    // HAL release calls happen on collected lists AFTER unlocking, per
    // the file-head locking model in dsp_service.cpp.
    using ParkedGeometry = std::array<uint32_t, 3>; /* {w, h, fmt} */
    struct ParkedBuffer {
        HalFrameBuffer* fb;
        std::chrono::steady_clock::time_point deadline;
        uint64_t chunk_bytes; /* full pool-chunk cost parked with this
                                * buffer — the vendor chunk it keeps alive */
    };
    std::map<ParkedGeometry, std::deque<ParkedBuffer>> parked_;
    std::deque<ParkedGeometry> parked_order_; /* first-park order — whole-
                                               * geometry LRU eviction */
    uint64_t parked_footprint_ = 0; /* chunk-accurate bytes parked        */
    /* Queued by park_or_free_locked under buffers_mu_ (refused inputs +
     * eviction victims + the retention-disabled case); released to HAL by
     * drain_pending_releases() after the lock drops. Reserved in start()
     * because detach_entry_locked pushes here on a noexcept path. */
    std::vector<HalFrameBuffer*> pending_release_;
    /* caller holds buffers_mu_; parks fb (chunk_bytes = the full pool
     * chunk this buffer keeps alive) or queues it into pending_release_,
     * then evicts whole oldest geometries while parked_footprint_ exceeds
     * the cap (evictions queued the same way). Refuses when not running,
     * retention is disabled, or the chunk alone exceeds the cap — fb is
     * queued for release instead. */
    void park_or_free_locked(const ParkedGeometry& g, HalFrameBuffer* fb,
                             uint64_t chunk_bytes);
    /* swaps pending_release_ under buffers_mu_ and HAL-releases outside
     * it; call only with buffers_mu_ NOT held */
    void drain_pending_releases() noexcept;
    /* takes buffers_mu_ itself (alloc path runs lock-free): drops expired
     * front entries of g (HAL-released after unlock), pops one reusable
     * buffer or returns nullptr. Counts retention_reuses on a hit. */
    HalFrameBuffer* take_parked(const ParkedGeometry& g);
    /* caller holds buffers_mu_; moves every expired park to to_free. */
    void sweep_parked_locked(std::vector<HalFrameBuffer*>& to_free);
    /* Retention-oriented HAL pool chunk size for a geometry: as many
     * buffers as the parking budget holds, floored at the hal app-pool
     * default and ceilinged at the fd ceiling; the fd ceiling itself
     * when retention is off or the geometry won't estimate. Defined in
     * dsp_service.cpp next to the constants it clamps between. */
    uint32_t retention_pool_chunk_n(uint32_t width, uint32_t height,
                                    HalPixelFormat format) const noexcept;

    // Cross-process lookup leases (DSP_LOOKUP): per-connection map of
    // buffer_id → pins taken by that connection. OWN mutex: pin_buffer and
    // ~BufferPin take buffers_mu_, so holding lookup_mu_ across a pin/unpin
    // would invert the lock order against disconnect paths. All methods
    // move pins in/out under lookup_mu_ only, never both at once.
    std::mutex lookup_mu_;
    std::unordered_map<int, std::unordered_map<uint64_t, std::vector<BufferPin>>>
        lookup_leases_;

    // Per-owner token buckets.
    std::mutex quota_mu_;
    std::unordered_map<QuotaKey, QuotaBucket, QuotaKeyHash> quotas_;
    QuotaBucket global_quota_;

    // Stats.
    mutable std::mutex stats_mu_;
    DspServiceStats stats_;
};
