/**
 * @file hailo15_media_impl.cpp
 * @brief Hailo-15 HAL media pipeline (MediaLibrary).
 */

#include "hailo15_common.hpp"
#include "hailo15_default_medialib.hpp"
#include "hailo15_media_priv.hpp"
#include "hailo15_medialib_config_field.hpp"
#include "hailo15_ml_frontend_bridge.hpp"
#include "hailo15_osd_ml.hpp"
#include "hailo15_video_ml.hpp"

#include "common/hal_log.h"

#include "media/hal_media_internal.h"

#include <hailo/media_library/encoder_config_types.hpp>
#include <hailo/media_library/media_library_api_types.hpp>

#include <algorithm>
#include <chrono>
#include <set>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <thread>
#include <openssl/sha.h>
#include <optional>
#include <sstream>
#include <iomanip>
#include <type_traits>
#include <unordered_map>
#include <variant>

extern "C" void hailo15_isp_webserver_reset_state(void);

static int reinit_media_library_on_layout_change(HalMediaContext *hm,
                                                   Hailo15MediaPriv *priv,
                                                   const config_profile_t &target_profile,
                                                   const char *tag);

namespace
{

/* Convert a JSON scalar to its canonical string representation,
 * matching medialib's ConfigValidator::to_canonical_string exactly.
 * Floats use std::fixed with 8 decimal places; everything else uses dump(). */
static std::optional<std::string> canonical_scalar(const nlohmann::json &val)
{
    try
    {
        if (val.is_number_float())
        {
            double v = val.get<double>();
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(8) << v;
            return oss.str();
        }
        return val.dump(-1);
    }
    catch (...)
    {
        return std::nullopt;
    }
}

/* Incrementally hash a JSON tree into SHA256_CTX.
 * Matches medialib's ConfigValidator::hash_json_incrementally:
 *   - Objects: sorted keys, key.dump() + ":" + value + ","
 *   - Arrays: elements in order + ","
 *   - Scalars: to_canonical_string (fixed-8 for floats) */
static bool hash_json_incremental(const nlohmann::json &val, SHA256_CTX &ctx)
{
    if (val.is_object())
    {
        SHA256_Update(&ctx, "{", 1);
        std::vector<std::string> keys;
        for (auto it = val.begin(); it != val.end(); ++it) keys.push_back(it.key());
        std::sort(keys.begin(), keys.end());
        for (size_t i = 0; i < keys.size(); i++)
        {
            auto ks = nlohmann::json(keys[i]).dump();
            SHA256_Update(&ctx, ks.data(), ks.size());
            SHA256_Update(&ctx, ":", 1);
            if (!hash_json_incremental(val.at(keys[i]), ctx)) return false;
            if (i + 1 < keys.size()) SHA256_Update(&ctx, ",", 1);
        }
        SHA256_Update(&ctx, "}", 1);
        return true;
    }
    if (val.is_array())
    {
        SHA256_Update(&ctx, "[", 1);
        for (size_t i = 0; i < val.size(); i++)
        {
            if (!hash_json_incremental(val[i], ctx)) return false;
            if (i + 1 < val.size()) SHA256_Update(&ctx, ",", 1);
        }
        SHA256_Update(&ctx, "]", 1);
        return true;
    }
    auto canon = canonical_scalar(val);
    if (!canon) return false;
    SHA256_Update(&ctx, canon->data(), canon->size());
    return true;
}

/* Compute content_hash exactly like medialib's ConfigValidator:
 *   1. Remove "metadata" key
 *   2. Incremental SHA-256 with sorted keys and fixed-8 float formatting
 *   3. Return hex string */
static std::optional<std::string> compute_medialib_content_hash(const nlohmann::json &node)
{
    if (!node.is_object() || !node.contains("metadata"))
    {
        return std::nullopt;
    }

    nlohmann::json content = node;
    content.erase("metadata");

    SHA256_CTX ctx;
    if (SHA256_Init(&ctx) != 1) return std::nullopt;
    if (!hash_json_incremental(content, ctx)) return std::nullopt;
    unsigned char out[SHA256_DIGEST_LENGTH];
    if (SHA256_Final(out, &ctx) != 1) return std::nullopt;

    std::ostringstream oss;
    oss << std::hex << std::setw(2) << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        oss << std::setw(2) << static_cast<int>(out[i]);
    return oss.str();
}

static bool sanitize_application_analytics(nlohmann::json &j)
{
    bool changed = false;

    auto recompute_content_hash = [&](nlohmann::json &node) {
        if (!node.is_object() || !node.contains("metadata") || !node["metadata"].is_object() ||
            !node["metadata"].contains("content_hash") || !node["metadata"]["content_hash"].is_string())
        {
            return;
        }
        auto h = compute_medialib_content_hash(node);
        if (!h.has_value())
        {
            return;
        }
        node["metadata"]["content_hash"] = *h;
        changed = true;
    };

    auto sanitize_container = [&](nlohmann::json &container) {
        if (!container.is_object() || !container.contains("application_analytics"))
        {
            return;
        }
        auto &aa = container["application_analytics"];
        if (aa.is_null())
        {
            container.erase("application_analytics");
            changed = true;
            return;
        }
        if (!aa.is_object())
        {
            return;
        }
        /* Known ML bug: backup may contain {"application_analytics":{"application_analytics":{}}}
         * which fails schema validation. If empty or double-nested, drop it. */
        if (aa.empty())
        {
            container.erase("application_analytics");
            changed = true;
            return;
        }
        if (aa.contains("application_analytics") && aa["application_analytics"].is_object() &&
            aa["application_analytics"].empty())
        {
            container.erase("application_analytics");
            changed = true;
            return;
        }
    };

    sanitize_container(j);
    recompute_content_hash(j);
    if (j.contains("application_settings"))
    {
        sanitize_container(j["application_settings"]);
        recompute_content_hash(j["application_settings"]);
    }
    return changed;
}

static void sanitize_profile_backups_on_disk(const std::string &backup_root)
{
    if (backup_root.empty())
    {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(backup_root, ec))
    {
        return;
    }

    for (auto it = std::filesystem::recursive_directory_iterator(backup_root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
    {
        if (ec || !it->is_regular_file(ec))
        {
            continue;
        }
        const auto &p = it->path();
        if (p.extension() != ".json")
        {
            continue;
        }
        const std::string filename = p.filename().string();
        const bool is_app_settings = (filename == "application_settings.json");
        if (!is_app_settings)
        {
            continue;
        }

        std::ifstream in(p, std::ios::binary);
        if (!in)
        {
            continue;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        const std::string raw = ss.str();
        if (raw.empty())
        {
            continue;
        }

        nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
        if (j.is_discarded())
        {
            continue;
        }
        if (!sanitize_application_analytics(j))
        {
            continue;
        }

        std::ofstream out(p, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            continue;
        }
        out << j.dump(2);
    }
}

const char *ml_pipeline_state_str(media_library_pipeline_state_t st)
{
    switch (st)
    {
        case media_library_pipeline_state_t::PIPELINE_STATE_UNINITIALIZED:
            return "UNINITIALIZED";
        case media_library_pipeline_state_t::PIPELINE_STATE_RUNNING:
            return "RUNNING";
        case media_library_pipeline_state_t::PIPELINE_STATE_STOPPED:
            return "STOPPED";
        default:
            return "?";
    }
}

const char *ml_throttling_state_str(media_library_throttling_state_t st)
{
    switch (st)
    {
        case media_library_throttling_state_t::THROTTLING_STATE_UNINITIALIZED:
            return "UNINITIALIZED";
        case media_library_throttling_state_t::THROTTLING_STATE_FULL_PERFORMANCE:
            return "FULL_PERFORMANCE";
        case media_library_throttling_state_t::THROTTLING_STATE_COOLING:
            return "COOLING";
        case media_library_throttling_state_t::THROTTLING_STATE_S0:
            return "S0";
        case media_library_throttling_state_t::THROTTLING_STATE_S1:
            return "S1";
        case media_library_throttling_state_t::THROTTLING_STATE_S2:
            return "S2";
        case media_library_throttling_state_t::THROTTLING_STATE_S3:
            return "S3";
        case media_library_throttling_state_t::THROTTLING_STATE_S4:
            return "S4";
        default:
            return "?";
    }
}

const char *hal_status_str(HalStatus s)
{
    switch (s)
    {
        case HAL_STATUS_UNINITIALIZED:
            return "UNINITIALIZED";
        case HAL_STATUS_INITIALIZED:
            return "INITIALIZED";
        case HAL_STATUS_RUNNING:
            return "RUNNING";
        case HAL_STATUS_STOPPED:
            return "STOPPED";
        case HAL_STATUS_ERROR:
            return "ERROR";
        default:
            return "?";
    }
}

void copy_stream_layout_from_profile(const config_profile_t &prof, std::vector<std::string> *fe,
                                     std::vector<std::string> *enc)
{
    fe->clear();
    enc->clear();
    for (const auto &res : prof.application_settings.application_input_streams.resolutions)
    {
        if (!res.stream_id.empty())
        {
            fe->push_back(res.stream_id);
        }
    }
    for (const auto &kv : prof.encoded_output_streams)
    {
        enc->push_back(kv.first);
    }
}

/** True if frontend or encoder stream ids differ (order ignored). Used to decide pre-set_profile unsubscribe. */
bool profile_stream_layout_differs(const config_profile_t &a, const config_profile_t &b)
{
    std::vector<std::string> fe_a, fe_b, enc_a, enc_b;
    copy_stream_layout_from_profile(a, &fe_a, &enc_a);
    copy_stream_layout_from_profile(b, &fe_b, &enc_b);
    std::sort(fe_a.begin(), fe_a.end());
    std::sort(fe_b.begin(), fe_b.end());
    std::sort(enc_a.begin(), enc_a.end());
    std::sort(enc_b.begin(), enc_b.end());
    return fe_a != fe_b || enc_a != enc_b;
}

EncoderType get_encoder_type_from_cfg(const encoder_config_t &config_variant)
{
    return std::visit(
        [](const auto &config) -> EncoderType {
            using T = std::decay_t<decltype(config)>;
            if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
            {
                return EncoderType::Hailo;
            }
            else if constexpr (std::is_same_v<T, jpeg_encoder_config_t>)
            {
                return EncoderType::Jpeg;
            }
            return EncoderType::Hailo;
        },
        config_variant);
}

/** Mirrors MediaLibrary::stream_restart_required — when true, set_override_parameters already stop/start pipeline. */
bool ml_stream_restart_required(const config_profile_t &previous_profile, const config_profile_t &new_profile)
{
    bool restart_required = false;

    for (const auto &resolution : previous_profile.application_settings.application_input_streams.resolutions)
    {
        if (std::find_if(new_profile.application_settings.application_input_streams.resolutions.begin(),
                         new_profile.application_settings.application_input_streams.resolutions.end(),
                         [&resolution](const auto &res) {
                             return resolution.dimensions_and_aspect_ratio_equal(res);
                         }) == new_profile.application_settings.application_input_streams.resolutions.end())
        {
            restart_required = true;
            break;
        }
    }
    // Framerate changes require restart — DSP buffer pools are sized per-fps
    // (e.g. pool3840x2160_30_y). Without restart the old pool overflows.
    if (!restart_required)
    {
        const auto &prev_res = previous_profile.application_settings.application_input_streams.resolutions;
        const auto &new_res = new_profile.application_settings.application_input_streams.resolutions;
        for (const auto &pr : prev_res)
        {
            for (const auto &nr : new_res)
            {
                if (pr.stream_id == nr.stream_id && pr.framerate != nr.framerate)
                {
                    restart_required = true;
                    break;
                }
            }
            if (restart_required) break;
        }
    }
    restart_required |= previous_profile.application_settings.rotation.effective_value() !=
                        new_profile.application_settings.rotation.effective_value();

    auto prev_encoder_map = previous_profile.to_encoder_config_map();
    auto new_encoder_map = new_profile.to_encoder_config_map();

    for (const auto &entry : new_encoder_map)
    {
        const auto &stream_id = entry.first;
        auto pit = prev_encoder_map.find(stream_id);
        if (pit != prev_encoder_map.end())
        {
            EncoderType prev_type = get_encoder_type_from_cfg(pit->second);
            EncoderType new_type = get_encoder_type_from_cfg(entry.second);
            if (prev_type != new_type)
            {
                restart_required = true;
                break;
            }
        }
    }
    return restart_required;
}

/**
 * Encoder input dimensions are a VCEnc preprocessing/buffer-layout property.
 * MediaLibrary's normal restart predicate does not treat a Hailo encoder
 * width/height change as a full restart, so set_override_parameters() attempts
 * update_stride() on the live encoder. VCEnc intermittently rejects that path
 * (error -3) and can continue consuming buffers with the old layout.
 *
 * Mitigation: apply_profile_override_and_refresh() (and reconfigure_pipeline())
 * now treat any encoder input layout change as requiring full ML reinit, so the
 * unreliable live update_stride path is bypassed for geometry changes.
 */
bool encoder_input_layout_changed(const config_profile_t &previous_profile,
                                  const config_profile_t &new_profile)
{
    for (const auto &entry : previous_profile.encoded_output_streams)
    {
        const auto next = new_profile.encoded_output_streams.find(entry.first);
        if (next == new_profile.encoded_output_streams.end())
        {
            continue;
        }

        const auto previous_shape = std::visit(
            [](const auto &enc) {
                return std::make_tuple(enc.input_stream.width,
                                       enc.input_stream.height,
                                       enc.input_stream.framerate);
            },
            entry.second.encoding);
        const auto new_shape = std::visit(
            [](const auto &enc) {
                return std::make_tuple(enc.input_stream.width,
                                       enc.input_stream.height,
                                       enc.input_stream.framerate);
            },
            next->second.encoding);

        if (previous_shape != new_shape)
        {
            const auto &[old_width, old_height, old_fps] = previous_shape;
            const auto &[new_width, new_height, new_fps] = new_shape;
            HAL_LOG_INFO("hailo15_media: encoder '%s' input layout changed "
                         "%ux%u@%u -> %ux%u@%u; full reinit required",
                         entry.first.c_str(), old_width, old_height, old_fps,
                         new_width, new_height, new_fps);
            return true;
        }
    }
    return false;
}

int connect_encoders(Hailo15MediaPriv *p);
int connect_frontend(Hailo15MediaPriv *p);
void disconnect_ml_bridge_callbacks(Hailo15MediaPriv *p);

void clear_encoder_privacy_masks(config_profile_t &p)
{
    for (auto &kv : p.encoded_output_streams)
    {
        privacy_mask_config_t &m = kv.second.masking;
        static_privacy_mask_config_t sp{};
        sp.enabled = false;
        sp.masks.clear();
        m.static_privacy_mask_config = sp;
        m.dynamic_privacy_mask_config = std::nullopt;
        m.mask_type = PrivacyMaskType::PIXELIZATION;
        m.pixelization_size = 60;
        m.color_value = rgb_color_t{0, 0, 0};
    }
}

void apply_hal_privacy_to_profile(config_profile_t &p, Hailo15MediaPriv *priv, const HalMediaImageConfig *cfg)
{
    p.application_settings.digital_zoom.enabled = false;
    p.application_settings.digital_zoom.mode = DIGITAL_ZOOM_MODE_MAGNIFICATION;
    p.application_settings.digital_zoom.magnification = 1.0F;

    const HalPrivacyMaskConfig *pm = &cfg->privacy_mask_config;
    const HalRotationAngle rot = cfg->rotation_angle;
    const HalFlipDirection flip = cfg->flip_direction;

    auto is_portrait = [](HalRotationAngle a) {
        return (a == HAL_ROTATION_ANGLE_90 || a == HAL_ROTATION_ANGLE_270);
    };

    /* Align with webserver privacy_mask behaviour:
     * - Incoming points are in the *current displayed* frame (after rotation/flip).
     * - Webserver stores an "original" unrotated/unflipped polygon by reversing the transform.
     * - When rotation/flip changes, it re-applies the new transform so the mask follows the scene. */
    struct NormPt
    {
        float x;
        float y;
    };
    auto flip_rotate_norm = [&](float x, float y) -> NormPt {
        float rx = 0.0f, ry = 0.0f;
        switch (rot)
        {
            case HAL_ROTATION_ANGLE_0:   rx = x;         ry = y;         break;
            case HAL_ROTATION_ANGLE_90:  rx = 1.0f - y;  ry = x;         break;
            case HAL_ROTATION_ANGLE_180: rx = 1.0f - x;  ry = 1.0f - y;  break;
            case HAL_ROTATION_ANGLE_270: rx = y;         ry = 1.0f - x;  break;
            default:                    rx = x;         ry = y;         break;
        }
        float fx = 0.0f, fy = 0.0f;
        switch (flip)
        {
            case HAL_FLIP_DIRECTION_NONE:       fx = rx;        fy = ry;        break;
            case HAL_FLIP_DIRECTION_HORIZONTAL: fx = 1.0f - rx; fy = ry;        break;
            case HAL_FLIP_DIRECTION_VERTICAL:   fx = rx;        fy = 1.0f - ry; break;
            case HAL_FLIP_DIRECTION_BOTH:       fx = 1.0f - rx; fy = 1.0f - ry; break;
            default:                            fx = rx;        fy = ry;        break;
        }
        return NormPt{fx, fy};
    };

    auto reverse_flip_rotate_norm = [&](float x, float y) -> NormPt {
        /* Reverse order: undo flip first, then undo rotation (matches webserver reverse_flip_rotate_point). */
        float ux = 0.0f, uy = 0.0f;
        switch (flip)
        {
            case HAL_FLIP_DIRECTION_NONE:       ux = x;         uy = y;         break;
            case HAL_FLIP_DIRECTION_HORIZONTAL: ux = 1.0f - x;  uy = y;         break;
            case HAL_FLIP_DIRECTION_VERTICAL:   ux = x;         uy = 1.0f - y;  break;
            case HAL_FLIP_DIRECTION_BOTH:       ux = 1.0f - x;  uy = 1.0f - y;  break;
            default:                            ux = x;         uy = y;         break;
        }
        float ox = 0.0f, oy = 0.0f;
        switch (rot)
        {
            case HAL_ROTATION_ANGLE_0:   ox = ux;          oy = uy;          break;
            case HAL_ROTATION_ANGLE_90:  ox = uy;          oy = 1.0f - ux;   break;
            case HAL_ROTATION_ANGLE_180: ox = 1.0f - ux;   oy = 1.0f - uy;   break;
            case HAL_ROTATION_ANGLE_270: ox = 1.0f - uy;   oy = ux;          break;
            default:                    ox = ux;          oy = uy;          break;
        }
        return NormPt{ox, oy};
    };

    if (priv && pm->items != nullptr && pm->item_count > 0U)
    {
        /* Webserver keeps an "original" polygon and only refreshes it when the user updates the polygon.
         * When rotation/flip changes, it re-applies the transform on the stored original.
         *
         * In our CLI, pm->items points are *not* auto-updated on rotation changes, so we must NOT
         * overwrite originals just because dynamic_change_image_config() was called for rotation/flip.
         * We detect real user updates by comparing to the last display-space polygon we saw. */
        for (uint32_t ii = 0; ii < pm->item_count; ++ii)
        {
            const HalPrivacyMaskItem &it = pm->items[ii];
            if (!it.is_enabled)
            {
                continue;
            }
            const char *sid = (it.id && it.id[0] != '\0') ? it.id : "hal_privacy_mask";
            const std::string id(sid);

            std::vector<std::pair<float, float>> display_pts;
            display_pts.reserve(8);
            for (int pi = 0; pi < 8; ++pi)
            {
                if (it.points[pi].x < 0.0f)
                {
                    break;
                }
                display_pts.emplace_back(std::clamp(it.points[pi].x, 0.0F, 1.0F),
                                         std::clamp(it.points[pi].y, 0.0F, 1.0F));
            }
            if (display_pts.size() < 3U)
            {
                continue;
            }

            bool changed = true;
            auto last_it = priv->privacy_mask_last_display.find(id);
            if (last_it != priv->privacy_mask_last_display.end() && last_it->second == display_pts)
            {
                changed = false;
            }
            if (changed)
            {
                std::vector<std::pair<float, float>> orig_pts;
                orig_pts.reserve(display_pts.size());
                for (const auto &dp : display_pts)
                {
                    NormPt o = reverse_flip_rotate_norm(dp.first, dp.second);
                    orig_pts.emplace_back(std::clamp(o.x, 0.0F, 1.0F), std::clamp(o.y, 0.0F, 1.0F));
                }
                priv->privacy_mask_original[id] = std::move(orig_pts);
                priv->privacy_mask_last_display[id] = std::move(display_pts);
            }
        }
    }

    for (auto &kv : p.encoded_output_streams)
    {
        /* Webserver uses the frontend input resolution as the base geometry, then swaps on portrait.
         * Using encoder->input_stream here is wrong when rotation is enabled, because encoder geometry
         * may already be swapped (e.g. 2160x3840) and we'd effectively "double swap". */
        uint32_t in_w = 0;
        uint32_t in_h = 0;
        for (const auto &res : p.application_settings.application_input_streams.resolutions)
        {
            if (res.stream_id == kv.first)
            {
                in_w = res.dimensions.destination_width;
                in_h = res.dimensions.destination_height;
                break;
            }
        }
        if (in_w == 0 || in_h == 0)
        {
            /* Fallback: encoder config (best-effort). */
            std::visit(
                [&](auto &&enc) {
                    in_w = enc.input_stream.width;
                    in_h = enc.input_stream.height;
                },
                kv.second.encoding);
        }

        if (in_w == 0U)
        {
            in_w = 1;
        }
        if (in_h == 0U)
        {
            in_h = 1;
        }

        /* Privacy mask vertices are applied to the *effective* output frame.
         * On portrait rotations, width/height swap. */
        const uint32_t eff_w = is_portrait(rot) ? in_h : in_w;
        const uint32_t eff_h = is_portrait(rot) ? in_w : in_h;

        privacy_mask_config_t m{};
        if (pm->blur_radius <= 0)
        {
            m.mask_type = PrivacyMaskType::COLOR;
            m.pixelization_size = 60;
        }
        else
        {
            m.mask_type = PrivacyMaskType::PIXELIZATION;
            int sz = std::clamp(pm->blur_radius, 2, 64);
            if (sz % 2)
            {
                sz--;
            }
            if (sz < 2)
            {
                sz = 2;
            }
            m.pixelization_size = static_cast<uint32_t>(sz);
        }
        m.color_value.r = static_cast<unsigned>(pm->color.r);
        m.color_value.g = static_cast<unsigned>(pm->color.g);
        m.color_value.b = static_cast<unsigned>(pm->color.b);

        static_privacy_mask_config_t sm{};
        sm.enabled = false;
        sm.masks.clear();

        if (pm->items != nullptr && pm->item_count > 0U)
        {
            for (uint32_t ii = 0; ii < pm->item_count; ++ii)
            {
                const HalPrivacyMaskItem &it = pm->items[ii];
                if (!it.is_enabled)
                {
                    continue;
                }
                polygon poly;
                const char *sid = (it.id && it.id[0] != '\0') ? it.id : "hal_privacy_mask";
                poly.id = sid;
                /* Use stored original polygon if available, otherwise treat incoming points as original. */
                std::vector<std::pair<float, float>> orig_pts;
                if (priv)
                {
                    auto it0 = priv->privacy_mask_original.find(std::string(sid));
                    if (it0 != priv->privacy_mask_original.end())
                    {
                        orig_pts = it0->second;
                    }
                }
                if (orig_pts.empty())
                {
                    for (int pi = 0; pi < 8; ++pi)
                    {
                        if (it.points[pi].x < 0.0f)
                        {
                            break;
                        }
                        orig_pts.emplace_back(std::clamp(it.points[pi].x, 0.0F, 1.0F),
                                              std::clamp(it.points[pi].y, 0.0F, 1.0F));
                    }
                }

                for (const auto &op : orig_pts)
                {
                    NormPt n = flip_rotate_norm(op.first, op.second);
                    const float nx = std::clamp(n.x, 0.0F, 1.0F);
                    const float ny = std::clamp(n.y, 0.0F, 1.0F);

                    const int max_x = static_cast<int>(eff_w) - 1;
                    const int max_y = static_cast<int>(eff_h) - 1;
                    int ix = static_cast<int>(std::lround(nx * static_cast<float>(std::max(0, max_x))));
                    int iy = static_cast<int>(std::lround(ny * static_cast<float>(std::max(0, max_y))));
                    ix = std::clamp(ix, 0, std::max(0, max_x));
                    iy = std::clamp(iy, 0, std::max(0, max_y));
                    poly.vertices.push_back(vertex{ix, iy});
                }
                if (poly.vertices.size() >= 3U)
                {
                    sm.masks.push_back(std::move(poly));
                }
            }
            sm.enabled = !sm.masks.empty();
        }

        m.static_privacy_mask_config = sm;
        if (pm->dynamic_enabled)
        {
            /* Build the dynamic config: enabled + masked_labels + dilation + label_to_class_id.
             * label_to_class_id is required by the segmentation path (the blender drops a semantic
             * mask whose class_id doesn't match the mapped id); the detection path ignores it.
             * The blender's dynamic path uses the global mask_type/pixelization_size set above. */
            dynamic_privacy_mask_config_t dm{};
            dm.enabled = true;
            dm.dilation_size = static_cast<size_t>(pm->dilation_size);
            for (uint32_t li = 0; li < pm->num_masked_labels && li < HAL_PM_MAX_LABELS; ++li)
            {
                if (pm->masked_labels[li][0] != '\0')
                {
                    dm.masked_labels.emplace_back(pm->masked_labels[li]);
                    label_t lab{};
                    lab.label = pm->masked_labels[li];
                    lab.id = static_cast<uint32_t>(std::max(0, pm->masked_label_class_ids[li]));
                    dm.label_to_class_id.push_back(lab);
                }
            }
            m.dynamic_privacy_mask_config = dm;
        }
        else
        {
            m.dynamic_privacy_mask_config = std::nullopt;
        }
        kv.second.masking = m;
    }
}


void refresh_ids_from_profile(Hailo15MediaPriv *p, const config_profile_t &prof)
{
    p->frontend_stream_ids.clear();
    for (const auto &res : prof.application_settings.application_input_streams.resolutions)
    {
        if (!res.stream_id.empty())
        {
            p->frontend_stream_ids.push_back(res.stream_id);
        }
    }
    p->encoder_stream_ids.clear();
    for (const auto &kv : prof.encoded_output_streams)
    {
        p->encoder_stream_ids.push_back(kv.first);
    }
}

int build_contexts(Hailo15MediaPriv *priv, HalMediaContext *hm)
{
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    const config_profile_t &prof = prof_exp.value();
    refresh_ids_from_profile(priv, prof);

    free(hm->video_ctx_list);
    free(hm->codec_ctx_list);
    hm->video_ctx_list = nullptr;
    hm->codec_ctx_list = nullptr;
    priv->video_by_stream.clear();
    priv->codec_by_stream.clear();

    hm->video_ctx_list_count = static_cast<uint32_t>(priv->frontend_stream_ids.size());
    hm->codec_ctx_list_count = static_cast<uint32_t>(priv->encoder_stream_ids.size());

    if (hm->video_ctx_list_count > 0)
    {
        hm->video_ctx_list =
            static_cast<void **>(calloc(hm->video_ctx_list_count, sizeof(void *)));
        if (!hm->video_ctx_list)
        {
            return HAL_ERR_NO_MEM;
        }
    }
    if (hm->codec_ctx_list_count > 0)
    {
        hm->codec_ctx_list =
            static_cast<void **>(calloc(hm->codec_ctx_list_count, sizeof(void *)));
        if (!hm->codec_ctx_list)
        {
            return HAL_ERR_NO_MEM;
        }
    }

    for (uint32_t i = 0; i < hm->video_ctx_list_count; i++)
    {
        const std::string &sid = priv->frontend_stream_ids[i];
        auto *vc = static_cast<HalVideoContext *>(calloc(1, sizeof(HalVideoContext)));
        if (!vc)
        {
            return HAL_ERR_NO_MEM;
        }
        vc->config.type = HAL_VIDEO_TYPE_FROM_MEDIA;
        vc->config.path = nullptr;
        vc->config.media_ptr = hm;
        vc->config.priv = nullptr;
        vc->video_fd = -1;
        vc->status = HAL_STATUS_INITIALIZED;
        std::strncpy(vc->video_name, sid.c_str(), sizeof(vc->video_name) - 1);

        for (const auto &res : prof.application_settings.application_input_streams.resolutions)
        {
            if (res.stream_id == sid)
            {
                vc->config.width = res.dimensions.destination_width;
                vc->config.height = res.dimensions.destination_height;
                vc->config.framerate = res.framerate;
                vc->config.format = hailo_format_to_hal(prof.application_settings.application_input_streams.format);
                vc->config.pool_max_buffers = res.pool_max_buffers;
                break;
            }
        }

        hm->video_ctx_list[i] = vc;
        priv->video_by_stream[sid] = vc;
    }

    for (uint32_t i = 0; i < hm->codec_ctx_list_count; i++)
    {
        const std::string &eid = priv->encoder_stream_ids[i];
        auto *cc = static_cast<HalCodecContext *>(calloc(1, sizeof(HalCodecContext)));
        if (!cc)
        {
            return HAL_ERR_NO_MEM;
        }
        auto it = prof.encoded_output_streams.find(eid);
        if (it != prof.encoded_output_streams.end())
        {
            hailo15::ml::fill_hal_codec_config(cc, eid, it->second, hm);
        }
        hm->codec_ctx_list[i] = cc;
        priv->codec_by_stream[eid] = cc;
    }

    return HAL_OK;
}

void destroy_contexts(Hailo15MediaPriv *priv, HalMediaContext *hm)
{
    if (hm->video_ctx_list)
    {
        for (uint32_t i = 0; i < hm->video_ctx_list_count; i++)
        {
            std::free(hm->video_ctx_list[i]);
        }
        std::free(hm->video_ctx_list);
        hm->video_ctx_list = nullptr;
        hm->video_ctx_list_count = 0;
    }
    if (hm->codec_ctx_list)
    {
        for (uint32_t i = 0; i < hm->codec_ctx_list_count; i++)
        {
            std::free(hm->codec_ctx_list[i]);
        }
        std::free(hm->codec_ctx_list);
        hm->codec_ctx_list = nullptr;
        hm->codec_ctx_list_count = 0;
    }
    priv->video_by_stream.clear();
    priv->codec_by_stream.clear();
}

void refresh_osd_layout_from_profile(Hailo15MediaPriv *priv, const config_profile_t &prof)
{
    if (!priv)
    {
        return;
    }
    priv->osd_layout_by_encoder.clear();
    const HalRotationAngle rot = prof.application_settings.rotation.enabled
                                     ? static_cast<HalRotationAngle>(prof.application_settings.rotation.angle)
                                     : HAL_ROTATION_ANGLE_0;
    for (const auto &kv : prof.encoded_output_streams)
    {
        uint32_t ew = 0;
        uint32_t eh = 0;
        std::visit([&](const auto &enc) { ew = enc.input_stream.width; eh = enc.input_stream.height; }, kv.second.encoding);
        priv->osd_layout_by_encoder[kv.first] = OsdLayoutState{ew, eh, static_cast<int>(rot)};
    }
}

int apply_profile_override_and_refresh(HalMediaContext *hm, Hailo15MediaPriv *priv, const config_profile_t &target_profile,
                                       const char *tag)
{
    if (!hm || !priv || !priv->media_lib)
    {
        return HAL_ERR_INVALID_ARG;
    }

    const bool had_bridge = priv->callbacks_registered;
    std::optional<config_profile_t> prev_prof;
    {
        auto prev_exp = priv->media_lib->get_current_profile();
        if (prev_exp.has_value())
        {
            prev_prof = std::move(prev_exp.value());
        }
    }

    // Defense-in-depth against the fast-path false-success bug: the live
    // set_override_parameters() path applies encoder input geometry changes
    // via VCEnc update_stride() on the *running* encoder. Hailo's medialib
    // treats a VCEnc rejection (error -3, e.g. a height not aligned to 8) as
    // non-fatal — it logs the failure but keeps consuming buffers with the OLD
    // layout while still reporting MEDIA_LIBRARY_SUCCESS. The encoder is then
    // silently stuck on stale dimensions (observed on 93.72: stream labelled
    // 960x540 while emitting ~921600-byte = 1280x720 packets, ~5 SLOW-dispatch
    // warnings/sec, 78% CPU). There is no reliable post-override readback of
    // the encoder's *actual* hardware geometry (m_encoders exposes config
    // bookkeeping, not VCEnc truth), so we cannot verify after the fact.
    // Instead, route every encoder input layout change (width/height/framerate)
    // through full MediaLibrary destroy+recreate, where the new geometry is
    // applied at encoder creation and a VCEnc rejection fails start_pipeline()
    // and propagates to the caller. This covers all callers of this shared
    // helper; reconfigure_pipeline() also pre-gates (see ~line 5392).
    if (prev_prof.has_value() && encoder_input_layout_changed(prev_prof.value(), target_profile))
    {
        HAL_LOG_INFO("hailo15_media: %s: encoder input layout changed; routing to full ML reinit "
                     "(live update_stride is unreliable for geometry changes)",
                     tag ? tag : "profile_update");
        if (had_bridge)
        {
            disconnect_ml_bridge_callbacks(priv);
        }
        return reinit_media_library_on_layout_change(hm, priv, target_profile, tag);
    }

    bool disconnect_before = false;
    if (had_bridge)
    {
        if (prev_prof.has_value())
        {
            disconnect_before = profile_stream_layout_differs(prev_prof.value(), target_profile);
        }
        else
        {
            disconnect_before = true;
        }
    }

    const bool pipeline_running = priv->media_lib->get_pipeline_state() == media_library_pipeline_state_t::PIPELINE_STATE_RUNNING;
    const bool adding_stream = disconnect_before && prev_prof.has_value() &&
        target_profile.encoded_output_streams.size() > prev_prof.value().encoded_output_streams.size();

    // When ADDING streams to a running pipeline, do a full ML destroy+recreate instead
    // of in-place set_override_parameters.  The multi_resize DSP pipeline does not
    // properly handle dynamic addition of output streams — pads get created but the
    // DSP doesn't produce output for the new stream after a remove→add cycle.
    if (adding_stream && pipeline_running)
    {
        // Disconnect bridges before destroying ML (reinit will destroy the ML object).
        if (disconnect_before)
        {
            disconnect_ml_bridge_callbacks(priv);
            HAL_LOG_INFO("hailo15_media: %s: unsubscribed HAL bridges before full ML reinit",
                         tag ? tag : "profile_update");
        }
        HAL_LOG_INFO("hailo15_media: %s: adding stream to running pipeline, full ML reinit",
                     tag ? tag : "profile_update");
        return reinit_media_library_on_layout_change(hm, priv, target_profile, tag);
    }

    // For stream REMOVAL: stop the pipeline BEFORE disconnecting bridges.
    // If we disconnect first while the pipeline is still running, the GStreamer
    // streaming thread may still be in on_new_sample → HAL bridge lambda, leading
    // to SIGSEGV when it accesses the now-destroyed callback data.
    const bool pre_stop = disconnect_before && pipeline_running && !adding_stream;
    if (pre_stop)
    {
        HAL_LOG_INFO("hailo15_media: %s: pre-stopping pipeline before disconnect + set_override_parameters",
                     tag ? tag : "profile_update");
        priv->media_lib->stop_pipeline();
    }
    if (disconnect_before)
    {
        disconnect_ml_bridge_callbacks(priv);
        HAL_LOG_INFO("hailo15_media: %s: unsubscribed HAL bridges (layout differs)",
                     tag ? tag : "profile_update");
    }

    auto sop_t0 = std::chrono::steady_clock::now();
    media_library_return r = priv->media_lib->set_override_parameters(target_profile);
    auto sop_t1 = std::chrono::steady_clock::now();
    HAL_LOG_INFO("[TIMING] apply_profile_override_and_refresh(%s) set_override_parameters=%lldms",
                 tag ? tag : "profile_update",
                 static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(sop_t1 - sop_t0).count()));
    if (r != MEDIA_LIBRARY_SUCCESS)
    {
        HAL_LOG_WARNING("hailo15_media: %s: set_override_parameters failed (%d)", tag ? tag : "profile_update",
                        static_cast<int>(r));
        if (pre_stop && pipeline_running)
        {
            priv->media_lib->start_pipeline();
        }
        if (disconnect_before && had_bridge)
        {
            const int ce = connect_encoders(priv);
            const int cf = connect_frontend(priv);
            if (ce == HAL_OK && cf == HAL_OK)
            {
                std::lock_guard<std::recursive_mutex> lock(priv->mutex);
                priv->callbacks_registered = true;
            }
            HAL_LOG_WARNING("hailo15_media: %s: reconnected bridges after set_override_parameters failure",
                            tag ? tag : "profile_update");
        }
        return hailo15_ml_err(r);
    }

    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    const config_profile_t &prof = prof_exp.value();
    const bool ml_did_full_restart =
        prev_prof.has_value() && ml_stream_restart_required(prev_prof.value(), prof);
    // HAL must do stop/start when layout changed and ML did NOT already restart.
    // When ML did a full restart (resolution/rotation/encoder type change), HAL only
    // reconnects bridges — an extra stop/start would be redundant and can deadlock.
    const bool hal_recycle_stop_start = disconnect_before && !ml_did_full_restart;

    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        refresh_ids_from_profile(priv, prof);

        const bool same_layout = (priv->frontend_stream_ids == priv->last_frontend_stream_ids) &&
                                 (priv->encoder_stream_ids == priv->last_encoder_stream_ids);

        if (same_layout)
        {
            for (uint32_t i = 0; i < hm->video_ctx_list_count; i++)
            {
                auto *vc = static_cast<HalVideoContext *>(hm->video_ctx_list[i]);
                hailo15::video_ml::apply_profile_to_video_ctx(vc, prof, vc->video_name);
            }
            for (uint32_t i = 0; i < hm->codec_ctx_list_count; i++)
            {
                auto *cc = static_cast<HalCodecContext *>(hm->codec_ctx_list[i]);
                hailo15::video_ml::apply_profile_to_codec_ctx(cc, prof);
            }
        }
        else
        {
            destroy_contexts(priv, hm);
            const int br = build_contexts(priv, hm);
            if (br != HAL_OK)
            {
                return br;
            }
        }

        refresh_osd_layout_from_profile(priv, prof);
        priv->last_frontend_stream_ids = priv->frontend_stream_ids;
        priv->last_encoder_stream_ids = priv->encoder_stream_ids;
    }

    if (had_bridge)
    {
        disconnect_ml_bridge_callbacks(priv);
        {
            std::lock_guard<std::recursive_mutex> lock(priv->mutex);
            priv->callbacks_registered = false;
        }

        if (hal_recycle_stop_start)
        {
            HAL_LOG_INFO("hailo15_media: %s: HAL recycle after layout change (stop/start)", tag ? tag : "profile_update");
            media_library_return sr = priv->media_lib->stop_pipeline();
            if (sr != MEDIA_LIBRARY_SUCCESS)
            {
                HAL_LOG_WARNING("hailo15_media: %s: stop_pipeline failed (%d)", tag ? tag : "profile_update",
                                static_cast<int>(sr));
                const int ce = connect_encoders(priv);
                if (ce != HAL_OK)
                {
                    return ce;
                }
                const int cf = connect_frontend(priv);
                if (cf != HAL_OK)
                {
                    return cf;
                }
                {
                    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
                    priv->callbacks_registered = true;
                }
                return hailo15_ml_err(sr);
            }
        }

        const int ce = connect_encoders(priv);
        if (ce != HAL_OK)
        {
            return ce;
        }
        const int cf = connect_frontend(priv);
        if (cf != HAL_OK)
        {
            return cf;
        }
        {
            std::lock_guard<std::recursive_mutex> lock(priv->mutex);
            priv->callbacks_registered = true;
        }

        if (hal_recycle_stop_start)
        {
            media_library_return st = priv->media_lib->start_pipeline();
            if (st != MEDIA_LIBRARY_SUCCESS)
            {
                HAL_LOG_WARNING("hailo15_media: %s: start_pipeline failed (%d)", tag ? tag : "profile_update",
                                static_cast<int>(st));
                return hailo15_ml_err(st);
            }
        }
    }

    {
        /* Do not hold priv->mutex across MediaLibrary calls. */
        const media_library_pipeline_state_t pst = priv->media_lib->get_pipeline_state();
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->pipeline_started = (pst == media_library_pipeline_state_t::PIPELINE_STATE_RUNNING);
        hm->status = priv->pipeline_started ? HAL_STATUS_RUNNING : HAL_STATUS_STOPPED;
    }
    hailo15::video_ml::refresh_all_context_configs(priv, hm);
    return HAL_OK;
}

/**
 * Drop HAL bridge subscriptions from Media Library before set_profile() changes frontend outputs.
 * ML refuses to remove an output stream id while MediaLibraryFrontend::m_callbacks still has that id
 * (see frontend.cpp verify_removed_outputs_not_subscribed). Encoder callbacks must also be cleared so
 * recreated encoder objects are not left with stale subscribers.
 */
void disconnect_ml_bridge_callbacks(Hailo15MediaPriv *p)
{
    if (!p || !p->media_lib)
    {
        return;
    }
    (void)p->media_lib->unsubscribe_all_from_frontend();
    for (auto &kv : p->media_lib->m_encoders)
    {
        if (kv.second)
        {
            (void)kv.second->unsubscribe();
        }
    }
}

int connect_encoders(Hailo15MediaPriv *p)
{
    /* MediaLibrary may recreate encoder objects on profile change; subscribe() appends callbacks.
     * Clear internal subscribers before re-arming HAL bridge lambdas (see media_library.cpp create_encoders). */
    for (const auto &eid : p->encoder_stream_ids)
    {
        (void)p->media_lib->unsubscribe_from_encoder_output(eid);
    }

    HAL_LOG_INFO("hailo15_media: connect_encoders: HAL encoder_stream_ids=%zu, ML m_encoders=%zu",
                 p->encoder_stream_ids.size(), p->media_lib->m_encoders.size());
    for (const auto &eid : p->encoder_stream_ids)
    {
        media_library_return s =
            p->media_lib->subscribe_to_encoder_output(eid, [p, eid](HailoMediaLibraryBufferPtr buf, uint32_t sz) {
                if (!buf)
                {
                    return;
                }
                const size_t packet_index = ++p->enc_pkt_count[eid];
                (void)buf->sync_start();
                HalCodecFrameCallback cb = nullptr;
                void *cb_ud = nullptr;
                HalCodecContext *cctx = nullptr;
                HalPacketType pt = HAL_PACKET_TYPE_H264;
                uint64_t seq = 0;
                {
                    /* Never hold priv->mutex across MediaLibrary calls or user callbacks: both may re-enter HAL. */
                    std::lock_guard<std::recursive_mutex> lock(p->mutex);
                    p->packet_seq++;
                    seq = p->packet_seq;
                    const auto cb_it = p->codec_packet_subscribers.find(eid);
                    if (cb_it != p->codec_packet_subscribers.end())
                    {
                        cb = cb_it->second.first;
                        cb_ud = cb_it->second.second;
                    }
                    const auto cc_it = p->codec_by_stream.find(eid);
                    if (cc_it != p->codec_by_stream.end())
                    {
                        cctx = cc_it->second;
                        pt = cctx ? cctx->config.packet_type : pt;
                    }
                }
                if (!cb)
                {
                    if (p->enc_pkt_count[eid] <= 5)
                    {
                        HAL_LOG_WARNING("hailo15_media: encoder output for '%s' has no packet subscriber, dropping",
                                        eid.c_str());
                    }
                    (void)buf->sync_end();
                    return;
                }
                HalPacketBuffer pkt{};
                hailo15_fill_packet_from_buffer(buf, &pkt, sz, pt);
                if (packet_index <= 3)
                {
                    HAL_LOG_INFO("hailo15_media: encoder output: '%s' pkt#%zu "
                                  "reported=%u payload=%u",
                                  eid.c_str(), packet_index, sz, pkt.size);
                }
                pkt.sequence = static_cast<uint32_t>(seq);
                cb(cctx, &pkt, cb_ud);
                (void)buf->sync_end();
            });
        if (s != MEDIA_LIBRARY_SUCCESS)
        {
            HAL_LOG_WARNING("hailo15_media: connect_encoders: subscribe_to_encoder_output('%s') failed: %d",
                            eid.c_str(), static_cast<int>(s));
            continue;
        }
    }
    return HAL_OK;
}

int connect_frontend(Hailo15MediaPriv *p)
{
    return hailo15_connect_media_priv_frontend(p);
}

/**
 * Apply explicit per-stream encoder dimension overrides from camera-daemon YAML config.
 * This replaces auto-detection from application_settings, ensuring ALL profiles (not
 * just default) get corrected encoder dimensions.  The YAML encoders section is the
 * NE503 project's authoritative configuration.
 */
static void apply_encoder_overrides(std::string &config_json, const char *overrides_json)
{
    if (!overrides_json || !overrides_json[0]) return;

    using json = nlohmann::json;
    json overrides;
    try { overrides = json::parse(overrides_json); }
    catch (...) { return; }
    if (!overrides.is_array() || overrides.empty()) return;

    struct StreamOverride {
        uint32_t width = 0, height = 0, fps = 0;
        std::string codec;
        uint32_t bitrate = 0, gop = 0;
    };
    std::map<std::string, StreamOverride> dim_map;
    for (const auto &ov : overrides)
    {
        std::string sid = ov.value("stream_id", "");
        StreamOverride so;
        so.width = ov.value("width", 0u);
        so.height = ov.value("height", 0u);
        so.fps = ov.value("framerate", 0u);
        so.codec = ov.value("codec", "");
        so.bitrate = ov.value("bitrate", 0u);
        so.gop = ov.value("gop", 0u);
        if (!sid.empty() && so.width > 0 && so.height > 0)
            dim_map[sid] = so;
    }
    if (dim_map.empty()) return;

    /* Helper: "h264" → "CODEC_TYPE_H264", "h265" → "CODEC_TYPE_HEVC" */
    auto codec_to_enum = [](const std::string &c) -> std::string {
        if (c == "h264" || c == "H264") return "CODEC_TYPE_H264";
        if (c == "h265" || c == "H265" || c == "hevc" || c == "HEVC") return "CODEC_TYPE_HEVC";
        return "";
    };

    /* Patch a single profile: fix dimensions of existing encoded_output_streams
     * and inject missing ones (e.g. sink2 when vendor profile only has sink0+sink1). */
    auto fix_profile = [&](json &profile) {
        if (!profile.contains("encoded_output_streams") ||
            !profile["encoded_output_streams"].is_array())
        {
            profile["encoded_output_streams"] = json::array();
        }

        /* --- Pass 0: remove stale/extra streams from profile ---
         * 1) Streams not in dim_map were disabled by config — remove them.
         * 2) Streams with stale /tmp/ encoding paths (dangling after reboot) — remove.
         * Both get re-injected with fresh config in Pass 2 if needed. */
        for (auto it = profile["encoded_output_streams"].begin();
             it != profile["encoded_output_streams"].end(); )
        {
            std::string sid = it->value("stream_id", "");
            // Remove streams not in the override list (disabled by config)
            if (!sid.empty() && dim_map.find(sid) == dim_map.end())
            {
                HAL_LOG_INFO("hailo15_media: encoder_override: removing stream '%s' "
                             "not present in encoder overrides", sid.c_str());
                it = profile["encoded_output_streams"].erase(it);
                continue;
            }
            // Remove stale /tmp/ encoding references
            std::string enc_path = it->value("encoding", "");
            if (!enc_path.empty() && enc_path.substr(0, 5) == "/tmp/")
            {
                std::ifstream test(enc_path);
                if (!test.is_open())
                {
                    HAL_LOG_INFO("hailo15_media: encoder_override: removing stale /tmp/ encoder "
                                 "reference for '%s' (path='%s' does not exist)",
                                 sid.c_str(), enc_path.c_str());
                    it = profile["encoded_output_streams"].erase(it);
                    continue;
                }
                test.close();
            }
            ++it;
        }

        /* --- Pass 1: patch existing streams --- */
        std::set<std::string> existing_streams;
        json *template_enc_cfg = nullptr;
        for (auto &eos : profile["encoded_output_streams"])
        {
            if (!eos.contains("stream_id")) continue;
            std::string sid = eos["stream_id"].get<std::string>();
            existing_streams.insert(sid);

            auto it = dim_map.find(sid);
            if (it == dim_map.end()) continue;
            auto &so = it->second;
            uint32_t exp_w = so.width, exp_h = so.height, exp_fps = so.fps;

            if (!eos.contains("encoding") || !eos["encoding"].is_string()) continue;
            std::string enc_path = eos["encoding"].get<std::string>();

            std::ifstream ef(enc_path);
            if (!ef.is_open()) continue;
            json enc_cfg;
            ef >> enc_cfg;
            ef.close();

            json *enc_root = &enc_cfg;
            if (enc_cfg.contains("encoding") && enc_cfg["encoding"].is_object())
                enc_root = &enc_cfg["encoding"];
            if (!enc_root->contains("input_stream")) continue;

            if (!template_enc_cfg) template_enc_cfg = new json(enc_cfg);

            auto &inp = (*enc_root)["input_stream"];
            uint32_t cur_w = inp.value("width", 0u);
            uint32_t cur_h = inp.value("height", 0u);
            uint32_t cur_fps = inp.value("framerate", 0u);

            /* Check if any override fields differ */
            bool dim_changed = (cur_w != exp_w || cur_h != exp_h || cur_fps != exp_fps);
            bool codec_changed = false, bitrate_changed = false, gop_changed = false;

            /* Check codec override */
            if (!so.codec.empty() && enc_root->contains("hailo_encoder")) {
                auto &he = (*enc_root)["hailo_encoder"];
                if (he.contains("config") && he["config"].contains("output_stream")) {
                    std::string cur_codec = he["config"]["output_stream"].value("codec", "");
                    std::string exp_codec = codec_to_enum(so.codec);
                    if (!exp_codec.empty() && cur_codec != exp_codec)
                        codec_changed = true;
                }
            }
            /* Check bitrate override */
            if (so.bitrate > 0 && enc_root->contains("hailo_encoder")) {
                auto &he = (*enc_root)["hailo_encoder"];
                if (he.contains("rate_control") && he["rate_control"].contains("bitrate")) {
                    uint32_t cur_br = he["rate_control"]["bitrate"].value("target_bitrate", 0u);
                    if (cur_br != so.bitrate) bitrate_changed = true;
                }
            }
            /* Check gop override */
            if (so.gop > 0 && enc_root->contains("hailo_encoder")) {
                auto &he = (*enc_root)["hailo_encoder"];
                if (he.contains("rate_control")) {
                    uint32_t cur_ipr = he["rate_control"].value("intra_pic_rate", 0u);
                    if (cur_ipr != so.gop) gop_changed = true;
                }
            }

            if (!dim_changed && !codec_changed && !bitrate_changed && !gop_changed) continue;

            HAL_LOG_INFO("hailo15_media: encoder_override: stream '%s' "
                         "(encoder=%ux%u@%u, override=%ux%u@%u, codec=%d br=%d gop=%d), patching",
                         sid.c_str(), cur_w, cur_h, cur_fps, exp_w, exp_h, exp_fps,
                         codec_changed, bitrate_changed, gop_changed);

            inp["width"] = exp_w;
            inp["height"] = exp_h;
            if (exp_fps > 0) inp["framerate"] = exp_fps;

            /* Apply codec/bitrate/gop overrides */
            if (enc_root->contains("hailo_encoder")) {
                auto &he = (*enc_root)["hailo_encoder"];
                if (codec_changed) {
                    he["config"]["output_stream"]["codec"] = codec_to_enum(so.codec);
                }
                if (bitrate_changed) {
                    he["rate_control"]["bitrate"]["target_bitrate"] = so.bitrate;
                }
                if (gop_changed) {
                    he["rate_control"]["intra_pic_rate"] = so.gop;
                }
            }

            if (enc_cfg.contains("metadata") && enc_cfg["metadata"].is_object())
            {
                auto enc_hash = compute_medialib_content_hash(enc_cfg);
                if (enc_hash) enc_cfg["metadata"]["content_hash"] = *enc_hash;
            }
            std::string tmp_path = std::string("/tmp/encoder_override_") + sid + ".json";
            std::ofstream of(tmp_path);
            if (of.is_open())
            {
                of << enc_cfg.dump(4);
                of.close();
                eos["encoding"] = tmp_path;
            }
        }

        /* --- Pass 2: inject missing streams from YAML overrides --- */
        for (const auto &[sid, so] : dim_map)
        {
            if (existing_streams.count(sid)) continue;
            uint32_t exp_w = so.width, exp_h = so.height, exp_fps = so.fps;

            /* Use the last seen encoder config as template, or create a minimal one */
            json new_enc;
            if (template_enc_cfg)
            {
                new_enc = *template_enc_cfg;
            }
            else
            {
                /* Minimal encoder config as fallback */
                new_enc = {
                    {"encoding", {
                        {"hailo_encoder", {
                            {"input_stream", {{"format", "NV12"}, {"framerate", 30}, {"height", 720}, {"width", 1280}}},
                            {"config", {{"output_stream", {{"codec", "CODEC_TYPE_H264"}}}}},
                            {"rate_control", {{"bitrate", {{"target_bitrate", 2000000}}}, {"intra_pic_rate", 30}}},
                            {"gop_config", {{"gop_size", 30}}}
                        }}
                    }}
                };
            }

            /* Patch template dimensions */
            json *enc_root = &new_enc;
            if (new_enc.contains("encoding") && new_enc["encoding"].is_object())
                enc_root = &new_enc["encoding"];
            if (!enc_root->contains("input_stream"))
                (*enc_root)["input_stream"] = json::object();
            auto &inp = (*enc_root)["input_stream"];
            inp["width"] = exp_w;
            inp["height"] = exp_h;
            if (exp_fps > 0) inp["framerate"] = exp_fps;

            /* Apply codec/bitrate/gop from YAML override */
            if (enc_root->contains("hailo_encoder")) {
                auto &he = (*enc_root)["hailo_encoder"];
                if (!so.codec.empty()) {
                    std::string codec_enum = codec_to_enum(so.codec);
                    if (!codec_enum.empty())
                        he["config"]["output_stream"]["codec"] = codec_enum;
                }
                if (so.bitrate > 0)
                    he["rate_control"]["bitrate"]["target_bitrate"] = so.bitrate;
                if (so.gop > 0)
                    he["rate_control"]["intra_pic_rate"] = so.gop;
            }

            if (new_enc.contains("metadata") && new_enc["metadata"].is_object())
            {
                auto enc_hash = compute_medialib_content_hash(new_enc);
                if (enc_hash) new_enc["metadata"]["content_hash"] = *enc_hash;
            }
            std::string tmp_path = std::string("/tmp/encoder_inject_") + sid + ".json";
            std::ofstream of(tmp_path);
            if (of.is_open())
            {
                of << new_enc.dump(4);
                of.close();

                json eos_entry;
                eos_entry["stream_id"] = sid;
                eos_entry["encoding"] = tmp_path;

                /* Resolve osd/masking files from the same profile directory.
                 * Look at an existing stream's paths to find the base directory,
                 * then check for osd_<sid>.json and masking_<sid>.json there. */
                for (const auto &existing : profile["encoded_output_streams"])
                {
                    if (!existing.contains("osd") || !existing["osd"].is_string()) continue;
                    std::string osd_path = existing["osd"].get<std::string>();
                    auto sep = osd_path.rfind('/');
                    if (sep == std::string::npos) continue;
                    std::string dir = osd_path.substr(0, sep + 1);

                    std::string osd_candidate = dir + "osd_" + sid + ".json";
                    std::ifstream test_osd(osd_candidate);
                    if (test_osd.is_open())
                    {
                        test_osd.close();
                        eos_entry["osd"] = osd_candidate;
                    }
                    else
                    {
                        /* Generate a minimal default OSD file if vendor didn't provide one */
                        std::string tmp_osd = std::string("/tmp/osd_default_") + sid + ".json";
                        json osd_data;
                        osd_data["version"] = "1.0.0";
                        osd_data["metadata"]["architecture"] = "hailo15h";
                        osd_data["osd"]["dateTime"] = json::array();
                        osd_data["osd"]["image"] = json::array();
                        osd_data["osd"]["text"] = json::array();
                        auto osd_hash = compute_medialib_content_hash(osd_data);
                        if (osd_hash) osd_data["metadata"]["content_hash"] = *osd_hash;
                        std::ofstream osd_of(tmp_osd);
                        if (osd_of.is_open())
                        {
                            osd_of << osd_data.dump(4);
                            osd_of.close();
                            eos_entry["osd"] = tmp_osd;
                        }
                    }

                    std::string mask_candidate = dir + "masking_" + sid + ".json";
                    std::ifstream test_mask(mask_candidate);
                    if (test_mask.is_open())
                    {
                        test_mask.close();
                        eos_entry["masking"] = mask_candidate;
                    }
                    else
                    {
                        /* Generate a minimal default masking file if vendor didn't provide one */
                        std::string tmp_mask = std::string("/tmp/masking_default_") + sid + ".json";
                        json mask_data;
                        mask_data["version"] = "3.0.0";
                        mask_data["metadata"]["architecture"] = "hailo15h";
                        mask_data["masking"]["color_value"] = {0, 0, 0};
                        mask_data["masking"]["mask_type"] = "PIXELIZATION";
                        mask_data["masking"]["pixelization_size"] = 60;
                        auto mask_hash = compute_medialib_content_hash(mask_data);
                        if (mask_hash) mask_data["metadata"]["content_hash"] = *mask_hash;
                        std::ofstream mask_of(tmp_mask);
                        if (mask_of.is_open())
                        {
                            mask_of << mask_data.dump(4);
                            mask_of.close();
                            eos_entry["masking"] = tmp_mask;
                        }
                    }
                    break;
                }

                profile["encoded_output_streams"].push_back(eos_entry);

                HAL_LOG_INFO("hailo15_media: encoder_override: injected missing stream '%s' "
                             "(%ux%u@%u) -> %s (osd=%s, masking=%s)",
                             sid.c_str(), exp_w, exp_h, exp_fps, tmp_path.c_str(),
                             eos_entry.value("osd", "none").c_str(),
                             eos_entry.value("masking", "none").c_str());
            }
        }

        delete template_enc_cfg;

        /* --- Pass 3: align application_settings resolutions with YAML overrides --- */
        json *app_obj = nullptr;
        json app_data;
        if (profile.contains("application_settings"))
        {
            if (profile["application_settings"].is_string())
            {
                std::string as_path = profile["application_settings"].get<std::string>();
                std::ifstream asf(as_path);
                if (asf.is_open())
                {
                    asf >> app_data;
                    asf.close();
                    app_obj = &app_data;
                }
            }
            else if (profile["application_settings"].is_object())
            {
                app_obj = &profile["application_settings"];
            }
        }
        if (app_obj && app_obj->contains("application_input_streams") &&
            (*app_obj)["application_input_streams"].contains("resolutions") &&
            (*app_obj)["application_input_streams"]["resolutions"].is_array())
        {
            // Remove resolution entries for streams not in dim_map (disabled/removed)
            auto &res_arr = (*app_obj)["application_input_streams"]["resolutions"];
            for (auto it = res_arr.begin(); it != res_arr.end(); )
            {
                std::string rid = it->value("stream_id", "");
                if (!rid.empty() && dim_map.find(rid) == dim_map.end())
                {
                    HAL_LOG_INFO("hailo15_media: encoder_override: removing resolution entry for '%s'", rid.c_str());
                    it = res_arr.erase(it);
                    continue;
                }
                ++it;
            }

            // Add resolution entries for injected streams (encoded_output_streams
            // may have new sinks from Pass 2 that don't exist in resolutions yet).
            // ML validates these arrays have matching sizes.
            bool app_changed = false;
            std::set<std::string> res_ids;
            uint32_t default_pool_buffers = 30; // from existing entries
            for (const auto &r : res_arr)
            {
                res_ids.insert(r.value("stream_id", ""));
                if (r.contains("pool_max_buffers"))
                    default_pool_buffers = r.value("pool_max_buffers", 30u);
            }
            for (const auto &eos : profile["encoded_output_streams"])
            {
                std::string sid = eos.value("stream_id", "");
                if (sid.empty() || res_ids.count(sid)) continue;
                auto dit = dim_map.find(sid);
                if (dit == dim_map.end()) continue;
                auto &dso = dit->second;
                HAL_LOG_INFO("hailo15_media: encoder_override: injecting resolution for '%s' (%ux%u@%u)",
                             sid.c_str(), dso.width, dso.height, dso.fps);
                res_arr.push_back({{"stream_id", sid},
                                   {"width", dso.width}, {"height", dso.height},
                                   {"framerate", dso.fps},
                                   {"pool_max_buffers", default_pool_buffers}});
                app_changed = true;
            }

            bool app_changed_dim = false;
            for (auto &r : (*app_obj)["application_input_streams"]["resolutions"])
            {
                std::string rid = r.value("stream_id", "");
                auto dit = dim_map.find(rid);
                if (dit == dim_map.end()) continue;
                auto &dso = dit->second;
                uint32_t rw = r.value("width", 0u);
                uint32_t rh = r.value("height", 0u);
                uint32_t rf = r.value("framerate", 0u);
                if (rw == dso.width && rh == dso.height && rf == dso.fps) continue;
                HAL_LOG_INFO("hailo15_media: encoder_override: app_settings stream '%s' "
                             "(%ux%u@%u -> %ux%u@%u)",
                             rid.c_str(), rw, rh, rf, dso.width, dso.height, dso.fps);
                r["width"] = dso.width;
                r["height"] = dso.height;
                if (dso.fps > 0) r["framerate"] = dso.fps;
                app_changed_dim = true;
            }
            if (app_changed_dim) app_changed = true;
            /* If application_settings was a file reference, write patched copy.
             * Use a hash of the original path to avoid collisions across profiles. */
            if (app_changed && profile["application_settings"].is_string())
            {
                if (app_data.contains("metadata") && app_data["metadata"].is_object())
                {
                    auto as_hash = compute_medialib_content_hash(app_data);
                    if (as_hash) app_data["metadata"]["content_hash"] = *as_hash;
                }
                std::string orig_path = profile["application_settings"].get<std::string>();
                size_t h = std::hash<std::string>{}(orig_path);
                std::string tmp_app = "/tmp/app_settings_" + std::to_string(h) + ".json";
                std::ofstream of(tmp_app);
                if (of.is_open())
                {
                    of << app_data.dump(4);
                    of.close();
                    profile["application_settings"] = tmp_app;
                }
            }
        }
    };

    json cfg;
    try { cfg = json::parse(config_json, nullptr, false); }
    catch (...) { return; }
    if (cfg.is_discarded()) return;

    if (cfg.contains("profiles") && cfg["profiles"].is_array())
    {
        for (auto &p : cfg["profiles"])
        {
            if (p.contains("config_file") && p["config_file"].is_string())
            {
                std::string prof_path = p["config_file"].get<std::string>();
                std::ifstream pf(prof_path);
                if (!pf.is_open()) continue;
                json prof_data;
                pf >> prof_data;
                pf.close();

                json orig = prof_data;
                fix_profile(prof_data);

                if (prof_data != orig)
                {
                    /* Remove content_hash — we cannot perfectly replicate medialib's
                     * hash algorithm, so omit it to skip validation on the override file. */
                    if (prof_data.contains("metadata") && prof_data["metadata"].is_object())
                    {
                        auto ph = compute_medialib_content_hash(prof_data);
                        if (ph) prof_data["metadata"]["content_hash"] = *ph;
                    }

                    std::string pname = p.value("name", "");
                    std::string tmp_prof = "/tmp/profile_override_" + pname + ".json";
                    std::ofstream of(tmp_prof);
                    if (of.is_open())
                    {
                        of << prof_data.dump(4);
                        of.close();
                        p["config_file"] = tmp_prof;
                    }
                }
            }
            else
            {
                fix_profile(p);
            }
        }
        /* Remove content_hash — cannot replicate medialib's hash algorithm */
        if (cfg.contains("metadata") && cfg["metadata"].is_object())
        {
            auto cfg_h = compute_medialib_content_hash(cfg);
            if (cfg_h) cfg["metadata"]["content_hash"] = *cfg_h;
        }
        config_json = cfg.dump(2);
    }
    else if (cfg.contains("encoded_output_streams"))
    {
        fix_profile(cfg);
        if (cfg.contains("metadata") && cfg["metadata"].is_object())
        {
            auto cfg_h = compute_medialib_content_hash(cfg);
            if (cfg_h) cfg["metadata"]["content_hash"] = *cfg_h;
        }
        config_json = cfg.dump(2);
    }
}

/**
 * Fix encoder dimension mismatches in the medialib config before initialize().
 *
 * Some vendor profiles (e.g. Daylight) have a stream whose encoder config
 * file dimensions differ from the application_settings resolution for the same
 * stream_id (e.g. sink2 encoder pointing to encoder_sink0.json = 1920x1080,
 * while application_settings says sink2 = 640x384@15).  This causes
 * gst_buffer_from_hailo_buffer to fail because the appsrc caps don't match
 * the actual buffer dimensions, and auto_feed add_buffer returns ERROR.
 *
 * This function scans the default profile, checks each encoded_output_stream
 * encoder config against the corresponding application_settings resolution,
 * and writes a corrected encoder config to /tmp/ when dimensions differ.
 * The in-memory JSON is updated to point to the corrected file.
 */
static void fix_encoder_dimension_mismatches(std::string &config_json)
{
    using json = nlohmann::json;

    json cfg;
    try { cfg = json::parse(config_json, nullptr, false); }
    catch (...) { return; }
    if (cfg.is_discarded()) return;

    /* Resolve the top-level config → active profile(s).
     * The config may be:
     *   - A top-level config with "profiles" array + "default_profile"
     *   - A profile directly (has "encoded_output_streams")
     *   - A profile with "config_file" reference
     */

    /* Helper: fix dimension mismatches in a single profile JSON (in-memory) */
    auto fix_profile = [&](json &profile) {
        /* Load application_settings to get expected resolutions */
        if (!profile.contains("application_settings")) return;
        json *app_obj = nullptr;
        json app_data;
        if (profile["application_settings"].is_string())
        {
            std::string as_path = profile["application_settings"].get<std::string>();
            std::ifstream asf(as_path);
            if (!asf.is_open()) return;
            asf >> app_data;
            asf.close();
            app_obj = &app_data;
        }
        else if (profile["application_settings"].is_object())
        {
            app_obj = &profile["application_settings"];
        }
        if (!app_obj) return;

        /* Build map: stream_id → {width, height, framerate} from application_settings */
        std::map<std::string, std::tuple<uint32_t, uint32_t, uint32_t>> expected_dims;
        if (app_obj->contains("application_input_streams") &&
            (*app_obj)["application_input_streams"].contains("resolutions") &&
            (*app_obj)["application_input_streams"]["resolutions"].is_array())
        {
            for (const auto &r : (*app_obj)["application_input_streams"]["resolutions"])
            {
                std::string sid = r.value("stream_id", "");
                uint32_t w = r.value("width", 0u);
                uint32_t h = r.value("height", 0u);
                uint32_t fps = r.value("framerate", 0u);
                if (!sid.empty() && w > 0 && h > 0)
                    expected_dims[sid] = {w, h, fps};
            }
        }
        if (expected_dims.empty()) return;

        /* Check each encoded_output_stream */
        if (!profile.contains("encoded_output_streams") ||
            !profile["encoded_output_streams"].is_array()) return;

        for (auto &eos : profile["encoded_output_streams"])
        {
            if (!eos.contains("stream_id") || !eos.contains("encoding")) continue;
            std::string sid = eos["stream_id"].get<std::string>();
            auto it = expected_dims.find(sid);
            if (it == expected_dims.end()) continue;

            auto [exp_w, exp_h, exp_fps] = it->second;

            /* Only handle file-path encoder references */
            if (!eos["encoding"].is_string()) continue;
            std::string enc_path = eos["encoding"].get<std::string>();

            /* Read encoder config */
            std::ifstream ef(enc_path);
            if (!ef.is_open()) continue;
            json enc_cfg;
            ef >> enc_cfg;
            ef.close();

            /* Navigate into "encoding" wrapper */
            json *enc_root = &enc_cfg;
            if (enc_cfg.contains("encoding") && enc_cfg["encoding"].is_object())
                enc_root = &enc_cfg["encoding"];

            if (!enc_root->contains("input_stream")) continue;
            auto &inp = (*enc_root)["input_stream"];
            uint32_t cur_w = inp.value("width", 0u);
            uint32_t cur_h = inp.value("height", 0u);
            uint32_t cur_fps = inp.value("framerate", 0u);

            if (cur_w == exp_w && cur_h == exp_h && cur_fps == exp_fps) continue;

            HAL_LOG_INFO("hailo15_media: fix_enc_dims: stream '%s' encoder mismatch "
                         "(encoder=%ux%u@%u, expected=%ux%u@%u), patching to /tmp/",
                         sid.c_str(), cur_w, cur_h, cur_fps, exp_w, exp_h, exp_fps);

            /* Patch dimensions */
            inp["width"] = exp_w;
            inp["height"] = exp_h;
            if (exp_fps > 0) inp["framerate"] = exp_fps;

            /* Update content_hash for the modified encoder config */
            if (enc_cfg.contains("metadata") && enc_cfg["metadata"].is_object())
            {
                auto enc_hash = compute_medialib_content_hash(enc_cfg);
                if (enc_hash) enc_cfg["metadata"]["content_hash"] = *enc_hash;
            }

            /* Write corrected config to /tmp/ */
            std::string tmp_path = std::string("/tmp/encoder_init_") + sid + ".json";
            std::ofstream of(tmp_path);
            if (of.is_open())
            {
                of << enc_cfg.dump(4);
                of.close();
                eos["encoding"] = tmp_path;
                HAL_LOG_INFO("hailo15_media: fix_enc_dims: wrote '%s' for stream '%s'",
                             tmp_path.c_str(), sid.c_str());
            }
            else
            {
                HAL_LOG_ERROR("hailo15_media: fix_enc_dims: cannot write '%s'", tmp_path.c_str());
            }
        }
    };

    /* Case 1: top-level config with profiles array */
    if (cfg.contains("profiles") && cfg["profiles"].is_array())
    {
        /* Determine which profiles to fix (all, or just the default) */
        std::string default_prof = cfg.value("default_profile", "");

        for (auto &p : cfg["profiles"])
        {
            if (!p.contains("name")) continue;
            std::string pname = p["name"].get<std::string>();

            /* Fix the default profile and any profile referenced inline */
            if (!default_prof.empty() && pname != default_prof) continue;

            if (p.contains("config_file") && p["config_file"].is_string())
            {
                /* Profile is in a separate file — load, fix, write back */
                std::string prof_path = p["config_file"].get<std::string>();
                std::ifstream pf(prof_path);
                if (!pf.is_open()) continue;
                json prof_data;
                pf >> prof_data;
                pf.close();

                json orig = prof_data; // compare after fix
                fix_profile(prof_data);

                if (prof_data != orig)
                {
                    /* Remove content_hash — cannot perfectly replicate medialib's hash algorithm */
                    if (prof_data.contains("metadata") && prof_data["metadata"].is_object())
                    {
                        auto ph = compute_medialib_content_hash(prof_data);
                        if (ph) prof_data["metadata"]["content_hash"] = *ph;
                    }

                    /* Write patched profile to /tmp/ and update reference */
                    std::string tmp_prof = std::string("/tmp/profile_init_") + pname + ".json";
                    std::ofstream of(tmp_prof);
                    if (of.is_open())
                    {
                        of << prof_data.dump(4);
                        of.close();
                        p["config_file"] = tmp_prof;
                        HAL_LOG_INFO("hailo15_media: fix_enc_dims: patched profile '%s' → '%s'",
                                     pname.c_str(), tmp_prof.c_str());
                    }
                }
            }
            else
            {
                /* Profile is inline — fix in-place */
                fix_profile(p);
            }
        }

        if (cfg.contains("metadata") && cfg["metadata"].is_object())
        {
            auto cfg_h = compute_medialib_content_hash(cfg);
            if (cfg_h) cfg["metadata"]["content_hash"] = *cfg_h;
        }
        config_json = cfg.dump(2);
    }
    else if (cfg.contains("encoded_output_streams"))
    {
        /* Case 2: config IS a profile (has encoded_output_streams directly) */
        fix_profile(cfg);
        if (cfg.contains("metadata") && cfg["metadata"].is_object())
        {
            auto cfg_h = compute_medialib_content_hash(cfg);
            if (cfg_h) cfg["metadata"]["content_hash"] = *cfg_h;
        }
        config_json = cfg.dump(2);
    }
}

/**
 * Patch stored_config_json so the active profile's application_input_streams.resolutions
 * and encoded_output_streams match the target profile layout.
 * The DSP multi_resize is configured during initialize() and cannot dynamically add/remove
 * output streams via set_override_parameters.  By patching the JSON before initialize(),
 * the DSP starts with the correct number of outputs from the beginning.
 */
static std::string patch_json_stream_layout(const std::string &stored_json,
                                             const config_profile_t &target_profile)
{
    using json = nlohmann::json;
    json cfg = json::parse(stored_json);

    std::string prof_name = target_profile.name;
    if (prof_name.empty() && cfg.contains("default_profile"))
        prof_name = cfg["default_profile"].get<std::string>();
    if (!prof_name.empty())
        cfg["default_profile"] = prof_name;

    /* Navigate to the active profile — may be inline, in profiles array, or in a file */
    auto *prof_entries = &cfg["profiles"];
    json *prof_entry = nullptr;
    if (prof_entries->is_array())
    {
        for (auto &p : *prof_entries)
            if (p.contains("name") && p["name"] == prof_name) { prof_entry = &p; break; }
    }
    else if (prof_entries->is_object())
    {
        if (prof_entries->contains(prof_name))
            prof_entry = &(*prof_entries)[prof_name];
    }
    if (!prof_entry)
    {
        HAL_LOG_ERROR("hailo15_media: patch_json: cannot find profile '%s'", prof_name.c_str());
        return {};
    }

    /* Resolve profile file reference */
    json profile_data;
    json *active_prof = prof_entry;
    if (prof_entry->contains("config_file") && (*prof_entry)["config_file"].is_string())
    {
        std::string prof_path = (*prof_entry)["config_file"].get<std::string>();
        std::ifstream pf(prof_path);
        if (pf.is_open())
        {
            pf >> profile_data;
            pf.close();
            active_prof = &profile_data;
        }
    }

    /* Resolve application_settings file reference */
    json app_data;
    json *app_obj = nullptr;
    if (active_prof->contains("application_settings"))
    {
        if ((*active_prof)["application_settings"].is_string())
        {
            std::string as_path = (*active_prof)["application_settings"].get<std::string>();
            std::ifstream asf(as_path);
            if (asf.is_open())
            {
                asf >> app_data;
                asf.close();
                app_obj = &app_data;
            }
        }
        else
        {
            app_obj = &(*active_prof)["application_settings"];
        }
    }
    if (!app_obj)
    {
        HAL_LOG_ERROR("hailo15_media: patch_json: cannot find application_settings");
        return {};
    }

    /* --- Patch application_input_streams.resolutions --- */
    if (app_obj->contains("application_input_streams") &&
        (*app_obj)["application_input_streams"].contains("resolutions"))
    {
        auto &res_arr = (*app_obj)["application_input_streams"]["resolutions"];
        if (res_arr.is_array())
        {
            /* Build set of stream_ids that must exist */
            std::unordered_set<std::string> target_ids;
            for (const auto &r : target_profile.application_settings.application_input_streams.resolutions)
                target_ids.insert(r.stream_id);

            /* Remove resolutions not in target */
            for (auto it = res_arr.begin(); it != res_arr.end(); )
            {
                if (it->contains("stream_id") && target_ids.count(it->at("stream_id").get<std::string>()) == 0)
                    it = res_arr.erase(it);
                else
                    ++it;
            }

            /* Add missing resolutions from target */
            std::unordered_set<std::string> existing_ids;
            for (const auto &r : res_arr)
                if (r.contains("stream_id"))
                    existing_ids.insert(r.at("stream_id").get<std::string>());

            for (const auto &tr : target_profile.application_settings.application_input_streams.resolutions)
            {
                if (existing_ids.count(tr.stream_id))
                    continue;
                json nr;
                nr["stream_id"] = tr.stream_id;
                nr["framerate"] = tr.framerate;
                nr["pool_max_buffers"] = tr.pool_max_buffers;
                nr["width"] = tr.dimensions.destination_width;
                nr["height"] = tr.dimensions.destination_height;
                res_arr.push_back(nr);
                HAL_LOG_INFO("hailo15_media: patch_json: added resolution for '%s' (%ux%u@%u)",
                             tr.stream_id.c_str(), tr.dimensions.destination_width,
                             tr.dimensions.destination_height, tr.framerate);
            }
        }
    }

    /* --- Patch encoded_output_streams --- */
    if (active_prof->contains("encoded_output_streams") &&
        (*active_prof)["encoded_output_streams"].is_array())
    {
        auto &eos_arr = (*active_prof)["encoded_output_streams"];

        std::unordered_set<std::string> target_enc_ids;
        for (const auto &kv : target_profile.encoded_output_streams)
            target_enc_ids.insert(kv.first);

        /* Remove entries not in target */
        for (auto it = eos_arr.begin(); it != eos_arr.end(); )
        {
            if (it->contains("stream_id") && target_enc_ids.count(it->at("stream_id").get<std::string>()) == 0)
            {
                HAL_LOG_INFO("hailo15_media: patch_json: removing encoder '%s'",
                             it->at("stream_id").get<std::string>().c_str());
                it = eos_arr.erase(it);
            }
            else
                ++it;
        }

        /* Add missing encoder entries by cloning an existing one */
        std::unordered_set<std::string> existing_enc_ids;
        for (const auto &e : eos_arr)
            if (e.contains("stream_id"))
                existing_enc_ids.insert(e.at("stream_id").get<std::string>());

        if (!eos_arr.empty())
        {
            auto template_entry = eos_arr.front(); // clone first entry as template
            for (const auto &tid : target_enc_ids)
            {
                if (existing_enc_ids.count(tid))
                    continue;
                json new_entry = template_entry;
                new_entry["stream_id"] = tid;

                /* Update dimensions/framerate from target profile */
                auto enc_it = target_profile.encoded_output_streams.find(tid);
                if (enc_it != target_profile.encoded_output_streams.end())
                {
                    std::visit([&](const auto &enc) {
                        using T = std::decay_t<decltype(enc)>;
                        if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
                        {
                            if (new_entry.contains("encoding"))
                            {
                                if (new_entry["encoding"].is_string())
                                {
                                    /* File-path reference: read, patch, write to new file.
                                     * Encoder config file structure is:
                                     *   {"encoding": {"input_stream": {...}, "hailo_encoder": {...}}}
                                     * Both input_stream and hailo_encoder are nested under "encoding". */
                                    std::string tmpl_path = new_entry["encoding"].get<std::string>();
                                    HAL_LOG_INFO("hailo15_media: patch_json: encoding is file path '%s' for '%s'",
                                                 tmpl_path.c_str(), tid.c_str());
                                    std::ifstream ef(tmpl_path);
                                    if (ef.is_open())
                                    {
                                        json enc_cfg;
                                        ef >> enc_cfg;
                                        ef.close();

                                        /* Navigate into the "encoding" wrapper if present */
                                        json *enc_root = &enc_cfg;
                                        if (enc_cfg.contains("encoding") && enc_cfg["encoding"].is_object())
                                        {
                                            enc_root = &enc_cfg["encoding"];
                                            HAL_LOG_INFO("hailo15_media: patch_json: found 'encoding' wrapper, navigating into it");
                                        }
                                        else
                                        {
                                            HAL_LOG_INFO("hailo15_media: patch_json: no 'encoding' wrapper, patching at top level");
                                        }

                                        if (enc_root->contains("input_stream"))
                                        {
                                            auto &inp = (*enc_root)["input_stream"];
                                            HAL_LOG_INFO("hailo15_media: patch_json: encoder input_stream BEFORE: %ux%u@%u",
                                                         inp.value("width", 0), inp.value("height", 0),
                                                         inp.value("framerate", 0));
                                            inp["width"] = enc.input_stream.width;
                                            inp["height"] = enc.input_stream.height;
                                            inp["framerate"] = enc.input_stream.framerate;
                                            HAL_LOG_INFO("hailo15_media: patch_json: encoder input_stream AFTER: %ux%u@%u",
                                                         enc.input_stream.width, enc.input_stream.height,
                                                         enc.input_stream.framerate);
                                        }
                                        else
                                        {
                                            HAL_LOG_WARNING("hailo15_media: patch_json: encoder config has no 'input_stream' key!");
                                        }
                                        if (enc_root->contains("hailo_encoder"))
                                        {
                                            auto &he = (*enc_root)["hailo_encoder"];
                                            if (he.contains("config") && he["config"].contains("output_stream"))
                                            {
                                                he["config"]["output_stream"]["codec"] =
                                                    (enc.output_stream.codec == CODEC_TYPE_HEVC)
                                                        ? "CODEC_TYPE_HEVC" : "CODEC_TYPE_H264";
                                            }
                                        }

                                        /* Write to /tmp/ — the template directory (e.g. /usr/bin/profile/)
                                         * may be on a read-only filesystem on the device. */
                                        if (enc_cfg.contains("metadata") && enc_cfg["metadata"].is_object())
                                        {
                                            auto enc_hash = compute_medialib_content_hash(enc_cfg);
                                            if (enc_hash) enc_cfg["metadata"]["content_hash"] = *enc_hash;
                                        }
                                        std::string new_path = std::string("/tmp/encoder_inject_") + tid + ".json";
                                        std::ofstream of(new_path);
                                        if (of.is_open())
                                        {
                                            of << enc_cfg.dump(4);
                                            of.close();
                                            new_entry["encoding"] = new_path;
                                            HAL_LOG_INFO("hailo15_media: patch_json: wrote encoder config '%s' for '%s'",
                                                         new_path.c_str(), tid.c_str());
                                        }
                                        else
                                        {
                                            HAL_LOG_ERROR("hailo15_media: patch_json: FAILED to write encoder config '%s'",
                                                          new_path.c_str());
                                        }
                                    }
                                    else
                                    {
                                        HAL_LOG_ERROR("hailo15_media: patch_json: FAILED to open template encoder config '%s'",
                                                      tmpl_path.c_str());
                                    }
                                }
                                else if (new_entry["encoding"].is_object())
                                {
                                    auto &enc_obj = new_entry["encoding"];
                                    if (enc_obj.contains("hailo_encoder"))
                                    {
                                        auto &he = enc_obj["hailo_encoder"];
                                        if (he.contains("config"))
                                        {
                                            if (he["config"].contains("input_stream"))
                                            {
                                                he["config"]["input_stream"]["width"] = enc.input_stream.width;
                                                he["config"]["input_stream"]["height"] = enc.input_stream.height;
                                                he["config"]["input_stream"]["framerate"] = enc.input_stream.framerate;
                                            }
                                            if (he["config"].contains("output_stream"))
                                            {
                                                he["config"]["output_stream"]["codec"] =
                                                    (enc.output_stream.codec == CODEC_TYPE_HEVC) ? "CODEC_TYPE_HEVC" : "CODEC_TYPE_H264";
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }, enc_it->second.encoding);
                }

                eos_arr.push_back(new_entry);
                HAL_LOG_INFO("hailo15_media: patch_json: added encoder entry for '%s'", tid.c_str());
            }
        }
    }

    /* Write back patched application_settings if it was in a file */
    if (app_obj == &app_data && active_prof->contains("application_settings") &&
        (*active_prof)["application_settings"].is_string())
    {
        if (app_data.contains("metadata") && app_data["metadata"].is_object())
        {
            auto as_hash = compute_medialib_content_hash(app_data);
            if (as_hash) app_data["metadata"]["content_hash"] = *as_hash;
        }
        std::string as_path = (*active_prof)["application_settings"].get<std::string>();
        std::ofstream asof(as_path);
        if (asof.is_open())
        {
            asof << app_data.dump(4);
            asof.close();
        }
    }
    /* Write back patched profile if it was in a file */
    if (active_prof == &profile_data && prof_entry->contains("config_file"))
    {
        if (profile_data.contains("metadata") && profile_data["metadata"].is_object())
        {
            auto p_hash = compute_medialib_content_hash(profile_data);
            if (p_hash) profile_data["metadata"]["content_hash"] = *p_hash;
        }
        std::string prof_path = (*prof_entry)["config_file"].get<std::string>();
        std::ofstream pof(prof_path);
        if (pof.is_open())
        {
            pof << profile_data.dump(4);
            pof.close();
        }
    }

    return cfg.dump(2);
}

/**
 * Full MediaLibrary teardown + reinit for stream layout changes (add/remove).
 * Patches the stored config JSON to match the target stream layout, then
 * initializes a fresh ML with that JSON.  The DSP multi_resize is configured
 * during initialize() and cannot add/remove outputs dynamically, so the JSON
 * must contain the correct layout before initialization.
 */
} // namespace

extern "C" {

static int hailo15_media_init(const HalMediaConfig *config, void **media_ctx_return)
{
    if (!config || !media_ctx_return)
    {
        return HAL_ERR_INVALID_ARG;
    }

    std::string json;
    std::string effective_path; /* non-empty when config was loaded from a file path */
    if (config->config_json && config->config_json[0])
    {
        json = config->config_json;
    }
    else
    {
        if (config->config_path && config->config_path[0])
        {
            effective_path = config->config_path;
            json = hailo15_read_file(effective_path.c_str());
            if (json.empty())
            {
                HAL_LOG_ERROR("hailo15_media: init: failed to read media library config '%s'",
                              effective_path.c_str());
                return HAL_ERR_INVALID_ARG;
            }
        }
        else
        {
            /* Neither config_path nor config_json provided: fall back to the compiled-in
             * default (SDK webserver config for the Basic profiles, baked into the .so).
             * The materializer extracts the embedded files to a scratch dir and returns the
             * self-contained container JSON. */
            HAL_LOG_INFO("hailo15_media: init: config_path/config_json both empty; "
                         "using compiled-in default medialib config");
            std::string err;
            if (!hailo15::materialize_default_medialib_config(json, &err))
            {
                HAL_LOG_ERROR("hailo15_media: init: default config materialization failed: %s", err.c_str());
                return HAL_ERR_INVALID_ARG;
            }
        }
    }

    auto ml_exp = MediaLibrary::create();
    if (!ml_exp)
    {
        return HAL_ERROR;
    }

    auto *priv = new Hailo15MediaPriv();
    priv->media_lib = ml_exp.value();
    /* Align with v1.11+ examples/webserver behavior: preserve persistent settings across profile switches. */
    priv->media_lib->set_override_persistent_settings(true);
    priv->stored_config_json = json;
    if (!effective_path.empty())
    {
        priv->stored_config_path = effective_path;
    }
    else if (config->config_path)
    {
        priv->stored_config_path = config->config_path;
    }
    priv->image_overrides = config->image_config;
    if (config->encoder_overrides_json)
        priv->encoder_overrides_json = config->encoder_overrides_json;

    const bool hal_backup_path =
        (config->backup_folder_path != nullptr && config->backup_folder_path[0] != '\0');
    if (hal_backup_path)
    {
        priv->media_lib->set_default_backup_folder_path(std::string(config->backup_folder_path));
    }

    /* IMPORTANT: MediaLibrary may load/validate existing backup profiles when backup is enabled at init-time.
     * If the folder contains schema-incompatible backups (e.g. application_analytics), init will fail.
     * Sanitize the known-bad fields before enabling restore from backups. */
    if (hal_backup_path)
    {
        sanitize_profile_backups_on_disk(std::string(config->backup_folder_path));
    }

    /* Apply encoder dimension overrides before initialize().
     * Prefer explicit YAML-based overrides from camera-daemon (apply_encoder_overrides)
     * which corrects ALL profiles.  Fall back to auto-detection from application_settings
     * (fix_encoder_dimension_mismatches) when no overrides are provided. */
    if (config->encoder_overrides_json && config->encoder_overrides_json[0])
    {
        apply_encoder_overrides(json, config->encoder_overrides_json);
    }
    else
    {
        fix_encoder_dimension_mismatches(json);
    }

    /* Re-sync stored_config_json now that encoder overrides / dimension fixes have
     * been applied to `json`. Otherwise stored_config_json diverges from the live
     * pipeline: it would hold the raw loaded config (e.g. a 4k module config) while
     * the pipeline runs at the YAML-overridden geometry (e.g. 1080P). That divergence
     * is what later forces rotation_full_reinit to re-apply the boot-frozen, never-
     * refreshed encoder_overrides_json and revert runtime resolution changes. Keeping
     * stored_config_json authoritative (= running config) lets rotation reuse it
     * directly, mirroring reconfigure_pipeline's skip_encoder_overrides path. */
    priv->stored_config_json = json;

    /* Validate medialib's internal config file before initialize().
     * Medialib reads /usr/bin/media_server_cfg.json during IspManager::switch_to_sdr()
     * → edit_media_server_cfg(). If the file is empty or unparseable, medialib calls ABRT.
     * Restore a known-good minimal config when the file is corrupted. */
    {
        constexpr const char *msc_path = "/usr/bin/media_server_cfg.json";
        static const std::string msc_default =
            "{\n"
            "    \"3dnr\": { \"enable\": true },\n"
            "    \"awb\": { \"stitch_mode\": 0 },\n"
            "    \"bls\": { \"dummy\": false },\n"
            "    \"dgain\": { \"dummy\": false },\n"
            "    \"hdr\": { \"compression\": false },\n"
            "    \"vsm\": {\n"
            "        \"vsm_h_offset\": 960, \"vsm_h_size\": 1920,\n"
            "        \"vsm_v_offset\": 540, \"vsm_v_size\": 1080\n"
            "    }\n"
            "}\n";
        bool needs_restore = false;
        std::ifstream msc_check(msc_path, std::ios::ate);
        if (!msc_check || msc_check.tellg() <= 0)
        {
            needs_restore = true;
        }
        else
        {
            msc_check.seekg(0);
            std::string msc_content((std::istreambuf_iterator<char>(msc_check)),
                                     std::istreambuf_iterator<char>());
            auto msc_json = nlohmann::json::parse(msc_content, nullptr, false);
            needs_restore = msc_json.is_discarded();
        }
        if (needs_restore)
        {
            HAL_LOG_WARNING("hailo15_media: %s is corrupted, restoring default", msc_path);
            std::ofstream msc_fix(msc_path, std::ios::trunc);
            if (msc_fix)
            {
                msc_fix << msc_default;
                HAL_LOG_INFO("hailo15_media: restored %s", msc_path);
            }
            else
            {
                HAL_LOG_ERROR("hailo15_media: failed to restore %s", msc_path);
            }
        }
    }

    const media_library_return ini = priv->media_lib->initialize(json, hal_backup_path);
    if (ini != MEDIA_LIBRARY_SUCCESS)
    {
        delete priv;
        return hailo15_ml_err(ini);
    }

    /* After initialize(), cache the patched encoder dimensions from the YAML overrides.
     * The in-memory config_profile_t from get_current_profile() may contain original
     * (unpatched) dimensions because medialib loads the profile from the original JSON
     * structure.  We store the correct (patched) dimensions so that rotation swaps use
     * the right values instead of the sensor resolution. */
    if (config->encoder_overrides_json && config->encoder_overrides_json[0])
    {
        try
        {
            auto ov = nlohmann::json::parse(config->encoder_overrides_json, nullptr, false);
            if (!ov.is_discarded() && ov.is_array())
            {
                for (const auto &entry : ov)
                {
                    std::string sid = entry.value("stream_name", "");
                    if (sid.empty()) continue;
                    uint32_t ow = entry.value("width", 0u);
                    uint32_t oh = entry.value("height", 0u);
                    if (ow == 0 || oh == 0) continue;
                    priv->encoder_patched_dims[sid] = {ow, oh};
                    HAL_LOG_INFO("hailo15_media: cached patched encoder dims: '%s' = %ux%u",
                                 sid.c_str(), ow, oh);
                }
            }
        }
        catch (...)
        {
        }
    }

    if (hal_backup_path)
    {
        priv->medialib_default_backup_folder = std::string(config->backup_folder_path);
    }
    else
    {
        try
        {
            const nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
            if (!j.is_discarded() && j.contains("backup_folder_path") && j["backup_folder_path"].is_string())
            {
                priv->medialib_default_backup_folder = j["backup_folder_path"].get<std::string>();
            }
        }
        catch (...)
        {
        }
    }

    {
        /* v1.12.0: single-subscriber — re-subscribe replaces the previous callback (no unsubscribe needed). */
        media_library_return sub = priv->media_lib->subscribe_to_pipeline_state_change(
            [](media_library_pipeline_state_t state) {
                HAL_LOG_INFO("hailo15_media: MediaLibrary pipeline state -> %s (%d)", ml_pipeline_state_str(state),
                             static_cast<int>(state));
            });
        if (sub != MEDIA_LIBRARY_SUCCESS)
        {
            HAL_LOG_WARNING("hailo15_media: subscribe_to_pipeline_state_change failed (%d)", static_cast<int>(sub));
        }
    }

    hailo15_parse_profile_names_from_config_json(json, &priv->profile_names);

    auto *hm = static_cast<HalMediaContext *>(calloc(1, sizeof(HalMediaContext)));
    if (!hm)
    {
        delete priv;
        return HAL_ERR_NO_MEM;
    }
    hm->status = HAL_STATUS_INITIALIZED;
    hm->config = *config;
    hm->config.config_json = priv->stored_config_json.c_str();
    hm->config.config_path =
        priv->stored_config_path.empty() ? nullptr : priv->stored_config_path.c_str();
    hm->priv = priv;
    priv->hal_media_ctx = hm;

    int br = build_contexts(priv, hm);
    if (br != HAL_OK)
    {
        destroy_contexts(priv, hm);
        std::free(hm);
        delete priv;
        return br;
    }
    priv->last_frontend_stream_ids = priv->frontend_stream_ids;
    priv->last_encoder_stream_ids = priv->encoder_stream_ids;

    /* Initialize OSD layout state from the active profile for future rescaling on layout changes. */
    {
        auto osd_prof_exp = priv->media_lib->get_current_profile();
        if (osd_prof_exp)
        {
            refresh_osd_layout_from_profile(priv, osd_prof_exp.value());
        }
    }

    /* Sync image_config from medialib profile so get_current_config() reflects actual runtime state.
     * Without this, hm->config.image_config stays zero-initialized and diverges from the profile
     * defaults (e.g. profile has dewarp=true but the API reports dewarp=false until a manual toggle). */
    {
        auto prof_exp = priv->media_lib->get_current_profile();
        if (prof_exp)
        {
            const auto &iq = prof_exp.value().iq_settings;
            hm->config.image_config.dewarp = iq.dewarp.enabled;
            hm->config.image_config.grayscale = iq.grayscale.enabled;
        }
    }

    *media_ctx_return = hm;
    return HAL_OK;
}

static int hailo15_media_deinit(void *media_ctx)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    if (!hm || !hm->priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    auto *priv = static_cast<Hailo15MediaPriv *>(hm->priv);

    if (priv->media_lib)
    {
        (void)priv->media_lib->stop_pipeline();
        (void)priv->media_lib->shutdown();
        priv->media_lib.reset();
    }

    priv->video_subscribers.clear();
    priv->codec_packet_subscribers.clear();
    destroy_contexts(priv, hm);

    delete priv;
    hm->priv = nullptr;
    std::free(hm);
    return HAL_OK;
}

static int hailo15_media_start(void *media_ctx)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!hm || !priv || !priv->media_lib)
    {
        return HAL_ERR_INVALID_ARG;
    }

    if (priv->pipeline_started)
    {
        /* Do not hold priv->mutex across MediaLibrary calls. */
        if (priv->media_lib->get_pipeline_state() == media_library_pipeline_state_t::PIPELINE_STATE_RUNNING)
        {
            return HAL_OK;
        }
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->pipeline_started = false;
        hm->status = HAL_STATUS_STOPPED;
    }

    /*
     * Do not hold priv->mutex across start_pipeline(): encoder threads may need the lock while the pipeline
     * is coming up (same ordering rule as stop — see hailo15_media_stop).
     */
    HAL_LOG_INFO("hailo15_media: start: connect_encoders / connect_frontend ...");
    int ce = connect_encoders(priv);
    if (ce != HAL_OK)
    {
        return ce;
    }
    int cf = connect_frontend(priv);
    if (cf != HAL_OK)
    {
        return cf;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->callbacks_registered = true;
    }

    {
        std::lock_guard<std::mutex> lock(priv->callback_lifecycle_mu);
        priv->callbacks_quiescing = false;
    }

    HAL_LOG_INFO("hailo15_media: start: calling start_pipeline() ...");
    media_library_return r = priv->media_lib->start_pipeline();
    if (r != MEDIA_LIBRARY_SUCCESS)
    {
        HAL_LOG_WARNING("hailo15_media: start_pipeline failed (%d)", static_cast<int>(r));
        {
            std::lock_guard<std::mutex> lock(priv->callback_lifecycle_mu);
            priv->callbacks_quiescing = true;
        }
        disconnect_ml_bridge_callbacks(priv);
        {
            std::lock_guard<std::recursive_mutex> lock(priv->mutex);
            priv->callbacks_registered = false;
        }
        return hailo15_ml_err(r);
    }

    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->pipeline_started = true;
        hm->status = HAL_STATUS_RUNNING;
    }
    HAL_LOG_INFO("hailo15_media: start_pipeline OK (ML callback will also log RUNNING)");
    /* Mirror webserver RESET_ISP behavior on pipeline start/restart. */
    hailo15_isp_webserver_reset_state();
    return HAL_OK;
}

static int hailo15_media_stop(void *media_ctx)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!hm || !priv || !priv->media_lib)
    {
        return HAL_ERR_INVALID_ARG;
    }

    /*
     * Stop new frontend callbacks and drain callbacks already inside add_buffer()
     * while the encoder is still consuming. Stopping or disconnecting first can
     * strand a callback in add_buffer(), race encoder stride reconfiguration, or
     * deadlock MediaLibrary's flush path.
     */
    uint32_t inflight_at_quiesce = 0;
    {
        std::lock_guard<std::mutex> lock(priv->callback_lifecycle_mu);
        priv->callbacks_quiescing = true;
        inflight_at_quiesce = priv->frontend_callbacks_inflight;
    }
    HAL_LOG_INFO("hailo15_media: stop: quiescing frontend callbacks (inflight=%u)",
                 inflight_at_quiesce);

    {
        constexpr auto callback_drain_timeout = std::chrono::seconds(3);
        std::unique_lock<std::mutex> lock(priv->callback_lifecycle_mu);
        if (!priv->callback_lifecycle_cv.wait_for(
                lock, callback_drain_timeout,
                [&] { return priv->frontend_callbacks_inflight == 0; }))
        {
            const uint32_t inflight = priv->frontend_callbacks_inflight;
            priv->callbacks_quiescing = false;
            HAL_LOG_ERROR("hailo15_media: stop: frontend callback drain timed out "
                          "after %llds (inflight=%u)",
                          static_cast<long long>(callback_drain_timeout.count()), inflight);
            return HAL_ERR_TIMEOUT;
        }
    }
    HAL_LOG_INFO("hailo15_media: stop: frontend callbacks drained");

    /*
     * No frontend bridge callback can now be running or newly enter. Drop the
     * subscriptions before GStreamer teardown without holding priv->mutex.
     */
    bool had_bridge = false;
    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        had_bridge = priv->callbacks_registered;
    }
    if (had_bridge)
    {
        HAL_LOG_INFO("hailo15_media: stop: disconnect_ml_bridge_callbacks before stop_pipeline()");
        disconnect_ml_bridge_callbacks(priv);
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->callbacks_registered = false;
    }

    /*
     * Never hold priv->mutex while stopping the pipeline: encoder callbacks take this lock around the full
     * user callback path; stop_pipeline() may join or synchronize with those threads and would deadlock.
     */
    HAL_LOG_INFO("hailo15_media: stop: calling stop_pipeline() ...");
    const media_library_return r = priv->media_lib->stop_pipeline();
    HAL_LOG_INFO("hailo15_media: stop: stop_pipeline() returned %d", static_cast<int>(r));

    if (r != MEDIA_LIBRARY_SUCCESS && had_bridge)
    {
        const int ce = connect_encoders(priv);
        const int cf = connect_frontend(priv);
        if (ce == HAL_OK && cf == HAL_OK)
        {
            std::lock_guard<std::recursive_mutex> bridge_lock(priv->mutex);
            priv->callbacks_registered = true;
        }
        {
            std::lock_guard<std::mutex> lifecycle_lock(priv->callback_lifecycle_mu);
            priv->callbacks_quiescing = false;
        }
    }

    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    priv->pipeline_started = false;
    hm->status = HAL_STATUS_STOPPED;
    if (r == MEDIA_LIBRARY_SUCCESS)
    {
        HAL_LOG_INFO("hailo15_media: stop_pipeline OK (ML callback will also log STOPPED)");
    }
    else
    {
        HAL_LOG_WARNING("hailo15_media: stop_pipeline returned %d", static_cast<int>(r));
    }
    /* Mirror webserver RESET_ISP: next ISP GET/SET should re-init and recapture baselines. */
    hailo15_isp_webserver_reset_state();
    return hailo15_ml_err(r);
}

static int hailo15_media_get_status(void *media_ctx)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv || !priv->media_lib)
    {
        return HAL_STATUS_UNINITIALIZED;
    }
    return priv->pipeline_started ? HAL_STATUS_RUNNING : HAL_STATUS_INITIALIZED;
}

