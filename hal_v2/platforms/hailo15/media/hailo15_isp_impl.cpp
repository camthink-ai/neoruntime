/**
 * @file hailo15_isp_impl.cpp
 * @brief Hailo-15 HAL ISP — v4l2::v4l2ControlManager (aligned with webserver isp.cpp stream_params / controls).
 */

#include "common/hal_common.h"
#include "media/hal_isp.h"
#include "media/hal_video.h"
#include "media/hal_video_internal.h"

#include "hailo15_media_priv.hpp"
#include "hailo15_video_ml.hpp"
#include <hailo/media_library/media_library_api_types.hpp>
#include <hailo/media_library/v4l2_ctrl.hpp>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <mutex>
#include <optional>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <vector>
#include <linux/videodev2.h>

namespace
{

v4l2::v4l2ControlManager &isp_ctrl_mgr()
{
    /* Use default-constructed control manager, same as V1 HAL/video path. */
    static v4l2::v4l2ControlManager m{};
    return m;
}

int map_pwr_freq(HalIspPowerFreq f)
{
    switch (f)
    {
        case HAL_ISP_PWR_FREQ_OFF:
            return 0;
        case HAL_ISP_PWR_FREQ_50HZ:
            return 1;
        case HAL_ISP_PWR_FREQ_60HZ:
            return 2;
        default:
            return 0;
    }
}

HalIspPowerFreq pwr_freq_from_driver(int32_t v)
{
    switch (v)
    {
        case 1:
            return HAL_ISP_PWR_FREQ_50HZ;
        case 2:
            return HAL_ISP_PWR_FREQ_60HZ;
        default:
            return HAL_ISP_PWR_FREQ_OFF;
    }
}

static Hailo15MediaPriv *media_priv_from_video_ctx(void *video_ctx)
{
    auto *vc = static_cast<HalVideoContext *>(video_ctx);
    if (!vc || vc->config.type != HAL_VIDEO_TYPE_FROM_MEDIA || vc->config.media_ptr == nullptr)
    {
        return nullptr;
    }
    return hailo15_media_priv_from_hal(vc->config.media_ptr);
}

/* V1 hal/media/hailo15/video_impl.cpp — maps backlight % to adaptive_ae.wdrContrast min/max. */
static void backlight_wdr_levels_from_percentage(uint16_t baseline_max, uint16_t baseline_min, uint16_t percentage,
                                                 uint16_t *out_max, uint16_t *out_min)
{
    constexpr float kAbsoluteMaxLevel = 255.0f;
    constexpr float kMaxCombinedLevel = kAbsoluteMaxLevel * 2.0f;

    const float current_backlight =
        1.0f - (static_cast<float>(baseline_max) + static_cast<float>(baseline_min)) / kMaxCombinedLevel;
    const float target_backlight = static_cast<float>(percentage) / 100.0f;
    float scale_factor = 1.0f;
    if (std::fabs(1.0f - current_backlight) > 1e-5f)
    {
        scale_factor = (1.0f - target_backlight) / (1.0f - current_backlight);
    }

    int new_max_level = static_cast<int>(static_cast<float>(baseline_max) * scale_factor);
    int new_min_level = static_cast<int>(static_cast<float>(baseline_min) * scale_factor);
    const int new_sum = new_max_level + new_min_level;
    if (new_max_level > static_cast<int>(kAbsoluteMaxLevel))
    {
        new_max_level = static_cast<int>(kAbsoluteMaxLevel);
        new_min_level = new_sum - new_max_level;
    }
    if (new_min_level < 0)
    {
        new_min_level = 0;
    }
    if (new_max_level < 0)
    {
        new_max_level = 0;
    }
    *out_max = static_cast<uint16_t>(new_max_level);
    *out_min = static_cast<uint16_t>(new_min_level);
}

/**
 * Webserver isp.cpp Post /isp/powerline_frequency: updates ioctl and adaptive_ae.flicker_period
 * (and hdr_adaptive_ae) so adaptive AE and V4L2 stay consistent.
 */
static void ml_sync_flicker_period(Hailo15MediaPriv *priv, uint16_t ioctl_freq_0_2)
{
    if (!priv || !priv->media_lib)
    {
        return;
    }
    try
    {
        /* Do not hold priv->mutex across MediaLibrary calls: ML may invoke callbacks that take this lock. */
        auto prof_exp = priv->media_lib->get_current_profile();
        if (!prof_exp.has_value())
        {
            return;
        }
        config_profile_t p = prof_exp.value();
        auto &aac = p.iq_settings.automatic_algorithms_config;
        aac.adaptive_ae.flicker_period = ioctl_freq_0_2;
        aac.hdr_adaptive_ae.flicker_period = ioctl_freq_0_2;
        const media_library_return r = priv->media_lib->set_override_parameters(p);
        if (r != MEDIA_LIBRARY_SUCCESS)
        {
            HAL_LOG_WARNING("Hailo15 ISP: set_override_parameters (flicker_period) failed (%d)", static_cast<int>(r));
        }
        else if (priv->hal_media_ctx != nullptr)
        {
            std::lock_guard<std::recursive_mutex> lock(priv->mutex);
            hailo15::video_ml::refresh_all_context_configs(priv, priv->hal_media_ctx);
        }
    }
    catch (const std::exception &e)
    {
        HAL_LOG_WARNING("Hailo15 ISP: ml_sync_flicker_period: %s", e.what());
    }
    catch (...)
    {
        HAL_LOG_WARNING("Hailo15 ISP: ml_sync_flicker_period: unknown exception");
    }
}

/** V1 apply_image_config_to_isp: backlight updates profile adaptive_ae.wdrContrast when media_lib is set. */
static void ml_sync_backlight_wdr(Hailo15MediaPriv *priv, int backlight_0_100)
{
    if (!priv || !priv->media_lib)
    {
        return;
    }
    try
    {
        /* Do not hold priv->mutex across MediaLibrary calls: ML may invoke callbacks that take this lock. */
        auto prof_exp = priv->media_lib->get_current_profile();
        if (!prof_exp.has_value())
        {
            return;
        }
        config_profile_t p = prof_exp.value();
        auto &aae = p.iq_settings.automatic_algorithms_config.adaptive_ae;
        const uint16_t mx = aae.wdrContrast.max;
        const uint16_t mn = aae.wdrContrast.min;
        const uint16_t pct = static_cast<uint16_t>(std::clamp(backlight_0_100, 0, 100));
        uint16_t wdr_max = 0;
        uint16_t wdr_min = 0;
        backlight_wdr_levels_from_percentage(mx, mn, pct, &wdr_max, &wdr_min);
        p.iq_settings.automatic_algorithms_config.adaptive_ae.wdrContrast.max = wdr_max;
        p.iq_settings.automatic_algorithms_config.adaptive_ae.wdrContrast.min = wdr_min;
        const media_library_return r = priv->media_lib->set_override_parameters(p);
        if (r != MEDIA_LIBRARY_SUCCESS)
        {
            HAL_LOG_WARNING("Hailo15 ISP: set_override_parameters (backlight wdrContrast) failed (%d)",
                            static_cast<int>(r));
        }
        else if (priv->hal_media_ctx != nullptr)
        {
            std::lock_guard<std::recursive_mutex> lock(priv->mutex);
            hailo15::video_ml::refresh_all_context_configs(priv, priv->hal_media_ctx);
        }
    }
    catch (const std::exception &e)
    {
        HAL_LOG_WARNING("Hailo15 ISP: ml_sync_backlight_wdr: %s", e.what());
    }
    catch (...)
    {
        HAL_LOG_WARNING("Hailo15 ISP: ml_sync_backlight_wdr: unknown exception");
    }
}

/**
 * Webserver pipeline/isp_blender.cpp IspBlender::set_auto_configs():
 * toggles ae_e_v1 / a_cproc / aw_drv4 in iq_settings.automatic_algorithms_config via set_override_parameters.
 * When HAL "manual" picture tuning is on, those must be disabled (enabled=false) so V4L2 stream_params apply.
 * Mirrors hal V1 video_impl set_image_config auto-config path.
 */
static bool s_ml_ae_cproc_aw_drv_enabled = true;

static void ml_apply_isp_auto_algorithm_blocks(Hailo15MediaPriv *priv, bool enable_ae_cproc_aw_drv)
{
    if (!priv || !priv->media_lib)
    {
        return;
    }
    if (s_ml_ae_cproc_aw_drv_enabled == enable_ae_cproc_aw_drv)
    {
        return;
    }
    /* V1 video_impl: brief delay before disabling auto blocks when entering manual (AE converge). */
    if (s_ml_ae_cproc_aw_drv_enabled && !enable_ae_cproc_aw_drv)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    try
    {
        /* Do not hold priv->mutex across MediaLibrary calls: ML may invoke callbacks that take this lock. */
        auto prof_exp = priv->media_lib->get_current_profile();
        if (!prof_exp.has_value())
        {
            HAL_LOG_WARNING("Hailo15 ISP: get_current_profile (isp auto blocks) failed");
            return;
        }
        config_profile_t p = prof_exp.value();
        auto &aac = p.iq_settings.automatic_algorithms_config;
        aac.ae_e_v1.disable = false;
        aac.a_cproc.disable = false;
        aac.aw_drv4.disable = false;
        aac.ae_e_v1.enabled = enable_ae_cproc_aw_drv;
        aac.a_cproc.enabled = enable_ae_cproc_aw_drv;
        aac.aw_drv4.enabled = enable_ae_cproc_aw_drv;
        const media_library_return r = priv->media_lib->set_override_parameters(p);
        if (r != MEDIA_LIBRARY_SUCCESS)
        {
            HAL_LOG_WARNING("Hailo15 ISP: set_override_parameters (ae_e_v1/a_cproc/aw_drv4) failed (%d)",
                            static_cast<int>(r));
            return;
        }
        s_ml_ae_cproc_aw_drv_enabled = enable_ae_cproc_aw_drv;
        if (priv->hal_media_ctx != nullptr)
        {
            std::lock_guard<std::recursive_mutex> lock(priv->mutex);
            hailo15::video_ml::refresh_all_context_configs(priv, priv->hal_media_ctx);
        }
    }
    catch (const std::exception &e)
    {
        HAL_LOG_WARNING("Hailo15 ISP: ml_apply_isp_auto_algorithm_blocks: %s", e.what());
    }
    catch (...)
    {
        HAL_LOG_WARNING("Hailo15 ISP: ml_apply_isp_auto_algorithm_blocks: unknown exception");
    }
}

/**
 * AWB mode / illuminant index are V4L2 controls (isp_awb_mode, isp_awb_illum_index).
 *
 * Do NOT use MediaLibrary::set_automatic_algorithm_configuration for AWB: that API validates against
 * CONFIG_SCHEMA_AUTOMATIC_ALGORITHMS (tuning blocks only, e.g. AWdrv4). The exported profile may add a
 * lowercase "aw_drv4" object with isp_awb_* for display; those keys are not accepted by that schema.
 *
 * Tuning blocks (ae_e_v1 / a_cproc / aw_drv4.enabled) are toggled via set_override_parameters in
 * ml_apply_isp_auto_algorithm_blocks — same as webserver IspBlender::set_auto_configs (see hal_v2/readme.md).
 */

/** Parse aw_drv4 isp_awb_* from iq_settings JSON string (single snapshot). */
static bool ml_parse_awb_idx_from_profile_json_string(const std::string &json_in, int *out_awb_idx)
{
    if (!out_awb_idx)
    {
        return false;
    }
    try
    {
        const nlohmann::json root = nlohmann::json::parse(json_in, nullptr, false);
        if (root.is_discarded() || !root.contains("iq_settings") || !root["iq_settings"].is_object())
        {
            return false;
        }
        const nlohmann::json &iq = root["iq_settings"];
        const nlohmann::json *aac = nullptr;
        if (iq.contains("automatic_algorithms") && iq["automatic_algorithms"].is_object())
        {
            aac = &iq["automatic_algorithms"];
        }
        else if (iq.contains("automatic_algorithms_config") && iq["automatic_algorithms_config"].is_object())
        {
            aac = &iq["automatic_algorithms_config"];
        }
        else
        {
            return false;
        }
        if (!aac->contains("aw_drv4") || !(*aac)["aw_drv4"].is_object())
        {
            return false;
        }
        const nlohmann::json &aw = (*aac)["aw_drv4"];
        if (!aw.contains("isp_awb_mode"))
        {
            return false;
        }
        const int mode = aw["isp_awb_mode"].get<int>();
        if (mode == 1)
        {
            *out_awb_idx = -1;
            return true;
        }
        if (aw.contains("isp_awb_illum_index"))
        {
            *out_awb_idx = aw["isp_awb_illum_index"].get<int>();
        }
        else
        {
            *out_awb_idx = 0;
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

static bool ml_get_awb_idx_from_profile_json(Hailo15MediaPriv *priv, int *out_awb_idx)
{
    if (!priv || !priv->media_lib || !out_awb_idx)
    {
        return false;
    }
    try
    {
        /*
         * Same idea as exposure GET: first profile snapshot can lag before ML has written aw_drv4;
         * re-read a few times so isp_awb_mode / isp_awb_illum_index match subsequent reads.
         */
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            if (attempt > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            std::string json_in;
            /* Do not hold priv->mutex across MediaLibrary calls. */
            const auto s = priv->media_lib->get_current_profile_str();
            if (!s.has_value())
            {
                continue;
            }
            json_in = s.value();
            if (ml_parse_awb_idx_from_profile_json_string(json_in, out_awb_idx))
            {
                return true;
            }
        }
        return false;
    }
    catch (...)
    {
        return false;
    }
}

/** Fill awb_profile_list / awb_profile_count from Awbv2.illuorder and/or aw_drv4 arrays (FROM_MEDIA). */
static void ml_fill_awb_profile_list(Hailo15MediaPriv *ml_priv, HalIspImageConfig *config)
{
    config->awb_profile_list = nullptr;
    config->awb_profile_count = 0;
    if (!ml_priv || !ml_priv->media_lib || !config)
    {
        return;
    }
    static constexpr size_t k_max = 48;
    static constexpr size_t k_nlen = 128;
    static char s_name_bufs[k_max][k_nlen];
    static const char *s_name_ptrs[k_max];

    try
    {
        std::string json_in;
        /* Do not hold ml_priv->mutex across MediaLibrary calls. */
        const auto s = ml_priv->media_lib->get_current_profile_str();
        if (!s.has_value())
        {
            return;
        }
        json_in = s.value();
        const nlohmann::json root = nlohmann::json::parse(json_in, nullptr, false);
        if (root.is_discarded() || !root.contains("iq_settings") || !root["iq_settings"].is_object())
        {
            return;
        }
        const nlohmann::json &iq = root["iq_settings"];
        const nlohmann::json *aac = nullptr;
        if (iq.contains("automatic_algorithms") && iq["automatic_algorithms"].is_object())
        {
            aac = &iq["automatic_algorithms"];
        }
        else if (iq.contains("automatic_algorithms_config") && iq["automatic_algorithms_config"].is_object())
        {
            aac = &iq["automatic_algorithms_config"];
        }
        else
        {
            return;
        }

        static const char *k_awbv2_keys[] = {"Awbv2", "awbv2"};
        for (size_t ki = 0; ki < sizeof(k_awbv2_keys) / sizeof(k_awbv2_keys[0]); ++ki)
        {
            const char *const k_awbv2 = k_awbv2_keys[ki];
            if (!aac->contains(k_awbv2) || !(*aac)[k_awbv2].is_object())
            {
                continue;
            }
            const nlohmann::json &awbv2 = (*aac)[k_awbv2];
            if (!awbv2.contains("illuorder") || !awbv2["illuorder"].is_string())
            {
                continue;
            }
            const std::string order = awbv2["illuorder"].get<std::string>();
            size_t n = 0;
            size_t start = 0;
            while (start < order.size() && n < k_max)
            {
                const size_t comma = order.find(',', start);
                std::string part =
                    (comma == std::string::npos) ? order.substr(start) : order.substr(start, comma - start);
                while (!part.empty() && (part.front() == ' ' || part.front() == '\t'))
                {
                    part.erase(0, 1);
                }
                while (!part.empty() && (part.back() == ' ' || part.back() == '\t'))
                {
                    part.pop_back();
                }
                if (!part.empty())
                {
                    (void)std::snprintf(s_name_bufs[n], k_nlen, "%s", part.c_str());
                    s_name_ptrs[n] = s_name_bufs[n];
                    ++n;
                }
                if (comma == std::string::npos)
                {
                    break;
                }
                start = comma + 1;
            }
            if (n > 0)
            {
                config->awb_profile_count = static_cast<uint32_t>(n);
                config->awb_profile_list = s_name_ptrs;
                return;
            }
        }

        if (!aac->contains("aw_drv4") || !(*aac)["aw_drv4"].is_object())
        {
            return;
        }
        const nlohmann::json &aw = (*aac)["aw_drv4"];
        const nlohmann::json *arr = nullptr;
        static const char *k_arr_keys[] = {"illuminant_profiles", "illuminants", "illuminant_names",
                                           "manual_illuminants", "preset_illuminants"};
        for (const char *k : k_arr_keys)
        {
            if (aw.contains(k) && aw[k].is_array())
            {
                arr = &aw[k];
                break;
            }
        }
        if (arr == nullptr)
        {
            return;
        }

        size_t n = 0;
        for (const auto &el : *arr)
        {
            if (n >= k_max)
            {
                break;
            }
            std::string name;
            if (el.is_string())
            {
                name = el.get<std::string>();
            }
            else if (el.is_object())
            {
                if (el.contains("name") && el["name"].is_string())
                {
                    name = el["name"].get<std::string>();
                }
                else if (el.contains("id") && el["id"].is_string())
                {
                    name = el["id"].get<std::string>();
                }
            }
            if (name.empty())
            {
                continue;
            }
            (void)std::snprintf(s_name_bufs[n], k_nlen, "%s", name.c_str());
            s_name_ptrs[n] = s_name_bufs[n];
            ++n;
        }
        if (n == 0)
        {
            return;
        }
        config->awb_profile_count = static_cast<uint32_t>(n);
        config->awb_profile_list = s_name_ptrs;
    }
    catch (...)
    {
        config->awb_profile_list = nullptr;
        config->awb_profile_count = 0;
    }
}

template<typename CtrlEnum, typename T>
bool safe_get(v4l2::v4l2ControlManager &m, CtrlEnum ctrl, T &value)
{
    try
    {
        return m.get<T>(ctrl, value);
    }
    catch (const std::exception &e)
    {
        /* Missing ctrl in map is common; avoid spamming WARNING on optional reads (e.g. POWERLINE_FREQUENCY). */
        HAL_LOG_DEBUG("Hailo15 ISP: get ctrl (%d) failed: %s", static_cast<int>(ctrl), e.what());
        return false;
    }
    catch (...)
    {
        HAL_LOG_DEBUG("Hailo15 ISP: get ctrl (%d) failed: unknown exception", static_cast<int>(ctrl));
        return false;
    }
}

template<typename CtrlEnum, typename T>
bool safe_set(v4l2::v4l2ControlManager &m, CtrlEnum ctrl, const T &value)
{
    try
    {
        return m.ctrl_set<T>(ctrl, value);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_WARNING("Hailo15 ISP: set ctrl (%d) failed: %s", static_cast<int>(ctrl), e.what());
        return false;
    }
    catch (...)
    {
        HAL_LOG_WARNING("Hailo15 ISP: set ctrl (%d) failed: unknown exception", static_cast<int>(ctrl));
        return false;
    }
}

/** V1 / Media Library path uses ext_ctrl_set for ISP controls; ctrl_set often does not apply. */
template<typename CtrlEnum, typename T>
bool safe_ext_ctrl_set(v4l2::v4l2ControlManager &m, CtrlEnum ctrl, const T &value)
{
    try
    {
        return m.ext_ctrl_set(ctrl, value);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_WARNING("Hailo15 ISP: ext_ctrl_set (%d) failed: %s", static_cast<int>(ctrl), e.what());
        return false;
    }
    catch (...)
    {
        HAL_LOG_WARNING("Hailo15 ISP: ext_ctrl_set (%d) failed: unknown exception", static_cast<int>(ctrl));
        return false;
    }
}

/** Ext-control read (e.g. WB gains); returns false when the control is absent. */
template<typename CtrlEnum, typename T>
bool safe_ext_ctrl_get(v4l2::v4l2ControlManager &m, CtrlEnum ctrl, T &value)
{
    try
    {
        auto opt = m.ext_ctrl_get<T>(ctrl);
        if (!opt.has_value())
        {
            return false;
        }
        value = opt.value();
        return true;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_DEBUG("Hailo15 ISP: ext_ctrl_get (%d) failed: %s", static_cast<int>(ctrl), e.what());
        return false;
    }
    catch (...)
    {
        HAL_LOG_DEBUG("Hailo15 ISP: ext_ctrl_get (%d) failed: unknown exception", static_cast<int>(ctrl));
        return false;
    }
}

/** Controls that may be absent from medialib v4l2 maps (e.g. NOISE_REDUCTION): no WARNING spam. */
template<typename CtrlEnum, typename T>
bool safe_ext_ctrl_set_optional(v4l2::v4l2ControlManager &m, CtrlEnum ctrl, const T &value)
{
    try
    {
        return m.ext_ctrl_set(ctrl, value);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_DEBUG("Hailo15 ISP: ext_ctrl_set optional (%d) skipped: %s", static_cast<int>(ctrl), e.what());
        return false;
    }
    catch (...)
    {
        HAL_LOG_DEBUG("Hailo15 ISP: ext_ctrl_set optional (%d) skipped", static_cast<int>(ctrl));
        return false;
    }
}

/**
 * AWB from V4L2 with short settle retries. Uses the **last** successful sample so the first read is not
 * stuck on a stale value (v4l2 ctrl cache / driver not ready — not a separate HAL cache).
 */
static bool get_awb_idx_from_v4l2_with_retry(v4l2::v4l2ControlManager &m, int *out_awb_idx)
{
    if (!out_awb_idx)
    {
        return false;
    }
    bool any = false;
    int last = 0;
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        if (attempt > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        int32_t awb_mode = 0;
        if (!safe_get(m, v4l2::Video0Ctrl::AWB_MODE, awb_mode))
        {
            continue;
        }
        any = true;
        if (awb_mode == 1)
        {
            last = -1;
        }
        else
        {
            int32_t idx = 0;
            if (safe_get(m, v4l2::Video0Ctrl::AWB_ILLUM_INDEX, idx))
            {
                last = static_cast<int>(idx);
            }
            else
            {
                last = 0;
            }
        }
    }
    if (any)
    {
        *out_awb_idx = last;
        return true;
    }
    return false;
}

static std::optional<uint32_t> v4l2_find_ctrl_id_by_name(int fd, const char *ctrl_name)
{
    if (fd < 0 || ctrl_name == nullptr || ctrl_name[0] == '\0')
    {
        return std::nullopt;
    }
    const unsigned next_flag = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    struct v4l2_query_ext_ctrl qctrl;
    std::memset(&qctrl, 0, sizeof(qctrl));
    qctrl.id = next_flag;
    while (true)
    {
        const int ret = ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &qctrl);
        if (ret < 0)
        {
            return std::nullopt;
        }
        if (0 == std::strcmp(reinterpret_cast<const char *>(qctrl.name), ctrl_name))
        {
            return qctrl.id;
        }
        qctrl.id |= next_flag;
    }
}

static bool v4l2_get_i32_ctrl_by_id(int fd, uint32_t ctrl_id, int32_t *out_value)
{
    if (fd < 0 || out_value == nullptr)
    {
        return false;
    }
    struct v4l2_ext_control ctrl;
    struct v4l2_ext_controls ctrls;
    std::memset(&ctrl, 0, sizeof(ctrl));
    std::memset(&ctrls, 0, sizeof(ctrls));
    ctrl.id = ctrl_id;
    ctrl.size = sizeof(int32_t);
    ctrls.count = 1;
    ctrls.controls = &ctrl;
    ctrls.which = V4L2_CTRL_ID2WHICH(ctrl.id);
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) < 0)
    {
        return false;
    }
    *out_value = static_cast<int32_t>(ctrl.value);
    return true;
}

static bool v4l2_query_ext_ctrl_by_exact_name(int fd, const char *exact_name, struct v4l2_query_ext_ctrl *out)
{
    if (fd < 0 || exact_name == nullptr || out == nullptr)
    {
        return false;
    }
    const unsigned next_flag = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    struct v4l2_query_ext_ctrl qctrl;
    std::memset(&qctrl, 0, sizeof(qctrl));
    qctrl.id = next_flag;
    while (true)
    {
        if (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) < 0)
        {
            return false;
        }
        if (0 == std::strcmp(reinterpret_cast<const char *>(qctrl.name), exact_name))
        {
            *out = qctrl;
            return true;
        }
        qctrl.id |= next_flag;
    }
}

