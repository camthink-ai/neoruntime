#include "session_manager.h"
#include "log.h"

#include <algorithm>
#include <sstream>
#include <chrono>

namespace aipc::ai_runtime {

namespace {

// Non-empty identifiers must stay within this ASCII subset: no control bytes,
// spaces, path separators or printf escapes can then reach map keys, logs or
// stats. Empty stays legal (existing clients omit stream_id/model_id/app_id).
constexpr char kInvalidIdentifier[] = "";

bool valid_identifier(const std::string& id) {
    if (id.size() > SessionManager::kMaxIdentifierBytes) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

}  // namespace

bool SessionManager::within_limits_locked(const std::string& app_id) const {
    if (sessions_.size() >= kMaxSessionsTotal) {
        LOG_WARN("SessionManager: global session cap reached (%zu); "
                 "rejecting create", kMaxSessionsTotal);
        return false;
    }
    size_t app_sessions = 0;
    for (const auto& kv : sessions_) {
        if (kv.second && kv.second->app_id == app_id &&
            ++app_sessions >= kMaxSessionsPerApp) {
            LOG_WARN("SessionManager: per-app session cap reached (%zu); "
                     "rejecting create", kMaxSessionsPerApp);
            return false;
        }
    }
    return true;
}

std::string SessionManager::create_session(const std::string& app_id,
                                           const std::string& stream_id,
                                           const std::string& model_id,
                                           uint32_t fps_limit,
                                           uint32_t max_qps,
                                           uint32_t priority) {
    if (!valid_identifier(app_id) || !valid_identifier(stream_id) ||
        !valid_identifier(model_id)) {
        LOG_WARN("SessionManager: create_session rejected invalid identifier");
        return kInvalidIdentifier;
    }
    auto now = SteadyClock::now();
    const uint64_t sequence =
        next_session_id_.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream oss;
    oss << app_id << "-" << stream_id << "-" << model_id << "-" << sequence;
    std::string session_id = oss.str();
    // Components are each bounded, so this only trips on pathological input;
    // it keeps a hard ceiling on key/log size regardless.
    if (session_id.size() > 4 * kMaxIdentifierBytes + 24) {
        LOG_WARN("SessionManager: generated session id too long; rejecting");
        return kInvalidIdentifier;
    }

    std::unique_lock lock(mu_);
    if (!within_limits_locked(app_id)) return kInvalidIdentifier;
    // A named session could have squatted a generated-looking id; if so,
    // regenerate instead of handing out a session another caller owns.
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (attempt > 0) {
            const uint64_t retry_sequence =
                next_session_id_.fetch_add(1, std::memory_order_relaxed);
            std::ostringstream retry;
            retry << app_id << "-" << stream_id << "-" << model_id << "-"
                  << retry_sequence;
            session_id = retry.str();
            if (session_id.size() > 4 * kMaxIdentifierBytes + 24)
                return kInvalidIdentifier;
        }
        auto s = std::make_unique<Session>();
        s->id         = session_id;
        s->app_id     = app_id;
        s->stream_id  = stream_id;
        s->model_id   = model_id;
        s->fps_limit  = fps_limit;
        s->max_qps    = max_qps;
        s->priority   = priority;
        s->created    = now;
        s->last_infer = now - std::chrono::seconds(1);  // allow first infer immediately
        s->running    = false;
        if (sessions_.emplace(session_id, std::move(s)).second)
            return session_id;
    }
    return kInvalidIdentifier;
}

std::string SessionManager::create_named_session(const std::string& session_id,
                                                 const std::string& app_id,
                                                 const std::string& stream_id,
                                                 const std::string& model_id,
                                                 uint32_t fps_limit,
                                                 uint32_t max_qps,
                                                 uint32_t priority) {
    // The named id is the map key: it must be non-empty and valid. The other
    // components may stay empty for wire compatibility.
    if (session_id.empty() || !valid_identifier(session_id) ||
        !valid_identifier(app_id) || !valid_identifier(stream_id) ||
        !valid_identifier(model_id)) {
        LOG_WARN("SessionManager: create_named_session rejected invalid id");
        return kInvalidIdentifier;
    }
    auto now = SteadyClock::now();

    std::unique_lock lock(mu_);
    // Idempotent ONLY for the exact same identity/config tuple: repeated
    // Infer() calls for one implicit session reuse it, while a colliding id
    // with different config is rejected instead of silently binding this
    // caller to the other session's stats and limits.
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        const Session& s = *it->second;
        if (s.app_id == app_id && s.stream_id == stream_id &&
            s.model_id == model_id && s.fps_limit == fps_limit &&
            s.max_qps == max_qps && s.priority == priority)
            return session_id;
        LOG_WARN("SessionManager: named session '%s' exists with different "
                 "config; rejecting", session_id.c_str());
        return kInvalidIdentifier;
    }
    if (!within_limits_locked(app_id)) return kInvalidIdentifier;

