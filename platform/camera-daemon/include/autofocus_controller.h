/**
 * @file autofocus_controller.h
 * @brief Product autofocus state machine for camera-daemon.
 */

#pragma once

#include <cstdint>
#include <memory>
#include <string>

extern "C" {
#include "hal_isp.h"
#include "hal_video.h"
}

class FrameRouter;
class LensController;

enum class AutofocusOperation {
    None,
    Startup,
    OneShot,
    ZoomFollow,
};

enum class AutofocusState {
    Idle,
    Initializing,
    StartupAf,
    Coarse,
    Fine,
    Verifying,
    PathMoving,
    EndpointAf,
    Recovering,
    Completed,
    Failed,
    Cancelled,
};

struct AutofocusConfig {
    bool enabled = true;
    bool startup_af = true;
    std::string stream_name = "main";
    std::string calibration_path;
    float startup_zoom_ratio = 1.0f;
    float startup_focus_distance_m = 3.0f;
    int startup_wait_frames = 15;
    int startup_recovery_span = 320;
    int startup_ready_timeout_ms = 120000;
    int frame_wait_timeout_ms = 900;
    int move_timeout_ms = 10000;
    int pps = 1600;
    bool follow_sync_motion = true;
    bool follow_sync_fallback_sequential = true;
    int follow_sync_zoom_max_pps = 1600;
    int follow_sync_focus_max_pps = 1600;
    int follow_sync_min_pps = 1600;
    int follow_sync_zoom_tolerance = 4;
    int follow_sync_focus_tolerance = 4;
    int follow_path_zoom_step_wide = 240;
    int follow_path_zoom_step_mid = 160;
    int follow_path_zoom_step_tele = 160;
    int follow_path_focus_step_wide = 24;
    int follow_path_focus_step_mid = 50;
    int follow_path_focus_step_tele = 50;
    bool follow_curve_error_enable = true;
    int follow_curve_error_wide = 16;
    int follow_curve_error_mid = 8;
    int follow_curve_error_tele = 8;
    int min_focus_pos = -844;
    int max_focus_pos = 592;
    int coarse_step = 40;
    int coarse_span = 160;
    int fine_step = 8;
    int fine_span = 24;
    int max_moves = 200;
    int balanced_retry = 1;
    double confidence_accept = 0.80;
    double confidence_recovery = 0.65;

    // Sensor native resolution.  Set to non-zero to force AF windows to use
    // full sensor coordinates regardless of encoder output resolution changes.
    // When 0, the resolution is read from the initial video context at startup
    // (which may reflect a persisted encoder override rather than the true
    // sensor native resolution).
    int sensor_native_width = 0;
    int sensor_native_height = 0;
};

struct AutofocusAnchor {
    bool valid = false;
    float zoom_ratio = 1.0f;
    int32_t zoom_pos = 0;
    int32_t best_focus = 0;
    float estimated_distance_m = 0.0f;
    double metric = 0.0;
    double confidence = 0.0;
    uint64_t timestamp_ms = 0;
};

struct AutofocusStatus {
    uint64_t job_id = 0;
    AutofocusOperation operation = AutofocusOperation::None;
    AutofocusState state = AutofocusState::Idle;
    double progress = 0.0;
    bool busy = false;
    bool anchor_valid = false;
    float requested_ratio = 1.0f;
    float effective_ratio = 1.0f;
    int32_t zoom_pos = 0;
    int32_t focus_pos = 0;
    int32_t best_focus = 0;
    double metric = 0.0;
    double confidence = 0.0;
    double reproducibility = 0.0;
    float estimated_distance_m = 0.0f;
    uint64_t elapsed_ms = 0;
    int error_code = 0;
    std::string message;
};

class AutofocusController {
public:
    AutofocusController(HalIspOps* isp_ops, HalVideoOps* video_ops,
                        void* video_ctx, FrameRouter* frame_router,
                        LensController* lens, const AutofocusConfig& config,
                        int sensor_native_width = 0, int sensor_native_height = 0);
    ~AutofocusController();

    AutofocusController(const AutofocusController&) = delete;
    AutofocusController& operator=(const AutofocusController&) = delete;

    void start();
    void stop();
    bool start_one_shot(uint64_t* job_id, std::string* error);
    bool start_zoom_follow(float ratio, uint64_t* job_id, std::string* error);
    bool cancel(uint64_t job_id, std::string* error);
    void invalidate_anchor(const std::string& reason);
    void update_video_context(void* video_ctx);
    AutofocusStatus status() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

const char* autofocus_operation_name(AutofocusOperation operation);
const char* autofocus_state_name(AutofocusState state);