// Search ALL V4L2 controls (not just compound) — needed for simple integer controls like brightness
static bool v4l2_query_ext_ctrl_by_name_all(int fd, const char *exact_name, struct v4l2_query_ext_ctrl *out)
{
    if (fd < 0 || exact_name == nullptr || out == nullptr) return false;
    const unsigned next_flag = V4L2_CTRL_FLAG_NEXT_CTRL;
    struct v4l2_query_ext_ctrl qctrl;
    std::memset(&qctrl, 0, sizeof(qctrl));
    qctrl.id = next_flag;
    while (true) {
        if (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) < 0) return false;
        if (0 == std::strcmp(reinterpret_cast<const char *>(qctrl.name), exact_name)) {
            *out = qctrl;
            return true;
        }
        qctrl.id |= next_flag;
    }
}

static bool v4l2_query_ext_ctrl_noise_like(int fd, struct v4l2_query_ext_ctrl *out)
{
    if (fd < 0 || out == nullptr)
    {
        return false;
    }
    const unsigned next_flag = V4L2_CTRL_FLAG_NEXT_CTRL | V4L2_CTRL_FLAG_NEXT_COMPOUND;
    struct v4l2_query_ext_ctrl qctrl;
    std::memset(&qctrl, 0, sizeof(qctrl));
    qctrl.id = next_flag;
    while (true)
    {
        if (ioctl(fd, VIDIOC_QUERY_EXT_CTRL, &qctrl) < 0)
        {
            return false;
        }
        std::string name(reinterpret_cast<const char *>(qctrl.name));
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const bool looks_noise = (lower.find("noise") != std::string::npos) || (lower.find("nois") != std::string::npos);
        const bool looks_reduction =
            (lower.find("reduc") != std::string::npos) || (lower.find("denois") != std::string::npos);
        if (looks_noise && looks_reduction)
        {
            *out = qctrl;
            return true;
        }
        qctrl.id |= next_flag;
    }
}

