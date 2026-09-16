#include "camera_control_service.h"
#include "camera_daemon.h"
#include "hal_loader.h"
#include "dsp_service.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

extern "C" {
    #include "hal_log.h"
    #include "peripheral/devices/hal_env_ctrl.h"
    #include "peripheral/devices/hal_alarm.h"
    #include "peripheral/devices/hal_rs485.h"
}

namespace {

void fill_infrared_status(const IlluminationStatus& status, bool success,
                          const std::string& message,
                          aipc::camera::InfraredStatusResponse* response) {
    response->set_success(success);
    response->set_message(message.empty() ? status.error : message);
    response->set_mode(imaging_mode_name(status.mode));
    const char* transition = status.transition == ImagingModeTransition::Switching
        ? "switching" : (status.transition == ImagingModeTransition::Failed ? "failed" : "idle");
    response->set_transition(transition);
    response->set_output_source(infrared_output_source_name(status.source));
    response->set_auto_follow(status.auto_follow);
    response->set_follow_active(status.follow_active);
    response->set_manual_override(status.manual_override);
    response->set_degraded(status.degraded);
    response->set_requested_near_pwm(status.requested_near_pwm);
    response->set_requested_far_pwm(status.requested_far_pwm);
    response->set_applied_near_pwm(status.applied_near_pwm);
    response->set_applied_far_pwm(status.applied_far_pwm);
    response->set_zoom_ratio(static_cast<float>(status.zoom_ratio));
    response->set_active_profile(status.active_profile);
    // Day/night auto (light-sensor) policy
    response->set_selected_mode(status.selected_mode);
    response->set_light_percent(status.light_percent);
    response->set_light_mv(status.light_mv);
    response->set_light_milli(status.light_milli);
    response->set_light_valid(status.light_valid);
    response->set_night_enter(status.night_enter);
    response->set_day_enter(status.day_enter);
}

} // namespace

CameraControlServiceImpl::CameraControlServiceImpl(CameraDaemon* daemon)
    : daemon_(daemon) {
}
/* ~CameraControlServiceImpl is defined at the end of this file:
 * JpegShotState (held by unique_ptr) is incomplete until then. */

