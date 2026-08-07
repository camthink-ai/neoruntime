#include "inference_scheduler.h"
#include "log.h"
#include <cstring>

namespace aipc::ai_runtime {

InferenceScheduler::InferenceScheduler(ModelManager* model_mgr,
                                       SessionManager* session_mgr,
                                       int num_workers,
                                       int queue_capacity)
    : model_mgr_(model_mgr)
    , session_mgr_(session_mgr)
    , num_workers_(num_workers)
    , queue_capacity_(queue_capacity) {}

InferenceScheduler::~InferenceScheduler() {
    stop();
}

void InferenceScheduler::start() {
    if (running_.exchange(true)) return;

    LOG_INFO("Starting inference scheduler: %d workers, queue=%d, async=%s",
             num_workers_, queue_capacity_,
             model_mgr_->has_async() ? "true" : "false");

    for (int i = 0; i < num_workers_; i++) {
        workers_.emplace_back(&InferenceScheduler::worker_loop, this, i);
    }
}

void InferenceScheduler::stop() {
    if (!running_.exchange(false)) return;

    LOG_INFO("Stopping inference scheduler");
    cv_.notify_all();

    for (auto& w : workers_) {
        if (w.joinable()) w.join();
    }
    workers_.clear();

    // Drop any requests still in the queue (workers may have exited before
    // draining them all). These requests were never acquired by a worker,
    // so model ref was NOT bumped — do NOT call on_complete (which would
    // trigger release_model on a non-existent ref). Callers handle this via
    // their own timeout (future.wait_for). resource_holder is freed by pop().
    std::lock_guard lock(mu_);
    int dropped = 0;
    for (auto& [sid, sq] : session_queues_) {
        while (!sq.empty()) {
            sq.pop();  // unique_ptr<InferRequest> destructed, resource_holder released
            dropped++;
        }
    }
    session_queues_.clear();
    deficits_.clear();
    total_queued_ = 0;
    if (dropped > 0) {
        LOG_WARN("Scheduler stop: dropped %d queued request(s)", dropped);
    }
}

int InferenceScheduler::drain_async(int timeout_ms) {
    if (async_in_flight_.load() == 0) return 0;

    LOG_INFO("Draining %d in-flight async callbacks (timeout=%dms)",
             async_in_flight_.load(), timeout_ms);

    std::unique_lock<std::mutex> lk(async_drain_mu_);
    async_drain_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
        [this] { return async_in_flight_.load() <= 0; });

    int orphaned = async_in_flight_.load();
    if (orphaned > 0) {
        LOG_WARN("drain_async: %d orphan job(s) did not complete — "
                 "late callbacks may access freed resources", orphaned);
    } else {
        LOG_INFO("drain_async: all async callbacks completed");
    }
    return orphaned;
}

void InferenceScheduler::notify_async_complete() {
    async_in_flight_.fetch_sub(1);
    std::lock_guard<std::mutex> lk(async_drain_mu_);
    async_drain_cv_.notify_all();
}

bool InferenceScheduler::submit(std::unique_ptr<InferRequest> req) {
    if (!running_) return false;

    std::lock_guard lock(mu_);
    if (total_queued_ >= queue_capacity_) {
        LOG_WARN("Inference queue full (%d), dropping request", queue_capacity_);
        return false;
    }

    req->enqueue_time = SteadyClock::now();

    // Route to session-specific queue
    auto& sq = session_queues_[req->session_id];
    sq.push(std::move(req));
    total_queued_++;

    cv_.notify_one();
    return true;
}

int InferenceScheduler::queue_depth() const {
    std::lock_guard lock(mu_);
    return total_queued_;
}

void InferenceScheduler::handle_completion(
    int rc, HalTensor* outputs, int num_outputs,
    uint64_t infer_us, uint64_t queue_us,
    const std::string& model_id,
    const std::string& session_id,
    bool owns_outputs,
    const std::shared_ptr<Session>& session)
{
    // Record stats
    if (session) {
        session_mgr_->record_inference(session.get(), infer_us);
    }

    // The on_complete callback is stored in the InferRequest, which is
    // owned by the caller (WorkerCallbackState in async mode, or the
    // worker_loop stack in sync mode). This method receives timing data
    // but does NOT call on_complete — that is the caller's job because
    // it owns the InferRequest.
    // (kept as a placeholder for future shared completion logic)
    (void)rc; (void)outputs; (void)num_outputs; (void)queue_us;
    (void)model_id; (void)session_id; (void)owns_outputs;
}

void InferenceScheduler::on_hw_complete(HalTensor* outputs, int num_outputs,
                                          int status, void* userdata) {
    auto* st = static_cast<WorkerCallbackState*>(userdata);
    if (!st) return;

    auto infer_time = std::chrono::duration_cast<Microseconds>(
        SteadyClock::now() - st->infer_start);

    // Stats are recorded by the on_complete callback (or the caller), not
    // here — recording in both places would double-count QPS/latency.

    // Deliver result to the on_complete callback
    if (st->req->on_complete) {
        st->req->on_complete(status, outputs, num_outputs,
                             infer_time.count(), st->queue_time_us,
                             true);  // model was acquired by worker
    }

    // If on_complete did not take ownership of outputs, clean up here
    if (!st->req->owns_outputs) {
        st->mgr->free_outputs(outputs, num_outputs);
        st->mgr->release_model(st->req->model_id);
    }

    // Free the heap-allocated output slot array that worker_loop passed to
    // run_async. Do not delete the callback's `outputs` pointer: Hailo15 passes
    // a shallow-copy vector owned by the HAL async context.
    delete[] st->outputs;

    // Decrement async in-flight counter and notify drain.
    InferenceScheduler* sched = st->scheduler;
    delete st;  // releases the unique_ptr<InferRequest>

    if (sched) {
        sched->notify_async_complete();
    }
}