    auto s = std::make_unique<Session>();
    s->id         = session_id;
    s->app_id     = app_id;
    s->stream_id  = stream_id;
    s->model_id   = model_id;
    s->fps_limit  = fps_limit;
    s->max_qps    = max_qps;
    s->priority   = priority;
    s->created    = now;
    s->last_infer = now - std::chrono::seconds(1);  // allow first infer immediately
    s->running    = false;

    sessions_.emplace(session_id, std::move(s));
    return session_id;
}

bool SessionManager::destroy_session(const std::string& session_id) {
    std::unique_lock lock(mu_);
    return sessions_.erase(session_id) > 0;
}

size_t SessionManager::destroy_sessions_by_model(const std::string& model_id) {
    std::unique_lock lock(mu_);
    size_t removed = 0;
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (it->second && it->second->model_id == model_id) {
            it = sessions_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

size_t SessionManager::destroy_sessions_by_app(const std::string& app_id) {
    std::unique_lock lock(mu_);
    size_t removed = 0;
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (it->second && it->second->app_id == app_id) {
            it = sessions_.erase(it);
            ++removed;
        } else {
            ++it;
        }
    }
    return removed;
}

std::shared_ptr<Session> SessionManager::get_session(const std::string& session_id) {
    std::shared_lock lock(mu_);
    auto it = sessions_.find(session_id);
    return (it != sessions_.end()) ? it->second : nullptr;
}

bool SessionManager::check_fps_limit(Session* s) {
    if (!s || s->fps_limit == 0) return true;
    auto elapsed = SteadyClock::now() -
                   s->last_infer.load(std::memory_order_relaxed);
    auto min_interval = Milliseconds(1000 / s->fps_limit);
    return elapsed >= min_interval;
}

bool SessionManager::check_qps_limit(Session* s) {
    if (!s || s->max_qps == 0) return true;
    auto elapsed = std::chrono::duration_cast<Milliseconds>(
        SteadyClock::now() - s->created);
    if (elapsed.count() < 1000) {
        return s->infer_count.load(std::memory_order_relaxed) < s->max_qps;
    }
    double avg_qps = static_cast<double>(s->infer_count.load(std::memory_order_relaxed)) * 1000.0
                     / elapsed.count();
    return avg_qps < static_cast<double>(s->max_qps);
}

void SessionManager::record_skew(Session* s, uint64_t skew_us) {
    if (!s || skew_us == 0) return;
    s->total_skew_us.fetch_add(skew_us, std::memory_order_relaxed);
    s->skew_count.fetch_add(1, std::memory_order_relaxed);
    // Monotonic max via CAS — concurrent recorders race benignly, the
    // largest value always wins eventually.
    uint64_t cur = s->max_skew_us.load(std::memory_order_relaxed);
    while (skew_us > cur &&
           !s->max_skew_us.compare_exchange_weak(
               cur, skew_us, std::memory_order_relaxed)) {
        // cur reloaded by compare_exchange_weak on failure
    }
}

void SessionManager::record_inference(Session* s, uint64_t latency_us) {
    if (!s) return;
    s->last_infer.store(SteadyClock::now(), std::memory_order_relaxed);
    s->infer_count.fetch_add(1, std::memory_order_relaxed);

    // Accumulate latency
    if (latency_us > 0) {
        s->total_latency_us.fetch_add(latency_us, std::memory_order_relaxed);
    }

    // QPS sliding window (5-second window)
    auto now_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count());
    auto window_start = s->window_start_ms.load(std::memory_order_relaxed);

    if (window_start == 0 || now_ms - window_start > 5000) {
        // Reset window
        s->window_start_ms.store(now_ms, std::memory_order_relaxed);
        s->window_infer_count.store(1, std::memory_order_relaxed);
    } else {
        s->window_infer_count.fetch_add(1, std::memory_order_relaxed);
    }
}

std::vector<std::shared_ptr<Session>> SessionManager::list_sessions() {
    std::shared_lock lock(mu_);
    std::vector<std::shared_ptr<Session>> result;
    result.reserve(sessions_.size());
    for (auto& [id, s] : sessions_) {
        result.push_back(s);
    }
    return result;
}

}  // namespace aipc::ai_runtime