grpc::Status CameraControlServiceImpl::StartOneShotAutofocus(
    grpc::ServerContext*, const aipc::camera::Empty*,
    aipc::camera::AutofocusJobResponse* response) {
    uint64_t job_id = 0;
    std::string error;
    const bool accepted = daemon_ && daemon_->start_autofocus_one_shot(&job_id, &error);
    response->set_accepted(accepted);
    response->set_job_id(job_id);
    response->set_message(accepted ? "queued" : error);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::StartZoomFollow(
    grpc::ServerContext*, const aipc::camera::AutofocusZoomFollowRequest* request,
    aipc::camera::AutofocusJobResponse* response) {
    uint64_t job_id = 0;
    std::string error;
    const bool accepted = daemon_ &&
        daemon_->start_autofocus_zoom_follow(request->ratio(), &job_id, &error);
    response->set_accepted(accepted);
    response->set_job_id(job_id);
    response->set_message(accepted ? "queued" : error);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetAutofocusStatus(
    grpc::ServerContext*, const aipc::camera::Empty*,
    aipc::camera::AutofocusStatusResponse* response) {
    if (!daemon_) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "daemon unavailable");
    const AutofocusStatus status = daemon_->get_autofocus_status();
    response->set_job_id(status.job_id);
    response->set_operation(autofocus_operation_name(status.operation));
    response->set_state(autofocus_state_name(status.state));
    response->set_progress(status.progress);
    response->set_busy(status.busy);
    response->set_anchor_valid(status.anchor_valid);
    response->set_requested_ratio(status.requested_ratio);
    response->set_effective_ratio(status.effective_ratio);
    response->set_zoom_pos(status.zoom_pos);
    response->set_focus_pos(status.focus_pos);
    response->set_best_focus(status.best_focus);
    response->set_metric(status.metric);
    response->set_confidence(status.confidence);
    response->set_reproducibility(status.reproducibility);
    response->set_estimated_distance_m(status.estimated_distance_m);
    response->set_elapsed_ms(status.elapsed_ms);
    response->set_error_code(status.error_code);
    response->set_message(status.message);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::CancelAutofocus(
    grpc::ServerContext*, const aipc::camera::AutofocusJobRequest* request,
    aipc::camera::Status* response) {
    std::string error;
    const bool success = daemon_ && daemon_->cancel_autofocus(request->job_id(), &error);
    response->set_success(success);
    response->set_message(success ? "cancellation requested" : error);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::InvalidateAutofocusAnchor(
    grpc::ServerContext*, const aipc::camera::AutofocusInvalidateRequest* request,
    aipc::camera::Status* response) {
    if (!daemon_) return grpc::Status(grpc::StatusCode::UNAVAILABLE, "daemon unavailable");
    daemon_->invalidate_autofocus_anchor(
        request->reason().empty() ? "external lens movement" : request->reason());
    response->set_success(true);
    response->set_message("anchor invalidated");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::UpdateISPSettings(
    grpc::ServerContext* context,
    const aipc::camera::ISPUpdateRequest* request,
    aipc::camera::ISPUpdateResponse* response) {

    if (!daemon_) {
        response->mutable_status()->set_success(false);
        response->mutable_status()->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] Update ISP: manual=%d B=%d C=%d S=%d Sh=%d AE=%d",
                 request->manual_mode(), request->brightness(), request->contrast(),
                 request->saturation(), request->sharpness(), request->auto_exposure());

    bool success = daemon_->update_isp_settings(*request);

    response->mutable_status()->set_success(success);
    if (!success) {
        response->mutable_status()->set_message("Failed to configure ISP via HAL");
    } else {
        response->mutable_status()->set_message("Success");
    }

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetISPConfig(
    grpc::ServerContext* context,
    const aipc::camera::Empty* request,
    aipc::camera::ISPConfigResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    daemon_->get_isp_config(*response);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetTransformConfig(
    grpc::ServerContext* context,
    const aipc::camera::Empty* request,
    aipc::camera::TransformConfig* response) {

    if (!daemon_) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    if (!daemon_->get_transform_config(*response)) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to get transform config");
    }

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetTransformConfig(
    grpc::ServerContext* context,
    const aipc::camera::TransformConfig* request,
    aipc::camera::Status* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] SetTransform: rot=%u flip=%u dewarp=%d gray=%d",
                 request->rotation(), request->flip(), request->dewarp(), request->grayscale());

    bool success = daemon_->set_transform_config(*request);
    response->set_success(success);
    response->set_message(success ? "Success" : "Failed to apply transform config");

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::UpdateEncoderConfig(
    grpc::ServerContext* context,
    const aipc::camera::EncoderConfigRequest* request,
    aipc::camera::Status* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    const std::string& stream_name = request->stream_name();
    HAL_LOG_INFO("[CameraControl] Update Encoder for stream '%s': bitrate=%u fps=%u gop=%u",
                 stream_name.c_str(),
                 request->bitrate_bps(),
                 request->framerate(),
                 request->gop());

    bool success = daemon_->update_encoder_config(
        stream_name,
        request->bitrate_bps(),
        request->framerate(),
        request->gop()
    );

    response->set_success(success);
    response->set_message(success ? "Encoder config updated" : "Failed to update encoder config");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetRtspEnabled(
    grpc::ServerContext* context,
    const aipc::camera::RtspEnabledRequest* request,
    aipc::camera::Status* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] Set RTSP enabled: %s",
                 request->enabled() ? "true" : "false");

    bool success = daemon_->set_rtsp_enabled(request->enabled());

    response->set_success(success);
    response->set_message(success ? "RTSP state updated" : "Failed to update RTSP state");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::UpdateAiOverlay(
    grpc::ServerContext* context,
    const aipc::camera::AiOverlayConfig* request,
    aipc::camera::Status* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] Update AI Overlay: enabled=%s labels=%s confidence=%s thickness=%u",
                 request->enabled() ? "true" : "false",
                 request->show_label() ? "true" : "false",
                 request->show_confidence() ? "true" : "false",
                 request->line_thickness());

    bool success = daemon_->update_ai_overlay_config(
        request->enabled(),
        request->show_label(),
        request->show_confidence(),
        request->line_thickness()
    );

    response->set_success(success);
    response->set_message(success ? "AI overlay config updated" : "Failed to update AI overlay config");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::UpdateOsdConfig(
    grpc::ServerContext* context,
    const aipc::camera::OsdConfigRequest* request,
    aipc::camera::Status* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] Update OSD Config: %d streams", request->streams_size());

    bool success = daemon_->update_osd_config(*request);

    response->set_success(success);
    response->set_message(success ? "OSD config updated" : "Failed to update OSD config");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetOsdConfig(
    grpc::ServerContext* context,
    const aipc::camera::Empty* request,
    aipc::camera::OsdConfigResponse* response) {

    if (!daemon_) {
        response->Clear();
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] GetOsdConfig");

    bool success = daemon_->get_osd_config(*response);

    if (!success) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to get OSD config");
    }

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::ReconfigureEncoder(
    grpc::ServerContext* context,
    const aipc::camera::EncoderReconfigRequest* request,
    aipc::camera::EncoderReconfigResponse* response) {
    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] ReconfigureEncoder: stream=%s, width=%u, height=%u, codec=%s, bitrate=%u, fps=%u, gop=%u",
        request->stream_name().c_str(),
        request->width(), request->height(),
        request->codec().c_str(),
        request->bitrate_bps(), request->fps(),
        request->gop());

    // Delegate to daemon's reconfigure_encoder method
    bool success = daemon_->reconfigure_encoder(*request, *response);

    HAL_LOG_INFO("[CameraControl] ReconfigureEncoder result: success=%d, interrupt_ms=%u",
        success, response->interrupt_ms());

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetProfile(
    grpc::ServerContext* context,
    const aipc::camera::Empty* request,
    aipc::camera::GetProfileResponse* response) {

    if (!daemon_) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    std::string profile = daemon_->get_current_profile();
    response->set_profile_name(profile);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::ListProfiles(
    grpc::ServerContext* context,
    const aipc::camera::Empty* request,
    aipc::camera::ListProfilesResponse* response) {

    if (!daemon_) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto profiles = daemon_->list_profiles();
    for (const auto& p : profiles) {
        response->add_profiles(p);
    }
    response->set_current_profile(daemon_->get_current_profile());
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SwitchProfile(
    grpc::ServerContext* context,
    const aipc::camera::SwitchProfileRequest* request,
    aipc::camera::EncoderReconfigResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    const std::string& name = request->profile_name();
    if (name.empty()) {
        response->set_success(false);
        response->set_message("profile_name is required");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "profile_name is required");
    }

    // Allowlist: the three AI ISP Basic profiles are exposed for UI switching,
    // plus Daylight_Basic as the explicit "AI ISP off" target the UI toggles to
    // when the user disables AI ISP. Other profiles (HDR/detection variants)
    // churn the GStreamer pipeline for no user-facing benefit, so they are
    // intentionally locked down here.
    static const std::unordered_set<std::string> kAllowedProfiles = {
        "AI_ISP_Gen1_Basic",
        "AI_ISP_Gen2_Basic",
        "AI_ISP_Gen3_Basic",
        "Daylight_Basic",
    };
    if (!kAllowedProfiles.count(name)) {
        response->set_success(false);
        response->set_message("profile '" + name + "' is not switchable from the UI (allowlist: AI_ISP_Gen{1,2,3}_Basic, Daylight_Basic)");
        return grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                            "profile not in switch allowlist");
    }

    // Reject unknown profiles before touching the pipeline.
    auto profiles = daemon_->list_profiles();
    if (std::find(profiles.begin(), profiles.end(), name) == profiles.end()) {
        response->set_success(false);
        response->set_message("profile '" + name + "' not found on device");
        return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "unknown profile");
    }

    HAL_LOG_INFO("[CameraControl] SwitchProfile: '%s' (current: '%s')",
                 name.c_str(), daemon_->get_current_profile().c_str());

    // switch_profile is synchronous: it stops consumers, switches, and re-discovers
    // stream config before returning. The wall-clock duration IS the pipeline
    // interruption window the player must wait out before reconnecting.
    std::string message;
    auto t0 = std::chrono::steady_clock::now();
    bool ok = daemon_->switch_profile(name, &message);
    auto t1 = std::chrono::steady_clock::now();
    uint32_t interrupt_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

    response->set_success(ok);
    response->set_message(message.empty() ? (ok ? "ok" : "switch failed") : message);
    response->set_interrupt_ms(interrupt_ms);

    HAL_LOG_INFO("[CameraControl] SwitchProfile result: success=%d, interrupt_ms=%u, msg='%s'",
                 ok, response->interrupt_ms(), response->message().c_str());

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::BackupProfile(
    grpc::ServerContext* context,
    const aipc::camera::BackupProfileRequest* request,
    aipc::camera::BackupProfileResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] BackupProfile: path='%s'", request->path().c_str());
    bool ok = daemon_->backup_profile(request->path());
    response->set_success(ok);
    response->set_message(ok ? "Profile backed up" : "Backup failed");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::ReconfigurePipeline(
    grpc::ServerContext* context,
    const aipc::camera::ReconfigurePipelineRequest* request,
    aipc::camera::ReconfigurePipelineResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] ReconfigurePipeline: %d streams", request->streams_size());

    bool success = daemon_->reconfigure_pipeline(*request, *response);

    HAL_LOG_INFO("[CameraControl] ReconfigurePipeline result: success=%d, interrupt_ms=%u",
        success, response->interrupt_ms());

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetStreamStatus(
    grpc::ServerContext* context,
    const aipc::camera::GetStreamStatusRequest* request,
    aipc::camera::GetStreamStatusResponse* response) {

    if (!daemon_) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    daemon_->get_stream_status(*response);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetSensorInfo(
    grpc::ServerContext* context,
    const aipc::camera::GetSensorInfoRequest* request,
    aipc::camera::SensorInfoResponse* response) {

    if (!daemon_) {
        response->set_available(false);
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HalVideoSensorModuleInfo info = {};
    bool success = daemon_->get_sensor_info(request->sensor_index(), &info);

    response->set_available(success);
    if (success) {
        response->set_sensor_model(info.sensor_model_name);
        response->set_i2c_bus(info.i2c_bus);
        response->set_i2c_address(info.i2c_address);
        response->set_pixel_format(info.sensor_pixel_format);
    }

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::AddStream(
    grpc::ServerContext* context,
    const aipc::camera::AddStreamRequest* request,
    aipc::camera::StreamOperationResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] AddStream: id=%s %ux%u %s fps=%u bitrate=%u gop=%u",
                 request->stream_id().c_str(),
                 request->width(), request->height(), request->codec().c_str(),
                 request->fps(), request->bitrate(), request->gop());

    daemon_->add_stream(*request, *response);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::RemoveStream(
    grpc::ServerContext* context,
    const aipc::camera::RemoveStreamRequest* request,
    aipc::camera::StreamOperationResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] RemoveStream: name=%s", request->stream_name().c_str());

    daemon_->remove_stream(request->stream_name(), *response);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetIrCut(
    grpc::ServerContext* context,
    const aipc::camera::SetIrCutRequest* request,
    aipc::camera::SetIrCutResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] SetIrCut: mode=%u", request->mode());

    bool ok = daemon_->set_ircut(request->mode());
    response->set_success(ok);
    if (!ok) {
        response->set_message("Failed to set IR-cut mode");
    }

    uint32_t cur = 0;
    if (daemon_->get_ircut(cur)) {
        response->set_current_mode(cur);
    }

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetIrCut(
    grpc::ServerContext* context,
    const aipc::camera::Empty* request,
    aipc::camera::SetIrCutResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    uint32_t mode = 0;
    bool ok = daemon_->get_ircut(mode);
    response->set_success(ok);
    if (ok) {
        response->set_current_mode(mode);
    } else {
        response->set_message("Failed to read IR-cut mode");
    }

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetLedDuty(
    grpc::ServerContext* context,
    const aipc::camera::SetLedDutyRequest* request,
    aipc::camera::LedStatus* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] SetLedDuty: led_id=%u duty=%u",
                 request->led_id(), request->duty_percent());

    bool ok = daemon_->set_led_duty(request->led_id(), request->duty_percent());
    response->set_success(ok);
    response->set_message(ok ? "OK" : "Failed to set LED duty");

    uint32_t duty = 0;
    if (daemon_->get_led_duty(request->led_id(), duty)) {
        response->set_duty_percent(duty);
    }

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetLedDuty(
    grpc::ServerContext* context,
    const aipc::camera::GetLedDutyRequest* request,
    aipc::camera::LedStatus* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    uint32_t duty = 0;
    bool ok = daemon_->get_led_duty(request->led_id(), duty);
    response->set_success(ok);
    response->set_message(ok ? "OK" : "Failed to read LED duty");
    if (ok) {
        response->set_duty_percent(duty);
    }

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetImagingMode(
    grpc::ServerContext*, const aipc::camera::ImagingModeRequest* request,
    aipc::camera::InfraredStatusResponse* response) {
    std::string error;
    const std::string& mode = request->mode();
    const bool valid = mode == "day" || mode == "infrared" || mode == "auto";
    const bool ok = daemon_ && valid && daemon_->set_selected_mode(mode, &error);
    fill_infrared_status(daemon_ ? daemon_->get_illumination_status() : IlluminationStatus{},
                         ok, valid ? error : "invalid imaging mode", response);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetInfraredStatus(
    grpc::ServerContext*, const aipc::camera::Empty*,
    aipc::camera::InfraredStatusResponse* response) {
    const bool ok = daemon_ != nullptr;
    fill_infrared_status(ok ? daemon_->get_illumination_status() : IlluminationStatus{},
                         ok, ok ? "" : "daemon unavailable", response);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetInfraredSettings(
    grpc::ServerContext*, const aipc::camera::InfraredSettingsRequest* request,
    aipc::camera::InfraredStatusResponse* response) {
    std::string error;
    bool ok = daemon_ != nullptr;
    if (ok && request->has_auto_follow())
        ok = daemon_->set_infrared_auto_follow(request->auto_follow(), &error);
    if (ok && (request->has_near_pwm() || request->has_far_pwm())) {
        const auto current = daemon_->get_illumination_status();
        const uint32_t near_pwm = request->has_near_pwm()
            ? request->near_pwm() : static_cast<uint32_t>(current.requested_near_pwm);
        const uint32_t far_pwm = request->has_far_pwm()
            ? request->far_pwm() : static_cast<uint32_t>(current.requested_far_pwm);
        ok = daemon_->set_infrared_manual(near_pwm, far_pwm, &error);
    }
    if (ok && (request->has_night_enter() || request->has_day_enter())) {
        const auto current = daemon_->get_illumination_status();
        const int night_enter = request->has_night_enter()
            ? request->night_enter() : current.night_enter;
        const int day_enter = request->has_day_enter()
            ? request->day_enter() : current.day_enter;
        ok = daemon_->set_light_thresholds(night_enter, day_enter, &error);
    }
    fill_infrared_status(daemon_ ? daemon_->get_illumination_status() : IlluminationStatus{},
                         ok, error, response);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::ClearInfraredManual(
    grpc::ServerContext*, const aipc::camera::Empty*,
    aipc::camera::InfraredStatusResponse* response) {
    std::string error;
    const bool ok = daemon_ && daemon_->clear_infrared_manual(&error);
    fill_infrared_status(daemon_ ? daemon_->get_illumination_status() : IlluminationStatus{},
                         ok, error, response);
    return grpc::Status::OK;
}

namespace {
void fill_ir_preset_list(const std::vector<IrPresetEntry>& presets, bool success,
                         const std::string& message,
                         aipc::camera::IrPresetListResponse* response) {
    response->set_success(success);
    response->set_message(message);
    response->clear_presets();
    for (const auto& p : presets) {
        auto* out = response->add_presets();
        out->set_name(p.name);
        out->set_zoom_ratio(p.zoom_ratio);
        out->set_near_pwm(p.near_pwm);
        out->set_far_pwm(p.far_pwm);
    }
}
} // namespace

grpc::Status CameraControlServiceImpl::ListIrPresets(
    grpc::ServerContext*, const aipc::camera::Empty*,
    aipc::camera::IrPresetListResponse* response) {
    std::string error;
    const auto presets = daemon_ ? daemon_->list_ir_presets(&error) : std::vector<IrPresetEntry>{};
    fill_ir_preset_list(presets, daemon_ != nullptr && error.empty(), error, response);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SaveIrPreset(
    grpc::ServerContext*, const aipc::camera::IrPreset* request,
    aipc::camera::IrPresetListResponse* response) {
    std::string error;
    IrPresetEntry entry;
    entry.name = request->name();
    entry.zoom_ratio = request->zoom_ratio();
    entry.near_pwm = request->near_pwm();
    entry.far_pwm = request->far_pwm();
    const bool ok = daemon_ && daemon_->save_ir_preset(entry, &error);
    fill_ir_preset_list(ok ? daemon_->list_ir_presets() : std::vector<IrPresetEntry>{},
                        ok, error, response);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::DeleteIrPreset(
    grpc::ServerContext*, const aipc::camera::DeleteIrPresetRequest* request,
    aipc::camera::IrPresetListResponse* response) {
    std::string error;
    const bool ok = daemon_ && daemon_->delete_ir_preset(request->name(), &error);
    fill_ir_preset_list(ok ? daemon_->list_ir_presets() : std::vector<IrPresetEntry>{},
                        ok, error, response);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetDeviceHardwareStatus(
    grpc::ServerContext* context,
    const aipc::camera::Empty* request,
    aipc::camera::DeviceHardwareStatus* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    daemon_->get_device_hardware_status(*response);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::McuRawRequest(
    grpc::ServerContext* context,
    const aipc::camera::McuRawRequestMessage* request,
    aipc::camera::McuRawResponseMessage* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    const auto& req_payload = request->payload();
    uint8_t resp_buf[512] = {};
    uint16_t resp_len = 0;

    HAL_LOG_INFO("[CameraControl] McuRawRequest: cmd=0x%04x payload_len=%u",
                 request->cmd(), static_cast<uint16_t>(req_payload.size()));

    int ret = daemon_->mcu_raw_request(
        static_cast<uint16_t>(request->cmd()),
        reinterpret_cast<const uint8_t*>(req_payload.data()),
        static_cast<uint16_t>(req_payload.size()),
        resp_buf, sizeof(resp_buf), resp_len);

    response->set_hal_code(ret);
    if (ret < 0) {
        response->set_success(false);
        response->set_message("MCU raw request failed: " + std::to_string(ret));
    } else {
        response->set_success(true);
        response->set_message("OK");
        if (resp_len > 0) {
            response->set_payload(resp_buf, resp_len);
        }
    }

    return grpc::Status::OK;
}

/* ========== Environment control (fan/heat/radar) ========== */

grpc::Status CameraControlServiceImpl::SetFan(
    grpc::ServerContext*,
    const aipc::camera::EnvCtrlRequest* req,
    aipc::camera::EnvCtrlStatus* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_env_ctrl() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("ENV_CTRL HAL not loaded");
        return grpc::Status::OK;
    }

    HAL_LOG_INFO("[CameraControl] SetFan: enable=%s", req->enable() ? "true" : "false");

    int rc = hal->env_ctrl()->fan_set(hal->mcu_ctx(), req->enable());
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));
    if (rc == 0) resp->set_enabled(req->enable());

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetFan(
    grpc::ServerContext*,
    const aipc::camera::Empty*,
    aipc::camera::EnvCtrlStatus* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_env_ctrl() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("ENV_CTRL HAL not loaded");
        return grpc::Status::OK;
    }

    bool enabled = false;
    int rc = hal->env_ctrl()->fan_get(hal->mcu_ctx(), &enabled);
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));
    if (rc == 0) resp->set_enabled(enabled);

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetHeat(
    grpc::ServerContext*,
    const aipc::camera::EnvCtrlRequest* req,
    aipc::camera::EnvCtrlStatus* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_env_ctrl() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("ENV_CTRL HAL not loaded");
        return grpc::Status::OK;
    }

    HAL_LOG_INFO("[CameraControl] SetHeat: enable=%s", req->enable() ? "true" : "false");

    int rc = hal->env_ctrl()->heat_set(hal->mcu_ctx(), req->enable());
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));
    if (rc == 0) resp->set_enabled(req->enable());

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetHeat(
    grpc::ServerContext*,
    const aipc::camera::Empty*,
    aipc::camera::EnvCtrlStatus* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_env_ctrl() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("ENV_CTRL HAL not loaded");
        return grpc::Status::OK;
    }

    bool enabled = false;
    int rc = hal->env_ctrl()->heat_get(hal->mcu_ctx(), &enabled);
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));
    if (rc == 0) resp->set_enabled(enabled);

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetRadar(
    grpc::ServerContext*,
    const aipc::camera::EnvCtrlRequest* req,
    aipc::camera::EnvCtrlStatus* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_env_ctrl() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("ENV_CTRL HAL not loaded");
        return grpc::Status::OK;
    }

    HAL_LOG_INFO("[CameraControl] SetRadar: enable=%s", req->enable() ? "true" : "false");

    int rc = hal->env_ctrl()->radar_set(hal->mcu_ctx(), req->enable());
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));
    if (rc == 0) resp->set_enabled(req->enable());

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetRadar(
    grpc::ServerContext*,
    const aipc::camera::Empty*,
    aipc::camera::EnvCtrlStatus* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_env_ctrl() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("ENV_CTRL HAL not loaded");
        return grpc::Status::OK;
    }

    bool enabled = false;
    int rc = hal->env_ctrl()->radar_get(hal->mcu_ctx(), &enabled);
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));
    if (rc == 0) resp->set_enabled(enabled);

    return grpc::Status::OK;
}

