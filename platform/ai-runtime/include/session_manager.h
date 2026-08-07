#pragma once

#include "common.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <atomic>

namespace aipc::ai_runtime {

struct Session {
    std::string id;
    std::string app_id;
    std::string stream_id;
    std::string model_id;
    uint32_t    fps_limit  = 0;
    uint32_t    max_qps    = 0;
    uint32_t    priority   = 5;
    TimePoint   created;
    TimePoint   last_infer;
    std::atomic<uint64_t> infer_count{0};
    std::atomic<uint64_t> total_latency_us{0};   // cumulative inference latency
    // QPS window: track inference count in a sliding window
    std::atomic<uint64_t> window_infer_count{0};
    std::atomic<uint64_t> window_start_ms{0};    // epoch ms when window started
    bool        running    = false;
    std::vector<std::string> allowed_models;
};

class SessionManager {
public:
    /// Create a new session. Returns session_id.
    std::string create_session(const std::string& app_id,
                               const std::string& stream_id,
                               const std::string& model_id,
                               uint32_t fps_limit,
                               uint32_t max_qps,
                               uint32_t priority);

    /// Create (or reuse) a session with a caller-chosen id. Idempotent: if a
    /// session with @p session_id already exists it is returned unchanged, so
    /// implicit sessions (e.g. "implicit-{model_id}") survive repeated Infer()
    /// calls without per-request create + scan. Returns @p session_id.
    std::string create_named_session(const std::string& session_id,
                                     const std::string& app_id,
                                     const std::string& stream_id,
                                     const std::string& model_id,
                                     uint32_t fps_limit,
                                     uint32_t max_qps,
                                     uint32_t priority);

    /// Destroy a session. Returns false if not found.
    bool destroy_session(const std::string& session_id);

    /// Destroy every session bound to @p model_id (e.g. implicit sessions when
    /// a model is unloaded). Returns the number of sessions removed.
    size_t destroy_sessions_by_model(const std::string& model_id);

    /// Destroy every session bound to @p app_id. Returns the number removed.
    size_t destroy_sessions_by_app(const std::string& app_id);

    /// Get a session. Returns a shared_ptr that keeps the Session alive for as
    /// long as the caller holds it — a concurrent destroy_session() only removes
    /// the map entry; the object is destroyed when the last holder releases.
    /// This is REQUIRED: call sites hold the session across long windows
    /// (model inference, async HAL callbacks) during which another thread can
    /// unregister the model and erase the implicit session. A raw pointer would
    /// dangle there -> use-after-free -> heap corruption (the grpcpp_sync_ser
    /// SIGABRT "corrupted double-linked list" seen on device). Empty ptr if not
    /// found. Thread-safe.
    std::shared_ptr<Session> get_session(const std::string& session_id);

    /// Check FPS limit: returns true if inference is allowed now.
    /// The caller MUST hold a shared_ptr<Session> for @p s (from get_session) so
    /// @p s cannot be destroyed concurrently.
    bool check_fps_limit(Session* s);

    /// Check QPS limit.
    /// Same liveness contract as check_fps_limit().
    bool check_qps_limit(Session* s);

    /// Record an inference event (with latency tracking).
    /// Same liveness contract as check_fps_limit().
    void record_inference(Session* s, uint64_t latency_us = 0);

    /// List all sessions (snapshot). The returned shared_ptrs keep every
    /// session alive across iteration.
    std::vector<std::shared_ptr<Session>> list_sessions();

private:
    mutable std::shared_mutex mu_;
    // shared_ptr (not unique_ptr): get_session hands out copies that extend the
    // session's life beyond its removal from the map. destroy_session*/erase
    // only drop the map's reference; the object lives until the last caller
    // releases. See get_session doc above.
    std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;
};

}  // namespace aipc::ai_runtime
