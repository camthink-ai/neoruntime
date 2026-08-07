/**
 * @file hailo15_osd_ml.hpp
 * @brief Inline conversion helpers between HAL OSD types and Hailo MediaLibrary OSD types.
 *
 * Also provides font-size rescaling and layout-change recalculation utilities
 * that mirror the webserver OsdResource::update_osds() behaviour.
 */
#pragma once

#include "media/hal_osd.h"
#include "media/hal_media.h"
#include "hailo15_media_priv.hpp"
#include "hailo15_common.hpp"

#include "hailo/osd_types.hpp"
#include "hailo/osd.hpp"
#include <hailo/media_library/media_library_types.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <type_traits>
#include <cstring>
#include <string>
#include <variant>

namespace hailo15::osd_ml
{

/* OsdLayoutState is defined in hailo15_media_priv.hpp (used by Hailo15MediaPriv::osd_layout_by_encoder). */

/* ====================================================================
 * Resolution-change rescale (preserve OSD + static privacy mask, don't clear)
 *
 * On a resolution change the OSD/privacy-mask blenders are NOT reconfigured by
 * set_override_parameters() (configure_frontend_encoder only sets the encoding). To keep OSD
 * overlays and the static privacy mask correct at the new geometry without dropping them:
 *  - OSD overlay x/y and ImageOverlay width/height are RELATIVE (0..1) -> auto-adapt, keep.
 *  - OSD font_size / line_thickness / outline_size are ABSOLUTE (px) -> scale by width ratio.
 *  - Static privacy mask polygon vertices are ABSOLUTE int px -> scale by w/h ratio + clamp.
 * After applying the profile, the caller must also push the rescaled config into the persisted
 * blenders via MediaLibrary::configure_osd() / configure_privacy_mask().
 * ==================================================================== */
inline void rescale_stream_osd_and_masking(config_profile_t &p, const std::string &stream_id,
                                           uint32_t old_w, uint32_t old_h,
                                           uint32_t new_w, uint32_t new_h)
{
    if (old_w == 0U || old_h == 0U || (old_w == new_w && old_h == new_h))
    {
        return;
    }
    auto it = p.encoded_output_streams.find(stream_id);
    if (it == p.encoded_output_streams.end())
    {
        return;
    }
    config_encoded_output_stream_t &eos = it->second;
    const float wr = static_cast<float>(new_w) / static_cast<float>(old_w);

    auto scale_text_overlay = [wr](auto &overlay_ptr) {
        if (!overlay_ptr)
        {
            return;
        }
        /* font_size is int; line_thickness / outline_size are std::optional<int> in the SDK. */
        overlay_ptr->font_size =
            std::max(1, static_cast<int>(std::lround(overlay_ptr->font_size * wr)));
        if (overlay_ptr->line_thickness.has_value())
        {
            overlay_ptr->line_thickness =
                std::optional<int>(std::max(1, static_cast<int>(std::lround((*overlay_ptr->line_thickness) * wr))));
        }
        if (overlay_ptr->outline_size.has_value())
        {
            overlay_ptr->outline_size =
                std::optional<int>(static_cast<int>(std::lround((*overlay_ptr->outline_size) * wr)));
        }
    };
    for (auto &tptr : eos.osd.text_overlays)
    {
        scale_text_overlay(tptr);
    }
    for (auto &tptr : eos.osd.datetime_overlays)
    {
        scale_text_overlay(tptr);
    }
    /* ImageOverlay width/height are relative (0..1) -> unchanged. */

    if (eos.masking.static_privacy_mask_config.has_value() &&
        eos.masking.static_privacy_mask_config->enabled)
    {
        const float hr = static_cast<float>(new_h) / static_cast<float>(old_h);
        for (auto &poly : eos.masking.static_privacy_mask_config->masks)
        {
            for (auto &v : poly.vertices)
            {
                long nx = std::lround(static_cast<float>(v.x) * wr);
                long ny = std::lround(static_cast<float>(v.y) * hr);
                v.x = static_cast<int>(std::max<long>(0, std::min<long>(static_cast<long>(new_w), nx)));
                v.y = static_cast<int>(std::max<long>(0, std::min<long>(static_cast<long>(new_h), ny)));
            }
        }
    }
}

/* ====================================================================
 * HAL -> ML conversions
 * ==================================================================== */

inline osd::rotation_alignment_policy_t hal_to_ml_rotation_policy(HalOsdRotationPolicy p)
{
    switch (p)
    {
        case HAL_OSD_ROTATION_POLICY_TOP_LEFT:
            return osd::rotation_alignment_policy_t::TOP_LEFT;
        case HAL_OSD_ROTATION_POLICY_CENTER:
        default:
            return osd::rotation_alignment_policy_t::CENTER;
    }
}

inline osd::HorizontalAlignment hal_to_ml_halign(HalOsdHorizontalAlignment a)
{
    switch (a)
    {
        case HAL_OSD_HALIGN_CENTER:
            return osd::HorizontalAlignment::CENTER;
        case HAL_OSD_HALIGN_RIGHT:
            return osd::HorizontalAlignment::RIGHT;
        case HAL_OSD_HALIGN_LEFT:
        default:
            return osd::HorizontalAlignment::LEFT;
    }
}

inline osd::VerticalAlignment hal_to_ml_valign(HalOsdVerticalAlignment a)
{
    switch (a)
    {
        case HAL_OSD_VALIGN_CENTER:
            return osd::VerticalAlignment::CENTER;
        case HAL_OSD_VALIGN_BOTTOM:
            return osd::VerticalAlignment::BOTTOM;
        case HAL_OSD_VALIGN_TOP:
        default:
            return osd::VerticalAlignment::TOP;
    }
}

inline osd::font_weight_t hal_to_ml_font_weight(HalOsdFontWeight w)
{
    switch (w)
    {
        case HAL_OSD_FONT_WEIGHT_BOLD:
            return osd::font_weight_t::BOLD;
        case HAL_OSD_FONT_WEIGHT_NORMAL:
        default:
            return osd::font_weight_t::NORMAL;
    }
}

inline osd::rgba_color_t hal_to_ml_color(const HalOsdColor &c)
{
    return osd::rgba_color_t{c.r, c.g, c.b, c.a};
}

/* -- Full overlay conversions HAL -> ML -- */

inline osd::ImageOverlay hal_to_ml_image(const HalOsdImageOverlay &h)
{
    return osd::ImageOverlay(
        std::string(h.base.id),
        h.base.x,
        h.base.y,
        h.width,
        h.height,
        std::string(h.image_path),
        h.base.z_index,
        h.base.angle,
        hal_to_ml_rotation_policy(h.base.rotation_policy),
        hal_to_ml_halign(h.base.h_align),
        hal_to_ml_valign(h.base.v_align));
}

inline osd::TextOverlay hal_to_ml_text(const HalOsdTextOverlay &h)
{
    const char *font_path = (h.font_path[0] != '\0') ? h.font_path : DEFAULT_FONT_PATH;
    return osd::TextOverlay(
        std::string(h.base.id),
        h.base.x,
        h.base.y,
        std::string(h.label),
        hal_to_ml_color(h.text_color),
        hal_to_ml_color(h.background_color),
        h.font_size,
        h.line_thickness,
        h.base.z_index,
        std::string(font_path),
        h.base.angle,
        hal_to_ml_rotation_policy(h.base.rotation_policy),
        hal_to_ml_color(h.shadow_color),
        h.shadow_offset_x,
        h.shadow_offset_y,
        hal_to_ml_font_weight(h.font_weight),
        h.outline_size,
        hal_to_ml_color(h.outline_color),
        hal_to_ml_halign(h.base.h_align),
        hal_to_ml_valign(h.base.v_align));
}

inline osd::DateTimeOverlay hal_to_ml_datetime(const HalOsdDateTimeOverlay &h)
{
    const char *font_path = (h.text.font_path[0] != '\0') ? h.text.font_path : DEFAULT_FONT_PATH;
    const char *fmt = (h.datetime_format[0] != '\0') ? h.datetime_format : DEFAULT_DATETIME_STRING;
    return osd::DateTimeOverlay(
        std::string(h.text.base.id),
        h.text.base.x,
        h.text.base.y,
        std::string(fmt),
        hal_to_ml_color(h.text.text_color),
        hal_to_ml_color(h.text.background_color),
        std::string(font_path),
        h.text.font_size,
        h.text.line_thickness,
        h.text.base.z_index,
        h.text.base.angle,
        hal_to_ml_rotation_policy(h.text.base.rotation_policy),
        hal_to_ml_color(h.text.shadow_color),
        h.text.shadow_offset_x,
        h.text.shadow_offset_y,
        hal_to_ml_font_weight(h.text.font_weight),
        h.text.outline_size,
        hal_to_ml_color(h.text.outline_color),
        hal_to_ml_halign(h.text.base.h_align),
        hal_to_ml_valign(h.text.base.v_align));
}

inline osd::CustomOverlay hal_to_ml_custom(const HalOsdCustomOverlay &h)
{
    osd::custom_overlay_format fmt = (h.format == HAL_OSD_CUSTOM_FMT_ARGB) ? osd::ARGB : osd::A420;
    return osd::CustomOverlay(
        std::string(h.base.id),
        h.base.x,
        h.base.y,
        h.width,
        h.height,
        h.base.z_index,
        fmt,
        h.base.angle,
        hal_to_ml_rotation_policy(h.base.rotation_policy),
        hal_to_ml_halign(h.base.h_align),
        hal_to_ml_valign(h.base.v_align));
}

/* ====================================================================
 * ML -> HAL conversions
 * ==================================================================== */

inline HalOsdRotationPolicy ml_to_hal_rotation_policy(osd::rotation_alignment_policy_t p)
{
    switch (p)
    {
        case osd::rotation_alignment_policy_t::TOP_LEFT:
            return HAL_OSD_ROTATION_POLICY_TOP_LEFT;
        case osd::rotation_alignment_policy_t::CENTER:
        default:
            return HAL_OSD_ROTATION_POLICY_CENTER;
    }
}

template <typename T>
struct is_std_optional : std::false_type
{
};
template <typename U>
struct is_std_optional<std::optional<U>> : std::true_type
{
    using value_type = U;
};
template <typename T>
static constexpr bool is_std_optional_v = is_std_optional<T>::value;

template <typename T>
static inline int enum_int_value(const T &v)
{
    if constexpr (is_std_optional_v<T>)
    {
        return v.has_value() ? (int)(*v) : 0;
    }
    else
    {
        return (int)v;
    }
}

template <typename AlignT>
static inline float align_float_value(const AlignT &a)
{
    if constexpr (is_std_optional_v<AlignT>)
    {
        return a.has_value() ? a->as_float() : 0.0f;
    }
    else
    {
        return a.as_float();
    }
}

template <typename ColorT>
static inline HalOsdColor color_to_hal_any(const ColorT &c)
{
    if constexpr (is_std_optional_v<ColorT>)
    {
        if (!c.has_value())
        {
            return HalOsdColor{-1, -1, -1, -1};
        }
        return HalOsdColor{c->red, c->green, c->blue, c->alpha};
    }
    else
    {
        return HalOsdColor{c.red, c.green, c.blue, c.alpha};
    }
}

template <typename RotT>
static inline HalOsdRotationPolicy rotation_policy_to_hal_any(const RotT &p)
{
    const int v = enum_int_value(p);
    return (v == (int)osd::rotation_alignment_policy_t::TOP_LEFT) ? HAL_OSD_ROTATION_POLICY_TOP_LEFT
                                                                  : HAL_OSD_ROTATION_POLICY_CENTER;
}

template <typename FontWeightT>
static inline HalOsdFontWeight font_weight_to_hal_any(const FontWeightT &w)
{
    const int v = enum_int_value(w);
    return (v == (int)osd::font_weight_t::BOLD) ? HAL_OSD_FONT_WEIGHT_BOLD : HAL_OSD_FONT_WEIGHT_NORMAL;
}

template <typename AlignT>
static inline HalOsdHorizontalAlignment halign_to_hal_any(const AlignT &a)
{
    const float f = align_float_value(a);
    if (std::fabs(f - osd::HorizontalAlignment::CENTER.as_float()) < 1e-6f)
    {
        return HAL_OSD_HALIGN_CENTER;
    }
    if (std::fabs(f - osd::HorizontalAlignment::RIGHT.as_float()) < 1e-6f)
    {
        return HAL_OSD_HALIGN_RIGHT;
    }
    return HAL_OSD_HALIGN_LEFT;
}

template <typename AlignT>
static inline HalOsdVerticalAlignment valign_to_hal_any(const AlignT &a)
{
    const float f = align_float_value(a);
    if (std::fabs(f - osd::VerticalAlignment::CENTER.as_float()) < 1e-6f)
    {
        return HAL_OSD_VALIGN_CENTER;
    }
    if (std::fabs(f - osd::VerticalAlignment::BOTTOM.as_float()) < 1e-6f)
    {
        return HAL_OSD_VALIGN_BOTTOM;
    }
    return HAL_OSD_VALIGN_TOP;
}

inline HalOsdHorizontalAlignment ml_to_hal_halign(const osd::HorizontalAlignment &a)
{
    float v = a.as_float();
    if (v <= 0.01f)
        return HAL_OSD_HALIGN_LEFT;
    if (v >= 0.99f)
        return HAL_OSD_HALIGN_RIGHT;
    return HAL_OSD_HALIGN_CENTER;
}

inline HalOsdVerticalAlignment ml_to_hal_valign(const osd::VerticalAlignment &a)
{
    float v = a.as_float();
    if (v <= 0.01f)
        return HAL_OSD_VALIGN_TOP;
    if (v >= 0.99f)
        return HAL_OSD_VALIGN_BOTTOM;
    return HAL_OSD_VALIGN_CENTER;
}

inline HalOsdFontWeight ml_to_hal_font_weight(osd::font_weight_t w)
{
    switch (w)
    {
        case osd::font_weight_t::BOLD:
            return HAL_OSD_FONT_WEIGHT_BOLD;
        case osd::font_weight_t::NORMAL:
        default:
            return HAL_OSD_FONT_WEIGHT_NORMAL;
    }
}

inline HalOsdColor ml_to_hal_color(const osd::rgba_color_t &c)
{
    return HalOsdColor{c.red, c.green, c.blue, c.alpha};
}

/* -- Extract base fields from an ML overlay into HalOsdOverlayBase -- */

template <typename OverlayT>
inline void ml_overlay_to_hal_base(const OverlayT &src, HalOsdOverlayBase *dst, HalOsdOverlayType type)
{
    std::memset(dst, 0, sizeof(*dst));
    std::strncpy(dst->id, src.id.c_str(), sizeof(dst->id) - 1);
    dst->id[sizeof(dst->id) - 1] = '\0';
    dst->type = type;
    dst->enabled = true;
    dst->x = src.x;
    dst->y = src.y;
    dst->z_index = src.z_index;
    dst->angle = src.angle;
    dst->rotation_policy = rotation_policy_to_hal_any(src.rotation_alignment_policy);
    dst->h_align = halign_to_hal_any(src.horizontal_alignment);
    dst->v_align = valign_to_hal_any(src.vertical_alignment);
}

/* -- Helper to fill text-related HAL fields from a BaseTextOverlay -- */

template <typename BaseTextT>
static inline void ml_base_text_to_hal(const BaseTextT &src, HalOsdTextOverlay *dst)
{
    std::strncpy(dst->label, src.label.c_str(), sizeof(dst->label) - 1);
    dst->label[sizeof(dst->label) - 1] = '\0';
    dst->text_color = color_to_hal_any(src.text_color);
    dst->background_color = color_to_hal_any(src.background_color);
    std::strncpy(dst->font_path, src.font_path.c_str(), sizeof(dst->font_path) - 1);
    dst->font_path[sizeof(dst->font_path) - 1] = '\0';
    dst->font_size = src.font_size;
    if constexpr (is_std_optional_v<decltype(src.line_thickness)>)
    {
        dst->line_thickness = src.line_thickness.value_or(0);
    }
    else
    {
        dst->line_thickness = src.line_thickness;
    }
    dst->shadow_color = color_to_hal_any(src.shadow_color);
    if constexpr (is_std_optional_v<decltype(src.shadow_offset_x)>)
    {
        dst->shadow_offset_x = src.shadow_offset_x.value_or(0.0f);
    }
    else
    {
        dst->shadow_offset_x = src.shadow_offset_x;
    }
    if constexpr (is_std_optional_v<decltype(src.shadow_offset_y)>)
    {
        dst->shadow_offset_y = src.shadow_offset_y.value_or(0.0f);
    }
    else
    {
        dst->shadow_offset_y = src.shadow_offset_y;
    }
    if constexpr (is_std_optional_v<decltype(src.outline_size)>)
    {
        dst->outline_size = src.outline_size.value_or(0);
    }
    else
    {
        dst->outline_size = src.outline_size;
    }
    dst->outline_color = color_to_hal_any(src.outline_color);
    dst->font_weight = font_weight_to_hal_any(src.font_weight);
}

/* -- Full overlay conversions ML -> HAL -- */

template <typename ImageT>
inline void ml_to_hal_image(const ImageT &src, HalOsdImageOverlay *dst)
{
    std::memset(dst, 0, sizeof(*dst));
    ml_overlay_to_hal_base(src, &dst->base, HAL_OSD_OVERLAY_IMAGE);
    dst->width = src.width;
    dst->height = src.height;
    std::strncpy(dst->image_path, src.image_path.c_str(), sizeof(dst->image_path) - 1);
    dst->image_path[sizeof(dst->image_path) - 1] = '\0';
}

template <typename TextT>
inline void ml_to_hal_text(const TextT &src, HalOsdTextOverlay *dst)
{
    std::memset(dst, 0, sizeof(*dst));
    ml_overlay_to_hal_base(src, &dst->base, HAL_OSD_OVERLAY_TEXT);
    ml_base_text_to_hal(src, dst);
}

template <typename DateTimeT>
inline void ml_to_hal_datetime(const DateTimeT &src, HalOsdDateTimeOverlay *dst)
{
    std::memset(dst, 0, sizeof(*dst));
    ml_overlay_to_hal_base(src, &dst->text.base, HAL_OSD_OVERLAY_DATETIME);
    ml_base_text_to_hal(src, &dst->text);
    const char *fmt = nullptr;
    if constexpr (is_std_optional_v<decltype(src.datetime_format)>)
    {
        fmt = src.datetime_format.has_value() ? src.datetime_format->c_str() : "";
    }
    else
    {
        fmt = src.datetime_format.c_str();
    }
    std::strncpy(dst->datetime_format, fmt ? fmt : "", sizeof(dst->datetime_format) - 1);
    dst->datetime_format[sizeof(dst->datetime_format) - 1] = '\0';
}

inline void ml_to_hal_custom(const osd::CustomOverlay &src, HalOsdCustomOverlay *dst)
{
    std::memset(dst, 0, sizeof(*dst));
    ml_overlay_to_hal_base(src, &dst->base, HAL_OSD_OVERLAY_CUSTOM);
    dst->width = src.width;
    dst->height = src.height;
    dst->format = (src.get_format() == osd::ARGB) ? HAL_OSD_CUSTOM_FMT_ARGB : HAL_OSD_CUSTOM_FMT_A420;
    dst->data = nullptr;
    dst->data_size = 0;
}

/* ====================================================================
 * Font size recalculation helpers
 * ==================================================================== */

/** Convert absolute pixel font size to a ratio of frame width. */
inline float font_size_abs_to_relative(float font_size, uint32_t width)
{
    if (width == 0)
        return 0.0f;
    return font_size / static_cast<float>(width);
}

/** Convert relative (ratio-of-width) font size to absolute pixels. */
inline float font_size_relative_to_abs(float relative, uint32_t width)
{
    return relative * static_cast<float>(width);
}

/** Check whether the rotation angle implies a portrait orientation (90 or 270). */
inline bool is_portrait_rotation(HalRotationAngle angle)
{
    return angle == HAL_ROTATION_ANGLE_90 || angle == HAL_ROTATION_ANGLE_270;
}

/**
 * Clamp an OSD overlay's normalized origin (and, for image overlays, its
 * footprint) so the medialib's absolute placement stays inside the DSP blend
 * frame under a portrait rotation. Returns true if any coordinate changed.
 *
 * Root cause: the closed-source medialib converts normalized overlay coords to
 * absolute pixels using the ENCODER OUTPUT dimensions (portrait, e.g. 540x960)
 * but the DSP blend that composites the overlay runs on the PRE-rotation frame
 * (960x540). Under portrait, overlay y is scaled by the long output axis (960)
 * yet validated against the short blend axis (540), so any norm_y > short/long
 * (e.g. > 0.5625) lands outside the blend frame => "Overlay y_offset outside
 * image height" => DSP blend fails => the whole encoded stream wedges (black
 * screen, pipeline stall on every restart while the bad OSD persists). We
 * cannot fix the medialib conversion, so we clamp the offending normalized
 * coordinate here, before handing the overlay to blender->add_overlay().
 *
 * Verified correct for HAL_ROTATION_ANGLE_90 on device 192.168.93.213.
 *
 * @param x, y        Overlay normalized origin (mutated in place).
 * @param span_x      Overlay normalized width  (image only; 0 for text/datetime).
 * @param span_y      Overlay normalized height (image only; 0 for text/datetime).
 * @param out_w/out_h Encoder OUTPUT dims (already portrait-swapped) for stream.
 * @param rotation    Current rotation angle.
 */
inline bool clamp_osd_for_rotation(float &x, float &y,
                                   float span_x, float span_y,
                                   uint32_t out_w, uint32_t out_h,
                                   HalRotationAngle rotation)
{
    if (out_w == 0 || out_h == 0 || !is_portrait_rotation(rotation))
    {
        return false;
    }
    const float lo = static_cast<float>(std::min(out_w, out_h));
    const float hi = static_cast<float>(std::max(out_w, out_h));
    const float ratio = lo / hi; /* short/long, < 1 in portrait */

    bool clamped = false;
    /* Keep origin (+ footprint) within the blend frame on the clamped axis. */
    auto clamp_axis = [&clamped, ratio](float &v, float span) {
        float cap = ratio - span;
        if (cap < 0.0f)
        {
            cap = 0.0f;
        }
        if (v > cap)
        {
            v = cap;
            clamped = true;
        }
        if (v < 0.0f)
        {
            v = 0.0f;
            clamped = true;
        }
    };

    /* Verified for 90deg: y is the long axis (scaled by output_h, validated
     * against the short blend height). For 270deg the dim-mismatch is the same
     * class but unverified on-device; clamp y identically and also clamp x
     * conservatively so neither axis can exceed the blend frame. A repositioned
     * overlay is strictly preferable to a wedged stream. */
    clamp_axis(y, span_y);
    if (rotation == HAL_ROTATION_ANGLE_270)
    {
        clamp_axis(x, span_x);
    }
    return clamped;
}

/* ====================================================================
 * clear_encoder_osd  (convenience wrapper, mirrors hailo15::video_ml)
 * ==================================================================== */

/**
 * Remove all OSD overlays from one (or all) encoded output streams in a profile.
 * @param p          Profile to modify (in-place).
 * @param stream_id  If non-null, clear only this stream; otherwise clear all.
 */
inline void clear_encoder_osd(config_profile_t &p, const std::string *stream_id)
{
    for (auto &kv : p.encoded_output_streams)
    {
        if (stream_id && kv.first != *stream_id)
        {
            continue;
        }
        kv.second.osd.image_overlays.clear();
        kv.second.osd.text_overlays.clear();
        kv.second.osd.datetime_overlays.clear();
    }
}

/* ====================================================================
 * recalculate_osd_on_layout_change
 *
 * When encoder resolution or rotation changes, rescale OSD font sizes
 * proportionally to the new width.  This matches the webserver
 * OsdResource::update_osds() behaviour.
 * ==================================================================== */

/**
 * Recalculate OSD overlays for a specific encoder stream when resolution
 * or rotation changes.
 *
 * @param priv          Hailo15MediaPriv (for media_lib access and osd_layout_by_encoder).
 * @param stream_id     Encoder stream id (e.g. "sink0").
 * @param new_w         New encoder input width after change.
 * @param new_h         New encoder input height after change.
 * @param new_rotation  New rotation angle after change.
 */
inline void recalculate_osd_on_layout_change(Hailo15MediaPriv *priv,
                                              const std::string &stream_id,
                                              uint32_t new_w, uint32_t new_h,
                                              HalRotationAngle new_rotation)
{
    if (!priv || !priv->media_lib)
        return;

    /* 1. Look up old state. If not found, just save current and return. */
    auto layout_it = priv->osd_layout_by_encoder.find(stream_id);
    if (layout_it == priv->osd_layout_by_encoder.end())
    {
        priv->osd_layout_by_encoder[stream_id] = OsdLayoutState{new_w, new_h, static_cast<int>(new_rotation)};
        return;
    }

    OsdLayoutState &old_state = layout_it->second;
    uint32_t old_w = old_state.width;
    uint32_t old_h = old_state.height;

    /* 3. Caller already provides new_w/new_h with portrait swap applied. */

    /* 4. Skip if nothing changed (same dimensions and same portrait/landscape). */
    bool old_portrait = is_portrait_rotation(static_cast<HalRotationAngle>(old_state.rotation));
    bool new_portrait = is_portrait_rotation(new_rotation);
    if (old_w == new_w && old_h == new_h && old_portrait == new_portrait)
    {
        old_state.rotation = static_cast<int>(new_rotation);
        return;
    }

    /* 5-6. Get the current profile and find the stream's OSD config. */
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        old_state = OsdLayoutState{new_w, new_h, static_cast<int>(new_rotation)};
        return;
    }
    config_profile_t prof = prof_exp.value();