static bool v4l2_set_ext_ctrl_from_i64(int fd, const struct v4l2_query_ext_ctrl &info, int64_t in_value)
{
    struct v4l2_ext_control ctrl;
    struct v4l2_ext_controls ctrls;
    std::memset(&ctrl, 0, sizeof(ctrl));
    std::memset(&ctrls, 0, sizeof(ctrls));
    ctrl.id = info.id;
    ctrls.count = 1;
    ctrls.controls = &ctrl;
    ctrls.which = V4L2_CTRL_ID2WHICH(ctrl.id);

    int64_t value = in_value;
    if (value < info.minimum)
    {
        value = info.minimum;
    }
    if (value > info.maximum)
    {
        value = info.maximum;
    }

    std::vector<uint8_t> buffer;
    if (info.type == V4L2_CTRL_TYPE_INTEGER || info.type == V4L2_CTRL_TYPE_BOOLEAN)
    {
        ctrl.value = static_cast<decltype(ctrl.value)>(value);
    }
    else if (info.type == V4L2_CTRL_TYPE_INTEGER64)
    {
        ctrl.value64 = static_cast<decltype(ctrl.value64)>(value);
    }
    else
    {
        size_t count = static_cast<size_t>(info.elems);
        if (count == 0)
        {
            count = 1;
        }
        size_t total = static_cast<size_t>(info.elem_size) * count;
        if (total == 0)
        {
            total = static_cast<size_t>(info.elem_size);
        }
        buffer.resize(total, 0);
        ctrl.ptr = buffer.data();
        ctrl.size = static_cast<decltype(ctrl.size)>(buffer.size());
        const bool signed_elem = info.minimum < 0;
        if (info.elem_size == 2)
        {
            if (signed_elem)
            {
                auto *p = reinterpret_cast<int16_t *>(buffer.data());
                for (size_t i = 0; i < count; ++i)
                {
                    p[i] = static_cast<int16_t>(value);
                }
            }
            else
            {
                auto *p = reinterpret_cast<uint16_t *>(buffer.data());
                for (size_t i = 0; i < count; ++i)
                {
                    p[i] = static_cast<uint16_t>(value);
                }
            }
        }
        else if (info.elem_size == 4)
        {
            if (signed_elem)
            {
                auto *p = reinterpret_cast<int32_t *>(buffer.data());
                for (size_t i = 0; i < count; ++i)
                {
                    p[i] = static_cast<int32_t>(value);
                }
            }
            else
            {
                auto *p = reinterpret_cast<uint32_t *>(buffer.data());
                for (size_t i = 0; i < count; ++i)
                {
                    p[i] = static_cast<uint32_t>(value);
                }
            }
        }
        else if (info.elem_size == 1)
        {
            uint8_t *p = buffer.data();
            for (size_t i = 0; i < count; ++i)
            {
                p[i] = static_cast<uint8_t>(value);
            }
        }
    }

    return ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls) >= 0;
}

