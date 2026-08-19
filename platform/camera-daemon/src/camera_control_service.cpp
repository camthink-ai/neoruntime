#include "camera_control_service.h"
#include "camera_daemon.h"
#include "hal_loader.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_set>

extern "C" {
    #include "hal_log.h"
    #include "peripheral/devices/hal_env_ctrl.h"
    #include "peripheral/devices/hal_alarm.h"
    #include "peripheral/devices/hal_rs485.h"
}

CameraControlServiceImpl::CameraControlServiceImpl(CameraDaemon* daemon)
    : daemon_(daemon) {
}

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
