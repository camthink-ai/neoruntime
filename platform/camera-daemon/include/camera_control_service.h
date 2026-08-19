#pragma once

#include "camera.grpc.pb.h"
#include <grpcpp/grpcpp.h>

class CameraDaemon;

class CameraControlServiceImpl final : public aipc::camera::CameraControl::Service {
public:
    explicit CameraControlServiceImpl(CameraDaemon* daemon);
    ~CameraControlServiceImpl() override = default;

    grpc::Status StartOneShotAutofocus(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::AutofocusJobResponse* response) override;

    grpc::Status StartZoomFollow(
        grpc::ServerContext* context,
        const aipc::camera::AutofocusZoomFollowRequest* request,
        aipc::camera::AutofocusJobResponse* response) override;

    grpc::Status GetAutofocusStatus(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::AutofocusStatusResponse* response) override;

    grpc::Status CancelAutofocus(
        grpc::ServerContext* context,
        const aipc::camera::AutofocusJobRequest* request,
        aipc::camera::Status* response) override;

    grpc::Status InvalidateAutofocusAnchor(
        grpc::ServerContext* context,
        const aipc::camera::AutofocusInvalidateRequest* request,
        aipc::camera::Status* response) override;

    grpc::Status UpdateISPSettings(
        grpc::ServerContext* context,
        const aipc::camera::ISPUpdateRequest* request,
        aipc::camera::ISPUpdateResponse* response) override;

    grpc::Status GetISPConfig(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::ISPConfigResponse* response) override;

    grpc::Status GetTransformConfig(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::TransformConfig* response) override;

    grpc::Status SetTransformConfig(
        grpc::ServerContext* context,
        const aipc::camera::TransformConfig* request,
        aipc::camera::Status* response) override;

    grpc::Status UpdateEncoderConfig(
        grpc::ServerContext* context,
        const aipc::camera::EncoderConfigRequest* request,
        aipc::camera::Status* response) override;

    grpc::Status SetRtspEnabled(
        grpc::ServerContext* context,
        const aipc::camera::RtspEnabledRequest* request,
        aipc::camera::Status* response) override;

    grpc::Status UpdateAiOverlay(
        grpc::ServerContext* context,
        const aipc::camera::AiOverlayConfig* request,
        aipc::camera::Status* response) override;

    grpc::Status UpdateOsdConfig(
        grpc::ServerContext* context,
        const aipc::camera::OsdConfigRequest* request,
        aipc::camera::Status* response) override;

    grpc::Status GetOsdConfig(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::OsdConfigResponse* response) override;

    grpc::Status ReconfigureEncoder(
        grpc::ServerContext* context,
        const aipc::camera::EncoderReconfigRequest* request,
        aipc::camera::EncoderReconfigResponse* response) override;

    grpc::Status GetProfile(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::GetProfileResponse* response) override;

    grpc::Status ListProfiles(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::ListProfilesResponse* response) override;

    grpc::Status SwitchProfile(
        grpc::ServerContext* context,
        const aipc::camera::SwitchProfileRequest* request,
        aipc::camera::EncoderReconfigResponse* response) override;

    grpc::Status BackupProfile(
        grpc::ServerContext* context,
        const aipc::camera::BackupProfileRequest* request,
        aipc::camera::BackupProfileResponse* response) override;

    grpc::Status ReconfigurePipeline(
        grpc::ServerContext* context,
        const aipc::camera::ReconfigurePipelineRequest* request,
        aipc::camera::ReconfigurePipelineResponse* response) override;

    grpc::Status GetSensorInfo(
        grpc::ServerContext* context,
        const aipc::camera::GetSensorInfoRequest* request,
        aipc::camera::SensorInfoResponse* response) override;

    grpc::Status GetStreamStatus(
        grpc::ServerContext* context,
        const aipc::camera::GetStreamStatusRequest* request,
        aipc::camera::GetStreamStatusResponse* response) override;

    grpc::Status AddStream(
        grpc::ServerContext* context,
        const aipc::camera::AddStreamRequest* request,
        aipc::camera::StreamOperationResponse* response) override;

    grpc::Status RemoveStream(
        grpc::ServerContext* context,
        const aipc::camera::RemoveStreamRequest* request,
        aipc::camera::StreamOperationResponse* response) override;

    grpc::Status SetIrCut(
        grpc::ServerContext* context,
        const aipc::camera::SetIrCutRequest* request,
        aipc::camera::SetIrCutResponse* response) override;

    grpc::Status GetIrCut(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::SetIrCutResponse* response) override;

    grpc::Status SetLedDuty(
        grpc::ServerContext* context,
        const aipc::camera::SetLedDutyRequest* request,
        aipc::camera::LedStatus* response) override;

    grpc::Status GetLedDuty(
        grpc::ServerContext* context,
        const aipc::camera::GetLedDutyRequest* request,
        aipc::camera::LedStatus* response) override;