static int hailo15_media_get_current_profile(void *media_ctx, char **profile_name)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv || !priv->media_lib || !profile_name)
    {
        return HAL_ERR_INVALID_ARG;
    }
    auto cp = priv->media_lib->get_current_profile();
    if (!cp)
    {
        return HAL_ERROR;
    }
    static thread_local char name_buf[256];
    std::strncpy(name_buf, cp.value().name.c_str(), sizeof(name_buf) - 1);
    name_buf[sizeof(name_buf) - 1] = '\0';
    *profile_name = name_buf;
    return HAL_OK;
}

static thread_local std::vector<std::string> g_profile_name_strings;

static int hailo15_media_get_profile_list(void *media_ctx, char **profile_list, uint32_t *profile_list_count)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv || !profile_list || !profile_list_count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    g_profile_name_strings = priv->profile_names;
    *profile_list_count = static_cast<uint32_t>(g_profile_name_strings.size());
    for (uint32_t i = 0; i < *profile_list_count; i++)
    {
        profile_list[i] = const_cast<char *>(g_profile_name_strings[i].c_str());
    }
    return HAL_OK;
}

/**
 * Profile switches use MediaLibrary::set_profile() -> set_override_parameters() (see media_library.cpp).
 * Webserver only calls set_profile(); HAL adds:
 * - Unsubscribe frontend + encoders before set_profile only when the target profile has a different set of
 *   frontend/encoder stream ids (verify_removed_outputs_not_subscribed). Same-layout switches (e.g. Daylight→HDR)
 *   skip this to match ML's configure path more closely.
 * - After success: refresh HAL contexts, then reconnect bridges. If MediaLibrary::stream_restart_required was
 *   true, ML already executed stop_pipeline/configure/start_pipeline; HAL skips a second stop/start and only
 *   reconnects bridges (avoids redundant teardown that can deadlock or confuse GStreamer).
 * - If ML did not full-restart and stream layout changed (pre-set_profile disconnect): HAL runs stop_pipeline →
 *   bridges → start_pipeline. Same-layout switches match webserver: reconnect bridges only, no extra stop/start.
 */
