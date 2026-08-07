/**
 * @file hailo15_codec_impl.cpp
 * @brief Hailo-15 HAL codec (FROM_MEDIA + stubs).
 */

#include "hailo15_common.hpp"
#include "hailo15_codec_ml.hpp"
#include "hailo15_hal_video_codec_ext.h"
#include "hailo15_media_priv.hpp"
#include "hailo15_medialib_config_resolve.hpp"
#include "hailo15_video_ml.hpp"

#include "media/hal_codec_internal.h"
#include "media/hal_media.h"

#include <hailo/media_library/encoder.hpp>
#include <hailo/media_library/files_utils.hpp>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <variant>

#include <nlohmann/json.hpp>

namespace
{

HalCodecContext *ctx_ptr(void *codec_ctx)
{
    return static_cast<HalCodecContext *>(codec_ctx);
}

Hailo15MediaPriv *media_priv_from_codec(HalCodecContext *cc)
{
    if (!cc || cc->config.type != HAL_CODEC_TYPE_FROM_MEDIA || !cc->config.media_ptr)
    {
        return nullptr;
    }
    return hailo15_media_priv_from_hal(cc->config.media_ptr);
}

struct Hailo15HwCodecPriv
{
    MediaLibraryEncoderPtr encoder;
    std::mutex mutex;
    HalCodecFrameCallback callback{nullptr};
    void *userdata{nullptr};
    std::string stream_id;
    std::string stored_json;
    HalCodecConfig effective_config{};
    bool started{false};
};

static Hailo15HwCodecPriv *hw_priv(HalCodecContext *cc)
{
    if (!cc || cc->config.type != HAL_CODEC_TYPE_HW || !cc->priv)
    {
        return nullptr;
    }
    return static_cast<Hailo15HwCodecPriv *>(cc->priv);
}

static int hw_codec_init(const HalCodecConfig *config, void **codec_ctx_return)
{
    const Hailo15HalCodecPrivExt *ext = static_cast<const Hailo15HalCodecPrivExt *>(config->priv);
    std::string json;
    HalCodecConfig effective{};
    const int rr = hailo15::cfg::resolve_hw_encoder_json(config, ext, &json, &effective);
    if (rr != HAL_OK)
    {
        return rr;
    }
    std::string sid =
        (ext && ext->encoder_stream_id && ext->encoder_stream_id[0]) ? ext->encoder_stream_id : "hal_hw_0";
    auto enc_exp = MediaLibraryEncoder::create(sid);
    if (!enc_exp.has_value())
    {
        return hailo15_ml_err(enc_exp.error());
    }
    MediaLibraryEncoderPtr enc = enc_exp.value();
    enc->add_config_attacher(true);
    media_library_return ret = enc->set_config(json);
    if (ret != MEDIA_LIBRARY_SUCCESS)
    {
        return hailo15_ml_err(ret);
    }
    if (ext && (ext->osd_config_json || ext->osd_config_path))
    {
        std::string osd_json = "{}";
        if (ext->osd_config_json && ext->osd_config_json[0])
        {
            osd_json = ext->osd_config_json;
        }
        else if (ext->osd_config_path)
        {
            auto opt = files_utils::read_string_from_file(ext->osd_config_path);
            if (!opt.has_value())
            {
                return HAL_ERR_RESULT;
            }
            osd_json = opt.value();
        }
        auto osd_blender = enc->get_osd_blender();
        if (osd_blender)
        {
            (void)osd_blender->configure(osd_json);
        }
    }
    auto *cc = static_cast<HalCodecContext *>(std::calloc(1, sizeof(HalCodecContext)));
    if (!cc)
    {
        return HAL_ERR_NO_MEM;
    }
    cc->config = effective;
    cc->config.type = HAL_CODEC_TYPE_HW;
    cc->config.path = nullptr;
    cc->config.media_ptr = nullptr;
    cc->config.priv = nullptr;
    cc->codec_fd = -1;
    cc->status = HAL_STATUS_INITIALIZED;
    std::strncpy(cc->codec_name, sid.c_str(), sizeof(cc->codec_name) - 1);
    cc->codec_name[sizeof(cc->codec_name) - 1] = '\0';
    auto *hp = new Hailo15HwCodecPriv{};
    hp->encoder = std::move(enc);
    hp->stream_id = std::move(sid);
    hp->stored_json = std::move(json);
    hp->effective_config = effective;
    cc->priv = hp;
    *codec_ctx_return = cc;
    return HAL_OK;
}

static int hw_codec_deinit(void *codec_ctx)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (hp->started && hp->encoder)
    {
        (void)hp->encoder->stop();
        hp->started = false;
    }
    hp->encoder.reset();
    delete hp;
    cc->priv = nullptr;
    std::free(cc);
    return HAL_OK;
}

