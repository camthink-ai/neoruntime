/**
 * @file hailo15_video_impl.cpp
 * @brief Hailo-15 HAL video (FROM_MEDIA + HAL_VIDEO_TYPE_CSI standalone paths).
 */

#include "hailo15_common.hpp"
#include "hailo15_hal_video_codec_ext.h"
#include "hailo15_media_priv.hpp"
#include "hailo15_ml_frontend_bridge.hpp"
#include "hailo15_medialib_config_resolve.hpp"
#include "hailo15_video_ml.hpp"

#include "common/hal_log.h"
#include "media/hal_media.h"
#include "media/hal_video_internal.h"

#include <hailo/media_library/frontend.hpp>
#include <hailo/media_library/media_library.hpp>
#include <hailo/media_library/sensor_registry.hpp>

#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{

HalVideoContext *ctx_ptr(void *video_ctx)
{
    return static_cast<HalVideoContext *>(video_ctx);
}

Hailo15MediaPriv *media_priv_from_video(HalVideoContext *vc)
{
    if (!vc || vc->config.type != HAL_VIDEO_TYPE_FROM_MEDIA || !vc->config.media_ptr)
    {
        return nullptr;
    }
    return hailo15_media_priv_from_hal(vc->config.media_ptr);
}

namespace
{
// Hailo-15 encoder (Hantro VC8000e) supports resolutions up to 3840x2160, and the medialib's
// encoder preset table (encoder_presets.csv) only carries envelopes up to that size. A
// resolution outside this range (in either orientation) matches no preset -> the medialib logs
// "No preset found for rc_mode: ... width: ... height: ..." on every frame, the encoder never
// configures, never drains its input, and the frontend multi_resize_output pool exhausts.
// Validate in the HAL so invalid resolutions are rejected before reaching the media library.
constexpr uint32_t kHailo15EncMaxSideLong = 3840;  // 4K long  side
constexpr uint32_t kHailo15EncMaxSideShort = 2160; // 4K short side

bool is_supported_encoder_resolution(uint32_t w, uint32_t h)
{
    if (w == 0U || h == 0U)
    {
        return false;
    }
    // The preset lookup allows width/height to be swapped (landscape or portrait), so accept
    // either orientation as long as each side fits within the supported envelope.
    return (w <= kHailo15EncMaxSideLong && h <= kHailo15EncMaxSideShort) ||
           (w <= kHailo15EncMaxSideShort && h <= kHailo15EncMaxSideLong);
}
} // namespace

struct Hailo15CsiVideoPriv
{
    Hailo15CsiPipelineMode mode{HAILO15_CSI_PIPELINE_FRONTEND_ONLY};
    MediaLibraryFrontendPtr frontend;
    MediaLibraryPtr media_lib;
    std::recursive_mutex mutex;
    std::map<std::string, std::pair<HalVideoFrameCallback, void *>> video_subscribers;
    std::vector<std::string> frontend_stream_ids;
    std::string stored_json;
    uint64_t frame_seq{0};
    bool started{false};
    bool fe_bridge_installed{false};
};

static Hailo15CsiVideoPriv *csi_priv(HalVideoContext *vc)
{
    if (!vc || vc->config.type != HAL_VIDEO_TYPE_CSI || !vc->priv)
    {
        return nullptr;
    }
    return static_cast<Hailo15CsiVideoPriv *>(vc->priv);
}