    grpc::Status GetDeviceHardwareStatus(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::DeviceHardwareStatus* response) override;

    grpc::Status McuRawRequest(
        grpc::ServerContext* context,
        const aipc::camera::McuRawRequestMessage* request,
        aipc::camera::McuRawResponseMessage* response) override;

    // Environment control
    grpc::Status SetFan(
        grpc::ServerContext* context,
        const aipc::camera::EnvCtrlRequest* request,
        aipc::camera::EnvCtrlStatus* response) override;

    grpc::Status GetFan(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::EnvCtrlStatus* response) override;

    grpc::Status SetHeat(
        grpc::ServerContext* context,
        const aipc::camera::EnvCtrlRequest* request,
        aipc::camera::EnvCtrlStatus* response) override;

    grpc::Status GetHeat(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::EnvCtrlStatus* response) override;

    grpc::Status SetRadar(
        grpc::ServerContext* context,
        const aipc::camera::EnvCtrlRequest* request,
        aipc::camera::EnvCtrlStatus* response) override;

    grpc::Status GetRadar(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::EnvCtrlStatus* response) override;

    // Alarm / Wiegand outputs
    grpc::Status SetAlarmOut(
        grpc::ServerContext* context,
        const aipc::camera::AlarmOutRequest* request,
        aipc::camera::AlarmOutStatus* response) override;

    grpc::Status GetAlarmOut(
        grpc::ServerContext* context,
        const aipc::camera::AlarmOutRequest* request,
        aipc::camera::AlarmOutStatus* response) override;

    grpc::Status SetWiegandOut(
        grpc::ServerContext* context,
        const aipc::camera::WiegandOutRequest* request,
        aipc::camera::AlarmOutStatus* response) override;

    grpc::Status GetWiegandOut(
        grpc::ServerContext* context,
        const aipc::camera::WiegandOutRequest* request,
        aipc::camera::AlarmOutStatus* response) override;

    grpc::Status GetAlarmOutputs(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::AlarmOutputsState* response) override;

    // RS485
    grpc::Status Rs485Init(
        grpc::ServerContext* context,
        const aipc::camera::Rs485InitRequest* request,
        aipc::camera::Status* response) override;

    grpc::Status Rs485Deinit(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::Status* response) override;

    grpc::Status Rs485Tx(
        grpc::ServerContext* context,
        const aipc::camera::Rs485TxRequest* request,
        aipc::camera::Status* response) override;

    grpc::Status GetCapabilities(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::CapabilitiesResponse* response) override;

    // Audio
    grpc::Status ListAudioCaptureDevices(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::ListAudioDevicesResponse* response) override;

    grpc::Status ListAudioPlaybackDevices(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::ListAudioDevicesResponse* response) override;

    grpc::Status StartAudioCapture(
        grpc::ServerContext* context,
        const aipc::camera::AudioConfigRequest* request,
        aipc::camera::Status* response) override;

    grpc::Status StopAudioCapture(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::Status* response) override;

    grpc::Status GetAudioStatus(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::AudioStatusResponse* response) override;

    grpc::Status SetAudioConfig(
        grpc::ServerContext* context,
        const aipc::camera::AudioConfigRequest* request,
        aipc::camera::Status* response) override;

    grpc::Status StartAudioPlayback(
        grpc::ServerContext* context,
        const aipc::camera::AudioConfigRequest* request,
        aipc::camera::Status* response) override;

    grpc::Status StreamAudioPcm(
        grpc::ServerContext* context,
        grpc::ServerReader<aipc::camera::AudioPcmChunk>* reader,
        aipc::camera::Status* response) override;

    grpc::Status StopAudioPlayback(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::Status* response) override;

    // Privacy mask
    grpc::Status GetPrivacyMaskConfig(
        grpc::ServerContext* context,
        const aipc::camera::Empty* request,
        aipc::camera::PrivacyMaskConfig* response) override;

    grpc::Status SetPrivacyMaskConfig(
        grpc::ServerContext* context,
        const aipc::camera::PrivacyMaskConfig* request,
        aipc::camera::Status* response) override;

    // Scalar profile config field (set/get). Thin pass-through to
    // CameraDaemon::set_config_field / get_config_field. The allow-list +
    // persistence + replay-on-boot all live in the daemon; the service layer
    // just forwards the RPC. See camera.proto ConfigFieldType.
    grpc::Status SetConfigField(
        grpc::ServerContext* context,
        const aipc::camera::SetConfigFieldRequest* request,
        aipc::camera::Status* response) override;

    grpc::Status GetConfigField(
        grpc::ServerContext* context,
        const aipc::camera::GetConfigFieldRequest* request,
        aipc::camera::GetConfigFieldResponse* response) override;

private:
    CameraDaemon* daemon_;
};
