/**
 * @file hailo15_osd_impl.cpp
 * @brief Hailo-15 HAL OSD implementation (FROM_MEDIA encoder-scoped overlays).
 *
 * Medialib 1.13 (SDK 4.0.23) declarative OSD model: overlays live in the
 * per-stream config_stream_osd_t of the medialib profile; every mutation is
 * get_current_profile() -> mutate -> set_override_parameters(), and the
 * internal blender picks the config up from the profile attached to each
 * encoded buffer. There is no enabled flag any more, so disable/enable is
 * modelled with a shadow snapshot map (Hailo15MediaPriv::osd_disabled_by_stream)
 * guarded by Hailo15MediaPriv::osd_state_mu, which also serializes OSD ops.
 *
 * Custom (raw ARGB/A420 buffer) overlays became internal in medialib 1.13 —
 * there is no public path to register them, so add/set custom return
 * HAL_ERR_NOT_SUPPORTED (the ops stay in the table to preserve the HAL ABI).
 *
 * HAL <-> ML type conversions use the helpers in hailo15_osd_ml.hpp.
 */

#include "hailo15_common.hpp"
#include "hailo15_media_priv.hpp"
#include "hailo15_osd_ml.hpp"

#include "common/hal_log.h"
#include "media/hal_osd.h"
#include "media/hal_codec_internal.h"

#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

namespace
{

/* -------------------------------------------------------------------
 * OSD context helper — resolves codec_ctx to media priv + stream id.
 * ------------------------------------------------------------------- */

struct OsdContext
{
    Hailo15MediaPriv *priv;
    std::string stream_id;
};

int resolve_osd_context(void *codec_ctx, OsdContext *out)
{
    if (!codec_ctx || !out)
    {
        return HAL_ERR_INVALID_ARG;
    }

    auto *cc = static_cast<HalCodecContext *>(codec_ctx);
    if (cc->config.type != HAL_CODEC_TYPE_FROM_MEDIA || !cc->config.media_ptr)
    {
        HAL_LOG_ERROR("Hailo15 OSD: codec context is not FROM_MEDIA type");
        return HAL_ERR_NOT_SUPPORTED;
    }

    Hailo15MediaPriv *priv = hailo15_media_priv_from_hal(cc->config.media_ptr);
    if (!priv || !priv->media_lib)
    {
        HAL_LOG_ERROR("Hailo15 OSD: media priv or media_lib is null");
        return HAL_ERR_NOT_INITIALIZED;
    }

    std::string stream_id(cc->codec_name);

    /* Defense-in-depth against a stale codec context: a rotation/flip reinit
     * (HAL_REINIT_PERFORMED from dynamic_change_image_config) FREES the old
     * contexts and builds new ones — a caller that keeps using the old pointer
     * is undefined behaviour. Validate this exact ctx is still the current one
     * for its stream (identity check against codec_by_stream, which
     * destroy_contexts clears / build_contexts repopulates under ctx_list_mu).
     * Leaf lock, taken before any MediaLibrary call, never nests with
     * osd_state_mu or priv->mutex. If the freed chunk has already been
     * overwritten this check itself is UB — the contract fix is always the
     * caller re-attaching after REINIT; this only converts the common
     * use-after-free into a clean error instead of a segfault. */
    {
        std::lock_guard<std::mutex> guard(priv->ctx_list_mu);
        auto cur_it = priv->codec_by_stream.find(stream_id);
        if (cur_it == priv->codec_by_stream.end() || cur_it->second != cc)
        {
            HAL_LOG_ERROR("Hailo15 OSD: codec context for '%s' is stale (freed by a rotation/"
                          "flip reinit); caller must re-attach contexts after HAL_REINIT_PERFORMED",
                          stream_id.c_str());
            return HAL_ERR_INVALID_STATE;
        }
    }

    out->priv = priv;
    out->stream_id = std::move(stream_id);
    return HAL_OK;
}

bool readable_image_file(const char *path)
{
    if (!path || path[0] == '\0')
    {
        return false;
    }
    struct stat st {};
    return (::stat(path, &st) == 0) && S_ISREG(st.st_mode) && (::access(path, R_OK) == 0);
}

/* -------------------------------------------------------------------
 * Declarative-model helpers. All assume the caller holds priv->osd_state_mu.
 * ------------------------------------------------------------------- */

/** Fetch a copy of the current profile; logs + returns false on failure. */
bool fetch_profile_locked(Hailo15MediaPriv *priv, config_profile_t *out)
{
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        HAL_LOG_ERROR("Hailo15 OSD: get_current_profile failed (%d)", static_cast<int>(prof_exp.error()));
        return false;
    }
    *out = prof_exp.value();
    return true;
}

