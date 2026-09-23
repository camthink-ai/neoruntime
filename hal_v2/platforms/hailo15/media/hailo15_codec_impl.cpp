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

#include <hailo/media_library/config_manager.hpp>
#include <hailo/media_library/dma_memory_allocator.hpp>
#include <hailo/media_library/encoder.hpp>
#include <hailo/media_library/files_utils.hpp>

#include "hailo15_default_medialib.hpp"

#include <dlfcn.h>

#include <atomic>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <variant>
#include <vector>

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
    HalCodecRoiConfig roi_config{};
    bool started{false};
    /* Fed-vs-delivered accounting: a fed frame that never produced a packet
     * is the observable signature of a pipeline bus error — and a bus error
     * leaves the vendor encoder un-joinable (see hw_codec_deinit). */
    std::atomic<uint64_t> frames_in{0};
    std::atomic<uint64_t> frames_out{0};
};

static Hailo15HwCodecPriv *hw_priv(HalCodecContext *cc)
{
    if (!cc || cc->config.type != HAL_CODEC_TYPE_HW || !cc->priv)
    {
        return nullptr;
    }
    return static_cast<Hailo15HwCodecPriv *>(cc->priv);
}

/*
 * Process-lifetime graveyard for errored encoders. Destroying a
 * MediaLibraryEncoder whose GStreamer bus already quit the internal main
 * loop runs the vendor's early-return stop() and then destroys a still
 * joinable std::thread -> std::terminate. Park such instances here
 * instead: the shared_ptr refcount stays alive, the leak is bounded (one
 * per failed context) and identifiable in a core dump.
 */
static std::mutex g_errored_encoders_mu;
static std::vector<MediaLibraryEncoderPtr> g_errored_encoders;

static HailoFormat hal_format_to_hailo(HalPixelFormat f)
{
    switch (f)
    {
        case HAL_PIX_FMT_GRAY8:
            return HAILO_FORMAT_GRAY8;
        case HAL_PIX_FMT_RGB24:
        case HAL_PIX_FMT_BGR24:
            return HAILO_FORMAT_RGB;
        case HAL_PIX_FMT_ARGB32:
        case HAL_PIX_FMT_RGBA32:
            return HAILO_FORMAT_ARGB;
        default:
            return HAILO_FORMAT_NV12;
    }
}

static size_t hailo_plane_rows(HailoFormat fmt, size_t height, uint32_t plane)
{
    switch (fmt)
    {
        case HAILO_FORMAT_NV12:
        case HAILO_FORMAT_A420:
            return (plane == 0U) ? height : (height / 2U);
        default:
            return height;
    }
}

/*
 * Wrap a standalone (non-pipeline) dma-buf HalFrameBuffer into a
 * MediaLibrary buffer so hw_codec_input_frame can feed the encoder.
 * Plane mappings live in DmaMemoryAllocator's external-buffer table;
 * the buffer's release path (owner == nullptr) unmaps them once the
 * last reference — held by the GstBuffer the encoder builds — drops.
 */
static HailoMediaLibraryBufferPtr hailo15_wrap_dmabuf_frame(const HalFrameBuffer *frame)
{
    if (!frame || frame->mem_type != HAL_MEM_DMABUF || frame->num_planes == 0U ||
        frame->num_planes > HAL_MAX_PLANES || frame->width == 0U || frame->height == 0U)
    {
        return nullptr;
    }

    auto &allocator = DmaMemoryAllocator::get_instance();
    const HailoFormat fmt = hal_format_to_hailo(frame->format);
    std::vector<hailo_data_plane_t> planes;
    std::vector<void *> mapped;

    for (uint32_t i = 0; i < frame->num_planes; i++)
    {
        if (frame->dma_fds[i] < 0 || frame->sizes[i] == 0U)
        {
            break;
        }
        void *userptr = nullptr;
        if (allocator.map_external_dma_buffer(frame->sizes[i], static_cast<uint>(frame->dma_fds[i]), &userptr) !=
            MEDIA_LIBRARY_SUCCESS)
        {
            break;
        }
        mapped.push_back(userptr);
        const size_t rows = hailo_plane_rows(fmt, frame->height, i);
        hailo_data_plane_t p{};
        p.userptr = userptr;
        p.fd = frame->dma_fds[i];
        p.bytesperline = frame->strides[i];
        /* Payload bytes: stride × rows when the stride is known, else the
         * imported plane size. */
        p.bytesused = frame->strides[i] ? (frame->strides[i] * rows) : frame->sizes[i];
        planes.push_back(p);
    }

    if (planes.size() != frame->num_planes)
    {
        for (void *ptr : mapped)
        {
            (void)allocator.unmap_external_dma_buffer(ptr);
        }
        return nullptr;
    }

    auto bd = std::make_shared<hailo_buffer_data_t>(frame->width, frame->height, frame->num_planes, fmt,
                                                    HAILO_MEMORY_TYPE_DMABUF, planes);
    auto buf = std::make_shared<hailo_media_library_buffer>();
    if (buf->create(nullptr, bd) != MEDIA_LIBRARY_SUCCESS)
    {
        return nullptr;
    }
    return buf;
}

