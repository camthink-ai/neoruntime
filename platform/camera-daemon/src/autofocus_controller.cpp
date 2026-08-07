/**
 * @file autofocus_controller.cpp
 * @brief Product autofocus orchestration using the proven demo math modules.
 */

#include "../include/autofocus_controller.h"
#include "../include/frame_router.h"
#include "../include/lens_controller.h"

#include "af_calibration.h"
#include "af_follow.h"
#include "af_integration_defaults.h"
#include "af_metric.h"
#include "af_scan.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C" {
#include "hal_common.h"
#include "hal_log.h"
}

namespace {

constexpr uint8_t kMotorStopped = 1;

uint64_t steady_now_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

bool motor_still(const LensControllerState& state) {
    return state.zoom_state == kMotorStopped && state.focus_state == kMotorStopped;
}

struct ScanResult {
    int error = HAL_OK;
    bool valid = false;
    bool confident = false;
    bool best_on_edge = false;
    int best_pos = 0;
    double metric = 0.0;
    double confidence = 0.0;
    double reproducibility = 0.0;
    double prominence = 0.0;
    double temporal_stability = 0.0;
    double texture_coverage = 0.0;
    double luma_stability = 0.0;
    hal_auto_af::PeakType peak_type = hal_auto_af::PeakType::EdgePeak;
    bool verification_passed = false;
    int scan_lo = 0;
    int scan_hi = 0;
    std::string message;
};

struct PendingJob {
    uint64_t id = 0;
    AutofocusOperation operation = AutofocusOperation::None;
    float ratio = 1.0f;
};

struct CachedFocusSample {
    hal_auto_af::FocusSample sample;
    int metric_frames = 0;
};

}  // namespace

const char* autofocus_operation_name(AutofocusOperation operation) {
    switch (operation) {
        case AutofocusOperation::Startup: return "startup";
        case AutofocusOperation::OneShot: return "oneshot";
        case AutofocusOperation::ZoomFollow: return "zoom_follow";
        default: return "none";
    }
}

const char* autofocus_state_name(AutofocusState state) {
    switch (state) {
        case AutofocusState::Initializing: return "initializing";
        case AutofocusState::StartupAf: return "startup_af";
        case AutofocusState::Coarse: return "coarse";
        case AutofocusState::Fine: return "fine";
        case AutofocusState::Verifying: return "verifying";
        case AutofocusState::PathMoving: return "path_moving";
        case AutofocusState::EndpointAf: return "endpoint_af";
        case AutofocusState::Recovering: return "recovering";
        case AutofocusState::Completed: return "completed";
        case AutofocusState::Failed: return "failed";
        case AutofocusState::Cancelled: return "cancelled";
        default: return "idle";
    }
}

class AutofocusController::Impl {
public:
    Impl(HalIspOps* isp_ops, HalVideoOps* video_ops, void* video_ctx,
         FrameRouter* frame_router, LensController* lens,
         const AutofocusConfig& config,
         int sensor_native_width, int sensor_native_height)
        : isp_ops_(isp_ops), video_ops_(video_ops), video_ctx_(video_ctx),
          frame_router_(frame_router), lens_(lens), config_(config) {
        // Priority: explicit constructor params > config fields > video_ctx query
        if (sensor_native_width > 0 && sensor_native_height > 0) {
            af_native_width_ = sensor_native_width;
            af_native_height_ = sensor_native_height;
        } else if (config_.sensor_native_width > 0 && config_.sensor_native_height > 0) {
            af_native_width_ = config_.sensor_native_width;
            af_native_height_ = config_.sensor_native_height;
        } else if (video_ops_ && video_ops_->get_current_config) {
            HalVideoConfig native{};
            if (video_ops_->get_current_config(video_ctx, &native) == HAL_OK &&
                native.width > 0 && native.height > 0) {
                af_native_width_ = static_cast<int>(native.width);
                af_native_height_ = static_cast<int>(native.height);
            }
        }
        HAL_LOG_INFO("Autofocus: native sensor resolution %dx%d",
                     af_native_width_, af_native_height_);
        metric_config_ = defaults_.metric;
        follow_config_ = defaults_.follow;
        config_.confidence_accept = std::clamp(config_.confidence_accept, 0.0, 1.0);
        config_.confidence_recovery = std::clamp(
            config_.confidence_recovery, 0.0, config_.confidence_accept);
        config_.follow_sync_min_pps = std::clamp(config_.follow_sync_min_pps, 24, 4000);
        config_.follow_sync_zoom_max_pps = std::clamp(
            config_.follow_sync_zoom_max_pps, config_.follow_sync_min_pps, 4000);
        config_.follow_sync_focus_max_pps = std::clamp(
            config_.follow_sync_focus_max_pps, config_.follow_sync_min_pps, 4000);
        config_.follow_sync_zoom_tolerance =
            std::max(0, config_.follow_sync_zoom_tolerance);
        config_.follow_sync_focus_tolerance =
            std::max(0, config_.follow_sync_focus_tolerance);
        follow_config_.follow_path_zoom_step_wide =
            std::max(1, config_.follow_path_zoom_step_wide);
        follow_config_.follow_path_zoom_step_mid =
            std::max(1, config_.follow_path_zoom_step_mid);
        follow_config_.follow_path_zoom_step_tele =
            std::max(1, config_.follow_path_zoom_step_tele);
        follow_config_.follow_path_focus_step_wide =
            std::max(1, config_.follow_path_focus_step_wide);
        follow_config_.follow_path_focus_step_mid =
            std::max(1, config_.follow_path_focus_step_mid);
        follow_config_.follow_path_focus_step_tele =
            std::max(1, config_.follow_path_focus_step_tele);
        follow_config_.follow_path_curve_error_enable =
            config_.follow_curve_error_enable ? 1 : 0;
        follow_config_.follow_path_curve_error_wide =
            std::max(0, config_.follow_curve_error_wide);
        follow_config_.follow_path_curve_error_mid =
            std::max(0, config_.follow_curve_error_mid);
        follow_config_.follow_path_curve_error_tele =
            std::max(0, config_.follow_curve_error_tele);
        HAL_LOG_INFO("Autofocus: follow motion=%s pps=[zoom:%d focus:%d min:%d] "
                     "zoom_steps=[%d,%d,%d] focus_steps=[%d,%d,%d] "
                     "curve_error=[%d,%d,%d]",
                     config_.follow_sync_motion ? "dual-axis-sync" : "sequential",
                     config_.follow_sync_zoom_max_pps,
                     config_.follow_sync_focus_max_pps,
                     config_.follow_sync_min_pps,
                     follow_config_.follow_path_zoom_step_wide,
                     follow_config_.follow_path_zoom_step_mid,
                     follow_config_.follow_path_zoom_step_tele,
                     follow_config_.follow_path_focus_step_wide,
                     follow_config_.follow_path_focus_step_mid,
                     follow_config_.follow_path_focus_step_tele,
                     follow_config_.follow_path_curve_error_wide,
                     follow_config_.follow_path_curve_error_mid,
                     follow_config_.follow_path_curve_error_tele);
        if (!config_.calibration_path.empty()) {
            if (hal_auto_af::load_calibration_file(config_.calibration_path.c_str(), &calibration_)) {
                HAL_LOG_INFO("Autofocus: loaded calibration %s", config_.calibration_path.c_str());
            } else {
                HAL_LOG_WARNING("Autofocus: calibration unavailable: %s; using fixed bias",
                                config_.calibration_path.c_str());
            }
        } else {
            HAL_LOG_INFO("Autofocus: using AF_demo fixed compensation (strength=%.2f)",
                         follow_config_.follow_calibration_strength);
        }
    }