/** Apply a mutated profile. Caller holds osd_state_mu (never priv->mutex). */
int apply_profile_locked(Hailo15MediaPriv *priv, config_profile_t &prof)
{
    media_library_return ret = priv->media_lib->set_override_parameters(prof);
    if (ret != MEDIA_LIBRARY_SUCCESS)
    {
        return hailo15_ml_err(ret);
    }
    return HAL_OK;
}

/** Which overlay vector an id lives in. */
enum class OsdSlot
{
    None,
    Image,
    Text,
    DateTime,
};

OsdSlot find_overlay_slot(const config_stream_osd_t &osd, const std::string &id)
{
    for (const auto &ptr : osd.image_overlays)
    {
        if (ptr && ptr->id == id)
        {
            return OsdSlot::Image;
        }
    }
    for (const auto &ptr : osd.text_overlays)
    {
        if (ptr && ptr->id == id)
        {
            return OsdSlot::Text;
        }
    }
    for (const auto &ptr : osd.datetime_overlays)
    {
        if (ptr && ptr->id == id)
        {
            return OsdSlot::DateTime;
        }
    }
    return OsdSlot::None;
}

/** Convert one profile overlay into its HAL snapshot form. */
bool ml_overlay_to_hal_snapshot(const config_stream_osd_t &osd, const std::string &id, bool enabled,
                                HalOsdOverlay *out)
{
    for (const auto &ptr : osd.image_overlays)
    {
        if (ptr && ptr->id == id)
        {
            out->type = HAL_OSD_OVERLAY_IMAGE;
            hailo15::osd_ml::ml_to_hal_image(*ptr, &out->data.image, enabled);
            return true;
        }
    }
    for (const auto &ptr : osd.text_overlays)
    {
        if (ptr && ptr->id == id)
        {
            out->type = HAL_OSD_OVERLAY_TEXT;
            hailo15::osd_ml::ml_to_hal_text(*ptr, &out->data.text, enabled);
            return true;
        }
    }
    for (const auto &ptr : osd.datetime_overlays)
    {
        if (ptr && ptr->id == id)
        {
            out->type = HAL_OSD_OVERLAY_DATETIME;
            hailo15::osd_ml::ml_to_hal_datetime(*ptr, &out->data.datetime, enabled);
            return true;
        }
    }
    return false;
}

/** Push a HAL overlay (by type) into the matching profile vector. */
void push_hal_overlay(config_stream_osd_t &osd, const HalOsdOverlay &ov)
{
    switch (ov.type)
    {
        case HAL_OSD_OVERLAY_IMAGE:
            osd.image_overlays.push_back(hailo15::osd_ml::hal_to_ml_image(ov.data.image));
            break;
        case HAL_OSD_OVERLAY_TEXT:
            osd.text_overlays.push_back(hailo15::osd_ml::hal_to_ml_text(ov.data.text));
            break;
        case HAL_OSD_OVERLAY_DATETIME:
            osd.datetime_overlays.push_back(hailo15::osd_ml::hal_to_ml_datetime(ov.data.datetime));
            break;
        case HAL_OSD_OVERLAY_CUSTOM:
        default:
            break; /* unreachable: custom adds are rejected up front */
    }
}

/** Replace-by-id that only compiles an assignment when the vector element type
 *  matches the converted overlay type (mismatched vectors are skipped). */
template <typename PtrT, typename MlT>
bool try_replace_overlay(std::vector<PtrT> &vec, const std::string &id, const MlT &ml)
{
    if constexpr (std::is_same_v<PtrT, MlT>)
    {
        for (auto &ptr : vec)
        {
            if (ptr && ptr->id == id)
            {
                ptr = ml;
                return true;
            }
        }
    }
    return false;
}

/** Remove one overlay (by id) from all three vectors; true if anything was erased. */
bool erase_overlay_by_id(config_stream_osd_t &osd, const std::string &id)
{
    bool erased = false;
    auto drop = [&id, &erased](auto &vec) {
        for (auto it = vec.begin(); it != vec.end();)
        {
            if (*it && (*it)->id == id)
            {
                it = vec.erase(it);
                erased = true;
            }
            else
            {
                ++it;
            }
        }
    };
    drop(osd.image_overlays);
    drop(osd.text_overlays);
    drop(osd.datetime_overlays);
    return erased;
}

} // namespace

