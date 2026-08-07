/**
 * @file hailo15_codec_ml.hpp
 * @brief Maps HalCodecConfig <-> MediaLibrary encoder_config_t (aligned with webserver encoder.cpp).
 */
#pragma once

#include "media/hal_codec_internal.h"
#include <hailo/media_library/encoder_config_types.hpp>
#include <hailo/media_library/media_library_types.hpp>

#include <cstring>
#include <optional>
#include <string>
#include <variant>

namespace hailo15::ml
{

inline HalRateControlMode rc_mode_to_hal(rc_mode_t m)
{
    switch (m)
    {
        case CBR:
            return HAL_RC_CBR;
        case VBR:
            return HAL_RC_VBR;
        case CVBR:
            return HAL_RC_CVBR;
        default:
            return HAL_RC_CBR;
    }
}

inline rc_mode_t hal_rc_to_ml(HalRateControlMode m)
{
    switch (m)
    {
        case HAL_RC_VBR:
            return VBR;
        case HAL_RC_CVBR:
            return CVBR;
        case HAL_RC_CBR:
            return CBR;
        case HAL_RC_CQP:
        default:
            return VBR;
    }
}

/**
 * Overwrites ctx->config from the current encoded_output_streams entry (same layout as media init).
 */
inline void fill_hal_codec_config(HalCodecContext *ctx, const std::string &stream_id,
                                  const config_encoded_output_stream_t &enc, void *media_ctx_ptr)
{
    HalCodecConfig *out = &ctx->config;
    std::memset(out, 0, sizeof(*out));
    out->type = HAL_CODEC_TYPE_FROM_MEDIA;
    out->path = nullptr;
    out->media_ptr = media_ctx_ptr;
    out->packet_type = HAL_PACKET_TYPE_H264;
    out->priv = nullptr;

    std::visit(
        [&](auto &&cfg) {
            using T = std::decay_t<decltype(cfg)>;
            out->width = cfg.input_stream.width;
            out->height = cfg.input_stream.height;
            out->framerate = cfg.input_stream.framerate;
            out->format = HAL_PIX_FMT_NV12;
            if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
            {
                out->packet_type =
                    (cfg.output_stream.codec == CODEC_TYPE_HEVC) ? HAL_PACKET_TYPE_H265 : HAL_PACKET_TYPE_H264;
                out->rc_mode = rc_mode_to_hal(cfg.rate_control.rc_mode);
                out->bitrate = cfg.rate_control.bitrate.target_bitrate;
                if (cfg.rate_control.quantization.qp_min.has_value())
                {
                    out->qp_min = *cfg.rate_control.quantization.qp_min;
                }
                if (cfg.rate_control.quantization.qp_max.has_value())
                {
                    out->qp_max = *cfg.rate_control.quantization.qp_max;
                }
                out->gop_size = cfg.gop.gop_size;
                out->intra_pic_rate = cfg.rate_control.intra_pic_rate;
                out->rate_control_gop_length =
                    cfg.rate_control.gop_length.has_value() ? *cfg.rate_control.gop_length : 0U;
            }
            else if constexpr (std::is_same_v<T, jpeg_encoder_config_t>)
            {
                out->packet_type = HAL_PACKET_TYPE_MJPEG;
                out->jpeg_quality = cfg.quality;
            }
        },
        enc.encoding);

    std::strncpy(ctx->codec_name, stream_id.c_str(), sizeof(ctx->codec_name) - 1);
    ctx->codec_fd = -1;
    ctx->status = HAL_STATUS_INITIALIZED;
}

/**
 * Merges user HalCodecConfig into an existing hailo_encoder_config_t (POST /encoder style).
 */
inline void apply_hal_to_hailo_encoder(hailo_encoder_config_t *enc, const HalCodecConfig *hal)
{
    if (hal->width > 0U)
    {
        enc->input_stream.width = hal->width;
    }
    if (hal->height > 0U)
    {
        enc->input_stream.height = hal->height;
    }
    if (hal->framerate > 0U)
    {
        enc->input_stream.framerate = hal->framerate;
    }

    enc->rate_control.rc_mode = hal_rc_to_ml(hal->rc_mode);
    if (hal->bitrate > 0U)
    {
        enc->rate_control.bitrate.target_bitrate = hal->bitrate;
    }

    if (hal->qp_min > 0U)
    {
        enc->rate_control.quantization.qp_min = hal->qp_min;
    }
    if (hal->qp_max > 0U)
    {
        enc->rate_control.quantization.qp_max = hal->qp_max;
    }
    if (hal->qp_hdr != 0)
    {
        enc->rate_control.quantization.qp_hdr = hal->qp_hdr;
    }
    if (hal->intra_qp_delta != 0)
    {
        enc->rate_control.quantization.intra_qp_delta = hal->intra_qp_delta;
    }
    if (hal->fixed_intra_qp != 0U)
    {
        enc->rate_control.quantization.fixed_intra_qp = hal->fixed_intra_qp;
    }

    /* gop_size in HalCodecConfig is ambiguous: callers sometimes set it to the
     * I-frame interval (30, 60) rather than the actual Hantro B-frame hierarchy
     * (valid: 1-8).  If the value exceeds the Hantro limit, treat it as a keyframe
     * interval and route it through intra_pic_rate only — leave enc->gop.gop_size
     * untouched (keeps the value from the loaded medialib profile). */
    const uint32_t gop_struct = hal->gop_size;
    const bool gop_struct_is_valid = (gop_struct > 0U && gop_struct <= 8U);
    const uint32_t intra = hal->intra_pic_rate ? hal->intra_pic_rate
                            : (gop_struct_is_valid ? 0U : gop_struct);
    uint32_t rc_gop = hal->rate_control_gop_length ? hal->rate_control_gop_length : 0U;

    /* MediaLibrary rule: when both are specified, gop_length must be a multiple of intra_pic_rate.
     * If we inherited intra_pic_rate from current config (dynamic change flow) and only gop_size was updated,
     * rc_gop might be derived from gop_size and become invalid (e.g. 2 vs 60). Clamp rc_gop to a valid multiple. */
    if ((intra > 0U) && (rc_gop > 0U) && ((rc_gop % intra) != 0U))
    {
        rc_gop = intra * ((rc_gop + intra - 1U) / intra);
    }

    if (gop_struct_is_valid)
    {
        enc->gop.gop_size = gop_struct;
    }
    if (intra > 0U)
    {
        enc->rate_control.intra_pic_rate = intra;
    }
    if (rc_gop > 0U)
    {
        enc->rate_control.gop_length = rc_gop;
    }

    if ((hal->rc_mode == HAL_RC_CBR) || (hal->rc_mode == HAL_RC_CVBR))
    {
        enc->rate_control.picture_rc = true;
        enc->rate_control.ctb_rc = true;
    }
    else
    {
        enc->rate_control.picture_rc = false;
        enc->rate_control.ctb_rc = false;
    }
}

inline void apply_hal_to_jpeg_encoder(jpeg_encoder_config_t *enc, const HalCodecConfig *hal)
{
    if (hal->jpeg_quality >= 1U && hal->jpeg_quality <= 100U)
    {
        enc->quality = hal->jpeg_quality;
    }
}

} // namespace hailo15::ml