/*
 * Standalone encoders (no frontend pipeline) build with add_config_attacher=true.
 * With no gst-mode interactor published, encodebin flags the attacher as owner and
 * it registers a dummy-profile interactor — but dummy profiles are hardcoded to
 * SENSOR_0 (from_frontend_config never maps the frontend sensor_index), which
 * collides with the main pipeline's interactor: validate_sensor_index_uniqueness
 * rejects the registration and parse_launch fails.
 *
 * Publish a full-json interactor instead: clone the compiled-in default profile
 * with its sensor_config patched to SENSOR_1, register it via create() (passes the
 * uniqueness check beside the main SENSOR_0 interactor), and store it in the
 * plugin's gst_mode_config_manager_interactor slot — the same global encodebin
 * consults (and frontendbinsrc sets when the GStreamer API owns the frontend).
 * First call per process wins; the interactor is intentionally never freed, since
 * the slot must outlive the encoder and the plugin nulls it only when the
 * attacher itself owns the interactor (not the case here).
 */
/*
 * The medialib GStreamer plugin keeps gst_mode_config_manager_interactor in its own
 * BSS and GStreamer loads plugins RTLD_LOCAL, so the symbol is absent from the
 * global lookup scope — dlsym(RTLD_DEFAULT) cannot see it. Locate the already-loaded
 * plugin through /proc/self/maps and dlopen() the same path with RTLD_NOLOAD (returns
 * the existing object without loading a second copy); dlsym() on that handle yields
 * the same memory the plugin's encodebin/config-attacher bind to internally.
 */
static ConfigManagerInteractor **find_gst_mode_interactor_slot()
{
    std::ifstream maps("/proc/self/maps");
    std::string line;
    while (std::getline(maps, line))
    {
        const auto slash = line.rfind('/');
        if (slash == std::string::npos)
        {
            continue;
        }
        if (line.substr(slash + 1) != "libgstmedialib.so")
        {
            continue;
        }
        void *handle = dlopen(line.substr(line.find('/')).c_str(), RTLD_NOW | RTLD_NOLOAD | RTLD_LOCAL);
        if (handle == nullptr)
        {
            continue;
        }
        return static_cast<ConfigManagerInteractor **>(dlsym(handle, "gst_mode_config_manager_interactor"));
    }
    return nullptr;
}