/* ====================================================================
 * Add overlays
 * ==================================================================== */

static int hailo15_osd_add_image(void *codec_ctx, const HalOsdImageOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (!readable_image_file(overlay->image_path))
    {
        HAL_LOG_ERROR("Hailo15 OSD add_image: invalid image_path='%s'", overlay->image_path);
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        /* Defensive: clamp overlay coords into the pre-rotation DSP blend
         * frame so a portrait rotation can't push an image outside the blend
         * height and wedge the encoded stream. See clamp_osd_for_rotation(). */
        HalOsdImageOverlay adjusted = *overlay;
        auto lay_it = ctx.priv->osd_layout_by_encoder.find(ctx.stream_id);
        if (lay_it != ctx.priv->osd_layout_by_encoder.end())
        {
            HalRotationAngle rot = static_cast<HalRotationAngle>(lay_it->second.rotation);
            float ox = adjusted.base.x;
            float oy = adjusted.base.y;
            if (hailo15::osd_ml::clamp_osd_for_rotation(ox, oy, adjusted.width, adjusted.height,
                                                        lay_it->second.width, lay_it->second.height, rot))
            {
                HAL_LOG_WARNING("Hailo15 OSD: image '%s' origin (%.4f,%.4f) clamped to (%.4f,%.4f) "
                                "to fit pre-rotation blend frame under rotation %d (out %ux%u); "
                                "unclamped portrait OSD can wedge the encoded stream",
                                adjusted.base.id, adjusted.base.x, adjusted.base.y, ox, oy,
                                rot, lay_it->second.width, lay_it->second.height);
                adjusted.base.x = ox;
                adjusted.base.y = oy;
            }
        }
        std::lock_guard<std::mutex> lock(ctx.priv->osd_state_mu);
        config_profile_t prof;
        if (!fetch_profile_locked(ctx.priv, &prof))
        {
            return HAL_ERROR;
        }
        auto stream_it = prof.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == prof.encoded_output_streams.end())
        {
            HAL_LOG_ERROR("Hailo15 OSD: stream '%s' not found in profile", ctx.stream_id.c_str());
            return HAL_ERR_NOT_FOUND;
        }
        config_stream_osd_t &osd = stream_it->second.osd;
        const std::string id(adjusted.base.id);
        const auto shadow_it = ctx.priv->osd_disabled_by_stream.find(ctx.stream_id);
        const bool in_shadow = (shadow_it != ctx.priv->osd_disabled_by_stream.end() &&
                                shadow_it->second.count(id) != 0U);
        if (find_overlay_slot(osd, id) != OsdSlot::None || in_shadow)
        {
            HAL_LOG_ERROR("Hailo15 OSD: add image '%s' failed: id already exists", id.c_str());
            return HAL_ERR_INVALID_STATE;
        }
        if (!adjusted.base.enabled)
        {
            HalOsdOverlay snap{};
            snap.type = HAL_OSD_OVERLAY_IMAGE;
            snap.data.image = adjusted;
            ctx.priv->osd_disabled_by_stream[ctx.stream_id][id] = snap;
            return HAL_OK;
        }
        osd.image_overlays.push_back(hailo15::osd_ml::hal_to_ml_image(adjusted));
        return apply_profile_locked(ctx.priv, prof);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD add_image exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_add_text(void *codec_ctx, const HalOsdTextOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        /* Defensive: clamp overlay origin into the pre-rotation DSP blend
         * frame under portrait rotation; an out-of-bounds y wedges the stream. */
        HalOsdTextOverlay adjusted = *overlay;
        auto lay_it = ctx.priv->osd_layout_by_encoder.find(ctx.stream_id);
        if (lay_it != ctx.priv->osd_layout_by_encoder.end())
        {
            HalRotationAngle rot = static_cast<HalRotationAngle>(lay_it->second.rotation);
            float ox = adjusted.base.x;
            float oy = adjusted.base.y;
            if (hailo15::osd_ml::clamp_osd_for_rotation(ox, oy, 0.0f, 0.0f,
                                                        lay_it->second.width, lay_it->second.height, rot))
            {
                HAL_LOG_WARNING("Hailo15 OSD: text '%s' origin (%.4f,%.4f) clamped to (%.4f,%.4f) "
                                "to fit pre-rotation blend frame under rotation %d (out %ux%u); "
                                "unclamped portrait OSD can wedge the encoded stream",
                                adjusted.base.id, adjusted.base.x, adjusted.base.y, ox, oy,
                                rot, lay_it->second.width, lay_it->second.height);
                adjusted.base.x = ox;
                adjusted.base.y = oy;
            }
        }
        std::lock_guard<std::mutex> lock(ctx.priv->osd_state_mu);
        config_profile_t prof;
        if (!fetch_profile_locked(ctx.priv, &prof))
        {
            return HAL_ERROR;
        }
        auto stream_it = prof.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == prof.encoded_output_streams.end())
        {
            HAL_LOG_ERROR("Hailo15 OSD: stream '%s' not found in profile", ctx.stream_id.c_str());
            return HAL_ERR_NOT_FOUND;
        }
        config_stream_osd_t &osd = stream_it->second.osd;
        const std::string id(adjusted.base.id);
        const auto shadow_it = ctx.priv->osd_disabled_by_stream.find(ctx.stream_id);
        const bool in_shadow = (shadow_it != ctx.priv->osd_disabled_by_stream.end() &&
                                shadow_it->second.count(id) != 0U);
        if (find_overlay_slot(osd, id) != OsdSlot::None || in_shadow)
        {
            HAL_LOG_ERROR("Hailo15 OSD: add text '%s' failed: id already exists", id.c_str());
            return HAL_ERR_INVALID_STATE;
        }
        if (!adjusted.base.enabled)
        {
            HalOsdOverlay snap{};
            snap.type = HAL_OSD_OVERLAY_TEXT;
            snap.data.text = adjusted;
            ctx.priv->osd_disabled_by_stream[ctx.stream_id][id] = snap;
            return HAL_OK;
        }
        osd.text_overlays.push_back(hailo15::osd_ml::hal_to_ml_text(adjusted));
        return apply_profile_locked(ctx.priv, prof);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD add_text exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_add_datetime(void *codec_ctx, const HalOsdDateTimeOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        /* Defensive: clamp overlay origin into the pre-rotation DSP blend
         * frame under portrait rotation; an out-of-bounds y wedges the stream. */
        HalOsdDateTimeOverlay adjusted = *overlay;
        auto lay_it = ctx.priv->osd_layout_by_encoder.find(ctx.stream_id);
        if (lay_it != ctx.priv->osd_layout_by_encoder.end())
        {
            HalRotationAngle rot = static_cast<HalRotationAngle>(lay_it->second.rotation);
            float ox = adjusted.text.base.x;
            float oy = adjusted.text.base.y;
            if (hailo15::osd_ml::clamp_osd_for_rotation(ox, oy, 0.0f, 0.0f,
                                                        lay_it->second.width, lay_it->second.height, rot))
            {
                HAL_LOG_WARNING("Hailo15 OSD: datetime '%s' origin (%.4f,%.4f) clamped to (%.4f,%.4f) "
                                "to fit pre-rotation blend frame under rotation %d (out %ux%u); "
                                "unclamped portrait OSD can wedge the encoded stream",
                                adjusted.text.base.id, adjusted.text.base.x, adjusted.text.base.y, ox, oy,
                                rot, lay_it->second.width, lay_it->second.height);
                adjusted.text.base.x = ox;
                adjusted.text.base.y = oy;
            }
        }
        std::lock_guard<std::mutex> lock(ctx.priv->osd_state_mu);
        config_profile_t prof;
        if (!fetch_profile_locked(ctx.priv, &prof))
        {
            return HAL_ERROR;
        }
        auto stream_it = prof.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == prof.encoded_output_streams.end())
        {
            HAL_LOG_ERROR("Hailo15 OSD: stream '%s' not found in profile", ctx.stream_id.c_str());
            return HAL_ERR_NOT_FOUND;
        }
        config_stream_osd_t &osd = stream_it->second.osd;
        const std::string id(adjusted.text.base.id);
        const auto shadow_it = ctx.priv->osd_disabled_by_stream.find(ctx.stream_id);
        const bool in_shadow = (shadow_it != ctx.priv->osd_disabled_by_stream.end() &&
                                shadow_it->second.count(id) != 0U);
        if (find_overlay_slot(osd, id) != OsdSlot::None || in_shadow)
        {
            HAL_LOG_ERROR("Hailo15 OSD: add datetime '%s' failed: id already exists", id.c_str());
            return HAL_ERR_INVALID_STATE;
        }
        if (!adjusted.text.base.enabled)
        {
            HalOsdOverlay snap{};
            snap.type = HAL_OSD_OVERLAY_DATETIME;
            snap.data.datetime = adjusted;
            ctx.priv->osd_disabled_by_stream[ctx.stream_id][id] = snap;
            return HAL_OK;
        }
        osd.datetime_overlays.push_back(hailo15::osd_ml::hal_to_ml_datetime(adjusted));
        return apply_profile_locked(ctx.priv, prof);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD add_datetime exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_add_custom(void *codec_ctx, const HalOsdCustomOverlay *overlay)
{
    (void)codec_ctx;
    (void)overlay;
    static bool logged = false; /* process-local, benign race (worst case: duplicate log) */
    if (!logged)
    {
        logged = true;
        HAL_LOG_WARNING("Hailo15 OSD: custom (raw-buffer) overlays are not supported under "
                        "medialib 1.13 (CustomOverlay became internal); add_custom_overlay "
                        "returns NOT_SUPPORTED");
    }
    return !overlay ? HAL_ERR_INVALID_ARG : HAL_ERR_NOT_SUPPORTED;
}

/* ====================================================================
 * Set (update) overlays
 * ==================================================================== */

/** Overlay id of a HAL snapshot, dispatched by its recorded type. */
const char *hal_snapshot_id(const HalOsdOverlay &s)
{
    switch (s.type)
    {
        case HAL_OSD_OVERLAY_IMAGE:
            return s.data.image.base.id;
        case HAL_OSD_OVERLAY_TEXT:
            return s.data.text.base.id;
        case HAL_OSD_OVERLAY_DATETIME:
            return s.data.datetime.text.base.id;
        case HAL_OSD_OVERLAY_CUSTOM:
        default:
            return "";
    }
}

/** Replace one overlay (matched by id) with the caller's definition.
 *  NB: unlike add_*, set_* does NOT consult base.enabled — a blended overlay
 *  stays blended, a shadowed (disabled) snapshot stays hidden. Toggling
 *  visibility is set_enabled()'s job; set only rewrites geometry/content.
 *  An id that is blended gets replaced in place (one profile apply); an id in
 *  the shadow gets its snapshot updated without applying; unknown -> NOT_FOUND. */
template <typename HalOverlayT, typename ToMl, typename ToSnap>
static int osd_set_by_type(void *codec_ctx, const HalOverlayT *overlay, ToMl to_ml, ToSnap to_snap,
                           const char *kind)
{
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        std::lock_guard<std::mutex> lock(ctx.priv->osd_state_mu);
        config_profile_t prof;
        if (!fetch_profile_locked(ctx.priv, &prof))
        {
            return HAL_ERROR;
        }
        auto stream_it = prof.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == prof.encoded_output_streams.end())
        {
            return HAL_ERR_NOT_FOUND;
        }
        config_stream_osd_t &osd = stream_it->second.osd;
        const HalOsdOverlay snap = to_snap(*overlay);
        const std::string id(hal_snapshot_id(snap));

        /* Replace in place if currently blended. */
        const auto ml = to_ml(*overlay);
        if (try_replace_overlay(osd.image_overlays, id, ml) ||
            try_replace_overlay(osd.text_overlays, id, ml) ||
            try_replace_overlay(osd.datetime_overlays, id, ml))
        {
            return apply_profile_locked(ctx.priv, prof);
        }

        /* Not blended: update the shadow snapshot only (stays hidden). The snapshot
         * takes the caller's overlay and type wholesale. */
        auto &shadow = ctx.priv->osd_disabled_by_stream[ctx.stream_id];
        auto sh_it = shadow.find(id);
        if (sh_it != shadow.end())
        {
            sh_it->second = snap;
            return HAL_OK;
        }

        HAL_LOG_ERROR("Hailo15 OSD: set %s '%s' failed: not found", kind, id.c_str());
        return HAL_ERR_NOT_FOUND;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD set %s exception: %s", kind, e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_set_image(void *codec_ctx, const HalOsdImageOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (!readable_image_file(overlay->image_path))
    {
        HAL_LOG_ERROR("Hailo15 OSD set_image: invalid image_path='%s'", overlay->image_path);
        return HAL_ERR_INVALID_ARG;
    }
    return osd_set_by_type(
        codec_ctx, overlay,
        [](const HalOsdImageOverlay &o) { return hailo15::osd_ml::hal_to_ml_image(o); },
        [](const HalOsdImageOverlay &o) {
            HalOsdOverlay snap{};
            snap.type = HAL_OSD_OVERLAY_IMAGE;
            snap.data.image = o;
            return snap;
        },
        "image");
}