static int csi_video_init(const HalVideoConfig *config, void **video_ctx_return)
{
    const Hailo15HalVideoPrivExt *ext = static_cast<const Hailo15HalVideoPrivExt *>(config->priv);
    Hailo15CsiPipelineMode mode = HAILO15_CSI_PIPELINE_FRONTEND_ONLY;
    if (ext)
    {
        mode = ext->csi_pipeline_mode;
    }

    auto *priv = new Hailo15CsiVideoPriv{};
    priv->mode = mode;

    HalVideoConfig effective{};
    std::string json;
    if (mode == HAILO15_CSI_PIPELINE_MEDIALIB_FULL)
    {
        const int rr = hailo15::cfg::resolve_csi_medialib_json(config, ext, &json, &effective);
        if (rr != HAL_OK)
        {
            delete priv;
            return rr;
        }
        auto ml_exp = MediaLibrary::create();
        if (!ml_exp.has_value())
        {
            delete priv;
            return hailo15_ml_err(ml_exp.error());
        }
        priv->media_lib = ml_exp.value();
        media_library_return ini = priv->media_lib->initialize(json);
        if (ini != MEDIA_LIBRARY_SUCCESS)
        {
            delete priv;
            return hailo15_ml_err(ini);
        }
        auto prof_exp = priv->media_lib->get_current_profile();
        if (!prof_exp)
        {
            delete priv;
            return HAL_ERROR;
        }
        for (const auto &res : prof_exp->application_settings.application_input_streams.resolutions)
        {
            if (!res.stream_id.empty())
            {
                priv->frontend_stream_ids.push_back(res.stream_id);
            }
        }
        if (priv->frontend_stream_ids.empty())
        {
            delete priv;
            return HAL_ERR_INVALID_STATE;
        }
    }
    else
    {
        const int rr = hailo15::cfg::resolve_csi_frontend_json(config, ext, &json, &effective);
        if (rr != HAL_OK)
        {
            delete priv;
            return rr;
        }
        auto fe_exp = MediaLibraryFrontend::create();
        if (!fe_exp.has_value())
        {
            delete priv;
            return hailo15_ml_err(fe_exp.error());
        }
        priv->frontend = fe_exp.value();
        media_library_return sc = priv->frontend->set_config(json);
        if (sc != MEDIA_LIBRARY_SUCCESS)
        {
            delete priv;
            return hailo15_ml_err(sc);
        }
        auto outs = priv->frontend->get_outputs_streams();
        if (!outs.has_value())
        {
            delete priv;
            return hailo15_ml_err(outs.error());
        }
        for (const auto &o : outs.value())
        {
            priv->frontend_stream_ids.push_back(o.id);
        }
        if (priv->frontend_stream_ids.empty())
        {
            delete priv;
            return HAL_ERR_INVALID_STATE;
        }
    }

    priv->stored_json = std::move(json);
    auto *vc = static_cast<HalVideoContext *>(std::calloc(1, sizeof(HalVideoContext)));
    if (!vc)
    {
        delete priv;
        return HAL_ERR_NO_MEM;
    }
    vc->config = effective;
    vc->config.type = HAL_VIDEO_TYPE_CSI;
    vc->config.path = nullptr;
    vc->config.media_ptr = nullptr;
    vc->config.priv = nullptr;
    vc->video_fd = -1;
    vc->status = HAL_STATUS_INITIALIZED;
    std::strncpy(vc->video_name, priv->frontend_stream_ids[0].c_str(), sizeof(vc->video_name) - 1);
    vc->video_name[sizeof(vc->video_name) - 1] = '\0';
    vc->priv = priv;
    *video_ctx_return = vc;
    return HAL_OK;
}

static int csi_video_deinit(void *video_ctx)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    Hailo15CsiVideoPriv *p = csi_priv(vc);
    if (!vc || !p)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (p->started)
    {
        if (p->mode == HAILO15_CSI_PIPELINE_FRONTEND_ONLY && p->frontend)
        {
            (void)p->frontend->stop();
        }
        else if (p->media_lib)
        {
            (void)p->media_lib->stop_pipeline();
        }
        p->started = false;
    }
    p->frontend.reset();
    p->media_lib.reset();
    delete p;
    vc->priv = nullptr;
    std::free(vc);
    return HAL_OK;
}

