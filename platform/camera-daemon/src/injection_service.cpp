/**
 * @file injection_service.cpp
 * @brief Implementation of the app frame injection service (PushFrame
 *        P0-P2).
 *
 * Locking model: one mutex (mu_) guards session state, queue and
 * counters — push_frame (gRPC thread), take_frame (bake-site encoder
 * thread) and status (any thread) all take it. No HAL call is made while
 * holding it: pin_buffer() and the resolvers run before the lock and
 * every queued pin is released by RAII (QueuedFrame dtor) on the thread
 * that drops it.
 *
 * Bake-site wiring (P0, resolved 2026-09-10): the encoder consumes the
 * media-library buffer, NOT the HalFrameBuffer view the bake callback
 * sees, so REPLACE is a CONTENT copy — handle_video_frame_for_routing
 * calls take_frame() after the DPM offer and composes the pinned fb's
 * pixels into the pipeline buffer; DPM mask and AI overlay then draw on
 * top (platform masking wins; the worker only ever offered real ISP
 * pixels, so risk 4 of the proposal is satisfied by construction).
 * EOS/StopInjection restores the ISP path at the next take_frame miss.
 *
 * P2-12 additions: OVERLAY accepts an NV12 dma-buf (opaque inset paste
 * at dest_x/dest_y) or an ARGB32 buffer (CPU alpha blend — build_blend
 * needs a registry-resident base and the pipeline frame is not one, and
 * the DSP queue is a single congested worker, so the bake site blends on
 * CPU instead); stream targeting (stream_id) replaces the P0 head-only
 * FIFO with a full queue scan; pts pacing picks the newest due frame and
 * drops superseded older ones; the manifest permission gate resolves the
 * buffer owner's UDS identity (SO_PEERCRED) and checks allowed_apps.
 *
 * TODO(P1+, tracked in docs/proposals/frame-injection.md):
 *  1. Zero-copy buffer swap — hand the pinned media-library buffer to
 *     add_buffer directly once a per-buffer encoder handoff exists.
 */

#include "injection_service.h"

#include <algorithm>
#include <utility>

#include "common/hal_log.h"

namespace {

/* Eligibility for one queued item against the frontier asking:
 * stream-targeted items follow their stream; legacy (empty stream_id)
 * items are REPLACE-only and must match the encode dims exactly (P0
 * semantics). */
bool item_eligible(const InjectionService::QueuedFrame& q,
                   const std::string& stream_name,
                   uint32_t width, uint32_t height) {
    if (!q.stream_id.empty())
        return q.stream_id == stream_name;
    return q.mode == InjectionMode::Replace &&
           q.width == width && q.height == height;
}

bool item_due(const InjectionService::QueuedFrame& q, uint64_t frame_ts_ns) {
    return q.pts_ns == 0 || q.pts_ns <= frame_ts_ns;
}

} // namespace

InjectionService::InjectionService(DspService* dsp,
                                   const InjectionServiceConfig& cfg)
    : dsp_(dsp), cfg_(cfg) {
    /* Drop-oldest on an empty deque would be UB; cap must be >= 1. */
    if (cfg_.queue_capacity == 0)
        cfg_.queue_capacity = 1;
}

InjectionService::~InjectionService() {
    stop();
}

void InjectionService::set_identity_resolver(
    std::function<std::string(int)> resolver) {
    identity_resolver_ = std::move(resolver);
}

void InjectionService::set_stream_dims_resolver(
    std::function<bool(const std::string&, uint32_t&, uint32_t&)> resolver) {
    stream_dims_resolver_ = std::move(resolver);
}

bool InjectionService::start() {
    if (!dsp_) {
        HAL_LOG_WARNING("InjectionService: no DSP registry, not starting");
        return false;
    }
    running_ = true;
    return true;
}

void InjectionService::stop() {
    running_ = false;
    stop_injection();
}

void InjectionService::close_session_locked(const char* why) {
    /* QueuedFrame dtor releases every pin back to the DSP registry. */
    const size_t flushed = queue_.size();
    queue_.clear();
    session_active_ = false;
    session_owner_fd_ = -1;
    session_session_id_.clear();
    if (flushed > 0) {
        HAL_LOG_INFO("InjectionService: session closed (%s), %zu frame(s) "
                     "flushed", why, flushed);
    }
}