static bool v4l2_read_ext_ctrl_as_i64(int fd, const struct v4l2_query_ext_ctrl &info, int64_t *out_value)
{
    if (fd < 0 || out_value == nullptr)
    {
        return false;
    }
    struct v4l2_ext_control ctrl;
    struct v4l2_ext_controls ctrls;
    std::memset(&ctrl, 0, sizeof(ctrl));
    std::memset(&ctrls, 0, sizeof(ctrls));
    ctrl.id = info.id;
    ctrls.count = 1;
    ctrls.controls = &ctrl;
    ctrls.which = V4L2_CTRL_ID2WHICH(ctrl.id);

    std::vector<uint8_t> buffer;
    if (info.type == V4L2_CTRL_TYPE_INTEGER || info.type == V4L2_CTRL_TYPE_BOOLEAN)
    {
        ctrl.value = 0;
    }
    else if (info.type == V4L2_CTRL_TYPE_INTEGER64)
    {
        ctrl.value64 = 0;
    }
    else
    {
        size_t total = static_cast<size_t>(info.elem_size) * static_cast<size_t>(info.elems);
        if (total == 0)
        {
            total = static_cast<size_t>(info.elem_size);
        }
        buffer.resize(total, 0);
        ctrl.ptr = buffer.data();
        ctrl.size = static_cast<decltype(ctrl.size)>(buffer.size());
    }

    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) < 0)
    {
        return false;
    }

    if (info.type == V4L2_CTRL_TYPE_INTEGER || info.type == V4L2_CTRL_TYPE_BOOLEAN)
    {
        *out_value = static_cast<int64_t>(ctrl.value);
        return true;
    }
    if (info.type == V4L2_CTRL_TYPE_INTEGER64)
    {
        *out_value = static_cast<int64_t>(ctrl.value64);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* AF window + AF measurement controls (best-effort by ctrl name)       */
/* ------------------------------------------------------------------ */

static std::mutex s_af_mu;
static bool s_af_win_ctrl_valid = false;
static struct v4l2_query_ext_ctrl s_af_win_ctrl_info {};
static bool s_af_enable_ctrl_valid = false;
static struct v4l2_query_ext_ctrl s_af_enable_ctrl_info {};
static bool s_af_meas_ctrl_valid = false;
static struct v4l2_query_ext_ctrl s_af_meas_ctrl_info {};

static bool resolve_af_enable_ctrl(int fd)
{
    std::lock_guard<std::mutex> lk(s_af_mu);
    if (s_af_enable_ctrl_valid)
    {
        return true;
    }
    static const char *k_try_names[] = {
        "isp_af_enable",
        "af_enable",
    };
    for (const char *nm : k_try_names)
    {
        if (v4l2_query_ext_ctrl_by_exact_name(fd, nm, &s_af_enable_ctrl_info))
        {
            s_af_enable_ctrl_valid = true;
            return true;
        }
    }
    return false;
}

static bool resolve_af_window_ctrl(int fd)
{
    std::lock_guard<std::mutex> lk(s_af_mu);
    if (s_af_win_ctrl_valid)
    {
        return true;
    }
    static const char *k_try_names[] = {
        "isp_af_window",
        "isp_afm_windows",
        "af_windows",
        "af_window",
    };
    for (const char *nm : k_try_names)
    {
        if (v4l2_query_ext_ctrl_by_exact_name(fd, nm, &s_af_win_ctrl_info))
        {
            s_af_win_ctrl_valid = true;
            return true;
        }
    }
    return false;
}

static bool resolve_af_measurement_ctrl(int fd)
{
    std::lock_guard<std::mutex> lk(s_af_mu);
    if (s_af_meas_ctrl_valid)
    {
        return true;
    }
    static const char *k_try_names[] = {
        "isp_af_measurement",
        "isp_af_stats",
        "isp_afm_stats",
        "af_measurement",
        "afm_measurement",
    };
    for (const char *nm : k_try_names)
    {
        if (v4l2_query_ext_ctrl_by_exact_name(fd, nm, &s_af_meas_ctrl_info))
        {
            s_af_meas_ctrl_valid = true;
            return true;
        }
    }
    return false;
}

/* ---- M2: AE statistics controls (isp_ae_hist / isp_ae_luma) ---- */
static std::mutex s_ae_mu;
static bool s_ae_hist_valid = false;
static bool s_ae_luma_valid = false;
static struct v4l2_query_ext_ctrl s_ae_hist_info {};
static struct v4l2_query_ext_ctrl s_ae_luma_info {};
static bool s_ae_en_valid = false;
static bool s_ae_win_valid = false;
static struct v4l2_query_ext_ctrl s_ae_en_info {};
static struct v4l2_query_ext_ctrl s_ae_win_info {};

static bool resolve_ae_enable_win_ctrls(int fd)
{
    std::lock_guard<std::mutex> lk(s_ae_mu);
    if (!s_ae_en_valid)
    {
        s_ae_en_valid = v4l2_query_ext_ctrl_by_exact_name(fd, "isp_ae_enable", &s_ae_en_info);
    }
    if (!s_ae_win_valid)
    {
        s_ae_win_valid = v4l2_query_ext_ctrl_by_exact_name(fd, "isp_ae_hist_window", &s_ae_win_info);
    }
    return s_ae_en_valid || s_ae_win_valid;
}

static bool resolve_ae_stats_ctrls(int fd)
{
    std::lock_guard<std::mutex> lk(s_ae_mu);
    if (s_ae_hist_valid || s_ae_luma_valid)
    {
        return true;
    }
    static const char *k_hist_names[] = {"isp_ae_hist", "isp_ae_histogram", "ae_hist"};
    for (const char *nm : k_hist_names)
    {
        if (v4l2_query_ext_ctrl_by_exact_name(fd, nm, &s_ae_hist_info))
        {
            s_ae_hist_valid = true;
            break;
        }
    }
    static const char *k_luma_names[] = {"isp_ae_luma", "isp_ae_average_luma_grid", "ae_luma"};
    for (const char *nm : k_luma_names)
    {
        if (v4l2_query_ext_ctrl_by_exact_name(fd, nm, &s_ae_luma_info))
        {
            s_ae_luma_valid = true;
            break;
        }
    }
    return s_ae_hist_valid || s_ae_luma_valid;
}

/* ---- M2: AWB enable control (manual/auto switch; absent from medialib v4l2 map) ---- */
static std::mutex s_awb_en_mu;
static bool s_awb_en_valid = false;
static struct v4l2_query_ext_ctrl s_awb_en_info {};

static bool resolve_awb_enable_ctrl(int fd)
{
    std::lock_guard<std::mutex> lk(s_awb_en_mu);
    if (s_awb_en_valid)
    {
        return true;
    }
    if (v4l2_query_ext_ctrl_by_exact_name(fd, "isp_awb_enable", &s_awb_en_info))
    {
        s_awb_en_valid = true;
        return true;
    }
    return false;
}

/* ---- M2: 3DNR (temporal NR) control probing ---- */
static std::mutex s_3dnr_mu;
static bool s_3dnr_ctrl_valid = false;
static struct v4l2_query_ext_ctrl s_3dnr_ctrl_info {};

static bool s_3dnr_en_valid = false;
static struct v4l2_query_ext_ctrl s_3dnr_en_info {};

static bool resolve_3dnr_ctrl(int fd)
{
    std::lock_guard<std::mutex> lk(s_3dnr_mu);
    if (!s_3dnr_ctrl_valid)
    {
        static const char *k_try_names[] = {
            "isp_3dnr_strength",
            "isp_3dnr",
            "isp_tnr_strength",
            "3dnr_strength",
        };
        for (const char *nm : k_try_names)
        {
            if (v4l2_query_ext_ctrl_by_exact_name(fd, nm, &s_3dnr_ctrl_info))
            {
                s_3dnr_ctrl_valid = true;
                break;
            }
        }
    }
    if (!s_3dnr_en_valid)
    {
        s_3dnr_en_valid = v4l2_query_ext_ctrl_by_exact_name(fd, "isp_3dnr_enable", &s_3dnr_en_info);
    }
    return s_3dnr_ctrl_valid || s_3dnr_en_valid;
}

static bool v4l2_set_ext_ctrl_blob(int fd, const struct v4l2_query_ext_ctrl &info, const void *data, size_t size)
{
    if (fd < 0 || data == nullptr || size == 0)
    {
        return false;
    }
    struct v4l2_ext_control ctrl;
    struct v4l2_ext_controls ctrls;
    std::memset(&ctrl, 0, sizeof(ctrl));
    std::memset(&ctrls, 0, sizeof(ctrls));
    ctrl.id = info.id;
    ctrl.ptr = const_cast<void *>(data);
    ctrl.size = static_cast<decltype(ctrl.size)>(size);
    ctrls.count = 1;
    ctrls.controls = &ctrl;
    ctrls.which = V4L2_CTRL_ID2WHICH(ctrl.id);
    const int rc = ioctl(fd, VIDIOC_S_EXT_CTRLS, &ctrls);
    if (rc < 0) {
        HAL_LOG_ERROR("v4l2_set_ext_ctrl_blob: ioctl VIDIOC_S_EXT_CTRLS failed id=0x%x size=%zu errno=%d (%s)",
                      ctrl.id, size, errno, strerror(errno));
    }
    return rc >= 0;
}

static bool v4l2_get_ext_ctrl_blob(int fd, const struct v4l2_query_ext_ctrl &info, void *data, size_t size)
{
    if (fd < 0 || data == nullptr || size == 0)
    {
        return false;
    }
    struct v4l2_ext_control ctrl;
    struct v4l2_ext_controls ctrls;
    std::memset(&ctrl, 0, sizeof(ctrl));
    std::memset(&ctrls, 0, sizeof(ctrls));
    ctrl.id = info.id;
    ctrl.ptr = data;
    ctrl.size = static_cast<decltype(ctrl.size)>(size);
    ctrls.count = 1;
    ctrls.controls = &ctrl;
    ctrls.which = V4L2_CTRL_ID2WHICH(ctrl.id);
    return ioctl(fd, VIDIOC_G_EXT_CTRLS, &ctrls) >= 0;
}

static bool af_window_ctrl_is_u16_array(const struct v4l2_query_ext_ctrl &info)
{
    /* Hailo15 isp_af_window is typically a vector control; hailo_ctrl expects 3x4, often stored as u16 elems. */
    return (info.elem_size == 2) && (info.elems >= (HAL_ISP_AF_MAX_WINDOWS * 4));
}

static std::mutex s_nr_ctrl_mu;
static bool s_nr_ctrl_valid = false;
static struct v4l2_query_ext_ctrl s_nr_ctrl_info {};

static bool resolve_noise_reduction_ctrl(int fd)
{
    std::lock_guard<std::mutex> lk(s_nr_ctrl_mu);
    if (s_nr_ctrl_valid)
    {
        return true;
    }
    static const char *k_try_names[] = {"isp_noise_reduction", "isp_2dnr_strength", "isp_nr_strength", "noise_reduction"};
    for (const char *nm : k_try_names)
    {
        if (v4l2_query_ext_ctrl_by_exact_name(fd, nm, &s_nr_ctrl_info))
        {
            s_nr_ctrl_valid = true;
            return true;
        }
    }
    if (v4l2_query_ext_ctrl_noise_like(fd, &s_nr_ctrl_info))
    {
        s_nr_ctrl_valid = true;
        return true;
    }
    return false;
}

static bool isp_apply_noise_reduction(v4l2::v4l2ControlManager &m, int nr_0_100)
{
    const int32_t v =
        std::clamp(static_cast<int32_t>(nr_0_100), static_cast<int32_t>(0), static_cast<int32_t>(100));
    (void)safe_ext_ctrl_set_optional(m, v4l2::Video0Ctrl::NOISE_REDUCTION, static_cast<uint16_t>(v));
    auto fd_opt = m.get_fd(v4l2::Device::VIDEO0);
    if (!fd_opt.has_value())
    {
        return false;
    }
    const int fd = *fd_opt.value();
    if (!resolve_noise_reduction_ctrl(fd))
    {
        return false;
    }
    return v4l2_set_ext_ctrl_from_i64(fd, s_nr_ctrl_info, static_cast<int64_t>(v));
}

static bool isp_read_noise_reduction(v4l2::v4l2ControlManager &m, int *out_nr)
{
    if (!out_nr)
    {
        return false;
    }
    int32_t tmp = 0;
    if (safe_get(m, v4l2::Video0Ctrl::NOISE_REDUCTION, tmp))
    {
        *out_nr = static_cast<int>(tmp);
        return true;
    }
    auto fd_opt = m.get_fd(v4l2::Device::VIDEO0);
    if (!fd_opt.has_value())
    {
        return false;
    }
    const int fd = *fd_opt.value();
    if (!resolve_noise_reduction_ctrl(fd))
    {
        return false;
    }
    int64_t v = 0;
    if (!v4l2_read_ext_ctrl_as_i64(fd, s_nr_ctrl_info, &v))
    {
        return false;
    }
    *out_nr = static_cast<int>(v);
    return true;
}

// ---- Picture control ioctl fallback (same pattern as noise reduction) ----

static std::mutex s_pic_ctrl_mu;
static bool s_pic_brightness_resolved = false;
static struct v4l2_query_ext_ctrl s_pic_brightness_info {};
static bool s_pic_contrast_resolved = false;
static struct v4l2_query_ext_ctrl s_pic_contrast_info {};
static bool s_pic_saturation_resolved = false;
static struct v4l2_query_ext_ctrl s_pic_saturation_info {};
static bool s_pic_sharpness_down_resolved = false;
static struct v4l2_query_ext_ctrl s_pic_sharpness_down_info {};
static bool s_pic_sharpness_up_resolved = false;
static struct v4l2_query_ext_ctrl s_pic_sharpness_up_info {};

static bool resolve_picture_ctrl(int fd, const char *const *names, size_t n_names,
                                  bool &resolved, struct v4l2_query_ext_ctrl &info)
{
    std::lock_guard<std::mutex> lk(s_pic_ctrl_mu);
    if (resolved) return true;
    for (size_t i = 0; i < n_names; ++i) {
        if (v4l2_query_ext_ctrl_by_name_all(fd, names[i], &info)) {
            resolved = true;
            return true;
        }
    }
    return false;
}

static bool isp_read_picture_ctrl_i32(v4l2::v4l2ControlManager &m,
                                       v4l2::Video0Ctrl ctrl,
                                       const char *const *names, size_t n_names,
                                       bool &resolved, struct v4l2_query_ext_ctrl &info,
                                       int32_t *out)
{
    if (safe_get(m, ctrl, *out)) return true;
    // Fallback: open /dev/video0 directly for ioctl reads (bypasses v4l2ControlManager's internal map)
    int fd = open("/dev/video0", O_RDWR);
    if (fd < 0) return false;
    bool ok = false;
    if (resolve_picture_ctrl(fd, names, n_names, resolved, info)) {
        int64_t v = 0;
        if (v4l2_read_ext_ctrl_as_i64(fd, info, &v)) {
            *out = static_cast<int32_t>(v);
            ok = true;
        }
    }
    close(fd);
    return ok;
}

static std::mutex s_ws_gate_mu;
static bool s_ws_gate_done = false;
static std::optional<uint32_t> s_ws_ae_converged_id;

/**
 * Webserver-compatible gate: ensure AE is enabled, then wait for isp_ae_converged once per process.
 * This avoids “first GET returns zeros / wrong state” without adding ad-hoc retries to every GET.
 */
static void wait_safe_to_pull_once(v4l2::v4l2ControlManager &m)
{
    std::lock_guard<std::mutex> lk(s_ws_gate_mu);
    if (s_ws_gate_done)
    {
        return;
    }

    /*
     * Ensure AE enabled (webserver init()).
     * If AE was disabled, webserver enables it and then waits for values to settle.
     */
    int32_t ae_en = 0;
    const bool have_ae = safe_get(m, v4l2::Video0Ctrl::AE_ENABLE, ae_en);
    if (!have_ae || ae_en == 0)
    {
        (void)safe_ext_ctrl_set(m, v4l2::Video0Ctrl::AE_ENABLE, static_cast<int32_t>(1));
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    /* Poll isp_ae_converged (webserver wait_isp_converge()) */
    auto fd_opt = m.get_fd(v4l2::Device::VIDEO0);
    if (fd_opt.has_value())
    {
        if (!s_ws_ae_converged_id.has_value())
        {
            s_ws_ae_converged_id = v4l2_find_ctrl_id_by_name(*fd_opt.value(), "isp_ae_converged");
        }

        if (s_ws_ae_converged_id.has_value())
        {
            int watchdog_ms = 2000;
            while (watchdog_ms > 0)
            {
                int32_t converged = 0;
                if (v4l2_get_i32_ctrl_by_id(*fd_opt.value(), s_ws_ae_converged_id.value(), &converged))
                {
                    if (converged == 1)
                    {
                        break;
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                watchdog_ms -= 50;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
    }
    s_ws_gate_done = true;
}

/* Defined later (after baseline/cache declarations). */

static int32_t clamp_i32(int64_t v, int32_t lo, int32_t hi)
{
    if (v < lo)
    {
        return lo;
    }
    if (v > hi)
    {
        return hi;
    }
    return static_cast<int32_t>(v);
}

static uint16_t clamp_u16(int64_t v, uint16_t lo, uint16_t hi)
{
    if (v < lo)
    {
        return lo;
    }
    if (v > hi)
    {
        return hi;
    }
    return static_cast<uint16_t>(v);
}

/* Map UI percent [0..100], neutral 50, to driver range — same idea as V1 apply_image_config_to_isp. */
static int32_t calc_from_percent_s(int32_t percent, int32_t minv, int32_t maxv, int32_t calib)
{
    if (percent >= 50)
    {
        const float t = (percent - 50) / 50.0f;
        const float out = t * static_cast<float>(maxv - calib) + static_cast<float>(calib);
        return static_cast<int32_t>(std::lround(out));
    }
    const float t = (50 - percent) / 50.0f;
    const float out = t * static_cast<float>(minv - calib) + static_cast<float>(calib);
    return static_cast<int32_t>(std::lround(out));
}

static uint16_t calc_from_percent_u(int32_t percent, uint16_t minv, uint16_t maxv, uint16_t calib)
{
    const float minf = static_cast<float>(minv);
    const float maxf = static_cast<float>(maxv);
    const float calibf = static_cast<float>(calib);
    if (percent >= 50)
    {
        const float t = (percent - 50) / 50.0f;
        const float out = t * (maxf - calibf) + calibf;
        return clamp_u16(static_cast<int64_t>(std::lround(out)), minv, maxv);
    }
    const float t = (50 - percent) / 50.0f;
    const float out = t * (minf - calibf) + calibf;
    return clamp_u16(static_cast<int64_t>(std::lround(out)), minv, maxv);
}

static int32_t percent_from_value_s(int32_t v, int32_t minv, int32_t maxv, int32_t calib)
{
    if (maxv == calib && minv == calib)
    {
        return 50;
    }
    if (v >= calib)
    {
        if (maxv == calib)
        {
            return 50;
        }
        const float t = static_cast<float>(v - calib) / static_cast<float>(maxv - calib);
        return clamp_i32(static_cast<int64_t>(std::lround(50.0f + t * 50.0f)), 0, 100);
    }
    if (calib == minv)
    {
        return 50;
    }
    const float t = static_cast<float>(calib - v) / static_cast<float>(calib - minv);
    return clamp_i32(static_cast<int64_t>(std::lround(50.0f - t * 50.0f)), 0, 100);
}

static int32_t percent_from_value_u(uint16_t v, uint16_t minv, uint16_t maxv, uint16_t calib)
{
    const auto vf = static_cast<float>(v);
    const float minf = static_cast<float>(minv);
    const float maxf = static_cast<float>(maxv);
    const float calibf = static_cast<float>(calib);
    if (maxv == calib && minv == calib)
    {
        return 50;
    }
    if (vf >= calibf)
    {
        if (maxf == calibf)
        {
            return 50;
        }
        const float t = (vf - calibf) / (maxf - calibf);
        return clamp_i32(static_cast<int64_t>(std::lround(50.0f + t * 50.0f)), 0, 100);
    }
    if (calibf == minf)
    {
        return 50;
    }
    const float t = (calibf - vf) / (calibf - minf);
    return clamp_i32(static_cast<int64_t>(std::lround(50.0f - t * 50.0f)), 0, 100);
}

/* Baselines read once from driver (webserver-style), used for percent <-> HW mapping. */
static int32_t s_baseline_brightness = 0;
static int32_t s_baseline_contrast = 114;
static int32_t s_baseline_saturation = 100;
static uint16_t s_baseline_sharpness_down = 32768;
static uint16_t s_baseline_sharpness_up = 15000;
static int32_t s_baseline_wdr = 0;
static bool s_picture_baselines_loaded = false;

/**
 * Webserver keeps m_baseline_stream_params fixed for the whole manual session — UI 50% maps to these values.
 * s_baseline_* can be re-read at wrong times; s_ref_* is captured once when entering manual (first apply_manual
 * after invalidate) and used for percent <-> HW until manual is turned off.
 */
static int32_t s_ref_brightness = 0;
static int32_t s_ref_contrast = 114;
static int32_t s_ref_saturation = 100;
static uint16_t s_ref_sharpness_down = 32768;
static uint16_t s_ref_sharpness_up = 15000;
static int32_t s_ref_wdr = 0;
static bool s_manual_ref_valid = false;

static void invalidate_picture_baselines()
{
    s_picture_baselines_loaded = false;
    s_manual_ref_valid = false;
}

/** Re-read V4L2 picture controls only; keeps s_manual_ref_* (webserver m_baseline_stream_params) for the session. */
static void invalidate_picture_baselines_hw_only()
{
    s_picture_baselines_loaded = false;
}

static void ensure_picture_baselines(v4l2::v4l2ControlManager &m)
{
    if (s_picture_baselines_loaded)
    {
        return;
    }
    /* Webserver isp.cpp reads BRIGHTNESS as int32_t into stream_isp_params_t before to_stream_params(). */
    int32_t b = 0;
    int32_t c = 0;
    int32_t s = 0;
    int32_t wdr = 0;
    int32_t shd = 0;
    int32_t shu = 0;

    static const char *k_brightness_names[] = {"isp_cproc_brightness"};
    static const char *k_contrast_names[]   = {"isp_cproc_contrast"};
    static const char *k_saturation_names[] = {"isp_cproc_saturation"};
    static const char *k_shd_names[]        = {"sharpness_down", "isp_sharpness_down"};
    static const char *k_shu_names[]        = {"sharpness_up", "isp_sharpness_up"};

    if (isp_read_picture_ctrl_i32(m, v4l2::Video0Ctrl::BRIGHTNESS,
            k_brightness_names, 1, s_pic_brightness_resolved, s_pic_brightness_info, &b)) {
        s_baseline_brightness = b;
    }
    if (isp_read_picture_ctrl_i32(m, v4l2::Video0Ctrl::CONTRAST,
            k_contrast_names, 1, s_pic_contrast_resolved, s_pic_contrast_info, &c)) {
        s_baseline_contrast = c;
        if (s_baseline_contrast < 30) s_baseline_contrast = 114;
    }
    if (isp_read_picture_ctrl_i32(m, v4l2::Video0Ctrl::SATURATION,
            k_saturation_names, 1, s_pic_saturation_resolved, s_pic_saturation_info, &s)) {
        s_baseline_saturation = s;
        if (s_baseline_saturation == 0) s_baseline_saturation = 100;
    }
    if (isp_read_picture_ctrl_i32(m, v4l2::Video0Ctrl::SHARPNESS_DOWN,
            k_shd_names, 2, s_pic_sharpness_down_resolved, s_pic_sharpness_down_info, &shd)) {
        s_baseline_sharpness_down = clamp_u16(shd, 0, 65535);
    }
    if (isp_read_picture_ctrl_i32(m, v4l2::Video0Ctrl::SHARPNESS_UP,
            k_shu_names, 2, s_pic_sharpness_up_resolved, s_pic_sharpness_up_info, &shu)) {
        s_baseline_sharpness_up = clamp_u16(shu, 0, 30000);
    }
    if (safe_get(m, v4l2::Video0Ctrl::WDR_CONTRAST, wdr))
    {
        s_baseline_wdr = wdr;
    }
    s_picture_baselines_loaded = true;
}

static int backlight_percentage_from_wdr_levels(uint16_t baseline_max, uint16_t baseline_min,
                                                uint16_t current_max, uint16_t current_min)
{
    constexpr float kAbsoluteMaxLevel = 255.0f;
    constexpr float kMaxCombinedLevel = kAbsoluteMaxLevel * 2.0f;

    const float base_sum = static_cast<float>(baseline_max) + static_cast<float>(baseline_min);
    const float cur_sum = static_cast<float>(current_max) + static_cast<float>(current_min);
    if (base_sum < 1e-5f)
    {
        return 0;
    }
    const float current_backlight =
        1.0f - base_sum / kMaxCombinedLevel;
    const float scale = cur_sum / base_sum;
    const float target_backlight = 1.0f - scale * (1.0f - current_backlight);
    const int pct = static_cast<int>(std::lround(target_backlight * 100.0f));
    return std::clamp(pct, 0, 100);
}

static std::mutex s_bl_mu;
static bool s_bl_baseline_valid = false;
static uint16_t s_bl_baseline_max = 0;
static uint16_t s_bl_baseline_min = 0;

extern "C" void hailo15_isp_webserver_reset_state(void)
{
    /*
     * Mirror webserver RESET_ISP / SWITCH_PROFILE side-effects:
     * - next GET/SET must re-run wait_safe_to_pull_once (AE enable + converge wait)
     * - baseline parameters must be re-captured for percent mappings
     */
    {
        std::lock_guard<std::mutex> lk(s_ws_gate_mu);
        s_ws_gate_done = false;
        s_ws_ae_converged_id.reset();
    }
    {
        std::lock_guard<std::mutex> lk2(s_bl_mu);
        s_bl_baseline_valid = false;
        s_bl_baseline_max = 0;
        s_bl_baseline_min = 0;
    }
    {
        std::lock_guard<std::mutex> lk3(s_nr_ctrl_mu);
        s_nr_ctrl_valid = false;
        std::memset(&s_nr_ctrl_info, 0, sizeof(s_nr_ctrl_info));
    }
    {
        std::lock_guard<std::mutex> lk4(s_af_mu);
        s_af_win_ctrl_valid = false;
        s_af_enable_ctrl_valid = false;
        s_af_meas_ctrl_valid = false;
        std::memset(&s_af_enable_ctrl_info, 0, sizeof(s_af_enable_ctrl_info));
        std::memset(&s_af_win_ctrl_info, 0, sizeof(s_af_win_ctrl_info));
        std::memset(&s_af_meas_ctrl_info, 0, sizeof(s_af_meas_ctrl_info));
    }
    invalidate_picture_baselines();
}

bool apply_exposure(v4l2::v4l2ControlManager &m, const HalIspExposureConfig *config, Hailo15MediaPriv *ml_priv)
{
    bool ok = true;
    ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::AE_ENABLE, static_cast<int32_t>(config->auto_exposure ? 1 : 0));
    if (!config->auto_exposure)
    {
        /* Align with V1 HAL manual exposure limits (see HAL_ISP_MANUAL_EXPOSURE_US_*). */
        const uint16_t integration =
            clamp_u16(static_cast<int64_t>(config->exposure_time_us), 2000u, 33000u);
        ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::AE_INTEGRATION_TIME, integration);
        /* UI gain -> driver register (same scaling as V1 / webserver). */
        const int32_t gain_ui = clamp_i32(config->gain, 1, 3800);
        const uint32_t reg_gain = static_cast<uint32_t>(static_cast<int64_t>(gain_ui) * 1024);
        ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::AE_GAIN, reg_gain);
    }
    else
    {
        const int32_t bl = clamp_i32(config->backlight, 0, 100);
        /*
         * Webserver behavior:
         * - backlight is controlled through ML automatic_algorithms_config.adaptive_ae.wdrContrast (set_auto_exposure)
         * - it does not depend on reading isp_wdr_contrast back from driver
         */
        if (ml_priv != nullptr && ml_priv->media_lib != nullptr)
        {
            ml_sync_backlight_wdr(ml_priv, bl);
        }
        else
        {
            /* Best-effort fallback when ML is not available. */
            ensure_picture_baselines(m);
            const int32_t wdr_hw = calc_from_percent_s(bl, -1023, 1023, s_baseline_wdr);
            ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::WDR_CONTRAST, static_cast<int16_t>(wdr_hw));
        }
    }
    return ok;
}

bool apply_manual(v4l2::v4l2ControlManager &m, const HalIspManualConfig *config)
{
    if (!config->manual_state)
    {
        return true;
    }
    /* First apply in a manual session: after writes, snap refs to HW so GET matches (driver may clamp/quantize). */
    const bool snap_refs_after_write = !s_manual_ref_valid;
    ensure_picture_baselines(m);
    if (!s_manual_ref_valid)
    {
        s_ref_brightness = s_baseline_brightness;
        s_ref_contrast = s_baseline_contrast;
        s_ref_saturation = s_baseline_saturation;
        s_ref_sharpness_down = s_baseline_sharpness_down;
        s_ref_sharpness_up = s_baseline_sharpness_up;
        s_ref_wdr = s_baseline_wdr;
        s_manual_ref_valid = true;
    }

    const int32_t brightness_p = clamp_i32(config->brightness, 0, 100);
    const int32_t contrast_p = clamp_i32(config->contrast, 0, 100);
    const int32_t saturation_p = clamp_i32(config->saturation, 0, 100);
    const int32_t sharpness_p = clamp_i32(config->sharpness, 0, 100);

    const int32_t b_hw = calc_from_percent_s(brightness_p, -128, 127, s_ref_brightness);
    const int32_t c_hw = calc_from_percent_s(contrast_p, 30, 199, s_ref_contrast);
    const int32_t s_hw = calc_from_percent_s(saturation_p, 0, 199, s_ref_saturation);
    const uint16_t sh_down = calc_from_percent_u(sharpness_p, 0, 65535, s_ref_sharpness_down);
    const uint16_t sh_up = calc_from_percent_u(sharpness_p, 0, 30000, s_ref_sharpness_up);

    /* Match webserver: set BRIGHTNESS as int32_t (int8 domain) — see isp.cpp stream_params POST. */
    bool ok = true;
    {
        const int32_t b_clamped = std::clamp(b_hw, -128, 127);
        ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::BRIGHTNESS, b_clamped);
    }
    ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::CONTRAST, c_hw);
    ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::SATURATION, s_hw);

    ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::EE_ENABLE, static_cast<int32_t>(0));
    ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::SHARPNESS_DOWN, static_cast<int32_t>(sh_down));
    ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::SHARPNESS_UP, static_cast<int32_t>(sh_up));
    ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::EE_ENABLE, static_cast<int32_t>(1));
    // Note: snap_refs_after_write removed — recalibrating baseline to just-written
    // HW values causes subsequent GETs to always return 50%. Keep original baselines.
    return ok;
}

} // namespace