    auto stream_it = prof.encoded_output_streams.find(stream_id);
    if (stream_it == prof.encoded_output_streams.end())
    {
        old_state = OsdLayoutState{new_w, new_h, static_cast<int>(new_rotation)};
        return;
    }

    /* Webserver behaviour:
     * - On resolution/rotation changes, it re-emits OSD config and deletes existing overlay IDs
     *   because previous overlays may not fit the new stream geometry.
     * - It also rescales font sizes proportionally to width.
     *
     * In HAL we mirror the safety aspect: clear existing overlays for this stream in the profile
     * so that the next OSD configure/add recreates them cleanly (avoids DSP verify failures). */
    bool modified = false;
    config_stream_osd_t &osd = stream_it->second.osd;

    /* Clear overlays for this stream: safest alignment with webserver's "delete all ids" on layout changes. */
    if (!osd.image_overlays.empty() || !osd.text_overlays.empty() || !osd.datetime_overlays.empty())
    {
        /* Also remove runtime overlays from the blender, otherwise the DSP may still try to blend
         * stale overlays created earlier via add_overlay(). */
        try
        {
            auto enc_it = priv->media_lib->m_encoders.find(stream_id);
            if (enc_it != priv->media_lib->m_encoders.end() && enc_it->second)
            {
                auto blender = enc_it->second->get_osd_blender();
                if (blender)
                {
                    for (const auto &ptr : osd.image_overlays)
                    {
                        if (ptr)
                        {
                            (void)blender->remove_overlay(ptr->id);
                        }
                    }
                    for (const auto &ptr : osd.text_overlays)
                    {
                        if (ptr)
                        {
                            (void)blender->remove_overlay(ptr->id);
                        }
                    }
                    for (const auto &ptr : osd.datetime_overlays)
                    {
                        if (ptr)
                        {
                            (void)blender->remove_overlay(ptr->id);
                        }
                    }
                }
            }
        }
        catch (...)
        {
            /* Best-effort cleanup: even if blender removal fails, still clear profile OSD to avoid further growth. */
        }

        osd.image_overlays.clear();
        osd.text_overlays.clear();
        osd.datetime_overlays.clear();
        modified = true;
    }

    if (old_w > 0 && new_w > 0 && old_w != new_w)
    {
        for (auto &text_ptr : osd.text_overlays)
        {
            if (text_ptr)
            {
                float old_font = text_ptr->font_size;
                text_ptr->font_size = (old_font / static_cast<float>(old_w)) * static_cast<float>(new_w);
                modified = true;
            }
        }
        for (auto &dt_ptr : osd.datetime_overlays)
        {
            if (dt_ptr)
            {
                float old_font = dt_ptr->font_size;
                dt_ptr->font_size = (old_font / static_cast<float>(old_w)) * static_cast<float>(new_w);
                modified = true;
            }
        }
    }

    /* 8. Apply the modified profile via set_override_parameters. */
    if (modified)
    {
        priv->media_lib->set_override_parameters(prof);
    }

    /* 9. Update the layout state. */
    old_state = OsdLayoutState{new_w, new_h, static_cast<int>(new_rotation)};
}

} // namespace hailo15::osd_ml