static int hailo15_osd_set_text(void *codec_ctx, const HalOsdTextOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    return osd_set_by_type(
        codec_ctx, overlay,
        [](const HalOsdTextOverlay &o) { return hailo15::osd_ml::hal_to_ml_text(o); },
        [](const HalOsdTextOverlay &o) {
            HalOsdOverlay snap{};
            snap.type = HAL_OSD_OVERLAY_TEXT;
            snap.data.text = o;
            return snap;
        },
        "text");
}

static int hailo15_osd_set_datetime(void *codec_ctx, const HalOsdDateTimeOverlay *overlay)
{
    if (!overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    return osd_set_by_type(
        codec_ctx, overlay,
        [](const HalOsdDateTimeOverlay &o) { return hailo15::osd_ml::hal_to_ml_datetime(o); },
        [](const HalOsdDateTimeOverlay &o) {
            HalOsdOverlay snap{};
            snap.type = HAL_OSD_OVERLAY_DATETIME;
            snap.data.datetime = o;
            return snap;
        },
        "datetime");
}

static int hailo15_osd_set_custom(void *codec_ctx, const HalOsdCustomOverlay *overlay)
{
    (void)codec_ctx;
    (void)overlay;
    static bool logged = false; /* process-local, benign race (worst case: duplicate log) */
    if (!logged)
    {
        logged = true;
        HAL_LOG_WARNING("Hailo15 OSD: custom (raw-buffer) overlays are not supported under "
                        "medialib 1.13 (CustomOverlay became internal); set_custom_overlay "
                        "returns NOT_SUPPORTED");
    }
    return !overlay ? HAL_ERR_INVALID_ARG : HAL_ERR_NOT_SUPPORTED;
}

/* ====================================================================
 * Remove / enable-disable
 * ==================================================================== */

static int hailo15_osd_remove(void *codec_ctx, const char *overlay_id)
{
    if (!overlay_id)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        std::lock_guard<std::mutex> lock(ctx.priv->osd_state_mu);
        config_profile_t prof;
        if (!fetch_profile_locked(ctx.priv, &prof))
        {
            return HAL_ERROR;
        }
        auto stream_it = prof.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == prof.encoded_output_streams.end())
        {
            return HAL_ERR_NOT_FOUND;
        }
        const std::string id(overlay_id);
        const bool in_profile = erase_overlay_by_id(stream_it->second.osd, id);
        /* find()-based erase: no empty-entry materialization on a miss. */
        bool removed_shadow = false;
        auto shadow_it = ctx.priv->osd_disabled_by_stream.find(ctx.stream_id);
        if (shadow_it != ctx.priv->osd_disabled_by_stream.end())
        {
            removed_shadow = (shadow_it->second.erase(id) != 0U);
        }
        if (!in_profile && !removed_shadow)
        {
            return HAL_ERR_NOT_FOUND;
        }
        return apply_profile_locked(ctx.priv, prof);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD remove exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_set_enabled(void *codec_ctx, const char *overlay_id, bool enabled)
{
    if (!overlay_id)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        std::lock_guard<std::mutex> lock(ctx.priv->osd_state_mu);
        config_profile_t prof;
        if (!fetch_profile_locked(ctx.priv, &prof))
        {
            return HAL_ERROR;
        }
        auto stream_it = prof.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == prof.encoded_output_streams.end())
        {
            return HAL_ERR_NOT_FOUND;
        }
        config_stream_osd_t &osd = stream_it->second.osd;
        auto &shadow = ctx.priv->osd_disabled_by_stream[ctx.stream_id];
        const std::string id(overlay_id);

        if (!enabled)
        {
            /* Disable: present in the profile -> snapshot + remove. */
            if (find_overlay_slot(osd, id) == OsdSlot::None)
            {
                return shadow.count(id) != 0U ? HAL_OK : HAL_ERR_NOT_FOUND;
            }
            HalOsdOverlay snap{};
            if (!ml_overlay_to_hal_snapshot(osd, id, false, &snap))
            {
                return HAL_ERR_NOT_FOUND;
            }
            erase_overlay_by_id(osd, id);
            shadow[id] = snap;
            return apply_profile_locked(ctx.priv, prof);
        }

        /* Enable: shadowed -> restore into the matching vector. */
        auto sh_it = shadow.find(id);
        if (sh_it == shadow.end())
        {
            return find_overlay_slot(osd, id) != OsdSlot::None ? HAL_OK : HAL_ERR_NOT_FOUND;
        }
        if (sh_it->second.type == HAL_OSD_OVERLAY_CUSTOM)
        {
            HAL_LOG_ERROR("Hailo15 OSD: cannot enable '%s': custom overlays are unsupported", id.c_str());
            return HAL_ERR_NOT_SUPPORTED;
        }
        push_hal_overlay(osd, sh_it->second);
        shadow.erase(sh_it);
        return apply_profile_locked(ctx.priv, prof);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD set_enabled exception: %s", e.what());
        return HAL_ERROR;
    }
}

