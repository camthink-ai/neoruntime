/**
 * @file hailo15_osd_ml.hpp
 * @brief Inline conversion helpers between HAL OSD types and Hailo MediaLibrary OSD types.
 *
 * Medialib 1.13 (SDK 4.0.23) removed the imperative osd::Blender API: overlays now live
 * declaratively in the per-stream config_stream_osd_t of the medialib profile, and the
 * internal blender picks them up from the profile attached to each encoded buffer.
 * Conversions therefore target the GLOBAL overlay structs from media_library_types.hpp
 * (ImageOverlay / TextOverlay / DateTimeOverlay), held via shared_ptr in
 * config_stream_osd_t. osd_types.hpp is still included for DEFAULT_FONT_PATH /
 * DEFAULT_DATETIME_STRING / osd::calculate_text_size (the global calculate_text_size
 * is not exported by libmedialib).
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
#include <hailo/media_library/media_library_types.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
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
 * Overlay x/y and ImageOverlay width/height are RELATIVE (0..1) -> auto-adapt, keep.
 * font_size / line_thickness / outline_size are ABSOLUTE (px) -> scale by width ratio.
 * Static privacy mask polygon vertices are ABSOLUTE int px -> scale by w/h ratio + clamp.
 * Since medialib 1.13 the blenders read OSD/masking from the profile attached to each
 * frame, so applying the rescaled profile via set_override_parameters() is sufficient —
 * no blender re-push is needed.
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
 * HAL -> ML conversions (global overlay structs from media_library_types.hpp)
 * ==================================================================== */

inline rotation_alignment_policy_t hal_to_ml_rotation_policy(HalOsdRotationPolicy p)
{
    switch (p)
    {
        case HAL_OSD_ROTATION_POLICY_TOP_LEFT:
            return rotation_alignment_policy_t::TOP_LEFT;
        case HAL_OSD_ROTATION_POLICY_CENTER:
        default:
            return rotation_alignment_policy_t::CENTER;
    }
}

inline HorizontalAlignment hal_to_ml_halign(HalOsdHorizontalAlignment a)
{
    switch (a)
    {
        case HAL_OSD_HALIGN_CENTER:
            return HorizontalAlignment::CENTER;
        case HAL_OSD_HALIGN_RIGHT:
            return HorizontalAlignment::RIGHT;
        case HAL_OSD_HALIGN_LEFT:
        default:
            return HorizontalAlignment::LEFT;
    }
}

inline VerticalAlignment hal_to_ml_valign(HalOsdVerticalAlignment a)
{
    switch (a)
    {
        case HAL_OSD_VALIGN_CENTER:
            return VerticalAlignment::CENTER;
        case HAL_OSD_VALIGN_BOTTOM:
            return VerticalAlignment::BOTTOM;
        case HAL_OSD_VALIGN_TOP:
        default:
            return VerticalAlignment::TOP;
    }
}

inline font_weight_t hal_to_ml_font_weight(HalOsdFontWeight w)
{
    switch (w)
    {
        case HAL_OSD_FONT_WEIGHT_BOLD:
            return font_weight_t::BOLD;
        case HAL_OSD_FONT_WEIGHT_NORMAL:
        default:
            return font_weight_t::NORMAL;
    }
}

inline rgba_color_t hal_to_ml_color(const HalOsdColor &c)
{
    return rgba_color_t{c.r, c.g, c.b, c.a};
}

/** r < 0 disables a colour in the HAL model; map that to an unset std::optional. */
inline std::optional<rgba_color_t> hal_to_ml_opt_color(const HalOsdColor &c)
{
    if (c.r < 0 || c.g < 0 || c.b < 0 || c.a < 0)
    {
        return std::nullopt;
    }
    return std::optional<rgba_color_t>(hal_to_ml_color(c));
}

/** Fill the shared Overlay base fields from a HAL base. */
inline void hal_base_to_ml_overlay(const HalOsdOverlayBase &h, Overlay *ml)
{
    ml->id = std::string(h.id);
    ml->x = h.x;
    ml->y = h.y;
    ml->z_index = h.z_index;
    ml->angle = h.angle;
    ml->rotation_alignment_policy = hal_to_ml_rotation_policy(h.rotation_policy);
    ml->horizontal_alignment = std::optional<HorizontalAlignment>(hal_to_ml_halign(h.h_align));
    ml->vertical_alignment = std::optional<VerticalAlignment>(hal_to_ml_valign(h.v_align));
}