void InferenceScheduler::worker_loop(int worker_id) {
    LOG_DEBUG("Worker %d started", worker_id);

    // Drain mode: keep processing until both running_ is false AND the
    // queue is empty. This ensures stop() doesn't leave queued requests
    // unhandled (they would otherwise be failed by stop()'s cleanup loop,
    // but draining via workers is preferred since it runs real inference).
    while (true) {
        std::unique_ptr<InferRequest> req;
        {
            std::unique_lock lock(mu_);
            cv_.wait(lock, [this] { return total_queued_ > 0 || !running_; });
            if (!running_ && total_queued_ == 0) break;
            if (total_queued_ == 0) continue;

            // Weighted-deficit round-robin: each session accumulates a deficit
            // counter weighted by its priority (higher priority = faster
            // accumulation). The session with the highest deficit is selected,
            // and its deficit is reduced by the total weight of all active
            // sessions. This guarantees fairness: no session starves, but
            // higher-priority sessions get proportionally more slots.
            //
            // For edge deployments (typically < 20 sessions) this O(N) scan
            // is negligible compared to inference latency.
            std::string best_sid;
            int64_t best_score = INT64_MIN;
            bool found = false;

            // First pass: bump deficits and find the best candidate
            int total_weight = 0;
            for (auto& [sid, sq] : session_queues_) {
                if (!sq.empty()) {
                    uint32_t prio = sq.front()->priority;
                    // weight = 1 + prio (prio 0-7 → weight 1-8)
                    int weight = 1 + static_cast<int>(prio);
                    total_weight += weight;
                    // Accumulate deficit
                    deficits_[sid] += weight;
                    if (deficits_[sid] > best_score) {
                        best_score = deficits_[sid];
                        best_sid = sid;
                        found = true;
                    }
                }
            }

            if (found) {
                // Subtract total_weight from the winner's deficit so others
                // get a chance next round.
                deficits_[best_sid] -= total_weight;

                auto& sq = session_queues_[best_sid];
                req = std::move(sq.front());
                sq.pop();
                total_queued_--;

                // Remove empty session queues to prevent unbounded growth
                if (sq.empty()) {
                    session_queues_.erase(best_sid);
                    deficits_.erase(best_sid);
                }
            }

            if (!req) continue;
        }

        auto queue_time = std::chrono::duration_cast<Microseconds>(
            SteadyClock::now() - req->enqueue_time);

        // Acquire model snapshot
        auto snap = model_mgr_->acquire_model_snapshot(req->model_id);
        if (!snap) {
            if (req->on_complete) {
                req->on_complete(-1, nullptr, 0, 0,
                                 static_cast<uint64_t>(queue_time.count()),
                                 false);  // model not acquired
            }
            continue;
        }

        int max_outputs = snap->num_outputs;
        auto infer_start = SteadyClock::now();
        auto session = session_mgr_->get_session(req->session_id);

        // ── Async path (preferred): submit run_async, return immediately ──
        if (model_mgr_->has_async()) {
            // Heap-allocate outputs: the worker loop continues after
            // submission, so stack-allocated outputs would be overwritten
            // before the HAL callback fires.
            auto* outputs = new HalTensor[HAL_MAX_TENSORS]();

            auto* cb_state = new WorkerCallbackState{
                this, model_mgr_, session_mgr_, std::move(req),
                outputs, max_outputs, infer_start,
                static_cast<uint64_t>(queue_time.count()), session
            };

            async_in_flight_.fetch_add(1);
            int rc = model_mgr_->run_async(snap->infer_session,
                                           cb_state->req->inputs,
                                           cb_state->req->num_inputs,
                                           outputs, max_outputs,
                                           &InferenceScheduler::on_hw_complete,
                                           cb_state);
            if (rc == 0) {
                // Submitted — worker is free to process the next request.
                // on_hw_complete will fire from a HAL completion thread.
                continue;
            }

            // Submission failed: fall through to sync path.
            // Reconstruct req from cb_state for the sync handler.
            async_in_flight_.fetch_sub(1);  // undo the increment
            req = std::move(cb_state->req);
            delete[] outputs;
            delete cb_state;
            // Fall through to sync infer below
        }

        // ── Sync path (fallback or HAL without async) ──
        {
            HalTensor outputs[HAL_MAX_TENSORS] = {};
            int rc = model_mgr_->infer(snap->infer_session,
                                       req->inputs, req->num_inputs,
                                       outputs, max_outputs);

            auto infer_time = std::chrono::duration_cast<Microseconds>(
                SteadyClock::now() - infer_start);

            // Record stats
            if (session) {
                session_mgr_->record_inference(session.get(),
                    static_cast<uint64_t>(infer_time.count()));
            }

            // Deliver result
            if (req->on_complete) {
                req->on_complete(rc, outputs, max_outputs,
                                 infer_time.count(), queue_time.count(),
                                 true);  // model was acquired
            }

            // Cleanup
            if (!req->owns_outputs) {
                model_mgr_->free_outputs(outputs, max_outputs);
                model_mgr_->release_model(req->model_id);
            }
        }

        LOG_DEBUG("Worker %d: model=%s queue=%luus",
                  worker_id, req->model_id.c_str(),
                  (unsigned long)queue_time.count());
    }

    LOG_DEBUG("Worker %d stopped", worker_id);
}

}  // namespace aipc::ai_runtime