/* ====================================================================
 * Query overlays
 * ==================================================================== */

static int hailo15_osd_get_overlays(void *codec_ctx, HalOsdOverlay *overlays, uint32_t *count)
{
    if (!count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        std::lock_guard<std::mutex> lock(ctx.priv->osd_state_mu);
        config_profile_t prof;
        if (!fetch_profile_locked(ctx.priv, &prof))
        {
            HAL_LOG_ERROR("Hailo15 OSD get_overlays: get_current_profile failed");
            return HAL_ERROR;
        }
        const config_profile_t &p = prof;
        auto stream_it = p.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == p.encoded_output_streams.end())
        {
            HAL_LOG_ERROR("Hailo15 OSD get_overlays: stream '%s' not found in profile", ctx.stream_id.c_str());
            return HAL_ERR_NOT_FOUND;
        }

        const config_stream_osd_t &osd = stream_it->second.osd;
        /* find(), not operator[]: reads must not materialize empty shadow entries. */
        static const std::map<std::string, HalOsdOverlay> kNoShadow{};
        const auto shadow_it = ctx.priv->osd_disabled_by_stream.find(ctx.stream_id);
        const auto &shadow = (shadow_it != ctx.priv->osd_disabled_by_stream.end()) ? shadow_it->second
                                                                                    : kNoShadow;
        const uint32_t n_image = static_cast<uint32_t>(osd.image_overlays.size());
        const uint32_t n_text = static_cast<uint32_t>(osd.text_overlays.size());
        const uint32_t n_datetime = static_cast<uint32_t>(osd.datetime_overlays.size());
        const uint32_t n_shadow = static_cast<uint32_t>(shadow.size());
        const uint32_t total = n_image + n_text + n_datetime + n_shadow;

        /* Query-only: caller wants to know the required count. */
        if (!overlays || *count < total)
        {
            *count = total;
            return overlays ? HAL_ERR_INSUFFICIENT_BUFFER : HAL_OK;
        }

        uint32_t idx = 0;

        for (uint32_t i = 0; i < n_image; ++i)
        {
            if (!osd.image_overlays[i])
            {
                continue;
            }
            overlays[idx].type = HAL_OSD_OVERLAY_IMAGE;
            hailo15::osd_ml::ml_to_hal_image(*osd.image_overlays[i], &overlays[idx].data.image, true);
            ++idx;
        }

        for (uint32_t i = 0; i < n_text; ++i)
        {
            if (!osd.text_overlays[i])
            {
                continue;
            }
            overlays[idx].type = HAL_OSD_OVERLAY_TEXT;
            hailo15::osd_ml::ml_to_hal_text(*osd.text_overlays[i], &overlays[idx].data.text, true);
            ++idx;
        }

        for (uint32_t i = 0; i < n_datetime; ++i)
        {
            if (!osd.datetime_overlays[i])
            {
                continue;
            }
            overlays[idx].type = HAL_OSD_OVERLAY_DATETIME;
            hailo15::osd_ml::ml_to_hal_datetime(*osd.datetime_overlays[i], &overlays[idx].data.datetime, true);
            ++idx;
        }

        /* Shadowed (disabled) overlays are reported with enabled=false. */
        for (const auto &kv : shadow)
        {
            overlays[idx] = kv.second; /* snapshot already carries enabled=false + type */
            ++idx;
        }

        *count = idx;
        return HAL_OK;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD get_overlays exception: %s", e.what());
        return HAL_ERROR;
    }
}

