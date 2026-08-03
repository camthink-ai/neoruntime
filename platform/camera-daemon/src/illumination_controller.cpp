#include "../include/illumination_controller.h"

extern "C" {
#include "hal_log.h"
}

#include <algorithm>
#include <utility>

namespace {

int clamp_pwm(int pwm) {
    return std::clamp(pwm, 0, 100);
}

} // namespace

const char* imaging_mode_name(ImagingMode mode) {
    return mode == ImagingMode::Infrared ? "infrared" : "day";
}

const char* infrared_output_source_name(InfraredOutputSource source) {
    switch (source) {
    case InfraredOutputSource::Automatic: return "automatic";
    case InfraredOutputSource::Manual: return "manual";
    case InfraredOutputSource::ZoomFollow: return "zoom_follow";
    case InfraredOutputSource::Off:
    default: return "off";
    }
}

IlluminationController::IlluminationController(const IlluminationConfig& config,
                                               SetDutyFn set_duty)
    : config_(config), set_duty_(std::move(set_duty)) {
    state_.config.auto_follow = config_.auto_follow;
    state_.config.deadband = std::clamp(config_.deadband_percent, 0, 100);
    state_.config.settle_frames = std::max(config_.endpoint_settle_frames, 0);
    state_.config.log_enabled = config_.log_updates;
    state_.lut = hal_auto_af::default_ir_zoom_lut();
    status_.enabled = config_.enabled;
    status_.auto_follow = config_.auto_follow;
}

bool IlluminationController::initialize(std::string* warning) {
    std::vector<hal_auto_af::IrLutPoint> loaded;
    std::string load_error;
    bool monotonic_warning = false;
    if (!config_.lut_path.empty() &&
        hal_auto_af::load_ir_zoom_lut_csv(config_.lut_path, &loaded, &load_error,
                                          &monotonic_warning)) {
        std::lock_guard<std::mutex> lock(mu_);
        state_.lut = std::move(loaded);
        state_.lut_source = config_.lut_path;
    } else if (!config_.lut_path.empty()) {
        if (warning) *warning = load_error + "; using built-in infrared LUT";
        HAL_LOG_WARNING("Illumination: cannot load LUT '%s': %s; using built-in LUT",
                        config_.lut_path.c_str(), load_error.c_str());
    }
    if (monotonic_warning) {
        HAL_LOG_WARNING("Illumination: infrared LUT is non-monotonic (allowed)");
    }
    return true;
}

IlluminationController::OutputRequest
IlluminationController::desired_locked(double zoom_ratio) const {
    OutputRequest request;
    request.zoom_ratio = zoom_ratio;
    if (!config_.enabled || status_.mode != ImagingMode::Infrared) {
        request.source = InfraredOutputSource::Off;
        return request;
    }
    request.pwm = hal_auto_af::desired_ir_pwm(state_, zoom_ratio);
    if (state_.follow_active && state_.config.auto_follow) {
        request.source = InfraredOutputSource::ZoomFollow;
    } else if (state_.manual_valid) {
        request.source = InfraredOutputSource::Manual;
    } else if (state_.config.auto_follow) {
        request.source = InfraredOutputSource::Automatic;
    } else {
        request.source = InfraredOutputSource::Off;
    }
    return request;
}