extern "C" {

static int hailo15_isp_get_current_manual_config(void *video_ctx, HalIspManualConfig *config);
static int hailo15_isp_get_current_exposure_config(void *video_ctx, HalIspExposureConfig *config);
void hailo15_isp_webserver_reset_state(void);

static bool g_manual_enabled = false;
/** Tracks ISP manual picture mode across set_image / set_manual for transition detection. */
static bool s_manual_picture_mode_track = false;

static int hailo15_isp_set_image_config(void *video_ctx, const HalIspImageConfig *config)
{
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (config->awb_profile_count > 0U && config->awb_profile_list != nullptr)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    try
    {
        Hailo15MediaPriv *const ml_priv = media_priv_from_video_ctx(video_ctx);
        auto &m = isp_ctrl_mgr();
        /* Match webserver behavior: on first touch, make sure ISP/AE is in a stable state (baselines depend on it). */
        wait_safe_to_pull_once(m);
        g_manual_enabled = config->manual_config.manual_state;
        const bool entering_manual = g_manual_enabled && !s_manual_picture_mode_track;
        const bool leaving_manual = !g_manual_enabled && s_manual_picture_mode_track;
        /* Webserver IspBlender::set_auto_configs: manual picture => disable ae_e_v1/a_cproc/aw_drv4 in profile. */
        ml_apply_isp_auto_algorithm_blocks(ml_priv, !g_manual_enabled);
        if (entering_manual || leaving_manual)
        {
            invalidate_picture_baselines();
        }
        s_manual_picture_mode_track = g_manual_enabled;
        /* Load WDR/NR baselines before touching those controls. */
        ensure_picture_baselines(m);
        bool ok = true;
        HalIspExposureConfig exp_cfg = config->exposure_config;
        const bool ok_pwr = safe_ext_ctrl_set_optional(m, v4l2::Video0Ctrl::POWERLINE_FREQUENCY,
                                              static_cast<uint16_t>(map_pwr_freq(config->pwr_freq)));
        if (ok_pwr)
        {
            ml_sync_flicker_period(ml_priv, static_cast<uint16_t>(map_pwr_freq(config->pwr_freq)));
        }
        /*
         * Webserver alignment:
         * - /isp/wdr is only settable in manual filters mode.
         * - backlight compensation in auto exposure is controlled via /isp/auto_exposure(backlight), not /isp/wdr.
         *
         * Our closest equivalent of "manual filters" is manual picture mode (auto algos disabled).
         */
        /* After NR/WDR/ioctl, re-read picture HW for ensure in apply_manual without dropping s_ref_* snapshot. */
        if (g_manual_enabled)
        {
            invalidate_picture_baselines_hw_only();
        }
        (void)apply_manual(m, &config->manual_config);
        /* Exposure is already applied by the separate set_exposure_config() call
           in camera_daemon.cpp. Discarding the return here prevents a V4L2 control
           failure in apply_exposure from blocking unrelated WDR/AWB/noise-reduction
           settings that follow. */
        (void)apply_exposure(m, &exp_cfg, ml_priv);
        /*
         * Webserver semantics: /isp/wdr is only meaningful in manual filters mode.
         * NE503: WDR applies in both auto and manual modes — user expects the slider to work.
         */
        {
            const int32_t wdr_p = clamp_i32(config->wdr_value, 0, 100);
            const int32_t calib = s_manual_ref_valid ? s_ref_wdr : s_baseline_wdr;
            const int32_t wdr_hw = calc_from_percent_s(wdr_p, -1023, 1023, calib);
            ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::WDR_CONTRAST, static_cast<int16_t>(wdr_hw));
        }
        (void)isp_apply_noise_reduction(m, config->noise_reduction);
        if (config->awb_idx >= 0)
        {
            ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::AWB_MODE, static_cast<uint16_t>(0));
            ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::AWB_ILLUM_INDEX,
                                         static_cast<uint16_t>(config->awb_idx));
        }
        else
        {
            ok = ok && safe_ext_ctrl_set(m, v4l2::Video0Ctrl::AWB_MODE, static_cast<uint16_t>(1));
        }
        return ok ? HAL_OK : HAL_ERROR;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("hailo15_isp_set_image_config exception: %s", e.what());
        return HAL_ERROR;
    }
    catch (...)
    {
        HAL_LOG_ERROR("hailo15_isp_set_image_config unknown exception");
        return HAL_ERROR;
    }
}

static int hailo15_isp_get_current_image_config(void *video_ctx, HalIspImageConfig *config)
{
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::memset(config, 0, sizeof(*config));
    try
    {
        auto &m = isp_ctrl_mgr();
        wait_safe_to_pull_once(m);
        ensure_picture_baselines(m);
        int32_t v = 0;

        /* Webserver GET: when adaptive_ae is on, flicker comes from ML config, not ioctl. */
        Hailo15MediaPriv *const ml_priv = media_priv_from_video_ctx(video_ctx);
        bool pwr_from_ml = false;
        if (ml_priv && ml_priv->media_lib)
        {
            try
            {
                /* Do not hold ml_priv->mutex across MediaLibrary calls. */
                auto prof_exp = ml_priv->media_lib->get_current_profile();
                if (prof_exp.has_value())
                {
                    const auto &aac = prof_exp.value().iq_settings.automatic_algorithms_config;
                    if (aac.adaptive_ae.enabled)
                    {
                        config->pwr_freq = pwr_freq_from_driver(static_cast<int32_t>(aac.adaptive_ae.flicker_period));
                        pwr_from_ml = true;
                    }
                }
            }
            catch (...)
            {
            }
        }
        if (!pwr_from_ml)
        {
            if (safe_get(m, v4l2::Video0Ctrl::POWERLINE_FREQUENCY, v))
            {
                config->pwr_freq = pwr_freq_from_driver(v);
            }
            else
            {
                config->pwr_freq = HAL_ISP_PWR_FREQ_OFF;
            }
        }

        {
            int nr = 0;
            if (isp_read_noise_reduction(m, &nr))
            {
                config->noise_reduction = nr;
            }
        }

        if (safe_get(m, v4l2::Video0Ctrl::WDR_CONTRAST, v))
        {
            const int32_t calib = s_manual_ref_valid ? s_ref_wdr : s_baseline_wdr;
            config->wdr_value = percent_from_value_s(v, -1023, 1023, calib);
        }

        if (hailo15_isp_get_current_manual_config(video_ctx, &config->manual_config) != HAL_OK)
        {
            return HAL_ERROR;
        }
        if (hailo15_isp_get_current_exposure_config(video_ctx, &config->exposure_config) != HAL_OK)
        {
            return HAL_ERROR;
        }
        /* Webserver /isp/wdr: in auto filters mode it returns the default 50. */
        if (!config->manual_config.manual_state)
        {
            config->wdr_value = 50;
        }

        int awb_prof = 0;
        if (get_awb_idx_from_v4l2_with_retry(m, &awb_prof))
        {
            config->awb_idx = awb_prof;
        }
        else if (ml_priv != nullptr && ml_get_awb_idx_from_profile_json(ml_priv, &awb_prof))
        {
            config->awb_idx = awb_prof;
        }
        else
        {
            config->awb_idx = -1;
        }
        ml_fill_awb_profile_list(ml_priv, config);
        return HAL_OK;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("hailo15_isp_get_current_image_config exception: %s", e.what());
        return HAL_ERROR;
    }
    catch (...)
    {
        HAL_LOG_ERROR("hailo15_isp_get_current_image_config unknown exception");
        return HAL_ERROR;
    }
}