static int csi_video_start(void *video_ctx)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    Hailo15CsiVideoPriv *p = csi_priv(vc);
    if (!vc || !p)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (p->started)
    {
        return HAL_OK;
    }
    if (p->mode == HAILO15_CSI_PIPELINE_FRONTEND_ONLY)
    {
        if (!p->frontend)
        {
            return HAL_ERR_INVALID_STATE;
        }
        FrontendCallbacksMap fe_map;
        for (const auto &sid : p->frontend_stream_ids)
        {
            fe_map[sid] = [p, vc, sid](HailoMediaLibraryBufferPtr buf, uint32_t sz) {
                (void)sz;
                if (!buf)
                {
                    return;
                }
                (void)buf->sync_start();
                HalVideoFrameCallback cb = nullptr;
                void *cb_ud = nullptr;
                uint64_t seq = 0;
                {
                    /* Never hold p->mutex across user callbacks. */
                    std::lock_guard<std::recursive_mutex> lk(p->mutex);
                    p->frame_seq++;
                    seq = p->frame_seq;
                    const auto vsub = p->video_subscribers.find(sid);
                    if (vsub != p->video_subscribers.end())
                    {
                        cb = vsub->second.first;
                        cb_ud = vsub->second.second;
                    }
                }
                if (cb)
                {
                    HalFrameBuffer frame{};
                    hailo15_fill_frame_from_buffer(buf, &frame);
                    frame.sequence = static_cast<uint32_t>(seq);
                    cb(vc, &frame, cb_ud);
                }
                (void)buf->sync_end();
            };
        }
        media_library_return r = p->frontend->subscribe(fe_map);
        if (r != MEDIA_LIBRARY_SUCCESS)
        {
            return hailo15_ml_err(r);
        }
        r = p->frontend->start();
        if (r != MEDIA_LIBRARY_SUCCESS)
        {
            return hailo15_ml_err(r);
        }
    }
    else
    {
        if (!p->media_lib)
        {
            return HAL_ERR_INVALID_STATE;
        }
        const int cf = hailo15_connect_csi_medialib_frontend(p->media_lib, p->frontend_stream_ids, p->mutex,
                                                           &p->frame_seq, p->video_subscribers, vc);
        if (cf != HAL_OK)
        {
            return cf;
        }
        p->fe_bridge_installed = true;
        media_library_return r = p->media_lib->start_pipeline();
        if (r != MEDIA_LIBRARY_SUCCESS)
        {
            return hailo15_ml_err(r);
        }
    }
    p->started = true;
    vc->status = HAL_STATUS_RUNNING;
    return HAL_OK;
}

static int csi_video_stop(void *video_ctx)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    Hailo15CsiVideoPriv *p = csi_priv(vc);
    if (!vc || !p)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (!p->started)
    {
        vc->status = HAL_STATUS_STOPPED;
        return HAL_OK;
    }
    if (p->mode == HAILO15_CSI_PIPELINE_FRONTEND_ONLY && p->frontend)
    {
        (void)p->frontend->stop();
    }
    else if (p->media_lib)
    {
        (void)p->media_lib->stop_pipeline();
    }
    p->started = false;
    p->fe_bridge_installed = false;
    vc->status = HAL_STATUS_STOPPED;
    return HAL_OK;
}

/** Apply HalVideoConfig fields to standalone frontend JSON and push to MediaLibraryFrontend (no mutex during set_config). */
static int csi_frontend_push_json_patch(Hailo15CsiVideoPriv *p, HalVideoContext *vc, HalVideoConfig &patch)
{
    if (!p || !vc || !p->frontend || p->stored_json.empty())
    {
        return HAL_ERR_INVALID_STATE;
    }
    std::string jcopy;
    {
        std::lock_guard<std::recursive_mutex> lock(p->mutex);
        jcopy = p->stored_json;
    }
    const int rr = hailo15::cfg::apply_hal_video_to_frontend_json_string(&jcopy, &patch);
    if (rr != HAL_OK)
    {
        return rr;
    }
    const media_library_return ret = p->frontend->set_config(jcopy);
    if (ret != MEDIA_LIBRARY_SUCCESS)
    {
        return hailo15_ml_err(ret);
    }
    {
        std::lock_guard<std::recursive_mutex> lock(p->mutex);
        p->stored_json = std::move(jcopy);
    }
    if (patch.width > 0U)
    {
        vc->config.width = patch.width;
    }
    if (patch.height > 0U)
    {
        vc->config.height = patch.height;
    }
    if (patch.framerate > 0U)
    {
        vc->config.framerate = patch.framerate;
    }
    if (patch.pool_max_buffers > 0U)
    {
        vc->config.pool_max_buffers = patch.pool_max_buffers;
    }
    return HAL_OK;
}

