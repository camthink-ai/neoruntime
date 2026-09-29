/**
 * @file injection_service.h
 * @brief App frame injection service (PushFrame P0-P2; see docs proposals
 *        frame-injection.md and composable-pipeline-contracts.md).
 *
 * Owns the injection session state and the shallow frame queue between
 * the PushFrame gRPC handler and the encoder feed. P0 landed REPLACE/NV12
 * at the target stream's encode resolution; P2-12 adds OVERLAY (NV12
 * inset paste or ARGB32 CPU alpha blend at dest_x/dest_y), stream
 * targeting via stream_id, pts pacing (newest-due-wins), and the manifest
 * permission gate. Composition happens at the daemon's bake/add_buffer
 * frontier (the commit 7cfc9526 site). The 2026-09-10 media-graph probe
 * found no NV12-accepting OUTPUT node (/dev/video10 is a Bayer ISP-front
 * virtual sensor), so this path adds zero media-graph nodes.
 *
 * Buffers arrive exclusively through the DspService registry (alloc or
 * SCM_RIGHTS import over camera.sock); push_frame() pins by buffer id —
 * a raw fd number never enters this service (the Tensor.dma_fd
 * anti-pattern is out of scope by construction). The pin's owner_fd is
 * also the identity anchor: a pinned buffer implies a live UDS
 * connection, and the FdPublisher captures that connection's SO_PEERCRED
 * identity at accept time.
 *
 * Scheduling contract (frame-injection.md): the queue is shallow and
 * lossy-tolerant — overflow drops the OLDEST queued frame and counts it
 * in frames_dropped; nothing here may ever backpressure the encoder.
 *
 * Wiring: the bake site (CameraDaemon::handle_video_frame_for_routing)
 * calls take_frame() per encoded frame with the stream name, its dims
 * and the frame's CLOCK_MONOTONIC timestamp, then composes the pinned
 * pixels into the pipeline frame (full-frame copy for REPLACE, inset
 * paste or CPU alpha blend for OVERLAY). The DPM/overlay draws still run
 * after the compose, so platform masking always wins. Disabled by
 * default (InjectionServiceConfig::enabled = false).
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

#include "dsp_service.h"

/** Service-level error codes (HAL ops keep their own negative codes;
 *  pin failures pass the DspServiceError through as-is). */
enum InjectionServiceError {
    INJ_SVC_OK = 0,
    INJ_SVC_ERR_DISABLED = -1,     /* config gate: injection off          */
    INJ_SVC_ERR_INVALID = -2,      /* validation failed (see message)     */
    INJ_SVC_ERR_NO_BUFFER = -3,    /* unknown/foreign id or unusable fb   */
    INJ_SVC_ERR_BUSY = -4,         /* another client's session is active  */
    INJ_SVC_ERR_UNAVAILABLE = -5,  /* not started / no dsp registry       */
    INJ_SVC_ERR_PERMISSION = -6,   /* app not in the allowed_apps list    */
};

struct InjectionServiceConfig {
    /* Master gate. P0 ships OFF: flip per deployment once the bake-site
     * wiring and the DPM-ordering check land (TODOs in injection_service.cpp). */
    bool enabled = false;
    /* Shallow queue cap (2-3 by design; drop-oldest above it). */
    uint32_t queue_capacity = 3;
    /* Dimension caps at validation (nonzero + even enforced separately). */
    uint32_t max_width = 3840;
    uint32_t max_height = 2160;
    /* Manifest permission gate: allowed app identities (SO_PEERCRED
     * cmdline basename of the UDS connection that owns the buffer).
     * EMPTY = allow all (development default); non-empty rejects everyone
     * not listed with INJ_SVC_ERR_PERMISSION. */
    std::vector<std::string> allowed_apps;
};

enum class InjectionMode { Replace = 0, Overlay = 1 };

/** Plain-struct mirror of the proto request — keeps HAL/proto decoupled
 *  (same convention as DspJobDesc). */
struct InjectionFrameDesc {
    uint64_t buffer_id = 0;   /* DSP-registry id; 0 valid only with eos */
    uint32_t width = 0;       /* must match the registered buffer       */
    uint32_t height = 0;
    uint32_t stride = 0;      /* luma stride (NV12, >= width) or byte
                                 stride (ARGB32, >= width*4)            */
    InjectionMode mode = InjectionMode::Replace;
    uint64_t pts_ns = 0;      /* device CLOCK_MONOTONIC presentation
                                 time; 0 = due immediately               */
    uint32_t dest_x = 0;      /* OVERLAY position within the target
                                 frame (0,0 for full-frame overlay)      */
    uint32_t dest_y = 0;
    bool end_of_stream = false;
    std::string stream_id;    /* target stream; REQUIRED for OVERLAY,
                                 optional for REPLACE (empty = legacy
                                 dims-matching against the encoded frame) */
    std::string session_id;   /* app session tag (P2-13): correlation and
                                 observability only — ownership stays
                                 anchored on the owner fd; the session
                                 records the latest non-empty tag */
};