static int hailo15_isp_set_manual_config(void *video_ctx, const HalIspManualConfig *config)
{
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    try
    {
        Hailo15MediaPriv *const ml_priv = media_priv_from_video_ctx(video_ctx);
        auto &m = isp_ctrl_mgr();
        wait_safe_to_pull_once(m);
        if (!config->manual_state)
        {
            g_manual_enabled = false;
            s_manual_picture_mode_track = false;
            ml_apply_isp_auto_algorithm_blocks(ml_priv, true);
            invalidate_picture_baselines();
            return HAL_OK;
        }
        g_manual_enabled = true;
        ml_apply_isp_auto_algorithm_blocks(ml_priv, false);
        if (!s_manual_picture_mode_track)
        {
            invalidate_picture_baselines();
            s_manual_picture_mode_track = true;
        }
        invalidate_picture_baselines_hw_only();
        (void)apply_manual(m, config);
        return HAL_OK;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("hailo15_isp_set_manual_config exception: %s", e.what());
        return HAL_ERROR;
    }
    catch (...)
    {
        HAL_LOG_ERROR("hailo15_isp_set_manual_config unknown exception");
        return HAL_ERROR;
    }
}

static int hailo15_isp_get_current_manual_config(void *video_ctx, HalIspManualConfig *config)
{
    (void)video_ctx;
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    try
    {
        /* Webserver GET /isp/stream_params: in auto filter mode returns nominal 50,50,50,50 (no V4L2 read). */
        if (!g_manual_enabled)
        {
            config->manual_state = false;
            config->brightness = 50;
            config->contrast = 50;
            config->saturation = 50;
            config->sharpness = 50;
            return HAL_OK;
        }

        auto &m = isp_ctrl_mgr();
        ensure_picture_baselines(m);
        int32_t b = 0;
        int32_t c = 0;
        int32_t s = 0;
        int32_t su = 0;
        int32_t sd = 0;

        static const char *k_bnames[]  = {"isp_cproc_brightness"};
        static const char *k_cnames[]  = {"isp_cproc_contrast"};
        static const char *k_snames[]  = {"isp_cproc_saturation"};
        static const char *k_shunames[] = {"sharpness_up", "isp_sharpness_up"};
        static const char *k_shdnames[] = {"sharpness_down", "isp_sharpness_down"};

        const bool ok_b  = isp_read_picture_ctrl_i32(m, v4l2::Video0Ctrl::BRIGHTNESS,
            k_bnames, 1, s_pic_brightness_resolved, s_pic_brightness_info, &b);
        const bool ok_c  = isp_read_picture_ctrl_i32(m, v4l2::Video0Ctrl::CONTRAST,
            k_cnames, 1, s_pic_contrast_resolved, s_pic_contrast_info, &c);
        const bool ok_s  = isp_read_picture_ctrl_i32(m, v4l2::Video0Ctrl::SATURATION,
            k_snames, 1, s_pic_saturation_resolved, s_pic_saturation_info, &s);
        const bool ok_su = isp_read_picture_ctrl_i32(m, v4l2::Video0Ctrl::SHARPNESS_UP,
            k_shunames, 2, s_pic_sharpness_up_resolved, s_pic_sharpness_up_info, &su);
        const bool ok_sd = isp_read_picture_ctrl_i32(m, v4l2::Video0Ctrl::SHARPNESS_DOWN,
            k_shdnames, 2, s_pic_sharpness_down_resolved, s_pic_sharpness_down_info, &sd);

        if (!(ok_b || ok_c || ok_s || ok_su || ok_sd))
        {
            return HAL_ERROR;
        }

        config->manual_state = true;
        const int32_t br = s_manual_ref_valid ? s_ref_brightness : s_baseline_brightness;
        const int32_t cr = s_manual_ref_valid ? s_ref_contrast : s_baseline_contrast;
        const int32_t sr = s_manual_ref_valid ? s_ref_saturation : s_baseline_saturation;
        const uint16_t sdr = s_manual_ref_valid ? s_ref_sharpness_down : s_baseline_sharpness_down;
        const uint16_t sur = s_manual_ref_valid ? s_ref_sharpness_up : s_baseline_sharpness_up;

        if (ok_b)
        {
            config->brightness = percent_from_value_s(b, -128, 127, br);
        }
        else
        {
            config->brightness = 50;
        }
        if (ok_c)
        {
            config->contrast = percent_from_value_s(c, 30, 199, cr);
        }
        else
        {
            config->contrast = 50;
        }
        if (ok_s)
        {
            config->saturation = percent_from_value_s(s, 0, 199, sr);
        }
        else
        {
            config->saturation = 50;
        }
        /* Webserver common.cpp to_stream_params: sharpness % from SHARPNESS_DOWN only. */
        if (ok_sd)
        {
            config->sharpness =
                percent_from_value_u(static_cast<uint16_t>(sd), 0, 65535, sdr);
        }
        else if (ok_su)
        {
            config->sharpness =
                percent_from_value_u(static_cast<uint16_t>(su), 0, 30000, sur);
        }
        else
        {
            config->sharpness = 50;
        }
        return HAL_OK;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("hailo15_isp_get_current_manual_config exception: %s", e.what());
        return HAL_ERROR;
    }
    catch (...)
    {
        HAL_LOG_ERROR("hailo15_isp_get_current_manual_config unknown exception");
        return HAL_ERROR;
    }
}

static int hailo15_isp_set_exposure_config(void *video_ctx, const HalIspExposureConfig *config)
{
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    try
    {
        auto &m = isp_ctrl_mgr();
        wait_safe_to_pull_once(m);
        Hailo15MediaPriv *const ml_priv = media_priv_from_video_ctx(video_ctx);
        const bool ok = apply_exposure(m, config, ml_priv);
        return ok ? HAL_OK : HAL_ERROR;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("hailo15_isp_set_exposure_config exception: %s", e.what());
        return HAL_ERROR;
    }
    catch (...)
    {
        HAL_LOG_ERROR("hailo15_isp_set_exposure_config unknown exception");
        return HAL_ERROR;
    }
}

static int hailo15_isp_get_current_exposure_config(void *video_ctx, HalIspExposureConfig *config)
{
    (void)video_ctx;
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    try
    {
        std::memset(config, 0, sizeof(*config));
        auto &m = isp_ctrl_mgr();
        wait_safe_to_pull_once(m);
        /*
         * Static default-constructed v4l2ControlManager opens the ISP control path lazily on first use.
         * The first batch of GETs can fail or return stale zeros while the node is not ready; retry a few
         * times (matches "second isp_exposure_get is correct" without any parameter change).
         */
        int32_t ae = 0, et = 0, g = 0, wdr = 0;
        bool ok_ae = false, ok_et = false, ok_g = false, ok_wdr = false;
        bool any = false;
        int32_t last_ae = 0, last_et = 0, last_g = 0, last_wdr = 0;
        bool last_ok_ae = false, last_ok_et = false, last_ok_g = false, last_ok_wdr = false;
        for (int attempt = 0; attempt < 10; ++attempt)
        {
            if (attempt > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }

            ae = et = g = wdr = 0;
            ok_ae = safe_get(m, v4l2::Video0Ctrl::AE_ENABLE, ae);
            ok_et = safe_get(m, v4l2::Video0Ctrl::AE_INTEGRATION_TIME, et);
            ok_g = safe_get(m, v4l2::Video0Ctrl::AE_GAIN, g);
            ok_wdr = safe_get(m, v4l2::Video0Ctrl::WDR_CONTRAST, wdr);

            if (ok_ae || ok_et || ok_g || ok_wdr)
            {
                any = true;
                last_ae = ae;
                last_et = et;
                last_g = g;
                last_wdr = wdr;
                last_ok_ae = ok_ae;
                last_ok_et = ok_et;
                last_ok_g = ok_g;
                last_ok_wdr = ok_wdr;
            }

            /*
             * When AE is enabled, integration/gain can briefly read as zeros right after startup or toggles.
             * Keep sampling a bit longer (webserver has an init/wait_safe_to_pull gate before serving GETs).
             */
            if (ok_ae && ae != 0 && (et == 0 || g == 0))
            {
                continue;
            }

            /* If we already have a consistent snapshot, stop early. */
            if (ok_ae && ok_et && ok_g)
            {
                break;
            }
        }

        if (!any)
        {
            return HAL_ERROR;
        }

        ae = last_ae;
        et = last_et;
        g = last_g;
        wdr = last_wdr;
        ok_ae = last_ok_ae;
        ok_et = last_ok_et;
        ok_g = last_ok_g;
        ok_wdr = last_ok_wdr;

        if (ok_ae)
        {
            config->auto_exposure = ae != 0;
        }
        if (ok_et)
        {
            config->exposure_time_us = et;
        }
        if (ok_g)
        {
            /* Register -> UI gain (integer division matches V1 intent). */
            config->gain = static_cast<int>(g / 1024U);
        }
        config->backlight = 0;
        if (config->auto_exposure)
        {
            /* Webserver: backlight comes from ML adaptive_ae wdrContrast (not from isp_wdr_contrast). */
            bool got_ml = false;
            Hailo15MediaPriv *const ml_priv = media_priv_from_video_ctx(video_ctx);
            if (ml_priv && ml_priv->media_lib)
            {
                try
                {
                    /* Do not hold ml_priv->mutex across MediaLibrary calls. */
                    auto prof_exp = ml_priv->media_lib->get_current_profile();
                    if (prof_exp.has_value())
                    {
                        const auto &aae = prof_exp.value().iq_settings.automatic_algorithms_config.adaptive_ae;
                        uint16_t bmax = 0, bmin = 0;
                        {
                            std::lock_guard<std::mutex> lk2(s_bl_mu);
                            if (!s_bl_baseline_valid)
                            {
                                /* Webserver captures baseline once at init. Use current values as baseline. */
                                s_bl_baseline_max = aae.wdrContrast.max;
                                s_bl_baseline_min = aae.wdrContrast.min;
                                s_bl_baseline_valid = true;
                            }
                            bmax = s_bl_baseline_max;
                            bmin = s_bl_baseline_min;
                        }
                        config->backlight = backlight_percentage_from_wdr_levels(
                            bmax, bmin, aae.wdrContrast.max, aae.wdrContrast.min);
                        got_ml = true;
                    }
                }
                catch (...)
                {
                }
            }
            if (!got_ml && ok_wdr)
            {
                ensure_picture_baselines(m);
                config->backlight = percent_from_value_s(wdr, -1023, 1023, s_baseline_wdr);
            }
        }
        return HAL_OK;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("hailo15_isp_get_current_exposure_config exception: %s", e.what());
        return HAL_ERROR;
    }
    catch (...)
    {
        HAL_LOG_ERROR("hailo15_isp_get_current_exposure_config unknown exception");
        return HAL_ERROR;
    }
}