static int hailo15_osd_get_overlay(void *codec_ctx, const char *overlay_id, HalOsdOverlay *overlay)
{
    if (!overlay_id || !overlay)
    {
        return HAL_ERR_INVALID_ARG;
    }
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        std::lock_guard<std::mutex> lock(ctx.priv->osd_state_mu);
        config_profile_t prof;
        if (!fetch_profile_locked(ctx.priv, &prof))
        {
            return HAL_ERROR;
        }
        auto stream_it = prof.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == prof.encoded_output_streams.end())
        {
            return HAL_ERR_NOT_FOUND;
        }

        const config_stream_osd_t &osd = stream_it->second.osd;
        const std::string id(overlay_id);

        if (ml_overlay_to_hal_snapshot(osd, id, true, overlay))
        {
            return HAL_OK;
        }

        /* find(), not operator[]: read path must not materialize an empty entry. */
        const auto shadow_it = ctx.priv->osd_disabled_by_stream.find(ctx.stream_id);
        if (shadow_it != ctx.priv->osd_disabled_by_stream.end())
        {
            auto id_it = shadow_it->second.find(id);
            if (id_it != shadow_it->second.end())
            {
                *overlay = id_it->second; /* snapshot carries enabled=false + type */
                return HAL_OK;
            }
        }

        return HAL_ERR_NOT_FOUND;
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD get_overlay exception: %s", e.what());
        return HAL_ERROR;
    }
}