static int hailo15_media_switch_profile(void *media_ctx, const char *profile_name, bool force_recycle)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!hm || !priv || !priv->media_lib || !profile_name)
    {
        return HAL_ERR_INVALID_ARG;
    }

    /* Same rule as stop: set_profile may stop/restart the pipeline; must not hold priv->mutex across it. */
    const bool had_bridge = priv->callbacks_registered;

    std::optional<config_profile_t> prev_prof;
    {
        auto prev_exp = priv->media_lib->get_current_profile();
        if (prev_exp.has_value())
        {
            prev_prof = std::move(prev_exp.value());
        }
    }

    std::optional<config_profile_t> next_prof;
    {
        auto next_exp = priv->media_lib->get_profile(std::string(profile_name));
        if (next_exp.has_value())
        {
            next_prof = std::move(next_exp.value());
        }
    }

    bool disconnect_before = false;
    if (had_bridge)
    {
        if (prev_prof.has_value() && next_prof.has_value())
        {
            disconnect_before = profile_stream_layout_differs(prev_prof.value(), next_prof.value());
        }
        else
        {
            disconnect_before = true;
        }
        if (disconnect_before)
        {
            disconnect_ml_bridge_callbacks(priv);
            HAL_LOG_INFO("hailo15_media: switch_profile: unsubscribed HAL bridges before set_profile (layout differs)");
        }
        else
        {
            HAL_LOG_INFO(
                "hailo15_media: switch_profile: same stream-id layout as target — skip pre-set_profile disconnect");
        }
    }

    /* Same-layout AI-ISP switches (Gen1<->Gen2<->Gen3, Daylight<->AI-ISP) would take the
     * in-place fast path: set_profile reconfigures a RUNNING pipeline and issues
     * FAST_TOGGLE_START on the live FE stream. Each switch leaves FE state residue; after
     * ~7 switches the FE wedges (isp_manager.cpp:875 "Failed to set FAST_TOGGLE_START" ->
     * kernel "fe transaction timeout! FE is stuck!", reboot-only). Pre-stopping makes
     * set_profile reconfigure a STOPPED pipeline (no mid-stream toggle), mirroring the
     * proven pre_stop pattern in apply_profile_override_and_refresh (:1025). Stop BEFORE
     * disconnect (avoids SIGSEGV if the GStreamer streaming thread is mid on_new_sample).
     * Forward switches only - force_recycle rollback keeps its existing post-recycle path,
     * and true cross-layout (disconnect_before) keeps ML's own full restart. */
    const bool pre_stop = had_bridge && !disconnect_before && !force_recycle;
    if (pre_stop)
    {
        HAL_LOG_INFO(
            "hailo15_media: switch_profile(%s): pre-stopping pipeline before set_profile "
            "(same-layout FAST_TOGGLE accumulation avoidance)",
            profile_name);
        priv->media_lib->stop_pipeline();
        disconnect_ml_bridge_callbacks(priv);
        HAL_LOG_INFO("hailo15_media: switch_profile(%s): unsubscribed HAL bridges after pre-stop", profile_name);
    }

    media_library_return r = priv->media_lib->set_profile(std::string(profile_name));
    if (r != MEDIA_LIBRARY_SUCCESS)
    {
        /* Surface the thermal/restriction context that MEDIA_LIBRARY_PROFILE_IS_RESTRICTED (12)
         * comes from: a non-FULL_PERFORMANCE throttling state gates AI Denoise off, so any target
         * profile with iq_settings.denoise.enabled is rejected before the pipeline is touched
         * (hence pipeline_state stays RUNNING). config_manager's restricted type is private, so we
         * report the observable signals: throttling state, auto-restriction flag, target denoise. */
        const bool restricted = (r == MEDIA_LIBRARY_PROFILE_IS_RESTRICTED);
        const auto throttling_exp = priv->media_lib->get_throttling_state();
        const media_library_throttling_state_t tst =
            throttling_exp.has_value() ? throttling_exp.value()
                                       : media_library_throttling_state_t::THROTTLING_STATE_UNINITIALIZED;
        const bool auto_restriction = priv->media_lib->get_auto_profile_restriction_enabled();
        const bool target_denoise =
            next_prof.has_value() && next_prof.value().iq_settings.denoise.enabled;
        HAL_LOG_WARNING(
            "hailo15_media: set_profile(%s) failed (%d), ML pipeline_state=%s (%d), throttling=%s, "
            "auto_restriction=%d, target_denoise.enabled=%d%s",
            profile_name, static_cast<int>(r), ml_pipeline_state_str(priv->media_lib->get_pipeline_state()),
            static_cast<int>(priv->media_lib->get_pipeline_state()), ml_throttling_state_str(tst),
            static_cast<int>(auto_restriction), static_cast<int>(target_denoise),
            restricted ? " -> PROFILE_IS_RESTRICTED (thermal gates AI Denoise; cool to FULL_PERFORMANCE "
                          "or pick a denoise-off profile)"
                       : "");
        if (pre_stop)
        {
            /* set_profile failed after we pre-stopped: restore the RUNNING pipeline + bridges
             * so streaming continues on the previous profile (mirrors apply_profile_override_and_refresh
             * failure handling at :1049). Profile is unchanged on failure, so reconnect uses prior IDs. */
            HAL_LOG_INFO("hailo15_media: switch_profile(%s): restoring pipeline after set_profile failure",
                         profile_name);
            const int ce = connect_encoders(priv);
            if (ce != HAL_OK)
            {
                return ce;
            }
            const int cf = connect_frontend(priv);
            if (cf != HAL_OK)
            {
                return cf;
            }
            {
                std::lock_guard<std::recursive_mutex> lock(priv->mutex);
                priv->callbacks_registered = true;
            }
            media_library_return st = priv->media_lib->start_pipeline();
            if (st != MEDIA_LIBRARY_SUCCESS)
            {
                HAL_LOG_WARNING("hailo15_media: switch_profile: restore start_pipeline failed (%d)",
                                static_cast<int>(st));
            }
        }
        else if (had_bridge && disconnect_before)
        {
            const int ce = connect_encoders(priv);
            if (ce != HAL_OK)
            {
                return ce;
            }
            const int cf = connect_frontend(priv);
            if (cf != HAL_OK)
            {
                return cf;
            }
        }
        return hailo15_ml_err(r);
    }

    /* Mirror webserver SWITCH_PROFILE: force ISP gate/baseline recapture on next GET/SET. */
    hailo15_isp_webserver_reset_state();

    HAL_LOG_INFO(
        "hailo15_media: set_profile(%s) OK — subscribe_to_pipeline_state_change only fires on full "
        "stop_pipeline()/start_pipeline(); pause/unpause or in-place reconfig does not emit STOPPED/RUNNING.",
        profile_name);

    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    const config_profile_t &prof = prof_exp.value();

    const bool ml_did_full_restart =
        prev_prof.has_value() && ml_stream_restart_required(prev_prof.value(), prof);
    /* Same-layout profile switches: align with webserver (set_profile only). Extra HAL stop/start breaks some ML
     * paths (e.g. HDR) that rely on pause/unpause or in-place reconfigure without a second stop/start.
     * force_recycle is the explicit override used by rollback after a post-switch frame-verify failure: the
     * media graph reports RUNNING but emits 0 frames, so we must reset it via the full stop/start recycle.
     * Guarded by !ml_did_full_restart so we never stack a HAL stop/start on top of ML's own full restart
     * (that double-stop can deadlock / confuse GStreamer). For same-layout verify-fail rollback
     * ml_did_full_restart is always false, so force_recycle takes effect there. */
    const bool hal_recycle_stop_start = (!ml_did_full_restart) && (disconnect_before || force_recycle);

    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        refresh_ids_from_profile(priv, prof);

        const bool same_layout = (priv->frontend_stream_ids == priv->last_frontend_stream_ids) &&
                                 (priv->encoder_stream_ids == priv->last_encoder_stream_ids);

        if (same_layout)
        {
            for (uint32_t i = 0; i < hm->video_ctx_list_count; i++)
            {
                auto *vc = static_cast<HalVideoContext *>(hm->video_ctx_list[i]);
                hailo15::video_ml::apply_profile_to_video_ctx(vc, prof, vc->video_name);
            }
            for (uint32_t i = 0; i < hm->codec_ctx_list_count; i++)
            {
                auto *cc = static_cast<HalCodecContext *>(hm->codec_ctx_list[i]);
                hailo15::video_ml::apply_profile_to_codec_ctx(cc, prof);
            }
        }
        else
        {
            destroy_contexts(priv, hm);
            const int br = build_contexts(priv, hm);
            if (br != HAL_OK)
            {
                return br;
            }
        }

        refresh_osd_layout_from_profile(priv, prof);
        priv->last_frontend_stream_ids = priv->frontend_stream_ids;
        priv->last_encoder_stream_ids = priv->encoder_stream_ids;
    }

    if (had_bridge)
    {
        disconnect_ml_bridge_callbacks(priv);
        {
            std::lock_guard<std::recursive_mutex> lock(priv->mutex);
            priv->callbacks_registered = false;
        }

        if (hal_recycle_stop_start)
        {
            HAL_LOG_INFO(
                "hailo15_media: switch_profile(%s): %s — HAL recycle (stop_pipeline, bridges, start_pipeline)",
                profile_name,
                disconnect_before ? "layout changed pre-set_profile"
                                  : "force_recycle (verify-fail rollback) on same layout");
            media_library_return sr = priv->media_lib->stop_pipeline();
            if (sr != MEDIA_LIBRARY_SUCCESS)
            {
                HAL_LOG_WARNING("hailo15_media: switch_profile: stop_pipeline failed (%d)", static_cast<int>(sr));
                const int ce = connect_encoders(priv);
                if (ce != HAL_OK)
                {
                    return ce;
                }
                const int cf = connect_frontend(priv);
                if (cf != HAL_OK)
                {
                    return cf;
                }
                {
                    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
                    priv->callbacks_registered = true;
                }
                return hailo15_ml_err(sr);
            }
        }
        else if (ml_did_full_restart)
        {
            HAL_LOG_INFO(
                "hailo15_media: switch_profile(%s): ML already full-restarted pipeline — HAL reconnects bridges only "
                "(no extra stop/start)",
                profile_name);
        }
        else if (pre_stop)
        {
            HAL_LOG_INFO(
                "hailo15_media: switch_profile(%s): pre-stopped before set_profile — reconnect bridges + start_pipeline",
                profile_name);
        }
        else
        {
            HAL_LOG_INFO(
                "hailo15_media: switch_profile(%s): same layout as webserver path — reconnect bridges only (no HAL "
                "stop/start)",
                profile_name);
        }

        const int ce = connect_encoders(priv);
        if (ce != HAL_OK)
        {
            return ce;
        }
        const int cf = connect_frontend(priv);
        if (cf != HAL_OK)
        {
            return cf;
        }
        {
            std::lock_guard<std::recursive_mutex> lock(priv->mutex);
            priv->callbacks_registered = true;
        }

        if (hal_recycle_stop_start || pre_stop)
        {
            media_library_return st = priv->media_lib->start_pipeline();
            if (st != MEDIA_LIBRARY_SUCCESS)
            {
                HAL_LOG_WARNING("hailo15_media: switch_profile: start_pipeline failed (%d)", static_cast<int>(st));
                return hailo15_ml_err(st);
            }
        }
    }

    {
        /* Do not hold priv->mutex across MediaLibrary calls. */
        const media_library_pipeline_state_t pst = priv->media_lib->get_pipeline_state();
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->pipeline_started = (pst == media_library_pipeline_state_t::PIPELINE_STATE_RUNNING);
        hm->status = priv->pipeline_started ? HAL_STATUS_RUNNING : HAL_STATUS_STOPPED;
        HAL_LOG_INFO(
            "hailo15_media: switch_profile(%s) OK, ML pipeline_state=%s (%d), HAL pipeline_started=%d, "
            "hm->status=%s(%d)",
            profile_name, ml_pipeline_state_str(pst), static_cast<int>(pst), static_cast<int>(priv->pipeline_started),
            hal_status_str(hm->status), static_cast<int>(hm->status));
    }

    return HAL_OK;
}