struct InjectionPushResult {
    int rc = INJ_SVC_OK;
    std::string message;
    uint64_t frame_id = 0;    /* assigned at enqueue; correlates with
                                 frames_injected once the feed takes it */
};

struct InjectionServiceStatus {
    bool active = false;
    InjectionMode mode = InjectionMode::Replace;
    int session_owner_fd = -1;    /* owning UDS client of the live session */
    std::string session_id;       /* live session's app tag (latest non-empty
                                     request tag; "" = untagged or none)   */
    uint64_t frames_injected = 0; /* taken by the encoder feed             */
    uint64_t frames_dropped = 0;  /* queue-full drops + rejects            */
    uint32_t queue_depth = 0;
    /* Write-lease snapshot (buffer-release reporting): every registry id
     * whose pixels the daemon may still read — queued frames plus frames
     * handed to a bake thread whose compose has not finished. A pool slot
     * is safe to rewrite only when its id is absent here. Reported on
     * PushFrame responses and GetInjectionStatus so the SDK can size its
     * in-flight set without a second RPC channel. */
    std::vector<uint64_t> in_flight_buffer_ids;
};

class InjectionService {
public:
    InjectionService(DspService* dsp,
                     const InjectionServiceConfig& cfg = InjectionServiceConfig());
    ~InjectionService();

    InjectionService(const InjectionService&) = delete;
    InjectionService& operator=(const InjectionService&) = delete;

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    /**
     * Resolve a UDS client fd to its app identity (SO_PEERCRED pid →
     * /proc/<pid>/cmdline basename, captured at accept time). Injected by
     * the daemon wiring from FdPublisher so this service stays decoupled
     * from the transport. Unset / unknown fd → "" (rejected under a
     * non-empty allow-list — a live pinned buffer implies a live
     * connection, so "" only happens when the anchor is gone).
     */
    void set_identity_resolver(std::function<std::string(int)> resolver);

    /**
     * Best-effort push-time bounds check: resolve a stream name to its
     * encode dims. Returns false when the resolver is unset or the name
     * is unknown (the bake-site hard check still catches bad geometry;
     * this only sharpens the push-time error message).
     */
    void set_stream_dims_resolver(
        std::function<bool(const std::string&, uint32_t&, uint32_t&)> resolver);

    /* ---------------- Push plane (gRPC worker thread) ------------------ */

    /**
     * Validate one frame, pin it in the DSP registry and enqueue it.
     * The first accepted frame of a stopped session opens the session
     * (single-session: a frame from a different owner fd is rejected
     * with INJ_SVC_ERR_BUSY until stop_injection()). The session's mode
     * is locked at open; a push in the other mode is rejected with
     * INJ_SVC_ERR_INVALID ("mode change requires stop_injection").
     * end_of_stream=true closes the session without enqueueing
     * (buffer_id may be 0).
     */
    InjectionPushResult push_frame(const InjectionFrameDesc& desc);

    /** Flush the queue (releasing every pin) and close the session. */
    void stop_injection();

    /**
     * UDS disconnect hook: close the session when (and only when) the
     * disconnecting client is its owner. Same flush as
     * stop_injection(), but a MISMATCHED fd is a no-op — another client
     * going away must not disturb the live session. Without this, a
     * killed app leaves the session wedged forever and the kernel's fd
     * recycling can hand the dead owner id to the next connection
     * (identity theft by fd number). FdPublisher::disconnect_client
     * calls this BEFORE DspService::release_client_buffers (queued pins
     * must unpin before the registry detaches the entries) and BEFORE
     * close(): the fd number is the ownership key and could be reused
     * by a new client after close() — the same reasoning as the
     * outstanding-frame sweep beside it.
     */
    void release_owner(int client_fd);

    /* ---------------- Observability (any thread) ----------------------- */

    InjectionServiceStatus status() const;

    /* ---------------- Encoder-feed handoff ------------------------------ */