static bool ensure_standalone_encoder_interactor(std::string *err_out)
{
    auto fail = [&](const std::string &msg) {
        HAL_LOG_ERROR("hailo15_codec: standalone encoder interactor: %s", msg.c_str());
        if (err_out != nullptr)
        {
            *err_out = msg;
        }
        return false;
    };

    ConfigManagerInteractor **slot = find_gst_mode_interactor_slot();
    if (slot == nullptr)
    {
        return fail("gst_mode_config_manager_interactor not found (medialib plugin not loaded)");
    }
    if (*slot != nullptr)
    {
        return true;
    }

    /* Source profile: the compiled-in default container's default profile — the
     * same Daylight-class config the main pipeline uses when the daemon starts
     * without a persisted config (EIS/HDR/denoise off, so the second instance
     * passes validate_multi_instance_restrictions too). */
    std::string container_str;
    std::string merr;
    if (!hailo15::materialize_default_medialib_config(container_str, &merr))
    {
        return fail("default medialib unavailable: " + merr);
    }
    auto container = nlohmann::json::parse(container_str, nullptr, false);
    if (container.is_discarded() || !container.contains("profiles") || !container["profiles"].is_array())
    {
        return fail("failed to parse default medialib container json");
    }
    const std::string default_name = container.value("default_profile", "");
    std::string profile_path;
    for (const auto &entry : container["profiles"])
    {
        if (entry.value("name", "") == default_name)
        {
            profile_path = entry.value("config_file", "");
            break;
        }
    }
    if (profile_path.empty())
    {
        return fail("default profile entry not found in container");
    }

    auto profile_src = files_utils::read_string_from_file(profile_path);
    if (!profile_src.has_value())
    {
        return fail("failed to read profile " + profile_path);
    }
    auto profile = nlohmann::json::parse(profile_src.value(), nullptr, false);
    if (profile.is_discarded() || profile.value("sensor_config", "").empty())
    {
        return fail("failed to parse profile " + profile_path);
    }
    const std::string sensor_path = profile.value("sensor_config", "");

    auto sensor_src = files_utils::read_string_from_file(sensor_path);
    if (!sensor_src.has_value())
    {
        return fail("failed to read sensor config " + sensor_path);
    }
    auto sensor = nlohmann::json::parse(sensor_src.value(), nullptr, false);
    if (sensor.is_discarded() || !sensor.contains("input_video"))
    {
        return fail("failed to parse sensor config " + sensor_path);
    }

    /* Patched copies under a private scratch dir. Only sensor_id changes; the
     * profile's other section refs keep pointing into the shared default bundle
     * (re-extracted on every daemon start). */
    const std::string scratch = "/var/tmp/hal_standalone_encoder";
    sensor["input_video"]["sensor_id"] = "SENSOR_1";
    const std::string patched_sensor_path = scratch + "/sensor_config.json";
    const std::string patched_profile_path = scratch + "/standalone_profile.json";
    if (files_utils::write_string_to_file_atomic(patched_sensor_path, sensor.dump()) != MEDIA_LIBRARY_SUCCESS)
    {
        return fail("failed to write " + patched_sensor_path);
    }
    profile["sensor_config"] = patched_sensor_path;
    if (files_utils::write_string_to_file_atomic(patched_profile_path, profile.dump()) != MEDIA_LIBRARY_SUCCESS)
    {
        return fail("failed to write " + patched_profile_path);
    }

    auto crafted = nlohmann::json{
        {"version", container.value("version", "2.0.0")},
        {"metadata", container.value("metadata", nlohmann::json::object())},
        {"backup_folder_path", scratch + "/backup"},
        {"default_profile", "HAL_STANDALONE"},
        {"profiles", nlohmann::json::array({{{"name", "HAL_STANDALONE"}, {"config_file", patched_profile_path}}})},
    };
    auto interactor_exp = ConfigManagerInteractor::create(crafted.dump());
    if (!interactor_exp.has_value())
    {
        return fail("ConfigManagerInteractor::create failed (" +
                    std::to_string(static_cast<int>(interactor_exp.error())) + ")");
    }
    *slot = interactor_exp.value().release();
    HAL_LOG_INFO("hailo15_codec: standalone encoder interactor registered (profile HAL_STANDALONE, sensor 1)");
    return true;
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
    std::string interactor_err;
    if (!ensure_standalone_encoder_interactor(&interactor_err))
    {
        HAL_LOG_ERROR("hailo15_codec: cannot init standalone encoder: %s", interactor_err.c_str());
        return HAL_ERR_RESULT;
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
    if (hp->encoder)
    {
        /*
         * Vendor hazard: a GStreamer bus error (e.g. caps negotiation
         * failure) quits the encoder's main loop from the bus callback.
         * stop() then early-returns on !is_started() without joining the
         * loop thread, and destroying the encoder tears down a still
         * joinable std::thread -> std::terminate -> daemon SIGABRT.
         *
         * A fed frame that never came back as a packet means that error
         * state is already latched, so in that case deliberately leak the
         * instance (bounded: one per failed context) instead of killing
         * the daemon. Balanced counters mean the pipeline is healthy and
         * stop() runs its full join path — destroy normally.
         */
        if (hp->frames_in.load() != hp->frames_out.load())
        {
            HAL_LOG_WARNING("hailo15_codec: encoder %s in errored state "
                            "(%llu frames in / %llu out) — parking instance "
                            "to avoid vendor terminate-on-destroy",
                            cc->codec_name,
                            (unsigned long long)hp->frames_in.load(),
                            (unsigned long long)hp->frames_out.load());
            std::lock_guard<std::mutex> lk(g_errored_encoders_mu);
            g_errored_encoders.push_back(std::move(hp->encoder));
        }
        else
        {
            if (hp->started)
            {
                (void)hp->encoder->stop();
            }
            hp->encoder.reset();
        }
    }
    hp->started = false;
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
    if (!cc || !hp || !hp->encoder || !frame)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (frame->priv)
    {
        auto *fp = static_cast<Hailo15FramePriv *>(frame->priv);
        media_library_return r = hp->encoder->add_buffer(fp->ml_buf);
        if (r == MEDIA_LIBRARY_SUCCESS)
        {
            hp->frames_in.fetch_add(1, std::memory_order_relaxed);
        }
        return hailo15_ml_err(r);
    }
    /*
     * Standalone frames (e.g. daemon-imported DSP buffers) carry no
     * Hailo15FramePriv: wrap their dma-buf planes instead. The mapping is
     * owned by the wrapped buffer — the encoder's GstBuffer keeps it alive
     * until encode completes, then release() unmaps the planes.
     */
    HailoMediaLibraryBufferPtr wrapped = hailo15_wrap_dmabuf_frame(frame);
    if (!wrapped)
    {
        return HAL_ERR_INVALID_ARG;
    }
    media_library_return r = hp->encoder->add_buffer(wrapped);
    if (r == MEDIA_LIBRARY_SUCCESS)
    {
        hp->frames_in.fetch_add(1, std::memory_order_relaxed);
    }
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
            /* Count every delivered packet even when unsubscribed: the
             * in/out balance is what makes hw_codec_deinit's destroy-or-
             * leak decision, and it must reflect pipeline health. */
            if (buf)
            {
                hp->frames_out.fetch_add(1, std::memory_order_relaxed);
            }
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

namespace
{

/* SmartStream+ constraint (Hailo Media Library 1.12): H.264 + CVBR only. */
static int check_smart_encoder_supported(const HalCodecConfig &cfg)
{
    if (cfg.packet_type != HAL_PACKET_TYPE_H264)
    {
        HAL_LOG_ERROR("hailo15_codec: smart encoder requires H.264 (current packet type %d)",
                      static_cast<int>(cfg.packet_type));
        return HAL_ERR_NOT_SUPPORTED;
    }
    if (cfg.rc_mode != HAL_RC_CVBR)
    {
        HAL_LOG_ERROR("hailo15_codec: smart encoder requires CVBR rate control (current rc_mode %d)",
                      static_cast<int>(cfg.rc_mode));
        return HAL_ERR_NOT_SUPPORTED;
    }
    return HAL_OK;
}

static int validate_roi_config(const HalCodecRoiConfig *config)
{
    if (config->roi_count > HAL_CODEC_ROI_MAX)
    {
        return HAL_ERR_INVALID_ARG;
    }
    /* medialib smart_encoder schema: background_qp_delta [1..15]. */
    if (config->background_qp_delta < 0 || config->background_qp_delta > 15)
    {
        return HAL_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < config->roi_count; ++i)
    {
        const HalCodecRoi &r = config->rois[i];
        if (r.x < 0.0f || r.y < 0.0f || r.w <= 0.0f || r.h <= 0.0f ||
            r.x + r.w > 1.001f || r.y + r.h > 1.001f)
        {
            return HAL_ERR_INVALID_ARG;
        }
    }
    return HAL_OK;
}

static void fill_roi_json(nlohmann::json &node, const HalCodecRoiConfig *config)
{
    node["enabled"] = config->enabled;
    node["background_qp_delta"] = config->background_qp_delta;
    node["analytics_labels"] = nlohmann::json::array(); /* schema-required field */
    nlohmann::json rois = nlohmann::json::array();
    for (uint32_t i = 0; i < config->roi_count; ++i)
    {
        rois.push_back(nlohmann::json{
            {"x", config->rois[i].x},
            {"y", config->rois[i].y},
            {"width", config->rois[i].w},
            {"height", config->rois[i].h},
        });
    }
    node["rois"] = std::move(rois);
}

static int hw_codec_set_roi_config(void *codec_ctx, const HalCodecRoiConfig *config)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp || !hp->encoder || !config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    const int sup = check_smart_encoder_supported(hp->effective_config);
    if (sup != HAL_OK)
    {
        return sup;
    }
    const int val = validate_roi_config(config);
    if (val != HAL_OK)
    {
        return val;
    }
    try
    {
        std::string json_copy;
        {
            std::lock_guard<std::mutex> lock(hp->mutex);
            json_copy = hp->stored_json;
        }
        auto j = nlohmann::json::parse(json_copy, nullptr, false);
        if (j.is_discarded())
        {
            return HAL_ERR_RESULT;
        }
        auto &node = j.at("encoding").at("hailo_encoder")["smart_encoder"];
        fill_roi_json(node, config);

        std::lock_guard<std::mutex> lock(hp->mutex);
        media_library_return r = hp->encoder->set_config(j.dump());
        if (r != MEDIA_LIBRARY_SUCCESS)
        {
            return hailo15_ml_err(r);
        }
        hp->stored_json = j.dump();
        hp->roi_config = *config;
        return HAL_OK;
    }
    catch (...)
    {
        return HAL_ERR_RESULT;
    }
}

static int hw_codec_get_roi_config(void *codec_ctx, HalCodecRoiConfig *config)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp || !config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(hp->mutex);
    *config = hp->roi_config;
    return HAL_OK;
}

static int hw_codec_force_idr(void *codec_ctx)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp || !hp->encoder)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(hp->mutex);
    hp->encoder->force_keyframe();
    return HAL_OK;
}