/* ========== Alarm / Wiegand outputs ========== */

grpc::Status CameraControlServiceImpl::SetAlarmOut(
    grpc::ServerContext*,
    const aipc::camera::AlarmOutRequest* req,
    aipc::camera::AlarmOutStatus* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_alarm() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("ALARM HAL not loaded");
        return grpc::Status::OK;
    }

    HAL_LOG_INFO("[CameraControl] SetAlarmOut: channel=%u enable=%s",
                 req->channel(), req->enable() ? "true" : "false");

    int rc = hal->alarm()->alarm_out_set(hal->mcu_ctx(), req->channel(), req->enable());
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));
    if (rc == 0) resp->set_enabled(req->enable());

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetAlarmOut(
    grpc::ServerContext*,
    const aipc::camera::AlarmOutRequest* req,
    aipc::camera::AlarmOutStatus* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_alarm() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("ALARM HAL not loaded");
        return grpc::Status::OK;
    }

    bool enabled = false;
    int rc = hal->alarm()->alarm_out_get(hal->mcu_ctx(), req->channel(), &enabled);
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));
    if (rc == 0) resp->set_enabled(enabled);

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetWiegandOut(
    grpc::ServerContext*,
    const aipc::camera::WiegandOutRequest* req,
    aipc::camera::AlarmOutStatus* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_alarm() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("ALARM HAL not loaded");
        return grpc::Status::OK;
    }

    HAL_LOG_INFO("[CameraControl] SetWiegandOut: channel=%u enable=%s",
                 req->channel(), req->enable() ? "true" : "false");

    int rc = hal->alarm()->wiegand_out_set(hal->mcu_ctx(), req->channel(), req->enable());
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));
    if (rc == 0) resp->set_enabled(req->enable());

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetWiegandOut(
    grpc::ServerContext*,
    const aipc::camera::WiegandOutRequest* req,
    aipc::camera::AlarmOutStatus* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_alarm() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("ALARM HAL not loaded");
        return grpc::Status::OK;
    }

    bool enabled = false;
    int rc = hal->alarm()->wiegand_out_get(hal->mcu_ctx(), req->channel(), &enabled);
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));
    if (rc == 0) resp->set_enabled(enabled);

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetAlarmOutputs(
    grpc::ServerContext*,
    const aipc::camera::Empty*,
    aipc::camera::AlarmOutputsState* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_alarm() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("ALARM HAL not loaded");
        return grpc::Status::OK;
    }

    HalAlarmOutputsState state;
    int rc = hal->alarm()->outputs_get(hal->mcu_ctx(), &state);
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));
    if (rc == 0) {
        resp->set_alarm_out0(state.alarm_out0);
        resp->set_alarm_out1(state.alarm_out1);
        resp->set_wiegand0(state.wiegand0);
        resp->set_wiegand1(state.wiegand1);
    }

    return grpc::Status::OK;
}