static int hw_codec_start(void *codec_ctx)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp || !hp->encoder)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(hp->mutex);
    if (hp->started)
    {
        return HAL_OK;
    }
    media_library_return r = hp->encoder->start();
    if (r != MEDIA_LIBRARY_SUCCESS)
    {
        return hailo15_ml_err(r);
    }
    hp->started = true;
    cc->status = HAL_STATUS_RUNNING;
    return HAL_OK;
}

static int hw_codec_stop(void *codec_ctx)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp || !hp->encoder)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(hp->mutex);
    if (!hp->started)
    {
        cc->status = HAL_STATUS_STOPPED;
        return HAL_OK;
    }
    media_library_return r = hp->encoder->stop();
    hp->started = false;
    cc->status = HAL_STATUS_STOPPED;
    return hailo15_ml_err(r);
}

static int hw_codec_input_frame(void *codec_ctx, HalFrameBuffer *frame)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp || !hp->encoder || !frame || !frame->priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    auto *fp = static_cast<Hailo15FramePriv *>(frame->priv);
    media_library_return r = hp->encoder->add_buffer(fp->ml_buf);
    return hailo15_ml_err(r);
}

static int hw_codec_subscribe(void *codec_ctx, HalCodecFrameCallback callback, void *userdata)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp || !hp->encoder || !callback)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(hp->mutex);
    hp->callback = callback;
    hp->userdata = userdata;
    media_library_return r = hp->encoder->subscribe(
        [cc, hp](HailoMediaLibraryBufferPtr buf, uint32_t sz) {
            HalCodecFrameCallback cb = nullptr;
            void *ud = nullptr;
            {
                std::lock_guard<std::mutex> lock(hp->mutex);
                cb = hp->callback;
                ud = hp->userdata;
            }
            if (!buf || !cb)
            {
                return;
            }
            (void)buf->sync_start();
            HalPacketType pt = HAL_PACKET_TYPE_H264;
            if (cc->config.packet_type == HAL_PACKET_TYPE_H265)
            {
                pt = HAL_PACKET_TYPE_H265;
            }
            else if (cc->config.packet_type == HAL_PACKET_TYPE_MJPEG)
            {
                pt = HAL_PACKET_TYPE_MJPEG;
            }
            HalPacketBuffer pkt{};
            hailo15_fill_packet_from_buffer(buf, &pkt, sz, pt);
            cb(cc, &pkt, ud);
            (void)buf->sync_end();
        });
    return hailo15_ml_err(r);
}

static int hw_codec_unsubscribe(void *codec_ctx, HalCodecFrameCallback callback)
{
    (void)callback;
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp || !hp->encoder)
    {
        return HAL_ERR_INVALID_ARG;
    }
    /* Do not hold hp->mutex across encoder->unsubscribe(): it may wait for callback thread drain. */
    {
        std::lock_guard<std::mutex> lock(hp->mutex);
        hp->callback = nullptr;
        hp->userdata = nullptr;
    }
    return hailo15_ml_err(hp->encoder->unsubscribe());
}

static int hw_codec_get_status(void *codec_ctx)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp)
    {
        return static_cast<int>(HAL_STATUS_UNINITIALIZED);
    }
    return static_cast<int>(cc->status);
}

static int hw_codec_get_current_config(void *codec_ctx, HalCodecConfig *config)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp || !config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *config = hp->effective_config;
    config->type = HAL_CODEC_TYPE_HW;
    return HAL_OK;
}

