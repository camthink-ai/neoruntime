/**
 * @file hailo15_medialib_config_resolve.cpp
 * @brief Medialib JSON resolution for HAL_CODEC_TYPE_HW / HAL_VIDEO_TYPE_CSI (V1-aligned).
 */

#include "hailo15_medialib_config_resolve.hpp"
#include "hailo15_common.hpp"
#include "common/hal_log.h"

#include <cstring>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace hailo15::cfg
{
namespace
{

static bool read_file_all(const char *path, std::string *out)
{
    if (!path || !path[0] || !out)
    {
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return !out->empty();
}

static const char *hal_pix_to_medialib_format(HalPixelFormat f)
{
    switch (f)
    {
        case HAL_PIX_FMT_NV12:
            return "NV12";
        case HAL_PIX_FMT_NV21:
            return "NV21";
        case HAL_PIX_FMT_YUYV:
            return "YUYV";
        case HAL_PIX_FMT_RGB24:
            return "RGB24";
        case HAL_PIX_FMT_ARGB32:
        case HAL_PIX_FMT_RGBA32:
            return "RGBA";
        default:
            return "NV12";
    }
}

static const char *hal_rc_to_medialib_rc(HalRateControlMode m)
{
    switch (m)
    {
        case HAL_RC_CBR:
            return "CBR";
        case HAL_RC_CVBR:
            return "CVBR";
        case HAL_RC_VBR:
            return "VBR";
        case HAL_RC_CQP:
        default:
            return "CVBR";
    }
}

static void reverse_fill_codec_from_encoder_json(const nlohmann::json &j, HalCodecConfig *eff)
{
    if (!eff)
    {
        return;
    }
    try
    {
        const auto &encj = j.at("encoding");
        const auto &in = encj.contains("input_stream") ? encj.at("input_stream") : encj;
        eff->width = in.value("width", eff->width ? eff->width : 1920u);
        eff->height = in.value("height", eff->height ? eff->height : 1080u);
        eff->framerate = in.value("framerate", eff->framerate ? eff->framerate : 30u);
        std::string fmt = in.value("format", "NV12");
        if (fmt == "NV12")
        {
            eff->format = HAL_PIX_FMT_NV12;
        }
        else if (fmt == "NV21")
        {
            eff->format = HAL_PIX_FMT_NV21;
        }
        else if (fmt == "YUYV")
        {
            eff->format = HAL_PIX_FMT_YUYV;
        }
        else if (fmt == "RGB24")
        {
            eff->format = HAL_PIX_FMT_RGB24;
        }
        else if (fmt == "RGBA")
        {
            eff->format = HAL_PIX_FMT_RGBA32;
        }
        if (encj.contains("hailo_encoder"))
        {
            const auto &he = encj.at("hailo_encoder");
            const auto &rc = he.at("rate_control");
            if (rc.contains("bitrate") && rc.at("bitrate").is_object())
            {
                eff->bitrate = rc.at("bitrate").value("target_bitrate", eff->bitrate);
            }
            if (rc.contains("gop_length") && rc.at("gop_length").is_number())
            {
                eff->rate_control_gop_length = rc.at("gop_length").get<uint32_t>();
            }
            uint32_t gop_cfg = 0u;
            if (he.contains("gop_config"))
            {
                gop_cfg = he.at("gop_config").value("gop_size", 0u);
            }
            if (gop_cfg > 0u)
            {
                eff->gop_size = gop_cfg;
            }
            if (rc.contains("intra_pic_rate") && rc.at("intra_pic_rate").is_number())
            {
                eff->intra_pic_rate = rc.at("intra_pic_rate").get<uint32_t>();
            }
            else if (eff->intra_pic_rate == 0u)
            {
                eff->intra_pic_rate =
                    eff->gop_size ? eff->gop_size : (eff->framerate ? eff->framerate : 30u);
            }
            if (eff->intra_pic_rate == 0u)
            {
                eff->intra_pic_rate = eff->framerate ? eff->framerate : 30u;
            }
            if (eff->gop_size == 0u)
            {
                eff->gop_size = eff->intra_pic_rate;
            }
            std::string rc_mode = rc.value("rc_mode", "CVBR");
            if (rc_mode == "CBR")
            {
                eff->rc_mode = HAL_RC_CBR;
            }
            else if (rc_mode == "VBR")
            {
                eff->rc_mode = HAL_RC_VBR;
            }
            else if (rc_mode == "CVBR")
            {
                eff->rc_mode = HAL_RC_CVBR;
            }
            if (he.contains("config") && he.at("config").contains("output_stream"))
            {
                std::string codec_str = he.at("config").at("output_stream").value("codec", "CODEC_TYPE_H264");
                if (codec_str == "CODEC_TYPE_HEVC" || codec_str == "CODEC_TYPE_H265")
                {
                    eff->packet_type = HAL_PACKET_TYPE_H265;
                }
                else if (codec_str.find("MJPEG") != std::string::npos)
                {
                    eff->packet_type = HAL_PACKET_TYPE_MJPEG;
                }
                else
                {
                    eff->packet_type = HAL_PACKET_TYPE_H264;
                }
            }
        }
    }
    catch (...)
    {
    }
}

static void patch_encoder_json_from_hal(const HalCodecConfig *cc, nlohmann::json &j)
{
    if (!cc)
    {
        return;
    }
    const char *codec_str = "CODEC_TYPE_H264";
    if (cc->packet_type == HAL_PACKET_TYPE_H265)
    {
        codec_str = "CODEC_TYPE_HEVC";
    }
    else if (cc->packet_type == HAL_PACKET_TYPE_MJPEG)
    {
        codec_str = "CODEC_TYPE_MJPEG";
    }

    auto &enc_node = j.at("encoding").at("hailo_encoder");
    auto &in_node = j.at("encoding").at("input_stream");

    if (cc->width > 0U)
    {
        in_node["width"] = cc->width;
    }
    if (cc->height > 0U)
    {
        in_node["height"] = cc->height;
    }
    if (cc->framerate > 0U)
    {
        in_node["framerate"] = cc->framerate;
    }
    in_node["format"] = hal_pix_to_medialib_format(cc->format);

    enc_node["config"]["output_stream"]["codec"] = codec_str;

    uint32_t bitrate = cc->bitrate ? cc->bitrate : (4u * 1000u * 1000u);
    uint32_t intra = cc->intra_pic_rate ? cc->intra_pic_rate
                                        : (cc->gop_size ? cc->gop_size : (cc->framerate ? cc->framerate : 30u));
    uint32_t gop_interval = cc->gop_size ? cc->gop_size : intra;
    uint32_t gop_cfg_size = (gop_interval <= 8u) ? (gop_interval < 1u ? 1u : gop_interval) : 8u;
    enc_node["gop_config"]["gop_size"] = gop_cfg_size;

    auto &rc = enc_node["rate_control"];
    rc["bitrate"]["target_bitrate"] = bitrate;
    rc["intra_pic_rate"] = intra;
    const uint32_t rc_gop = cc->rate_control_gop_length ? cc->rate_control_gop_length : cc->gop_size;
    uint32_t rc_gop_checked = rc_gop;
    if ((intra > 0U) && (rc_gop_checked > 0U) && ((rc_gop_checked % intra) != 0U))
    {
        rc_gop_checked = intra * ((rc_gop_checked + intra - 1U) / intra);
    }
    if (rc_gop_checked > 0U)
    {
        rc["gop_length"] = rc_gop_checked;
    }
    rc["rc_mode"] = hal_rc_to_medialib_rc(cc->rc_mode);

    try
    {
        auto &db = enc_node["coding_control"]["deblocking_filter"];
        db["tc_offset"] = 0;
        db["beta_offset"] = 0;
    }
    catch (...)
    {
    }
}

/* Embedded default encoder JSON (aligned with hal/codec/hailo15/codec_impl.cpp). */
static const char kDefaultEncoderJson[] = R"HALCFG({
  "version": "4.0.0",
  "metadata": {
    "architecture": "hailo15h",
    "content_hash": "",
    "description": "Embedded default encoder config (HAL v2)",
    "generation_timestamp": "1970-01-01T00:00:00Z"
  },
  "encoding": {
    "hailo_encoder": {
      "coding_control": {
        "deblocking_filter": {
          "beta_offset": 0,
          "deblock_override": false,
          "tc_offset": 0,
          "type": "DEBLOCKING_FILTER_DISABLED"
        },
        "intra_area": { "bottom": 0, "enable": false, "left": 0, "right": 0, "top": 0 },
        "ipcm_area1": { "bottom": 0, "enable": false, "left": 0, "right": 0, "top": 0 },
        "ipcm_area2": { "bottom": 0, "enable": false, "left": 0, "right": 0, "top": 0 },
        "sei_messages": { "encoder_timing_sei": true, "user_metadata_sei": true }
      },
      "config": { "output_stream": { "codec": "CODEC_TYPE_H264" } },
      "gop_config": { "b_frame_qp_delta": 0, "gop_size": 1 },
      "monitors_control": {
        "bitrate_monitor": { "enable": true, "output_result_to_file": false, "period": 3, "result_output_path": "bitrate.txt" },
        "cycle_monitor": { "deviation_threshold": 5, "enable": true, "output_result_to_file": false, "result_output_path": "cycle.txt", "start_delay": 0 }
      },
      "rate_control": {
        "bitrate": { "target_bitrate": 10000000 },
        "gop_anomaly_bitrate_adjuster": { "enable": false },
        "intra_pic_rate": 30,
        "padding": false,
        "picture_rc": true,
        "picture_skip": false,
        "qp_smooth_settings": { "alpha": 0, "q_step_divisor": 2, "qp_delta": 128, "qp_delta_limit": 1536, "qp_delta_limit_step": 384, "qp_delta_step": 128 },
        "quantization": { "qp_hdr": -1 },
        "rc_mode": "CVBR",
        "zoom_bitrate_adjuster": { "mode": "DISABLED" }
      },
      "smart_encoder": { "analytics_labels": [], "background_qp_delta": 10, "enabled": false, "rois": [] }
    },
    "input_stream": { "format": "NV12", "framerate": 30, "height": 1080, "width": 1920 }
  }
})HALCFG";

#ifdef HAILO15_CFG_TEMPLATE_ROOT
static std::string template_file(const char *name)
{
    std::string base = HAILO15_CFG_TEMPLATE_ROOT;
    if (!base.empty() && base.back() != '/')
    {
        base += '/';
    }
    return base + name;
}
#else
static std::string template_file(const char *name)
{
    (void)name;
    return {};
}
#endif

static void patch_frontend_resolution(nlohmann::json &j, const HalVideoConfig *cfg)
{
    if (!cfg ||
        (cfg->width == 0U && cfg->height == 0U && cfg->framerate == 0U && cfg->pool_max_buffers == 0U))
    {
        return;
    }
    try
    {
        if (j.contains("input_video") && j["input_video"].is_object() && j["input_video"].contains("resolution"))
        {
            auto &r = j["input_video"]["resolution"];
            if (cfg->width > 0U)
            {
                r["width"] = cfg->width;
            }
            if (cfg->height > 0U)
            {
                r["height"] = cfg->height;
            }
            if (cfg->framerate > 0U)
            {
                r["framerate"] = cfg->framerate;
            }
        }
        if (j.contains("application_input_streams") && j["application_input_streams"].contains("resolutions") &&
            j["application_input_streams"]["resolutions"].is_array() &&
            !j["application_input_streams"]["resolutions"].empty())
        {
            auto &r0 = j["application_input_streams"]["resolutions"][0];
            if (cfg->width > 0U)
            {
                r0["width"] = cfg->width;
            }
            if (cfg->height > 0U)
            {
                r0["height"] = cfg->height;
            }
            if (cfg->framerate > 0U)
            {
                r0["framerate"] = cfg->framerate;
            }
            if (cfg->pool_max_buffers > 0U)
            {
                r0["pool_max_buffers"] = cfg->pool_max_buffers;
            }
        }
    }
    catch (...)
    {
    }
}

static void reverse_fill_video_from_frontend_json(const nlohmann::json &j, HalVideoConfig *eff)
{
    if (!eff)
    {
        return;
    }
    try
    {
        if (j.contains("input_video") && j["input_video"].contains("resolution"))
        {
            const auto &r = j["input_video"]["resolution"];
            eff->width = r.value("width", eff->width);
            eff->height = r.value("height", eff->height);
            eff->framerate = r.value("framerate", eff->framerate);
        }
        if (j.contains("application_input_streams") && j["application_input_streams"].contains("resolutions") &&
            j["application_input_streams"]["resolutions"].is_array() &&
            !j["application_input_streams"]["resolutions"].empty())
        {
            const auto &r0 = j["application_input_streams"]["resolutions"][0];
            eff->width = r0.value("width", eff->width);
            eff->height = r0.value("height", eff->height);
            eff->framerate = r0.value("framerate", eff->framerate);
            eff->pool_max_buffers = r0.value("pool_max_buffers", eff->pool_max_buffers);
        }
        eff->format = HAL_PIX_FMT_NV12;
    }
    catch (...)
    {
    }
}

} // namespace

