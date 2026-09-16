#include "auto_infer.h"
#include "log.h"

#include <chrono>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <sys/mman.h>
#include <condition_variable>
#include <sstream>
#include <unistd.h>

namespace aipc::ai_runtime {

using SteadyClock  = std::chrono::steady_clock;

namespace {

// Last-resort termination for shutdown paths that cannot quiesce while
// preserving callback lifetimes. _Exit avoids both C++ destructors (late HAL
// callbacks own raw dependencies) and the SIGABRT core dump std::abort would
// produce; the process is exiting either way.
[[noreturn]] void terminate_shutdown_failure(const char* why) {
    LOG_FATAL("AutoInfer: %s; terminating without destructors", why);
    std::_Exit(EXIT_FAILURE);
}

// Process-lifetime token distinguishing event ids across ai-runtime restarts,
// where publisher sequence numbers and monotonic frame timestamps can repeat.
uint64_t process_instance_token() {
    static const uint64_t token = [] {
        const uint64_t pid = static_cast<uint64_t>(::getpid());
        const uint64_t tick = static_cast<uint64_t>(
            SteadyClock::now().time_since_epoch().count());
        return pid * 0x9E3779B97F4A7C15ULL ^ tick;
    }();
    return token;
}

// Event ids must be unique across models on the same stream, publisher
// reconnects, and process restarts: the frame source only guarantees
// uniqueness of (sequence, timestamp) within one publisher connection.
std::string make_event_id(const std::string& model_id,
                          const std::string& stream_id,
                          uint64_t generation,
                          uint64_t frame_seq,
                          uint64_t ts_ns) {
    std::ostringstream os;
    os << "auto-" << std::hex << process_instance_token() << std::dec
       << "-" << model_id << "-" << stream_id
       << "-" << generation << "-" << frame_seq << "-" << ts_ns;
    return os.str();
}

}  // namespace

// ─── Color Space Helpers ──────────────────────────────────────────────────────

static inline uint8_t clamp8(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void nv12_to_rgb_resize(
    const uint8_t* y_plane, const uint8_t* uv_plane,
    int src_w, int src_h, int src_y_stride, int src_uv_stride,
    uint8_t* dst_rgb, int dst_w, int dst_h) {

    for (int y = 0; y < dst_h; y++) {
        int src_y_idx = (y * src_h) / dst_h;
        const uint8_t* src_y_row = y_plane + src_y_idx * src_y_stride;
        const uint8_t* src_uv_row = uv_plane + (src_y_idx / 2) * src_uv_stride;
        uint8_t* dst_row = dst_rgb + y * dst_w * 3;

        for (int x = 0; x < dst_w; x++) {
            int src_x = (x * src_w) / dst_w;
            uint8_t Y = src_y_row[src_x];
            int uv_offset = (src_x / 2) * 2;
            uint8_t U = src_uv_row[uv_offset];
            uint8_t V = src_uv_row[uv_offset + 1];

            int c = Y - 16, d = U - 128, e = V - 128;
            dst_row[x * 3 + 0] = clamp8((298 * c           + 409 * e + 128) >> 8);
            dst_row[x * 3 + 1] = clamp8((298 * c - 100 * d - 208 * e + 128) >> 8);
            dst_row[x * 3 + 2] = clamp8((298 * c + 516 * d           + 128) >> 8);
        }
    }
}

static void nv12_to_nv12_resize(
    const uint8_t* src_y, const uint8_t* src_uv,
    int src_w, int src_h, int src_y_stride, int src_uv_stride,
    uint8_t* dst_y, uint8_t* dst_uv, int dst_w, int dst_h) {

    for (int y = 0; y < dst_h; y++) {
        int sy = (y * src_h) / dst_h;
        for (int x = 0; x < dst_w; x++) {
            int sx = (x * src_w) / dst_w;
            dst_y[y * dst_w + x] = src_y[sy * src_y_stride + sx];
        }
    }

    int dst_uv_stride = dst_w;  // UV plane: dst_w bytes per row
    for (int y = 0; y < dst_h / 2; y++) {
        int sy = (y * src_h) / dst_h;
        for (int x = 0; x < dst_w / 2; x++) {
            int sx = (x * src_w) / dst_w;
            dst_uv[y * dst_uv_stride + x * 2]     = src_uv[sy * src_uv_stride + sx * 2];
            dst_uv[y * dst_uv_stride + x * 2 + 1] = src_uv[sy * src_uv_stride + sx * 2 + 1];
        }
    }
}

// ─── Memory-Mapped NV12 Frame ─────────────────────────────────────────────────
//
// Encapsulates mmap lifetime for DMA-BUF NV12 frames.
// On destruction, automatically unmaps all mapped regions.

struct MappedNV12Frame {
    const uint8_t* y_plane  = nullptr;
    const uint8_t* uv_plane = nullptr;
    size_t y_size  = 0;
    size_t uv_size = 0;

    // Returns true if both planes are available
    explicit operator bool() const { return y_plane && uv_plane; }

    ~MappedNV12Frame() {
        if (!y_plane) return;
        if (uv_plane == y_plane + y_size) {
            // Single contiguous mapping
            ::munmap(const_cast<uint8_t*>(y_plane), y_size + uv_size);
        } else {
            ::munmap(const_cast<uint8_t*>(y_plane), y_size);
            if (uv_plane) ::munmap(const_cast<uint8_t*>(uv_plane), uv_size);
        }
    }

    // Non-copyable, movable
    MappedNV12Frame() = default;
    MappedNV12Frame(const MappedNV12Frame&) = delete;
    MappedNV12Frame& operator=(const MappedNV12Frame&) = delete;
    MappedNV12Frame(MappedNV12Frame&& o) noexcept
        : y_plane(o.y_plane), uv_plane(o.uv_plane), y_size(o.y_size), uv_size(o.uv_size)
    { o.y_plane = o.uv_plane = nullptr; }
    MappedNV12Frame& operator=(MappedNV12Frame&& o) noexcept {
        if (this != &o) {
            this->~MappedNV12Frame();
            y_plane = o.y_plane; uv_plane = o.uv_plane;
            y_size = o.y_size; uv_size = o.uv_size;
            o.y_plane = o.uv_plane = nullptr;
        }
        return *this;
    }

    /// Map Y+UV planes from a ReceivedFrame's DMA-BUF file descriptors.
    static MappedNV12Frame from_frame(const ReceivedFrame& frame) {
        MappedNV12Frame m;
        if (!frame.fd_group || frame.fd_group->fds.empty()) return m;

        int fd0 = frame.fd_group->fds[0];
        int fd1 = frame.fd_group->fds.size() > 1 ? frame.fd_group->fds[1] : -1;
        m.y_size  = frame.sizes[0] ? frame.sizes[0] : (frame.width * frame.height);
        m.uv_size = frame.sizes[1] ? frame.sizes[1] : (frame.width * frame.height / 2);

        if (fd1 < 0 || fd1 == fd0) {
            // Single FD: map Y+UV contiguously
            void* p = ::mmap(nullptr, m.y_size + m.uv_size, PROT_READ, MAP_SHARED, fd0, 0);
            if (p == MAP_FAILED) { m.y_plane = m.uv_plane = nullptr; return m; }
            m.y_plane  = static_cast<const uint8_t*>(p);
            m.uv_plane = m.y_plane + m.y_size;
        } else {
            // Separate FDs for Y and UV
            void* py = ::mmap(nullptr, m.y_size, PROT_READ, MAP_SHARED, fd0, 0);
            if (py == MAP_FAILED) { m.y_plane = m.uv_plane = nullptr; return m; }
            m.y_plane = static_cast<const uint8_t*>(py);

            void* puv = ::mmap(nullptr, m.uv_size, PROT_READ, MAP_SHARED, fd1, 0);
            if (puv == MAP_FAILED) {
                ::munmap(const_cast<uint8_t*>(m.y_plane), m.y_size);
                m.y_plane = m.uv_plane = nullptr;
                return m;
            }
            m.uv_plane = static_cast<const uint8_t*>(puv);
        }
        return m;
    }
};

// ─── Input Preparation ────────────────────────────────────────────────────────
//
// Builds a CPU-side HalTensor from a camera frame for a specific model type.
// Returns a malloc'd buffer that must be freed by the caller.

struct PreparedInput {
    void* buffer    = nullptr;
    HalTensor tensor{};

    PreparedInput() = default;
    PreparedInput(const PreparedInput&) = delete;
    PreparedInput& operator=(const PreparedInput&) = delete;
    PreparedInput(PreparedInput&& other) noexcept
        : buffer(other.buffer), tensor(other.tensor) {
        other.buffer = nullptr;
        other.tensor.data = nullptr;
    }
    PreparedInput& operator=(PreparedInput&& other) noexcept {
        if (this != &other) {
            free_buffer();
            buffer = other.buffer;
            tensor = other.tensor;
            other.buffer = nullptr;
            other.tensor.data = nullptr;
        }
        return *this;
    }
    ~PreparedInput() { free_buffer(); }

    explicit operator bool() const { return buffer != nullptr; }

    void* release_buffer() noexcept {
        void* released = buffer;
        buffer = nullptr;
        tensor.data = nullptr;
        return released;
    }

    void free_buffer() noexcept {
        if (buffer) {
            std::free(buffer);
            buffer = nullptr;
            tensor.data = nullptr;
        }
    }
};

struct FreeBuffer {
    void operator()(void* ptr) const noexcept { std::free(ptr); }
};

struct AutoInferRequestResources {
    FrameDelivery delivery;
    std::shared_ptr<FdGroup> fd_group;
    std::unique_ptr<void, FreeBuffer> input;

    void release_source() noexcept {
        delivery.acknowledge();
        input.reset();
        fd_group.reset();
    }
};

struct AutoInferInFlightTicket {
    AutoInferInFlightTicket(
        std::shared_ptr<std::atomic<int>> counter_in,
        std::shared_ptr<AutoInferOutstandingWorkState> outstanding_in)
        : counter(std::move(counter_in)),
          outstanding(std::move(outstanding_in)) {
        std::lock_guard lock(outstanding->mu);
        ++outstanding->count;
        counter->fetch_add(1);
    }

    ~AutoInferInFlightTicket() {
        counter->fetch_sub(1);
        std::lock_guard lock(outstanding->mu);
        if (--outstanding->count == 0)
            outstanding->cv.notify_all();
    }

    std::shared_ptr<std::atomic<int>> counter;
    std::shared_ptr<AutoInferOutstandingWorkState> outstanding;
};

struct AutoInferImmediateOutputs {
    ModelManager* model_mgr = nullptr;
    HalTensor* outputs = nullptr;
    int num_outputs = 0;
    const std::string* model_id = nullptr;
    bool model_acquired = false;
    bool armed = true;

    void disarm() noexcept { armed = false; }
    ~AutoInferImmediateOutputs() noexcept {
        if (!armed) return;
        try {
            if (outputs)
                model_mgr->free_outputs(outputs, num_outputs);
        } catch (...) {
            LOG_ERROR("AutoInfer: immediate output cleanup threw");
        }
        try {
            if (model_acquired)
                model_mgr->release_model(*model_id);
        } catch (...) {
            LOG_ERROR("AutoInfer: immediate model release threw");
        }
    }
};

struct AutoInferOutputResources {
    AutoInferOutputResources(
        ModelManager* model_mgr_in,
        std::unique_ptr<HalTensor[]> outputs_in,
        int num_outputs_in,
        std::string model_id_in,
        bool model_acquired_in,
        std::shared_ptr<AutoInferInFlightTicket> ticket_in)
        : model_mgr(model_mgr_in),
          outputs(std::move(outputs_in)),
          num_outputs(num_outputs_in),
          model_id(std::move(model_id_in)),
          model_acquired(model_acquired_in),
          ticket(std::move(ticket_in)) {}

    ModelManager* model_mgr = nullptr;
    std::unique_ptr<HalTensor[]> outputs;
    int num_outputs = 0;
    std::string model_id;
    bool model_acquired = false;
    std::shared_ptr<AutoInferInFlightTicket> ticket;

    ~AutoInferOutputResources() noexcept {
        try {
            if (outputs)
                model_mgr->free_outputs(outputs.get(), num_outputs);
        } catch (...) {
            LOG_ERROR("AutoInfer: output cleanup threw");
        }
        try {
            if (model_acquired)
                model_mgr->release_model(model_id);
        } catch (...) {
            LOG_ERROR("AutoInfer: model release threw");
        }
    }
};

/// Prepare CLIP input: NV12 → RGB resize to target_w × target_h
static PreparedInput prepare_clip_input(
    const MappedNV12Frame& mapped, const ReceivedFrame& frame,
    int target_w, int target_h) {

    PreparedInput pi;
    size_t target_size = target_w * target_h * 3;
    pi.buffer = std::malloc(target_size);
    if (!pi.buffer) return {};

    nv12_to_rgb_resize(
        mapped.y_plane, mapped.uv_plane,
        frame.width, frame.height,
        frame.strides[0] ? frame.strides[0] : frame.width,
        frame.strides[1] ? frame.strides[1] : frame.width,
        static_cast<uint8_t*>(pi.buffer), target_w, target_h
    );

    pi.tensor = {};
    pi.tensor.data      = pi.buffer;
    pi.tensor.dma_fd    = -1;
    pi.tensor.byte_size = target_size;
    pi.tensor.ndim      = 3;
    pi.tensor.shape[0]  = target_h;
    pi.tensor.shape[1]  = target_w;
    pi.tensor.shape[2]  = 3;
    pi.tensor.dtype     = HAL_DTYPE_UINT8;
    return pi;
}

/// Prepare NV12 input: resize (or straight-copy) to target_w × target_h
static PreparedInput prepare_nv12_input(
    const MappedNV12Frame& mapped, const ReceivedFrame& frame,
    int target_w, int target_h) {

    PreparedInput pi;
    size_t expected_size = target_w * target_h * 3 / 2;
    pi.buffer = std::malloc(expected_size);
    if (!pi.buffer) return {};

    bool needs_resize = (static_cast<int>(frame.width) != target_w ||
                         static_cast<int>(frame.height) != target_h);
    if (needs_resize) {
        nv12_to_nv12_resize(
            mapped.y_plane, mapped.uv_plane,
            frame.width, frame.height,
            frame.strides[0] ? frame.strides[0] : frame.width,
            frame.strides[1] ? frame.strides[1] : frame.width,
            static_cast<uint8_t*>(pi.buffer),
            static_cast<uint8_t*>(pi.buffer) + target_w * target_h,
            target_w, target_h
        );
    } else {
        std::memcpy(pi.buffer, mapped.y_plane, mapped.y_size);
        std::memcpy(static_cast<uint8_t*>(pi.buffer) + mapped.y_size,
                    mapped.uv_plane, mapped.uv_size);
    }

    pi.tensor = {};
    pi.tensor.data      = pi.buffer;
    pi.tensor.dma_fd    = -1;
    pi.tensor.byte_size = expected_size;
    pi.tensor.ndim      = 3;
    pi.tensor.shape[0]  = expected_size;
    pi.tensor.shape[1]  = target_h;
    pi.tensor.shape[2]  = target_w;
    pi.tensor.dtype     = HAL_DTYPE_UINT8;
    return pi;
}

// ─── AutoInfer Implementation ─────────────────────────────────────────────────

struct AutoInfer::PipelineFrameState {
    std::mutex mu;
    std::condition_variable cv;
    ReceivedFrame latest_frame{};
    bool has_frame = false;
    bool accepting_frames = true;
};

struct AutoInfer::PipelineRunState {
    std::mutex mu;
    std::condition_variable cv;
    size_t expected = 0;
    size_t startup_reported = 0;
    size_t exited = 0;
    bool startup_failed = false;

    void report_startup(bool success) noexcept {
        std::lock_guard lock(mu);
        ++startup_reported;
        if (!success) startup_failed = true;
        cv.notify_all();
    }

    void report_exit() noexcept {
        std::lock_guard lock(mu);
        ++exited;
        cv.notify_all();
    }
};

namespace {

std::string auto_infer_subscriber_id(const AutoInferPipeline& pipe) {
    return pipe.model_id + ":" + pipe.stream_id;
}

struct SubscriptionGuard {
    FdReceiver* receiver = nullptr;
    std::string stream_id;
    std::string subscriber_id;
    std::function<void()> quiesce_callback_state;

    void reset() noexcept {
        auto* current = receiver;
        receiver = nullptr;
        if (!current) return;
        try {
            if (quiesce_callback_state) quiesce_callback_state();
            current->unsubscribe(stream_id, subscriber_id);
        } catch (...) {
            LOG_ERROR("AutoInfer: subscription cleanup threw");
        }
    }

    void arm(FdReceiver* new_receiver) noexcept { receiver = new_receiver; }

    ~SubscriptionGuard() noexcept { reset(); }
};

struct SessionGuard {
    SessionManager* manager = nullptr;
    std::string session_id;

    explicit SessionGuard(SessionManager* manager_in) noexcept
        : manager(manager_in) {}

    void arm(std::string new_session_id) noexcept {
        session_id.swap(new_session_id);
    }

    ~SessionGuard() noexcept {
        if (!manager || session_id.empty()) return;
        try {
            manager->destroy_session(session_id);
        } catch (...) {
            LOG_ERROR("AutoInfer: session cleanup threw");
        }
    }
};

}  // namespace

AutoInfer::AutoInfer(ModelManager* model_mgr,
                     FdReceiver* fd_receiver,
                     EventBusClient* event_bus,
                     InferenceScheduler* scheduler,
                     SessionManager* session_mgr,
                     PostprocessPool* postprocess_pool,
                     const Config& cfg)
    : model_mgr_(model_mgr)
    , fd_receiver_(fd_receiver)
    , event_bus_(event_bus)
    , scheduler_(scheduler)
    , session_mgr_(session_mgr)
    , postprocess_pool_(postprocess_pool)
    , cfg_(cfg)
    , outstanding_work_(
          std::make_shared<AutoInferOutstandingWorkState>())
{}

AutoInfer::~AutoInfer() {
    stop();
}

bool AutoInfer::start() {
    if (cfg_.auto_infer_pipelines.empty()) {
        LOG_WARN("AutoInfer: no pipelines configured");
        return false;
    }

    std::unique_lock lifecycle_lock(lifecycle_mu_);
    if (lifecycle_state_ == LifecycleState::Running) {
        if (!pipeline_failed_.load(std::memory_order_acquire)) return true;
        LOG_WARN("AutoInfer: restarting after a pipeline exited unexpectedly");
        stop_locked(lifecycle_lock);
    }

    lifecycle_state_ = LifecycleState::Starting;
    running_.store(true, std::memory_order_release);
    pipeline_failed_.store(false, std::memory_order_release);
    std::shared_ptr<PipelineRunState> run_state;
    try {
        run_state = std::make_shared<PipelineRunState>();
        run_state->expected = cfg_.auto_infer_pipelines.size();
        pipeline_run_state_ = run_state;
    } catch (...) {
        running_.store(false, std::memory_order_release);
        lifecycle_state_ = LifecycleState::Stopped;
        LOG_ERROR("AutoInfer: failed to allocate pipeline startup state");
        return false;
    }

    {
        std::lock_guard lock(pipeline_states_mu_);
        pipeline_states_.clear();
    }

    try {
        threads_.reserve(cfg_.auto_infer_pipelines.size());
        for (const auto& pipe : cfg_.auto_infer_pipelines) {
            LOG_INFO("AutoInfer: starting pipeline model=%s stream=%s fps=%u",
                     pipe.model_id.c_str(), pipe.stream_id.c_str(), pipe.fps);
            threads_.emplace_back(
                &AutoInfer::pipeline_thread_main, this, pipe, run_state);
        }
    } catch (const std::exception& e) {
        {
            std::lock_guard lock(run_state->mu);
            run_state->expected = threads_.size();
        }
        LOG_ERROR("AutoInfer: pipeline thread creation failed: %s", e.what());
        stop_locked(lifecycle_lock);
        return false;
    } catch (...) {
        {
            std::lock_guard lock(run_state->mu);
            run_state->expected = threads_.size();
        }
        LOG_ERROR("AutoInfer: pipeline thread creation failed");
        stop_locked(lifecycle_lock);
        return false;
    }

    constexpr auto kStartupTimeout = std::chrono::seconds(10);
    bool startup_ready = false;
    {
        std::unique_lock state_lock(run_state->mu);
        startup_ready = run_state->cv.wait_for(
            state_lock, kStartupTimeout, [&] {
                return run_state->startup_failed ||
                       run_state->startup_reported == run_state->expected;
            });
        startup_ready = startup_ready && !run_state->startup_failed &&
                        run_state->startup_reported == run_state->expected;
    }
    if (!startup_ready) {
        LOG_ERROR("AutoInfer: pipeline startup failed or timed out");
        stop_locked(lifecycle_lock);
        return false;
    }

    lifecycle_state_ = LifecycleState::Running;
    return true;
}

void AutoInfer::stop() {
    // Signal workers before waiting for the lifecycle mutex so a stop racing a
    // startup handshake can make that handshake fail promptly instead of waiting
    // for its full startup timeout.
    running_.store(false, std::memory_order_release);
    std::unique_lock lifecycle_lock(lifecycle_mu_);
    if (lifecycle_state_ == LifecycleState::Stopped) return;
    stop_locked(lifecycle_lock);
}

void AutoInfer::stop_locked(
    std::unique_lock<std::mutex>& lifecycle_lock) {
    if (!lifecycle_lock.owns_lock()) std::abort();

    constexpr auto kShutdownTimeout = std::chrono::seconds(5);
    const auto shutdown_deadline = SteadyClock::now() + kShutdownTimeout;
    lifecycle_state_ = LifecycleState::Stopping;
    running_.store(false, std::memory_order_release);

    std::vector<std::shared_ptr<PipelineFrameState>> pipeline_states;
    {
        std::lock_guard lock(pipeline_states_mu_);
        pipeline_states.reserve(pipeline_states_.size());
        for (const auto& weak_state : pipeline_states_) {
            if (auto state = weak_state.lock())
                pipeline_states.push_back(std::move(state));
        }
    }
    for (const auto& state : pipeline_states) {
        std::lock_guard lock(state->mu);
        state->accepting_frames = false;
        if (state->has_frame)
            state->latest_frame.delivery.acknowledge();
        state->latest_frame = {};
        state->has_frame = false;
        state->cv.notify_all();
    }

    for (const auto& pipe : cfg_.auto_infer_pipelines) {
        try {
            if (!fd_receiver_->unsubscribe_until(
                    pipe.stream_id, auto_infer_subscriber_id(pipe),
                    shutdown_deadline)) {
                terminate_shutdown_failure(
                    "unsubscribe did not quiesce before the shutdown deadline");
            }
        } catch (...) {
            terminate_shutdown_failure(
                "unsubscribe threw; preserving callback lifetime");
        }
        if (SteadyClock::now() >= shutdown_deadline) {
            terminate_shutdown_failure(
                "unsubscribe exceeded shutdown deadline");
        }
    }

    const auto run_state = pipeline_run_state_;
    if (run_state) {
        std::unique_lock state_lock(run_state->mu);
        if (!run_state->cv.wait_until(
                state_lock, shutdown_deadline, [&] {
                    return run_state->exited == run_state->expected;
                })) {
            terminate_shutdown_failure(
                "pipeline threads did not stop before the shutdown deadline");
        }
    }

    for (auto& thread : threads_) {
        if (thread.joinable()) thread.join();
    }
    threads_.clear();

    for (const auto& state : pipeline_states) {
        std::lock_guard lock(state->mu);
        if (state->has_frame) {
            state->latest_frame.delivery.acknowledge();
            state->latest_frame = {};
            state->has_frame = false;
        }
    }
    {
        std::lock_guard lock(pipeline_states_mu_);
        pipeline_states_.clear();
    }

    {
        std::unique_lock work_lock(outstanding_work_->mu);
        if (!outstanding_work_->cv.wait_until(
                work_lock, shutdown_deadline,
                [this] { return outstanding_work_->count == 0; })) {
            terminate_shutdown_failure(
                "outstanding inference work did not quiesce before the "
                "shutdown deadline");
        }
    }

    pipeline_run_state_.reset();
    lifecycle_state_ = LifecycleState::Stopped;
    LOG_INFO("AutoInfer: all pipelines stopped");
}

void AutoInfer::pipeline_thread_main(
    AutoInferPipeline pipe,
    std::shared_ptr<PipelineRunState> run_state) noexcept {
    bool startup_succeeded = false;
    try {
        pipeline_loop(pipe, run_state, startup_succeeded);
    } catch (const std::exception& e) {
        LOG_ERROR("AutoInfer: pipeline '%s/%s' terminated with exception: %s",
                  pipe.model_id.c_str(), pipe.stream_id.c_str(), e.what());
    } catch (...) {
        LOG_ERROR("AutoInfer: pipeline '%s/%s' terminated with exception",
                  pipe.model_id.c_str(), pipe.stream_id.c_str());
    }

    if (!startup_succeeded) {
        run_state->report_startup(false);
    } else if (running_.load(std::memory_order_acquire)) {
        pipeline_failed_.store(true, std::memory_order_release);
        LOG_ERROR("AutoInfer: pipeline '%s/%s' exited unexpectedly",
                  pipe.model_id.c_str(), pipe.stream_id.c_str());
    }
    run_state->report_exit();
}

void AutoInfer::pipeline_loop(
    const AutoInferPipeline& pipe,
    const std::shared_ptr<PipelineRunState>& run_state,
    bool& startup_succeeded) {
    // Prepare the guard before acquisition so copying the model id cannot leak
    // a live ref if allocation throws.
    ModelGuard model_guard{model_mgr_, pipe.model_id};
    auto snap = model_mgr_->acquire_model_snapshot(pipe.model_id);
    if (!snap) {
        LOG_ERROR("AutoInfer: model '%s' not found, pipeline aborted", pipe.model_id.c_str());
        return;
    }
    model_guard.arm();

    HalInferenceSession* infer_session = snap->infer_session;
    HalPostprocessSession* pp_session = snap->post_session;
    int max_outputs = snap->num_outputs;

    bool enable_post = model_mgr_->has_post_ops();

    // Determine model input requirements once
    const bool is_clip = (snap->post_type == HAL_POST_TYPE_CLIP ||
                          snap->post_type == HAL_POST_TYPE_EMBEDDING);
    int model_target_w = 0, model_target_h = 0;

    if (is_clip) {
        model_target_h = model_target_w = 224;
        if (snap->model_info.inputs[0].byte_size > 0 &&
            snap->model_info.inputs[0].byte_size != 150528) {
            model_target_w = model_target_h =
                static_cast<int>(std::sqrt(snap->model_info.inputs[0].byte_size / 3.0));
        }
    } else if (snap->model_info.num_inputs == 1) {
        model_target_h = snap->model_info.inputs[0].shape[1];
        model_target_w = snap->model_info.inputs[0].shape[2];
        if (model_target_h <= 0 || model_target_w <= 0) {
            model_target_w = 640; model_target_h = 384;
        }
    }

    // Unique subscriber name for this pipeline
    const std::string subscriber_name = auto_infer_subscriber_id(pipe);

    // Keep callback state alive independently from the pipeline stack. The exact
    // subscription is fenced below before this shared state is released.
    auto frame_state = std::make_shared<PipelineFrameState>();
    {
        std::lock_guard lock(pipeline_states_mu_);
        pipeline_states_.push_back(frame_state);
    }
    SessionGuard session_guard{session_mgr_};

    auto do_subscribe = [&]() -> bool {
        return fd_receiver_->subscribe(
            pipe.stream_id,
            subscriber_name,
            [frame_state](const ReceivedFrame& frame) {
                std::lock_guard lock(frame_state->mu);
                if (!frame_state->accepting_frames) {
                    frame.delivery.acknowledge();
                    return;
                }
                // Release the previously buffered frame if it was never consumed.
                // This prevents camera-daemon buffer pool exhaustion when frames
                // arrive faster than inference can process them.
                if (frame_state->has_frame &&
                    frame_state->latest_frame.frame_id != 0) {
                    frame_state->latest_frame.delivery.acknowledge();
                }
                frame_state->latest_frame = frame;
                frame_state->has_frame = true;
                frame_state->cv.notify_one();
            });
    };

    bool subscribed = false;
    int retry = 0;
    while (running_.load() && !(subscribed = do_subscribe())) {
        if (++retry > 30) {
            LOG_ERROR("AutoInfer: failed to subscribe stream '%s' after %d retries (sub=%s)",
                      pipe.stream_id.c_str(), retry, subscriber_name.c_str());
            return;
        }
        LOG_WARN("AutoInfer: FdReceiver subscribe '%s' failed, retry %d/30... (sub=%s)",
                 pipe.stream_id.c_str(), retry, subscriber_name.c_str());
        std::unique_lock lock(frame_state->mu);
        frame_state->cv.wait_for(
            lock, Milliseconds(2000), [&] { return !running_.load(); });
    }

    if (!subscribed) return;
    auto quiesce_callback_state = [frame_state] {
        std::lock_guard lock(frame_state->mu);
        frame_state->accepting_frames = false;
        if (frame_state->has_frame)
            frame_state->latest_frame.delivery.acknowledge();
        frame_state->latest_frame = {};
        frame_state->has_frame = false;
        frame_state->cv.notify_all();
    };
    SubscriptionGuard subscription_guard{
        fd_receiver_, pipe.stream_id, subscriber_name,
        quiesce_callback_state};
    if (!running_.load()) return;

    LOG_INFO("AutoInfer: pipeline '%s/%s' running", pipe.model_id.c_str(), pipe.stream_id.c_str());

    // Register session for this auto-pipeline to enable stats and scheduling.
    // Hold a shared_ptr: get_session() now returns shared_ptr<Session> (a raw
    // pointer could dangle if the session is destroyed concurrently, and the
    // prior `get_session(id)->running` deref'd it with NO null check).
    // app_id is plain "system": SessionManager identifiers are restricted to
    // [A-Za-z0-9._-], and this app_id only feeds stats output.
    session_guard.arm(session_mgr_->create_session(
        "system", pipe.stream_id, pipe.model_id, pipe.fps, 0, 5));
    const std::string session_id = session_guard.session_id;
    auto session = session_mgr_->get_session(session_id);
    if (!session) {
        LOG_ERROR("AutoInfer: failed to create session for pipeline '%s/%s'",
                  pipe.model_id.c_str(), pipe.stream_id.c_str());
        return;
    }
    session->running = true;
    startup_succeeded = true;
    run_state->report_startup(true);

    auto fps = pipe.fps > 0 ? pipe.fps : 10;
    auto frame_interval = Milliseconds(1000 / fps);
    // Sequence-zero is a valid first frame; track "seen" separately from the
    // value so it is never dropped as a phantom duplicate.
    uint64_t last_seq = 0;
    bool has_last_seq = false;
    uint64_t publisher_generation = 0;

    // Limit outstanding frames to prevent camera-daemon buffer pool exhaustion.
    // With N subscribers sharing the same pool, each pipeline should hold at most
    // a few buffers.  The pool has ~15 buffers, so 3 per pipeline leaves headroom.
    constexpr int MAX_IN_FLIGHT = 3;
    auto in_flight = std::make_shared<std::atomic<int>>(0);

    while (running_.load()) {
        ReceivedFrame frame{};
        {
            std::unique_lock lock(frame_state->mu);
            if (!frame_state->cv.wait_for(
                    lock, frame_interval,
                    [&] { return frame_state->has_frame || !running_.load(); })) {
                lock.unlock();
                if (!fd_receiver_->stream_connected(pipe.stream_id)) {
                    LOG_WARN("AutoInfer: publisher connection lost for stream '%s'; reconnecting",
                             pipe.stream_id.c_str());
                    subscription_guard.reset();

                    {
                        std::lock_guard state_lock(frame_state->mu);
                        frame_state->accepting_frames = true;
                    }

                    subscribed = false;
                    uint64_t reconnect_retry = 0;
                    while (running_.load(std::memory_order_acquire) &&
                           !(subscribed = do_subscribe())) {
                        ++reconnect_retry;
                        LOG_WARN("AutoInfer: reconnect subscribe '%s' failed, retry %lu (sub=%s)",
                                 pipe.stream_id.c_str(), reconnect_retry,
                                 subscriber_name.c_str());
                        std::unique_lock state_lock(frame_state->mu);
                        frame_state->cv.wait_for(
                            state_lock, Milliseconds(2000), [&] {
                                return !running_.load(std::memory_order_acquire);
                            });
                    }
                    if (!subscribed) break;

                    subscription_guard.arm(fd_receiver_);
                    ++publisher_generation;
                    last_seq = 0;
                    has_last_seq = false;
                    LOG_INFO("AutoInfer: publisher connection restored for stream '%s'",
                             pipe.stream_id.c_str());
                }
                continue;
            }
            if (!running_.load()) break;
            frame = std::move(frame_state->latest_frame);
            frame_state->latest_frame = {};
            frame_state->has_frame = false;
        }

        if (has_last_seq && frame.sequence == last_seq) {
            frame.delivery.acknowledge();
            continue;
        }
        last_seq = frame.sequence;
        has_last_seq = true;

        // Back-pressure: if too many frames are in the scheduler queue awaiting
        // completion, drop this frame to avoid exhausting camera-daemon's buffer pool.
        if (in_flight->load() >= MAX_IN_FLIGHT) {
            frame.delivery.acknowledge();
            continue;
        }

        // ── Input Preparation ─────────────────────────────────────────────

        // Map NV12 DMA-BUF planes into user-space (RAII: auto-unmaps on scope exit)
        MappedNV12Frame mapped = MappedNV12Frame::from_frame(frame);
        if (!mapped) {
            LOG_ERROR("AutoInfer: DMA-BUF mmap failed for model=%s stream=%s",
                      pipe.model_id.c_str(), pipe.stream_id.c_str());
            frame.delivery.acknowledge();
            continue;
        }

        PreparedInput input;
        if (is_clip) {
            input = prepare_clip_input(mapped, frame, model_target_w, model_target_h);
        } else if (snap->model_info.num_inputs == 1) {
            input = prepare_nv12_input(mapped, frame, model_target_w, model_target_h);
        }

        // mapped is no longer needed after prepare — destructor will unmap

        if (!input) {
            LOG_ERROR("AutoInfer: failed to prepare input for model=%s", pipe.model_id.c_str());
            frame.delivery.acknowledge();
            continue;
        }

        int num_inputs = 1;

        LOG_DEBUG("AutoInfer: frame seq=%lu %ux%u planes=%u num_inputs=%d",
                  frame.sequence, frame.width, frame.height,
                  frame.num_planes, num_inputs);

        // ── Submit to Scheduler ───────────────────────────────────────────

        auto request_resources = std::make_shared<AutoInferRequestResources>();
        request_resources->delivery = frame.delivery;
        request_resources->fd_group = frame.fd_group;
        request_resources->input.reset(input.release_buffer());
        auto ticket = std::make_shared<AutoInferInFlightTicket>(
            in_flight, outstanding_work_);

        auto inf_req = std::make_unique<InferRequest>();
        inf_req->model_id   = pipe.model_id;
        inf_req->session_id = session_id;
        inf_req->num_inputs = num_inputs;
        inf_req->inputs[0]  = input.tensor;
        inf_req->inputs[0].data = request_resources->input.get();
        inf_req->timeout_ms = 1000;
        inf_req->resource_holder = request_resources;
        inf_req->owns_outputs = true;

        auto stream_id = pipe.stream_id;
        auto model_id = pipe.model_id;
        auto frame_seq = frame.sequence;
        auto ts_ns = frame.timestamp_ns;
        const auto frame_generation = publisher_generation;

        inf_req->on_complete =
            [this, stream_id, model_id, frame_seq, ts_ns, frame_generation,
             enable_post, pp_session, request_resources, ticket, session_id]
            (int rc, HalTensor* outputs, int num_outputs,
             uint64_t infer_time_us, uint64_t queue_time_us,
             bool model_acquired) noexcept {
            (void)queue_time_us;
            AutoInferImmediateOutputs immediate{
                model_mgr_, outputs, num_outputs, &model_id, model_acquired};

            try {
                request_resources->release_source();

                auto sess = session_mgr_->get_session(session_id);
                if (sess)
                    session_mgr_->record_inference(sess.get(), infer_time_us);

                if (rc != 0) {
                    LOG_WARN("AutoInfer: scheduler infer failed rc=%d "
                             "(model=%s stream=%s)",
                             rc, model_id.c_str(), stream_id.c_str());
                    return;
                }

                auto outputs_copy =
                    std::make_unique<HalTensor[]>(num_outputs);
                std::memcpy(outputs_copy.get(), outputs,
                            sizeof(HalTensor) * num_outputs);

                auto deferred = std::make_shared<AutoInferOutputResources>(
                    model_mgr_, std::move(outputs_copy), num_outputs,
                    model_id, model_acquired, ticket);
                immediate.disarm();

                auto* mgr = model_mgr_;
                auto* pool = postprocess_pool_;
                auto* eb = event_bus_;
                const bool eb_publish = cfg_.event_bus_auto_publish;
                auto pp = pp_session;
                const bool do_post = enable_post;

                PostprocessPool::Task post_task =
                    [mgr, eb, eb_publish, stream_id, model_id, frame_seq,
                     ts_ns, frame_generation, pp, do_post, deferred]() noexcept {
                    try {
                        if (do_post && pp) {
                            HalPostprocessResult post_result{};
                            struct PostResultGuard {
                                ModelManager* mgr;
                                HalPostprocessResult* result;
                                ~PostResultGuard() noexcept {
                                    try {
                                        mgr->free_post_result(result);
                                    } catch (...) {
                                        LOG_ERROR("AutoInfer: post-result cleanup threw");
                                    }
                                }
                            } post_guard{mgr, &post_result};

                            int post_rc = mgr->post_process(
                                pp, deferred->outputs.get(),
                                deferred->num_outputs, &post_result);
                            if (post_rc == 0 && eb && eb->connected() &&
                                eb_publish) {
                                std::string topic = "inference/" + stream_id;
                                std::string payload = post_result_to_json(
                                    stream_id, model_id, frame_seq, ts_ns,
                                    post_result);
                                std::string event_id = make_event_id(
                                    model_id, stream_id, frame_generation,
                                    frame_seq, ts_ns);
                                eb->publish(topic, "auto-infer", ts_ns,
                                            event_id, payload);
                            }
                        }
                    } catch (const std::exception& e) {
                        LOG_ERROR("AutoInfer: postprocess failed for %s: %s",
                                  model_id.c_str(), e.what());
                    } catch (...) {
                        LOG_ERROR("AutoInfer: postprocess failed for %s",
                                  model_id.c_str());
                    }
                };

                if (!pool->submit(post_task)) post_task();
            } catch (const std::exception& e) {
                LOG_ERROR("AutoInfer: completion failed for %s: %s",
                          model_id.c_str(), e.what());
            } catch (...) {
                LOG_ERROR("AutoInfer: completion failed for %s",
                          model_id.c_str());
            }
        };

        if (!scheduler_->submit(std::move(inf_req))) {
            request_resources->release_source();
            ticket.reset();
            LOG_WARN("AutoInfer: failed to submit to scheduler (model=%s stream=%s)",
                     pipe.model_id.c_str(), pipe.stream_id.c_str());
        }
    }

    LOG_INFO("AutoInfer: pipeline '%s/%s' stopped", pipe.model_id.c_str(), pipe.stream_id.c_str());
}

}  // namespace aipc::ai_runtime