InjectionPushResult InjectionService::push_frame(
    const InjectionFrameDesc& desc) {
    InjectionPushResult res;

    if (!running_.load() || !dsp_) {
        res.rc = INJ_SVC_ERR_UNAVAILABLE;
        res.message = "injection service not running";
        return res;
    }
    if (!cfg_.enabled) {
        res.rc = INJ_SVC_ERR_DISABLED;
        res.message = "frame injection disabled by configuration";
        return res;
    }

    /* EOS closes the session without enqueueing (buffer_id may be 0). */
    if (desc.end_of_stream) {
        std::lock_guard<std::mutex> lk(mu_);
        if (!session_active_) {
            res.message = "end of stream with no active session";
            return res;
        }
        close_session_locked("end_of_stream");
        res.message = "end of stream: session closed";
        return res;
    }

    /* ---- validation -------------------------------------------------- */
    if (desc.mode == InjectionMode::Overlay && desc.stream_id.empty()) {
        res.rc = INJ_SVC_ERR_INVALID;
        res.message = "OVERLAY requires a stream_id target";
        return res;
    }
    if ((desc.dest_x & 1u) != 0 || (desc.dest_y & 1u) != 0) {
        res.rc = INJ_SVC_ERR_INVALID;
        res.message = "dest_x/dest_y must be even (chroma alignment)";
        return res;
    }
    if (desc.buffer_id == 0) {
        res.rc = INJ_SVC_ERR_INVALID;
        res.message = "buffer_id is required (0 is valid only with end_of_stream)";
        return res;
    }
    if (desc.width == 0 || desc.height == 0 ||
        (desc.width & 1u) != 0 || (desc.height & 1u) != 0) {
        res.rc = INJ_SVC_ERR_INVALID;
        res.message = "width/height must be nonzero and even";
        return res;
    }
    if (desc.width > cfg_.max_width || desc.height > cfg_.max_height) {
        res.rc = INJ_SVC_ERR_INVALID;
        res.message = "frame exceeds injection dimension caps";
        return res;
    }
    if (desc.stride < desc.width) {
        res.rc = INJ_SVC_ERR_INVALID;
        res.message = "stride must be >= width";
        return res;
    }

    /* ---- pin: everything below owns `pin` via RAII -------------------- */
    DspService::BufferPin pin = dsp_->pin_buffer(desc.buffer_id);
    if (!pin.ok()) {
        res.rc = pin.rc(); /* DspServiceError pass-through */
        res.message = "unknown or foreign buffer id";
        return res;
    }

    HalFrameBuffer* fb = pin.fb();
    const bool is_nv12 = fb && fb->format == HAL_PIX_FMT_NV12;
    const bool is_argb = fb && fb->format == HAL_PIX_FMT_ARGB32;
    if (!fb || fb->num_planes == 0u || (!is_nv12 && !is_argb)) {
        res.rc = INJ_SVC_ERR_NO_BUFFER;
        res.message = "buffer is not an NV12 or ARGB32 frame";
        return res;
    }
    if (is_nv12 && fb->mem_type != HAL_MEM_DMABUF) {
        res.rc = INJ_SVC_ERR_NO_BUFFER;
        res.message = "NV12 buffer is not a dma-buf frame";
        return res;
    }
    if (desc.mode == InjectionMode::Replace && !is_nv12) {
        res.rc = INJ_SVC_ERR_INVALID;
        res.message = "REPLACE takes NV12 (convert RGB via the DSP first)";
        return res;
    }
    if (is_argb && desc.stride < desc.width * 4u) {
        res.rc = INJ_SVC_ERR_INVALID;
        res.message = "ARGB32 byte stride must be >= width*4";
        return res;
    }
    if (fb->width != desc.width || fb->height != desc.height) {
        res.rc = INJ_SVC_ERR_INVALID;
        res.message = "width/height do not match the registered buffer";
        return res;
    }

    /* Best-effort geometry against the live stream config (sharpens the
     * error at push time; the bake-site check is the hard guarantee). */
    if (!desc.stream_id.empty() && stream_dims_resolver_) {
        uint32_t sw = 0, sh = 0;
        if (!stream_dims_resolver_(desc.stream_id, sw, sh)) {
            res.rc = INJ_SVC_ERR_INVALID;
            res.message = "unknown stream_id '" + desc.stream_id + "'";
            return res;
        }
        const bool dims_ok =
            desc.mode == InjectionMode::Overlay
                ? (desc.dest_x + desc.width <= sw &&
                   desc.dest_y + desc.height <= sh)
                : (desc.width == sw && desc.height == sh);
        if (!dims_ok) {
            res.rc = INJ_SVC_ERR_INVALID;
            res.message = desc.mode == InjectionMode::Overlay
                ? "overlay rect exceeds the target stream bounds"
                : "REPLACE dims must equal the target stream encode dims";
            return res;
        }
    }

    /* ---- manifest permission gate -------------------------------------
     * The pin's owner fd anchors the app identity: a pinned buffer
     * implies a live UDS connection, whose SO_PEERCRED identity the
     * FdPublisher captured at accept time. An empty allow-list allows
     * all (development default). */
    if (!cfg_.allowed_apps.empty()) {
        const std::string identity =
            identity_resolver_ ? identity_resolver_(pin.owner_fd()) : "";
        if (identity.empty() ||
            std::find(cfg_.allowed_apps.begin(), cfg_.allowed_apps.end(),
                      identity) == cfg_.allowed_apps.end()) {
            res.rc = INJ_SVC_ERR_PERMISSION;
            res.message = "app '" +
                          (identity.empty() ? std::string("<unknown>")
                                            : identity) +
                          "' is not allowed to inject frames";
            HAL_LOG_WARNING("InjectionService: PERMISSION denied for "
                            "owner_fd=%d identity='%s'",
                            pin.owner_fd(), identity.c_str());
            return res;
        }
    }

    std::lock_guard<std::mutex> lk(mu_);

    /* Single-session: first accepted frame opens the session; frames
     * from any other owner fd are rejected until stop_injection(). */
    if (session_active_ && pin.owner_fd() != session_owner_fd_) {
        res.rc = INJ_SVC_ERR_BUSY;
        res.message = "another injection session is active";
        return res;
    }
    if (session_active_ && desc.mode != session_mode_) {
        res.rc = INJ_SVC_ERR_INVALID;
        res.message = "mode change requires stop_injection";
        return res;
    }
    if (!session_active_) {
        session_active_ = true;
        session_mode_ = desc.mode;
        session_owner_fd_ = pin.owner_fd();
        const std::string tag = desc.session_id.empty()
                                    ? std::string()
                                    : (", session='" + desc.session_id + "'");
        HAL_LOG_INFO("InjectionService: session opened (owner_fd=%d, %ux%u, "
                     "mode=%s, stream='%s'%s)",
                     session_owner_fd_, desc.width, desc.height,
                     desc.mode == InjectionMode::Overlay ? "overlay"
                                                         : "replace",
                     desc.stream_id.c_str(), tag.c_str());
    }
    /* Latest non-empty tag wins; an untagged frame never clears it — the
     * session still belongs to the fd that opened it, the tag is
     * evidence for GetInjectionStatus/StopInjection observability. */
    if (!desc.session_id.empty()) {
        session_session_id_ = desc.session_id;
    }

    /* Shallow lossy queue: overflow drops the OLDEST frame and counts
     * it — never blocks, never backpressures the caller (the encoder
     * pulls from the other end via take_frame). */
    if (queue_.size() >= cfg_.queue_capacity) {
        queue_.pop_front(); /* dtor releases the pin */
        ++frames_dropped_;
    }

    const uint64_t frame_id = next_frame_id_++;
    QueuedFrame q;
    q.pin = std::move(pin);
    q.buffer_id = desc.buffer_id;
    q.width = desc.width;
    q.height = desc.height;
    q.stride = desc.stride;
    q.pts_ns = desc.pts_ns;
    q.frame_id = frame_id;
    q.dest_x = desc.dest_x;
    q.dest_y = desc.dest_y;
    q.mode = desc.mode;
    q.stream_id = desc.stream_id;
    queue_.push_back(std::move(q));

    res.frame_id = frame_id;
    return res;
}