// Legacy standalone encoder path — directly calls encoder->set_config() without
// MediaLibrary.  Only used when the codec operates outside a media pipeline
// (e.g. standalone HW encoder tests).  The production path for encoder config
// changes within a media pipeline is hailo15_codec_dynamic_change_config()
// which delegates to set_override_parameters via the MediaLibrary instance.
static int hw_codec_dynamic_change_config(void *codec_ctx, const HalCodecConfig *config)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp || !hp->encoder || !config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    try
    {
        auto j = nlohmann::json::parse(hp->stored_json, nullptr, false);
        if (j.is_discarded())
        {
            return HAL_ERR_RESULT;
        }
        auto &enc_node = j.at("encoding").at("hailo_encoder");
        auto &in_node = j.at("encoding").at("input_stream");

        /* Detect changes that require encoder stop/reconfig/start (resolution, framerate).
         * Bitrate/GOP changes are dynamic and don't need restart. */
        const HalCodecConfig &cur = hp->effective_config;
        const bool res_changed = ((config->width > 0U && config->width != cur.width) ||
                                  (config->height > 0U && config->height != cur.height));
        const bool fps_changed = (config->framerate > 0U && config->framerate != cur.framerate);
        const bool needs_restart = hp->started && (res_changed || fps_changed);

        /* Hold mutex across the entire stop→reconfig→start sequence to prevent
         * concurrent threads from interleaving encoder operations. */
        std::lock_guard<std::mutex> lock(hp->mutex);

        if (needs_restart)
        {
            media_library_return sr = hp->encoder->stop();
            if (sr != MEDIA_LIBRARY_SUCCESS)
            {
                HAL_LOG_WARNING("hw_codec_dynamic_change_config: encoder stop failed (%d) before restart", static_cast<int>(sr));
            }
            hp->started = false;
        }

        if (config->width > 0U)
        {
            in_node["width"] = config->width;
        }
        if (config->height > 0U)
        {
            in_node["height"] = config->height;
        }
        if (config->framerate > 0U)
        {
            in_node["framerate"] = config->framerate;
        }
        if (config->bitrate > 0U)
        {
            enc_node["rate_control"]["bitrate"]["target_bitrate"] = config->bitrate;
        }
        const uint32_t gop_struct = config->gop_size;
        const bool gop_struct_is_valid = (gop_struct > 0U && gop_struct <= 8U);
        const uint32_t intra =
            config->intra_pic_rate ? config->intra_pic_rate
                                    : (gop_struct_is_valid ? 0U : gop_struct);
        if (intra > 0U)
        {
            enc_node["rate_control"]["intra_pic_rate"] = intra;
        }
        uint32_t rc_gop = config->rate_control_gop_length ? config->rate_control_gop_length : 0U;
        uint32_t rc_gop_checked = rc_gop;
        if ((intra > 0U) && (rc_gop_checked > 0U) && ((rc_gop_checked % intra) != 0U))
        {
            rc_gop_checked = intra * ((rc_gop_checked + intra - 1U) / intra);
        }
        if (rc_gop_checked > 0U)
        {
            enc_node["rate_control"]["gop_length"] = rc_gop_checked;
        }
        /* gop_size: only write to gop_config when the value is a valid Hantro B-frame
         * hierarchy (1-8). Larger values are I-frame intervals already routed through
         * intra_pic_rate above. */
        if (gop_struct_is_valid)
        {
            enc_node["gop_config"]["gop_size"] = gop_struct;
        }
        std::string new_json = j.dump();
        media_library_return r = hp->encoder->set_config(new_json);
        if (r != MEDIA_LIBRARY_SUCCESS)
        {
            /* If restart was attempted but set_config failed, try to recover. */
            if (needs_restart)
            {
                media_library_return recov = hp->encoder->start();
                if (recov == MEDIA_LIBRARY_SUCCESS)
                {
                    hp->started = true;
                }
            }
            return hailo15_ml_err(r);
        }

        /* Restart encoder after successful reconfig for resolution/framerate changes. */
        if (needs_restart)
        {
            media_library_return sr = hp->encoder->start();
            if (sr != MEDIA_LIBRARY_SUCCESS)
            {
                return hailo15_ml_err(sr);
            }
            hp->started = true;
            cc->status = HAL_STATUS_RUNNING;
        }
        hp->stored_json = std::move(new_json);
        auto jj = nlohmann::json::parse(hp->stored_json, nullptr, false);
        if (!jj.is_discarded())
        {
            HalCodecConfig eff = hp->effective_config;
            try
            {
                const auto &encj = jj.at("encoding");
                const auto &in = encj.contains("input_stream") ? encj.at("input_stream") : encj;
                eff.width = in.value("width", eff.width);
                eff.height = in.value("height", eff.height);
                eff.framerate = in.value("framerate", eff.framerate);
                if (encj.contains("hailo_encoder"))
                {
                    const auto &he = encj.at("hailo_encoder");
                    const auto &rc = he.at("rate_control");
                    eff.bitrate = rc.value("bitrate", nlohmann::json::object()).value("target_bitrate", eff.bitrate);
                    if (rc.contains("gop_length") && rc.at("gop_length").is_number())
                    {
                        eff.rate_control_gop_length = rc.at("gop_length").get<uint32_t>();
                    }
                    if (he.contains("gop_config"))
                    {
                        eff.gop_size = he.at("gop_config").value("gop_size", eff.gop_size);
                    }
                    if (rc.contains("intra_pic_rate") && rc.at("intra_pic_rate").is_number())
                    {
                        eff.intra_pic_rate = rc.at("intra_pic_rate").get<uint32_t>();
                    }
                    else if (eff.intra_pic_rate == 0u)
                    {
                        eff.intra_pic_rate =
                            eff.gop_size ? eff.gop_size : (eff.framerate ? eff.framerate : 30u);
                    }
                    if (eff.intra_pic_rate == 0u)
                    {
                        eff.intra_pic_rate = eff.framerate ? eff.framerate : 30u;
                    }
                    if (eff.gop_size == 0u)
                    {
                        eff.gop_size = eff.intra_pic_rate;
                    }
                }
            }
            catch (...)
            {
            }
            hp->effective_config = eff;
            cc->config = eff;
            cc->config.type = HAL_CODEC_TYPE_HW;
        }
        return HAL_OK;
    }
    catch (...)
    {
        return HAL_ERR_RESULT;
    }
}

} // namespace

