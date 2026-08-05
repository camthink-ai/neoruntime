/**
 * @file illumination_controller.h
 * @brief Product infrared illumination policy and output arbitration.
 */

#pragma once

#include "af_ir_follow.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

enum class ImagingMode {
    Day,
    Infrared,
};

enum class ImagingModeTransition {
    Idle,
    Switching,
    Failed,
};

enum class InfraredOutputSource {
    Off,
    Automatic,
    Manual,
    ZoomFollow,
};

struct IlluminationConfig {
    bool enabled = true;
    std::string infrared_profile = "Infrared_Basic";
    std::string default_mode = "day";
    uint32_t near_led_id = 0;
    uint32_t far_led_id = 1;
    bool auto_follow = true;
    std::string lut_path = "/data/aipc/etc/ir_zoom_lut.csv";
    int deadband_percent = 2;
    int endpoint_settle_frames = 3;
    int mode_settle_frames = 10;
    bool log_updates = true;
};

struct IlluminationStatus {
    ImagingMode mode = ImagingMode::Day;
    ImagingModeTransition transition = ImagingModeTransition::Idle;
    InfraredOutputSource source = InfraredOutputSource::Off;
    bool enabled = false;
    bool auto_follow = true;
    bool follow_active = false;
    bool manual_override = false;
    bool degraded = false;
    int requested_near_pwm = 0;
    int requested_far_pwm = 0;
    int applied_near_pwm = 0;
    int applied_far_pwm = 0;
    double zoom_ratio = 1.0;
    std::string active_profile;
    std::string error;

    // Day/night auto (light-sensor) policy — filled by CameraDaemon.
    std::string selected_mode = "day";   // operator selection: auto | day | infrared
    int light_percent = 0;               // 0..100, endpoint-normalized
    uint16_t light_mv = 0;               // raw photodiode milli-volts
    int32_t light_milli = 0;             // raw scaled value
    bool light_valid = false;
    int night_enter = 28;
    int day_enter = 82;
};

class IlluminationController {
public:
    using SetDutyFn = std::function<bool(uint32_t, uint32_t)>;

    IlluminationController(const IlluminationConfig& config, SetDutyFn set_duty);

    bool initialize(std::string* warning = nullptr);
    bool set_mode(ImagingMode mode, double zoom_ratio, std::string* error = nullptr);
    void set_transition(ImagingModeTransition transition, const std::string& error = {});
    void set_active_profile(const std::string& profile);

    bool set_auto_follow(bool enabled, double zoom_ratio, std::string* error = nullptr);
    bool set_manual_pwm(int near_pwm, int far_pwm, double zoom_ratio,
                        std::string* error = nullptr);
    bool clear_manual(double zoom_ratio, std::string* error = nullptr);

    bool begin_zoom_follow(double zoom_ratio, std::string* error = nullptr);
    bool apply_follow_ratio(double zoom_ratio, std::string* error = nullptr);
    bool apply_endpoint_ratio(double zoom_ratio, std::string* error = nullptr);
    bool end_zoom_follow(double zoom_ratio, std::string* error = nullptr);

    IlluminationStatus status() const;
    const IlluminationConfig& config() const { return config_; }

private:
    struct OutputRequest {
        hal_auto_af::IrPwm pwm{};
        InfraredOutputSource source = InfraredOutputSource::Off;
        double zoom_ratio = 1.0;
    };

    OutputRequest desired_locked(double zoom_ratio) const;
    bool apply_desired(double zoom_ratio, bool force, std::string* error);
    bool apply_output(const OutputRequest& request, bool force, std::string* error);

    IlluminationConfig config_;
    SetDutyFn set_duty_;
    mutable std::mutex mu_;
    hal_auto_af::IrFollowState state_;
    IlluminationStatus status_;
};

const char* imaging_mode_name(ImagingMode mode);
const char* infrared_output_source_name(InfraredOutputSource source);
