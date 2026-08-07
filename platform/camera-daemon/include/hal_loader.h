/**
 * @file hal_loader.h
 * @brief HAL Dynamic Loader - Load HAL libraries via dlopen
 *
 * Resolves HAL_VIDEO_OPS, HAL_CODEC_OPS, HAL_MEDIA_OPS, HAL_ISP_OPS, HAL_AUDIO_OPS
 * from shared libraries. OSD via separate HAL_OSD_OPS; drawing via HAL_DRAW_OPS.
 */

#pragma once

#include <string>

extern "C" {
    #include "hal_video.h"
    #include "hal_codec.h"
    #include "hal_osd.h"
    #include "hal_draw.h"
    #include "hal_media.h"
    #include "hal_isp.h"
    #include "hal_audio.h"
    #include "hal_inference.h"
    #include "hal_postprocess.h"
    #include "hal_dsp.h"
    #include "hal_buffer.h"
    #include "peripheral/devices/hal_led.h"
    #include "peripheral/devices/hal_sensor.h"
    #include "peripheral/devices/hal_env_ctrl.h"
    #include "peripheral/devices/hal_alarm.h"
    #include "peripheral/devices/hal_rs485.h"
    #include "peripheral/hal_mcu.h"
}

class HalLoader {
public:
    HalLoader() = default;
    ~HalLoader();

    HalLoader(const HalLoader&) = delete;
    HalLoader& operator=(const HalLoader&) = delete;

    /**
     * @brief Load HAL libraries
     * @param video_lib Path to video HAL .so (required)
     * @param codec_lib Path to codec HAL .so (optional, empty to skip)
     * @param overlay_lib Path to draw HAL .so (optional, empty to skip)
     * @param led_lib Path to LED HAL .so (optional, empty to skip)
     * @param audio_lib Path to audio HAL .so (optional, empty = try monolithic)
     * @return true if video HAL loaded successfully
     */
    bool load(const std::string& video_lib,
              const std::string& codec_lib = "",
              const std::string& overlay_lib = "",
              const std::string& led_lib = "",
              const std::string& audio_lib = "");

    void unload();

    HalVideoOps* video() const { return video_ops_; }
    HalCodecOps* codec() const { return codec_ops_; }
    HalMediaOps* media() const { return media_ops_; }
    HalIspOps*   isp()   const { return isp_ops_; }

    HalOsdOps*        osd()   const { return osd_ops_; }
    const HalDrawOps* draw()  const { return draw_ops_; }
    bool has_osd()   const { return osd_ops_ != nullptr; }
    bool has_draw()  const { return draw_ops_ != nullptr; }

    bool has_video() const { return video_ops_ != nullptr; }
    bool has_codec() const { return codec_ops_ != nullptr; }
    bool has_media() const { return media_ops_ != nullptr; }
    bool has_isp()   const { return isp_ops_ != nullptr; }
    bool has_led()   const { return led_ops_ != nullptr; }
    bool has_sensor() const { return sensor_ops_ != nullptr; }
    bool has_mcu()   const { return mcu_ops_ != nullptr; }
    bool has_env_ctrl() const { return env_ctrl_ops_ != nullptr; }
    bool has_alarm()   const { return alarm_ops_ != nullptr; }
    bool has_rs485()   const { return rs485_ops_ != nullptr; }
    bool has_audio()   const { return audio_ops_ != nullptr; }
    bool has_inference()   const { return inference_ops_ != nullptr; }
    bool has_postprocess() const { return postprocess_ops_ != nullptr; }
    bool has_dsp()         const { return dsp_ops_ != nullptr; }
    bool has_frame_buffer() const { return frame_buffer_ops_ != nullptr; }

    HalLedOps*       led()       const { return led_ops_; }
    HalSensorOps*    sensor()    const { return sensor_ops_; }
    HalMcuOps*       mcu()       const { return mcu_ops_; }
    HalEnvCtrlOps*   env_ctrl()  const { return env_ctrl_ops_; }
    HalAlarmOps*     alarm()     const { return alarm_ops_; }
    HalRs485Ops*     rs485()     const { return rs485_ops_; }
    HalAudioOps*     audio()     const { return audio_ops_; }
    HalInferenceOps*    inference()    const { return inference_ops_; }
    HalPostprocessOps*  postprocess()  const { return postprocess_ops_; }
    HalDspOps*          dsp()          const { return dsp_ops_; }
    HalFrameBufferOps*  frame_buffer() const { return frame_buffer_ops_; }
    void*            mcu_ctx()   const { return mcu_ctx_; }

    /** Set MCU context from an external owner (e.g. lens bridge). */
    void set_mcu_ctx(void* ctx) { mcu_ctx_ = ctx; }

private:
    void* video_handle_   = nullptr;
    void* codec_handle_   = nullptr;
    void* overlay_handle_ = nullptr;
    void* led_handle_     = nullptr;

    HalVideoOps*      video_ops_   = nullptr;
    HalCodecOps*      codec_ops_   = nullptr;
    HalOsdOps*        osd_ops_     = nullptr;
    HalDrawOps*       draw_ops_    = nullptr;
    HalMediaOps*      media_ops_   = nullptr;
    HalIspOps*        isp_ops_     = nullptr;
    HalLedOps*        led_ops_     = nullptr;
    HalSensorOps*     sensor_ops_  = nullptr;
    HalMcuOps*        mcu_ops_     = nullptr;
    HalEnvCtrlOps*    env_ctrl_ops_ = nullptr;
    HalAlarmOps*      alarm_ops_    = nullptr;
    HalRs485Ops*      rs485_ops_    = nullptr;
    HalAudioOps*      audio_ops_    = nullptr;
    HalInferenceOps*    inference_ops_    = nullptr;
    HalPostprocessOps*  postprocess_ops_  = nullptr;
    HalDspOps*          dsp_ops_          = nullptr;
    HalFrameBufferOps*  frame_buffer_ops_ = nullptr;
    void*             audio_handle_ = nullptr;
    void*             mcu_ctx_     = nullptr;

    bool load_one(const std::string& path, const char* symbol,
                  void** out_handle, void** out_ops);
};