static int hw_codec_get_stream_stats(void *codec_ctx, HalCodecStreamStats *stats)
{
    auto *cc = ctx_ptr(codec_ctx);
    auto *hp = hw_priv(cc);
    if (!cc || !hp || !hp->encoder || !stats)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::mutex> lock(hp->mutex);
    stats->fps = hp->encoder->get_current_fps();
    stats->bitrate_kbps = 0;
    stats->monitor_period_s = 0;
    if (cc->config.packet_type != HAL_PACKET_TYPE_MJPEG)
    {
        encoder_monitors mon = hp->encoder->get_encoder_monitors();
        if (mon.bitrate_monitor.enabled && mon.bitrate_monitor.ma_bitrate > 0)
        {
            /* ma_bitrate is bytes/s over the moving window (see hailo_encoder_impl.cpp). */
            stats->bitrate_kbps =
                static_cast<uint32_t>((static_cast<uint64_t>(mon.bitrate_monitor.ma_bitrate) * 8U) / 1000U);
            stats->monitor_period_s = mon.bitrate_monitor.period;
        }
    }
    return HAL_OK;
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

static int hailo15_codec_set_roi_config(void *codec_ctx, const HalCodecRoiConfig *config)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_set_roi_config(codec_ctx, config);
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
    const int sup = check_smart_encoder_supported(cc->config);
    if (sup != HAL_OK)
    {
        return sup;
    }
    const int val = validate_roi_config(config);
    if (val != HAL_OK)
    {
        return val;
    }

    const std::string eid = cc->codec_name;
    /* Do not hold priv->mutex across MediaLibrary calls: ML may invoke callbacks that take this lock. */
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    config_profile_t prof = prof_exp.value();
    auto it = prof.encoded_output_streams.find(eid);
    if (it == prof.encoded_output_streams.end())
    {
        return HAL_ERR_INVALID_STATE;
    }

    bool merged = false;
    std::visit(
        [&](auto &&enc) {
            using T = std::decay_t<decltype(enc)>;
            if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
            {
                smart_encoder_config_t &se = enc.smart_encoder;
                se.enabled = config->enabled;
                se.background_qp_delta = static_cast<uint8_t>(config->background_qp_delta);
                se.rois.clear();
                for (uint32_t i = 0; i < config->roi_count; ++i)
                {
                    se.rois.push_back(normalized_roi_t{
                        config->rois[i].x, config->rois[i].y, config->rois[i].w, config->rois[i].h});
                }
                merged = true;
            }
        },
        it->second.encoding);
    if (!merged)
    {
        /* jpeg_encoder_config_t has no smart encoder. */
        return HAL_ERR_NOT_SUPPORTED;
    }

    media_library_return r = priv->media_lib->set_override_parameters(prof);
    if (r != MEDIA_LIBRARY_SUCCESS)
    {
        return hailo15_ml_err(r);
    }
    return HAL_OK;
}