    ~Impl() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        worker_ = std::thread(&Impl::worker_loop, this);
    }

    void stop() {
        if (!running_.exchange(false)) return;
        cancel_requested_.store(true);
        job_cv_.notify_all();
        if (lens_ && lens_->autofocus_operation_active()) lens_->stop_all(1000);
        if (worker_.joinable()) worker_.join();
    }

    bool enqueue(AutofocusOperation operation, float ratio,
                 uint64_t* job_id, std::string* error) {
        if (!config_.enabled || !running_.load()) {
            if (error) *error = "autofocus is disabled or not running";
            return false;
        }
        std::lock_guard<std::mutex> lock(mu_);
        if (status_.busy || pending_.has_value()) {
            if (error) *error = "autofocus operation already active";
            return false;
        }
        PendingJob job;
        job.id = next_job_id_++;
        job.operation = operation;
        job.ratio = ratio;
        pending_ = job;
        status_.job_id = job.id;
        status_.operation = operation;
        status_.state = AutofocusState::Initializing;
        status_.progress = 0.0;
        status_.busy = true;
        status_.requested_ratio = ratio;
        status_.error_code = HAL_OK;
        status_.message = "queued";
        job_started_ms_ = steady_now_ms();
        cancel_requested_.store(false);
        if (job_id) *job_id = job.id;
        job_cv_.notify_one();
        return true;
    }

    bool cancel(uint64_t job_id, std::string* error) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!status_.busy) {
                // Idempotent no-op: no active AF job to cancel. This covers the
                // job_id==0 wildcard ("ensure AF is off") used by the web console,
                // model-showcase, and SDK when no one-shot AF was started.
                // Returning success lets those callers proceed to manual-focus or
                // AF-enable without a spurious precondition failure.
                return true;
            }
            if (job_id != 0 && status_.job_id != job_id) {
                if (error) *error = "autofocus job id mismatch";
                return false;  // tried to cancel a different running job
            }
        }
        cancel_requested_.store(true);
        if (lens_) lens_->stop_all(1000);
        job_cv_.notify_all();
        return true;
    }

    void invalidate_anchor(const std::string& reason) {
        std::lock_guard<std::mutex> lock(mu_);
        anchor_.valid = false;
        status_.anchor_valid = false;
        if (!status_.busy) status_.message = "anchor invalidated: " + reason;
        HAL_LOG_INFO("Autofocus: anchor invalidated: %s", reason.c_str());
    }

    AutofocusStatus status() const {
        std::lock_guard<std::mutex> lock(mu_);
        AutofocusStatus copy = status_;
        copy.anchor_valid = anchor_.valid;
        copy.estimated_distance_m = anchor_.estimated_distance_m;
        if (copy.busy && job_started_ms_ > 0) {
            copy.elapsed_ms = steady_now_ms() - job_started_ms_;
        }
        return copy;
    }

    void update_video_context(void* video_ctx) {
        video_ctx_.store(video_ctx);
        invalidate_anchor("video context changed");
    }

