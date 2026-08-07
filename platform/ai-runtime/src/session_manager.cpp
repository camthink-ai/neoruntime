#include "session_manager.h"
#include <sstream>
#include <chrono>

namespace aipc::ai_runtime {

std::string SessionManager::create_session(const std::string& app_id,
                                           const std::string& stream_id,
                                           const std::string& model_id,
                                           uint32_t fps_limit,
                                           uint32_t max_qps,
                                           uint32_t priority) {
    auto now = SteadyClock::now();
    auto ts  = std::chrono::duration_cast<std::chrono::seconds>(
                   now.time_since_epoch()).count();

    std::ostringstream oss;
    oss << app_id << "-" << stream_id << "-" << model_id << "-" << ts;
    std::string session_id = oss.str();

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

    std::unique_lock lock(mu_);
    sessions_.emplace(session_id, std::move(s));
    return session_id;
}

std::string SessionManager::create_named_session(const std::string& session_id,
                                                 const std::string& app_id,
                                                 const std::string& stream_id,
                                                 const std::string& model_id,
                                                 uint32_t fps_limit,
                                                 uint32_t max_qps,
                                                 uint32_t priority) {
    auto now = SteadyClock::now();

    std::unique_lock lock(mu_);
    // Idempotent: an existing session for this id is reused so repeated Infer()
    // calls for the same implicit session don't create + scan each time.
    if (sessions_.find(session_id) != sessions_.end())
        return session_id;

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
    auto elapsed = SteadyClock::now() - s->last_infer;
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

void SessionManager::record_inference(Session* s, uint64_t latency_us) {
    if (!s) return;
    s->last_infer = SteadyClock::now();
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