static int hailo15_codec_get_roi_config(void *codec_ctx, HalCodecRoiConfig *config)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_get_roi_config(codec_ctx, config);
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
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    auto it = prof_exp->encoded_output_streams.find(eid);
    if (it == prof_exp->encoded_output_streams.end())
    {
        return HAL_ERR_INVALID_STATE;
    }

    bool merged = false;
    std::visit(
        [&](auto &&enc) {
            using T = std::decay_t<decltype(enc)>;
            if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
            {
                const smart_encoder_config_t &se = enc.smart_encoder;
                std::memset(config, 0, sizeof(*config));
                config->enabled = se.enabled;
                config->background_qp_delta = se.background_qp_delta;
                config->roi_count = 0;
                for (const auto &roi : se.rois)
                {
                    if (config->roi_count >= HAL_CODEC_ROI_MAX)
                    {
                        break;
                    }
                    config->rois[config->roi_count].x = roi.x;
                    config->rois[config->roi_count].y = roi.y;
                    config->rois[config->roi_count].w = roi.width;
                    config->rois[config->roi_count].h = roi.height;
                    config->roi_count++;
                }
                merged = true;
            }
        },
        it->second.encoding);
    if (!merged)
    {
        return HAL_ERR_NOT_SUPPORTED;
    }
    return HAL_OK;
}

