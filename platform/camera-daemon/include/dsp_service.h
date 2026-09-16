/**
 * @file dsp_service.h
 * @brief App-facing DSP job service (PLAT-1/4/5 of the DSP-offload roadmap).
 *
 * Owns the single HAL DSP context used for app-submitted jobs and the
 * dma-buf buffer registry those jobs reference. Context model is D1 from
 * docs/proposals/dsp-offload.md: one context, daemon-multiplexed — extra
 * in-process contexts buy zero parallelism (measured speedup 1.00x), so
 * this service never inits per-client contexts. dpm_worker keeps its own
 * context for now; the vendor PriorityQueueSingleton serializes all DSP
 * work process-wide regardless (P0 deliberately does not touch dpm).
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
 * it) and the underlying HAL buffer is freed when the last pin drops.
 */

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#ifdef DSP_SERVICE_TESTING
#include <functional>
#endif
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
     * single-op resize ~1500 ops/s, multi-crop N=7 ~6500 rects/s. */
    double quota_jobs_per_sec = 100.0;
    double quota_mpix_per_sec = 120.0;
    uint32_t job_timeout_ms = 2000;
    /* Registry caps per owning UDS client. 16 MPix ≈ 24 MB NV12. */
    uint32_t max_buffers_per_client = 128;
    uint64_t max_client_pixels = 16777216; /* 16 MPix outstanding */
    /* Zero-copy source imports (DSP_IMPORT) per client. Imports hold no
     * daemon pixel budget — the memory is the client's own — but each one
     * pins a descriptor and dup'd fds, so cap them separately. */
    uint32_t max_imports_per_client = 64;
    /* P2: outstanding SubmitDspJobAsync jobs per owner. Bounds the jobs_
     * registry (each entry holds a JobItem + pins until waited/reaped). */
    uint32_t max_async_jobs_per_client = 32;
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
};

class DspService {
public:
    DspService(HalDspOps* dsp_ops, HalFrameBufferOps* fb_ops,
               const DspServiceConfig& cfg = DspServiceConfig());
    ~DspService();

    DspService(const DspService&) = delete;
    DspService& operator=(const DspService&) = delete;

    /** Init the HAL DSP context and start the worker thread. */
    bool start();
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

    /* ---------------- One-shot ops plane (daemon-internal) ----------- */

    /**
     * RAII pin of one registered buffer, for daemon-internal one-shot ops
     * (EncodeImage). Same lifecycle semantics as job pins: a concurrent
     * release detaches the id; the HAL buffer stays alive until this pin
     * drops. Move-only.
     */
    class BufferPin {
    public:
        BufferPin() = default;
        ~BufferPin();
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
        void release_pin();

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

    DspServiceStats stats() const;

#ifdef DSP_SERVICE_TESTING
    /** Test-only fence immediately after async registration and before enqueue. */
    void set_after_async_register_hook(std::function<void()> hook);
    size_t async_job_count_for_test();
    uint32_t client_async_job_count_for_test(int client_fd);
#endif

private:
    struct BufferEntry {
        uint64_t id = 0;
        int client_fd = -1;
        HalFrameBuffer* fb = nullptr;
        uint32_t pins = 0;      /* held by queued/running jobs            */
        bool detached = false;  /* removed from registry, pending free    */
        bool imported = false;  /* DSP_IMPORT descriptor, not a HAL pool buffer */
    };

    struct JobItem {
        DspJobDesc desc;
        DspPriority priority = DspPriority::Normal;
        int owner_fd = -1;         /* quota owner (src buffer's client)   */
        double charge_mpix = 0.0;
        std::vector<BufferEntry*> pinned; /* resolved at validation       */
        DspJobResult result;
        bool done = false;
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

    // Validation + resolution (caller: any thread; takes registry lock).
    int validate_and_pin(DspJobDesc desc, JobRef& job_out, std::string& why);
    bool resolve_pin_buffer(uint64_t id, int& owner_fd_out, BufferEntry*& entry);
    void unpin_entries(const std::vector<BufferEntry*>& entries);
    /* caller holds buffers_mu_; appends fb to `to_free` when refcount hits 0 */
    void detach_entry_locked(BufferEntry* entry,
                             std::vector<HalFrameBuffer*>& to_free);

    bool quota_try_consume(int owner_fd, double mpix, std::string& why);
    void quota_forget(int owner_fd);

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

    void* dsp_ctx_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};

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
    std::unordered_map<int, uint32_t> client_async_jobs_;
#ifdef DSP_SERVICE_TESTING
    std::function<void()> after_async_register_hook_;
#endif

    // Buffer registry (keys are unpredictable random ids; 0 is never valid).
    std::mutex buffers_mu_;
    std::unordered_map<uint64_t, BufferEntry*> buffers_;
    std::unordered_map<int, uint32_t> client_buffer_count_;
    std::unordered_map<int, uint64_t> client_pixels_;
    std::unordered_map<int, uint32_t> client_import_count_;
    uint64_t next_buffer_id_ = 1; /* starts at 1; 0 is never a valid id */

    // Per-owner token buckets.
    std::mutex quota_mu_;
    std::unordered_map<int, QuotaBucket> quotas_;

    // Stats.
    mutable std::mutex stats_mu_;
    DspServiceStats stats_;
};