static int hailo15_isp_set_af_windows_config(void *video_ctx, const HalIspAfWindowsConfig *config)
{
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    try
    {
        auto &m = isp_ctrl_mgr();
        wait_safe_to_pull_once(m);
        auto fd_opt = m.get_fd(v4l2::Device::VIDEO0);
        if (!fd_opt.has_value())
        {
            return HAL_ERROR;
        }
        const int fd = *fd_opt.value();
        const bool have_enable = resolve_af_enable_ctrl(fd);
        const bool have_windows = resolve_af_window_ctrl(fd);
        if (!have_enable && !have_windows)
        {
            return HAL_ERR_NOT_SUPPORTED;
        }

        if (!config->enabled)
        {
            /* Per doc: disable/enable autofocus via isp_af_enable (ISP_CID_AF_ENABLE). */
            if (have_enable)
            {
                if (!v4l2_set_ext_ctrl_from_i64(fd, s_af_enable_ctrl_info, 0))
                {
                    return HAL_ERROR;
                }
                return HAL_OK;
            }
            return HAL_ERR_NOT_SUPPORTED;
        }

        /* Enable AF first (matches doc) */
        if (have_enable)
        {
            if (!v4l2_set_ext_ctrl_from_i64(fd, s_af_enable_ctrl_info, 1))
            {
                HAL_LOG_ERROR("hailo15_isp: v4l2_set_ext_ctrl_from_i64(AF_ENABLE=1) failed");
                return HAL_ERROR;
            }
        }

        if (!have_windows)
        {
            /* Some drivers may support enable without window selection. */
            return HAL_OK;
        }

        const uint32_t n = std::clamp<uint32_t>(config->window_count, 1U, HAL_ISP_AF_MAX_WINDOWS);

        /* Hailo ctrl requires 3 windows; we always write a full 3x4 buffer and zero-fill unused windows. */
        if (af_window_ctrl_is_u16_array(s_af_win_ctrl_info))
        {
            uint16_t win16[HAL_ISP_AF_MAX_WINDOWS][4] = {};
            for (uint32_t i = 0; i < n; ++i)
            {
                const int32_t x = config->windows[i].x;
                const int32_t y = config->windows[i].y;
                const int32_t w = config->windows[i].w;
                const int32_t h = config->windows[i].h;

                /* Doc constraints (Hailo Imaging UG 6.3 AF): x>=5,y>=2; w/h positive; x+w/y+h within image. */
                if (x < 5 || y < 2 || w <= 0 || h <= 0)
                {
                    HAL_LOG_ERROR("hailo15_isp: AF window[%u] boundary check failed x=%d y=%d w=%d h=%d (need x>=5,y>=2,w>0,h>0)",
                                  i, x, y, w, h);
                    return HAL_ERR_INVALID_ARG;
                }
                /* Doc constraint: window area must not exceed 128^3 to avoid measurement overflow. */
                {
                    static constexpr int64_t k_max_area = 128LL * 128LL * 128LL; /* 2,097,152 */
                    const int64_t area = static_cast<int64_t>(w) * static_cast<int64_t>(h);
                    if (area > k_max_area)
                    {
                        HAL_LOG_ERROR("hailo15_isp: AF window[%u] area check failed w=%d h=%d area=%lld max=%lld",
                                      i, w, h, (long long)area, (long long)k_max_area);
                        return HAL_ERR_INVALID_ARG;
                    }
                }
                /* NOTE: The output-resolution boundary check (x+w <= img_w etc.)
                 * is intentionally omitted here.  AF statistics are computed by ISP
                 * firmware on the raw sensor data, not on the scaled output frame.
                 * The window coordinates may exceed the current output resolution
                 * when the encoder output resolution is lower than the sensor's
                 * native resolution — this is expected and handled correctly by
                 * the ISP hardware.  Only the Hailo hardware constraints above
                 * (x>=5, y>=2, area≤128³) are enforced. */
                if (x > 0xFFFF || y > 0xFFFF || w > 0xFFFF || h > 0xFFFF)
                {
                    HAL_LOG_ERROR("hailo15_isp: AF window[%u] 0xFFFF check failed x=%d y=%d w=%d h=%d",
                                  i, x, y, w, h);
                    return HAL_ERR_INVALID_ARG;
                }
                win16[i][0] = static_cast<uint16_t>(x);
                win16[i][1] = static_cast<uint16_t>(y);
                win16[i][2] = static_cast<uint16_t>(w);
                win16[i][3] = static_cast<uint16_t>(h);
            }
            HAL_LOG_INFO("hailo15_isp: AF windows configured OK (u16)");
            return v4l2_set_ext_ctrl_blob(fd, s_af_win_ctrl_info, &win16, sizeof(win16)) ? HAL_OK : HAL_ERROR;
        }
        else
        {
            int32_t win32[HAL_ISP_AF_MAX_WINDOWS][4] = {};
            for (uint32_t i = 0; i < n; ++i)
            {
                const int32_t x = config->windows[i].x;
                const int32_t y = config->windows[i].y;
                const int32_t w = config->windows[i].w;
                const int32_t h = config->windows[i].h;

                if (x < 5 || y < 2 || w <= 0 || h <= 0)
                {
                    HAL_LOG_ERROR("hailo15_isp: AF window[%u](int32) boundary check failed x=%d y=%d w=%d h=%d",
                                  i, x, y, w, h);
                    return HAL_ERR_INVALID_ARG;
                }
                {
                    static constexpr int64_t k_max_area = 128LL * 128LL * 128LL;
                    const int64_t area = static_cast<int64_t>(w) * static_cast<int64_t>(h);
                    if (area > k_max_area)
                    {
                        HAL_LOG_ERROR("hailo15_isp: AF window[%u](int32) area check failed w=%d h=%d area=%lld max=%lld",
                                      i, w, h, (long long)area, (long long)k_max_area);
                        return HAL_ERR_INVALID_ARG;
                    }
                }
                /* NOTE: output-resolution boundary check intentionally omitted
                 * (see comment in the u16 branch above). */

                win32[i][0] = x;
                win32[i][1] = y;
                win32[i][2] = w;
                win32[i][3] = h;
            }
            HAL_LOG_INFO("hailo15_isp: AF windows configured OK (int32)");
            return v4l2_set_ext_ctrl_blob(fd, s_af_win_ctrl_info, &win32, sizeof(win32)) ? HAL_OK : HAL_ERROR;
        }
    }
    catch (...)
    {
        return HAL_ERROR;
    }
}

static int hailo15_isp_get_af_windows_config(void *video_ctx, HalIspAfWindowsConfig *config)
{
    (void)video_ctx;
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::memset(config, 0, sizeof(*config));
    try
    {
        auto &m = isp_ctrl_mgr();
        wait_safe_to_pull_once(m);
        auto fd_opt = m.get_fd(v4l2::Device::VIDEO0);
        if (!fd_opt.has_value())
        {
            return HAL_ERROR;
        }
        const int fd = *fd_opt.value();
        const bool have_enable = resolve_af_enable_ctrl(fd);
        int64_t enabled = 1;
        if (have_enable)
        {
            if (!v4l2_read_ext_ctrl_as_i64(fd, s_af_enable_ctrl_info, &enabled))
            {
                return HAL_ERROR;
            }
        }
        config->enabled = (enabled != 0);
        if (!config->enabled)
        {
            config->window_count = 0;
            return HAL_OK;
        }

        if (!resolve_af_window_ctrl(fd))
        {
            return HAL_ERR_NOT_SUPPORTED;
        }

        /* Hailo ctrl expects 3 windows; report 3 and leave unused windows as zeros. */
        config->window_count = HAL_ISP_AF_MAX_WINDOWS;
        if (af_window_ctrl_is_u16_array(s_af_win_ctrl_info))
        {
            uint16_t win16[HAL_ISP_AF_MAX_WINDOWS][4] = {};
            if (!v4l2_get_ext_ctrl_blob(fd, s_af_win_ctrl_info, &win16, sizeof(win16)))
            {
                return HAL_ERROR;
            }
            for (uint32_t i = 0; i < HAL_ISP_AF_MAX_WINDOWS; ++i)
            {
                config->windows[i].x = static_cast<int32_t>(win16[i][0]);
                config->windows[i].y = static_cast<int32_t>(win16[i][1]);
                config->windows[i].w = static_cast<int32_t>(win16[i][2]);
                config->windows[i].h = static_cast<int32_t>(win16[i][3]);
            }
        }
        else
        {
            int32_t win32[HAL_ISP_AF_MAX_WINDOWS][4] = {};
            if (!v4l2_get_ext_ctrl_blob(fd, s_af_win_ctrl_info, &win32, sizeof(win32)))
            {
                return HAL_ERROR;
            }
            for (uint32_t i = 0; i < HAL_ISP_AF_MAX_WINDOWS; ++i)
            {
                config->windows[i].x = win32[i][0];
                config->windows[i].y = win32[i][1];
                config->windows[i].w = win32[i][2];
                config->windows[i].h = win32[i][3];
            }
        }
        return HAL_OK;
    }
    catch (...)
    {
        return HAL_ERROR;
    }
}

static int hailo15_isp_subscribe_af_measurement(void *video_ctx, bool enable)
{
    (void)video_ctx;
    (void)enable;
    /*
     * Event-driven AFM notifications require Hailo-specific V4L2 event IDs.
     * This HAL build keeps the API stable but does not hardcode IDs here.
     */
    return HAL_ERR_NOT_SUPPORTED;
}

static int hailo15_isp_wait_af_measurement(void *video_ctx, int timeout_ms)
{
    (void)video_ctx;
    (void)timeout_ms;
    return HAL_ERR_NOT_SUPPORTED;
}

static int hailo15_isp_get_af_measurement(void *video_ctx, HalIspAfMeasurement *meas)
{
    (void)video_ctx;
    if (!meas)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::memset(meas, 0, sizeof(*meas));
    try
    {
        auto &m = isp_ctrl_mgr();
        wait_safe_to_pull_once(m);
        auto fd_opt = m.get_fd(v4l2::Device::VIDEO0);
        if (!fd_opt.has_value())
        {
            return HAL_ERROR;
        }
        const int fd = *fd_opt.value();
        if (!resolve_af_measurement_ctrl(fd))
        {
            return HAL_ERR_NOT_SUPPORTED;
        }

        /* Doc format: uint32_t 1x6: sum1,sum2,sum3,luma1,luma2,luma3 */
        uint32_t buf[6] = {0};
        if (!v4l2_get_ext_ctrl_blob(fd, s_af_meas_ctrl_info, buf, sizeof(buf)))
        {
            return HAL_ERROR;
        }

        meas->window_count = HAL_ISP_AF_MAX_WINDOWS;
        meas->sum[0] = buf[0];
        meas->sum[1] = buf[1];
        meas->sum[2] = buf[2];
        meas->luma[0] = buf[3];
        meas->luma[1] = buf[4];
        meas->luma[2] = buf[5];
        return HAL_OK;
    }
    catch (...)
    {
        return HAL_ERROR;
    }
}

static const char *hailo15_isp_get_version(void)
{
    return "Hailo15 HAL-ISP 2.2.0";
}

/* ---- M2: manual white balance ---- */

/** Freeze/unfreeze the AWB auto algorithm (awbv2 — NOT aw_drv4, which is
 *  Auto-WDR). Manual WB gains are overwritten every frame while AWB runs
 *  (imaging guide: "wb control must disable awb"), so manual mode must first
 *  disable the algorithm block, then isp_awb_enable + isp_awb_mode. */
static bool ml_set_awbv2_auto(Hailo15MediaPriv *priv, bool auto_enabled)
{
    if (!priv || !priv->media_lib)
    {
        return false;
    }
    try
    {
        /* Do not hold priv->mutex across MediaLibrary calls: ML may invoke callbacks that take this lock. */
        auto prof_exp = priv->media_lib->get_current_profile();
        if (!prof_exp.has_value())
        {
            return false;
        }
        config_profile_t p = prof_exp.value();
        auto &awbv2 = p.iq_settings.automatic_algorithms_config.awbv2;
        if (awbv2.disable)
        {
            return true; /* module disabled in tuning — nothing to toggle */
        }
        if (awbv2.enabled == auto_enabled)
        {
            return true;
        }
        awbv2.enabled = auto_enabled;
        const media_library_return r = priv->media_lib->set_override_parameters(p);
        if (r != MEDIA_LIBRARY_SUCCESS)
        {
            HAL_LOG_WARNING("Hailo15 ISP: set_override_parameters (awbv2=%d) failed (%d)",
                            auto_enabled ? 1 : 0, static_cast<int>(r));
            return false;
        }
        return true;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_WARNING("Hailo15 ISP: ml_set_awbv2_auto: %s", e.what());
        return false;
    }
    catch (...)
    {
        return false;
    }
}
/* ---- M2: manual white balance ---- */