static int csi_video_subscribe_stream(void *video_ctx, const char *stream_name, HalVideoFrameCallback callback,
                                      void *userdata)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    Hailo15CsiVideoPriv *p = csi_priv(vc);
    if (!vc || !p)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::string key = stream_name ? stream_name : "";
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->video_subscribers[key] = {callback, userdata};
    return HAL_OK;
}

static int csi_video_unsubscribe_stream(void *video_ctx, const char *stream_name)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    Hailo15CsiVideoPriv *p = csi_priv(vc);
    if (!vc || !p)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::string key = stream_name ? stream_name : "";
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    p->video_subscribers.erase(key);
    return HAL_OK;
}

} // namespace

extern "C" {

static int hailo15_video_init(const HalVideoConfig *config, void **video_ctx_return)
{
    if (!config || !video_ctx_return)
    {
        return HAL_ERR_INVALID_ARG;
    }
    /* Video contexts for the media pipeline are allocated in hailo15_media_impl (get_video_list). */
    if (config->type == HAL_VIDEO_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    if (config->type == HAL_VIDEO_TYPE_CSI)
    {
        return csi_video_init(config, video_ctx_return);
    }
    return HAL_ERR_NOT_IMPLEMENTED;
}

static int hailo15_video_deinit(void *video_ctx)
{
    if (!video_ctx)
    {
        return HAL_ERR_INVALID_ARG;
    }
    HalVideoContext *vc = ctx_ptr(video_ctx);
    if (vc->config.type == HAL_VIDEO_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        return csi_video_deinit(video_ctx);
    }
    return HAL_ERR_NOT_IMPLEMENTED;
}

static int hailo15_video_start(void *video_ctx)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        return csi_video_start(video_ctx);
    }
    Hailo15MediaPriv *priv = media_priv_from_video(vc);
    if (!vc || !priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (HAL_MEDIA_OPS.start)
    {
        return HAL_MEDIA_OPS.start(vc->config.media_ptr);
    }
    return HAL_ERR_NOT_INITIALIZED;
}

static int hailo15_video_stop(void *video_ctx)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        return csi_video_stop(video_ctx);
    }
    Hailo15MediaPriv *priv = media_priv_from_video(vc);
    if (!vc || !priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (HAL_MEDIA_OPS.stop)
    {
        return HAL_MEDIA_OPS.stop(vc->config.media_ptr);
    }
    return HAL_ERR_NOT_INITIALIZED;
}

static HalStatus hailo15_video_get_status(void *video_ctx)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    if (!vc)
    {
        return HAL_STATUS_UNINITIALIZED;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        return vc->status;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_FROM_MEDIA && vc->config.media_ptr && HAL_MEDIA_OPS.get_status)
    {
        const int st = HAL_MEDIA_OPS.get_status(vc->config.media_ptr);
        if (st == HAL_STATUS_RUNNING)
        {
            return HAL_STATUS_RUNNING;
        }
        if (st == HAL_STATUS_STOPPED)
        {
            return HAL_STATUS_STOPPED;
        }
    }
    return vc->status;
}

static int hailo15_video_get_current_config(void *video_ctx, HalVideoConfig *config)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    if (!vc || !config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        *config = vc->config;
        return HAL_OK;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_FROM_MEDIA)
    {
        Hailo15MediaPriv *priv = media_priv_from_video(vc);
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
        hailo15::video_ml::apply_profile_to_video_ctx(vc, prof_exp.value(), vc->video_name);
    }
    *config = vc->config;
    return HAL_OK;
}