static int hailo15_media_get_video_list(void *media_ctx, void **video_list, uint32_t *video_list_count)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    if (!hm || !video_list || !video_list_count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *video_list_count = hm->video_ctx_list_count;
    *video_list = hm->video_ctx_list;
    return HAL_OK;
}

static int hailo15_media_get_codec_list(void *media_ctx, void **codec_list, uint32_t *codec_list_count)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    if (!hm || !codec_list || !codec_list_count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *codec_list_count = hm->codec_ctx_list_count;
    *codec_list = hm->codec_ctx_list;
    return HAL_OK;
}

static int hailo15_media_get_current_config(void *media_ctx, HalMediaConfig *config)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    if (!hm || !config)
    {
        return HAL_ERR_INVALID_ARG;
    }
    *config = hm->config;
    return HAL_OK;
}

static int hailo15_media_set_config_field(void *media_ctx, const char *field_path,
                                           HalConfigFieldType field_type, const char *field_value)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv || !priv->media_lib || !field_path || !field_value)
    {
        return HAL_ERR_INVALID_ARG;
    }

    HalMediaContext *hm = static_cast<HalMediaContext *>(media_ctx);
    /* Do not hold priv->mutex across MediaLibrary calls: ML may invoke callbacks that take this lock. */
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        HAL_LOG_ERROR("hailo15_media: set_config_field: get_current_profile failed");
        return hailo15_ml_err(prof_exp.error());
    }
    config_profile_t p = prof_exp.value();

    std::string err;
    if (!hailo15_patch_profile_field(p, field_path, field_type, field_value, &err))
    {
        HAL_LOG_WARNING("hailo15_media: set_config_field: '%s' = '%s' rejected (%s)",
                        field_path, field_value, err.c_str());
        return HAL_ERR_INVALID_ARG;
    }

    /* Reuse the shared apply path (handles stream restart / encoder re-sync / bridge refresh
     * exactly like override_stream_params / add_*_stream). */
    const int rc = apply_profile_override_and_refresh(hm, priv, p, "set_config_field");
    if (rc == HAL_OK)
    {
        HAL_LOG_INFO("hailo15_media: set_config_field: '%s' applied", field_path);
    }
    return rc;
}