static int hailo15_isp_set_wb_config(void *video_ctx, const HalIspWbConfig *config)
{
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    /* Hailo WB gain controls are Q8.8 (256 = 1.0x). Control ranges (imaging
     * guide 6.2): r/gr [100..1023] (~0.39..4.0x), gb/b [100..399] (~0.39..1.56x). */
    for (float g : {config->r_gain, config->gr_gain, config->gb_gain, config->b_gain})
    {
        if (g < 0.39f || g > 4.0f)
        {
            return HAL_ERR_INVALID_ARG;
        }
    }
    if (config->gb_gain > 1.56f || config->b_gain > 1.56f)
    {
        return HAL_ERR_INVALID_ARG;
    }
    bool ccm_requested = false;
    for (float v : config->ccm)
    {
        if (v != 0.0f)
        {
            ccm_requested = true;
            break;
        }
    }
    try
    {
        auto &m = isp_ctrl_mgr();
        wait_safe_to_pull_once(m);
        if (config->manual_state)
        {
            /* Official manual-WB sequence (imaging guide 6.2 + webserver):
             * 1) freeze the AWB algorithm (awbv2) so it stops overwriting gains
             * 2) isp_awb_enable = 0  (hard prerequisite for gain writes)
             * 3) isp_awb_mode  = 0  (manual)
             * 4) write the four Q8.8 gains
             * 5) optional CCM matrix when requested */
            Hailo15MediaPriv *priv = media_priv_from_video_ctx(video_ctx);
            if (priv && !ml_set_awbv2_auto(priv, false))
            {
                HAL_LOG_WARNING("hailo15_isp_set_wb_config: freezing awbv2 failed (continuing)");
            }
            auto fd_opt = m.get_fd(v4l2::Device::VIDEO0);
            if (!fd_opt.has_value())
            {
                return HAL_ERROR;
            }
            const int fd = *fd_opt.value();
            if (resolve_awb_enable_ctrl(fd))
            {
                const int32_t zero = 0;
                if (!v4l2_set_ext_ctrl_blob(fd, s_awb_en_info, &zero, sizeof(zero)))
                {
                    HAL_LOG_ERROR("hailo15_isp_set_wb_config: isp_awb_enable=0 failed");
                    return HAL_ERROR;
                }
            }
            if (!safe_ext_ctrl_set(m, v4l2::Video0Ctrl::AWB_MODE, static_cast<int32_t>(0)))
            {
                HAL_LOG_ERROR("hailo15_isp_set_wb_config: AWB_MODE->manual failed");
                return HAL_ERROR;
            }
            const int32_t r  = static_cast<int32_t>(config->r_gain  * 256.0f + 0.5f);
            const int32_t gr = static_cast<int32_t>(config->gr_gain * 256.0f + 0.5f);
            const int32_t gb = static_cast<int32_t>(config->gb_gain * 256.0f + 0.5f);
            const int32_t b  = static_cast<int32_t>(config->b_gain  * 256.0f + 0.5f);
            if (!safe_ext_ctrl_set(m, v4l2::Video0Ctrl::WB_R_GAIN, r) ||
                !safe_ext_ctrl_set(m, v4l2::Video0Ctrl::WB_GR_GAIN, gr) ||
                !safe_ext_ctrl_set(m, v4l2::Video0Ctrl::WB_GB_GAIN, gb) ||
                !safe_ext_ctrl_set(m, v4l2::Video0Ctrl::WB_B_GAIN, b))
            {
                HAL_LOG_ERROR("hailo15_isp_set_wb_config: WB gain write failed (r=%d gr=%d gb=%d b=%d)",
                              r, gr, gb, b);
                return HAL_ERROR;
            }
            if (ccm_requested)
            {
                static struct v4l2_query_ext_ctrl ccm_info {};
                static bool ccm_valid = false;
                if (!ccm_valid)
                {
                    ccm_valid = v4l2_query_ext_ctrl_by_exact_name(fd, "isp_wb_cc_matrix", &ccm_info);
                }
                if (ccm_valid)
                {
                    if (!v4l2_set_ext_ctrl_blob(fd, ccm_info, config->ccm, sizeof(config->ccm)))
                    {
                        HAL_LOG_ERROR("hailo15_isp_set_wb_config: isp_wb_cc_matrix write failed");
                        return HAL_ERROR;
                    }
                }
                else
                {
                    HAL_LOG_ERROR("hailo15_isp_set_wb_config: isp_wb_cc_matrix control not present");
                    return HAL_ERR_NOT_SUPPORTED;
                }
            }
        }
        else
        {
            /* Auto: re-enable the algorithm block and both v4l2 switches. */
            Hailo15MediaPriv *priv = media_priv_from_video_ctx(video_ctx);
            if (priv && !ml_set_awbv2_auto(priv, true))
            {
                HAL_LOG_WARNING("hailo15_isp_set_wb_config: re-enabling awbv2 failed (continuing)");
            }
            auto fd_opt = m.get_fd(v4l2::Device::VIDEO0);
            if (fd_opt.has_value() && resolve_awb_enable_ctrl(*fd_opt.value()))
            {
                const int32_t one = 1;
                (void)v4l2_set_ext_ctrl_blob(*fd_opt.value(), s_awb_en_info, &one, sizeof(one));
            }
            if (!safe_ext_ctrl_set(m, v4l2::Video0Ctrl::AWB_MODE, static_cast<int32_t>(1)))
            {
                HAL_LOG_ERROR("hailo15_isp_set_wb_config: AWB_MODE->auto failed");
                return HAL_ERROR;
            }
        }
        return HAL_OK;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("hailo15_isp_set_wb_config exception: %s", e.what());
        return HAL_ERROR;
    }
    catch (...)
    {
        return HAL_ERROR;
    }
}

static int hailo15_isp_get_current_wb_config(void *video_ctx, HalIspWbConfig *config)
{
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::memset(config, 0, sizeof(*config));
    try
    {
        auto &m = isp_ctrl_mgr();
        wait_safe_to_pull_once(m);
        Hailo15MediaPriv *priv = media_priv_from_video_ctx(video_ctx);
        bool manual_known = false;
        if (priv && priv->media_lib)
        {
            try
            {
                auto prof_exp = priv->media_lib->get_current_profile();
                if (prof_exp.has_value())
                {
                    config->manual_state = !prof_exp->iq_settings.automatic_algorithms_config.awbv2.enabled;
                    manual_known = true;
                }
            }
            catch (...)
            {
            }
        }
        if (!manual_known)
        {
            int32_t awb_mode = 0;
            if (!safe_get(m, v4l2::Video0Ctrl::AWB_MODE, awb_mode))
            {
                return HAL_ERROR;
            }
            config->manual_state = (awb_mode == 1);
        }
        int32_t r = 256, gr = 256, gb = 256, b = 256;
        if (!safe_ext_ctrl_get(m, v4l2::Video0Ctrl::WB_R_GAIN, r) ||
            !safe_ext_ctrl_get(m, v4l2::Video0Ctrl::WB_GR_GAIN, gr) ||
            !safe_ext_ctrl_get(m, v4l2::Video0Ctrl::WB_GB_GAIN, gb) ||
            !safe_ext_ctrl_get(m, v4l2::Video0Ctrl::WB_B_GAIN, b))
        {
            return HAL_ERROR;
        }
        config->r_gain  = static_cast<float>(r)  / 256.0f;
        config->gr_gain = static_cast<float>(gr) / 256.0f;
        config->gb_gain = static_cast<float>(gb) / 256.0f;
        config->b_gain  = static_cast<float>(b)  / 256.0f;
        (void)video_ctx;
        return HAL_OK;
    }
    catch (...)
    {
        return HAL_ERROR;
    }
}

/* ---- M2: 3DNR ---- */

static int hailo15_isp_set_3dnr_config(void *video_ctx, const HalIspNr3dConfig *config)
{
    if (!config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (config->strength < 0 || config->strength > 100)
    {
        return HAL_ERR_INVALID_ARG;
    }
    try
    {
        auto &m = isp_ctrl_mgr();
        wait_safe_to_pull_once(m);
        auto fd_opt = m.get_fd(v4l2::Device::VIDEO0);
        if (!fd_opt.has_value())
        {
            return HAL_ERROR;
        }
        const int fd = *fd_opt.value();
        if (!resolve_3dnr_ctrl(fd))
        {
            HAL_LOG_WARNING("hailo15_isp_set_3dnr_config: no temporal-NR control found (tried isp_3dnr_strength et al.)");
            return HAL_ERR_NOT_SUPPORTED;
        }
        /* Verified on target: isp_3dnr_enable (bool) gates the engine and
         * isp_3dnr_strength is [0..128] — strength 128 measurably drops
         * flat-region high-frequency energy by ~40%. Enable first, then map
         * the HAL [0..100] scale onto [0..128]. */
        /* Both controls are scalars (bool / int) — S_CTRL semantics, not
         * blob payloads (a 4-byte blob write against a 1-byte bool is
         * silently ineffective, verified on target). */
        auto scalar_set = [fd](const struct v4l2_query_ext_ctrl &info, int32_t value) -> bool
        {
            struct v4l2_control c{};
            c.id = info.id;
            c.value = value;
            return ioctl(fd, VIDIOC_S_CTRL, &c) == 0;
        };
        if (s_3dnr_en_valid)
        {
            const int32_t en = config->enabled ? 1 : 0;
            if (!scalar_set(s_3dnr_en_info, en))
            {
                HAL_LOG_ERROR("hailo15_isp_set_3dnr_config: isp_3dnr_enable=%d failed", en);
                return HAL_ERROR;
            }
        }
        if (s_3dnr_ctrl_valid)
        {
            const int32_t val = config->enabled
                ? static_cast<int32_t>((config->strength * 128 + 50) / 100)
                : 0;
            if (!scalar_set(s_3dnr_ctrl_info, val))
            {
                HAL_LOG_ERROR("hailo15_isp_set_3dnr_config: isp_3dnr_strength=%d failed", val);
                return HAL_ERROR;
            }
        }
        (void)video_ctx;
        return HAL_OK;
    }
    catch (...)
    {
        return HAL_ERROR;
    }
}

/* ---- M2: AE statistics ---- */

static int hailo15_isp_get_ae_stats(void *video_ctx, HalIspAeStats *stats)
{
    if (!stats)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::memset(stats, 0, sizeof(*stats));
    try
    {
        auto &m = isp_ctrl_mgr();
        wait_safe_to_pull_once(m);
        auto fd_opt = m.get_fd(v4l2::Device::VIDEO0);
        if (!fd_opt.has_value())
        {
            return HAL_ERROR;
        }
        const int fd = *fd_opt.value();
        if (!resolve_ae_stats_ctrls(fd))
        {
            return HAL_ERR_NOT_SUPPORTED;
        }
        /* Ensure AE statistics are enabled (mirrors AF: isp_af_enable gates the
         * stats engine; isp_ae_enable is the AE equivalent) and the histogram
         * window is configured (default is 0x0 = no measurement region, which
         * yields an all-zero histogram). */
        (void)resolve_ae_enable_win_ctrls(fd);
        if (s_ae_en_valid)
        {
            const int32_t one = 1;
            struct v4l2_control c{};
            c.id = s_ae_en_info.id;
            c.value = one;
            (void)ioctl(fd, VIDIOC_S_CTRL, &c);
        }
        if (s_ae_win_valid)
        {
            /* isp_ae_hist_window is u16 dims=[4] = {x, y, w, h}; default all
             * zeros -> driver may treat as "no window". Use 0,0 (full frame
             * implied) only if a size is required; otherwise leave driver
             * default. We write zeros explicitly to match AF window style. */
            uint16_t win[4] = {0, 0, 0, 0};
            (void)v4l2_set_ext_ctrl_blob(fd, s_ae_win_info, win, sizeof(win));
        }
        if (s_ae_hist_valid)
        {
            stats->hist_valid =
                v4l2_get_ext_ctrl_blob(fd, s_ae_hist_info, stats->hist, sizeof(stats->hist));
        }
        if (s_ae_luma_valid)
        {
            /* isp_ae_luma is a u8 x 25 control (element type differs from the
             * u32 histogram) — read bytes, then widen into the HAL array. */
            uint8_t luma8[HAL_ISP_AE_LUMA_GRID];
            if (v4l2_get_ext_ctrl_blob(fd, s_ae_luma_info, luma8, sizeof(luma8)))
            {
                for (int i = 0; i < HAL_ISP_AE_LUMA_GRID; ++i)
                {
                    stats->luma[i] = luma8[i];
                }
                stats->luma_valid = true;
            }
        }
        if (!stats->hist_valid && !stats->luma_valid)
        {
            return HAL_ERROR;
        }
        (void)video_ctx;
        return HAL_OK;
    }
    catch (...)
    {
        return HAL_ERROR;
    }
}

/* ---- M2: HDR exposure ratios ---- */

static int hailo15_isp_set_hdr_ratios(void *video_ctx, float ls_ratio, float vs_ratio)
{
    Hailo15MediaPriv *priv = media_priv_from_video_ctx(video_ctx);
    if (!priv || !priv->media_lib)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (ls_ratio <= 1.0f || ls_ratio > 64.0f)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vs_ratio < 0.0f || vs_ratio > 64.0f)
    {
        return HAL_ERR_INVALID_ARG;
    }
    /* Do not hold priv->mutex across MediaLibrary calls: ML may invoke callbacks that take this lock. */
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    config_profile_t p = prof_exp.value();
    hdr_config_t &hdr = p.iq_settings.hdr;
    if (!hdr.enabled)
    {
        return HAL_ERR_INVALID_STATE;
    }
    hdr.ls_ratio = ls_ratio;
    if (vs_ratio > 0.0f)
    {
        hdr.vs_ratio = vs_ratio;
    }
    media_library_return r = priv->media_lib->set_override_parameters(p);
    return hailo15_ml_err(r);
}

HalIspOps HAL_ISP_OPS = {
    .set_image_config = hailo15_isp_set_image_config,
    .get_current_image_config = hailo15_isp_get_current_image_config,
    .set_manual_config = hailo15_isp_set_manual_config,
    .get_current_manual_config = hailo15_isp_get_current_manual_config,
    .set_exposure_config = hailo15_isp_set_exposure_config,
    .get_current_exposure_config = hailo15_isp_get_current_exposure_config,
    .set_af_windows_config = hailo15_isp_set_af_windows_config,
    .get_af_windows_config = hailo15_isp_get_af_windows_config,
    .subscribe_af_measurement = hailo15_isp_subscribe_af_measurement,
    .wait_af_measurement = hailo15_isp_wait_af_measurement,
    .get_af_measurement = hailo15_isp_get_af_measurement,
    .get_version = hailo15_isp_get_version,
    .set_wb_config = hailo15_isp_set_wb_config,
    .get_current_wb_config = hailo15_isp_get_current_wb_config,
    .set_3dnr_config = hailo15_isp_set_3dnr_config,
    .get_ae_stats = hailo15_isp_get_ae_stats,
    .set_hdr_ratios = hailo15_isp_set_hdr_ratios,
};

} // extern "C"