void InjectionService::stop_injection() {
    std::lock_guard<std::mutex> lk(mu_);
    if (session_active_) {
        close_session_locked("stop_injection");
    } else {
        queue_.clear(); /* belt and braces: pins from a dying session */
    }
}

void InjectionService::release_owner(int client_fd) {
    std::lock_guard<std::mutex> lk(mu_);
    /* Owner match only: a different client's disconnect must not close
     * the live session (the fd key namespace is shared with unrelated
     * connections). */
    if (session_active_ && session_owner_fd_ == client_fd) {
        close_session_locked("owner disconnect");
    }
}

InjectionServiceStatus InjectionService::status() const {
    std::lock_guard<std::mutex> lk(mu_);
    InjectionServiceStatus s;
    s.active = session_active_;
    s.mode = session_mode_;
    s.session_owner_fd = session_owner_fd_;
    s.session_id = session_session_id_;
    s.frames_injected = frames_injected_;
    s.frames_dropped = frames_dropped_;
    s.queue_depth = static_cast<uint32_t>(queue_.size());
    /* Write-lease snapshot: queued ids plus baking ids, sorted for a
     * deterministic wire order (tests assert on the exact list). */
    s.in_flight_buffer_ids.reserve(queue_.size() + baking_ids_.size());
    for (const QueuedFrame& q : queue_)
        s.in_flight_buffer_ids.push_back(q.buffer_id);
    s.in_flight_buffer_ids.insert(s.in_flight_buffer_ids.end(),
                                  baking_ids_.begin(), baking_ids_.end());
    std::sort(s.in_flight_buffer_ids.begin(),
              s.in_flight_buffer_ids.end());
    return s;
}