static int hailo15_video_dynamic_change_resolution(void *video_ctx, uint32_t width, uint32_t height)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    /* Reject unsupported resolutions before they reach the media library — see
     * is_supported_encoder_resolution(). Otherwise the encoder finds no preset, fails to
     * configure on every frame, and the frontend multi_resize_output pool exhausts. */
    if (!vc || !is_supported_encoder_resolution(width, height))
    {
        HAL_LOG_ERROR("hailo15_media: dynamic_change_resolution %ux%u rejected: unsupported "
                      "(max %ux%u, either orientation)",
                      width, height, kHailo15EncMaxSideLong, kHailo15EncMaxSideShort);
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        Hailo15CsiVideoPriv *p = csi_priv(vc);
        if (!vc || !p || width == 0U || height == 0U)
        {
            return HAL_ERR_INVALID_ARG;
        }
        if (p->mode == HAILO15_CSI_PIPELINE_MEDIALIB_FULL && p->media_lib)
        {
            const int r = hailo15::video_ml::apply_frontend_stream_override(
                p->media_lib, vc->video_name, std::make_optional(std::make_pair(width, height)), std::nullopt,
                std::nullopt, std::nullopt, nullptr);
            if (r != HAL_OK)
            {
                return r;
            }
            vc->config.width = width;
            vc->config.height = height;
            return HAL_OK;
        }
        if (p->mode == HAILO15_CSI_PIPELINE_FRONTEND_ONLY && p->frontend && !p->stored_json.empty())
        {
            HalVideoConfig tmp = vc->config;
            tmp.width = width;
            tmp.height = height;
            tmp.framerate = 0U;
            tmp.pool_max_buffers = 0U;
            return csi_frontend_push_json_patch(p, vc, tmp);
        }
        return HAL_ERR_NOT_SUPPORTED;
    }
    Hailo15MediaPriv *priv = media_priv_from_video(vc);
    if (!vc || !priv || width == 0U || height == 0U)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type != HAL_VIDEO_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    /* Light path: medialib's set_override_parameters() handles a frontend output resolution
     * change as a pipeline stop/start and recreates the encoder at the new geometry
     * (to_encoder_config_map() differs, so create_encoders() runs while the pipeline is
     * stopped) — reliable and far cheaper than a full MediaLibrary destroy+recreate. The
     * encoder input dimensions are kept in sync for the matching stream id below. */
    const int r = hailo15::video_ml::apply_frontend_stream_override(
        priv->media_lib, vc->video_name, std::make_optional(std::make_pair(width, height)), std::nullopt,
        std::nullopt, std::nullopt, priv);
    if (r != HAL_OK)
    {
        return r;
    }
    auto *hm = static_cast<HalMediaContext *>(vc->config.media_ptr);
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    hailo15::video_ml::refresh_all_context_configs(priv, hm);
    return HAL_OK;
}

static int hailo15_video_dynamic_change_framerate(void *video_ctx, uint32_t framerate)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    if (!vc || framerate == 0U)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        Hailo15CsiVideoPriv *p = csi_priv(vc);
        if (!p)
        {
            return HAL_ERR_INVALID_ARG;
        }
        if (p->mode == HAILO15_CSI_PIPELINE_MEDIALIB_FULL && p->media_lib)
        {
            const int r = hailo15::video_ml::apply_frontend_stream_override(
                p->media_lib, vc->video_name, std::nullopt, framerate, std::nullopt, std::nullopt, nullptr);
            if (r != HAL_OK)
            {
                return r;
            }
            vc->config.framerate = framerate;
            return HAL_OK;
        }
        if (p->mode == HAILO15_CSI_PIPELINE_FRONTEND_ONLY && p->frontend && !p->stored_json.empty())
        {
            HalVideoConfig tmp{};
            tmp.framerate = framerate;
            return csi_frontend_push_json_patch(p, vc, tmp);
        }
        return HAL_ERR_NOT_SUPPORTED;
    }
    Hailo15MediaPriv *priv = media_priv_from_video(vc);
    if (!priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type != HAL_VIDEO_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    const int r = hailo15::video_ml::apply_frontend_stream_override(priv->media_lib, vc->video_name, std::nullopt,
                                                                    framerate, std::nullopt, std::nullopt);
    if (r != HAL_OK)
    {
        return r;
    }
    auto *hm = static_cast<HalMediaContext *>(vc->config.media_ptr);
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    hailo15::video_ml::refresh_all_context_configs(priv, hm);
    return HAL_OK;
}