private:
    HalIspOps* isp_ops_ = nullptr;
    HalVideoOps* video_ops_ = nullptr;
    std::atomic<void*> video_ctx_{nullptr};
    FrameRouter* frame_router_ = nullptr;
    LensController* lens_ = nullptr;
    AutofocusConfig config_;
    hal_auto_af::IntegrationDefaults defaults_{};
    hal_auto_af::MetricConfig metric_config_{};
    hal_auto_af::FollowConfig follow_config_{};
    hal_auto_af::CalibrationProfile calibration_{};

    // Sensor native resolution captured at construction time.
    // AF windows use these coordinates so focus quality is independent
    // of encoder output resolution changes.
    int af_native_width_ = 0;
    int af_native_height_ = 0;

    mutable std::mutex mu_;
    std::condition_variable job_cv_;
    std::optional<PendingJob> pending_;
    AutofocusStatus status_{};
    AutofocusAnchor anchor_{};
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> cancel_requested_{false};
    uint64_t next_job_id_ = 1;
    uint64_t job_started_ms_ = 0;

    void worker_loop() {
        if (config_.startup_af && config_.enabled) {
            uint64_t startup_id = 0;
            std::string ignored;
            enqueue(AutofocusOperation::Startup, 1.0f, &startup_id, &ignored);
        }

        while (running_.load()) {
            PendingJob job;
            {
                std::unique_lock<std::mutex> lock(mu_);
                job_cv_.wait(lock, [&] { return !running_.load() || pending_.has_value(); });
                if (!running_.load()) break;
                job = *pending_;
                pending_.reset();
            }
            run_job(job);
        }
    }

    void set_state(AutofocusState state, double progress, const std::string& message) {
        std::lock_guard<std::mutex> lock(mu_);
        status_.state = state;
        status_.progress = std::clamp(progress, 0.0, 1.0);
        status_.message = message;
    }

    void finish_job(AutofocusState state, int error, const std::string& message) {
        std::lock_guard<std::mutex> lock(mu_);
        if (state == AutofocusState::Failed || state == AutofocusState::Cancelled) {
            anchor_.valid = false;
        }
        status_.state = state;
        status_.progress = state == AutofocusState::Completed ? 1.0 : status_.progress;
        status_.busy = false;
        status_.error_code = error;
        status_.message = message;
        status_.anchor_valid = anchor_.valid;
        status_.estimated_distance_m = anchor_.estimated_distance_m;
        status_.elapsed_ms = job_started_ms_ > 0 ? steady_now_ms() - job_started_ms_ : 0;
    }

    bool cancelled() const {
        return cancel_requested_.load() || !running_.load();
    }

    bool wait_lens_ready() {
        const auto deadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(std::max(1000, config_.startup_ready_timeout_ms));
        int stable_reads = 0;
        while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
            LensControllerState state{};
            if (lens_ && lens_->initialized() && lens_->af0832_bootstrapped() &&
                lens_->state_get(&state) == HAL_OK && motor_still(state)) {
                if (++stable_reads >= 5) return true;
            } else {
                stable_reads = 0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    bool configure_windows() {
        void* video_ctx = video_ctx_.load();
        if (!isp_ops_ || !isp_ops_->set_af_windows_config || !video_ctx) {
            HAL_LOG_WARNING("Autofocus: configure_windows skipped — isp=%p set_af=%p ctx=%p",
                         (void*)isp_ops_, (void*)(isp_ops_ ? isp_ops_->set_af_windows_config : nullptr),
                         video_ctx);
            return false;
        }

        // AF statistics are computed by ISP firmware on raw sensor data, not on
        // the scaled output.  Use the native resolution captured at construction
        // so window size and placement stay at full sensor quality regardless of
        // the current encoder output resolution.
        int window_w = af_native_width_;
        int window_h = af_native_height_;
        if (window_w <= 0 || window_h <= 0) {
            HalVideoConfig video{};
            if (!video_ops_ || !video_ops_->get_current_config ||
                video_ops_->get_current_config(video_ctx, &video) != HAL_OK ||
                video.width == 0 || video.height == 0) {
                HAL_LOG_WARNING("Autofocus: configure_windows — cannot read video config, fallback failed");
                return false;
            }
            window_w = static_cast<int>(video.width);
            window_h = static_cast<int>(video.height);
        }

        // Read current video_ctx resolution for logging only (to confirm mismatch)
        HalVideoConfig cur{};
        bool cur_ok = video_ops_ && video_ops_->get_current_config &&
                      video_ops_->get_current_config(video_ctx, &cur) == HAL_OK;

        const HalIspAfWindowsConfig windows =
            hal_auto_af::make_default_af_windows(window_w, window_h);
        HAL_LOG_INFO("Autofocus: configuring AF windows native=%dx%d cur=%dx%d ctx=%p enabled=%d count=%u",
                     window_w, window_h,
                     cur_ok ? cur.width : 0, cur_ok ? cur.height : 0,
                     video_ctx,
                     windows.enabled, windows.window_count);

        int ret = isp_ops_->set_af_windows_config(video_ctx, &windows);
        if (ret != HAL_OK) {
            HAL_LOG_ERROR("Autofocus: set_af_windows_config failed ret=%d native=%dx%d cur=%dx%d",
                          ret, window_w, window_h,
                          cur_ok ? cur.width : 0, cur_ok ? cur.height : 0);
            return false;
        }
        HAL_LOG_INFO("Autofocus: AF windows configured OK");
        return true;
    }

    int wait_frames(int count) {
        if (!frame_router_) return HAL_ERR_NOT_READY;
        return frame_router_->wait_next_frames(
                   config_.stream_name, std::max(1, count),
                   std::chrono::milliseconds(std::max(100, config_.frame_wait_timeout_ms)))
                   ? HAL_OK : HAL_ERR_TIMEOUT;
    }

    int read_observation(int sync_frames, int metric_frames,
                         hal_auto_af::MetricObservation* observation) {
        void* video_ctx = video_ctx_.load();
        if (!observation || !isp_ops_ || !isp_ops_->get_af_measurement || !video_ctx)
            return HAL_ERR_INVALID_ARG;
        int ret = wait_frames(sync_frames);
        if (ret != HAL_OK) return ret;
        std::vector<HalIspAfMeasurement> measurements;
        measurements.reserve(std::max(1, metric_frames));
        for (int i = 0; i < std::max(1, metric_frames); ++i) {
            HalIspAfMeasurement measurement{};
            ret = isp_ops_->get_af_measurement(video_ctx, &measurement);
            if (ret != HAL_OK) return ret;
            measurements.push_back(measurement);
            if (i + 1 < metric_frames && (ret = wait_frames(1)) != HAL_OK) return ret;
        }
        *observation = hal_auto_af::observation_from_frames(measurements, metric_config_);
        return observation->valid_mask == 0 ? HAL_ERR_INVALID_STATE : HAL_OK;
    }

    int measure_focus(int position, int metric_frames,
                      hal_auto_af::FocusSample* sample,
                      const hal_auto_af::TextureModel* texture = nullptr) {
        if (cancelled()) return HAL_ERR_INVALID_STATE;
        const int ret = lens_->focus_abs_wait(config_.pps, position, config_.move_timeout_ms);
        if (ret != HAL_OK) return ret;
        hal_auto_af::MetricObservation observation{};
        const int read_ret = read_observation(1, metric_frames, &observation);
        if (read_ret != HAL_OK) return read_ret;
        if (texture) hal_auto_af::apply_texture_model(&observation, *texture, metric_config_);
        if (sample) *sample = {position, observation.metric, observation};
        return HAL_OK;
    }

    int take_sample(int position, int metric_frames,
                    std::vector<hal_auto_af::FocusSample>* curve,
                    std::unordered_map<int, CachedFocusSample>* cache,
                    int* moves, int* best_pos, double* best_metric) {
        if (!curve || !cache || !moves || !best_pos || !best_metric)
            return HAL_ERR_INVALID_ARG;
        const auto cached = cache->find(position);
        if (cached != cache->end() && cached->second.metric_frames >= metric_frames) {
            curve->push_back(cached->second.sample);
            if (cached->second.sample.m > *best_metric) {
                *best_metric = cached->second.sample.m;
                *best_pos = position;
            }
            return HAL_OK;
        }
        if (++*moves > config_.max_moves) return HAL_ERR_INVALID_STATE;
        hal_auto_af::FocusSample sample{};
        const int ret = measure_focus(position, metric_frames, &sample);
        if (ret != HAL_OK) return ret;
        curve->push_back(sample);
        (*cache)[position] = {sample, metric_frames};
        if (sample.m > *best_metric) {
            *best_metric = sample.m;
            *best_pos = position;
        }
        return HAL_OK;
    }

    int scan_symmetric(int center, int lo, int hi, int step, int metric_frames,
                       std::vector<hal_auto_af::FocusSample>* curve,
                       std::unordered_map<int, CachedFocusSample>* cache,
                       int* moves, int* best_pos, double* best_metric) {
        const int c = std::clamp(center, lo, hi);
        const int s = std::max(1, step);
        int ret = take_sample(c, metric_frames, curve, cache, moves, best_pos, best_metric);
        if (ret != HAL_OK) return ret;
        int right = c;
        while (right + s <= hi) {
            if (hal_auto_af::fit_stop_extend_right(
                    *curve, s, defaults_.fit_min_samples, defaults_.fit_min_curvature,
                    lo, hi, defaults_.early_stop_min_spread)) break;
            right += s;
            ret = take_sample(right, metric_frames, curve, cache, moves, best_pos, best_metric);
            if (ret != HAL_OK) return ret;
        }
        int left = c;
        while (left - s >= lo) {
            if (hal_auto_af::fit_stop_extend_left(
                    *curve, s, defaults_.fit_min_samples, defaults_.fit_min_curvature,
                    lo, hi, defaults_.early_stop_min_spread)) break;
            left -= s;
            ret = take_sample(left, metric_frames, curve, cache, moves, best_pos, best_metric);
            if (ret != HAL_OK) return ret;
        }
        return HAL_OK;
    }

    ScanResult run_scan(int center, int coarse_span, bool balanced,
                        bool endpoint) {
        ScanResult result;
        const int min_pos = std::min(config_.min_focus_pos, config_.max_focus_pos);
        const int max_pos = std::max(config_.min_focus_pos, config_.max_focus_pos);
        const int span = std::max(config_.coarse_step, coarse_span);
        const int coarse_lo = std::max(min_pos, center - span);
        const int coarse_hi = std::min(max_pos, center + span);
        result.scan_lo = coarse_lo;
        result.scan_hi = coarse_hi;

        std::unordered_map<int, CachedFocusSample> cache;
        std::vector<hal_auto_af::FocusSample> coarse_curve;
        std::vector<hal_auto_af::FocusSample> fine_curve;
        int moves = 0;
        int best_pos = center;
        double best_metric = -1.0;

        set_state(AutofocusState::Coarse, endpoint ? 0.88 : 0.15,
                  balanced ? "balanced coarse scan" : "fast coarse scan");
        int ret = scan_symmetric(center, coarse_lo, coarse_hi, config_.coarse_step, 1,
                                 &coarse_curve, &cache, &moves, &best_pos, &best_metric);
        if (ret != HAL_OK) {
            result.error = ret;
            result.message = "coarse scan failed";
            return result;
        }

        auto texture = hal_auto_af::build_texture_model({&coarse_curve}, metric_config_);
        hal_auto_af::apply_texture_reliability(&coarse_curve, texture, metric_config_);
        hal_auto_af::discrete_best(coarse_curve, &best_pos, &best_metric);
        int fine_center = best_pos;
        const double fitted = hal_auto_af::fitted_peak_x(
            coarse_curve, defaults_.fit_min_samples, defaults_.fit_min_curvature);
        const int fitted_center = static_cast<int>(std::lround(
            std::clamp(fitted, static_cast<double>(min_pos), static_cast<double>(max_pos))));
        if (std::abs(fitted_center - best_pos) <= config_.coarse_step) fine_center = fitted_center;

        const int fine_span = std::max(config_.fine_step, config_.fine_span);
        const int fine_lo = std::max(min_pos, fine_center - fine_span);
        const int fine_hi = std::min(max_pos, fine_center + fine_span);
        int fine_best = best_pos;
        double fine_metric = best_metric;
        set_state(AutofocusState::Fine, endpoint ? 0.92 : 0.48,
                  balanced ? "balanced fine scan" : "fast fine scan");
        ret = scan_symmetric(fine_center, fine_lo, fine_hi, config_.fine_step,
                             balanced ? 3 : 1, &fine_curve, &cache, &moves,
                             &fine_best, &fine_metric);
        if (ret != HAL_OK) {
            result.error = ret;
            result.message = "fine scan failed";
            return result;
        }

        texture = hal_auto_af::build_texture_model({&coarse_curve, &fine_curve}, metric_config_);
        hal_auto_af::apply_texture_reliability(&coarse_curve, texture, metric_config_);
        hal_auto_af::apply_texture_reliability(&fine_curve, texture, metric_config_);
        std::vector<hal_auto_af::FocusSample> merged;
        hal_auto_af::merge_curves(coarse_curve, fine_curve, &merged);
        int global_pos = best_pos;
        double global_metric = best_metric;
        hal_auto_af::discrete_best(merged, &global_pos, &global_metric);

        if (global_pos < fine_lo || global_pos > fine_hi) {
            std::vector<hal_auto_af::FocusSample> fine2;
            int fine2_best = global_pos;
            double fine2_metric = -1.0;
            const int lo2 = std::max(min_pos, global_pos - fine_span);
            const int hi2 = std::min(max_pos, global_pos + fine_span);
            ret = scan_symmetric(global_pos, lo2, hi2, config_.fine_step,
                                 balanced ? 3 : 1, &fine2, &cache, &moves,
                                 &fine2_best, &fine2_metric);
            if (ret != HAL_OK) {
                result.error = ret;
                result.message = "second fine scan failed";
                return result;
            }
            texture = hal_auto_af::build_texture_model({&coarse_curve, &fine_curve, &fine2},
                                                        metric_config_);
            hal_auto_af::apply_texture_reliability(&coarse_curve, texture, metric_config_);
            hal_auto_af::apply_texture_reliability(&fine_curve, texture, metric_config_);
            hal_auto_af::apply_texture_reliability(&fine2, texture, metric_config_);
            hal_auto_af::merge_curves(coarse_curve, fine_curve, &merged);
            hal_auto_af::merge_curves(merged, fine2, &merged);
            hal_auto_af::discrete_best(merged, &global_pos, &global_metric);
        }

        std::vector<hal_auto_af::FocusSample> ranked = merged;
        std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
            return a.m == b.m ? a.pos < b.pos : a.m > b.m;
        });
        set_state(AutofocusState::Verifying, endpoint ? 0.97 : 0.78,
                  "verifying focus peak");
        int picked_pos = global_pos;
        double picked_curve_metric = global_metric;
        double picked_metric = -1.0;
        double best_verified = -1.0;
        int best_verified_pos = global_pos;
        hal_auto_af::MetricObservation picked_observation{};
        hal_auto_af::MetricObservation best_verified_observation{};
        bool verification_passed = false;
        for (int i = 0; i < static_cast<int>(ranked.size()) &&
                        i < defaults_.peak_verify_max_tries; ++i) {
            hal_auto_af::FocusSample verified{};
            ret = measure_focus(ranked[i].pos, defaults_.verification_metric_frames,
                                &verified, &texture);
            if (ret != HAL_OK) continue;
            if (verified.m > best_verified) {
                best_verified = verified.m;
                best_verified_pos = ranked[i].pos;
                best_verified_observation = verified.observation;
            }
            if (ranked[i].m <= 1e-12 ||
                verified.m >= defaults_.peak_verify_min_frac * ranked[i].m) {
                picked_pos = ranked[i].pos;
                picked_curve_metric = ranked[i].m;
                picked_metric = verified.m;
                picked_observation = verified.observation;
                verification_passed = true;
                break;
            }
        }
        if (picked_metric < 0.0 && best_verified >= 0.0) {
            picked_pos = best_verified_pos;
            picked_curve_metric = hal_auto_af::metric_at(merged, picked_pos);
            picked_metric = best_verified;
            picked_observation = best_verified_observation;
        }
        if (picked_metric < 0.0) {
            result.error = ret == HAL_OK ? HAL_ERR_RESULT : ret;
            result.message = "peak verification failed";
            return result;
        }

        auto existing = std::find_if(merged.begin(), merged.end(), [picked_pos](const auto& sample) {
            return sample.pos == picked_pos;
        });
        const hal_auto_af::FocusSample verified_sample{
            picked_pos, picked_metric, picked_observation};
        if (existing == merged.end()) {
            merged.push_back(verified_sample);
            std::sort(merged.begin(), merged.end(), [](const auto& a, const auto& b) {
                return a.pos < b.pos;
            });
        } else {
            *existing = verified_sample;
        }

        result.valid = true;
        result.best_pos = picked_pos;
        result.metric = picked_metric;
        result.verification_passed = verification_passed;
        result.reproducibility = picked_curve_metric > 1e-12
            ? std::clamp(picked_metric / picked_curve_metric, 0.0, 1.5) : 1.0;
        result.prominence = hal_auto_af::peak_prominence(merged, picked_pos);
        const double prominence_score = std::clamp(
            result.prominence / defaults_.peak_min_prominence, 0.0, 1.0);
        result.best_on_edge = picked_pos <= coarse_lo + follow_config_.follow_edge_margin_steps ||
                              picked_pos >= coarse_hi - follow_config_.follow_edge_margin_steps;
        hal_auto_af::PeakClosureConfig closure_config{};
        closure_config.noise_floor = balanced ? defaults_.peak_noise_floor
                                              : defaults_.peak_fast_noise_floor;
        closure_config.plateau_ratio = defaults_.peak_plateau_ratio;
        closure_config.focus_step = config_.fine_step;
        closure_config.open_boundary_margin = defaults_.peak_open_boundary_margin_steps;
        const auto closure = hal_auto_af::classify_peak(
            merged, picked_pos, coarse_lo, coarse_hi, min_pos, max_pos, closure_config);
        result.peak_type = closure.type;

        int texture_count = 0;
        for (size_t i = 0; i < picked_observation.texture.size(); ++i) {
            if ((picked_observation.valid_mask & (1u << i)) == 0u) continue;
            result.texture_coverage += picked_observation.texture[i];
            ++texture_count;
        }
        if (texture_count > 0) result.texture_coverage /= static_cast<double>(texture_count);
        result.temporal_stability = picked_observation.temporal_stability;
        result.luma_stability = hal_auto_af::luma_stability_from_observation(
            picked_observation, defaults_.peak_luma_mad_limit);

        hal_auto_af::ConfidenceV2Inputs confidence_inputs{};
        confidence_inputs.reproducibility = std::min(1.0, result.reproducibility);
        confidence_inputs.peak_type = result.peak_type;
        confidence_inputs.prominence_score = prominence_score;
        confidence_inputs.temporal_stability = result.temporal_stability;
        confidence_inputs.texture_coverage = result.texture_coverage;
        confidence_inputs.luma_stability = result.luma_stability;
        confidence_inputs.scene_stable = true;
        confidence_inputs.verification_passed = result.verification_passed;
        result.confidence = hal_auto_af::confidence_v2(confidence_inputs);
        result.confident = result.confidence >= config_.confidence_accept &&
                           !result.best_on_edge;
        result.message = result.confident ? "focus acquired" : "low confidence focus";
        HAL_LOG_INFO("Autofocus: peak=%s confidence_v2=%.3f repro=%.3f prominence=%.3f "
                     "temporal=%.3f texture=%.3f luma=%.3f verify=%d edge=%d",
                     hal_auto_af::peak_type_name(result.peak_type), result.confidence,
                     result.reproducibility, prominence_score, result.temporal_stability,
                     result.texture_coverage, result.luma_stability,
                     result.verification_passed ? 1 : 0, result.best_on_edge ? 1 : 0);
        return result;
    }

    int calibration_delta(float ratio) const {
        int32_t delta = 0;
        hal_auto_af::calibration_delta_for_ratio(
            calibration_, ratio, follow_config_.follow_use_conservative_bias != 0,
            follow_config_.follow_calibration_strength, &delta);
        return delta;
    }

    int move_zoom_focus(int32_t zoom_target, int32_t focus_target,
                        const char* operation) {
        LensControllerState before{};
        int ret = lens_->state_get(&before);
        if (ret != HAL_OK) return ret;

        const int zoom_distance = std::abs(zoom_target - before.zoom_pos);
        const int focus_distance = std::abs(focus_target - before.focus_pos);
        if (zoom_distance == 0 && focus_distance == 0) return HAL_OK;

        const double zoom_time = zoom_distance > 0
            ? static_cast<double>(zoom_distance) / config_.follow_sync_zoom_max_pps
            : 0.0;
        const double focus_time = focus_distance > 0
            ? static_cast<double>(focus_distance) / config_.follow_sync_focus_max_pps
            : 0.0;
        const double segment_time = std::max(zoom_time, focus_time);
        const auto axis_pps = [&](int distance, int max_pps) {
            if (distance <= 0 || segment_time <= 0.0) return config_.follow_sync_min_pps;
            return std::clamp(
                static_cast<int>(std::lround(distance / segment_time)),
                config_.follow_sync_min_pps, max_pps);
        };
        const int zoom_pps = axis_pps(
            zoom_distance, config_.follow_sync_zoom_max_pps);
        const int focus_pps = axis_pps(
            focus_distance, config_.follow_sync_focus_max_pps);

        bool used_sync = false;
        if (config_.follow_sync_motion) {
            ret = lens_->zoom_focus_abs_wait(
                zoom_pps, zoom_target, focus_pps, focus_target,
                config_.move_timeout_ms);
            used_sync = ret == HAL_OK;
            if (ret == HAL_ERR_NOT_SUPPORTED &&
                config_.follow_sync_fallback_sequential) {
                HAL_LOG_WARNING("Autofocus: %s synchronized motion unavailable; "
                                "using sequential fallback", operation);
            } else if (ret != HAL_OK) {
                HAL_LOG_ERROR("Autofocus: %s synchronized motion failed ret=%d "
                              "target=[%d,%d]", operation, ret,
                              zoom_target, focus_target);
                return ret;
            }
        }

        if (!config_.follow_sync_motion ||
            (ret == HAL_ERR_NOT_SUPPORTED &&
             config_.follow_sync_fallback_sequential)) {
            if (zoom_distance > 0) {
                ret = lens_->zoom_abs_wait(
                    zoom_pps, zoom_target, config_.move_timeout_ms);
            }
            if (ret == HAL_OK && focus_distance > 0) {
                ret = lens_->focus_abs_wait(
                    focus_pps, focus_target, config_.move_timeout_ms);
            }
            if (ret != HAL_OK) return ret;
        }

        LensControllerState after{};
        ret = lens_->state_get(&after);
        if (ret != HAL_OK) return ret;
        const int zoom_error = std::abs(after.zoom_pos - zoom_target);
        const int focus_error = std::abs(after.focus_pos - focus_target);
        if (zoom_error > config_.follow_sync_zoom_tolerance ||
            focus_error > config_.follow_sync_focus_tolerance) {
            HAL_LOG_ERROR("Autofocus: %s position mismatch target=[%d,%d] "
                          "actual=[%d,%d] error=[%d,%d]",
                          operation, zoom_target, focus_target,
                          after.zoom_pos, after.focus_pos,
                          zoom_error, focus_error);
            return HAL_ERR_RESULT;
        }

        HAL_LOG_INFO("Autofocus: %s %s delta=[%d,%d] pps=[%d,%d] "
                     "target=[%d,%d]",
                     operation, used_sync ? "sync" : "sequential",
                     zoom_target - before.zoom_pos,
                     focus_target - before.focus_pos,
                     zoom_pps, focus_pps, zoom_target, focus_target);
        return HAL_OK;
    }

    int prepare_startup_position(int* focus_center) {
        if (!focus_center || !std::isfinite(config_.startup_zoom_ratio) ||
            config_.startup_zoom_ratio <= 0.0f ||
            !std::isfinite(config_.startup_focus_distance_m) ||
            config_.startup_focus_distance_m <= 0.0f) {
            return HAL_ERR_INVALID_ARG;
        }

        int32_t zoom_target = 0;
        int32_t focus_target = 0;
        int ret = lens_->calc_targets(config_.startup_zoom_ratio,
                                      config_.startup_focus_distance_m,
                                      &zoom_target, &focus_target);
        if (ret != HAL_OK) return ret;

        const float effective_ratio = lens_->pos_to_ratio(zoom_target);
        const int delta = calibration_delta(effective_ratio);
        focus_target = std::clamp(focus_target + delta,
                                  config_.min_focus_pos, config_.max_focus_pos);

        HAL_LOG_INFO("Autofocus: startup seed ratio=%.3f distance=%.2fm "
                     "zoom=%d focus=%d calibration_delta=%d",
                     effective_ratio, config_.startup_focus_distance_m,
                     zoom_target, focus_target, delta);
        set_state(AutofocusState::StartupAf, 0.04,
                  "positioning startup autofocus seed");
        ret = move_zoom_focus(zoom_target, focus_target, "startup seed");
        if (ret != HAL_OK) return ret;

        *focus_center = focus_target;
        {
            std::lock_guard<std::mutex> lock(mu_);
            status_.effective_ratio = effective_ratio;
            status_.zoom_pos = zoom_target;
            status_.focus_pos = focus_target;
        }
        return HAL_OK;
    }

    void update_anchor(const ScanResult& result) {
        LensControllerState lens_state{};
        if (!result.confident || lens_->state_get(&lens_state) != HAL_OK) return;
        const float ratio = lens_->pos_to_ratio(lens_state.zoom_pos);
        const int table_focus = result.best_pos - calibration_delta(ratio);
        float distance = 0.0f;
        if (lens_->estimate_distance(ratio, table_focus, &distance) != HAL_OK) return;
        std::lock_guard<std::mutex> lock(mu_);
        if (anchor_.valid && anchor_.estimated_distance_m > 0.0f && distance > 0.0f) {
            distance = hal_auto_af::blend_finite_distance(
                anchor_.estimated_distance_m, distance, follow_config_.follow_distance_alpha);
        }
        anchor_.valid = true;
        anchor_.zoom_ratio = ratio;
        anchor_.zoom_pos = lens_state.zoom_pos;
        anchor_.best_focus = result.best_pos;
        anchor_.estimated_distance_m = distance;
        anchor_.metric = result.metric;
        anchor_.confidence = result.confidence;
        anchor_.timestamp_ms = steady_now_ms();
        status_.anchor_valid = true;
        status_.effective_ratio = ratio;
        status_.zoom_pos = lens_state.zoom_pos;
        status_.focus_pos = lens_state.focus_pos;
        status_.best_focus = result.best_pos;
        status_.metric = result.metric;
        status_.confidence = result.confidence;
        status_.reproducibility = result.reproducibility;
        status_.estimated_distance_m = distance;
    }

    ScanResult run_one_shot_at(int center, int span, bool endpoint,
                               bool startup = false) {
        ScanResult result = run_scan(center, span, false, endpoint);
        if (cancelled()) return result;
        const bool weak_shape = result.peak_type == hal_auto_af::PeakType::OneSideWeak ||
                                result.peak_type == hal_auto_af::PeakType::OpenLeft ||
                                result.peak_type == hal_auto_af::PeakType::OpenRight ||
                                result.peak_type == hal_auto_af::PeakType::EdgePeak;
        const bool ordinary_recovery = result.error == HAL_OK && result.valid &&
            !result.confident && result.confidence >= config_.confidence_recovery;
        const bool endpoint_recovery = endpoint && result.error == HAL_OK && result.valid &&
            !result.confident && (result.best_on_edge || weak_shape);
        const bool startup_recovery = startup && result.error == HAL_OK && result.valid &&
            !result.confident;
        if (config_.balanced_retry != 0 &&
            (ordinary_recovery || endpoint_recovery || startup_recovery)) {
            set_state(AutofocusState::Recovering, endpoint ? 0.90 : 0.62,
                      "balanced autofocus recovery");
            const int recovery_center = result.valid ? result.best_pos : center;
            const int recovery_span = startup
                ? std::max(config_.startup_recovery_span, span * 2)
                : (endpoint ? follow_config_.follow_recovery_span_steps
                            : std::max(follow_config_.follow_fallback_span_steps,
                                       span * 2));
            if (startup) {
                HAL_LOG_WARNING("Autofocus: startup Fast AF not accepted "
                                "(confidence=%.3f peak=%s edge=%d); "
                                "retrying Balanced center=%d span=%d",
                                result.confidence,
                                hal_auto_af::peak_type_name(result.peak_type),
                                result.best_on_edge ? 1 : 0,
                                recovery_center, recovery_span);
            }
            result = run_scan(recovery_center, recovery_span, true, endpoint);
        }
        if (result.error == HAL_OK && result.confident) update_anchor(result);
        return result;
    }

    bool anchor_is_fresh(const LensControllerState& state) const {
        std::lock_guard<std::mutex> lock(mu_);
        if (!anchor_.valid) return false;
        return std::abs(state.zoom_pos - anchor_.zoom_pos) <=
                   follow_config_.follow_path_zoom_tolerance &&
               std::abs(state.focus_pos - anchor_.best_focus) <=
                   follow_config_.follow_path_anchor_focus_tolerance;
    }

    std::vector<hal_auto_af::FollowPathSample> build_path(
        const LensControllerState& state, float requested_ratio,
        float distance, float* effective_ratio) {
        int32_t target_zoom = state.zoom_pos;
        int32_t target_focus = state.focus_pos;
        if (lens_->calc_targets(requested_ratio, distance,
                                &target_zoom, &target_focus) != HAL_OK) return {};
        const float target_ratio = lens_->pos_to_ratio(target_zoom);
        if (effective_ratio) *effective_ratio = target_ratio;
        const float start_ratio = lens_->pos_to_ratio(state.zoom_pos);
        const int dense_steps = std::max(1, static_cast<int>(
            std::ceil(std::abs(target_ratio - start_ratio) / 0.01f)));
        std::vector<hal_auto_af::FollowPathSample> dense;
        dense.reserve(static_cast<size_t>(dense_steps + 1));
        dense.push_back({start_ratio, state.zoom_pos, state.focus_pos});
        for (int i = 1; i <= dense_steps; ++i) {
            const float t = static_cast<float>(i) / dense_steps;
            const float ratio = start_ratio + (target_ratio - start_ratio) * t;
            int32_t zoom = 0;
            int32_t focus = 0;
            if (lens_->calc_targets(ratio, distance, &zoom, &focus) != HAL_OK) return {};
            focus = std::clamp(focus + calibration_delta(ratio),
                               config_.min_focus_pos, config_.max_focus_pos);
            dense.push_back({lens_->pos_to_ratio(zoom), zoom, focus});
        }
        auto waypoints =
            hal_auto_af::select_follow_path_waypoints(dense, follow_config_);
        HAL_LOG_INFO("Autofocus: follow path ratio=%.3f->%.3f dense=%zu "
                     "waypoints=%zu curve_error=%s",
                     start_ratio, target_ratio, dense.size(), waypoints.size(),
                     follow_config_.follow_path_curve_error_enable != 0
                         ? "enabled" : "disabled");
        return waypoints;
    }

    ScanResult run_follow(float requested_ratio) {
        LensControllerState state{};
        if (lens_->state_get(&state) != HAL_OK) {
            ScanResult failed;
            failed.error = HAL_ERR_NOT_READY;
            failed.message = "lens state unavailable";
            return failed;
        }
        if (!anchor_is_fresh(state)) {
            {
                std::lock_guard<std::mutex> lock(mu_);
                anchor_.valid = false;
                status_.anchor_valid = false;
            }
            const ScanResult primed = run_one_shot_at(
                state.focus_pos, config_.coarse_span, false);
            if (!primed.confident) return primed;
            if (lens_->state_get(&state) != HAL_OK) return primed;
            if (!anchor_is_fresh(state)) {
                ScanResult failed;
                failed.error = HAL_ERR_NOT_READY;
                failed.message = "unable to establish follow anchor";
                return failed;
            }
        }

        float distance = 0.0f;
        {
            std::lock_guard<std::mutex> lock(mu_);
            /* AF0832 uses 0.0m as a valid infinity-distance table entry. */
            distance = anchor_.estimated_distance_m;
        }
        float effective_ratio = requested_ratio;
        auto path = build_path(state, requested_ratio, distance, &effective_ratio);
        if (path.empty()) {
            ScanResult failed;
            failed.error = HAL_ERR_RESULT;
            failed.message = "unable to generate zoom-focus path";
            return failed;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            status_.effective_ratio = effective_ratio;
        }

        set_state(AutofocusState::PathMoving, 0.05, "moving zoom-focus path");
        for (size_t i = 1; i < path.size(); ++i) {
            if (cancelled()) {
                ScanResult cancelled_result;
                cancelled_result.error = HAL_ERR_INVALID_STATE;
                cancelled_result.message = "cancelled";
                return cancelled_result;
            }
            const int ret = move_zoom_focus(
                path[i].zoom_pos, path[i].focus_pos, "follow segment");
            if (ret != HAL_OK) {
                invalidate_anchor("zoom-focus path movement failed");
                ScanResult failed;
                failed.error = ret;
                failed.message = "zoom-focus path movement failed";
                return failed;
            }
            const double progress = 0.05 + 0.78 *
                (static_cast<double>(i) / static_cast<double>(path.size() - 1));
            {
                std::lock_guard<std::mutex> lock(mu_);
                status_.state = AutofocusState::PathMoving;
                status_.progress = progress;
                status_.zoom_pos = path[i].zoom_pos;
                status_.focus_pos = path[i].focus_pos;
                status_.message = "moving zoom-focus path";
            }
        }

        set_state(AutofocusState::EndpointAf, 0.85, "endpoint autofocus");
        const auto& endpoint_sample = path.back();
        const int endpoint_span = hal_auto_af::refine_span_for_ratio(
            effective_ratio, config_.fine_span, follow_config_);
        return run_one_shot_at(endpoint_sample.focus_pos, endpoint_span, true);
    }

    void run_job(const PendingJob& job) {
        set_state(AutofocusState::Initializing, 0.01, "waiting for lens and video");
        if (!wait_lens_ready()) {
            finish_job(cancelled() ? AutofocusState::Cancelled : AutofocusState::Failed,
                       cancelled() ? HAL_ERR_INVALID_STATE : HAL_ERR_TIMEOUT,
                       cancelled() ? "cancelled" : "lens did not become ready");
            return;
        }
        if (!lens_->begin_autofocus_operation()) {
            finish_job(AutofocusState::Failed, HAL_ERR_INVALID_STATE,
                       "lens is busy");
            return;
        }
        struct OperationScope {
            LensController* lens;
            ~OperationScope() { lens->end_autofocus_operation(); }
        } scope{lens_};

        if (!wait_lens_ready()) {
            finish_job(cancelled() ? AutofocusState::Cancelled : AutofocusState::Failed,
                       cancelled() ? HAL_ERR_INVALID_STATE : HAL_ERR_TIMEOUT,
                       cancelled() ? "cancelled" : "lens did not become ready");
            return;
        }

        if (!configure_windows()) {
            finish_job(AutofocusState::Failed, HAL_ERR_NOT_READY,
                       "unable to configure AF windows");
            return;
        }
        ScanResult result;
        if (job.operation == AutofocusOperation::ZoomFollow) {
            result = run_follow(job.ratio);
        } else {
            LensControllerState state{};
            int startup_center = 0;
            if (job.operation == AutofocusOperation::Startup) {
                const int seed_ret = prepare_startup_position(&startup_center);
                if (seed_ret != HAL_OK) {
                    result.error = seed_ret;
                    result.message = "unable to position startup autofocus seed";
                } else if (wait_frames(config_.startup_wait_frames) != HAL_OK) {
                    result.error = HAL_ERR_TIMEOUT;
                    result.message = "video did not become ready after startup positioning";
                } else {
                    set_state(AutofocusState::StartupAf, 0.08, "startup autofocus");
                    result = run_one_shot_at(startup_center, config_.coarse_span,
                                             false, true);
                }
            } else if (lens_->state_get(&state) != HAL_OK) {
                result.error = HAL_ERR_NOT_READY;
                result.message = "lens state unavailable";
            } else {
                result = run_one_shot_at(state.focus_pos, config_.coarse_span, false);
            }
        }

        if (cancelled()) {
            lens_->stop_all(1000);
            finish_job(AutofocusState::Cancelled, HAL_ERR_INVALID_STATE, "cancelled");
        } else if (result.error == HAL_OK && result.confident) {
            finish_job(AutofocusState::Completed, HAL_OK, "autofocus completed");
        } else {
            {
                std::lock_guard<std::mutex> lock(mu_);
                if (result.valid) {
                    status_.best_focus = result.best_pos;
                    status_.focus_pos = result.best_pos;
                    status_.metric = result.metric;
                    status_.confidence = result.confidence;
                    status_.reproducibility = result.reproducibility;
                }
            }
            lens_->stop_all(1000);
            const int error = result.error == HAL_OK ? HAL_ERR_RESULT : result.error;
            finish_job(AutofocusState::Failed, error,
                       result.message.empty() ? "autofocus failed" : result.message);
        }
    }
};