bool InjectionService::take_frame(const std::string& stream_name,
                                  uint32_t width, uint32_t height,
                                  uint64_t frame_ts_ns, QueuedFrame& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (queue_.empty())
        return false;

    /* Newest-due-wins pts pacing: scan the (<= capacity) queue and keep
     * the LAST eligible+due item. Older eligible+due items are stale —
     * a newer frame for the same target superseded them — and are
     * dropped+counted below; eligible-but-not-yet-due items stay for a
     * later frame. */
    const QueuedFrame* chosen = nullptr;
    for (const QueuedFrame& q : queue_) {
        if (item_eligible(q, stream_name, width, height) &&
            item_due(q, frame_ts_ns))
            chosen = &q;
    }
    if (!chosen)
        return false;
    const uint64_t chosen_id = chosen->frame_id;
    const uint64_t chosen_buffer = chosen->buffer_id;

    for (auto it = queue_.begin(); it != queue_.end();) {
        if (it->frame_id == chosen_id ||
            !(item_eligible(*it, stream_name, width, height) &&
              item_due(*it, frame_ts_ns))) {
            ++it;
            continue;
        }
        it = queue_.erase(it); /* dtor releases the pin */
        ++frames_dropped_;
    }

    for (auto it = queue_.begin(); it != queue_.end(); ++it) {
        if (it->frame_id == chosen_id) {
            /* Lease transfer queue -> bake: the id leaves the queued set
             * and enters baking_ids_ so status() keeps reporting it until
             * the bake site's note_bake_done() lands. Between the move
             * and that ack the daemon may still read these pixels — a
             * slot rewrite in that window is the tear P1-1 described. */
            baking_ids_.insert(chosen_buffer);
            out = std::move(*it);
            queue_.erase(it);
            ++frames_injected_;
            return true;
        }
    }
    return false; /* unreachable: chosen_id was just seen in the queue */
}

void InjectionService::note_bake_done(uint64_t buffer_id) {
    std::lock_guard<std::mutex> lk(mu_);
    baking_ids_.erase(buffer_id);
}
