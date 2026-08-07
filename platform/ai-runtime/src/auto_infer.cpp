#include "auto_infer.h"
#include "log.h"

#include <chrono>
#include <cstring>
#include <cmath>
#include <sys/mman.h>
#include <condition_variable>
#include <sstream>
#include <unistd.h>

namespace aipc::ai_runtime {

using SteadyClock  = std::chrono::steady_clock;

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
    void* buffer    = nullptr;   // malloc'd buffer (caller must free)
    HalTensor tensor{};          // ready-to-submit tensor (data points into buffer)

    explicit operator bool() const { return buffer != nullptr; }

    void free_buffer() {
        if (buffer) { std::free(buffer); buffer = nullptr; tensor.data = nullptr; }
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
{}

AutoInfer::~AutoInfer() {
    stop();
}

bool AutoInfer::start() {
    if (cfg_.auto_infer_pipelines.empty()) {
        LOG_WARN("AutoInfer: no pipelines configured");
        return false;
    }

    running_.store(true);

    for (auto& pipe : cfg_.auto_infer_pipelines) {
        LOG_INFO("AutoInfer: starting pipeline model=%s stream=%s fps=%u",
                 pipe.model_id.c_str(), pipe.stream_id.c_str(), pipe.fps);
        threads_.emplace_back(&AutoInfer::pipeline_loop, this, pipe);
    }

    return true;
}

void AutoInfer::stop() {
    if (!running_.exchange(false)) return;

    for (auto& pipe : cfg_.auto_infer_pipelines) {
        fd_receiver_->unsubscribe(pipe.stream_id);
    }

    for (auto& t : threads_) {
        if (t.joinable()) t.join();
    }
    threads_.clear();

    LOG_INFO("AutoInfer: all pipelines stopped");
}

void AutoInfer::pipeline_loop(const AutoInferPipeline& pipe) {
    // Acquire model snapshot — safe to use after lock release
    auto snap = model_mgr_->acquire_model_snapshot(pipe.model_id);
    if (!snap) {
        LOG_ERROR("AutoInfer: model '%s' not found, pipeline aborted", pipe.model_id.c_str());
        return;
    }

    ModelGuard model_guard{model_mgr_, pipe.model_id};

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
    std::string subscriber_name = pipe.model_id + ":" + pipe.stream_id;

    // Subscribe to camera-daemon's FD publisher with retry
    std::mutex frame_mu;
    std::condition_variable frame_cv;
    ReceivedFrame latest_frame{};
    bool has_frame = false;

    auto do_subscribe = [&]() -> bool {
        return fd_receiver_->subscribe(
            pipe.stream_id,
            subscriber_name,
            [&](const ReceivedFrame& frame) {
                std::lock_guard lock(frame_mu);
                // Release the previously buffered frame if it was never consumed.
                // This prevents camera-daemon buffer pool exhaustion when frames
                // arrive faster than inference can process them.
                if (has_frame && latest_frame.frame_id != 0) {
                    fd_receiver_->release_frame(pipe.stream_id, latest_frame.frame_id);
                }
                latest_frame = frame;
                has_frame = true;
                frame_cv.notify_one();
            });
    };

    int retry = 0;
    while (running_.load() && !do_subscribe()) {
        if (++retry > 30) {
            LOG_ERROR("AutoInfer: failed to subscribe stream '%s' after %d retries (sub=%s)",
                      pipe.stream_id.c_str(), retry, subscriber_name.c_str());
            return;
        }
        LOG_WARN("AutoInfer: FdReceiver subscribe '%s' failed, retry %d/30... (sub=%s)",
                 pipe.stream_id.c_str(), retry, subscriber_name.c_str());
        std::this_thread::sleep_for(Milliseconds(2000));
    }

    if (!running_.load()) return;

    LOG_INFO("AutoInfer: pipeline '%s/%s' running", pipe.model_id.c_str(), pipe.stream_id.c_str());

    // Register session for this auto-pipeline to enable stats and scheduling.
    // Hold a shared_ptr: get_session() now returns shared_ptr<Session> (a raw
    // pointer could dangle if the session is destroyed concurrently, and the
    // prior `get_session(id)->running` deref'd it with NO null check).
    std::string session_id = session_mgr_->create_session("<system>", pipe.stream_id, pipe.model_id, pipe.fps, 0, 5);
    auto session = session_mgr_->get_session(session_id);
    if (!session) {
        LOG_ERROR("AutoInfer: failed to create session for pipeline '%s/%s'",
                  pipe.model_id.c_str(), pipe.stream_id.c_str());
        return;
    }
    session->running = true;

    auto fps = pipe.fps > 0 ? pipe.fps : 10;
    auto frame_interval = Milliseconds(1000 / fps);
    uint64_t last_seq = 0;

    // Limit outstanding frames to prevent camera-daemon buffer pool exhaustion.
    // With N subscribers sharing the same pool, each pipeline should hold at most
    // a few buffers.  The pool has ~15 buffers, so 3 per pipeline leaves headroom.
    constexpr int MAX_IN_FLIGHT = 3;
    auto in_flight = std::make_shared<std::atomic<int>>(0);

    while (running_.load()) {
        ReceivedFrame frame{};
        {
            std::unique_lock lock(frame_mu);
            if (!frame_cv.wait_for(lock, frame_interval,
                                   [&] { return has_frame || !running_.load(); })) {
                continue;
            }
            if (!running_.load()) break;
            frame = latest_frame;
            has_frame = false;
        }

        if (frame.sequence == last_seq) continue;
        last_seq = frame.sequence;

        // Back-pressure: if too many frames are in the scheduler queue awaiting
        // completion, drop this frame to avoid exhausting camera-daemon's buffer pool.
        if (in_flight->load() >= MAX_IN_FLIGHT) {
            fd_receiver_->release_frame(pipe.stream_id, frame.frame_id);
            continue;
        }

        // ── Input Preparation ─────────────────────────────────────────────

        // Map NV12 DMA-BUF planes into user-space (RAII: auto-unmaps on scope exit)
        MappedNV12Frame mapped = MappedNV12Frame::from_frame(frame);
        if (!mapped) {
            LOG_ERROR("AutoInfer: DMA-BUF mmap failed for model=%s stream=%s",
                      pipe.model_id.c_str(), pipe.stream_id.c_str());
            fd_receiver_->release_frame(pipe.stream_id, frame.frame_id);
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
            fd_receiver_->release_frame(pipe.stream_id, frame.frame_id);
            continue;
        }

        int num_inputs = 1;

        LOG_DEBUG("AutoInfer: frame seq=%lu %ux%u planes=%u num_inputs=%d",
                  frame.sequence, frame.width, frame.height,
                  frame.num_planes, num_inputs);

        // ── Submit to Scheduler ───────────────────────────────────────────

        auto inf_req = std::make_unique<InferRequest>();
        inf_req->model_id   = pipe.model_id;
        inf_req->session_id = session_id;
        inf_req->num_inputs = num_inputs;
        inf_req->inputs[0]  = input.tensor;
        inf_req->timeout_ms = 1000;
        inf_req->resource_holder = frame.fd_group;  // Keep FDs alive
        inf_req->owns_outputs = true;  // Async post-process takes ownership

        // Captured variables for callback
        auto stream_id  = pipe.stream_id;
        auto model_id   = pipe.model_id;
        auto frame_seq  = frame.sequence;
        auto ts_ns      = frame.timestamp_ns;
        auto fid        = frame.frame_id;
        void* buf_to_free = input.buffer;
        input.buffer = nullptr;  // Transfer ownership to lambda

        in_flight->fetch_add(1);

        inf_req->on_complete = [this, stream_id, model_id, frame_seq, ts_ns, fid, enable_post,
                                pp_session, frame, buf_to_free, in_flight, session_id]
            (int rc, HalTensor* outputs, int num_outputs,
             uint64_t infer_time_us, uint64_t queue_time_us,
             bool model_acquired) {

            // Release frame in camera-daemon
            fd_receiver_->release_frame(stream_id, fid);

            // Free the prepared input buffer
            std::free(buf_to_free);

            // Record stats on this pipeline's session (single source;
            // on_hw_complete does not record to avoid double-counting)
            auto sess = session_mgr_->get_session(session_id);
            if (sess) {
                session_mgr_->record_inference(sess.get(), infer_time_us);
            }

            if (rc != 0) {
                LOG_WARN("AutoInfer: scheduler infer failed rc=%d (model=%s stream=%s)",
                         rc, model_id.c_str(), stream_id.c_str());
                if (outputs) model_mgr_->free_outputs(outputs, num_outputs);
                if (model_acquired) model_mgr_->release_model(model_id);
                in_flight->fetch_sub(1);
                return;
            }

            // Offload post-processing to the pool so the scheduler worker
            // can immediately process the next frame's inference.
            //
            // The callback's HalTensor* storage is not owned by this lambda
            // (async HALs may pass memory from their own callback context).
            // Copy the structs to the heap so they survive beyond on_complete.
            // The data pointers are HAL-owned and freed exactly once by
            // free_outputs() inside the lambda.
            auto* outputs_copy = new HalTensor[num_outputs];
            std::memcpy(outputs_copy, outputs, sizeof(HalTensor) * num_outputs);

            auto* mgr = model_mgr_;
            auto* pool = postprocess_pool_;
            auto* eb = event_bus_;
            bool eb_publish = cfg_.event_bus_auto_publish;
            auto pp = pp_session;
            auto do_post = enable_post;

            PostprocessPool::Task post_task = [mgr, eb, eb_publish, stream_id, model_id,
                              frame_seq, ts_ns, outputs_copy, num_outputs,
                              pp, do_post, in_flight]() {
                if (do_post && pp) {
                    HalPostprocessResult post_result{};
                    int post_rc = mgr->post_process(pp, outputs_copy,
                                                    num_outputs, &post_result);
                    if (post_rc == 0) {
                        // Publish to event-bus
                        if (eb && eb->connected() && eb_publish) {
                            std::string topic = "inference/" + stream_id;
                            std::string payload = post_result_to_json(
                                stream_id, model_id, frame_seq, ts_ns, post_result);
                            std::string event_id = stream_id + "-" +
                                std::to_string(frame_seq);
                            eb->publish(topic, "auto-infer", ts_ns,
                                        event_id, payload);
                        }
                    }
                    mgr->free_post_result(&post_result);
                }

                // Free HAL-allocated output buffers
                mgr->free_outputs(outputs_copy, num_outputs);
                delete[] outputs_copy;

                // Release model ref held by the scheduler worker
                mgr->release_model(model_id);

                in_flight->fetch_sub(1);
            };

            if (!pool->submit(post_task)) {
                // Queue full — run synchronously (still off the NPU thread)
                // post_task is NOT moved-from because submit takes Task&
                post_task();
            }
        };

        if (!scheduler_->submit(std::move(inf_req))) {
            LOG_WARN("AutoInfer: failed to submit to scheduler (model=%s stream=%s)",
                     pipe.model_id.c_str(), pipe.stream_id.c_str());
            fd_receiver_->release_frame(pipe.stream_id, frame.frame_id);
            std::free(buf_to_free);
            in_flight->fetch_sub(1);
        }
    }

    session_mgr_->destroy_session(session_id);
    fd_receiver_->unsubscribe(pipe.stream_id);
    LOG_INFO("AutoInfer: pipeline '%s/%s' stopped", pipe.model_id.c_str(), pipe.stream_id.c_str());
}

void AutoInfer::publish_result(const std::string& stream_id,
                               const std::string& model_id,
                               uint64_t frame_seq,
                               uint64_t timestamp_ns,
                               const HalPostprocessResult& result) {
    if (!event_bus_ || !event_bus_->connected()) return;

    std::string topic = cfg_.event_bus_result_topic_prefix + model_id + "/" + stream_id;
    std::string payload = post_result_to_json(stream_id, model_id, frame_seq, timestamp_ns, result);
    std::string event_id = "auto-" + std::to_string(frame_seq) + "-" + std::to_string(timestamp_ns);

    event_bus_->publish(topic, "ai-runtime", timestamp_ns, event_id, payload,
                        {{"stream_id", stream_id}, {"model_id", model_id}});
}

}  // namespace aipc::ai_runtime