/** Fill BaseTextOverlay fields (used by both TextOverlay and DateTimeOverlay). */
template <typename HalTextT>
inline void hal_text_to_ml_base_text(const HalTextT &h, BaseTextOverlay *ml)
{
    const char *font_path = (h.font_path[0] != '\0') ? h.font_path : DEFAULT_FONT_PATH;
    ml->label = std::string(h.label);
    ml->text_color = hal_to_ml_color(h.text_color);
    ml->background_color = hal_to_ml_color(h.background_color);
    ml->font_path = std::string(font_path);
    ml->font_size = static_cast<int>(std::lround(h.font_size));
    ml->line_thickness = (h.line_thickness > 0) ? std::optional<int>(h.line_thickness) : std::nullopt;
    const std::optional<rgba_color_t> shadow = hal_to_ml_opt_color(h.shadow_color);
    ml->shadow_color = shadow;
    ml->shadow_offset_x = shadow ? std::optional<float>(h.shadow_offset_x) : std::nullopt;
    ml->shadow_offset_y = shadow ? std::optional<float>(h.shadow_offset_y) : std::nullopt;
    ml->font_weight = std::optional<font_weight_t>(hal_to_ml_font_weight(h.font_weight));
    ml->outline_size = (h.outline_size > 0) ? std::optional<int>(h.outline_size) : std::nullopt;
    ml->outline_color = (h.outline_size > 0) ? hal_to_ml_opt_color(h.outline_color) : std::nullopt;
    /* m_width / m_height are informational text metrics; fill via the exported
     * osd::calculate_text_size (the global one is not exported by libmedialib). */
    try
    {
        mat_dims d = osd::calculate_text_size(ml->label, ml->font_path, ml->font_size,
                                              ml->line_thickness.value_or(0));
        ml->m_width = static_cast<size_t>(d.width);
        ml->m_height = static_cast<size_t>(d.height);
    }
    catch (...)
    {
        ml->m_width = 0;
        ml->m_height = 0;
    }
}

/* -- Full overlay conversions HAL -> ML (shared_ptr for config_stream_osd_t) -- */

inline std::shared_ptr<ImageOverlay> hal_to_ml_image(const HalOsdImageOverlay &h)
{
    auto ml = std::make_shared<ImageOverlay>();
    hal_base_to_ml_overlay(h.base, ml.get());
    ml->width = h.width;
    ml->height = h.height;
    ml->image_path = std::string(h.image_path);
    return ml;
}

inline std::shared_ptr<TextOverlay> hal_to_ml_text(const HalOsdTextOverlay &h)
{
    auto ml = std::make_shared<TextOverlay>();
    hal_base_to_ml_overlay(h.base, ml.get());
    hal_text_to_ml_base_text(h, ml.get());
    return ml;
}

inline std::shared_ptr<DateTimeOverlay> hal_to_ml_datetime(const HalOsdDateTimeOverlay &h)
{
    auto ml = std::make_shared<DateTimeOverlay>();
    hal_base_to_ml_overlay(h.text.base, ml.get());
    hal_text_to_ml_base_text(h.text, ml.get());
    const char *fmt = (h.datetime_format[0] != '\0') ? h.datetime_format : DEFAULT_DATETIME_STRING;
    ml->datetime_format = std::optional<std::string>(std::string(fmt));
    return ml;
}

/* Custom overlays (raw ARGB/A420 buffers) have no public path in medialib 1.13
 * (CustomOverlay became internal); no HAL->ML conversion exists. */

/* ====================================================================
 * ML -> HAL conversions (operate on the global overlay structs)
 * ==================================================================== */