/* ========== RS485 ========== */

grpc::Status CameraControlServiceImpl::Rs485Init(
    grpc::ServerContext*,
    const aipc::camera::Rs485InitRequest* req,
    aipc::camera::Status* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_rs485() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("RS485 HAL not loaded");
        return grpc::Status::OK;
    }

    char config[HAL_RS485_CONFIG_LEN] = {'8', 'N', '1'};
    if (!req->config().empty() && req->config().size() == HAL_RS485_CONFIG_LEN) {
        config[0] = req->config()[0];
        config[1] = req->config()[1];
        config[2] = req->config()[2];
    }

    HAL_LOG_INFO("[CameraControl] Rs485Init: baudrate=%u config=%c%c%c",
                 req->baudrate(), config[0], config[1], config[2]);

    int rc = hal->rs485()->rs485_init(hal->mcu_ctx(), req->baudrate(), config);
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::Rs485Deinit(
    grpc::ServerContext*,
    const aipc::camera::Empty*,
    aipc::camera::Status* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_rs485() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("RS485 HAL not loaded");
        return grpc::Status::OK;
    }

    HAL_LOG_INFO("[CameraControl] Rs485Deinit");

    int rc = hal->rs485()->rs485_deinit(hal->mcu_ctx());
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::Rs485Tx(
    grpc::ServerContext*,
    const aipc::camera::Rs485TxRequest* req,
    aipc::camera::Status* resp) {

    if (!daemon_) {
        resp->set_success(false);
        resp->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    auto* hal = daemon_->hal_loader();
    if (!hal || !hal->has_rs485() || !hal->mcu_ctx()) {
        resp->set_success(false);
        resp->set_message("RS485 HAL not loaded");
        return grpc::Status::OK;
    }

    HAL_LOG_INFO("[CameraControl] Rs485Tx: len=%u", static_cast<uint16_t>(req->data().size()));

    int rc = hal->rs485()->rs485_tx(hal->mcu_ctx(),
                                     reinterpret_cast<const uint8_t*>(req->data().data()),
                                     static_cast<uint16_t>(req->data().size()));
    resp->set_success(rc == 0);
    resp->set_message(rc == 0 ? "OK" : ("HAL error: " + std::to_string(rc)));

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetCapabilities(
    grpc::ServerContext*,
    const aipc::camera::Empty*,
    aipc::camera::CapabilitiesResponse* resp) {
    auto* hal = daemon_ ? daemon_->hal_loader() : nullptr;
    if (!hal) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "HAL not loaded");
    }
    resp->set_has_video(hal->has_video());
    resp->set_has_codec(hal->has_codec());
    resp->set_has_led(hal->has_led());
    resp->set_has_sensor(hal->has_sensor());
    resp->set_has_mcu(hal->has_mcu());
    resp->set_has_env_ctrl(hal->has_env_ctrl());
    resp->set_has_alarm(hal->has_alarm());
    resp->set_has_rs485(hal->has_rs485());
    resp->set_has_osd(hal->has_osd());
    resp->set_has_draw(hal->has_draw());
    resp->set_has_audio(hal->has_audio());
    return grpc::Status::OK;
}

