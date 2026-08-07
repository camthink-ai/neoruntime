/**
 * @file hal_loader.cpp
 * @brief HAL Dynamic Loader Implementation
 */

#include "../include/hal_loader.h"
#include <dlfcn.h>
#include <cstring>

extern "C" {
    #include "hal_log.h"
}

HalLoader::~HalLoader() {
    unload();
}

bool HalLoader::load(const std::string& video_lib,
                     const std::string& codec_lib,
                     const std::string& overlay_lib,
                     const std::string& led_lib,
                     const std::string& audio_lib) {
    // Video HAL is required
    if (!load_one(video_lib, "HAL_VIDEO_OPS",
                  &video_handle_, (void**)&video_ops_)) {
        HAL_LOG_ERROR("Failed to load video HAL: %s", video_lib.c_str());
        return false;
    }
    HAL_LOG_INFO("Loaded video HAL: %s", video_lib.c_str());
    if (video_ops_->get_version) {
        HAL_LOG_INFO("  Version: %s", video_ops_->get_version());
    }

    // Codec HAL is optional
    if (!codec_lib.empty()) {
        if (load_one(codec_lib, "HAL_CODEC_OPS",
                     &codec_handle_, (void**)&codec_ops_)) {
            HAL_LOG_INFO("Loaded codec HAL: %s", codec_lib.c_str());
            if (codec_ops_->get_version) {
                HAL_LOG_INFO("  Version: %s", codec_ops_->get_version());
            }
        } else {
            HAL_LOG_WARNING("Codec HAL not available, encoding disabled");
        }
    }

    // Load HAL_OSD_OPS from codec lib (same library)
    if (codec_handle_) {
        osd_ops_ = (HalOsdOps*)dlsym(codec_handle_, "HAL_OSD_OPS");
        if (osd_ops_) {
            HAL_LOG_INFO("Loaded OSD ops from codec HAL");
        } else {
            HAL_LOG_WARNING("HAL_OSD_OPS not found in codec HAL, OSD disabled");
        }
    }

    // Load HAL_DRAW_OPS from overlay lib
    if (!overlay_lib.empty()) {
        if (load_one(overlay_lib, "HAL_DRAW_OPS",
                     &overlay_handle_, (void**)&draw_ops_)) {
            HAL_LOG_INFO("Loaded draw HAL: %s", overlay_lib.c_str());
            if (draw_ops_->get_version) {
                HAL_LOG_INFO("  Version: %s", draw_ops_->get_version());
            }
        } else {
            HAL_LOG_WARNING("Draw HAL not available, AI overlay disabled");
        }
    }

    // Load HAL_MEDIA_OPS from video lib (same library for monolithic v2)
    if (video_handle_) {
        media_ops_ = (HalMediaOps*)dlsym(video_handle_, "HAL_MEDIA_OPS");
        if (media_ops_) {
            HAL_LOG_INFO("Loaded media HAL from video lib");
            if (media_ops_->get_version) {
                HAL_LOG_INFO("  Version: %s", media_ops_->get_version());
            }
        } else {
            HAL_LOG_INFO("HAL_MEDIA_OPS not found, media pipeline disabled");
        }

        // Load HAL_ISP_OPS from video lib (monolithic v2 build)
        isp_ops_ = (HalIspOps*)dlsym(video_handle_, "HAL_ISP_OPS");
        if (isp_ops_) {
            HAL_LOG_INFO("Loaded ISP HAL from video lib");
            if (isp_ops_->get_version) {
                HAL_LOG_INFO("  Version: %s", isp_ops_->get_version());
            }
        } else {
            HAL_LOG_INFO("HAL_ISP_OPS not found, ISP controls disabled");
        }

        // Try HAL_LED_OPS from video lib (monolithic build includes peripherals)
        led_ops_ = (HalLedOps*)dlsym(video_handle_, "HAL_LED_OPS");
        if (led_ops_) {
            HAL_LOG_INFO("Loaded LED/IR-cut HAL from video lib (monolithic)");
            if (led_ops_->get_version) {
                HAL_LOG_INFO("  Version: %s", led_ops_->get_version());
            }
        }
    }

    // Load LED/IR-cut HAL — separate library overrides monolithic dlsym
    if (!led_lib.empty()) {
        if (load_one(led_lib, "HAL_LED_OPS",
                     &led_handle_, (void**)&led_ops_)) {
            HAL_LOG_INFO("Loaded LED/IR-cut HAL: %s", led_lib.c_str());
            if (led_ops_->get_version) {
                HAL_LOG_INFO("  Version: %s", led_ops_->get_version());
            }
        } else {
            HAL_LOG_INFO("LED HAL library not found, IR-cut disabled");
        }
    }

    // LED ops will share MCU context with the lens bridge (set via set_mcu_ctx later).
    // Do NOT open a second serial port here — the MCU UART is single-owner.

    // Load HAL_SENSOR_OPS from video lib (monolithic build includes peripherals)
    if (video_handle_) {
        sensor_ops_ = (HalSensorOps*)dlsym(video_handle_, "HAL_SENSOR_OPS");
        if (sensor_ops_) {
            HAL_LOG_INFO("Loaded sensor HAL from video lib (monolithic)");
        }
    }

    // Load HAL_MCU_OPS from video lib (monolithic build)
    if (video_handle_) {
        mcu_ops_ = (HalMcuOps*)dlsym(video_handle_, "HAL_MCU_OPS");
        if (mcu_ops_) {
            HAL_LOG_INFO("Loaded MCU HAL from video lib (monolithic)");
        }
    }

    // Load HAL_ENV_CTRL_OPS from video lib (monolithic build)
    if (video_handle_) {
        env_ctrl_ops_ = (HalEnvCtrlOps*)dlsym(video_handle_, "HAL_ENV_CTRL_OPS");
        if (env_ctrl_ops_) {
            HAL_LOG_INFO("Loaded env_ctrl HAL from video lib (monolithic)");
        }
    }

    // Load HAL_ALARM_OPS from video lib (monolithic build)
    if (video_handle_) {
        alarm_ops_ = (HalAlarmOps*)dlsym(video_handle_, "HAL_ALARM_OPS");
        if (alarm_ops_) {
            HAL_LOG_INFO("Loaded alarm HAL from video lib (monolithic)");
        }
    }

    // Load HAL_RS485_OPS from video lib (monolithic build)
    if (video_handle_) {
        rs485_ops_ = (HalRs485Ops*)dlsym(video_handle_, "HAL_RS485_OPS");
        if (rs485_ops_) {
            HAL_LOG_INFO("Loaded RS485 HAL from video lib (monolithic)");
        }
    }

    // Load HAL_AUDIO_OPS — try monolithic (video lib) first, then dedicated library
    if (video_handle_) {
        audio_ops_ = (HalAudioOps*)dlsym(video_handle_, "HAL_AUDIO_OPS");
        if (audio_ops_) {
            HAL_LOG_INFO("Loaded audio HAL from video lib (monolithic)");
            if (audio_ops_->get_version) {
                HAL_LOG_INFO("  Version: %s", audio_ops_->get_version());
            }
        }
    }
    if (!audio_ops_ && !audio_lib.empty()) {
        if (load_one(audio_lib, "HAL_AUDIO_OPS",
                     &audio_handle_, (void**)&audio_ops_)) {
            HAL_LOG_INFO("Loaded audio HAL: %s", audio_lib.c_str());
            if (audio_ops_->get_version) {
                HAL_LOG_INFO("  Version: %s", audio_ops_->get_version());
            }
        } else {
            HAL_LOG_INFO("Audio HAL library not found, audio disabled");
        }
    }

    // Load DPM-related ops from video lib (monolithic v2 build).
    // These back the Dynamic Privacy Mask worker (detector + segmentation
    // inference, DSP ROI crop, reusable frame buffers). All optional — DPM
    // stays disabled if any is absent.
    if (video_handle_) {
        inference_ops_ = (HalInferenceOps*)dlsym(video_handle_, "HAL_INFERENCE_OPS");
        if (inference_ops_) {
            HAL_LOG_INFO("Loaded inference HAL from video lib (monolithic) — DPM enabled");
        }

        postprocess_ops_ = (HalPostprocessOps*)dlsym(video_handle_, "HAL_POSTPROCESS_OPS");
        if (postprocess_ops_) {
            HAL_LOG_INFO("Loaded postprocess HAL from video lib (monolithic)");
        }

        dsp_ops_ = (HalDspOps*)dlsym(video_handle_, "HAL_DSP_OPS");
        if (dsp_ops_) {
            HAL_LOG_INFO("Loaded DSP HAL from video lib (monolithic)");
        }

        frame_buffer_ops_ = (HalFrameBufferOps*)dlsym(video_handle_, "HAL_FRAME_BUFFER_OPS");
        if (frame_buffer_ops_) {
            HAL_LOG_INFO("Loaded frame_buffer HAL from video lib (monolithic)");
        }
    }

    return true;
}