static MediaLibraryEncoderPtr from_media_encoder(Hailo15MediaPriv *priv, HalCodecContext *cc)
{
    const std::string eid = cc->codec_name;
    auto it = priv->media_lib->m_encoders.find(eid);
    if (it == priv->media_lib->m_encoders.end())
    {
        return nullptr;
    }
    return it->second;
}

static int hailo15_codec_force_idr(void *codec_ctx)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_force_idr(codec_ctx);
    }
    Hailo15MediaPriv *priv = media_priv_from_codec(cc);
    if (!cc || !priv || !priv->media_lib)
    {
        return HAL_ERR_INVALID_ARG;
    }
    MediaLibraryEncoderPtr enc = from_media_encoder(priv, cc);
    if (!enc)
    {
        return HAL_ERR_INVALID_STATE;
    }
    media_library_return r = enc->force_keyframe();
    return hailo15_ml_err(r);
}

static int hailo15_codec_get_stream_stats(void *codec_ctx, HalCodecStreamStats *stats)
{
    HalCodecContext *cc = ctx_ptr(codec_ctx);
    if (cc->config.type == HAL_CODEC_TYPE_HW)
    {
        return hw_codec_get_stream_stats(codec_ctx, stats);
    }
    Hailo15MediaPriv *priv = media_priv_from_codec(cc);
    if (!cc || !priv || !priv->media_lib || !stats)
    {
        return HAL_ERR_INVALID_ARG;
    }
    MediaLibraryEncoderPtr enc = from_media_encoder(priv, cc);
    if (!enc)
    {
        return HAL_ERR_INVALID_STATE;
    }
    stats->fps = enc->get_current_fps();
    stats->bitrate_kbps = 0;
    stats->monitor_period_s = 0;
    if (cc->config.packet_type != HAL_PACKET_TYPE_MJPEG)
    {
        encoder_monitors mon = enc->get_encoder_monitors();
        if (mon.bitrate_monitor.enabled && mon.bitrate_monitor.ma_bitrate > 0)
        {
            /* ma_bitrate is bytes/s over the moving window (see hailo_encoder_impl.cpp). */
            stats->bitrate_kbps =
                static_cast<uint32_t>((static_cast<uint64_t>(mon.bitrate_monitor.ma_bitrate) * 8U) / 1000U);
            stats->monitor_period_s = mon.bitrate_monitor.period;
        }
    }
    return HAL_OK;
}

static const char *hailo15_codec_get_version(void)
{
    return "Hailo15 HAL-CODEC 2.1.0";
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
    .set_roi_config = hailo15_codec_set_roi_config,
    .get_roi_config = hailo15_codec_get_roi_config,
    .force_idr = hailo15_codec_force_idr,
    .get_stream_stats = hailo15_codec_get_stream_stats,
};

} // extern "C"