inline HalOsdRotationPolicy ml_to_hal_rotation_policy(rotation_alignment_policy_t p)
{
    switch (p)
    {
        case rotation_alignment_policy_t::TOP_LEFT:
            return HAL_OSD_ROTATION_POLICY_TOP_LEFT;
        case rotation_alignment_policy_t::CENTER:
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
    return (v == (int)rotation_alignment_policy_t::TOP_LEFT) ? HAL_OSD_ROTATION_POLICY_TOP_LEFT
                                                             : HAL_OSD_ROTATION_POLICY_CENTER;
}

template <typename FontWeightT>
static inline HalOsdFontWeight font_weight_to_hal_any(const FontWeightT &w)
{
    const int v = enum_int_value(w);
    return (v == (int)font_weight_t::BOLD) ? HAL_OSD_FONT_WEIGHT_BOLD : HAL_OSD_FONT_WEIGHT_NORMAL;
}

template <typename AlignT>
static inline HalOsdHorizontalAlignment halign_to_hal_any(const AlignT &a)
{
    const float f = align_float_value(a);
    if (std::fabs(f - HorizontalAlignment::CENTER.as_float()) < 1e-6f)
    {
        return HAL_OSD_HALIGN_CENTER;
    }
    if (std::fabs(f - HorizontalAlignment::RIGHT.as_float()) < 1e-6f)
    {
        return HAL_OSD_HALIGN_RIGHT;
    }
    return HAL_OSD_HALIGN_LEFT;
}

template <typename AlignT>
static inline HalOsdVerticalAlignment valign_to_hal_any(const AlignT &a)
{
    const float f = align_float_value(a);
    if (std::fabs(f - VerticalAlignment::CENTER.as_float()) < 1e-6f)
    {
        return HAL_OSD_VALIGN_CENTER;
    }
    if (std::fabs(f - VerticalAlignment::BOTTOM.as_float()) < 1e-6f)
    {
        return HAL_OSD_VALIGN_BOTTOM;
    }
    return HAL_OSD_VALIGN_TOP;
}

inline HalOsdHorizontalAlignment ml_to_hal_halign(const HorizontalAlignment &a)
{
    float v = a.as_float();
    if (v <= 0.01f)
        return HAL_OSD_HALIGN_LEFT;
    if (v >= 0.99f)
        return HAL_OSD_HALIGN_RIGHT;
    return HAL_OSD_HALIGN_CENTER;
}

inline HalOsdVerticalAlignment ml_to_hal_valign(const VerticalAlignment &a)
{
    float v = a.as_float();
    if (v <= 0.01f)
        return HAL_OSD_VALIGN_TOP;
    if (v >= 0.99f)
        return HAL_OSD_VALIGN_BOTTOM;
    return HAL_OSD_VALIGN_CENTER;
}

inline HalOsdFontWeight ml_to_hal_font_weight(font_weight_t w)
{
    switch (w)
    {
        case font_weight_t::BOLD:
            return HAL_OSD_FONT_WEIGHT_BOLD;
        case font_weight_t::NORMAL:
        default:
            return HAL_OSD_FONT_WEIGHT_NORMAL;
    }
}

inline HalOsdColor ml_to_hal_color(const rgba_color_t &c)
{
    return HalOsdColor{c.red, c.green, c.blue, c.alpha};
}

/* -- Extract base fields from an ML overlay into HalOsdOverlayBase -- */

template <typename OverlayT>
inline void ml_overlay_to_hal_base(const OverlayT &src, HalOsdOverlayBase *dst, HalOsdOverlayType type,
                                   bool enabled = true)
{
    std::memset(dst, 0, sizeof(*dst));
    std::strncpy(dst->id, src.id.c_str(), sizeof(dst->id) - 1);
    dst->id[sizeof(dst->id) - 1] = '\0';
    dst->type = type;
    dst->enabled = enabled;
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
inline void ml_to_hal_image(const ImageT &src, HalOsdImageOverlay *dst, bool enabled = true)
{
    std::memset(dst, 0, sizeof(*dst));
    ml_overlay_to_hal_base(src, &dst->base, HAL_OSD_OVERLAY_IMAGE, enabled);
    dst->width = src.width;
    dst->height = src.height;
    std::strncpy(dst->image_path, src.image_path.c_str(), sizeof(dst->image_path) - 1);
    dst->image_path[sizeof(dst->image_path) - 1] = '\0';
}

template <typename TextT>
inline void ml_to_hal_text(const TextT &src, HalOsdTextOverlay *dst, bool enabled = true)
{
    std::memset(dst, 0, sizeof(*dst));
    ml_overlay_to_hal_base(src, &dst->base, HAL_OSD_OVERLAY_TEXT, enabled);
    ml_base_text_to_hal(src, dst);
}

template <typename DateTimeT>
inline void ml_to_hal_datetime(const DateTimeT &src, HalOsdDateTimeOverlay *dst, bool enabled = true)
{
    std::memset(dst, 0, sizeof(*dst));
    ml_overlay_to_hal_base(src, &dst->text.base, HAL_OSD_OVERLAY_DATETIME, enabled);
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
 * coordinate here, before putting the overlay into the stream's OSD config.
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
 * Serialized under priv->osd_state_mu for the WHOLE fetch -> mutate ->
 * set_override_parameters cycle, exactly like the OSD ops in
 * hailo15_osd_impl.cpp: a concurrent OSD op running its own cycle on a
 * pre-clear profile copy would otherwise re-apply cleared overlays (lost
 * update). Callers must NOT hold priv->mutex (same discipline as OSD ops —
 * verified for both call sites in hailo15_media_impl.cpp: the
 * rotation_full_reinit step-13 loop and set_transform's light path). NB: the
 * callers apply their own earlier-fetched profile copy afterwards
 * (set_override_parameters(p) in dynamic_change/rotation paths) OUTSIDE this
 * lock — a pre-existing residual race, unchanged by this serialization.
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

    std::lock_guard<std::mutex> osd_lock(priv->osd_state_mu);

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

    /* Clear overlays for this stream: safest alignment with webserver's "delete all ids" on layout changes.
     * The declarative model has no separate blender state to purge — clearing the profile config is enough. */
    if (!osd.image_overlays.empty() || !osd.text_overlays.empty() || !osd.datetime_overlays.empty())
    {
        osd.image_overlays.clear();
        osd.text_overlays.clear();
        osd.datetime_overlays.clear();
        modified = true;
    }

    /* Shadow-disabled overlays are snapshots of the old geometry; drop them so a
     * later osd_enable can't resurrect an overlay with stale absolute fields.
     * (Caller holds osd_state_mu — plain access, no nested lock.) */
    {
        auto shadow_it = priv->osd_disabled_by_stream.find(stream_id);
        if (shadow_it != priv->osd_disabled_by_stream.end() && !shadow_it->second.empty())
        {
            priv->osd_disabled_by_stream.erase(shadow_it);
            modified = true;
        }
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