static int hailo15_media_get_config_field(void *media_ctx, const char *field_path,
                                           HalConfigFieldType *type_out, const char **value_out)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv || !priv->media_lib || !field_path || !value_out)
    {
        return HAL_ERR_INVALID_ARG;
    }

    /* Current profile JSON reflects runtime overrides (sees set_config_field changes). */
    auto exp = priv->media_lib->get_current_profile_str();
    if (!exp.has_value())
    {
        HAL_LOG_ERROR("hailo15_media: get_config_field: get_current_profile_str failed");
        return hailo15_ml_err(exp.error());
    }

    HalConfigFieldType detected = HAL_CONFIG_FIELD_STRING;
    std::string value;
    if (!hailo15_read_profile_field(exp.value(), field_path, &detected, value))
    {
        HAL_LOG_WARNING("hailo15_media: get_config_field: '%s' could not be read", field_path);
        return HAL_ERR_INVALID_ARG;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->current_config_field_value = std::move(value);
    }
    if (type_out)
        *type_out = detected;
    *value_out = priv->current_config_field_value.c_str();
    return HAL_OK;
}

static int hailo15_media_get_current_profile_json(void *media_ctx, const char **json_out)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv || !priv->media_lib || !json_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    auto exp = priv->media_lib->get_current_profile_str();
    if (!exp.has_value())
    {
        return hailo15_ml_err(exp.error());
    }
    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->current_profile_json = exp.value();
    }
    *json_out = priv->current_profile_json.c_str();
    return HAL_OK;
}

static int hailo15_media_backup_current_profile(void *media_ctx, const char *path)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv || !priv->media_lib)
    {
        return HAL_ERR_INVALID_ARG;
    }

    std::string dest;
    if (path && path[0] != '\0')
    {
        dest = path;
    }
    else if (!priv->medialib_default_backup_folder.empty())
    {
        dest = priv->medialib_default_backup_folder;
    }
    else
    {
        return HAL_ERR_INVALID_ARG;
    }

    /* Do not hold priv->mutex across MediaLibrary calls: backup may trigger internal callbacks/IO. */
    priv->media_lib->set_default_backup_folder_path(dest);
    const media_library_return r = priv->media_lib->backup_profiles();
    priv->media_lib->set_default_backup_folder_path(priv->medialib_default_backup_folder);
    if (r != MEDIA_LIBRARY_SUCCESS)
    {
        return hailo15_ml_err(r);
    }

    /* Post-process backups: drop application_analytics (known schema-violating ML output). */
    sanitize_profile_backups_on_disk(dest);
    return HAL_OK;
}

static int hailo15_media_add_video_stream(void *media_ctx, const HalMediaAddVideoConfig *cfg)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!hm || !priv || !priv->media_lib || !cfg || !cfg->stream_id || cfg->stream_id[0] == '\0')
    {
        return HAL_ERR_INVALID_ARG;
    }

    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    config_profile_t p = prof_exp.value();

    const std::string new_id(cfg->stream_id);
    if (std::find_if(p.application_settings.application_input_streams.resolutions.begin(),
                     p.application_settings.application_input_streams.resolutions.end(),
                     [&](const auto &res) { return res.stream_id == new_id; }) !=
        p.application_settings.application_input_streams.resolutions.end())
    {
        HAL_LOG_ERROR("hailo15_media: add_video_stream: target frontend stream '%s' already exists", cfg->stream_id);
        return HAL_ERR_INVALID_STATE;
    }

    output_resolution_t new_res{};
    new_res.stream_id = new_id;
    new_res.dimensions.destination_width = cfg->video.width;
    new_res.dimensions.destination_height = cfg->video.height;
    new_res.framerate = cfg->video.framerate;
    new_res.pool_max_buffers = cfg->video.pool_max_buffers;
    if (new_res.pool_max_buffers == 0U)
    {
        const auto &existing = p.application_settings.application_input_streams.resolutions;
        if (!existing.empty())
        {
            new_res.pool_max_buffers = existing.front().pool_max_buffers;
        }
        if (new_res.pool_max_buffers < 20U)
        {
            new_res.pool_max_buffers = 20U;
        }
    }
    if (new_res.dimensions.destination_width == 0U || new_res.dimensions.destination_height == 0U || new_res.framerate == 0U)
    {
        return HAL_ERR_INVALID_ARG;
    }
    p.application_settings.application_input_streams.resolutions.push_back(new_res);

    p.application_settings.application_input_streams.format = hailo15::video_ml::hal_pixel_to_hailo(cfg->video.format);

    return apply_profile_override_and_refresh(hm, priv, p, "add_video_stream");
}

static int hailo15_media_add_codec_stream(void *media_ctx, const HalMediaAddCodecConfig *cfg)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!hm || !priv || !priv->media_lib || !cfg || !cfg->stream_id || cfg->stream_id[0] == '\0')
    {
        return HAL_ERR_INVALID_ARG;
    }

    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    config_profile_t p = prof_exp.value();
    const std::string new_id(cfg->stream_id);
    if (p.encoded_output_streams.find(new_id) != p.encoded_output_streams.end())
    {
        HAL_LOG_ERROR("hailo15_media: add_codec_stream: target encoder stream '%s' already exists", cfg->stream_id);
        return HAL_ERR_INVALID_STATE;
    }

    auto pick_template = [&](bool want_jpeg) -> std::optional<config_encoded_output_stream_t> {
        for (const auto &kv : p.encoded_output_streams)
        {
            const bool is_jpeg = std::holds_alternative<jpeg_encoder_config_t>(kv.second.encoding);
            if (is_jpeg == want_jpeg)
            {
                return kv.second;
            }
        }
        return std::nullopt;
    };

    const bool want_jpeg = (cfg->codec.packet_type == HAL_PACKET_TYPE_MJPEG);
    auto tmpl = pick_template(want_jpeg);
    if (!tmpl.has_value())
    {
        HAL_LOG_ERROR("hailo15_media: add_codec_stream: no template encoder of requested family exists");
        return HAL_ERR_NOT_SUPPORTED;
    }

    config_encoded_output_stream_t new_stream = tmpl.value();
    new_stream.osd = config_stream_osd_t{};
    new_stream.masking = privacy_mask_config_t{};
    std::visit(
        [&](auto &enc) {
            using T = std::decay_t<decltype(enc)>;
            if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
            {
                if (cfg->codec.packet_type == HAL_PACKET_TYPE_H265)
                {
                    enc.output_stream.codec = CODEC_TYPE_HEVC;
                }
                else
                {
                    enc.output_stream.codec = CODEC_TYPE_H264;
                }
                hailo15::ml::apply_hal_to_hailo_encoder(&enc, &cfg->codec);
                if (cfg->codec.width == 0U || cfg->codec.height == 0U || cfg->codec.framerate == 0U)
                {
                    /* keep template dimensions if caller omitted them */
                }
            }
            else if constexpr (std::is_same_v<T, jpeg_encoder_config_t>)
            {
                hailo15::ml::apply_hal_to_jpeg_encoder(&enc, &cfg->codec);
                if (cfg->codec.width > 0U)
                {
                    enc.input_stream.width = cfg->codec.width;
                }
                if (cfg->codec.height > 0U)
                {
                    enc.input_stream.height = cfg->codec.height;
                }
                if (cfg->codec.framerate > 0U)
                {
                    enc.input_stream.framerate = cfg->codec.framerate;
                }
            }
        },
        new_stream.encoding);

    p.encoded_output_streams[new_id] = new_stream;
    const int rc = apply_profile_override_and_refresh(hm, priv, p, "add_codec_stream");
    if (rc == HAL_OK)
    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->encoder_auto_feed_by_stream[new_id] = false;
    }
    return rc;
}

static std::string next_sink_id(const config_profile_t &p)
{
    /* Find the first unused sink index (reuse freed slots).
       e.g. sink0 + sink1 exist → returns "sink2"
       e.g. sink0 + sink2 exist → returns "sink1" (reuse gap) */
    std::unordered_set<uint32_t> used;
    for (const auto &kv : p.encoded_output_streams)
    {
        if (kv.first.rfind("sink", 0) == 0)
        {
            used.insert(static_cast<uint32_t>(std::stoul(kv.first.substr(4))));
        }
    }
    for (uint32_t i = 0; ; i++)
    {
        if (used.find(i) == used.end())
        {
            return "sink" + std::to_string(i);
        }
    }
}

static int hailo15_media_add_streams_batch(void *media_ctx, const HalMediaAddCodecConfig *codec_cfg,
                                           const HalMediaAddVideoConfig *video_cfg)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!hm || !priv || !priv->media_lib || !codec_cfg || !codec_cfg->stream_id || codec_cfg->stream_id[0] == '\0')
    {
        return HAL_ERR_INVALID_ARG;
    }

    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    config_profile_t p = prof_exp.value();
    const std::string logical_id(codec_cfg->stream_id);

    /* Map logical name to sink-style profile id (sub → sink1, third → sink2, …) */
    const std::string profile_id = next_sink_id(p);
    HAL_LOG_INFO("hailo15_media: add_streams_batch: logical '%s' → profile '%s'", logical_id.c_str(), profile_id.c_str());

    /* --- Codec (encoder) stream --- */
    if (p.encoded_output_streams.find(profile_id) != p.encoded_output_streams.end())
    {
        HAL_LOG_WARNING("hailo15_media: add_streams_batch: replacing stale encoder stream '%s'", profile_id.c_str());
        p.encoded_output_streams.erase(profile_id);
    }

    auto pick_template = [&](bool want_jpeg) -> std::optional<config_encoded_output_stream_t> {
        for (const auto &kv : p.encoded_output_streams)
        {
            const bool is_jpeg = std::holds_alternative<jpeg_encoder_config_t>(kv.second.encoding);
            if (is_jpeg == want_jpeg)
            {
                return kv.second;
            }
        }
        return std::nullopt;
    };

    const bool want_jpeg = (codec_cfg->codec.packet_type == HAL_PACKET_TYPE_MJPEG);
    auto tmpl = pick_template(want_jpeg);
    if (!tmpl.has_value())
    {
        HAL_LOG_ERROR("hailo15_media: add_streams_batch: no template encoder of requested family exists");
        return HAL_ERR_NOT_SUPPORTED;
    }

    config_encoded_output_stream_t new_stream = tmpl.value();
    new_stream.osd = config_stream_osd_t{};
    new_stream.masking = privacy_mask_config_t{};
    std::visit(
        [&](auto &enc) {
            using T = std::decay_t<decltype(enc)>;
            if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
            {
                if (codec_cfg->codec.packet_type == HAL_PACKET_TYPE_H265)
                {
                    enc.output_stream.codec = CODEC_TYPE_HEVC;
                }
                else
                {
                    enc.output_stream.codec = CODEC_TYPE_H264;
                }
                hailo15::ml::apply_hal_to_hailo_encoder(&enc, &codec_cfg->codec);
                if (codec_cfg->codec.width == 0U || codec_cfg->codec.height == 0U || codec_cfg->codec.framerate == 0U)
                {
                    /* keep template dimensions if caller omitted them */
                }
            }
            else if constexpr (std::is_same_v<T, jpeg_encoder_config_t>)
            {
                hailo15::ml::apply_hal_to_jpeg_encoder(&enc, &codec_cfg->codec);
                if (codec_cfg->codec.width > 0U)
                {
                    enc.input_stream.width = codec_cfg->codec.width;
                }
                if (codec_cfg->codec.height > 0U)
                {
                    enc.input_stream.height = codec_cfg->codec.height;
                }
                if (codec_cfg->codec.framerate > 0U)
                {
                    enc.input_stream.framerate = codec_cfg->codec.framerate;
                }
            }
        },
        new_stream.encoding);

    p.encoded_output_streams[profile_id] = new_stream;

    /* --- Video (frontend) stream --- */
    if (video_cfg)
    {
        // Remove any stale frontend entry left from a previous add that wasn't
        // properly cleaned up (e.g. remove_streams_batch was called with the
        // logical name instead of the sink-style profile id).
        auto &res_vec = p.application_settings.application_input_streams.resolutions;
        auto stale_it = std::find_if(res_vec.begin(), res_vec.end(),
                         [&](const auto &res) { return res.stream_id == profile_id; });
        if (stale_it != res_vec.end())
        {
            HAL_LOG_WARNING("hailo15_media: add_streams_batch: replacing stale frontend stream '%s'", profile_id.c_str());
            res_vec.erase(stale_it);
        }

        output_resolution_t new_res{};
        new_res.stream_id = profile_id;
        new_res.dimensions.destination_width = video_cfg->video.width;
        new_res.dimensions.destination_height = video_cfg->video.height;
        new_res.framerate = video_cfg->video.framerate;
        /* pool_max_buffers: copy from an existing stream to match hardware
         * constraints.  Caller may override via video_cfg. */
        new_res.pool_max_buffers = video_cfg->video.pool_max_buffers;
        if (new_res.pool_max_buffers == 0U)
        {
            const auto &existing = p.application_settings.application_input_streams.resolutions;
            if (!existing.empty())
            {
                new_res.pool_max_buffers = existing.front().pool_max_buffers;
            }
            if (new_res.pool_max_buffers < 20U)
            {
                new_res.pool_max_buffers = 20U;
            }
        }
        if (new_res.dimensions.destination_width == 0U || new_res.dimensions.destination_height == 0U || new_res.framerate == 0U)
        {
            return HAL_ERR_INVALID_ARG;
        }
        p.application_settings.application_input_streams.resolutions.push_back(new_res);
        p.application_settings.application_input_streams.format = hailo15::video_ml::hal_pixel_to_hailo(video_cfg->video.format);
    }

    /* Single profile commit — one pipeline restart, not two */
    const int rc = apply_profile_override_and_refresh(hm, priv, p, "add_streams_batch");
    if (rc == HAL_OK)
    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->encoder_auto_feed_by_stream[profile_id] = false;
    }
    return rc;
}

static int hailo15_media_remove_video_stream(void *media_ctx, const HalMediaRemoveVideoConfig *cfg)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!hm || !priv || !priv->media_lib || !cfg || !cfg->stream_id || cfg->stream_id[0] == '\0')
    {
        return HAL_ERR_INVALID_ARG;
    }
    const std::string sid(cfg->stream_id);

    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    config_profile_t p = prof_exp.value();

    auto &vec = p.application_settings.application_input_streams.resolutions;
    const size_t before = vec.size();
    vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const auto &res) { return res.stream_id == sid; }),
              vec.end());
    if (vec.size() == before)
    {
        return HAL_ERR_NOT_FOUND;
    }
    if (vec.empty())
    {
        return HAL_ERR_INVALID_STATE;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->video_subscribers.erase(sid);
        priv->encoder_auto_feed_by_stream.erase(sid);
    }

    return apply_profile_override_and_refresh(hm, priv, p, "remove_video_stream");
}

static int hailo15_media_remove_codec_stream(void *media_ctx, const HalMediaRemoveCodecConfig *cfg)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!hm || !priv || !priv->media_lib || !cfg || !cfg->stream_id || cfg->stream_id[0] == '\0')
    {
        return HAL_ERR_INVALID_ARG;
    }
    const std::string sid(cfg->stream_id);

    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    config_profile_t p = prof_exp.value();

    const auto it = p.encoded_output_streams.find(sid);
    if (it == p.encoded_output_streams.end())
    {
        return HAL_ERR_NOT_FOUND;
    }
    p.encoded_output_streams.erase(it);
    if (p.encoded_output_streams.empty())
    {
        return HAL_ERR_INVALID_STATE;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->codec_packet_subscribers.erase(sid);
        priv->encoder_auto_feed_by_stream.erase(sid);
    }

    return apply_profile_override_and_refresh(hm, priv, p, "remove_codec_stream");
}

static int hailo15_media_remove_streams_batch(void *media_ctx, const char *stream_id)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!hm || !priv || !priv->media_lib || !stream_id || stream_id[0] == '\0')
    {
        return HAL_ERR_INVALID_ARG;
    }
    const std::string sid(stream_id);

    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    config_profile_t p = prof_exp.value();

    /* Remove encoder stream */
    const auto enc_it = p.encoded_output_streams.find(sid);
    if (enc_it != p.encoded_output_streams.end())
    {
        p.encoded_output_streams.erase(enc_it);
    }
    if (p.encoded_output_streams.empty())
    {
        return HAL_ERR_INVALID_STATE;
    }

    /* Remove frontend stream */
    auto &vec = p.application_settings.application_input_streams.resolutions;
    vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const auto &res) { return res.stream_id == sid; }),
              vec.end());
    if (vec.empty())
    {
        return HAL_ERR_INVALID_STATE;
    }

    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->codec_packet_subscribers.erase(sid);
        priv->video_subscribers.erase(sid);
        priv->encoder_auto_feed_by_stream.erase(sid);
    }

    return apply_profile_override_and_refresh(hm, priv, p, "remove_streams_batch");
}

static bool is_portrait_rotation(rotation_angle_t angle)
{
    return angle == ROTATION_ANGLE_90 || angle == ROTATION_ANGLE_270;
}

/**
 * Swap encoder temp-file dimensions in-place and update the config JSON
 * rotation fields.  This is a direct, simple approach: after
 * apply_encoder_overrides() has written /tmp/encoder_override_{id}.json files,
 * we read each one, swap input_stream width↔height, and write it back.
 * Then we patch the main config JSON's rotation settings and
 * application_input_streams resolutions.
 */
static std::string patch_config_json_for_rotation(
    const std::string &config_json,
    const std::map<std::string, std::pair<uint32_t, uint32_t>> &patched_dims,
    bool to_portrait,
    rotation_angle_t angle)
{
    using json = nlohmann::json;

    /* --- Pass A: Find encoder override files and swap their dimensions.
     * The main config's profiles point to /tmp/profile_override_*.json files
     * (created by apply_encoder_overrides).  Those profile files contain
     * encoded_output_streams with /tmp/encoder_override_*.json paths.
     * We need to: (1) load each profile override, (2) collect encoder paths,
     * (3) swap width/height in each encoder file. --- */
    {
        json cfg_pre;
        try { cfg_pre = json::parse(config_json, nullptr, false); }
        catch (...) {}
        if (!cfg_pre.is_discarded())
        {
            /* Collect encoder file paths */
            std::vector<std::string> enc_paths;

            auto collect_encoders_from = [&](json &obj) {
                if (obj.contains("encoded_output_streams") && obj["encoded_output_streams"].is_array())
                {
                    for (auto &eos : obj["encoded_output_streams"])
                    {
                        if (eos.contains("encoding") && eos["encoding"].is_string())
                        {
                            std::string p = eos["encoding"].get<std::string>();
                            /* Only patch /tmp/ override files — vendor files under /etc/
                             * have content_hash validation that rejects modifications. */
                            if (!p.empty() && p.substr(0, 5) == "/tmp/")
                                enc_paths.push_back(p);
                        }
                    }
                }
            };

            /* Check if profiles have config_file references (two-level structure) */
            if (cfg_pre.contains("profiles") && cfg_pre["profiles"].is_array())
            {
                for (auto &prof : cfg_pre["profiles"])
                {
                    if (prof.contains("config_file") && prof["config_file"].is_string())
                    {
                        std::string pf_path = prof["config_file"].get<std::string>();
                        std::ifstream pf(pf_path);
                        if (pf.is_open())
                        {
                            json pf_data;
                            try { pf >> pf_data; } catch (...) {}
                            pf.close();
                            collect_encoders_from(pf_data);
                        }
                    }
                    else
                    {
                        collect_encoders_from(prof);
                    }
                }
            }
            else
            {
                collect_encoders_from(cfg_pre);
            }

            /* Deduplicate */
            std::sort(enc_paths.begin(), enc_paths.end());
            enc_paths.erase(std::unique(enc_paths.begin(), enc_paths.end()), enc_paths.end());

            HAL_LOG_INFO("hailo15_media: rotation_patch: found %zu encoder override files to patch",
                         enc_paths.size());

            for (const auto &path : enc_paths)
            {
                std::ifstream ef(path);
                if (!ef.is_open())
                {
                    HAL_LOG_WARNING("hailo15_media: rotation_patch: cannot open %s", path.c_str());
                    continue;
                }
                json enc_cfg;
                try { ef >> enc_cfg; } catch (...) { ef.close(); continue; }
                ef.close();

                json *enc_root = &enc_cfg;
                if (enc_cfg.contains("encoding") && enc_cfg["encoding"].is_object())
                    enc_root = &enc_cfg["encoding"];
                if (!enc_root->contains("input_stream"))
                {
                    HAL_LOG_WARNING("hailo15_media: rotation_patch: %s has no input_stream", path.c_str());
                    continue;
                }

                auto &inp = (*enc_root)["input_stream"];
                uint32_t w = inp.value("width", 0u);
                uint32_t h = inp.value("height", 0u);
                HAL_LOG_INFO("hailo15_media: rotation_patch: %s before swap: %ux%u, to_portrait=%d",
                             path.c_str(), w, h, to_portrait);
                if (to_portrait) std::swap(w, h);
                inp["width"] = w;
                inp["height"] = h;
                HAL_LOG_INFO("hailo15_media: rotation_patch: %s after swap: %ux%u", path.c_str(), w, h);

                /* Recompute content_hash to pass medialib validation. */
                if (enc_cfg.contains("metadata") && enc_cfg["metadata"].is_object())
                {
                    auto h = compute_medialib_content_hash(enc_cfg);
                    if (h) enc_cfg["metadata"]["content_hash"] = *h;
                }

                std::ofstream of(path);
                if (of.is_open())
                {
                    of << enc_cfg.dump(4);
                    of.close();
                }
            }
        }
    }

    /* --- Pass B: Patch rotation and app_input_streams in profile override files.
     * The main config points to /tmp/profile_override_*.json files.  These contain
     * application_settings (which may reference an external app_settings JSON file).
     * We need to patch both the profile file and any referenced app_settings file. --- */
    json cfg;
    try { cfg = json::parse(config_json, nullptr, false); }
    catch (...) { return config_json; }
    if (cfg.is_discarded()) return config_json;

    /* Helper: patch application_settings content (inline or loaded from file) */
    auto patch_app_settings = [&](json &app_obj) {
        if (!app_obj.contains("rotation")) app_obj["rotation"] = json::object();
        app_obj["rotation"]["enabled"] = (angle != ROTATION_ANGLE_0);
        {
            const char *angle_str = "ROTATION_ANGLE_0";
            switch (angle) {
                case ROTATION_ANGLE_90:  angle_str = "ROTATION_ANGLE_90"; break;
                case ROTATION_ANGLE_180: angle_str = "ROTATION_ANGLE_180"; break;
                case ROTATION_ANGLE_270: angle_str = "ROTATION_ANGLE_270"; break;
                default: break;
            }
            app_obj["rotation"]["angle"] = angle_str;
        }

        if (app_obj.contains("application_input_streams") &&
            app_obj["application_input_streams"].contains("resolutions") &&
            app_obj["application_input_streams"]["resolutions"].is_array())
        {
            for (auto &res : app_obj["application_input_streams"]["resolutions"])
            {
                if (to_portrait)
                {
                    uint32_t rw = res.value("width", 0u);
                    uint32_t rh = res.value("height", 0u);
                    res["width"] = rh;
                    res["height"] = rw;
                }
            }
        }
    };

    auto patch_profile_file = [&](const std::string &pf_path) {
        std::ifstream pf(pf_path);
        if (!pf.is_open()) return;
        json pf_data;
        try { pf >> pf_data; } catch (...) { pf.close(); return; }
        pf.close();

        bool changed = false;
        if (pf_data.contains("application_settings"))
        {
            if (pf_data["application_settings"].is_string())
            {
                std::string as_path = pf_data["application_settings"].get<std::string>();
                std::ifstream asf(as_path);
                if (asf.is_open())
                {
                    json as_data;
                    try { asf >> as_data; } catch (...) { asf.close(); return; }
                    asf.close();
                    patch_app_settings(as_data);
                    /* Recompute content_hash within app_settings itself */
                    if (as_data.contains("metadata") && as_data["metadata"].is_object())
                    {
                        auto h = compute_medialib_content_hash(as_data);
                        if (h) as_data["metadata"]["content_hash"] = *h;
                    }
                    /* Write to a unique temp file per profile */
                    std::string basename = pf_path;
                    auto slash = basename.rfind('/');
                    if (slash != std::string::npos) basename = basename.substr(slash + 1);
                    auto dot = basename.rfind('.');
                    if (dot != std::string::npos) basename = basename.substr(0, dot);
                    std::string tmp_as = "/tmp/app_settings_rot_" + basename + ".json";
                    std::ofstream of(tmp_as);
                    if (of.is_open())
                    {
                        of << as_data.dump(4);
                        of.close();
                        pf_data["application_settings"] = tmp_as;
                        changed = true;
                    }
                }
            }
            else if (pf_data["application_settings"].is_object())
            {
                patch_app_settings(pf_data["application_settings"]);
                /* Recompute content_hash within inline app_settings */
                if (pf_data["application_settings"].contains("metadata") &&
                    pf_data["application_settings"]["metadata"].is_object())
                {
                    auto h = compute_medialib_content_hash(pf_data["application_settings"]);
                    if (h) pf_data["application_settings"]["metadata"]["content_hash"] = *h;
                }
                changed = true;
            }
        }

        if (changed)
        {
            /* Recompute content_hash for the profile override */
            if (pf_data.contains("metadata") && pf_data["metadata"].is_object())
            {
                auto h = compute_medialib_content_hash(pf_data);
                if (h) pf_data["metadata"]["content_hash"] = *h;
            }
            std::ofstream of(pf_path);
            if (of.is_open())
            {
                of << pf_data.dump(4);
                of.close();
                HAL_LOG_INFO("hailo15_media: rotation_patch: patched profile %s", pf_path.c_str());
            }
        }
    };

    if (cfg.contains("profiles") && cfg["profiles"].is_array())
    {
        for (auto &prof : cfg["profiles"])
        {
            if (prof.contains("config_file") && prof["config_file"].is_string())
            {
                patch_profile_file(prof["config_file"].get<std::string>());
            }
        }
    }

    return cfg.dump();
}

/**
 * Full medialib shutdown + reinitialize for rotation transitions on large resolutions.
 *
 * The normal set_override_parameters() path stops and restarts the pipeline, but the
 * DSP multi_resize output buffer pool must be re-allocated with swapped dimensions.
 * For large resolutions (e.g. 4K 3840x2160 rotated to 2160x3840), the CMA DMA allocator
 * often fails due to fragmentation — even though total free CMA is sufficient, the bitmap
 * allocator cannot find a contiguous range because ISP and other medialib internal buffers
 * fragment the address space.
 *
 * This function works around the issue by:
 *   1. Shutting down medialib completely (releases ALL DMA buffers including ISP)
 *   2. Patching the config JSON with rotated encoder dimensions
 *   3. Re-initializing medialib from scratch with clean CMA
 *
 * The pipeline starts directly in the rotated configuration, avoiding any stop→restart
 * buffer reallocation.
 */