    /** One queued frame, handed to the encoder feed. The pin is move-only;
     *  the caller holds it for exactly the duration of the add_buffer use. */
    struct QueuedFrame {
        DspService::BufferPin pin;
        uint64_t buffer_id = 0;    /* registry id (write-lease key; the bake
                                      site reports it via note_bake_done
                                      once the compose has finished)       */
        uint32_t width = 0;        /* injected content dims                */
        uint32_t height = 0;
        uint32_t stride = 0;
        uint64_t pts_ns = 0;
        uint64_t frame_id = 0;
        uint32_t dest_x = 0;       /* OVERLAY position in the target frame */
        uint32_t dest_y = 0;
        InjectionMode mode = InjectionMode::Replace;
        std::string stream_id;     /* target stream ("" = legacy dims
                                      matching, REPLACE only)             */
    };

    /**
     * Hand the encoder feed the frame this encoded frame should carry,
     * if any. Selection among the (<= queue_capacity) queued items:
     *
     *   eligible = item targeted this stream (item.stream_id == stream_name
     *              for stream-targeted pushes) OR (item.stream_id empty AND
     *              item is Replace AND width/height match exactly — the P0
     *              legacy path).
     *   due      = item.pts_ns == 0 or item.pts_ns <= frame_ts_ns.
     *
     * Among eligible+due items the NEWEST (last enqueued) wins; the older
     * due ones are dropped and counted in frames_dropped (pts pacing:
     * a burst collapses to its freshest frame). Items that are eligible
     * but not yet due are LEFT for a later frame. Returns false when
     * nothing eligible is due — the ISP pixels flow on untouched.
     *
     * pts_ns is in the device CLOCK_MONOTONIC domain (same clock as
     * HalFrameBuffer::timestamp_ns, shared with on-device apps). A
     * far-future pts is self-limiting: drop-oldest overflow and the EOS
     * flush both evict it, so a clock-mismatched producer cannot wedge
     * the queue.
     *
     * Caller: the bake site (handle_video_frame_for_routing) on each
     * stream's streaming thread, between the DPM clean-frame offer and
     * the DPM/overlay draws. On success the caller composes the pinned
     * fb's pixels into the pipeline frame (copy for NV12 inputs, CPU
     * alpha blend for ARGB32) and lets the pin release at scope exit;
     * masking then draws on top by construction (frame-injection.md
     * risk 4).
     */
    bool take_frame(const std::string& stream_name, uint32_t width,
                    uint32_t height, uint64_t frame_ts_ns, QueuedFrame& out);

    /**
     * Bake-completion ack (write-lease release): the bake site calls this
     * with the QueuedFrame's buffer_id once its compose has finished and
     * the frame's pixels are no longer read. Until then the id stays in
     * status().in_flight_buffer_ids and the SDK must not rewrite the
     * slot. Idempotent and safe for unknown ids (a dropped or already
     * flushed frame reports nothing new). take_frame() itself inserts
     * the id into the in-flight set, so a bake site that skips this call
     * only makes the SDK conservative (slot never recycled), never
     * corrupt — the wrong-direction failure is a leak, not a tear.
     */
    void note_bake_done(uint64_t buffer_id);

private:
    /* caller holds mu_ */
    void close_session_locked(const char* why);

    DspService* dsp_ = nullptr;
    InjectionServiceConfig cfg_;
    std::atomic<bool> running_{false};

    std::function<std::string(int)> identity_resolver_;   /* set before start */
    std::function<bool(const std::string&, uint32_t&, uint32_t&)>
        stream_dims_resolver_;                            /* set before start */

    mutable std::mutex mu_;
    std::deque<QueuedFrame> queue_;
    /* Write-lease half beyond the queue: ids handed to a bake thread via
     * take_frame() whose note_bake_done() has not landed yet. Deliberately
     * NOT cleared by close_session_locked(): a compose already handed out
     * outlives the session (EOS/owner-disconnect can land mid-bake), and
     * its pin is owned by the bake site's scope — only note_bake_done()
     * clears the id, so a close can never make the SDK recycle a slot
     * the daemon is still reading. Worst case of a missing ack is a
     * leaked lease (visible, blocking), never a torn frame. */
    std::unordered_set<uint64_t> baking_ids_;
    bool session_active_ = false;
    InjectionMode session_mode_ = InjectionMode::Replace;
    int session_owner_fd_ = -1;
    std::string session_session_id_; /* latest non-empty request tag of the
                                        live session; cleared at close    */
    uint64_t next_frame_id_ = 1; /* starts at 1; 0 is never a valid id */
    uint64_t frames_injected_ = 0;
    uint64_t frames_dropped_ = 0;
};