static int hailo15_video_dynamic_change_format(void *video_ctx, HalPixelFormat format)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    if (!vc)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        Hailo15CsiVideoPriv *p = csi_priv(vc);
        if (!p)
        {
            return HAL_ERR_INVALID_ARG;
        }
        if (p->mode == HAILO15_CSI_PIPELINE_MEDIALIB_FULL && p->media_lib)
        {
            const int r = hailo15::video_ml::apply_frontend_stream_override(
                p->media_lib, vc->video_name, std::nullopt, std::nullopt, format, std::nullopt, nullptr);
            if (r != HAL_OK)
            {
                return r;
            }
            vc->config.format = format;
            return HAL_OK;
        }
        (void)format;
        return HAL_ERR_NOT_SUPPORTED;
    }
    Hailo15MediaPriv *priv = media_priv_from_video(vc);
    if (!priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type != HAL_VIDEO_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    const int r = hailo15::video_ml::apply_frontend_stream_override(priv->media_lib, vc->video_name, std::nullopt,
                                                                    std::nullopt, format, std::nullopt);
    if (r != HAL_OK)
    {
        return r;
    }
    auto *hm = static_cast<HalMediaContext *>(vc->config.media_ptr);
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    hailo15::video_ml::refresh_all_context_configs(priv, hm);
    return HAL_OK;
}

static int hailo15_video_dynamic_change_pool_max_buffers(void *video_ctx, uint32_t pool_max_buffers)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    if (!vc)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        Hailo15CsiVideoPriv *p = csi_priv(vc);
        if (!p)
        {
            return HAL_ERR_INVALID_ARG;
        }
        if (p->mode == HAILO15_CSI_PIPELINE_MEDIALIB_FULL && p->media_lib)
        {
            const int r = hailo15::video_ml::apply_frontend_stream_override(
                p->media_lib, vc->video_name, std::nullopt, std::nullopt, std::nullopt, pool_max_buffers, nullptr);
            if (r != HAL_OK)
            {
                return r;
            }
            vc->config.pool_max_buffers = pool_max_buffers;
            return HAL_OK;
        }
        if (p->mode == HAILO15_CSI_PIPELINE_FRONTEND_ONLY && p->frontend && !p->stored_json.empty())
        {
            HalVideoConfig tmp{};
            tmp.pool_max_buffers = pool_max_buffers;
            return csi_frontend_push_json_patch(p, vc, tmp);
        }
        return HAL_ERR_NOT_SUPPORTED;
    }
    Hailo15MediaPriv *priv = media_priv_from_video(vc);
    if (!priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type != HAL_VIDEO_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    const int r = hailo15::video_ml::apply_frontend_stream_override(priv->media_lib, vc->video_name, std::nullopt,
                                                                    std::nullopt, std::nullopt, pool_max_buffers);
    if (r != HAL_OK)
    {
        return r;
    }
    auto *hm = static_cast<HalMediaContext *>(vc->config.media_ptr);
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    hailo15::video_ml::refresh_all_context_configs(priv, hm);
    return HAL_OK;
}

static int hailo15_video_subscribe_stream(void *video_ctx, const char *stream_name,
                                          HalVideoFrameCallback callback, void *userdata)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    if (!vc)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        return csi_video_subscribe_stream(video_ctx, stream_name, callback, userdata);
    }
    Hailo15MediaPriv *priv = media_priv_from_video(vc);
    if (!priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::string key = stream_name ? stream_name : "";
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    priv->video_subscribers[key] = {callback, userdata};
    return HAL_OK;
}

static int hailo15_video_unsubscribe_stream(void *video_ctx, const char *stream_name)
{
    HalVideoContext *vc = ctx_ptr(video_ctx);
    if (!vc)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        return csi_video_unsubscribe_stream(video_ctx, stream_name);
    }
    Hailo15MediaPriv *priv = media_priv_from_video(vc);
    if (!priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::string key = stream_name ? stream_name : "";
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    priv->video_subscribers.erase(key);
    return HAL_OK;
}

