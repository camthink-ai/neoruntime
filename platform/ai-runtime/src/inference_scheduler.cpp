#include "inference_scheduler.h"
#include "log.h"
#include <algorithm>
#include <cstring>
#include <exception>

namespace aipc::ai_runtime {

void WorkerCallbackState::release_owner() noexcept {
    if (owners.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    auto* completion_scheduler = scheduler;
    const bool tracked = tracker_active;
    delete this;
    if (tracked && completion_scheduler)
        completion_scheduler->complete_external_async();
}

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
    {
        std::lock_guard lock(mu_);
        if (!running_) return;
        running_ = false;
    }

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
#ifdef AIPC_AI_RUNTIME_TESTING
    if (async_drain_wait_hook_for_test_) async_drain_wait_hook_for_test_();
#endif
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

void InferenceScheduler::begin_external_async() {
    async_in_flight_.fetch_add(1);
}

void InferenceScheduler::complete_external_async() {
    notify_async_complete();
}

bool InferenceScheduler::submit(std::unique_ptr<InferRequest> req) {
#ifdef AIPC_AI_RUNTIME_TESTING
    if (submit_prelock_hook_for_test_) submit_prelock_hook_for_test_();
#endif

    std::lock_guard lock(mu_);
    if (!running_) return false;
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

void InferenceScheduler::on_hw_complete(HalTensor* /*outputs*/,
                                        int num_outputs, int status,
                                        void* userdata) {
    auto* state = static_cast<WorkerCallbackState*>(userdata);
    if (!state) return;

    state->callback_status = status;
    state->callback_num_outputs =
        std::max(0, std::min(num_outputs, state->max_outputs));
    const auto action = state->gate.on_callback();
    if (action == AsyncCallbackAction::CompleteHere)
        complete_async(state);
}

void InferenceScheduler::complete_async(
    WorkerCallbackState* state) noexcept {
    struct CallbackOwnerGuard {
        WorkerCallbackState* state;
        ~CallbackOwnerGuard() noexcept { state->release_owner(); }
    } callback_owner{state};

    auto infer_time = std::chrono::duration_cast<Microseconds>(
        SteadyClock::now() - state->infer_start);
    HalTensor* outputs = state->output_slots.get();
    const int num_outputs = state->callback_num_outputs;

    // Stats are recorded by the on_complete callback (or the caller), not
    // here — recording in both places would double-count QPS/latency.
    bool callback_failed = false;
    if (state->req->on_complete) {
        try {
            state->req->on_complete(
                state->callback_status, outputs, num_outputs,
                infer_time.count(), state->queue_time_us,
                true);  // model was acquired by worker
        } catch (const std::exception& e) {
            callback_failed = true;
            LOG_ERROR("Inference completion callback threw: %s", e.what());
        } catch (...) {
            callback_failed = true;
            LOG_ERROR("Inference completion callback threw");
        }
    }

    if (!state->req->owns_outputs) {
        try {
            state->mgr->free_outputs(outputs, state->max_outputs);
        } catch (...) {
            LOG_ERROR("Inference output cleanup threw");
        }
        try {
            state->mgr->release_model(state->req->model_id);
        } catch (...) {
            LOG_ERROR("Inference model release threw");
        }
    } else if (callback_failed) {
        // Ownership may already have moved into an asynchronous postprocess
        // task. Reclaiming it here would risk a double-free and use-after-free;
        // owning callbacks therefore provide their own no-throw RAII cleanup.
        LOG_ERROR("Owning inference callback failed after possible transfer; "
                  "scheduler cannot safely reclaim outputs");
    }

    // Application completion and cleanup run only after run_async() returned.
    // The HAL callback's shallow-copy vector is not retained; output_slots is
    // caller-owned storage and survives until both intrusive owners retire.
    // Async tracking is retired by the final owner only after deleting state,
    // including InferRequest::resource_holder and output slot storage.
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
                try {
                    req->on_complete(-1, nullptr, 0, 0,
                                     static_cast<uint64_t>(queue_time.count()),
                                     false);  // model not acquired
                } catch (const std::exception& e) {
                    LOG_ERROR("Inference completion callback threw: %s", e.what());
                } catch (...) {
                    LOG_ERROR("Inference completion callback threw");
                }
            }
            continue;
        }

        int max_outputs = snap->num_outputs;
        auto infer_start = SteadyClock::now();
        auto session = session_mgr_->get_session(req->session_id);

        // ── Async path (preferred): submit run_async, return immediately ──
        if (model_mgr_->has_async()) {
            WorkerCallbackState* cb_state = nullptr;
            int async_rc = HAL_ERROR;

            try {
                auto setup_state = std::make_unique<WorkerCallbackState>();
                setup_state->scheduler = this;
                setup_state->mgr = model_mgr_;
                setup_state->smgr = session_mgr_;
                setup_state->output_slots =
                    std::make_unique<HalTensor[]>(HAL_MAX_TENSORS);
                setup_state->max_outputs = max_outputs;
                setup_state->infer_start = infer_start;
                setup_state->queue_time_us =
                    static_cast<uint64_t>(queue_time.count());
                setup_state->session = session;
                setup_state->req = std::move(req);
                cb_state = setup_state.release();

                async_in_flight_.fetch_add(1);
                cb_state->tracker_active = true;
                async_rc = model_mgr_->run_async(
                    snap->infer_session,
                    cb_state->req->inputs,
                    cb_state->req->num_inputs,
                    cb_state->output_slots.get(), max_outputs,
                    &InferenceScheduler::on_hw_complete, cb_state);
            } catch (const std::exception& e) {
                LOG_ERROR("Async inference submission threw: %s", e.what());
            } catch (...) {
                LOG_ERROR("Async inference submission threw");
            }

            if (cb_state) {
                const auto action =
                    cb_state->gate.on_return(async_rc == HAL_OK);
                if (action == AsyncReturnAction::AwaitCallback) {
                    cb_state->release_owner();  // submitter owner
                    continue;
                }
                if (action == AsyncReturnAction::CompleteHere) {
                    complete_async(cb_state);
                    cb_state->release_owner();  // submitter owner
                    continue;
                }

                req = std::move(cb_state->req);
                try {
                    // A rejected backend may have allocated output backing
                    // storage before queue submission failed.
                    model_mgr_->free_outputs(
                        cb_state->output_slots.get(), max_outputs);
                } catch (...) {
                    LOG_ERROR("Async inference rollback cleanup threw");
                }
                cb_state->release_owner();  // unused callback owner
                cb_state->release_owner();  // submitter owner
            }
            // Fall through to synchronous inference with the restored request.
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

            bool callback_failed = false;
            if (req->on_complete) {
                try {
                    req->on_complete(rc, outputs, max_outputs,
                                     infer_time.count(), queue_time.count(),
                                     true);  // model was acquired
                } catch (const std::exception& e) {
                    callback_failed = true;
                    LOG_ERROR("Inference completion callback threw: %s", e.what());
                } catch (...) {
                    callback_failed = true;
                    LOG_ERROR("Inference completion callback threw");
                }
            }

            if (!req->owns_outputs) {
                model_mgr_->free_outputs(outputs, max_outputs);
                model_mgr_->release_model(req->model_id);
            } else if (callback_failed) {
                // Ownership may already have moved into an asynchronous
                // postprocess task. Owning callbacks must contain exceptions
                // and complete their own cleanup before returning.
                LOG_ERROR("Owning inference callback failed after possible "
                          "transfer; scheduler cannot safely reclaim outputs");
            }
        }

        LOG_DEBUG("Worker %d: model=%s queue=%luus",
                  worker_id, req->model_id.c_str(),
                  (unsigned long)queue_time.count());
    }

    LOG_DEBUG("Worker %d stopped", worker_id);
}

}  // namespace aipc::ai_runtime