/* ========== Audio RPCs ========== */

grpc::Status CameraControlServiceImpl::ListAudioCaptureDevices(
    grpc::ServerContext*,
    const aipc::camera::Empty*,
    aipc::camera::ListAudioDevicesResponse* resp) {
    auto* svc = daemon_ ? daemon_->audio_service() : nullptr;
    if (!svc) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Audio not available");
    }
    auto devices = svc->list_capture_devices();
    for (auto& d : devices) {
        auto* info = resp->add_devices();
        info->set_name(d.name);
        info->set_description(d.id);
    }
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::ListAudioPlaybackDevices(
    grpc::ServerContext*,
    const aipc::camera::Empty*,
    aipc::camera::ListAudioDevicesResponse* resp) {
    auto* svc = daemon_ ? daemon_->audio_service() : nullptr;
    if (!svc) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Audio not available");
    }
    auto devices = svc->list_playback_devices();
    for (auto& d : devices) {
        auto* info = resp->add_devices();
        info->set_name(d.name);
        info->set_description(d.id);
    }
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::StartAudioCapture(
    grpc::ServerContext*,
    const aipc::camera::AudioConfigRequest* req,
    aipc::camera::Status* resp) {
    auto* svc = daemon_ ? daemon_->audio_service() : nullptr;
    if (!svc) {
        resp->set_success(false);
        resp->set_message("Audio not available");
        return grpc::Status::OK;
    }
    bool ok = svc->start_capture();
    if (ok && req->has_volume()) {
        ok = svc->set_volume(req->volume()) && ok;
    }
    if (ok && req->has_mute()) {
        ok = svc->set_mute(req->mute()) && ok;
    }
    resp->set_success(ok);
    if (!resp->success()) resp->set_message("Start capture failed");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::StopAudioCapture(
    grpc::ServerContext*,
    const aipc::camera::Empty*,
    aipc::camera::Status* resp) {
    auto* svc = daemon_ ? daemon_->audio_service() : nullptr;
    if (!svc) {
        resp->set_success(false);
        resp->set_message("Audio not available");
        return grpc::Status::OK;
    }
    svc->stop_capture();
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetAudioStatus(
    grpc::ServerContext*,
    const aipc::camera::Empty*,
    aipc::camera::AudioStatusResponse* resp) {
    auto* svc = daemon_ ? daemon_->audio_service() : nullptr;
    if (!svc) {
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "Audio not available");
    }
    AudioService::AudioStatus status;
    if (!svc->get_status(HAL_AUDIO_IO_CAPTURE, status)) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Get audio status failed");
    }
    resp->set_capturing(status.capturing);
    resp->set_playing(status.playing);
    resp->set_device(status.device);
    resp->set_sample_rate(status.sample_rate);
    resp->set_channels(status.channels);
    resp->set_codec(status.codec);
    resp->set_volume(status.volume);
    resp->set_mute(status.mute);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetAudioConfig(
    grpc::ServerContext*,
    const aipc::camera::AudioConfigRequest* req,
    aipc::camera::Status* resp) {
    auto* svc = daemon_ ? daemon_->audio_service() : nullptr;
    if (!svc) {
        resp->set_success(false);
        resp->set_message("Audio not available");
        return grpc::Status::OK;
    }
    bool ok = true;
    // volume/mute are proto3 optional: only apply when the caller actually set
    // the field. Without has_*(), a missing volume (default 0.0) would trigger
    // a spurious set_volume(0) (which stop+restarts the ALSA device) and a
    // missing mute could never express "unmute" (mute=false is the default).
    if (req->has_volume()) {
        ok = svc->set_volume(req->volume()) && ok;
    }
    if (req->has_mute()) {
        ok = svc->set_mute(req->mute()) && ok;
    }
    resp->set_success(ok);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::StartAudioPlayback(
    grpc::ServerContext*,
    const aipc::camera::AudioConfigRequest* req,
    aipc::camera::Status* resp) {
    auto* svc = daemon_ ? daemon_->audio_service() : nullptr;
    if (!svc) {
        resp->set_success(false);
        resp->set_message("Audio not available");
        return grpc::Status::OK;
    }
    bool ok = svc->start_playback(
        req->device().empty() ? "default" : req->device(),
        req->sample_rate() > 0 ? req->sample_rate() : 48000,
        req->channels() > 0 ? req->channels() : 1,
        req->codec().empty() ? "pcm" : req->codec());
    resp->set_success(ok);
    if (!ok) resp->set_message("Playback start failed");
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::StreamAudioPcm(
    grpc::ServerContext*,
    grpc::ServerReader<aipc::camera::AudioPcmChunk>* reader,
    aipc::camera::Status* resp) {
    auto* svc = daemon_ ? daemon_->audio_service() : nullptr;
    if (!svc) {
        resp->set_success(false);
        resp->set_message("Audio not available");
        return grpc::Status::OK;
    }
    aipc::camera::AudioPcmChunk chunk;
    while (reader->Read(&chunk)) {
        if (!svc->write_pcm(reinterpret_cast<const uint8_t*>(chunk.data().data()), chunk.data().size())) {
            resp->set_success(false);
            resp->set_message("PCM write failed");
            return grpc::Status::OK;
        }
    }
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::StopAudioPlayback(
    grpc::ServerContext*,
    const aipc::camera::Empty*,
    aipc::camera::Status* resp) {
    auto* svc = daemon_ ? daemon_->audio_service() : nullptr;
    if (!svc) {
        resp->set_success(false);
        resp->set_message("Audio not available");
        return grpc::Status::OK;
    }
    svc->stop_playback();
    resp->set_success(true);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetPrivacyMaskConfig(
    grpc::ServerContext* context,
    const aipc::camera::Empty* request,
    aipc::camera::PrivacyMaskConfig* response) {

    if (!daemon_) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    if (!daemon_->get_privacy_mask_config(*response)) {
        return grpc::Status(grpc::StatusCode::INTERNAL, "Failed to get privacy mask config");
    }

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetPrivacyMaskConfig(
    grpc::ServerContext* context,
    const aipc::camera::PrivacyMaskConfig* request,
    aipc::camera::Status* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] SetPrivacyMask: enabled=%d blur=%d regions=%d",
                 request->enabled(), request->blur_radius(), request->regions_size());

    bool success = daemon_->set_privacy_mask_config(*request);
    response->set_success(success);
    response->set_message(success ? "Success" : "Failed to apply privacy mask config");

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SetConfigField(
    grpc::ServerContext* context,
    const aipc::camera::SetConfigFieldRequest* request,
    aipc::camera::Status* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    HAL_LOG_INFO("[CameraControl] SetConfigField: %s type=%d value=%s",
                 request->field_path().c_str(), (int)request->type(), request->value().c_str());

    // Allow-list + HAL apply + mirror persist all live in the daemon; msg carries
    // the precise failure reason (e.g. "field not in allow-list: ...") on false.
    std::string msg;
    bool success = daemon_->set_config_field(*request, &msg);
    response->set_success(success);
    response->set_message(success ? "Success" : msg);

    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::GetConfigField(
    grpc::ServerContext* context,
    const aipc::camera::GetConfigFieldRequest* request,
    aipc::camera::GetConfigFieldResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    aipc::camera::ConfigFieldType type = aipc::camera::CONFIG_FIELD_BOOL;
    std::string value;
    std::string msg;
    bool success = daemon_->get_config_field(request->field_path(), type, value, &msg);
    response->set_success(success);
    response->set_message(success ? "Success" : msg);
    if (success) {
        response->set_type(type);
        response->set_value(value);
    }

    HAL_LOG_INFO("[CameraControl] GetConfigField: %s -> success=%d value=%s",
                 request->field_path().c_str(), success ? 1 : 0,
                 success ? value.c_str() : "(n/a)");
    return grpc::Status::OK;
}

bool CameraControlServiceImpl::FillDspJobDesc(
    const aipc::camera::DspJobRequest* request,
    DspJobDesc& desc,
    aipc::camera::DspJobResponse* response) {

    // DspOp is NOT a numeric mirror of HalDspOpType: proto orders RESIZE first
    // and CONVERT last, HAL puts CONVERT_FORMAT at 0 — explicit switch, no cast.
    switch (request->op()) {
    case aipc::camera::DSP_OP_RESIZE:
        desc.op = HAL_DSP_OP_RESIZE; break;
    case aipc::camera::DSP_OP_CROP_AND_RESIZE:
        desc.op = HAL_DSP_OP_CROP_RESIZE; break;
    case aipc::camera::DSP_OP_MULTI_CROP_AND_RESIZE:
        desc.op = HAL_DSP_OP_MULTI_CROP_RESIZE; break;
    case aipc::camera::DSP_OP_CONVERT_FORMAT:
        desc.op = HAL_DSP_OP_CONVERT_FORMAT; break;
    case aipc::camera::DSP_OP_BLEND:
        desc.op = HAL_DSP_OP_BLEND; break;
    default:
        response->set_success(false);
        response->set_message("unknown DspOp " +
                              std::to_string(static_cast<int>(request->op())));
        response->set_error_code(DSP_SVC_ERR_INVALID);
        return false;
    }

    // Interpolation + scaling mode ARE order-identical 0..3 in proto and HAL.
    int interp = static_cast<int>(request->interpolation());
    int scaling = static_cast<int>(request->scaling_mode());
    if (interp < static_cast<int>(aipc::camera::DSP_INTERP_NEAREST) ||
        interp > static_cast<int>(aipc::camera::DSP_INTERP_BICUBIC) ||
        scaling < static_cast<int>(aipc::camera::DSP_SCALING_STRETCH) ||
        scaling > static_cast<int>(aipc::camera::DSP_SCALING_SCALE_AND_CROP)) {
        response->set_success(false);
        response->set_message("DspInterpolation/DspScalingMode out of range");
        response->set_error_code(DSP_SVC_ERR_INVALID);
        return false;
    }
    desc.interpolation = static_cast<HalDspInterpolation>(interp);
    desc.scaling_mode = static_cast<HalDspScalingMode>(scaling);

    desc.src_id = request->src_buffer_id();
    for (uint64_t id : request->dst_buffer_ids()) {
        desc.dst_ids.push_back(id);
    }
    for (const auto& r : request->rects()) {
        DspRect dr;
        dr.x = r.x();
        dr.y = r.y();
        dr.width = r.width();
        dr.height = r.height();
        dr.dst_width = r.dst_width();
        dr.dst_height = r.dst_height();
        desc.rects.push_back(dr);
    }
    desc.priority = (request->priority() == aipc::camera::DSP_PRIORITY_BACKGROUND)
                        ? DspPriority::Background
                        : DspPriority::Normal;
    return true;
}

grpc::Status CameraControlServiceImpl::SubmitDspJob(
    grpc::ServerContext* context,
    const aipc::camera::DspJobRequest* request,
    aipc::camera::DspJobResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        response->set_error_code(DSP_SVC_ERR_UNAVAILABLE);
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    DspService* svc = daemon_->dsp_service();
    if (!svc || !svc->is_running()) {
        response->set_success(false);
        response->set_message("DSP offload service unavailable");
        response->set_error_code(DSP_SVC_ERR_UNAVAILABLE);
        return grpc::Status::OK;
    }

    DspJobDesc desc;
    if (!FillDspJobDesc(request, desc, response)) return grpc::Status::OK;

    HAL_LOG_INFO("[CameraControl] SubmitDspJob: op=%d src=%lu dsts=%zu rects=%zu",
                 static_cast<int>(request->op()),
                 (unsigned long)desc.src_id, desc.dst_ids.size(),
                 desc.rects.size());

    DspJobResult result = svc->submit_job(desc);
    bool ok = (result.rc == DSP_SVC_OK);
    response->set_success(ok);
    response->set_message(ok && result.message.empty() ? "OK" : result.message);
    response->set_error_code(result.rc);
    response->set_elapsed_ms(result.elapsed_ms);
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::SubmitDspJobAsync(
    grpc::ServerContext* context,
    const aipc::camera::DspJobRequest* request,
    aipc::camera::DspJobResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        response->set_error_code(DSP_SVC_ERR_UNAVAILABLE);
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    DspService* svc = daemon_->dsp_service();
    if (!svc || !svc->is_running()) {
        response->set_success(false);
        response->set_message("DSP offload service unavailable");
        response->set_error_code(DSP_SVC_ERR_UNAVAILABLE);
        return grpc::Status::OK;
    }

    DspJobDesc desc;
    if (!FillDspJobDesc(request, desc, response)) return grpc::Status::OK;

    HAL_LOG_INFO("[CameraControl] SubmitDspJobAsync: op=%d src=%lu dsts=%zu rects=%zu",
                 static_cast<int>(request->op()),
                 (unsigned long)desc.src_id, desc.dst_ids.size(),
                 desc.rects.size());

    uint64_t job_id = 0;
    DspJobResult result = svc->submit_job_async(desc, job_id);
    bool ok = (result.rc == DSP_SVC_OK);
    response->set_success(ok);
    response->set_message(result.message);
    response->set_error_code(result.rc);
    response->set_job_id(job_id);
    response->set_done(false); /* enqueued, not executed */
    return grpc::Status::OK;
}

grpc::Status CameraControlServiceImpl::WaitDspJob(
    grpc::ServerContext* context,
    const aipc::camera::DspWaitRequest* request,
    aipc::camera::DspJobResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        response->set_error_code(DSP_SVC_ERR_UNAVAILABLE);
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    DspService* svc = daemon_->dsp_service();
    if (!svc || !svc->is_running()) {
        response->set_success(false);
        response->set_message("DSP offload service unavailable");
        response->set_error_code(DSP_SVC_ERR_UNAVAILABLE);
        return grpc::Status::OK;
    }

    bool done = false;
    DspJobResult result = svc->wait_job(request->job_id(), request->timeout_ms(),
                                        done);
    bool ok = (result.rc == DSP_SVC_OK);
    response->set_success(ok);
    response->set_message(ok && result.message.empty() ? "OK" : result.message);
    response->set_error_code(result.rc);
    response->set_elapsed_ms(result.elapsed_ms);
    response->set_job_id(request->job_id());
    /* done tracks the job's lifecycle, not its outcome: a completed job
     * with a nonzero HAL rc is done too, and reporting it as pending would
     * send the client back to a poll that can only answer "unknown or
     * reaped job id" (the entry is dropped at reap). false = still pending
     * (timeout / poll) or unknown id — error_code separates those two. */
    response->set_done(done);
    return grpc::Status::OK;
}

/* ================================================================== */
/* EncodeImage — one-shot JPEG encode of a DSP-registry buffer (S-3(a)) */
/* ================================================================== */

namespace {

/* Wall-clock budget for one JPEG frame through the standalone encoder
 * (first-call GStreamer pipeline spin-up included). Mirrors the DSP
 * service's job_timeout_ms convention: a slow encode fails the RPC. */
constexpr std::chrono::milliseconds kJpegShotTimeout(2000);
constexpr uint32_t kJpegShotDefaultQuality = 85;

} // namespace

/**
 * Daemon-owned standalone MJPEG encoder backing EncodeImage.
 *
 * One HAL codec context, recreated when (width, height, format, quality)
 * changes — and after a timed-out or failed encode, which is also the
 * stale-packet guard: a late packet from a destroyed context can never
 * match the fresh wait (wait_ctx_ is compared against the codec_ctx the
 * packet arrived on). Encodes are serialized on ctx_mu_ (single-stream
 * appsrc pipeline underneath); the wait state has its own wait_mu_ so the
 * packet callback never contends with context teardown — unsubscribe
 * drains in-flight callbacks and the callback must be able to finish.
 *
 * The callback copies the JPEG bytes under wait_mu_ and releases the
 * packet itself, so no packet pointer ever crosses threads.
 */
struct JpegShotState {
    struct Shot {
        int rc = HAL_OK;          /* 0, negative HAL rc, or DspServiceError */
        std::string message;
        std::vector<uint8_t> jpeg;
        uint32_t elapsed_ms = 0;
    };

    explicit JpegShotState(HalCodecOps* ops) : ops_(ops) {}

    Shot encode(HalFrameBuffer* fb, uint32_t quality) {
        Shot shot;
        const auto t0 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> ctx_lk(ctx_mu_);

        std::string why;
        int rc = ensure_ctx_locked(fb, quality, why);
        if (rc != HAL_OK) {
            shot.rc = rc;
            shot.message = why;
            return shot;
        }

        /* Arm the wait, then feed the frame. */
        {
            std::lock_guard<std::mutex> wait_lk(wait_mu_);
            out_.clear();
            wait_done_ = false;
            wait_active_ = true;
            wait_ctx_ = handle_;
        }

        rc = ops_->input_frame(handle_, fb);
        if (rc != HAL_OK) {
            {
                std::lock_guard<std::mutex> wait_lk(wait_mu_);
                wait_active_ = false;
                wait_ctx_ = nullptr;
            }
            /* The pipeline may be wedged — start clean on the next shot. */
            destroy_ctx_locked();
            shot.rc = rc;
            shot.message = "encoder rejected the frame";
            return shot;
        }

        bool done = false;
        {
            std::unique_lock<std::mutex> wait_lk(wait_mu_);
            done = cv_.wait_for(wait_lk, kJpegShotTimeout,
                                [this] { return wait_done_; });
            wait_active_ = false;
            wait_ctx_ = nullptr;
            if (done) shot.jpeg = std::move(out_);
        }
        shot.elapsed_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0)
                .count());

        if (!done) {
            destroy_ctx_locked();
            shot.rc = DSP_SVC_ERR_TIMEOUT;
            shot.message = "jpeg encode timed out";
            return shot;
        }
        if (shot.jpeg.empty()) {
            destroy_ctx_locked();
            shot.rc = HAL_ERR_RESULT;
            shot.message = "encoder produced an empty packet";
            return shot;
        }
        return shot;
    }

    static void on_packet_thunk(void* codec_ctx, HalPacketBuffer* pkt, void* userdata) {
        auto* self = static_cast<JpegShotState*>(userdata);
        if (!self) return;
        bool mine = false;
        if (pkt && pkt->type == HAL_PACKET_TYPE_MJPEG && pkt->data && pkt->size > 0u) {
            std::lock_guard<std::mutex> lk(self->wait_mu_);
            if (self->wait_active_ && self->wait_ctx_ == codec_ctx) {
                self->out_.assign(pkt->data, pkt->data + pkt->size);
                self->wait_done_ = true;
                mine = true;
            }
        }
        if (mine) self->cv_.notify_one();
        /* The HAL fills every packet with a heap priv holding a shared
         * buffer ref — release it on every path, matching or not. */
        if (pkt) self->ops_->release_packet(codec_ctx, pkt);
    }

    ~JpegShotState() {
        std::lock_guard<std::mutex> lk(ctx_mu_);
        destroy_ctx_locked();
    }

    HalCodecOps* ops_;

    /* Context lifecycle (encode() holds ctx_mu_ for its whole call). */
    std::mutex ctx_mu_;
    void* handle_ = nullptr;
    std::string key_;

    /* Wait state, shared with on_packet_thunk. */
    std::mutex wait_mu_;
    std::condition_variable cv_;
    bool wait_active_ = false;
    void* wait_ctx_ = nullptr;
    bool wait_done_ = false;
    std::vector<uint8_t> out_;

private:
    int ensure_ctx_locked(HalFrameBuffer* fb, uint32_t quality, std::string& why) {
        char key[64];
        std::snprintf(key, sizeof(key), "%ux%u:%d:q%u", fb->width, fb->height,
                      static_cast<int>(fb->format), quality);
        if (handle_ && key_ == key) return HAL_OK;

        destroy_ctx_locked();

        HalCodecConfig cfg = {};
        cfg.type = HAL_CODEC_TYPE_HW;
        cfg.packet_type = HAL_PACKET_TYPE_MJPEG;
        cfg.width = fb->width;
        cfg.height = fb->height;
        cfg.format = fb->format;
        cfg.framerate = 30; /* caps only — one-shot use, not a stream */
        cfg.jpeg_quality = quality;

        void* handle = nullptr;
        int rc = ops_->init(&cfg, &handle);
        if (rc != HAL_OK || !handle) {
            why = "standalone jpeg encoder init failed";
            return rc != HAL_OK ? rc : HAL_ERR_RESULT;
        }
        rc = ops_->subscribe(handle, &JpegShotState::on_packet_thunk, this);
        if (rc != HAL_OK) {
            why = "standalone jpeg encoder subscribe failed";
            ops_->deinit(handle);
            return rc;
        }
        rc = ops_->start(handle);
        if (rc != HAL_OK) {
            why = "standalone jpeg encoder start failed";
            ops_->unsubscribe(handle, &JpegShotState::on_packet_thunk);
            ops_->deinit(handle);
            return rc;
        }
        handle_ = handle;
        key_ = key;
        HAL_LOG_INFO("EncodeImage: standalone jpeg encoder up (%s)", key);
        return HAL_OK;
    }

    void destroy_ctx_locked() {
        if (!handle_) return;
        /* Not holding wait_mu_ here on purpose: unsubscribe drains
         * in-flight callbacks and on_packet_thunk takes wait_mu_. */
        ops_->unsubscribe(handle_, &JpegShotState::on_packet_thunk);
        ops_->deinit(handle_);
        handle_ = nullptr;
        key_.clear();
    }
};

grpc::Status CameraControlServiceImpl::EncodeImage(
    grpc::ServerContext* /*context*/,
    const aipc::camera::EncodeImageRequest* request,
    aipc::camera::EncodeImageResponse* response) {

    if (!daemon_) {
        response->set_success(false);
        response->set_message("CameraDaemon not initialized");
        response->set_error_code(DSP_SVC_ERR_UNAVAILABLE);
        return grpc::Status(grpc::StatusCode::INTERNAL, "Daemon missing");
    }

    DspService* dsp = daemon_->dsp_service();
    if (!dsp || !dsp->is_running()) {
        response->set_success(false);
        response->set_message("DSP buffer registry unavailable");
        response->set_error_code(DSP_SVC_ERR_UNAVAILABLE);
        return grpc::Status::OK;
    }

    HalCodecOps* codec_ops = daemon_->hal_loader()->codec();
    if (!codec_ops || !codec_ops->init || !codec_ops->subscribe ||
        !codec_ops->input_frame) {
        response->set_success(false);
        response->set_message("codec HAL unavailable");
        response->set_error_code(DSP_SVC_ERR_UNAVAILABLE);
        return grpc::Status::OK;
    }

    if (request->quality() > 100u) {
        response->set_success(false);
        response->set_message("quality out of range [0..100]");
        response->set_error_code(DSP_SVC_ERR_INVALID);
        return grpc::Status::OK;
    }
    const uint32_t quality = request->quality() == 0u
                                 ? kJpegShotDefaultQuality
                                 : request->quality();

    DspService::BufferPin pin = dsp->pin_buffer(request->src_buffer_id());
    if (!pin.ok()) {
        response->set_success(false);
        response->set_message("unknown or foreign buffer id");
        response->set_error_code(pin.rc());
        return grpc::Status::OK;
    }

    HalFrameBuffer* fb = pin.fb();
    if (!fb || fb->width == 0u || fb->height == 0u ||
        fb->mem_type != HAL_MEM_DMABUF || fb->num_planes == 0u) {
        response->set_success(false);
        response->set_message("buffer is not a usable dma-buf frame");
        response->set_error_code(DSP_SVC_ERR_INVALID);
        return grpc::Status::OK;
    }

    /* Normalize to NV12 before the encoder: every hailoencodebin carries an
     * OSD element with NV12-only pad templates, so any other input format
     * fails caps negotiation deep inside the pipeline. NV12 feeds the
     * encoder directly (zero-copy); RGB/BGR ride the DSP CONVERT job into a
     * per-call scratch NV12 buffer; the rest is rejected with a clear
     * message — the SDK client takes that as a normal hardware miss and
     * falls back to its CPU leg. */
    HalFrameBuffer* encode_fb = fb;
    DspService::BufferPin nv12_pin;
    if (fb->format != HAL_PIX_FMT_NV12) {
        if (fb->format != HAL_PIX_FMT_RGB24 && fb->format != HAL_PIX_FMT_BGR24) {
            response->set_success(false);
            response->set_message(
                "source format not supported by hardware jpeg encoder "
                "(nv12/rgb24/bgr24)");
            response->set_error_code(DSP_SVC_ERR_INVALID);
            return grpc::Status::OK;
        }

        DspService::AllocResult scratch = dsp->alloc_buffers(
            pin.owner_fd(), fb->width, fb->height, HAL_PIX_FMT_NV12, 1);
        if (scratch.rc != DSP_SVC_OK || scratch.ids.empty()) {
            response->set_success(false);
            response->set_message("scratch nv12 buffer alloc failed: " +
                                  scratch.message);
            response->set_error_code(scratch.rc != DSP_SVC_OK ? scratch.rc
                                                             : DSP_SVC_ERR_NO_MEM);
            return grpc::Status::OK;
        }

        DspJobDesc conv;
        conv.op = HAL_DSP_OP_CONVERT_FORMAT;
        conv.src_id = request->src_buffer_id();
        conv.dst_ids = scratch.ids;
        DspJobResult conv_res = dsp->submit_job(conv);
        if (conv_res.rc != DSP_SVC_OK) {
            dsp->release_buffer(pin.owner_fd(), scratch.ids[0]);
            response->set_success(false);
            response->set_message("dsp convert rgb->nv12 failed: " +
                                  conv_res.message);
            response->set_error_code(conv_res.rc);
            return grpc::Status::OK;
        }

        nv12_pin = dsp->pin_buffer(scratch.ids[0]);
        /* Detach the id now — the pin keeps the HAL buffer alive to the end
         * of this call even if the client vanishes mid-encode. */
        dsp->release_buffer(pin.owner_fd(), scratch.ids[0]);
        if (!nv12_pin.ok() || !nv12_pin.fb()) {
            response->set_success(false);
            response->set_message("converted nv12 buffer vanished");
            response->set_error_code(DSP_SVC_ERR_NO_BUFFER);
            return grpc::Status::OK;
        }
        encode_fb = nv12_pin.fb();
    }

    /* Serialize encoder-context use: JpegShotState's internals are guarded,
     * but the jpeg_ pointer itself (lazy create + shutdown teardown) is not
     * RPC-thread safe. One-shot snapshots simply queue behind each other. */
    std::lock_guard<std::mutex> jpeg_lk(jpeg_mu_);
    if (!jpeg_ || jpeg_->ops_ != codec_ops) {
        jpeg_ = std::make_unique<JpegShotState>(codec_ops);
    }

    HAL_LOG_INFO("[CameraControl] EncodeImage: src=%lu %ux%u fmt=%d q=%u%s",
                 (unsigned long)request->src_buffer_id(), fb->width, fb->height,
                 static_cast<int>(fb->format), quality,
                 encode_fb == fb ? "" : " +dsp-convert->nv12");

    JpegShotState::Shot shot = jpeg_->encode(encode_fb, quality);
    const bool ok = (shot.rc == HAL_OK);
    response->set_success(ok);
    response->set_message(ok ? "OK" : shot.message);
    response->set_error_code(shot.rc);
    response->set_elapsed_ms(shot.elapsed_ms);
    if (ok) response->set_jpeg(shot.jpeg.data(), shot.jpeg.size());
    return grpc::Status::OK;
}

CameraControlServiceImpl::~CameraControlServiceImpl() {
    /* Out-of-line and placed after JpegShotState's definition: destroying
     * jpeg_ (unique_ptr<JpegShotState>) needs the complete type. */
    std::lock_guard<std::mutex> lk(jpeg_mu_);
    jpeg_.reset();
}