void HalLoader::unload() {
    video_ops_ = nullptr;
    codec_ops_ = nullptr;
    osd_ops_  = nullptr;
    draw_ops_ = nullptr;
    media_ops_ = nullptr;
    isp_ops_   = nullptr;
    led_ops_  = nullptr;
    sensor_ops_ = nullptr;
    mcu_ops_  = nullptr;
    mcu_ctx_  = nullptr;
    env_ctrl_ops_ = nullptr;
    alarm_ops_    = nullptr;
    rs485_ops_    = nullptr;
    audio_ops_    = nullptr;
    inference_ops_    = nullptr;
    postprocess_ops_  = nullptr;
    dsp_ops_          = nullptr;
    frame_buffer_ops_ = nullptr;

    if (audio_handle_) {
        dlclose(audio_handle_);
        audio_handle_ = nullptr;
    }
    if (led_handle_) {
        dlclose(led_handle_);
        led_handle_ = nullptr;
    }
    if (overlay_handle_) {
        dlclose(overlay_handle_);
        overlay_handle_ = nullptr;
    }
    if (codec_handle_) {
        dlclose(codec_handle_);
        codec_handle_ = nullptr;
    }
    if (video_handle_) {
        dlclose(video_handle_);
        video_handle_ = nullptr;
    }
}

bool HalLoader::load_one(const std::string& path, const char* symbol,
                         void** out_handle, void** out_ops) {
    *out_handle = dlopen(path.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!*out_handle) {
        HAL_LOG_ERROR("dlopen(%s): %s", path.c_str(), dlerror());
        return false;
    }

    *out_ops = dlsym(*out_handle, symbol);
    if (!*out_ops) {
        HAL_LOG_ERROR("dlsym(%s, %s): %s", path.c_str(), symbol, dlerror());
        dlclose(*out_handle);
        *out_handle = nullptr;
        return false;
    }

    return true;
}