/* ====================================================================
 * Clear all overlays
 * ==================================================================== */

static int hailo15_osd_clear(void *codec_ctx)
{
    OsdContext ctx;
    int r = resolve_osd_context(codec_ctx, &ctx);
    if (r != HAL_OK)
    {
        return r;
    }
    try
    {
        std::lock_guard<std::mutex> lock(ctx.priv->osd_state_mu);
        config_profile_t prof;
        if (!fetch_profile_locked(ctx.priv, &prof))
        {
            return HAL_ERROR;
        }
        auto stream_it = prof.encoded_output_streams.find(ctx.stream_id);
        if (stream_it == prof.encoded_output_streams.end())
        {
            return HAL_ERR_NOT_FOUND;
        }

        config_stream_osd_t &osd = stream_it->second.osd;
        const bool had_overlays = !osd.image_overlays.empty() || !osd.text_overlays.empty() ||
                                  !osd.datetime_overlays.empty();
        osd.image_overlays.clear();
        osd.text_overlays.clear();
        osd.datetime_overlays.clear();
        ctx.priv->osd_disabled_by_stream.erase(ctx.stream_id);

        if (!had_overlays)
        {
            return HAL_OK;
        }
        return apply_profile_locked(ctx.priv, prof);
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("Hailo15 OSD clear exception: %s", e.what());
        return HAL_ERROR;
    }
}

/* ====================================================================
 * Version
 * ==================================================================== */

static const char *hailo15_osd_get_version(void)
{
    return "Hailo15 HAL-OSD 2.1.0";
}

/* ====================================================================
 * OSD ops table
 * ==================================================================== */

extern "C" {

HalOsdOps HAL_OSD_OPS = {
    .add_image_overlay    = hailo15_osd_add_image,
    .add_text_overlay     = hailo15_osd_add_text,
    .add_datetime_overlay = hailo15_osd_add_datetime,
    .add_custom_overlay   = hailo15_osd_add_custom,

    .set_image_overlay    = hailo15_osd_set_image,
    .set_text_overlay     = hailo15_osd_set_text,
    .set_datetime_overlay = hailo15_osd_set_datetime,
    .set_custom_overlay   = hailo15_osd_set_custom,

    .remove_overlay       = hailo15_osd_remove,
    .set_overlay_enabled  = hailo15_osd_set_enabled,

    .get_overlays         = hailo15_osd_get_overlays,
    .get_overlay          = hailo15_osd_get_overlay,
    .clear_overlays       = hailo15_osd_clear,

    .get_version          = hailo15_osd_get_version,
};

} // extern "C"