static int rotation_full_reinit(void *media_ctx, HalMediaContext *hm, Hailo15MediaPriv *priv,
                                const HalMediaImageConfig *cfg)
{
    const rotation_angle_t angle = static_cast<rotation_angle_t>(cfg->rotation_angle);
    const bool to_portrait = is_portrait_rotation(angle);

    HAL_LOG_INFO("hailo15_media: rotation_full_reinit: angle=%d, to_portrait=%d",
                 static_cast<int>(angle), to_portrait);

    /* 1. Reuse stored_config_json as-is. It is authoritative — synced to the running
     *    config at init (after encoder overrides) and after every page change
     *    (reinit_media_library_on_stream_change:5568). Do NOT re-apply encoder_overrides_json
     *    here: it is frozen at boot from YAML encoders (e.g. main=1080P) and would revert
     *    any runtime resolution change (e.g. page-set 4K) when rotation triggers a full
     *    reinit — the same reason reconfigure_pipeline passes skip_encoder_overrides=true.
     *    Only fix encoder/app dimension mismatches to avoid medialib validation failures. */
    std::string patched_json = priv->stored_config_json;
    fix_encoder_dimension_mismatches(patched_json);

    /* 2. Patch rotation in application_settings only (Pass B). Skip encoder dim swap (Pass A).
     * For the full-reinit path, medialib's internal DSP pipeline handles rotation.
     * The encoder input_stream dimensions must remain UN-rotated so that the
     * appsrc caps match the DSP output format.  Only Pass B is needed:
     * set rotation enabled/angle and swap application_input_streams resolutions. */
    {
        using json = nlohmann::json;
        json cfg;
        try { cfg = json::parse(patched_json); } catch (...) {}

        if (!cfg.is_discarded() && cfg.contains("profiles") && cfg["profiles"].is_array())
        {
            /* Helper: patch application_settings content */
            auto patch_app_settings = [&](json &app_obj) {
                if (!app_obj.contains("rotation")) app_obj["rotation"] = json::object();
                app_obj["rotation"]["enabled"] = (angle != ROTATION_ANGLE_0);
                {
                    const char *angle_str = "ROTATION_ANGLE_0";
                    switch (angle) {
                        case ROTATION_ANGLE_90:  angle_str = "ROTATION_ANGLE_90"; break;
                        case ROTATION_ANGLE_180: angle_str = "ROTATION_ANGLE_180"; break;
                        case ROTATION_ANGLE_270: angle_str = "ROTATION_ANGLE_270"; break;
                        default: break;
                    }
                    app_obj["rotation"]["angle"] = angle_str;
                }
                /* Do NOT swap application_input_streams resolutions here.
                 * multi_resize's get_output_resolution_by_index() already
                 * swaps dimensions when rotation is 90/270. Swapping here
                 * too causes a double-swap that negates the rotation. */
            };

            for (auto &prof : cfg["profiles"])
            {
                if (!prof.contains("config_file") || !prof["config_file"].is_string())
                    continue;
                std::string pf_path = prof["config_file"].get<std::string>();
                std::ifstream pf(pf_path);
                if (!pf.is_open()) continue;
                json pf_data;
                try { pf >> pf_data; } catch (...) { pf.close(); continue; }
                pf.close();

                bool changed = false;
                if (pf_data.contains("application_settings"))
                {
                    if (pf_data["application_settings"].is_string())
                    {
                        std::string as_path = pf_data["application_settings"].get<std::string>();
                        std::ifstream asf(as_path);
                        if (asf.is_open())
                        {
                            json as_data;
                            try { asf >> as_data; } catch (...) { asf.close(); continue; }
                            asf.close();
                            patch_app_settings(as_data);
                            if (as_data.contains("metadata") && as_data["metadata"].is_object())
                            {
                                auto h = compute_medialib_content_hash(as_data);
                                if (h) as_data["metadata"]["content_hash"] = *h;
                            }
                            std::string basename = pf_path;
                            auto slash = basename.rfind('/');
                            if (slash != std::string::npos) basename = basename.substr(slash + 1);
                            auto dot = basename.rfind('.');
                            if (dot != std::string::npos) basename = basename.substr(0, dot);
                            std::string tmp_as = "/tmp/app_settings_rot_" + basename + ".json";
                            std::ofstream of(tmp_as);
                            if (of.is_open())
                            {
                                of << as_data.dump(4);
                                of.close();
                                pf_data["application_settings"] = tmp_as;
                                changed = true;
                            }
                        }
                    }
                    else if (pf_data["application_settings"].is_object())
                    {
                        patch_app_settings(pf_data["application_settings"]);
                        if (pf_data["application_settings"].contains("metadata") &&
                            pf_data["application_settings"]["metadata"].is_object())
                        {
                            auto h = compute_medialib_content_hash(pf_data["application_settings"]);
                            if (h) pf_data["application_settings"]["metadata"]["content_hash"] = *h;
                        }
                        changed = true;
                    }
                }
                if (changed)
                {
                    if (pf_data.contains("metadata") && pf_data["metadata"].is_object())
                    {
                        auto h = compute_medialib_content_hash(pf_data);
                        if (h) pf_data["metadata"]["content_hash"] = *h;
                    }
                    std::ofstream of(pf_path);
                    if (of.is_open())
                    {
                        of << pf_data.dump(4);
                        of.close();
                        HAL_LOG_INFO("hailo15_media: rotation_reinit: patched profile %s (rotation only, no encoder swap)",
                                     pf_path.c_str());
                    }
                }
            }
            patched_json = cfg.dump(4);
        }
    }

    /* 3. Disconnect bridge callbacks from old medialib objects. */
    disconnect_ml_bridge_callbacks(priv);

    /* 4. Full shutdown — releases ALL medialib DMA buffers (ISP, DSP, encoder). */
    HAL_LOG_INFO("hailo15_media: rotation_full_reinit: shutting down medialib");
    using Clock = std::chrono::steady_clock;
    auto rr_t0 = Clock::now();
    (void)priv->media_lib->stop_pipeline();
    auto rr_t1 = Clock::now();
    (void)priv->media_lib->shutdown();
    auto rr_t2 = Clock::now();
    priv->media_lib.reset();
    auto rr_t3 = Clock::now();
    HAL_LOG_INFO("[TIMING] rotation_reinit: teardown timing stop_pipeline=%lldms shutdown=%lldms reset(destruct)=%lldms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(rr_t1 - rr_t0).count(),
                 std::chrono::duration_cast<std::chrono::milliseconds>(rr_t2 - rr_t1).count(),
                 std::chrono::duration_cast<std::chrono::milliseconds>(rr_t3 - rr_t2).count());

    /* 5. Let CMA settle after releasing all buffers. */
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    /* 6. Create fresh MediaLibrary instance and initialize with patched config. */
    auto rr_t4 = Clock::now();
    auto ml_exp = MediaLibrary::create();
    if (!ml_exp)
    {
        HAL_LOG_ERROR("hailo15_media: rotation_full_reinit: MediaLibrary::create() failed");
        return HAL_ERROR;
    }
    priv->media_lib = ml_exp.value();
    auto rr_t5 = Clock::now();
    priv->media_lib->set_override_persistent_settings(true);

    const bool has_backup = !priv->medialib_default_backup_folder.empty();
    if (has_backup)
    {
        priv->media_lib->set_default_backup_folder_path(priv->medialib_default_backup_folder);
        sanitize_profile_backups_on_disk(priv->medialib_default_backup_folder);
    }

    /* Do NOT restore from backup during rotation reinit — the patched JSON already
     * contains the correct rotated dimensions; restoring from backup would overwrite
     * them with the pre-rotation values. */
    media_library_return ini = priv->media_lib->initialize(patched_json, false);
    auto rr_t6 = Clock::now();
    if (ini != MEDIA_LIBRARY_SUCCESS)
    {
        HAL_LOG_ERROR("hailo15_media: rotation_full_reinit: initialize() failed (%d)", static_cast<int>(ini));
        return hailo15_ml_err(ini);
    }
    HAL_LOG_INFO("hailo15_media: rotation_full_reinit: initialize() succeeded");
    HAL_LOG_INFO("[TIMING] rotation_reinit: rebuild timing create=%lldms initialize=%lldms (total shutdown->ready=%lldms, profiles_json=%zub)",
                 std::chrono::duration_cast<std::chrono::milliseconds>(rr_t5 - rr_t4).count(),
                 std::chrono::duration_cast<std::chrono::milliseconds>(rr_t6 - rr_t5).count(),
                 std::chrono::duration_cast<std::chrono::milliseconds>(rr_t6 - rr_t0).count(),
                 patched_json.size());

    /* 7. Re-register pipeline state callback.
     * v1.12.0: single-subscriber — re-subscribe replaces the previous callback (no unsubscribe needed). */
    (void)priv->media_lib->subscribe_to_pipeline_state_change(
        [](media_library_pipeline_state_t state) {
            HAL_LOG_INFO("hailo15_media: MediaLibrary pipeline state -> %s (%d)",
                         ml_pipeline_state_str(state), static_cast<int>(state));
        });

    /* 8. Update profile names list. */
    hailo15_parse_profile_names_from_config_json(patched_json, &priv->profile_names);

    /* 9. Rebuild HAL video/codec contexts for the new medialib. */
    destroy_contexts(priv, hm);
    int br = build_contexts(priv, hm);
    if (br != HAL_OK)
    {
        HAL_LOG_ERROR("hailo15_media: rotation_full_reinit: build_contexts() failed (%d)", br);
        return br;
    }
    priv->last_frontend_stream_ids = priv->frontend_stream_ids;
    priv->last_encoder_stream_ids = priv->encoder_stream_ids;

    /* 10. Refresh OSD layout from the new profile. */
    {
        auto osd_prof_exp = priv->media_lib->get_current_profile();
        if (osd_prof_exp)
            refresh_osd_layout_from_profile(priv, osd_prof_exp.value());
    }

    /* 11. Sync image_config from the new profile so get_current_config() is correct. */
    {
        auto prof_exp = priv->media_lib->get_current_profile();
        if (prof_exp)
        {
            const auto &iq = prof_exp.value().iq_settings;
            hm->config.image_config.dewarp = iq.dewarp.enabled;
            hm->config.image_config.grayscale = iq.grayscale.enabled;
        }
        hm->config.image_config = *cfg;
    }

    /* 12. Update hm->config pointers to stored strings. */
    hm->config.config_json = priv->stored_config_json.c_str();

    /* 13. Apply non-rotation image settings (flip, zoom, dewarp, etc.) via set_override_parameters. */
    {
        auto prof_exp = priv->media_lib->get_current_profile();
        if (prof_exp)
        {
            config_profile_t p = prof_exp.value();
            p.application_settings.flip.enabled = (cfg->flip_direction != HAL_FLIP_DIRECTION_NONE);
            p.application_settings.flip.direction = static_cast<flip_direction_t>(cfg->flip_direction);
            p.application_settings.digital_zoom.enabled = cfg->digital_zoom;
            if (cfg->digital_zoom)
            {
                p.application_settings.digital_zoom.mode = DIGITAL_ZOOM_MODE_MAGNIFICATION;
                p.application_settings.digital_zoom.magnification = static_cast<float>(cfg->digital_zoom_value);
            }
            p.iq_settings.dewarp.enabled = cfg->dewarp;
            p.stabilizer_settings.dis.enabled = cfg->dis;
            p.stabilizer_settings.eis.enabled = cfg->eis;
            p.iq_settings.grayscale.enabled = cfg->grayscale;

            /* Recalculate OSD for new dimensions. */
            HalRotationAngle new_rot = cfg->rotation_angle;
            for (auto &kv : p.encoded_output_streams)
            {
                uint32_t ew = 0, eh = 0;
                std::visit([&](const auto &enc) { ew = enc.input_stream.width; eh = enc.input_stream.height; },
                           kv.second.encoding);
                hailo15::osd_ml::recalculate_osd_on_layout_change(priv, kv.first, ew, eh, new_rot);
            }

            media_library_return r = priv->media_lib->set_override_parameters(p);
            if (r != MEDIA_LIBRARY_SUCCESS)
            {
                HAL_LOG_WARNING("hailo15_media: rotation_full_reinit: set_override_parameters for extras failed (%d)",
                                static_cast<int>(r));
            }
        }
    }

    hailo15::video_ml::refresh_all_context_configs(priv, hm);

    /* 14. Ensure pipeline is started. set_override_parameters() may not start
     * the pipeline when it detects no material changes vs the current profile.
     * An explicit start guarantees the pipeline is running. */
    {
        auto pst = priv->media_lib->get_pipeline_state();
        if (pst != media_library_pipeline_state_t::PIPELINE_STATE_RUNNING)
        {
            HAL_LOG_INFO("hailo15_media: rotation_full_reinit: starting pipeline (state=%d)", static_cast<int>(pst));
            media_library_return sr = priv->media_lib->start_pipeline();
            if (sr != MEDIA_LIBRARY_SUCCESS)
            {
                HAL_LOG_ERROR("hailo15_media: rotation_full_reinit: start_pipeline failed (%d)", static_cast<int>(sr));
            }
        }
    }

    /* 15. Connect HAL bridge callbacks (encoder + frontend) to the new medialib objects. */
    {
        priv->encoder_auto_feed_default = true;
        priv->encoder_auto_feed_by_stream.clear();
        int ce = connect_encoders(priv);
        if (ce != HAL_OK)
        {
            HAL_LOG_WARNING("hailo15_media: rotation_full_reinit: connect_encoders failed (%d)", ce);
        }
        int cf = connect_frontend(priv);
        if (cf != HAL_OK)
        {
            HAL_LOG_WARNING("hailo15_media: rotation_full_reinit: connect_frontend failed (%d)", cf);
        }
    }

    HAL_LOG_INFO("hailo15_media: rotation_full_reinit: complete");
    return HAL_REINIT_PERFORMED;
}

static int hailo15_media_dynamic_change_image_config(void *media_ctx, const HalMediaImageConfig *cfg)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    if (!priv || !priv->media_lib || !cfg || !hm)
    {
        return HAL_ERR_INVALID_ARG;
    }

    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        return HAL_ERROR;
    }
    config_profile_t p = prof_exp.value();

    const bool prev_portrait = is_portrait_rotation(p.application_settings.rotation.effective_value());
    const bool new_portrait = is_portrait_rotation(static_cast<rotation_angle_t>(cfg->rotation_angle));

    p.application_settings.rotation.enabled = (cfg->rotation_angle != HAL_ROTATION_ANGLE_0);
    p.application_settings.rotation.angle = static_cast<rotation_angle_t>(cfg->rotation_angle);

    /* Swap encoder width/height when transitioning between landscape and portrait.
     * This matches the Hailo rotation_example pattern — medialib's internal
     * update_encoder_streams_for_rotation() will see that the dimensions already
     * match the target orientation and skip its own swap, preventing a double-swap.
     *
     * For large resolutions (any dimension > 2688), the normal set_override_parameters()
     * path often fails due to CMA DMA buffer fragmentation — the medialib pipeline restart
     * (stop→start) doesn't release enough CMA for the larger rotated buffer pools.
     * In that case we fall back to a full medialib shutdown/reinitialize cycle which
     * releases ALL DMA resources before allocating the rotated pipeline from a clean state. */
    if (prev_portrait != new_portrait)
    {
        HAL_LOG_INFO("hailo15_media: rotation transition (prev_portrait=%d, new_portrait=%d), swapping dimensions",
                     prev_portrait, new_portrait);

        /* Detect large encoders, or a dense DSP transform stack, that need the
         * full-reinit path. A dimension-swap rotation (we are inside the
         * prev_portrait != new_portrait branch) layered on top of dewarp + (DIS
         * or EIS) wedges the in-place set_override_parameters path: the light
         * medialib stop->restart does not fully tear down the DSP dewarp/DIS/EIS
         * stages, so they desync with the new geometry -> the FE output callback's
         * add_buffer() is rejected -> encoder starvation -> /media black screen
         * (reproduced on 93.213). rotation_full_reinit rebuilds all DSP stages
         * from a clean medialib and avoids the wedge. Rotation alone (no DSP) is
         * unaffected and still takes the fast in-place path below. */
        const bool dense_dsp_stack = (cfg->dewarp && (cfg->dis || cfg->eis));
        bool needs_full_reinit = dense_dsp_stack;
        for (auto &kv : p.encoded_output_streams)
        {
            uint32_t w = 0, h = 0;
            std::visit([&](auto &enc) { w = enc.input_stream.width; h = enc.input_stream.height; },
                       kv.second.encoding);
            auto pd = priv->encoder_patched_dims.find(kv.first);
            if (pd != priv->encoder_patched_dims.end())
            {
                w = pd->second.first;
                h = pd->second.second;
            }
            // 4K (3840x2160) rotation uses the fast in-place set_override_parameters
            // path. Verified on 93.72 (2026-08-05): no OOM, no resolution regression.
            // If a future resolution exceeds 4096, or CMA fragments and the in-place
            // restart OOMs, the BUFFER_ALLOCATION_ERROR fallback below still recovers
            // via rotation_full_reinit. Keep 4096 (not the old 2688) so 4K stays fast.
            if (w > 4096 || h > 4096)
            {
                needs_full_reinit = true;
                break;
            }
        }

        if (needs_full_reinit)
        {
            return rotation_full_reinit(media_ctx, hm, priv, cfg);
        }

        for (auto &kv : p.encoded_output_streams)
        {
            auto pd = priv->encoder_patched_dims.find(kv.first);
            if (pd != priv->encoder_patched_dims.end())
            {
                uint32_t cw = 0, ch = 0;
                std::visit([&](auto &enc) { cw = enc.input_stream.width; ch = enc.input_stream.height; },
                           kv.second.encoding);
                if (cw != pd->second.first || ch != pd->second.second)
                {
                    HAL_LOG_INFO("hailo15_media: rotation: correcting encoder '%s' from profile %ux%u to patched %ux%u",
                                 kv.first.c_str(), cw, ch, pd->second.first, pd->second.second);
                    std::visit([&](auto &enc) {
                        enc.input_stream.width = pd->second.first;
                        enc.input_stream.height = pd->second.second;
                    }, kv.second.encoding);
                }
            }

            std::visit([](auto &enc) { std::swap(enc.input_stream.width, enc.input_stream.height); },
                       kv.second.encoding);
        }

        for (auto &res : p.application_settings.application_input_streams.resolutions)
        {
            auto pd = priv->encoder_patched_dims.find(res.stream_id);
            if (pd != priv->encoder_patched_dims.end())
            {
                if (res.dimensions.destination_width != pd->second.first ||
                    res.dimensions.destination_height != pd->second.second)
                {
                    HAL_LOG_INFO("hailo15_media: rotation: correcting app_stream '%s' from %ux%u to %ux%u",
                                 res.stream_id.c_str(),
                                 res.dimensions.destination_width, res.dimensions.destination_height,
                                 pd->second.first, pd->second.second);
                    res.dimensions.destination_width = pd->second.first;
                    res.dimensions.destination_height = pd->second.second;
                }
            }
        }
    }

    p.application_settings.flip.enabled = (cfg->flip_direction != HAL_FLIP_DIRECTION_NONE);
    p.application_settings.flip.direction = static_cast<flip_direction_t>(cfg->flip_direction);

    p.application_settings.digital_zoom.enabled = cfg->digital_zoom;
    if (cfg->digital_zoom)
    {
        p.application_settings.digital_zoom.mode = DIGITAL_ZOOM_MODE_MAGNIFICATION;
        p.application_settings.digital_zoom.magnification = static_cast<float>(cfg->digital_zoom_value);
        clear_encoder_privacy_masks(p);
    }

    p.iq_settings.dewarp.enabled = cfg->dewarp;
    p.stabilizer_settings.dis.enabled = cfg->dis;
    p.stabilizer_settings.eis.enabled = cfg->eis;
    p.iq_settings.grayscale.enabled = cfg->grayscale;

    if (cfg->privacy_mask && !cfg->digital_zoom)
    {
        apply_hal_privacy_to_profile(p, priv, cfg);
    }
    else if (!cfg->privacy_mask)
    {
        clear_encoder_privacy_masks(p);
    }

    /* Rotation / resolution changes require OSD overlay coordinate recalculation.
     * Rescale font sizes proportionally to new encoder width (webserver-aligned behaviour). */
    {
        HalRotationAngle new_rot = cfg->rotation_angle;
        for (auto &kv : p.encoded_output_streams)
        {
            uint32_t ew = 0;
            uint32_t eh = 0;
            std::visit([&](const auto &enc) { ew = enc.input_stream.width; eh = enc.input_stream.height; },
                       kv.second.encoding);
            hailo15::osd_ml::recalculate_osd_on_layout_change(priv, kv.first, ew, eh, new_rot);
        }
    }

    const auto transform_t0 = std::chrono::steady_clock::now();
    media_library_return r = priv->media_lib->set_override_parameters(p);
    HAL_LOG_INFO("[TIMING] set_transform: set_override_parameters=%lldms (in-place rotation/flip path)",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - transform_t0).count());
    if (r == MEDIA_LIBRARY_BUFFER_ALLOCATION_ERROR)
    {
        /* Large-res transform (flip/dewarp) OOM'd on a fragmented CMA — the
         * light stop→start restart inside set_override_parameters cannot free
         * enough contiguous DMA to re-allocate the 4K buffer pools. Fall back
         * to a full medialib shutdown/recreate (rotation_full_reinit), which
         * releases ALL DMA resources and then re-applies the whole transform
         * (rotation + flip + dewarp + dis + eis + grayscale) on a clean
         * medialib where the allocation succeeds. This is the same memory-safe
         * path already used for large-res rotation (see line ~4259). */
        HAL_LOG_WARNING("hailo15_media: set_override_parameters OOM (r=%d) on image "
                        "transform; falling back to rotation_full_reinit", r);
        return rotation_full_reinit(media_ctx, hm, priv, cfg);
    }
    if (r != MEDIA_LIBRARY_SUCCESS)
    {
        return hailo15_ml_err(r);
    }
    hailo15::video_ml::refresh_all_context_configs(priv, static_cast<HalMediaContext *>(media_ctx));
    /* Keep HalMediaConfig.image_config in sync so get_current_config()
     * reflects the latest runtime image configuration. */
    hm->config.image_config = *cfg;
    return HAL_OK;
}

static int hailo15_media_set_encoder_auto_feed(void *media_ctx, bool enable)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    priv->encoder_auto_feed_default = enable;
    priv->encoder_auto_feed_by_stream.clear();
    return HAL_OK;
}

static int hailo15_media_get_encoder_auto_feed(void *media_ctx, bool *enable_out)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv || !enable_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    *enable_out = priv->encoder_auto_feed_default;
    return HAL_OK;
}

static int hailo15_media_set_encoder_auto_feed_for_stream(void *media_ctx, const char *stream_id, bool enable)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv || !stream_id || stream_id[0] == '\0')
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    priv->encoder_auto_feed_by_stream[stream_id] = enable;
    return HAL_OK;
}

static int hailo15_media_attach_frame_analytics(void *media_ctx, HalFrameBuffer *frame,
                                                const HalFrameDetection *dets, uint32_t det_count,
                                                const HalFrameSegmentation *segs, uint32_t seg_count)
{
    (void)media_ctx;
    if (!frame || !frame->priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    auto *fp = static_cast<Hailo15FramePriv *>(frame->priv);
    auto ml_buf = fp->ml_buf;
    if (!ml_buf)
    {
        return HAL_ERR_INVALID_STATE;
    }

    /* Both counts 0 (or NULL arrays) clears any previously attached metadata (per-frame replace). */
    if ((det_count == 0U || dets == nullptr) && (seg_count == 0U || segs == nullptr))
    {
        ml_buf->m_analytics_metadata = nullptr;
        return HAL_OK;
    }

    const uint32_t fw = ml_buf->buffer_data ? ml_buf->buffer_data->width : 0U;
    const uint32_t fh = ml_buf->buffer_data ? ml_buf->buffer_data->height : 0U;
    if (fw == 0U || fh == 0U)
    {
        return HAL_ERR_INVALID_STATE;
    }

    auto md = std::make_shared<AnalyticsMetadata>();

    /* Detections → solid-block masks (bbox only). */
    if (det_count > 0U && dets != nullptr)
    {
        auto dvec = std::make_shared<std::vector<LabeledDetection>>();
        dvec->reserve(det_count);
        for (uint32_t i = 0U; i < det_count; ++i)
        {
            if (dets[i].label[0] == '\0')
            {
                continue;
            }
            LabeledDetection d;
            d.label = dets[i].label;
            const float cx = std::clamp(dets[i].x, 0.0f, 1.0f);
            const float cy = std::clamp(dets[i].y, 0.0f, 1.0f);
            const float cw = std::clamp(dets[i].w, 0.0f, 1.0f);
            const float ch = std::clamp(dets[i].h, 0.0f, 1.0f);
            d.detection.x_min = cx * static_cast<float>(fw);
            d.detection.y_min = cy * static_cast<float>(fh);
            d.detection.x_max = (cx + cw) * static_cast<float>(fw);
            d.detection.y_max = (cy + ch) * static_cast<float>(fh);
            d.detection.score = dets[i].score;
            d.detection.class_id = 0;
            dvec->push_back(std::move(d));
        }
        md->m_detections = dvec;
    }

    /* Segmentations → irregular per-pixel masks. The bytemask is copied into HAL-owned storage
     * pinned via m_source_keepalives so it stays valid through the synchronous DSP blend call. */
    if (seg_count > 0U && segs != nullptr)
    {
        auto svec = std::make_shared<std::vector<LabeledSemanticMask>>();
        svec->reserve(seg_count);
        for (uint32_t i = 0U; i < seg_count; ++i)
        {
            const auto &s = segs[i];
            if (s.label[0] == '\0' || s.mask_w == 0U || s.mask_h == 0U || !s.mask)
            {
                continue;
            }
            const size_t bytes = static_cast<size_t>(s.mask_w) * static_cast<size_t>(s.mask_h);
            auto buf = std::make_shared<std::vector<uint8_t>>(s.mask, s.mask + bytes);
            LabeledSemanticMask lm;
            lm.label = s.label;
            lm.mask.class_id = s.class_id;
            lm.mask.width = s.mask_w;
            lm.mask.height = s.mask_h;
            lm.mask.transparency = 0.0f;
            lm.mask.mask_size = bytes;
            lm.mask.mask = buf->data();
            /* detection_x/y/width/height are in encoded-frame PIXEL space (the blender's producer
             * contract — see hailo_analytics encoder_stage.cpp make_labeled_mask). Convert the
             * caller's normalized [0..1] bbox to pixels using the encoded frame dims. */
            const float px = std::clamp(s.x, 0.0f, 1.0f);
            const float py = std::clamp(s.y, 0.0f, 1.0f);
            const float pw = std::clamp(s.w, 0.0f, 1.0f);
            const float ph = std::clamp(s.h, 0.0f, 1.0f);
            lm.mask.detection_x = px * static_cast<float>(fw);
            lm.mask.detection_y = py * static_cast<float>(fh);
            lm.mask.detection_width = pw * static_cast<float>(fw);
            lm.mask.detection_height = ph * static_cast<float>(fh);
            svec->push_back(std::move(lm));
            md->m_source_keepalives.emplace_back(std::static_pointer_cast<void>(buf));
        }
        md->m_semantic_segmentation = svec;
    }

    ml_buf->m_analytics_metadata = md;
    return HAL_OK;
}

static int hailo15_media_get_encoder_auto_feed_for_stream(void *media_ctx, const char *stream_id, bool *enable_out)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv || !stream_id || stream_id[0] == '\0' || !enable_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    std::lock_guard<std::recursive_mutex> lock(priv->mutex);
    const auto it = priv->encoder_auto_feed_by_stream.find(stream_id);
    if (it != priv->encoder_auto_feed_by_stream.end())
    {
        *enable_out = it->second;
    }
    else
    {
        *enable_out = priv->encoder_auto_feed_default;
    }
    return HAL_OK;
}

static std::string hailo15_generate_pipeline_config_json(
    const std::string &stored_json, const HalPipelineReconfig *reconfig,
    const std::string &active_profile_name);
static int reinit_media_library_on_stream_change(HalMediaContext *hm,
                                                  Hailo15MediaPriv *priv,
                                                  const std::string &new_config_json,
                                                  bool skip_encoder_overrides = false);