extern "C" {

static int hailo15_codec_init(const HalCodecConfig *config, void **codec_ctx_return)
{
    if (!config || !codec_ctx_return)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (config->type == HAL_CODEC_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    if (config->type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_init(config, codec_ctx_return);
    }
    return HAL_ERR_NOT_IMPLEMENTED;
}

static int hailo15_codec_deinit(void *codec_ctx)
{
    if (!codec_ctx)
    {
        return HAL_ERR_INVALID_ARG;
    }
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_deinit(codec_ctx);
    }
    return HAL_ERR_NOT_IMPLEMENTED;
}

static int hailo15_codec_start(void *codec_ctx)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_start(codec_ctx);
    }
    Hailo15MediaPriv *priv = media_priv_from_codec(cc);
    if (!cc || !priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (HAL_MEDIA_OPS.start)
    {
        return HAL_MEDIA_OPS.start(cc->config.media_ptr);
    }
    return HAL_ERR_NOT_INITIALIZED;
}

static int hailo15_codec_stop(void *codec_ctx)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_stop(codec_ctx);
    }
    Hailo15MediaPriv *priv = media_priv_from_codec(cc);
    if (!cc || !priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (HAL_MEDIA_OPS.stop)
    {
        return HAL_MEDIA_OPS.stop(cc->config.media_ptr);
    }
    return HAL_ERR_NOT_INITIALIZED;
}

static int hailo15_codec_input_frame(void *codec_ctx, HalFrameBuffer *frame)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_input_frame(codec_ctx, frame);
    }
    Hailo15MediaPriv *priv = media_priv_from_codec(cc);
    if (!cc || !priv || !frame || !frame->priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::string eid = cc->codec_name;
    /* If feeding is suspended (a resolution change is in progress), drop the frame instead of
     * pushing it into the encoder. A stride-mismatched buffer fed while the encoder is being
     * reconfigured triggers update_stride() -> VCEnc -3 and stalls the encoder (see
     * apply_frontend_stream_override). The window is brief; a few dropped frames are expected. */
    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        const auto sus_it = priv->encoder_feed_suspended.find(eid);
        if (sus_it != priv->encoder_feed_suspended.end() && sus_it->second)
        {
            return HAL_OK; /* dropped: encoder is being reconfigured */
        }
    }
    auto enc_it = priv->media_lib->m_encoders.find(eid);
    if (enc_it == priv->media_lib->m_encoders.end())
    {
        return HAL_ERR_INVALID_STATE;
    }
    auto *fp = static_cast<Hailo15FramePriv *>(frame->priv);
    media_library_return r = enc_it->second->add_buffer(fp->ml_buf);
    return hailo15_ml_err(r);
}

static int hailo15_codec_subscribe(void *codec_ctx, HalCodecFrameCallback callback, void *userdata)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_subscribe(codec_ctx, callback, userdata);
    }
    Hailo15MediaPriv *priv = media_priv_from_codec(cc);
    if (!cc || !priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::string eid = cc->codec_name;
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    priv->codec_packet_subscribers[eid] = {callback, userdata};
    return HAL_OK;
}

static int hailo15_codec_unsubscribe(void *codec_ctx, HalCodecFrameCallback callback)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_unsubscribe(codec_ctx, callback);
    }
    (void)callback;
    Hailo15MediaPriv *priv = media_priv_from_codec(cc);
    if (!cc || !priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::string eid = cc->codec_name;
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    priv->codec_packet_subscribers.erase(eid);
    return HAL_OK;
}