AutofocusController::AutofocusController(
    HalIspOps* isp_ops, HalVideoOps* video_ops, void* video_ctx,
    FrameRouter* frame_router, LensController* lens,
    const AutofocusConfig& config,
    int sensor_native_width, int sensor_native_height)
    : impl_(std::make_unique<Impl>(isp_ops, video_ops, video_ctx,
                                   frame_router, lens, config,
                                   sensor_native_width, sensor_native_height)) {}

AutofocusController::~AutofocusController() = default;
void AutofocusController::start() { impl_->start(); }
void AutofocusController::stop() { impl_->stop(); }

bool AutofocusController::start_one_shot(uint64_t* job_id, std::string* error) {
    return impl_->enqueue(AutofocusOperation::OneShot, 1.0f, job_id, error);
}

bool AutofocusController::start_zoom_follow(float ratio, uint64_t* job_id,
                                             std::string* error) {
    if (!std::isfinite(ratio) || ratio <= 0.0f) {
        if (error) *error = "invalid zoom ratio";
        return false;
    }
    return impl_->enqueue(AutofocusOperation::ZoomFollow, ratio, job_id, error);
}

bool AutofocusController::cancel(uint64_t job_id, std::string* error) {
    return impl_->cancel(job_id, error);
}

void AutofocusController::invalidate_anchor(const std::string& reason) {
    impl_->invalidate_anchor(reason);
}

void AutofocusController::update_video_context(void* video_ctx) {
    impl_->update_video_context(video_ctx);
}

AutofocusStatus AutofocusController::status() const { return impl_->status(); }