static int hailo15_media_override_stream_params(void *media_ctx, const HalStreamOverrideBatch *batch)
{
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!priv || !priv->media_lib || !batch || !batch->streams || batch->stream_count == 0)
    {
        return HAL_ERR_INVALID_ARG;
    }
    /* Do not hold priv->mutex across MediaLibrary calls: ML may invoke callbacks that take this lock. */
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        HAL_LOG_ERROR("hailo15_media: override_stream_params: get_current_profile failed");
        return HAL_ERROR;
    }
    config_profile_t p = prof_exp.value();
    // Save original profile for restart detection (framerate changes need buffer pool reallocation).
    const config_profile_t prev_p = p;

    for (uint32_t i = 0; i < batch->stream_count; i++)
    {
        const HalStreamOverride &ov = batch->streams[i];
        const std::string sid(ov.stream_id);

        // --- Override frontend input stream ---
        if (ov.input_width > 0 || ov.input_height > 0 || ov.input_framerate > 0)
        {
            for (auto &res : p.application_settings.application_input_streams.resolutions)
            {
                if (res.stream_id == sid)
                {
                    if (ov.input_width > 0)
                        res.dimensions.destination_width = ov.input_width;
                    if (ov.input_height > 0)
                        res.dimensions.destination_height = ov.input_height;
                    if (ov.input_framerate > 0)
                    {
                        res.framerate = ov.input_framerate;
                    }
                    break;
                }
            }
        }

        // --- Override encoded output stream ---
        auto enc_it = p.encoded_output_streams.find(sid);
        if (enc_it == p.encoded_output_streams.end())
        {
            HAL_LOG_WARNING("hailo15_media: override_stream_params: stream '%s' not found in encoded_output_streams, skipping encoder overrides",
                         sid.c_str());
            continue;
        }

        config_encoded_output_stream_t &eos = enc_it->second;

        // hailo_encoder_config_t is inside the variant
        std::visit([&](auto &enc) {
            using T = std::decay_t<decltype(enc)>;
            if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
            {
                // Encoder input dimensions / framerate
                if (ov.encoder_width > 0)
                    enc.input_stream.width = ov.encoder_width;
                if (ov.encoder_height > 0)
                    enc.input_stream.height = ov.encoder_height;
                if (ov.encoder_framerate > 0)
                    enc.input_stream.framerate = ov.encoder_framerate;

                // Bitrate
                if (ov.encoder_bitrate > 0)
                    enc.rate_control.bitrate.target_bitrate = ov.encoder_bitrate;

                // GOP — only set intra_pic_rate (keyframe interval).
                // gop_size is the Hantro B-frame structure (valid: 1-8) and must
                // NOT be overridden with an arbitrary keyframe interval value, or
                // the encoder malfunctions and buffer pools exhaust.
                if (ov.encoder_gop > 0)
                {
                    enc.rate_control.intra_pic_rate = ov.encoder_gop;
                }

                // Codec
                if (ov.encoder_codec[0] != '\0')
                {
                    std::string codec_str(ov.encoder_codec);
                    if (codec_str == "h265" || codec_str == "HEVC")
                        enc.output_stream.codec = CODEC_TYPE_HEVC;
                    else
                        enc.output_stream.codec = CODEC_TYPE_H264;
                }
            }
            // jpeg_encoder_config_t: skip encoder overrides
        }, eos.encoding);
    }

    // Detect framerate changes — DSP buffer pools are sized per-fps (e.g. pool3840x2160_30_y).
    // set_override_parameters only does an in-place restart that leaves multi_resize timestamp
    // accumulators stale, causing buffer pool exhaustion.  Do a full ML destroy+recreate instead.
    const bool full_reinit_required =
        ml_stream_restart_required(prev_p, p) || encoder_input_layout_changed(prev_p, p);
    HAL_LOG_INFO("hailo15_media: override_stream_params: full_reinit_required=%d, "
                 "prev_framerate=%u, new_framerate=%u",
                 full_reinit_required,
                 prev_p.application_settings.application_input_streams.resolutions.empty() ? 0 :
                     prev_p.application_settings.application_input_streams.resolutions[0].framerate,
                 p.application_settings.application_input_streams.resolutions.empty() ? 0 :
                     p.application_settings.application_input_streams.resolutions[0].framerate);

    if (full_reinit_required)
    {
        HAL_LOG_INFO("hailo15_media: override_stream_params: stream buffer layout change detected, full reinit");
        // Convert HalStreamOverrideBatch to HalPipelineReconfig for JSON generation
        HalPipelineReconfig fake_reconfig{};
        HalPipelineStreamConfig stream_cfgs[4]{};
        fake_reconfig.streams = stream_cfgs;
        fake_reconfig.stream_count = batch->stream_count;
        for (uint32_t i = 0; i < batch->stream_count && i < 4; i++)
        {
            const auto &ov = batch->streams[i];
            auto &sc = stream_cfgs[i];
            strncpy(sc.stream_id, ov.stream_id, sizeof(sc.stream_id) - 1);
            sc.input_width = ov.input_width;
            sc.input_height = ov.input_height;
            sc.input_framerate = ov.input_framerate;
            sc.encoder_width = ov.encoder_width;
            sc.encoder_height = ov.encoder_height;
            sc.encoder_framerate = ov.encoder_framerate;
            sc.encoder_bitrate = ov.encoder_bitrate;
            sc.encoder_gop = ov.encoder_gop;
            strncpy(sc.codec, ov.encoder_codec, sizeof(sc.codec) - 1);
        }
        std::string new_json = hailo15_generate_pipeline_config_json(priv->stored_config_json, &fake_reconfig, p.name);
        if (new_json.empty())
        {
            HAL_LOG_ERROR("hailo15_media: override_stream_params: failed to generate new config JSON");
            return HAL_ERROR;
        }
        int rc = reinit_media_library_on_stream_change(static_cast<HalMediaContext *>(media_ctx), priv, new_json);
        if (rc == HAL_OK)
        {
            HAL_LOG_INFO("hailo15_media: override_stream_params: reinit success (%u streams)", batch->stream_count);
        }
        return rc;
    }

    const int rc = apply_profile_override_and_refresh(
        static_cast<HalMediaContext *>(media_ctx), priv, p, "override_stream_params");
    if (rc == HAL_OK)
    {
        HAL_LOG_INFO("hailo15_media: override_stream_params: applied %u stream overrides", batch->stream_count);
    }
    return rc;
}

/**
 * Generate a new medialib config JSON from stored config + requested streams.
 *
 * The stored config is the original webserver_medialib_config.json.  We modify
 * the current profile's application_settings (input streams) and
 * encoded_output_streams (encoders) to match the requested stream layout,
 * then write the config inline and re-initialize MediaLibrary.
 */
static std::string hailo15_generate_pipeline_config_json(
    const std::string &stored_json,
    const HalPipelineReconfig *reconfig,
    const std::string &active_profile_name)
{
    using json = nlohmann::json;

    json cfg = json::parse(stored_json);

    /* Preserve the runtime profile across full MediaLibrary reinit. */
    std::string prof_name = active_profile_name;
    if (prof_name.empty() && cfg.contains("default_profile"))
    {
        prof_name = cfg["default_profile"].get<std::string>();
    }
    if (!prof_name.empty())
    {
        cfg["default_profile"] = prof_name;
    }

    /* Find the profile entry — could be in "profiles" array or object */
    json *prof_entry = nullptr;
    if (cfg.contains("profiles"))
    {
        auto &profs = cfg["profiles"];
        if (profs.is_array())
        {
            for (auto &p : profs)
            {
                if (p.contains("name") && p["name"] == prof_name)
                {
                    prof_entry = &p;
                    break;
                }
            }
        }
        else if (profs.is_object())
        {
            if (profs.contains(prof_name))
            {
                prof_entry = &profs[prof_name];
            }
        }
    }

    if (!prof_entry)
    {
        HAL_LOG_ERROR("hailo15_media: generate_config: cannot find profile '%s' in stored JSON", prof_name.c_str());
        return {};
    }

    /* If profile is a file reference, load the profile JSON for editing.
     * The main config only has {config_file, name} — application_settings and
     * encoded_output_streams live inside the profile file. */
    json profile_data;
    json *active_prof = prof_entry;
    if (prof_entry->contains("config_file") && (*prof_entry)["config_file"].is_string())
    {
        std::string prof_path = (*prof_entry)["config_file"].get<std::string>();
        std::ifstream pf(prof_path);
        if (pf.is_open())
        {
            pf >> profile_data;
            pf.close();
            active_prof = &profile_data;
            HAL_LOG_INFO("hailo15_media: generate_config: loaded profile file '%s'", prof_path.c_str());
        }
        else
        {
            HAL_LOG_WARNING("hailo15_media: generate_config: cannot open profile file '%s'", prof_path.c_str());
        }
    }

    /* --- Modify application_input_streams.resolutions ---
     * application_settings can be inline or a file-path reference.
     * For file references, load the file, patch it, write it back.
     * Patch in-place: find matching stream_id entries and update their
     * framerate/width/height. Do NOT replace the entire resolutions array.
     */
    auto patch_app_settings = [&](json &app_obj) {
        if (app_obj.contains("application_input_streams"))
        {
            auto &input_streams = app_obj["application_input_streams"];
            if (input_streams.contains("resolutions") && input_streams["resolutions"].is_array())
            {
                auto &res_arr = input_streams["resolutions"];
                for (uint32_t i = 0; i < reconfig->stream_count; i++)
                {
                    const HalPipelineStreamConfig &sc = reconfig->streams[i];
                    std::string sid(sc.stream_id);
                    for (auto &r : res_arr)
                    {
                        if (r.contains("stream_id") && r["stream_id"] == sid)
                        {
                            if (sc.input_framerate > 0)  r["framerate"] = sc.input_framerate;
                            if (sc.input_width > 0)       r["width"]     = sc.input_width;
                            if (sc.input_height > 0)      r["height"]    = sc.input_height;
                            break;
                        }
                    }
                }
            }
        }
    };

    if (active_prof->contains("application_settings"))
    {
        auto &as = (*active_prof)["application_settings"];
        if (as.is_object())
        {
            patch_app_settings(as);
        }
        else if (as.is_string())
        {
            /* File-path reference — load, patch, write back */
            std::string as_path = as.get<std::string>();
            std::ifstream asf(as_path);
            if (asf.is_open())
            {
                json as_data;
                asf >> as_data;
                asf.close();
                patch_app_settings(as_data);

                /* Update content_hash using medialib-compatible computation */
                auto as_hash = compute_medialib_content_hash(as_data);
                if (as_hash)
                    as_data["metadata"]["content_hash"] = *as_hash;

                std::ofstream asof(as_path);
                if (asof.is_open())
                {
                    asof << as_data.dump(4);
                    asof.close();
                    HAL_LOG_INFO("hailo15_media: generate_config: patched application_settings file '%s'",
                                 as_path.c_str());
                }
            }
            else
            {
                HAL_LOG_WARNING("hailo15_media: generate_config: cannot open application_settings file '%s'",
                                as_path.c_str());
            }
        }
    }

    /* --- Modify encoded_output_streams (inline only) ---
     * File-path encoder references are NOT modified on disk — causes config corruption.
     * Codec changes use remove_codec_stream + add_codec_stream APIs instead.
     */
    if (active_prof->contains("encoded_output_streams") && (*active_prof)["encoded_output_streams"].is_array())
    {
        auto &eos_array = (*active_prof)["encoded_output_streams"];

        /* Track which stream_ids exist in the profile to detect missing ones */
        std::set<std::string> existing_streams;
        for (const auto &e : eos_array)
        {
            if (e.contains("stream_id") && e["stream_id"].is_string())
                existing_streams.insert(e["stream_id"].get<std::string>());
        }

        for (uint32_t i = 0; i < reconfig->stream_count; i++)
        {
            const HalPipelineStreamConfig &sc = reconfig->streams[i];
            std::string sid(sc.stream_id);

            /* Match by stream_id: find the encoded_output_streams entry whose
             * stream_id matches the reconfig stream (sink0, sink1, etc.) */
            json *entry_ptr = nullptr;
            for (auto &e : eos_array)
            {
                if (e.contains("stream_id") && e["stream_id"] == sid)
                {
                    entry_ptr = &e;
                    break;
                }
            }

            /* If stream doesn't exist in profile, inject it using an existing
             * encoder file as template (same approach as apply_encoder_overrides). */
            if (!entry_ptr)
            {
                if (eos_array.empty()) continue;
                auto &template_entry = eos_array[0];
                if (!template_entry.contains("encoding") || !template_entry["encoding"].is_string()) continue;

                std::string template_enc_path = template_entry["encoding"].get<std::string>();
                std::ifstream tef(template_enc_path);
                if (!tef.is_open()) continue;
                json template_enc;
                tef >> template_enc;
                tef.close();

                json *enc_root = &template_enc;
                if (template_enc.contains("encoding") && template_enc["encoding"].is_object())
                    enc_root = &template_enc["encoding"];
                if (!enc_root->contains("input_stream")) continue;

                /* Patch dimensions from reconfig */
                auto &inp = (*enc_root)["input_stream"];
                if (sc.encoder_width > 0)  inp["width"] = sc.encoder_width;
                if (sc.encoder_height > 0) inp["height"] = sc.encoder_height;
                if (sc.input_framerate > 0) inp["framerate"] = sc.input_framerate;

                /* Patch codec, bitrate, gop */
                std::string codec_type = (std::string(sc.codec) == "h265" || std::string(sc.codec) == "HEVC")
                                             ? "CODEC_TYPE_HEVC" : "CODEC_TYPE_H264";
                if (enc_root->contains("hailo_encoder"))
                {
                    auto &he = (*enc_root)["hailo_encoder"];
                    if (he.contains("config") && he["config"].contains("output_stream"))
                        he["config"]["output_stream"]["codec"] = codec_type;
                    if (sc.encoder_bitrate > 0 && he.contains("rate_control") && he["rate_control"].contains("bitrate"))
                        he["rate_control"]["bitrate"]["target_bitrate"] = sc.encoder_bitrate;
                    if (sc.encoder_gop > 0 && he.contains("rate_control"))
                        he["rate_control"]["intra_pic_rate"] = sc.encoder_gop;
                    if (sc.encoder_gop > 0 && sc.encoder_gop <= 8 && he.contains("gop_config"))
                        he["gop_config"]["gop_size"] = sc.encoder_gop;
                }

                /* Write injected encoder file */
                if (template_enc.contains("metadata") && template_enc["metadata"].is_object())
                {
                    auto enc_hash = compute_medialib_content_hash(template_enc);
                    if (enc_hash) template_enc["metadata"]["content_hash"] = *enc_hash;
                }
                std::string tmp_path = std::string("/tmp/encoder_reconfig_") + sid + ".json";
                std::ofstream of(tmp_path);
                if (!of.is_open()) continue;
                of << template_enc.dump(4);
                of.close();

                /* Build eos entry with osd/masking from same directory */
                json new_entry;
                new_entry["stream_id"] = sid;
                new_entry["encoding"] = tmp_path;

                std::string dir = template_enc_path.substr(0, template_enc_path.rfind('/') + 1);
                std::string osd_candidate = dir + "osd_" + sid + ".json";
                std::ifstream test_osd(osd_candidate);
                if (test_osd.is_open()) { test_osd.close(); new_entry["osd"] = osd_candidate; }
                else
                {
                    std::string tmp_osd = std::string("/tmp/osd_default_") + sid + ".json";
                    json osd_data;
                    osd_data["version"] = "1.0.0";
                    osd_data["metadata"]["architecture"] = "hailo15h";
                    osd_data["osd"]["dateTime"] = json::array();
                    osd_data["osd"]["image"] = json::array();
                    osd_data["osd"]["text"] = json::array();
                    auto osd_hash = compute_medialib_content_hash(osd_data);
                    if (osd_hash) osd_data["metadata"]["content_hash"] = *osd_hash;
                    std::ofstream osd_of(tmp_osd);
                    if (osd_of.is_open())
                    {
                        osd_of << osd_data.dump(4);
                        osd_of.close();
                        new_entry["osd"] = tmp_osd;
                    }
                }

                std::string mask_candidate = dir + "masking_" + sid + ".json";
                std::ifstream test_mask(mask_candidate);
                if (test_mask.is_open()) { test_mask.close(); new_entry["masking"] = mask_candidate; }
                else
                {
                    std::string tmp_mask = std::string("/tmp/masking_default_") + sid + ".json";
                    json mask_data;
                    mask_data["version"] = "3.0.0";
                    mask_data["metadata"]["architecture"] = "hailo15h";
                    mask_data["masking"]["color_value"] = {0, 0, 0};
                    mask_data["masking"]["mask_type"] = "PIXELIZATION";
                    mask_data["masking"]["pixelization_size"] = 60;
                    auto mask_hash = compute_medialib_content_hash(mask_data);
                    if (mask_hash) mask_data["metadata"]["content_hash"] = *mask_hash;
                    std::ofstream mask_of(tmp_mask);
                    if (mask_of.is_open())
                    {
                        mask_of << mask_data.dump(4);
                        mask_of.close();
                        new_entry["masking"] = tmp_mask;
                    }
                }

                eos_array.push_back(new_entry);
                existing_streams.insert(sid);
                HAL_LOG_INFO("hailo15_media: generate_config: injected missing stream '%s' "
                             "(%ux%u@%u) -> %s",
                             sid.c_str(), sc.encoder_width, sc.encoder_height,
                             sc.input_framerate, tmp_path.c_str());
                continue;
            }
            auto &entry = *entry_ptr;

            std::string codec_type = (std::string(sc.codec) == "h265" || std::string(sc.codec) == "HEVC")
                                         ? "CODEC_TYPE_HEVC"
                                         : "CODEC_TYPE_H264";

            if (entry.contains("encoding") && entry["encoding"].is_object())
            {
                auto &enc = entry["encoding"];
                if (enc.contains("hailo_encoder"))
                {
                    auto &he = enc["hailo_encoder"];
                    if (he.contains("config") && he["config"].contains("output_stream"))
                    {
                        he["config"]["output_stream"]["codec"] = codec_type;
                        HAL_LOG_INFO("hailo15_media: generate_config: set codec=%s for inline stream[%u]",
                                     codec_type.c_str(), i);
                    }
                }
            }
            else if (entry.contains("encoding") && entry["encoding"].is_string())
            {
                /* File-path encoder reference — modify the file on disk.
                 * The pipeline will be fully re-initialized after this, so the
                 * updated file will be read during initialize(). */
                std::string enc_path = entry["encoding"].get<std::string>();
                HAL_LOG_INFO("hailo15_media: generate_config: patching encoder file '%s' for stream[%u]",
                             enc_path.c_str(), i);
                std::ifstream ef(enc_path);
                if (ef.is_open())
                {
                    json enc_cfg;
                    ef >> enc_cfg;
                    ef.close();

                    if (enc_cfg.contains("encoding"))
                    {
                        auto &enc_obj = enc_cfg["encoding"];

                        /* input_stream: framerate, width, height */
                        if (enc_obj.contains("input_stream"))
                        {
                            auto &inp = enc_obj["input_stream"];
                            if (sc.input_framerate > 0)  inp["framerate"] = sc.input_framerate;
                            if (sc.encoder_width > 0)     inp["width"]     = sc.encoder_width;
                            if (sc.encoder_height > 0)    inp["height"]    = sc.encoder_height;
                        }

                        /* hailo_encoder config */
                        if (enc_obj.contains("hailo_encoder"))
                        {
                            auto &he = enc_obj["hailo_encoder"];
                            /* output_stream codec */
                            if (he.contains("config") && he["config"].contains("output_stream"))
                            {
                                he["config"]["output_stream"]["codec"] = codec_type;
                            }
                            /* rate_control: bitrate, intra_pic_rate (GOP) */
                            if (he.contains("rate_control"))
                            {
                                auto &rc = he["rate_control"];
                                if (sc.encoder_bitrate > 0 && rc.contains("bitrate"))
                                {
                                    rc["bitrate"]["target_bitrate"] = sc.encoder_bitrate;
                                }
                                if (sc.encoder_gop > 0)
                                {
                                    rc["intra_pic_rate"] = sc.encoder_gop;
                                }
                            }
                            /* gop_config: only write gop_size when the value is a valid Hantro
                             * B-frame hierarchy (1-8). Larger values are I-frame intervals
                             * already routed through intra_pic_rate above. */
                            if (sc.encoder_gop > 0 && sc.encoder_gop <= 8 && he.contains("gop_config"))
                            {
                                he["gop_config"]["gop_size"] = sc.encoder_gop;
                            }
                        }

                        /* Update content_hash using medialib-compatible computation */
                        auto enc_hash = compute_medialib_content_hash(enc_cfg);
                        if (enc_hash)
                            enc_cfg["metadata"]["content_hash"] = *enc_hash;

                        /* Write back — prefer original path; fall back to /tmp/
                         * if the device filesystem is read-only. */
                        auto write_enc_file = [&](const std::string &path) -> bool {
                            std::ofstream of(path);
                            if (!of.is_open()) return false;
                            of << enc_cfg.dump(4);
                            of.close();
                            return true;
                        };
                        if (write_enc_file(enc_path))
                        {
                            HAL_LOG_INFO("hailo15_media: generate_config: updated encoder file '%s' (framerate=%u, bitrate=%u, gop=%u, codec=%s)",
                                         enc_path.c_str(), sc.encoder_framerate, sc.encoder_bitrate, sc.encoder_gop, codec_type.c_str());
                        }
                        else
                        {
                            std::string tmp_path = std::string("/tmp/encoder_inject_") + sc.stream_id + ".json";
                            if (write_enc_file(tmp_path))
                            {
                                entry["encoding"] = tmp_path;
                                HAL_LOG_INFO("hailo15_media: generate_config: wrote fallback encoder file '%s' for stream '%s'",
                                             tmp_path.c_str(), sc.stream_id);
                            }
                            else
                            {
                                HAL_LOG_ERROR("hailo15_media: generate_config: cannot write encoder file '%s' or '%s'",
                                              enc_path.c_str(), tmp_path.c_str());
                            }
                        }
                    }
                }
                else
                {
                    HAL_LOG_WARNING("hailo15_media: generate_config: cannot open encoder file '%s'",
                                    enc_path.c_str());
                }
            }
        }
    }

    /* --- Align resolutions with encoded_output_streams ---
     * After injecting missing streams into encoded_output_streams, the
     * resolutions array in application_settings must match in size.
     * Inject resolution entries for any streams present in encoded_output_streams
     * but missing from resolutions; remove extras not in encoded_output_streams.
     */
    {
        /* Collect stream_ids from encoded_output_streams */
        std::set<std::string> eos_ids;
        if (active_prof->contains("encoded_output_streams") &&
            (*active_prof)["encoded_output_streams"].is_array())
        {
            for (const auto &eos : (*active_prof)["encoded_output_streams"])
            {
                if (eos.contains("stream_id") && eos["stream_id"].is_string())
                    eos_ids.insert(eos["stream_id"].get<std::string>());
            }
        }

        /* Lambda to align a single application_settings JSON object */
        auto align_resolutions = [&](json &app_obj) {
            if (!app_obj.contains("application_input_streams")) return;
            auto &input_streams = app_obj["application_input_streams"];
            if (!input_streams.contains("resolutions") || !input_streams["resolutions"].is_array()) return;

            auto &res_arr = input_streams["resolutions"];

            /* Collect existing resolution stream_ids and default pool_max_buffers */
            std::set<std::string> res_ids;
            uint32_t default_pool_buffers = 30;
            for (const auto &r : res_arr)
            {
                if (r.contains("stream_id") && r["stream_id"].is_string())
                    res_ids.insert(r["stream_id"].get<std::string>());
                if (r.contains("pool_max_buffers"))
                    default_pool_buffers = r["pool_max_buffers"].get<uint32_t>();
            }

            /* Build dimension lookup from reconfig streams */
            struct DimInfo { uint32_t w, h, fps; };
            std::map<std::string, DimInfo> dim_map;
            for (uint32_t i = 0; i < reconfig->stream_count; i++)
            {
                const HalPipelineStreamConfig &sc = reconfig->streams[i];
                if (sc.encoder_width > 0 && sc.encoder_height > 0 && sc.input_framerate > 0)
                    dim_map[sc.stream_id] = {sc.encoder_width, sc.encoder_height, sc.input_framerate};
            }

            /* Inject missing resolution entries */
            for (const auto &sid : eos_ids)
            {
                if (res_ids.count(sid)) continue;
                auto dit = dim_map.find(sid);
                if (dit == dim_map.end()) continue;
                res_arr.push_back({
                    {"stream_id", sid},
                    {"width", dit->second.w},
                    {"height", dit->second.h},
                    {"framerate", dit->second.fps},
                    {"pool_max_buffers", default_pool_buffers}
                });
                HAL_LOG_INFO("hailo15_media: generate_config: injected resolution for '%s' (%ux%u@%u)",
                             sid.c_str(), dit->second.w, dit->second.h, dit->second.fps);
            }

            /* Remove resolution entries for streams not in encoded_output_streams */
            for (auto it = res_arr.begin(); it != res_arr.end(); )
            {
                std::string rid = it->value("stream_id", "");
                if (!rid.empty() && eos_ids.find(rid) == eos_ids.end())
                {
                    HAL_LOG_INFO("hailo15_media: generate_config: removing extra resolution for '%s'", rid.c_str());
                    it = res_arr.erase(it);
                }
                else
                {
                    ++it;
                }
            }
        };

        /* Apply to inline or file-based application_settings */
        if (active_prof->contains("application_settings"))
        {
            auto &as = (*active_prof)["application_settings"];
            if (as.is_object())
            {
                align_resolutions(as);
            }
            else if (as.is_string())
            {
                std::string as_path = as.get<std::string>();
                std::ifstream asf(as_path);
                if (asf.is_open())
                {
                    json as_data;
                    asf >> as_data;
                    asf.close();
                    align_resolutions(as_data);
                    auto as_hash = compute_medialib_content_hash(as_data);
                    if (as_hash) as_data["metadata"]["content_hash"] = *as_hash;
                    std::ofstream asof(as_path);
                    if (asof.is_open())
                    {
                        asof << as_data.dump(4);
                        asof.close();
                        HAL_LOG_INFO("hailo15_media: generate_config: aligned resolutions in '%s'",
                                     as_path.c_str());
                    }
                }
            }
        }
    }

    /* If we loaded a separate profile file (config_file reference), write the
     * modified profile_data back to a temp file and update config_file so that
     * medialib reads our patched version during initialize(). Without this,
     * medialib follows the original config_file path and ignores in-memory edits. */
    if (active_prof == &profile_data && prof_entry->contains("config_file"))
    {
        std::string orig_path = (*prof_entry)["config_file"].get<std::string>();
        std::string pname = orig_path;
        auto slash = pname.rfind('/');
        if (slash != std::string::npos) pname = pname.substr(slash + 1);
        std::string tmp_prof = std::string("/tmp/profile_reconfig_") + pname;
        std::ofstream pof(tmp_prof);
        if (pof.is_open())
        {
            if (profile_data.contains("metadata") && profile_data["metadata"].is_object())
            {
                auto p_hash = compute_medialib_content_hash(profile_data);
                if (p_hash) profile_data["metadata"]["content_hash"] = *p_hash;
            }

            pof << profile_data.dump(4);
            pof.close();
            (*prof_entry)["config_file"] = tmp_prof;
            HAL_LOG_INFO("hailo15_media: generate_config: wrote patched profile to '%s' "
                         "(was '%s')", tmp_prof.c_str(), orig_path.c_str());
        }
        else
        {
            HAL_LOG_ERROR("hailo15_media: generate_config: cannot write temp profile '%s' — "
                          "medialib will use original '%s'", tmp_prof.c_str(), orig_path.c_str());
        }
    }

    /* Update main config content_hash after modifications */
    if (cfg.contains("metadata") && cfg["metadata"].is_object())
    {
        auto cfg_hash = compute_medialib_content_hash(cfg);
        if (cfg_hash) cfg["metadata"]["content_hash"] = *cfg_hash;
    }

    return cfg.dump(2);
}

/**
 * Full MediaLibrary teardown + reinit for FPS changes.
 * Destroys the entire ML instance and recreates it from the patched config JSON,
 * giving multi_resize a completely fresh state (buffer pools, timestamp accumulators).
 */