static int hailo15_codec_release_packet(void *codec_ctx, HalPacketBuffer *packet)
{
    (void)codec_ctx;
    if (packet && packet->priv)
    {
        delete static_cast<Hailo15PacketPriv *>(packet->priv);
        packet->priv = nullptr;
    }
    return HAL_OK;
}

static int hailo15_codec_get_status(void *codec_ctx)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (!cc)
    {
        return static_cast<int>(HAL_STATUS_UNINITIALIZED);
    }
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_get_status(codec_ctx);
    }
    if (cc->config.type == HAL_CODEC_TYPE_FROM_MEDIA && cc->config.media_ptr && HAL_MEDIA_OPS.get_status)
    {
        return HAL_MEDIA_OPS.get_status(cc->config.media_ptr);
    }
    return static_cast<int>(cc->status);
}

static int hailo15_codec_get_current_config(void *codec_ctx, HalCodecConfig *config)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_get_current_config(codec_ctx, config);
    }
    if (!cc || !config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (cc->config.type == HAL_CODEC_TYPE_FROM_MEDIA)
    {
        Hailo15MediaPriv *priv = media_priv_from_codec(cc);
        if (!priv || !priv->media_lib)
        {
            return HAL_ERR_INVALID_ARG;
        }
        /* Do not hold priv->mutex across MediaLibrary calls: ML may invoke callbacks that take this lock. */
        auto prof_exp = priv->media_lib->get_current_profile();
        if (!prof_exp)
        {
            return HAL_ERROR;
        }
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        hailo15::video_ml::apply_profile_to_codec_ctx(cc, prof_exp.value());
    }
    *config = cc->config;
    return HAL_OK;
}

static int hailo15_codec_dynamic_change_config(void *codec_ctx, const HalCodecConfig *config)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_dynamic_change_config(codec_ctx, config);
    }
    Hailo15MediaPriv *priv = media_priv_from_codec(cc);
    if (!cc || !priv || !config || !priv->media_lib)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (cc->config.type != HAL_CODEC_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }

    const std::string eid = cc->codec_name;
    config_profile_t prof{};
    bool merged = false;
    /* Do not hold priv->mutex across MediaLibrary calls: ML may invoke callbacks that take this lock. */
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    prof = prof_exp.value();
    auto it = prof.encoded_output_streams.find(eid);
    if (it == prof.encoded_output_streams.end())
    {
        return HAL_ERR_INVALID_STATE;
    }

    std::visit(
        [&](auto &&enc) {
            using T = std::decay_t<decltype(enc)>;
            if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
            {
                hailo15::ml::apply_hal_to_hailo_encoder(&enc, config);
                merged = true;
            }
            else if constexpr (std::is_same_v<T, jpeg_encoder_config_t>)
            {
                hailo15::ml::apply_hal_to_jpeg_encoder(&enc, config);
                merged = true;
            }
        },
        it->second.encoding);

    if (!merged)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }

    /* Do not hold priv->mutex across set_override_parameters(): encoder callbacks take this lock (see media_impl). */
    media_library_return r = priv->media_lib->set_override_parameters(prof);
    if (r != MEDIA_LIBRARY_SUCCESS)
    {
        return hailo15_ml_err(r);
    }

    auto prof2 = priv->media_lib->get_current_profile();
    if (!prof2)
    {
        return HAL_ERROR;
    }
    auto it2 = prof2->encoded_output_streams.find(eid);
    if (it2 == prof2->encoded_output_streams.end())
    {
        return HAL_ERR_INVALID_STATE;
    }
    void *mp = cc->config.media_ptr;
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    hailo15::ml::fill_hal_codec_config(cc, eid, it2->second, mp);
    return HAL_OK;
}

static const char *hailo15_codec_get_version(void)
{
    return "Hailo15 HAL-CODEC 2.0.0";
}

HalCodecOps HAL_CODEC_OPS = {
    .init = hailo15_codec_init,
    .deinit = hailo15_codec_deinit,
    .start = hailo15_codec_start,
    .stop = hailo15_codec_stop,
    .input_frame = hailo15_codec_input_frame,
    .subscribe = hailo15_codec_subscribe,
    .unsubscribe = hailo15_codec_unsubscribe,
    .release_packet = hailo15_codec_release_packet,
    .get_status = hailo15_codec_get_status,
    .get_current_config = hailo15_codec_get_current_config,
    .dynamic_change_config = hailo15_codec_dynamic_change_config,
    .get_version = hailo15_codec_get_version,
};

} // extern "C"