bool IlluminationController::apply_output(const OutputRequest& request, bool force,
                                          std::string* error) {
    hal_auto_af::IrPwm current;
    bool current_valid = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        current = state_.applied;
        current_valid = state_.applied_valid;
        status_.requested_near_pwm = request.pwm.near_pwm;
        status_.requested_far_pwm = request.pwm.far_pwm;
        status_.zoom_ratio = request.zoom_ratio;
        status_.source = request.source;
    }

    const bool near_changed = force || !current_valid ||
        hal_auto_af::ir_pwm_channel_needs_update(
            current.near_pwm, request.pwm.near_pwm, state_.config.deadband);
    const bool far_changed = force || !current_valid ||
        hal_auto_af::ir_pwm_channel_needs_update(
            current.far_pwm, request.pwm.far_pwm, state_.config.deadband);

    bool near_ok = true;
    bool far_ok = true;
    if (near_changed) {
        near_ok = set_duty_ && set_duty_(config_.near_led_id,
                                        static_cast<uint32_t>(request.pwm.near_pwm));
    }
    if (far_changed) {
        far_ok = set_duty_ && set_duty_(config_.far_led_id,
                                       static_cast<uint32_t>(request.pwm.far_pwm));
    }

    const bool ok = near_ok && far_ok;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (near_ok && near_changed) state_.applied.near_pwm = request.pwm.near_pwm;
        if (far_ok && far_changed) state_.applied.far_pwm = request.pwm.far_pwm;
        state_.applied_valid = current_valid || near_changed || far_changed;
        state_.degraded = !ok;
        status_.degraded = !ok;
        status_.applied_near_pwm = state_.applied.near_pwm;
        status_.applied_far_pwm = state_.applied.far_pwm;
        status_.error = ok ? std::string{} : "infrared PWM update failed";
    }
    if (!ok && error) *error = "infrared PWM update failed";
    if (config_.log_updates && (near_changed || far_changed)) {
        HAL_LOG_INFO("Illumination: source=%s ratio=%.3f pwm=[near:%d far:%d] ok=%d",
                     infrared_output_source_name(request.source), request.zoom_ratio,
                     request.pwm.near_pwm, request.pwm.far_pwm, ok ? 1 : 0);
    }
    return ok;
}

bool IlluminationController::apply_desired(double zoom_ratio, bool force,
                                           std::string* error) {
    OutputRequest request;
    {
        std::lock_guard<std::mutex> lock(mu_);
        request = desired_locked(zoom_ratio);
    }
    return apply_output(request, force, error);
}

bool IlluminationController::set_mode(ImagingMode mode, double zoom_ratio,
                                      std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        status_.mode = mode;
        state_.night_mode = mode == ImagingMode::Infrared;
        if (mode == ImagingMode::Day) state_.follow_active = false;
        status_.follow_active = state_.follow_active;
    }
    return apply_desired(zoom_ratio, true, error);
}

void IlluminationController::set_transition(ImagingModeTransition transition,
                                            const std::string& error) {
    std::lock_guard<std::mutex> lock(mu_);
    status_.transition = transition;
    status_.error = error;
}

void IlluminationController::set_active_profile(const std::string& profile) {
    std::lock_guard<std::mutex> lock(mu_);
    status_.active_profile = profile;
}

bool IlluminationController::set_auto_follow(bool enabled, double zoom_ratio,
                                             std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        state_.config.auto_follow = enabled;
        status_.auto_follow = enabled;
    }
    return apply_desired(zoom_ratio, false, error);
}

bool IlluminationController::set_manual_pwm(int near_pwm, int far_pwm,
                                            double zoom_ratio, std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        hal_auto_af::set_ir_manual(&state_, {clamp_pwm(near_pwm), clamp_pwm(far_pwm)});
        status_.manual_override = true;
    }
    return apply_desired(zoom_ratio, false, error);
}

bool IlluminationController::clear_manual(double zoom_ratio, std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        hal_auto_af::clear_ir_manual(&state_);
        status_.manual_override = false;
    }
    return apply_desired(zoom_ratio, false, error);
}

bool IlluminationController::begin_zoom_follow(double zoom_ratio, std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        hal_auto_af::begin_ir_follow(&state_);
        status_.follow_active = state_.follow_active;
    }
    return apply_desired(zoom_ratio, false, error);
}

bool IlluminationController::apply_follow_ratio(double zoom_ratio, std::string* error) {
    return apply_desired(zoom_ratio, false, error);
}

bool IlluminationController::apply_endpoint_ratio(double zoom_ratio, std::string* error) {
    return apply_desired(zoom_ratio, true, error);
}

bool IlluminationController::end_zoom_follow(double zoom_ratio, std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        hal_auto_af::end_ir_follow(&state_);
        status_.follow_active = state_.follow_active;
    }
    return apply_desired(zoom_ratio, false, error);
}

IlluminationStatus IlluminationController::status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
}