static int reinit_media_library_on_stream_change(HalMediaContext *hm,
                                                  Hailo15MediaPriv *priv,
                                                  const std::string &new_config_json,
                                                  bool skip_encoder_overrides)
{
    HAL_LOG_INFO("hailo15_media: reinit: full MediaLibrary destroy+recreate for stream buffer layout change");

    const auto reinit_t0 = std::chrono::steady_clock::now();
    auto reinit_tprev = reinit_t0;
    const auto stage_ms = [&]() -> long long {
        const auto now = std::chrono::steady_clock::now();
        const auto d = std::chrono::duration_cast<std::chrono::milliseconds>(now - reinit_tprev).count();
        reinit_tprev = now;
        return static_cast<long long>(d);
    };

    /*
     * Stop accepting new frontend callbacks, then wait while the old pipeline
     * is still running so callbacks already blocked in encoder add_buffer()
     * can drain.  Calling stop_pipeline() first can stop encoder consumption
     * while an on_new_sample callback is pushing into appsrc, producing the
     * flush_pipeline() <-> add_buffer() deadlock seen during FPS reconfigure.
     */
    uint32_t inflight_at_quiesce = 0;
    {
        std::lock_guard<std::mutex> lock(priv->callback_lifecycle_mu);
        priv->callbacks_quiescing = true;
        inflight_at_quiesce = priv->frontend_callbacks_inflight;
    }
    HAL_LOG_INFO("hailo15_media: reinit: quiescing frontend callbacks "
                 "(inflight=%u)", inflight_at_quiesce);

    {
        constexpr auto callback_drain_timeout = std::chrono::seconds(3);
        std::unique_lock<std::mutex> lock(priv->callback_lifecycle_mu);
        if (!priv->callback_lifecycle_cv.wait_for(
                lock, callback_drain_timeout,
                [&] { return priv->frontend_callbacks_inflight == 0; }))
        {
            const uint32_t inflight = priv->frontend_callbacks_inflight;
            priv->callbacks_quiescing = false;
            HAL_LOG_ERROR("hailo15_media: reinit: frontend callback drain timed out "
                          "after %llds (inflight=%u)",
                          static_cast<long long>(callback_drain_timeout.count()), inflight);
            return HAL_ERR_TIMEOUT;
        }
    }
    HAL_LOG_INFO("hailo15_media: reinit: frontend callbacks drained");
    HAL_LOG_INFO("[TIMING] reinit: drain_callbacks=%lldms", stage_ms());

    // No bridge callback can now be running or newly enter MediaLibrary.
    if (priv->callbacks_registered)
    {
        disconnect_ml_bridge_callbacks(priv);
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->callbacks_registered = false;
    }
    HAL_LOG_INFO("hailo15_media: reinit: bridges disconnected");

    // Stop only after the callback/auto-feed path is fully quiescent.
    if (priv->media_lib)
    {
        HAL_LOG_INFO("hailo15_media: reinit: stop_pipeline ...");
        const media_library_return stop_result = priv->media_lib->stop_pipeline();
        if (stop_result != MEDIA_LIBRARY_SUCCESS)
        {
            HAL_LOG_ERROR("hailo15_media: reinit: stop_pipeline failed (%d)",
                          static_cast<int>(stop_result));
            const int ce = connect_encoders(priv);
            const int cf = connect_frontend(priv);
            if (ce == HAL_OK && cf == HAL_OK)
            {
                std::lock_guard<std::recursive_mutex> lock(priv->mutex);
                priv->callbacks_registered = true;
            }
            {
                std::lock_guard<std::mutex> lock(priv->callback_lifecycle_mu);
                priv->callbacks_quiescing = false;
            }
            return hailo15_ml_err(stop_result);
        }
    }

    HAL_LOG_INFO("[TIMING] reinit: stop_pipeline=%lldms (incl disconnect_bridges)", stage_ms());

    // Shutdown + FULLY destroy old MediaLibrary.
    //    The medialib ConfigManager is a process-level singleton that tracks
    //    all registered "interactors" (one per ML instance).  If the old ML
    //    is merely shut down but not destroyed, its interactor remains
    //    registered, and the new ML's initialize() will trigger
    //    validate_sensor_index_uniqueness with >1 interactor sharing
    //    sensor_index 0 → MEDIA_LIBRARY_CONFIGURATION_ERROR.
    //    Fully releasing the old ML unregisters its interactor first.
    if (priv->media_lib)
    {
        HAL_LOG_INFO("hailo15_media: reinit: shutdown + destroy old ML ...");
        (void)priv->media_lib->shutdown();
        priv->media_lib.reset();
    }

    HAL_LOG_INFO("[TIMING] reinit: shutdown_destroy_old_ml=%lldms", stage_ms());

    // Destroy HAL contexts (they referenced the old ML objects)
    destroy_contexts(priv, hm);

    // Create new MediaLibrary (old is fully destroyed, ConfigManager clean)
    auto ml_exp = MediaLibrary::create();
    if (!ml_exp)
    {
        HAL_LOG_ERROR("hailo15_media: reinit: MediaLibrary::create() failed");
        return HAL_ERROR;
    }
    MediaLibraryPtr new_ml = ml_exp.value();
    new_ml->set_override_persistent_settings(true);

    if (!priv->medialib_default_backup_folder.empty())
    {
        new_ml->set_default_backup_folder_path(priv->medialib_default_backup_folder);
    }

    // Apply encoder dimension overrides before initializing the new ML instance.
    // Skip when caller (reconfigure_pipeline) has already set the correct dimensions
    // — applying YAML overrides here would revert the caller's requested changes.
    std::string fixed_config = new_config_json;
    if (!skip_encoder_overrides)
    {
        if (!priv->encoder_overrides_json.empty())
        {
            apply_encoder_overrides(fixed_config, priv->encoder_overrides_json.c_str());
        }
        else
        {
            fix_encoder_dimension_mismatches(fixed_config);
        }
    }

    // Initialize with new config
    const media_library_return ini = new_ml->initialize(fixed_config, !priv->medialib_default_backup_folder.empty());
    if (ini != MEDIA_LIBRARY_SUCCESS)
    {
        HAL_LOG_ERROR("hailo15_media: reinit: initialize failed (%d)", static_cast<int>(ini));
        return hailo15_ml_err(ini);
    }

    HAL_LOG_INFO("[TIMING] reinit: ml_create+initialize=%lldms", stage_ms());

    // Commit the new ML
    priv->media_lib = std::move(new_ml);

    /* v1.12.0: single-subscriber — re-subscribe replaces the previous callback (no unsubscribe needed). */
    (void)priv->media_lib->subscribe_to_pipeline_state_change(
        [](media_library_pipeline_state_t state) {
            HAL_LOG_INFO("hailo15_media: MediaLibrary pipeline state -> %s (%d)",
                         ml_pipeline_state_str(state), static_cast<int>(state));
        });

    // Update stored config
    priv->stored_config_json = new_config_json;

    // Get profile, rebuild contexts
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        HAL_LOG_ERROR("hailo15_media: reinit: get_current_profile failed");
        return HAL_ERROR;
    }
    const config_profile_t &prof = prof_exp.value();

    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        refresh_ids_from_profile(priv, prof);
        const int br = build_contexts(priv, hm);
        if (br != HAL_OK)
        {
            return br;
        }
        refresh_osd_layout_from_profile(priv, prof);
        priv->last_frontend_stream_ids = priv->frontend_stream_ids;
        priv->last_encoder_stream_ids = priv->encoder_stream_ids;
    }

    // Diag: log encoder dimensions after reinit
    for (const auto &kv : prof.encoded_output_streams)
    {
        std::visit([&](const auto &enc) {
            using T = std::decay_t<decltype(enc)>;
            if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
            {
                HAL_LOG_INFO("hailo15_media: reinit: encoder '%s' configured %ux%u@%u codec=%s",
                             kv.first.c_str(), enc.input_stream.width, enc.input_stream.height,
                             enc.input_stream.framerate,
                             enc.output_stream.codec == CODEC_TYPE_HEVC ? "HEVC" : "H264");
            }
        }, kv.second.encoding);
    }
    HAL_LOG_INFO("hailo15_media: reinit: m_encoders count=%zu", priv->media_lib->m_encoders.size());

    // Reconnect bridges before start so the new pipeline cannot emit into an
    // unregistered frontend.  callbacks_quiescing remains true until both
    // subscriptions are installed.
    const int ce = connect_encoders(priv);
    if (ce != HAL_OK)
    {
        return ce;
    }
    const int cf = connect_frontend(priv);
    if (cf != HAL_OK)
    {
        return cf;
    }
    {
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->callbacks_registered = true;
    }

    {
        std::lock_guard<std::mutex> lock(priv->callback_lifecycle_mu);
        priv->callbacks_quiescing = false;
    }

    // Start only after callback delivery and auto-feed are ready.
    const media_library_return sr = priv->media_lib->start_pipeline();
    if (sr != MEDIA_LIBRARY_SUCCESS)
    {
        HAL_LOG_ERROR("hailo15_media: reinit: start_pipeline failed (%d)", static_cast<int>(sr));
        {
            std::lock_guard<std::mutex> lock(priv->callback_lifecycle_mu);
            priv->callbacks_quiescing = true;
        }
        disconnect_ml_bridge_callbacks(priv);
        {
            std::lock_guard<std::recursive_mutex> lock(priv->mutex);
            priv->callbacks_registered = false;
        }
        return hailo15_ml_err(sr);
    }

    // Update state
    {
        const media_library_pipeline_state_t pst = priv->media_lib->get_pipeline_state();
        std::lock_guard<std::recursive_mutex> lock(priv->mutex);
        priv->pipeline_started = (pst == media_library_pipeline_state_t::PIPELINE_STATE_RUNNING);
        hm->status = priv->pipeline_started ? HAL_STATUS_RUNNING : HAL_STATUS_STOPPED;
    }

    hailo15::video_ml::refresh_all_context_configs(priv, hm);
    HAL_LOG_INFO("[TIMING] reinit: build_contexts+connect+start=%lldms", stage_ms());
    HAL_LOG_INFO("[TIMING] reinit: TOTAL=%lldms",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - reinit_t0).count());
    HAL_LOG_INFO("hailo15_media: reinit: complete");
    return HAL_OK;
}

/* True iff the prev->new profile change touches only resolution/framerate (frontend
 * dims + encoder input w/h/fps) — i.e. every encoder's codec type, bitrate and GOP are
 * unchanged, and no stream was added or removed.
 *
 * Used to decide whether reconfigure_pipeline can take the light
 * set_override_parameters() path. Because that path modifies res/fps only, it is
 * correct only when codec/bitrate/gop already match. The reconfig request always
 * carries the full encoder state, so this is a prev-vs-new delta check, not a
 * field-presence check. */
static bool reconfigure_is_res_fps_only(const config_profile_t &prev_p,
                                        const config_profile_t &new_p)
{
    auto prev_map = prev_p.to_encoder_config_map();
    auto new_map = new_p.to_encoder_config_map();

    auto rate_of = [](const encoder_config_t &cfg) -> std::pair<uint32_t, uint32_t> {
        return std::visit(
            [](const auto &c) -> std::pair<uint32_t, uint32_t> {
                using T = std::decay_t<decltype(c)>;
                if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
                {
                    return {c.rate_control.bitrate.target_bitrate,
                            c.rate_control.intra_pic_rate};
                }
                return {0, 0};
            },
            cfg);
    };

    for (const auto &entry : new_map)
    {
        const auto pit = prev_map.find(entry.first);
        if (pit == prev_map.end())
        {
            return false; /* newly added stream */
        }
        if (get_encoder_type_from_cfg(pit->second) != get_encoder_type_from_cfg(entry.second))
        {
            return false; /* codec type changed */
        }
        if (rate_of(pit->second) != rate_of(entry.second))
        {
            return false; /* bitrate or gop changed */
        }
    }
    for (const auto &entry : prev_map)
    {
        if (new_map.find(entry.first) == new_map.end())
        {
            return false; /* stream removed */
        }
    }
    return true;
}

/* Apply a resolution/framerate-only reconfigure through the light in-place
 * set_override_parameters() path, bypassing the ~4s full MediaLibrary destroy/recreate.
 *
 * apply_frontend_stream_override() suspends encoder feeding across
 * set_override_parameters(), which mitigates the VCEnc -3 stride stall that
 * encoder_input_layout_changed() otherwise guards against with a full reinit — so a
 * geometry-only change can be applied without the heavy recreate.
 *
 * Eligibility is reconfigure_is_res_fps_only(prev_p, p): the delta must be resolution
 * and/or framerate only. codec / bitrate / gop deltas are not applied here (the light
 * path fetches the live profile and would drop them) and must take the heavy reinit.
 *
 * Returns HAL_OK with attempted=false when ineligible (caller takes the heavy path),
 * HAL_OK with attempted=true on success, or non-OK with attempted=true on an apply
 * failure (caller falls back to the heavy reinit). On success stored_config_json is
 * regenerated to stay authoritative, mirroring reinit_media_library_on_stream_change.
 * OSD/privacy-mask geometry is rescaled, but the OSD blender is not fully re-pushed
 * (only the privacy-mask blender is); a later full reinit refreshes OSD fonts/lines. */
static int try_light_reconfigure(HalMediaContext *hm, Hailo15MediaPriv *priv,
                                 const config_profile_t &prev_p,
                                 const config_profile_t &p,
                                 const HalPipelineReconfig *reconfig, bool &attempted)
{
    attempted = false;
    if (!reconfigure_is_res_fps_only(prev_p, p))
    {
        return HAL_OK; /* codec/bitrate/gop changed — caller takes the heavy path */
    }

    attempted = true;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 0; i < reconfig->stream_count; i++)
    {
        const HalPipelineStreamConfig &sc = reconfig->streams[i];
        const std::string sid(sc.stream_id);
        const uint32_t w = (sc.input_width > 0U) ? sc.input_width : sc.encoder_width;
        const uint32_t h = (sc.input_height > 0U) ? sc.input_height : sc.encoder_height;
        const uint32_t f = (sc.input_framerate > 0U) ? sc.input_framerate : sc.encoder_framerate;
        std::optional<std::pair<uint32_t, uint32_t>> res = std::nullopt;
        if (w > 0U && h > 0U)
        {
            res = std::make_pair(w, h);
        }
        std::optional<uint32_t> fps = std::nullopt;
        if (f > 0U)
        {
            fps = f;
        }
        if (!res.has_value() && !fps.has_value())
        {
            continue; /* nothing res/fps to change for this stream */
        }
        HAL_LOG_INFO("hailo15_media: reconfigure_pipeline: light path stream '%s' -> %ux%u@%u",
                     sid.c_str(), w, h, f);
        const int r = hailo15::video_ml::apply_frontend_stream_override(
            priv->media_lib, sid, res, fps, std::nullopt, std::nullopt, priv);
        if (r != HAL_OK)
        {
            HAL_LOG_WARNING("hailo15_media: reconfigure_pipeline: light path stream '%s' failed (%d), falling back to full reinit",
                            sid.c_str(), r);
            return r;
        }
    }

    /* Keep stored_config_json authoritative: regenerate it with the new dims so a
     * later rotation_full_reinit / reconfigure sees the same geometry as the live
     * pipeline (same call the heavy path uses to build its config). */
    std::string new_json = hailo15_generate_pipeline_config_json(priv->stored_config_json, reconfig, p.name);
    if (!new_json.empty())
    {
        priv->stored_config_json = new_json;
    }
    hailo15::video_ml::refresh_all_context_configs(priv, hm);

    HAL_LOG_INFO("[TIMING] reconfigure: light-path=%lldms (skipped full reinit)",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - t0).count());
    return HAL_OK;
}

static int hailo15_media_reconfigure_pipeline(void *media_ctx, const HalPipelineReconfig *reconfig)
{
    auto *hm = static_cast<HalMediaContext *>(media_ctx);
    auto *priv = hailo15_media_priv_from_hal(media_ctx);
    if (!hm || !priv || !priv->media_lib || !reconfig || !reconfig->streams || reconfig->stream_count == 0)
    {
        return HAL_ERR_INVALID_ARG;
    }

    if (reconfig->stream_count > 4)
    {
        HAL_LOG_ERROR("hailo15_media: reconfigure_pipeline: stream_count %u exceeds max 4", reconfig->stream_count);
        return HAL_ERR_INVALID_ARG;
    }

    /* Get the current profile and modify it to match the requested stream configs.
     * Uses set_override_parameters internally via apply_profile_override_and_refresh,
     * which lets medialib decide whether to restart (resolution/codec), pause/unpause,
     * or apply changes directly (bitrate/gop).  This avoids the previous approach of
     * destroying/recreating the entire MediaLibrary which caused GStreamer SIGSEGV
     * and medialib JSON config file corruption. */
    auto prof_exp = priv->media_lib->get_current_profile();
    if (!prof_exp)
    {
        HAL_LOG_ERROR("hailo15_media: reconfigure_pipeline: get_current_profile failed");
        return HAL_ERROR;
    }
    config_profile_t p = prof_exp.value();
    // Save original profile for restart detection (framerate changes need buffer pool reallocation).
    const config_profile_t prev_p = p;

    for (uint32_t i = 0; i < reconfig->stream_count; i++)
    {
        const HalPipelineStreamConfig &sc = reconfig->streams[i];
        const std::string sid(sc.stream_id);

        // --- Modify frontend input stream ---
        if (sc.input_width > 0 || sc.input_height > 0 || sc.input_framerate > 0)
        {
            for (auto &res : p.application_settings.application_input_streams.resolutions)
            {
                if (res.stream_id == sid)
                {
                    if (sc.input_width > 0)
                        res.dimensions.destination_width = sc.input_width;
                    if (sc.input_height > 0)
                        res.dimensions.destination_height = sc.input_height;
                    if (sc.input_framerate > 0)
                        res.framerate = sc.input_framerate;
                    break;
                }
            }
        }

        // --- Modify encoded output stream ---
        auto enc_it = p.encoded_output_streams.find(sid);
        if (enc_it == p.encoded_output_streams.end())
        {
            HAL_LOG_WARNING("hailo15_media: reconfigure_pipeline: stream '%s' not found in encoded_output_streams, skipping encoder overrides",
                         sid.c_str());
            continue;
        }

        config_encoded_output_stream_t &eos = enc_it->second;

        std::visit([&](auto &enc) {
            using T = std::decay_t<decltype(enc)>;
            if constexpr (std::is_same_v<T, hailo_encoder_config_t>)
            {
                if (sc.encoder_width > 0)
                    enc.input_stream.width = sc.encoder_width;
                if (sc.encoder_height > 0)
                    enc.input_stream.height = sc.encoder_height;
                if (sc.encoder_framerate > 0)
                    enc.input_stream.framerate = sc.encoder_framerate;
                if (sc.encoder_bitrate > 0)
                    enc.rate_control.bitrate.target_bitrate = sc.encoder_bitrate;
                if (sc.encoder_gop > 0)
                    enc.rate_control.intra_pic_rate = sc.encoder_gop;

                if (sc.codec[0] != '\0')
                {
                    std::string codec_str(sc.codec);
                    if (codec_str == "h265" || codec_str == "HEVC")
                        enc.output_stream.codec = CODEC_TYPE_HEVC;
                    else
                        enc.output_stream.codec = CODEC_TYPE_H264;
                }
            }
        }, eos.encoding);
    }

    HAL_LOG_INFO("hailo15_media: reconfigure_pipeline: applying %u stream overrides", reconfig->stream_count);

    // Encoder dimensions and framerate define VCEnc preprocessing and DSP buffer
    // layouts. The in-place override path can retain stale stride/pool state, so
    // destroy and recreate MediaLibrary whenever that layout changes.
    const bool full_reinit_required =
        ml_stream_restart_required(prev_p, p) || encoder_input_layout_changed(prev_p, p);
    if (full_reinit_required)
    {
        /* Resolution/framerate-only changes take the light set_override_parameters()
         * path (encoder feed-suspend mitigates the VCEnc -3 stride stall this full
         * reinit otherwise guards against), bypassing the ~4s MediaLibrary
         * destroy/recreate. codec/bitrate/gop deltas are not eligible and take the
         * heavy path; any apply failure falls back to the heavy reinit below. */
        bool light_attempted = false;
        const int light_rc = try_light_reconfigure(hm, priv, prev_p, p, reconfig, light_attempted);
        if (light_attempted && light_rc == HAL_OK)
        {
            HAL_LOG_INFO("hailo15_media: reconfigure_pipeline: light-path success (%u streams)", reconfig->stream_count);
            return HAL_OK;
        }
        HAL_LOG_INFO("hailo15_media: reconfigure_pipeline: stream buffer layout change detected, full reinit%s",
                     light_attempted ? " (light-path fallback)" : "");
        std::string new_json = hailo15_generate_pipeline_config_json(priv->stored_config_json, reconfig, p.name);
        if (new_json.empty())
        {
            HAL_LOG_ERROR("hailo15_media: reconfigure_pipeline: failed to generate new config JSON");
            return HAL_ERROR;
        }
        int rc = reinit_media_library_on_stream_change(hm, priv, new_json, true);
        if (rc == HAL_OK)
        {
            HAL_LOG_INFO("hailo15_media: reconfigure_pipeline: reinit success (%u streams)", reconfig->stream_count);
        }
        return rc;
    }

    const int rc = apply_profile_override_and_refresh(hm, priv, p, "reconfigure_pipeline");

    if (rc == HAL_OK)
    {
        HAL_LOG_INFO("hailo15_media: reconfigure_pipeline: success (%u streams)", reconfig->stream_count);
    }
    return rc;
}

static const char *hailo15_media_get_version(void)
{
    return "Hailo15 HAL-MEDIA 2.0.0";
}

/* --------------------------------------------------------------------
 * HalFrameBuffer request/release ops (used for DSP dst / overlays)
 * -------------------------------------------------------------------- */

static const char *hailo15_frame_buffer_get_version(void)
{
    return "Hailo15 HAL-FRAMEBUFFER 2.0.0";
}

static HailoFormat hailo15_hal_pixfmt_to_hailo_format(HalPixelFormat fmt)
{
    switch (fmt)
    {
        case HAL_PIX_FMT_NV12:
            return HAILO_FORMAT_NV12;
        case HAL_PIX_FMT_GRAY8:
            return HAILO_FORMAT_GRAY8;
        case HAL_PIX_FMT_RGB24:
            return HAILO_FORMAT_RGB;
        case HAL_PIX_FMT_ARGB32:
            return HAILO_FORMAT_ARGB;
        default:
            return HAILO_FORMAT_NV12;
    }
}

static bool hailo15_is_supported_request_format(HalPixelFormat fmt)
{
    switch (fmt)
    {
        case HAL_PIX_FMT_NV12:
        case HAL_PIX_FMT_GRAY8:
        case HAL_PIX_FMT_RGB24:
        case HAL_PIX_FMT_ARGB32:
            return true;
        default:
            return false;
    }
}

static HailoMemoryType hailo15_hal_memtype_to_hailo(HalMemoryType mem_type)
{
    switch (mem_type)
    {
        case HAL_MEM_DMABUF:
            return HAILO_MEMORY_TYPE_DMABUF;
        case HAL_MEM_MMAP:
        case HAL_MEM_MALLOC:
        default:
            return HAILO_MEMORY_TYPE_CMA;
    }
}

struct Hailo15FramePoolKey
{
    uint32_t width{0};
    uint32_t height{0};
    HalPixelFormat fmt{HAL_PIX_FMT_NV12};
    uint32_t max_buffers{0};
    HalMemoryType mem_type{HAL_MEM_DMABUF};
};

struct Hailo15FramePoolKeyHash
{
    size_t operator()(const Hailo15FramePoolKey &k) const noexcept
    {
        // simple combine
        size_t h = static_cast<size_t>(k.width);
        h ^= (static_cast<size_t>(k.height) << 1);
        h ^= (static_cast<size_t>(k.fmt) << 2);
        h ^= (static_cast<size_t>(k.max_buffers) << 3);
        h ^= (static_cast<size_t>(k.mem_type) << 4);
        return h;
    }
};

struct Hailo15FramePoolKeyEq
{
    bool operator()(const Hailo15FramePoolKey &a, const Hailo15FramePoolKey &b) const noexcept
    {
        return (a.width == b.width) && (a.height == b.height) && (a.fmt == b.fmt) && (a.max_buffers == b.max_buffers) &&
               (a.mem_type == b.mem_type);
    }
};

static MediaLibraryBufferPoolPtr hailo15_get_or_create_pool(const Hailo15FramePoolKey &key)
{
    static std::mutex s_pool_mtx;
    static std::unordered_map<Hailo15FramePoolKey, std::weak_ptr<MediaLibraryBufferPool>, Hailo15FramePoolKeyHash,
                              Hailo15FramePoolKeyEq>
        s_pools;

    std::lock_guard<std::mutex> lock(s_pool_mtx);
    auto it = s_pools.find(key);
    if (it != s_pools.end())
    {
        if (auto existing = it->second.lock())
        {
            return existing;
        }
        // expired pool - remove and recreate
        s_pools.erase(it);
    }

    // Conservative default pool size for application-requested buffers.
    constexpr size_t kDefaultMaxBuffers = 8;
    const size_t max_buffers = (key.max_buffers > 0) ? static_cast<size_t>(key.max_buffers) : kDefaultMaxBuffers;
    const HailoFormat hf = hailo15_hal_pixfmt_to_hailo_format(key.fmt);
    const HailoMemoryType hm = hailo15_hal_memtype_to_hailo(key.mem_type);
    std::string name = "hal_v2_frame_request_pool";
    auto pool = std::make_shared<MediaLibraryBufferPool>(key.width, key.height, hf, max_buffers, hm, name);
    if (pool->init() != MEDIA_LIBRARY_SUCCESS)
    {
        return nullptr;
    }
    s_pools[key] = pool; // weak cache: pool is freed when last buffer releases it
    return pool;
}

static int hailo15_frame_buffer_request(const HalFrameBufferRequest *req, HalFrameBuffer **frame_out)
{
    if (!req || !frame_out)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (req->width == 0 || req->height == 0)
    {
        return HAL_ERR_INVALID_ARG;
    }

    if (!hailo15_is_supported_request_format(req->format))
    {
        return HAL_ERR_NOT_SUPPORTED;
    }

    Hailo15FramePoolKey key{};
    key.width = req->width;
    key.height = req->height;
    key.fmt = req->format;
    key.max_buffers = req->pool_max_buffers;
    key.mem_type = req->mem_type;

    MediaLibraryBufferPoolPtr pool = hailo15_get_or_create_pool(key);
    if (!pool)
    {
        return HAL_ERR_NO_MEM;
    }

    // The MediaLibraryBufferPool API expects a shared_ptr<hailo_media_library_buffer>.
    HailoMediaLibraryBufferPtr buf = std::make_shared<hailo_media_library_buffer>();
    media_library_return r = pool->acquire_buffer(buf);
    if (r != MEDIA_LIBRARY_SUCCESS)
    {
        return hailo15_ml_err(r);
    }

    // Map to HAL buffer (creates Hailo15FramePriv with ml_buf held inside).
    auto *fb = static_cast<HalFrameBuffer *>(std::calloc(1, sizeof(HalFrameBuffer)));
    if (!fb)
    {
        // Let the shared_ptr drop and return buffer to pool.
        buf.reset();
        return HAL_ERR_NO_MEM;
    }
    hailo15_fill_frame_from_buffer(buf, fb);

    // Ensure format/geometry fields match request (fill_frame already sets from buffer_data).
    fb->width = req->width;
    fb->height = req->height;
    fb->format = req->format;

    // Optional zero init: for DMABUF we need sync_start/end around memset.
    if (req->zero_initialize && fb->num_planes > 0)
    {
        if (buf && buf->is_dmabuf())
        {
            (void)buf->sync_start();
        }
        for (uint32_t i = 0; i < fb->num_planes; i++)
        {
            if (fb->planes[i] && fb->sizes[i] > 0)
            {
                std::memset(fb->planes[i], 0, fb->sizes[i]);
            }
        }
        if (buf && buf->is_dmabuf())
        {
            (void)buf->sync_end();
        }
    }

    fb->sequence = 0;
    fb->timestamp_ns = 0;
    *frame_out = fb;
    return HAL_OK;
}

static int hailo15_frame_buffer_release(HalFrameBuffer *frame)
{
    if (!frame)
    {
        return HAL_ERR_INVALID_ARG;
    }
    if (frame->priv)
    {
        delete static_cast<Hailo15FramePriv *>(frame->priv);
        frame->priv = nullptr;
    }
    std::free(frame);
    return HAL_OK;
}

extern "C" int hailo15_ml_clone_buffer_metadata(const HalFrameBuffer *src, HalFrameBuffer *dst);

static int hailo15_frame_buffer_copy_metadata_from_frame_buffer(const HalFrameBuffer *src, HalFrameBuffer *dst)
{
    return hailo15_ml_clone_buffer_metadata(src, dst);
}

HalFrameBufferOps HAL_FRAME_BUFFER_OPS = {
    .request_frame_buffer = hailo15_frame_buffer_request,
    .copy_metadata_from_frame_buffer = hailo15_frame_buffer_copy_metadata_from_frame_buffer,
    .release_frame_buffer = hailo15_frame_buffer_release,
    .get_version = hailo15_frame_buffer_get_version,
};

HalMediaOps HAL_MEDIA_OPS = {
    .init = hailo15_media_init,
    .deinit = hailo15_media_deinit,
    .start = hailo15_media_start,
    .stop = hailo15_media_stop,
    .get_status = hailo15_media_get_status,
    .get_current_profile = hailo15_media_get_current_profile,
    .get_profile_list = hailo15_media_get_profile_list,
    .switch_profile = hailo15_media_switch_profile,
    .get_video_list = hailo15_media_get_video_list,
    .get_codec_list = hailo15_media_get_codec_list,
    .get_current_config = hailo15_media_get_current_config,
    .get_current_profile_json = hailo15_media_get_current_profile_json,
    .backup_current_profile = hailo15_media_backup_current_profile,
    .set_config_field = hailo15_media_set_config_field,
    .get_config_field = hailo15_media_get_config_field,
    .dynamic_change_image_config = hailo15_media_dynamic_change_image_config,
    .add_video_stream = hailo15_media_add_video_stream,
    .add_codec_stream = hailo15_media_add_codec_stream,
    .add_streams_batch = hailo15_media_add_streams_batch,
    .remove_video_stream = hailo15_media_remove_video_stream,
    .remove_codec_stream = hailo15_media_remove_codec_stream,
    .remove_streams_batch = hailo15_media_remove_streams_batch,
    .set_encoder_auto_feed = hailo15_media_set_encoder_auto_feed,
    .get_encoder_auto_feed = hailo15_media_get_encoder_auto_feed,
    .set_encoder_auto_feed_for_stream = hailo15_media_set_encoder_auto_feed_for_stream,
    .get_encoder_auto_feed_for_stream = hailo15_media_get_encoder_auto_feed_for_stream,
    .override_stream_params = hailo15_media_override_stream_params,
    .reconfigure_pipeline = hailo15_media_reconfigure_pipeline,
    .get_version = hailo15_media_get_version,
    .attach_frame_analytics = hailo15_media_attach_frame_analytics,
};

} // extern "C"

static int reinit_media_library_on_layout_change(HalMediaContext *hm,
                                                   Hailo15MediaPriv *priv,
                                                   const config_profile_t &target_profile,
                                                   const char *tag)
{
    HAL_LOG_INFO("hailo15_media: reinit_layout [%s]: full destroy+recreate for stream layout change", tag ? tag : "?");

    std::string patched_json = patch_json_stream_layout(priv->stored_config_json, target_profile);
    if (patched_json.empty())
    {
        HAL_LOG_ERROR("hailo15_media: reinit_layout: failed to patch JSON for layout change");
        return HAL_ERROR;
    }

    // Do NOT skip encoder overrides: patch_json_stream_layout only patches the NEW
    // stream's dimensions; existing streams still need their YAML overrides applied
    // (e.g. sink0=1920x1080 vs vendor default 3840x2160).  Without this, existing
    // encoder dimensions revert to vendor defaults after reinit.
    return reinit_media_library_on_stream_change(hm, priv, patched_json, false);
}

void *hailo15_buffer_priv_create(HailoMediaLibraryBufferPtr buf)
{
    auto *fp = new Hailo15FramePriv{};
    fp->ml_buf = std::move(buf);
    return fp;
}

extern "C" int hailo15_ml_clone_buffer_metadata(const HalFrameBuffer *src, HalFrameBuffer *dst)
{
    if (!src || !dst || !src->priv || !dst->priv)
    {
        return HAL_ERR_INVALID_ARG;
    }
    auto *src_fp = static_cast<Hailo15FramePriv *>(src->priv);
    auto *dst_fp = static_cast<Hailo15FramePriv *>(dst->priv);
    if (!src_fp->ml_buf || !dst_fp->ml_buf)
    {
        return HAL_ERR_INVALID_ARG;
    }

    /* Best-effort metadata clone using SDK helper.
     * This copies per-buffer fields that some pipeline elements depend on (timestamps, AE stats, motion detection, etc). */
    dst_fp->ml_buf->copy_metadata_from(src_fp->ml_buf);
    return HAL_OK;
}