static int hailo15_video_release_frame(void *video_ctx, HalFrameBuffer *frame)
{
    (void)video_ctx;
    if (frame && frame->priv)
    {
        delete static_cast<Hailo15FramePriv *>(frame->priv);
        frame->priv = nullptr;
    }
    return HAL_OK;
}

static const char *hailo15_video_get_version(void)
{
    return "Hailo15 HAL-VIDEO 2.0.0";
}

static int hailo15_video_get_sensor_module_info(void *video_ctx, uint32_t sensor_index, HalVideoSensorModuleInfo *info)
{
    if (!info)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::memset(info, 0, sizeof(*info));
    info->i2c_bus = -1;
    info->sensor_pixel_format = -1;

    HalVideoContext *vc = ctx_ptr(video_ctx);
    if (!vc)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_CSI)
    {
        Hailo15CsiVideoPriv *p = csi_priv(vc);
        if (!p || p->mode != HAILO15_CSI_PIPELINE_MEDIALIB_FULL)
        {
            return HAL_ERR_NOT_SUPPORTED;
        }
    }
    else if (vc->config.type != HAL_VIDEO_TYPE_FROM_MEDIA)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    if (vc->config.type == HAL_VIDEO_TYPE_FROM_MEDIA && !vc->config.media_ptr)
    {
        return HAL_ERR_INVALID_ARG;
    }

    auto &reg = SensorRegistry::get_instance();

    const auto name_opt = reg.detect_sensor_type_str(static_cast<size_t>(sensor_index));
    if (!name_opt.has_value())
    {
        HAL_LOG_WARNING(
            "get_sensor_module_info: SensorRegistry could not detect sensor (index=%u). "
            "Requires target /sys/class/video4linux v4l-subdev names matching a supported IMX driver, "
            "and correct sensor_index (0/1).",
            static_cast<unsigned>(sensor_index));
        return HAL_ERR_NOT_FOUND;
    }
    std::strncpy(info->sensor_model_name, name_opt->c_str(), sizeof(info->sensor_model_name) - 1);
    info->sensor_model_name[sizeof(info->sensor_model_name) - 1] = '\0';
    info->valid_fields |= HAL_VIDEO_SENSOR_INFO_VALID_MODEL_NAME;

    const auto bus_addr = reg.get_i2c_bus_and_address(static_cast<size_t>(sensor_index));
    if (bus_addr.has_value())
    {
        info->i2c_bus = bus_addr->first;
        std::snprintf(info->i2c_address, sizeof(info->i2c_address), "%s", bus_addr->second.c_str());
        info->valid_fields |= HAL_VIDEO_SENSOR_INFO_VALID_I2C;
    }

    const auto stype = reg.detect_sensor_type(static_cast<size_t>(sensor_index));
    if (stype.has_value())
    {
        const auto caps = reg.get_sensor_capabilities(stype.value());
        if (caps.has_value())
        {
            info->sensor_pixel_format = caps->pixel_format;
            info->valid_fields |= HAL_VIDEO_SENSOR_INFO_VALID_PIXEL_FORMAT;
        }
    }

    return HAL_OK;
}

HalVideoOps HAL_VIDEO_OPS = {
    .init = hailo15_video_init,
    .deinit = hailo15_video_deinit,
    .start = hailo15_video_start,
    .stop = hailo15_video_stop,
    .get_status = hailo15_video_get_status,
    .get_current_config = hailo15_video_get_current_config,
    .dynamic_change_resolution = hailo15_video_dynamic_change_resolution,
    .dynamic_change_framerate = hailo15_video_dynamic_change_framerate,
    .dynamic_change_format = hailo15_video_dynamic_change_format,
    .dynamic_change_pool_max_buffers = hailo15_video_dynamic_change_pool_max_buffers,
    .subscribe_stream = hailo15_video_subscribe_stream,
    .unsubscribe_stream = hailo15_video_unsubscribe_stream,
    .release_frame = hailo15_video_release_frame,
    .get_version = hailo15_video_get_version,
    .get_sensor_module_info = hailo15_video_get_sensor_module_info,
};

} // extern "C"