int apply_hal_video_to_frontend_json_string(std::string *json_io, const HalVideoConfig *patch)
{
    if (!json_io || !patch)
    {
        return HAL_ERR_INVALID_ARG;
    }
    try
    {
        auto j = nlohmann::json::parse(*json_io, nullptr, false);
        if (j.is_discarded())
        {
            return HAL_ERR_RESULT;
        }
        patch_frontend_resolution(j, patch);
        *json_io = j.dump();
        return HAL_OK;
    }
    catch (...)
    {
        return HAL_ERR_RESULT;
    }
}

bool json_is_full_medialib(const std::string &raw)
{
    try
    {
        auto j = nlohmann::json::parse(raw, nullptr, false);
        return j.is_object() && j.contains("version") && j.contains("profiles");
    }
    catch (...)
    {
        return false;
    }
}

int resolve_hw_encoder_json(const HalCodecConfig *cfg, const Hailo15HalCodecPrivExt *ext, std::string *out_json,
                            HalCodecConfig *effective_out)
{
    if (!cfg || !out_json || !effective_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (cfg->packet_type == HAL_PACKET_TYPE_MJPEG)
    {
        HAL_LOG_ERROR("hailo15 cfg: HAL_CODEC_TYPE_HW MJPEG not implemented (use FROM_MEDIA JPEG path)");
        return HAL_ERR_NOT_SUPPORTED;
    }

    *effective_out = *cfg;
    effective_out->type = HAL_CODEC_TYPE_HW;

    std::string raw;
    if (ext && ext->config_path && ext->config_path[0])
    {
        if (!read_file_all(ext->config_path, &raw))
        {
            HAL_LOG_ERROR("hailo15 cfg: failed to read encoder config_path '%s'", ext->config_path);
            return HAL_ERR_RESULT;
        }
    }
    else if (ext && ext->config_json && ext->config_json[0])
    {
        raw = ext->config_json;
    }
    else
    {
        nlohmann::json j = nlohmann::json::parse(kDefaultEncoderJson, nullptr, false);
        if (j.is_discarded())
        {
            return HAL_ERR_RESULT;
        }
        try
        {
            patch_encoder_json_from_hal(cfg, j);
        }
        catch (const std::exception &e)
        {
            HAL_LOG_ERROR("hailo15 cfg: patch default encoder json failed: %s", e.what());
            return HAL_ERR_RESULT;
        }
        raw = j.dump();
    }

    if (!raw.empty())
    {
        auto j = nlohmann::json::parse(raw, nullptr, false);
        if (!j.is_discarded())
        {
            reverse_fill_codec_from_encoder_json(j, effective_out);
        }
    }

    *out_json = std::move(raw);
    if (out_json->empty())
    {
        return HAL_ERR_INVALID_ARG;
    }
    return HAL_OK;
}

static int pick_csi_config_raw(const HalVideoConfig *cfg, const Hailo15HalVideoPrivExt *ext, bool allow_full_medialib,
                               std::string *raw_out)
{
    (void)cfg;
    raw_out->clear();
    if (ext)
    {
        if (ext->medialib_config_json && ext->medialib_config_json[0])
        {
            *raw_out = ext->medialib_config_json;
        }
        else if (ext->medialib_config_path && ext->medialib_config_path[0] &&
                 read_file_all(ext->medialib_config_path, raw_out))
        {
        }
        else if (ext->frontend_config_json && ext->frontend_config_json[0])
        {
            *raw_out = ext->frontend_config_json;
        }
        else if (ext->frontend_config_path && ext->frontend_config_path[0] &&
                 read_file_all(ext->frontend_config_path, raw_out))
        {
        }
        else if (ext->legacy_config_json && ext->legacy_config_json[0])
        {
            *raw_out = ext->legacy_config_json;
        }
        else if (ext->legacy_config_path && ext->legacy_config_path[0] &&
                 read_file_all(ext->legacy_config_path, raw_out))
        {
        }
    }

    if (raw_out->empty())
    {
#ifdef HAILO15_CFG_TEMPLATE_ROOT
        if (allow_full_medialib)
        {
            std::string ml = template_file("medialib_config.json");
            if (!ml.empty())
            {
                (void)read_file_all(ml.c_str(), raw_out);
            }
        }
        if (raw_out->empty())
        {
            std::string fe = template_file("frontend_config_example.json");
            if (!fe.empty())
            {
                (void)read_file_all(fe.c_str(), raw_out);
            }
        }
#endif
    }
    return raw_out->empty() ? HAL_ERR_INVALID_ARG : HAL_OK;
}

int resolve_csi_frontend_json(const HalVideoConfig *cfg, const Hailo15HalVideoPrivExt *ext, std::string *out_json,
                              HalVideoConfig *effective_out)
{
    if (!cfg || !out_json || !effective_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *effective_out = *cfg;
    effective_out->type = HAL_VIDEO_TYPE_CSI;

    std::string raw;
    int pr = pick_csi_config_raw(cfg, ext, false, &raw);
    if (pr != HAL_OK)
    {
        return pr;
    }
    if (json_is_full_medialib(raw))
    {
        HAL_LOG_ERROR("hailo15 cfg: full medialib JSON not allowed for CSI FRONTEND_ONLY; use HalMedia + FROM_MEDIA");
        return HAL_ERR_INVALID_ARG;
    }

    auto j = nlohmann::json::parse(raw, nullptr, false);
    if (j.is_discarded())
    {
        return HAL_ERR_RESULT;
    }
    patch_frontend_resolution(j, cfg);
    *out_json = j.dump();
    reverse_fill_video_from_frontend_json(j, effective_out);
    return HAL_OK;
}

int resolve_csi_medialib_json(const HalVideoConfig *cfg, const Hailo15HalVideoPrivExt *ext, std::string *out_json,
                              HalVideoConfig *effective_out)
{
    if (!cfg || !out_json || !effective_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *effective_out = *cfg;
    effective_out->type = HAL_VIDEO_TYPE_CSI;

    std::string raw;
    int pr = pick_csi_config_raw(cfg, ext, true, &raw);
    if (pr != HAL_OK)
    {
        return pr;
    }
    if (!json_is_full_medialib(raw))
    {
        HAL_LOG_ERROR("hailo15 cfg: medialib full pipeline expects root JSON with profiles + version");
        return HAL_ERR_INVALID_ARG;
    }
    *out_json = std::move(raw);
    /* Best-effort: dimensions may live inside active profile; leave effective from HalVideoConfig. */
    return HAL_OK;
}

} // namespace hailo15::cfg
