/**
 * @file hailo15_postprocess_impl.cpp
 * @brief Hailo-15 HAL postprocess implementation.
 *
 * This module aligns with the Hailo-15 platform postprocess flow:
 * it loads and runs a vendor postprocess plugin (.so) operating on HailoROIPtr.
 */

#include "common/hal_common.h"
#include "common/hal_log.h"
#include "model/hal_postprocess.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <unistd.h>
#include <chrono>
#include <random>
#include <string>
#include <utility>
#include <vector>

#include "common/hal_hailo15_priv.hpp"
#include "model/hal_clip_text_encoder.hpp"
#include "common/hal_clip_prompt_scorer.hpp"
#include "platforms/hailo15/model/hal_internal_ocr_paddle.hpp"
#include "platforms/hailo15/model/hal_internal_yolov8_pose.hpp"
#include "platforms/hailo15/model/hal_internal_scdepth.hpp"

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
#include <dlfcn.h>
#include <hailo_postprocess_tools/objects/hailo_objects.hpp>
#endif

namespace
{

struct Hailo15PostPriv
{
    HalPostprocessConfig cfg{};
    /** Session-owned copy of vendor / application JSON merged with runtime patches (stable `config_json` pointers). */
    std::string merged_vendor_json;
    /** Best-effort labels extracted from vendor JSON key "labels" (e.g., apps/webserver/configs/yolov5*.json). */
    std::vector<std::string> labels;
    int32_t label_offset = 0;

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
    // Vendor plugin backing (optional).
    void *dl_handle = nullptr;
    void *post_params = nullptr;
    void (*post_fn)(HailoROIPtr, void *) = nullptr;
    void (*post_fn_no_params)(HailoROIPtr) = nullptr;
    void (*post_free)(void *) = nullptr;
    std::string plugin_lib_path;
    std::string plugin_function;
    std::string plugin_config_path;
    std::string temp_config_path;
    /** When true, OCR det/rec use hal_internal_ocr_paddle (no external libocr_post.so). */
    bool ocr_builtin = false;
    std::vector<std::string> ocr_rec_charset;
    /** When true, YOLOv8-Pose decode runs in HAL (no libyolov8pose_postprocess.so). */
    bool yolov8_pose_builtin = false;
    // CLIP zero-shot classification components
    hal_v2::HalClipTextEncoder *clip_text_encoder = nullptr;
    hal_v2::HalClipPromptScorer *clip_scorer = nullptr;
#endif
};

static std::string read_file_to_string(const char *path)
{
    if (!path || path[0] == '\0')
        return {};
    std::FILE *f = std::fopen(path, "rb");
    if (!f)
        return {};
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0)
    {
        std::fclose(f);
        return {};
    }
    std::string s;
    s.resize((size_t)sz);
    const size_t nread = std::fread(s.data(), 1, (size_t)sz, f);
    std::fclose(f);
    if (nread != (size_t)sz)
        s.resize(nread);
    return s;
}

static bool write_string_to_file(const char *path, const std::string &s)
{
    if (!path || path[0] == '\0')
        return false;
    std::FILE *f = std::fopen(path, "wb");
    if (!f)
        return false;
    const size_t nw = std::fwrite(s.data(), 1, s.size(), f);
    std::fclose(f);
    return nw == s.size();
}

static std::string make_temp_json_path(const char *tag)
{
    // Thread-safe, unpredictable temp name. std::rand() shares global state and
    // defaults to seed 1, and a predictable /tmp path is a symlink-race surface.
    // Mix pid + a nanosecond clock + a per-thread PRNG seeded from OS entropy.
    const pid_t pid = getpid();
    const long long now_ns = (long long)std::chrono::steady_clock::now().time_since_epoch().count();
    thread_local std::mt19937_64 rng = [] {
        std::random_device rd;
        std::seed_seq seq{rd(), rd(), rd(), rd(),
                          (unsigned int)std::chrono::steady_clock::now().time_since_epoch().count()};
        return std::mt19937_64(seq);
    }();
    char buf[256];
    std::snprintf(buf, sizeof(buf), "/tmp/hal_post_%s_%ld_%lld_%llu.json",
                  tag ? tag : "cfg", (long)pid, now_ns, (unsigned long long)rng());
    return std::string(buf);
}

static std::string json_extract_string_best_effort(const std::string &json, const char *key)
{
    if (!key)
        return {};
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = json.find(kq);
    if (p == std::string::npos)
        return {};
    p = json.find(':', p);
    if (p == std::string::npos)
        return {};
    p++;
    while (p < json.size() && std::isspace((unsigned char)json[p]))
        p++;
    if (p >= json.size() || json[p] != '"')
        return {};
    p++;
    size_t q = json.find('"', p);
    if (q == std::string::npos)
        return {};
    return json.substr(p, q - p);
}

static bool str_has_json_object_prefix(const char *s)
{
    if (!s)
        return false;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        ++s;
    return *s == '{';
}

static void repoint_config_json(HalPostprocessConfig *cfg, const char *json_cstr)
{
    if (!cfg || !json_cstr)
        return;
    switch (cfg->type)
    {
        case HAL_POST_TYPE_DETECTION:
            cfg->config.detection.config_json = json_cstr;
            break;
        case HAL_POST_TYPE_CLASSIFICATION:
            cfg->config.classification.config_json = json_cstr;
            break;
        case HAL_POST_TYPE_CLIP:
            cfg->config.clip.config_json = json_cstr;
            break;
        case HAL_POST_TYPE_SEGMENTATION:
            cfg->config.segmentation.config_json = json_cstr;
            break;
        case HAL_POST_TYPE_KEYPOINT:
            cfg->config.keypoint.config_json = json_cstr;
            break;
        case HAL_POST_TYPE_EMBEDDING:
            cfg->config.embedding.config_json = json_cstr;
            break;
        case HAL_POST_TYPE_OCR_DETECTION:
            cfg->config.ocr_detection.config_json = json_cstr;
            break;
        case HAL_POST_TYPE_OCR_RECOGNITION:
            cfg->config.ocr_recognition.config_json = json_cstr;
            break;
        case HAL_POST_TYPE_DEPTH:
            cfg->config.depth.config_json = json_cstr;
            break;
        default:
            break;
    }
}

static float extract_json_float_after_key(const std::string &j, const char *key)
{
    if (!key)
        return NAN;
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = j.find(kq);
    if (p == std::string::npos)
        return NAN;
    p = j.find(':', p);
    if (p == std::string::npos)
        return NAN;
    p++;
    while (p < j.size() && std::isspace((unsigned char)j[p]))
        p++;
    if (p >= j.size())
        return NAN;
    char *end = nullptr;
    const float v = std::strtof(j.c_str() + p, &end);
    if (!end || end == j.c_str() + p)
        return NAN;
    return v;
}

static std::vector<std::string> parse_string_array_from_json_key(const std::string &json, const char *array_key)
{
    std::vector<std::string> out;
    if (!array_key)
        return out;
    const std::string key = std::string("\"") + array_key + "\"";
    size_t p = json.find(key);
    if (p == std::string::npos)
        return out;
    p = json.find('[', p);
    if (p == std::string::npos)
        return out;
    size_t q = json.find(']', p);
    if (q == std::string::npos || q <= p)
        return out;
    const std::string inside = json.substr(p + 1, q - (p + 1));
    size_t i = 0;
    while (i < inside.size())
    {
        while (i < inside.size() && std::isspace((unsigned char)inside[i]))
            i++;
        if (i >= inside.size())
            break;
        if (inside[i] != '"')
        {
            i++;
            continue;
        }
        i++;
        size_t j = inside.find('"', i);
        if (j == std::string::npos)
            break;
        out.push_back(inside.substr(i, j - i));
        i = j + 1;
    }
    return out;
}

static void refresh_labels_from_vendor_json(Hailo15PostPriv *p)
{
    if (!p)
        return;
    p->labels.clear();
    p->label_offset = 0;
    if (p->merged_vendor_json.empty())
        return;
    p->labels = parse_string_array_from_json_key(p->merged_vendor_json, "labels");
    const float lo = extract_json_float_after_key(p->merged_vendor_json, "label_offset");
    if (std::isfinite(lo))
        p->label_offset = (int32_t)lo;
}

/**
 * Map merged JSON scalar keys into HalPostprocessConfig for the active session type.
 * Vendor names (e.g. yolov5.json detection_threshold / iou_threshold) align with in-tree analytics configs.
 */
static void sync_hal_postprocess_scalar(HalPostprocessConfig *cfg, HalPostprocessType t, const char *key, float v)
{
    if (!cfg || !key || !std::isfinite(v))
        return;
    const uint32_t as_u = (uint32_t)(v >= 0.f ? (int)(v + 0.5f) : 0);

    switch (t)
    {
        case HAL_POST_TYPE_DETECTION:
            if (!std::strcmp(key, "detection_threshold") || !std::strcmp(key, "confidence_threshold"))
                cfg->config.detection.confidence_threshold = v;
            else if (!std::strcmp(key, "iou_threshold") || !std::strcmp(key, "nms_threshold"))
                cfg->config.detection.nms_threshold = v;
            else if (!std::strcmp(key, "max_boxes") || !std::strcmp(key, "max_detections"))
            {
                if (as_u > 0U)
                    cfg->config.detection.max_detections = as_u;
            }
            break;
        case HAL_POST_TYPE_CLASSIFICATION:
            if (!std::strcmp(key, "confidence_threshold"))
                cfg->config.classification.confidence_threshold = v;
            else if (!std::strcmp(key, "top_k") && as_u > 0U)
                cfg->config.classification.top_k = as_u;
            break;
        case HAL_POST_TYPE_CLIP:
            if (!std::strcmp(key, "score_threshold"))
                cfg->config.clip.score_threshold = v;
            else if (!std::strcmp(key, "top_k") && as_u > 0U)
                cfg->config.clip.top_k = as_u;
            break;
        case HAL_POST_TYPE_SEGMENTATION:
            if (!std::strcmp(key, "confidence_threshold"))
                cfg->config.segmentation.confidence_threshold = v;
            else if (!std::strcmp(key, "output_width") && as_u > 0U)
                cfg->config.segmentation.output_width = as_u;
            else if (!std::strcmp(key, "output_height") && as_u > 0U)
                cfg->config.segmentation.output_height = as_u;
            break;
        case HAL_POST_TYPE_KEYPOINT:
            if (!std::strcmp(key, "confidence_threshold"))
                cfg->config.keypoint.confidence_threshold = v;
            else if (!std::strcmp(key, "keypoint_threshold"))
                cfg->config.keypoint.keypoint_threshold = v;
            else if (!std::strcmp(key, "num_keypoints") && as_u > 0U)
                cfg->config.keypoint.num_keypoints = as_u;
            break;
        case HAL_POST_TYPE_OCR_DETECTION:
            if (!std::strcmp(key, "min_confidence"))
                cfg->config.ocr_detection.min_confidence = v;
            else if (!std::strcmp(key, "det_bin_thresh"))
                cfg->config.ocr_detection.det_bin_thresh = v;
            else if (!std::strcmp(key, "det_box_thresh"))
                cfg->config.ocr_detection.det_box_thresh = v;
            else if (!std::strcmp(key, "det_unclip_ratio"))
                cfg->config.ocr_detection.det_unclip_ratio = v;
            else if (!std::strcmp(key, "det_max_candidates"))
                cfg->config.ocr_detection.det_max_candidates = (int32_t)as_u;
            else if (!std::strcmp(key, "det_min_box_size"))
                cfg->config.ocr_detection.det_min_box_size = v;
            else if (!std::strcmp(key, "det_map_h"))
                cfg->config.ocr_detection.det_map_h = (int32_t)as_u;
            else if (!std::strcmp(key, "det_map_w"))
                cfg->config.ocr_detection.det_map_w = (int32_t)as_u;
            break;
        case HAL_POST_TYPE_OCR_RECOGNITION:
            if (!std::strcmp(key, "min_confidence"))
                cfg->config.ocr_recognition.min_confidence = v;
            else if (!std::strcmp(key, "text_conf_smooth"))
                cfg->config.ocr_recognition.text_conf_smooth = v;
            else if (!std::strcmp(key, "max_edit_distance"))
                cfg->config.ocr_recognition.max_edit_distance = (int32_t)as_u;
            else if (!std::strcmp(key, "charset_index_offset"))
                cfg->config.ocr_recognition.charset_index_offset = (int32_t)(v + (v >= 0.f ? 0.5f : -0.5f));
            else if (!std::strcmp(key, "blank_index"))
                cfg->config.ocr_recognition.blank_index = (int32_t)as_u;
            break;
        default:
            break;
    }
}

// Forward declarations for create-time patch merge helper.
static bool extract_json_bool_after_key(const std::string &j, const char *key, bool *out);
static bool replace_or_insert_json_bool(std::string &j, const char *key, bool val);
static bool replace_or_insert_json_number(std::string &j, const char *key, float v);
static bool replace_or_insert_json_string(std::string &j, const char *key, const std::string &val);
static void refresh_hal_ocr_structs_from_merged_json(Hailo15PostPriv *p);

static void merge_vendor_patch_json_best_effort(Hailo15PostPriv *p, const char *patch_json)
{
    if (!p || !patch_json || !str_has_json_object_prefix(patch_json))
        return;
    std::string patch(patch_json);
    if (p->merged_vendor_json.empty())
        p->merged_vendor_json = "{}";

    bool changed = false;

    // Numeric keys (keep aligned with apply_config_json).
    static const char *kNumericKeys[] = {
        "detection_threshold",
        "confidence_threshold",
        "iou_threshold",
        "nms_threshold",
        "max_boxes",
        "max_detections",
        "label_offset",
        "score_threshold",
        "top_k",
        "output_width",
        "output_height",
        "keypoint_threshold",
        "num_keypoints",
        "min_confidence",
        /* hailo-apps ocr_postprocess vendor JSON (see local_resources/ocr_config.json) */
        "det_bin_thresh",
        "det_box_thresh",
        "det_unclip_ratio",
        "det_max_candidates",
        "det_min_box_size",
        "det_map_h",
        "det_map_w",
        "text_conf_smooth",
        "max_edit_distance",
        "charset_index_offset",
        "blank_index",
    };
    for (const char *key : kNumericKeys)
    {
        const float v = extract_json_float_after_key(patch, key);
        if (!std::isfinite(v))
            continue;
        if (replace_or_insert_json_number(p->merged_vendor_json, key, v))
        {
            changed = true;
            sync_hal_postprocess_scalar(&p->cfg, p->cfg.type, key, v);
        }
    }

    // Common string keys used by vendor plugins.
    for (const char *key : {"backend_lib_path", "backend_function", "backend_config_path", "output_activation",
                            "scdepth_output_name", "output_tensor_name"})
    {
        const std::string before = json_extract_string_best_effort(p->merged_vendor_json, key);
        const std::string v = json_extract_string_best_effort(patch, key);
        if (!v.empty() && replace_or_insert_json_string(p->merged_vendor_json, key, v))
        {
            changed = true;
            if (before != v)
                HAL_LOG_INFO("hailo15_postprocess: merged %s override \"%s\" -> \"%s\"", key, before.c_str(), v.c_str());
#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
            // Also update chosen plugin fields directly so defaults won't override the patch.
            if (std::strcmp(key, "backend_function") == 0)
                p->plugin_function = v;
            else if (std::strcmp(key, "backend_lib_path") == 0)
                p->plugin_lib_path = v;
            else if (std::strcmp(key, "backend_config_path") == 0)
                p->plugin_config_path = v;
#endif
        }
    }

    // Bool keys
    {
        bool nb = false;
        if (extract_json_bool_after_key(patch, "normalize", &nb))
        {
            if (replace_or_insert_json_bool(p->merged_vendor_json, "normalize", nb))
            {
                changed = true;
                if (p->cfg.type == HAL_POST_TYPE_EMBEDDING)
                    p->cfg.config.embedding.normalize = nb;
            }
        }
    }
    {
        bool df = false;
        if (extract_json_bool_after_key(patch, "depth_float32", &df) && p->cfg.type == HAL_POST_TYPE_DEPTH)
        {
            if (replace_or_insert_json_bool(p->merged_vendor_json, "depth_float32", df))
                changed = true;
        }
    }

    if (changed)
        repoint_config_json(&p->cfg, p->merged_vendor_json.c_str());
    refresh_labels_from_vendor_json(p);
    refresh_hal_ocr_structs_from_merged_json(p);
}

static bool extract_json_bool_after_key(const std::string &j, const char *key, bool *out)
{
    if (!key || !out)
        return false;
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = j.find(kq);
    if (p == std::string::npos)
        return false;
    p = j.find(':', p);
    if (p == std::string::npos)
        return false;
    p++;
    while (p < j.size() && std::isspace((unsigned char)j[p]))
        p++;
    if (p >= j.size())
        return false;
    if (j.size() - p >= 4 && j.compare(p, 4, "true") == 0)
    {
        *out = true;
        return true;
    }
    if (j.size() - p >= 5 && j.compare(p, 5, "false") == 0)
    {
        *out = false;
        return true;
    }
    char *end = nullptr;
    const float vf = std::strtof(j.c_str() + p, &end);
    if (end && end != j.c_str() + p)
    {
        *out = (vf != 0.f);
        return true;
    }
    return false;
}

/** Copy merged vendor JSON OCR keys into @c p->cfg so builtin and filters match @c config_file-only JSON. */
static void refresh_hal_ocr_structs_from_merged_json(Hailo15PostPriv *p)
{
    if (!p || p->merged_vendor_json.empty())
        return;
    const std::string &j = p->merged_vendor_json;

    if (p->cfg.type == HAL_POST_TYPE_OCR_DETECTION)
    {
        HalOcrDetectionPostConfig *c = &p->cfg.config.ocr_detection;
#define HAL_OCR_RF(field, key)                                                                                         \
    do                                                                                                                 \
    {                                                                                                                  \
        const float v = extract_json_float_after_key(j, key);                                                        \
        if (std::isfinite(v))                                                                                          \
            c->field = v;                                                                                              \
    } while (0)
        HAL_OCR_RF(det_bin_thresh, "det_bin_thresh");
        HAL_OCR_RF(det_box_thresh, "det_box_thresh");
        HAL_OCR_RF(det_unclip_ratio, "det_unclip_ratio");
        HAL_OCR_RF(det_min_box_size, "det_min_box_size");
        HAL_OCR_RF(min_confidence, "min_confidence");
#undef HAL_OCR_RF
        {
            const float v = extract_json_float_after_key(j, "det_max_candidates");
            if (std::isfinite(v) && v >= 0.f)
                c->det_max_candidates = (int32_t)(v + 0.5f);
        }
        {
            const float vh = extract_json_float_after_key(j, "det_map_h");
            if (std::isfinite(vh) && vh >= 0.f)
                c->det_map_h = (int32_t)(vh + 0.5f);
            const float vw = extract_json_float_after_key(j, "det_map_w");
            if (std::isfinite(vw) && vw >= 0.f)
                c->det_map_w = (int32_t)(vw + 0.5f);
        }
        {
            bool lb = c->letterbox_fix;
            if (extract_json_bool_after_key(j, "letterbox_fix", &lb))
                c->letterbox_fix = lb;
        }
        {
            const std::string s = json_extract_string_best_effort(j, "det_output_name");
            if (!s.empty())
                std::snprintf(c->det_output_name, sizeof(c->det_output_name), "%s", s.c_str());
        }
    }
    else if (p->cfg.type == HAL_POST_TYPE_OCR_RECOGNITION)
    {
        HalOcrRecognitionPostConfig *c = &p->cfg.config.ocr_recognition;
        {
            const float v = extract_json_float_after_key(j, "min_confidence");
            if (std::isfinite(v))
                c->min_confidence = v;
        }
        {
            const float v = extract_json_float_after_key(j, "text_conf_smooth");
            if (std::isfinite(v))
                c->text_conf_smooth = v;
        }
        {
            const float v = extract_json_float_after_key(j, "charset_index_offset");
            if (std::isfinite(v))
                c->charset_index_offset = (int32_t)(v + (v >= 0.f ? 0.5f : -0.5f));
        }
        {
            const float v = extract_json_float_after_key(j, "blank_index");
            if (std::isfinite(v))
                c->blank_index = (int32_t)(v + 0.5f);
        }
        {
            const float v = extract_json_float_after_key(j, "max_edit_distance");
            if (std::isfinite(v) && v >= 0.f)
                c->max_edit_distance = (int32_t)(v + 0.5f);
        }
        {
            bool b = c->logits_are_softmax;
            if (extract_json_bool_after_key(j, "logits_are_softmax", &b))
                c->logits_are_softmax = b;
        }
        {
            bool b = c->time_major;
            if (extract_json_bool_after_key(j, "time_major", &b))
                c->time_major = b;
        }
        {
            bool b = c->attach_caption_box;
            if (extract_json_bool_after_key(j, "attach_caption_box", &b))
                c->attach_caption_box = b;
        }
        {
            const std::string s = json_extract_string_best_effort(j, "rec_output_name");
            if (!s.empty())
                std::snprintf(c->rec_output_name, sizeof(c->rec_output_name), "%s", s.c_str());
        }
        {
            const std::string s = json_extract_string_best_effort(j, "charset_path");
            if (!s.empty())
                std::snprintf(c->charset_path, sizeof(c->charset_path), "%s", s.c_str());
        }
        {
            const std::string s = json_extract_string_best_effort(j, "frequency_dict_path");
            if (!s.empty())
                std::snprintf(c->frequency_dict_path, sizeof(c->frequency_dict_path), "%s", s.c_str());
        }
    }
}

static bool replace_or_insert_json_bool(std::string &j, const char *key, bool val)
{
    if (!key || key[0] == '\0')
        return false;
    const char *lit = val ? "true" : "false";
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = j.find(kq);
    if (p != std::string::npos)
    {
        const size_t colon = j.find(':', p);
        if (colon == std::string::npos)
            return false;
        size_t start = colon + 1;
        while (start < j.size() && std::isspace((unsigned char)j[start]))
            start++;
        size_t end = start;
        if (end < j.size() && j.compare(end, 4, "true") == 0)
            end += 4;
        else if (end < j.size() && j.compare(end, 5, "false") == 0)
            end += 5;
        else
        {
            while (end < j.size() && (std::isdigit((unsigned char)j[end]) || j[end] == '.' || j[end] == '-' ||
                                      j[end] == '+' || j[end] == 'e' || j[end] == 'E'))
                end++;
        }
        j.replace(start, end - start, lit);
        return true;
    }
    const size_t rb = j.rfind('}');
    if (rb == std::string::npos)
        return false;
    const bool need_comma = rb > 0 && j[rb - 1] != '{';
    const std::string ins = std::string(need_comma ? "," : "") + kq + ":" + lit;
    j.insert(rb, ins);
    return true;
}

static bool replace_or_insert_json_number(std::string &j, const char *key, float v)
{
    if (!key || key[0] == '\0')
        return false;
    char numbuf[64];
    std::snprintf(numbuf, sizeof(numbuf), "%.6g", (double)v);
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = j.find(kq);
    if (p != std::string::npos)
    {
        const size_t colon = j.find(':', p);
        if (colon == std::string::npos)
            return false;
        size_t start = colon + 1;
        while (start < j.size() && std::isspace((unsigned char)j[start]))
            start++;
        size_t end = start;
        while (end < j.size() && (std::isdigit((unsigned char)j[end]) || j[end] == '.' || j[end] == '-' ||
                                  j[end] == '+' || j[end] == 'e' || j[end] == 'E'))
            end++;
        j.replace(start, end - start, numbuf);
        return true;
    }
    const size_t rb = j.rfind('}');
    if (rb == std::string::npos)
        return false;
    const bool need_comma = rb > 0 && j[rb - 1] != '{';
    const std::string ins = std::string(need_comma ? "," : "") + kq + ":" + numbuf;
    j.insert(rb, ins);
    return true;
}

static bool replace_or_insert_json_string(std::string &j, const char *key, const std::string &val)
{
    if (!key || key[0] == '\0')
        return false;
    std::string esc;
    esc.reserve(val.size() + 8);
    for (char c : val)
    {
        if (c == '\\' || c == '"')
            esc.push_back('\\');
        esc.push_back(c);
    }
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = j.find(kq);
    if (p != std::string::npos)
    {
        const size_t colon = j.find(':', p);
        if (colon == std::string::npos)
            return false;
        size_t start = colon + 1;
        while (start < j.size() && std::isspace((unsigned char)j[start]))
            start++;
        if (start >= j.size() || j[start] != '"')
            return false;
        size_t end = start + 1;
        while (end < j.size())
        {
            if (j[end] == '"' && j[end - 1] != '\\')
                break;
            end++;
        }
        if (end >= j.size())
            return false;
        const std::string rep = std::string("\"") + esc + "\"";
        j.replace(start, end - start + 1, rep);
        return true;
    }
    const size_t rb = j.rfind('}');
    if (rb == std::string::npos)
        return false;
    const bool need_comma = rb > 0 && j[rb - 1] != '{';
    const std::string ins = std::string(need_comma ? "," : "") + kq + ":\"" + esc + "\"";
    j.insert(rb, ins);
    return true;
}

/** Insert or replace a JSON value that is a literal (array/object), not a quoted string. */
static bool replace_or_insert_json_raw_value(std::string &j, const char *key, const std::string &raw)
{
    if (!key || key[0] == '\0')
        return false;
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = j.find(kq);
    if (p != std::string::npos)
    {
        p = j.find(':', p);
        if (p == std::string::npos)
            return false;
        p++;
        while (p < j.size() && std::isspace((unsigned char)j[p]))
            p++;
        if (p >= j.size())
            return false;
        const char open = j[p];
        if (open == '[' || open == '{')
        {
            char close = (open == '[') ? ']' : '}';
            int depth = 0;
            size_t q = p;
            for (; q < j.size(); ++q)
            {
                if (j[q] == open)
                    depth++;
                else if (j[q] == close)
                {
                    depth--;
                    if (depth == 0)
                    {
                        q++;
                        break;
                    }
                }
            }
            if (q > p)
            {
                j.replace(p, q - p, raw);
                return true;
            }
        }
        return false;
    }
    const size_t rb = j.rfind('}');
    if (rb == std::string::npos)
        return false;
    const bool need_comma = rb > 0 && j[rb - 1] != '{';
    const std::string ins = std::string(need_comma ? "," : "") + kq + ":" + raw;
    j.insert(rb, ins);
    return true;
}

/** True when merged vendor JSON is missing hailo-apps OCR core keys (partial patch or empty). */
static bool ocr_vendor_json_needs_schema_defaults(const std::string &j)
{
    return j.empty() || j.find("\"det_bin_thresh\"") == std::string::npos;
}

/**
 * Push @c HalOcrDetectionPostConfig into vendor JSON (hailo-apps @c ocr_postprocess schema).
 * Recognition-side keys are set to neutral defaults so a detection-only HEF still validates.
 */
static void merge_hal_ocr_detection_struct_into_json(std::string &j, const HalOcrDetectionPostConfig *c)
{
    if (!c)
        return;
    if (j.empty())
        j = "{}";
    (void)replace_or_insert_json_number(j, "det_bin_thresh", c->det_bin_thresh);
    (void)replace_or_insert_json_number(j, "det_box_thresh", c->det_box_thresh);
    (void)replace_or_insert_json_number(j, "det_unclip_ratio", c->det_unclip_ratio);
    (void)replace_or_insert_json_number(j, "det_max_candidates", (float)c->det_max_candidates);
    (void)replace_or_insert_json_number(j, "det_min_box_size", c->det_min_box_size);
    (void)replace_or_insert_json_number(j, "det_map_h", (float)c->det_map_h);
    (void)replace_or_insert_json_number(j, "det_map_w", (float)c->det_map_w);
    (void)replace_or_insert_json_bool(j, "letterbox_fix", c->letterbox_fix);
    (void)replace_or_insert_json_string(j, "det_output_name", std::string(c->det_output_name));
    (void)replace_or_insert_json_string(j, "rec_output_name", std::string(""));
    (void)replace_or_insert_json_string(j, "charset_path", std::string(""));
    (void)replace_or_insert_json_number(j, "blank_index", 0.f);
    (void)replace_or_insert_json_bool(j, "logits_are_softmax", false);
    (void)replace_or_insert_json_bool(j, "time_major", false);
    (void)replace_or_insert_json_number(j, "text_conf_smooth", 0.9f);
    (void)replace_or_insert_json_bool(j, "attach_caption_box", false);
    (void)replace_or_insert_json_string(j, "frequency_dict_path", std::string(""));
    (void)replace_or_insert_json_number(j, "max_edit_distance", 2.f);
    (void)replace_or_insert_json_number(j, "charset_index_offset", 0.f);
}

/**
 * Push @c HalOcrRecognitionPostConfig plus fixed detection defaults (same as @c OcrParams in hailo-apps).
 */
static void merge_hal_ocr_recognition_struct_into_json(std::string &j, const HalOcrRecognitionPostConfig *c)
{
    if (!c)
        return;
    if (j.empty())
        j = "{}";
    (void)replace_or_insert_json_number(j, "det_bin_thresh", 0.3f);
    (void)replace_or_insert_json_number(j, "det_box_thresh", 0.15f);
    (void)replace_or_insert_json_number(j, "det_unclip_ratio", 3.0f);
    (void)replace_or_insert_json_number(j, "det_max_candidates", 100.f);
    (void)replace_or_insert_json_number(j, "det_min_box_size", 1.0f);
    (void)replace_or_insert_json_number(j, "det_map_h", 960.f);
    (void)replace_or_insert_json_number(j, "det_map_w", 544.f);
    (void)replace_or_insert_json_bool(j, "letterbox_fix", true);
    (void)replace_or_insert_json_string(j, "det_output_name", std::string(""));
    (void)replace_or_insert_json_string(j, "rec_output_name", std::string(c->rec_output_name));
    (void)replace_or_insert_json_string(j, "charset_path", std::string(c->charset_path));
    (void)replace_or_insert_json_number(j, "charset_index_offset", (float)c->charset_index_offset);
    (void)replace_or_insert_json_number(j, "blank_index", (float)c->blank_index);
    (void)replace_or_insert_json_bool(j, "logits_are_softmax", c->logits_are_softmax);
    (void)replace_or_insert_json_bool(j, "time_major", c->time_major);
    (void)replace_or_insert_json_number(j, "text_conf_smooth", c->text_conf_smooth);
    (void)replace_or_insert_json_bool(j, "attach_caption_box", c->attach_caption_box);
    (void)replace_or_insert_json_string(j, "frequency_dict_path", std::string(c->frequency_dict_path));
    (void)replace_or_insert_json_number(j, "max_edit_distance", (float)c->max_edit_distance);
}

/** Only merge struct into vendor JSON when the app filled prompt-related fields (avoids overwriting JSON-only configs with init defaults). */
static bool clip_struct_has_prompt_fields(const HalClipPostprocessConfig *c)
{
    if (!c)
        return false;
    if (c->positive_prompt[0] != '\0')
        return true;
    if (c->num_negative_prompts > 0U)
        return true;
    if (c->num_zero_shot_prompts > 0U)
        return true;
    if (c->semantic_type[0] != '\0')
        return true;
    return false;
}

static void merge_hal_clip_struct_into_json(std::string &j, const HalClipPostprocessConfig *c)
{
    if (!c)
        return;
    if (!clip_struct_has_prompt_fields(c))
        return;
    if (j.empty())
        j = "{}";
    (void)replace_or_insert_json_number(j, "top_k", (float)c->top_k);
    (void)replace_or_insert_json_number(j, "score_threshold", c->score_threshold);
    {
        const char *pol = "softmax";
        if (c->match_policy == HAL_CLIP_MATCH_MARGIN)
            pol = "margin";
        else if (c->match_policy == HAL_CLIP_MATCH_POS_ONLY)
            pol = "pos_only";
        (void)replace_or_insert_json_string(j, "match_policy", std::string(pol));
    }
    if (c->semantic_type[0] != '\0')
        (void)replace_or_insert_json_string(j, "type", std::string(c->semantic_type));
    if (c->positive_prompt[0] != '\0')
        (void)replace_or_insert_json_string(j, "positive_prompt", std::string(c->positive_prompt));
    if (c->num_negative_prompts > 0U)
    {
        std::string arr = "[";
        for (uint32_t i = 0; i < c->num_negative_prompts && i < HAL_MAX_CLIP_NEGATIVE_PROMPTS; i++)
        {
            if (i)
                arr += ",";
            arr += "\"";
            for (const char *s = c->negative_prompts[i]; s && *s; ++s)
            {
                if (*s == '\\' || *s == '"')
                    arr += '\\';
                arr += *s;
            }
            arr += "\"";
        }
        arr += "]";
        (void)replace_or_insert_json_raw_value(j, "negative_prompts", arr);
    }
    if (c->num_zero_shot_prompts > 0U)
    {
        std::string arr = "[";
        const uint32_t n = c->num_zero_shot_prompts;
        for (uint32_t i = 0; i < n && i < HAL_MAX_CLASSES; i++)
        {
            if (i)
                arr += ",";
            arr += "\"";
            for (const char *s = c->zero_shot_prompts[i]; s && *s; ++s)
            {
                if (*s == '\\' || *s == '"')
                    arr += '\\';
                arr += *s;
            }
            arr += "\"";
        }
        arr += "]";
        (void)replace_or_insert_json_raw_value(j, "prompts", arr);
    }
}

} // namespace

extern "C" {

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)

static inline std::string json_extract_string_value_best_effort(const std::string &json, const char *key,
                                                                const char *def)
{
    const std::string v = json_extract_string_best_effort(json, key);
    if (!v.empty())
        return v;
    return def ? std::string(def) : std::string();
}

/**
 * Vendor @c init(config_path, fn) expects a JSON file that matches the **plugin's** schema (YOLO/CLIP/OCR…).
 * HAL often merges `backend_lib_path` / `backend_function` / `backend_config_path` into the same blob; passing
 * that whole blob as @c config_path makes @c libyolo_post.so validate it and throw (e.g. unknown keys / schema).
 * Strip those loader keys before persisting to the temp path passed to @c init().
 */
static std::string json_strip_hailo_postprocess_loader_keys(std::string j)
{
    static const char *kLoaderKeys[] = {"backend_lib_path", "backend_function", "backend_config_path"};
    for (const char *key : kLoaderKeys)
    {
        const std::string kq = std::string("\"") + key + "\"";
        for (;;)
        {
            const size_t kp = j.find(kq);
            if (kp == std::string::npos)
                break;

            size_t erase_from = kp;
            while (erase_from > 0 && std::isspace((unsigned char)j[erase_from - 1]))
                erase_from--;
            if (erase_from > 0 && j[erase_from - 1] == ',')
                erase_from--;

            size_t colon = j.find(':', kp + kq.size());
            if (colon == std::string::npos)
                break;
            size_t i = colon + 1;
            while (i < j.size() && std::isspace((unsigned char)j[i]))
                i++;
            size_t val_end = i;
            if (i < j.size() && j[i] == '"')
            {
                val_end = i + 1;
                while (val_end < j.size())
                {
                    if (j[val_end] == '\\' && val_end + 1 < j.size())
                    {
                        val_end += 2;
                        continue;
                    }
                    if (j[val_end] == '"')
                    {
                        val_end++;
                        break;
                    }
                    val_end++;
                }
            }
            else
            {
                while (val_end < j.size() && j[val_end] != ',' && j[val_end] != '}' && j[val_end] != ']' &&
                       !std::isspace((unsigned char)j[val_end]))
                    val_end++;
            }

            while (val_end < j.size() && std::isspace((unsigned char)j[val_end]))
                val_end++;
            if (val_end < j.size() && j[val_end] == ',')
                val_end++;
            while (val_end < j.size() && std::isspace((unsigned char)j[val_end]))
                val_end++;

            j.erase(erase_from, val_end - erase_from);
        }
    }

    for (size_t p = 0; (p = j.find(",,", p)) != std::string::npos;)
        j.erase(p, 1);
    for (size_t p = 0; (p = j.find(",}", p)) != std::string::npos;)
        j.erase(p, 1);
    for (size_t p = 0; (p = j.find(",]", p)) != std::string::npos;)
        j.erase(p, 1);
    for (size_t p = 0; (p = j.find("{,", p)) != std::string::npos;)
        j.erase(p + 1, 1);
    for (size_t p = 0; (p = j.find("[,", p)) != std::string::npos;)
        j.erase(p + 1, 1);
    return j;
}
#endif

static HalPostprocessSession *hailo15_post_create(const HalPostprocessConfig *config)
{
    if (!config)
        return nullptr;
    auto *p = new (std::nothrow) Hailo15PostPriv();
    if (!p)
        return nullptr;
    p->cfg = *config;

    {
        const char *cf = nullptr;
        if (config->type == HAL_POST_TYPE_DETECTION)
            cf = config->config.detection.config_file;
        else if (config->type == HAL_POST_TYPE_CLASSIFICATION)
            cf = config->config.classification.config_file;
        else if (config->type == HAL_POST_TYPE_CLIP)
            cf = config->config.clip.config_file;
        else if (config->type == HAL_POST_TYPE_SEGMENTATION)
            cf = config->config.segmentation.config_file;
        else if (config->type == HAL_POST_TYPE_KEYPOINT)
            cf = config->config.keypoint.config_file;
        else if (config->type == HAL_POST_TYPE_EMBEDDING)
            cf = config->config.embedding.config_file;
        else if (config->type == HAL_POST_TYPE_OCR_DETECTION)
            cf = config->config.ocr_detection.config_file;
        else if (config->type == HAL_POST_TYPE_OCR_RECOGNITION)
            cf = config->config.ocr_recognition.config_file;
        else if (config->type == HAL_POST_TYPE_DEPTH)
            cf = config->config.depth.config_file;

        std::string json_cfg;
        if (cf && cf[0] != '\0')
            json_cfg = read_file_to_string(cf);
        if (json_cfg.empty())
        {
            const char *cj = nullptr;
            if (config->type == HAL_POST_TYPE_DETECTION)
                cj = config->config.detection.config_json;
            else if (config->type == HAL_POST_TYPE_CLASSIFICATION)
                cj = config->config.classification.config_json;
            else if (config->type == HAL_POST_TYPE_CLIP)
                cj = config->config.clip.config_json;
            else if (config->type == HAL_POST_TYPE_SEGMENTATION)
                cj = config->config.segmentation.config_json;
            else if (config->type == HAL_POST_TYPE_KEYPOINT)
                cj = config->config.keypoint.config_json;
            else if (config->type == HAL_POST_TYPE_EMBEDDING)
                cj = config->config.embedding.config_json;
            else if (config->type == HAL_POST_TYPE_OCR_DETECTION)
                cj = config->config.ocr_detection.config_json;
            else if (config->type == HAL_POST_TYPE_OCR_RECOGNITION)
                cj = config->config.ocr_recognition.config_json;
            else if (config->type == HAL_POST_TYPE_DEPTH)
                cj = config->config.depth.config_json;
            if (str_has_json_object_prefix(cj))
                json_cfg = cj;
        }
        p->merged_vendor_json = std::move(json_cfg);
        // If both config_file and config_json are provided, treat config_json as a patch on top of the file.
        {
            const char *cj = nullptr;
            if (config->type == HAL_POST_TYPE_DETECTION)
                cj = config->config.detection.config_json;
            else if (config->type == HAL_POST_TYPE_CLASSIFICATION)
                cj = config->config.classification.config_json;
            else if (config->type == HAL_POST_TYPE_CLIP)
                cj = config->config.clip.config_json;
            else if (config->type == HAL_POST_TYPE_SEGMENTATION)
                cj = config->config.segmentation.config_json;
            else if (config->type == HAL_POST_TYPE_KEYPOINT)
                cj = config->config.keypoint.config_json;
            else if (config->type == HAL_POST_TYPE_EMBEDDING)
                cj = config->config.embedding.config_json;
            else if (config->type == HAL_POST_TYPE_OCR_DETECTION)
                cj = config->config.ocr_detection.config_json;
            else if (config->type == HAL_POST_TYPE_OCR_RECOGNITION)
                cj = config->config.ocr_recognition.config_json;
            else if (config->type == HAL_POST_TYPE_DEPTH)
                cj = config->config.depth.config_json;
            merge_vendor_patch_json_best_effort(p, cj);
        }
        if (config->type == HAL_POST_TYPE_CLIP)
            merge_hal_clip_struct_into_json(p->merged_vendor_json, &config->config.clip);
        if (config->type == HAL_POST_TYPE_OCR_DETECTION &&
            ocr_vendor_json_needs_schema_defaults(p->merged_vendor_json))
        {
            merge_hal_ocr_detection_struct_into_json(p->merged_vendor_json, &config->config.ocr_detection);
            merge_vendor_patch_json_best_effort(p, config->config.ocr_detection.config_json);
        }
        else if (config->type == HAL_POST_TYPE_OCR_RECOGNITION &&
                 ocr_vendor_json_needs_schema_defaults(p->merged_vendor_json))
        {
            merge_hal_ocr_recognition_struct_into_json(p->merged_vendor_json, &config->config.ocr_recognition);
            merge_vendor_patch_json_best_effort(p, config->config.ocr_recognition.config_json);
        }
        if (!p->merged_vendor_json.empty())
            repoint_config_json(&p->cfg, p->merged_vendor_json.c_str());
        refresh_labels_from_vendor_json(p);
        refresh_hal_ocr_structs_from_merged_json(p);
    }

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
    if (config->type == HAL_POST_TYPE_KEYPOINT)
    {
        bool native_pose = false;
        if (extract_json_bool_after_key(p->merged_vendor_json, "native_yolov8_pose", &native_pose))
            p->yolov8_pose_builtin = native_pose;
    }

    /* Built-in Paddle OCR unless JSON explicitly requests an external plugin (both backend_* set). */
    if (config->type == HAL_POST_TYPE_OCR_DETECTION || config->type == HAL_POST_TYPE_OCR_RECOGNITION)
    {
        p->ocr_builtin = true;
        const std::string bl = json_extract_string_best_effort(p->merged_vendor_json, "backend_lib_path");
        const std::string bf = json_extract_string_best_effort(p->merged_vendor_json, "backend_function");
        if (!bl.empty() && !bf.empty())
            p->ocr_builtin = false;
        if (config->type == HAL_POST_TYPE_OCR_RECOGNITION && p->ocr_builtin)
            hal_v2::internal_ocr::load_recognition_charset(p->cfg.config.ocr_recognition, p->ocr_rec_charset);
    }

    // CLIP zero-shot: initialize text encoder + prompt scorer
    if (config->type == HAL_POST_TYPE_CLIP)
    {
        auto *enc = new (std::nothrow) hal_v2::HalClipTextEncoder();
        if (enc)
        {
            int irc = enc->init();
            if (irc == HAL_OK && enc->is_ready())
            {
                auto *scorer = new (std::nothrow) hal_v2::HalClipPromptScorer(*enc);
                if (scorer)
                {
                    scorer->configure(config->config.clip);
                    p->clip_text_encoder = enc;
                    p->clip_scorer = scorer;
                    HAL_LOG_INFO("CLIP zero-shot scorer ready (text encoder dim=%u)", (unsigned)enc->embedding_dim());
                }
                else
                {
                    delete enc;
                }
            }
            else
            {
                HAL_LOG_WARNING("CLIP text encoder init failed: %d (zero-shot disabled)", irc);
                delete enc;
            }
        }
    }

    // Plugin selection contract:
    // - Prefer JSON file at config_file with keys: backend_lib_path, backend_function, backend_config_path.
    // - If config_file is missing/unreadable, accept platform_config as an in-memory JSON blob.
    // - If still missing, use known defaults for common types.
    auto choose_defaults = [&]() {
        switch (p->cfg.type)
        {
            case HAL_POST_TYPE_DETECTION:
                p->plugin_lib_path = "/usr/lib/hailo-post-processes/libyolo_hailortpp_post.so";
                p->plugin_function = "hailo_yolov8n";
                break;
            case HAL_POST_TYPE_KEYPOINT:
                // V1 default: mediapipe facial landmarks on NV12.
                p->plugin_lib_path = "/usr/lib/hailo-post-processes/libmediapipe_post.so";
                p->plugin_function = "facial_landmarks_nv12";
                break;
            case HAL_POST_TYPE_SEGMENTATION:
                // V1 default: linknet post (class mask).
                p->plugin_lib_path = "/usr/lib/hailo-post-processes/liblinknet_post.so";
                p->plugin_function = "linknet_post";
                break;
            case HAL_POST_TYPE_CLIP:
                p->plugin_lib_path = "/usr/lib/hailo-post-processes/libclipgen_post.so";
                p->plugin_function = "clip_vit_b_32";
                break;
            case HAL_POST_TYPE_CLASSIFICATION:
                // Best-effort: default to CLIP plugin if a dedicated classifier plugin isn't configured.
                p->plugin_lib_path = "/usr/lib/hailo-post-processes/libclip_post.so";
                p->plugin_function = "clip_resnet_50_nv12";
                break;
            case HAL_POST_TYPE_OCR_DETECTION:
                if (!p->ocr_builtin)
                {
                    p->plugin_lib_path = "/usr/lib/hailo-post-processes/libocr_post.so";
                    p->plugin_function = "paddleocr_det";
                }
                break;
            case HAL_POST_TYPE_OCR_RECOGNITION:
                if (!p->ocr_builtin)
                {
                    p->plugin_lib_path = "/usr/lib/hailo-post-processes/libocr_post.so";
                    p->plugin_function = "paddleocr_recognize";
                }
                break;
            case HAL_POST_TYPE_DEPTH:
                break;
            default:
                break;
        }
    };

    const std::string &json_cfg = p->merged_vendor_json;

    if (!json_cfg.empty())
    {
        p->plugin_lib_path = json_extract_string_value_best_effort(json_cfg, "backend_lib_path", "");
        p->plugin_function = json_extract_string_value_best_effort(json_cfg, "backend_function", "");
        p->plugin_config_path = json_extract_string_value_best_effort(json_cfg, "backend_config_path", "");
    }
    if ((p->plugin_lib_path.empty() || p->plugin_function.empty()) && !p->yolov8_pose_builtin &&
        p->cfg.type != HAL_POST_TYPE_DEPTH)
        choose_defaults();

    // Ensure merged JSON patch wins over defaults (especially when only backend_function is provided).
    if (!json_cfg.empty())
    {
        const std::string bl = json_extract_string_value_best_effort(json_cfg, "backend_lib_path", "");
        const std::string bf = json_extract_string_value_best_effort(json_cfg, "backend_function", "");
        const std::string bc = json_extract_string_value_best_effort(json_cfg, "backend_config_path", "");
        if (!bl.empty())
            p->plugin_lib_path = bl;
        if (!bf.empty())
            p->plugin_function = bf;
        if (!bc.empty())
            p->plugin_config_path = bc;
    }

    if (p->yolov8_pose_builtin || p->cfg.type == HAL_POST_TYPE_DEPTH)
    {
        p->plugin_lib_path.clear();
        p->plugin_function.clear();
        p->plugin_config_path.clear();
    }

    // Compatibility: many vendor postprocess plugins (including CLIP) expect *their* full config
    // in a file path passed to init(config_path, backend_function). Our CLI commonly provides a single
    // in-memory JSON blob with prompts/top_k/etc. If backend_config_path is not provided, but we did
    // receive a JSON blob, persist it to a temp file and pass that path to init().
    if (!p->yolov8_pose_builtin && p->cfg.type != HAL_POST_TYPE_DEPTH && p->plugin_config_path.empty() && !json_cfg.empty())
    {
        p->temp_config_path = make_temp_json_path("vendor");
        const std::string plugin_only_json = json_strip_hailo_postprocess_loader_keys(json_cfg);
        if (write_string_to_file(p->temp_config_path.c_str(), plugin_only_json))
        {
            p->plugin_config_path = p->temp_config_path;
        }
        else
        {
            p->temp_config_path.clear();
        }
    }

    HAL_LOG_INFO("hailo15_postprocess: type=%d backend_lib_path=\"%s\" backend_function=\"%s\" backend_config_path=\"%s\"%s",
                 (int)p->cfg.type,
                 p->plugin_lib_path.c_str(),
                 p->plugin_function.c_str(),
                 p->plugin_config_path.c_str(),
                 (!p->temp_config_path.empty() ? " (temp)" : ""));
    if (p->ocr_builtin && (p->cfg.type == HAL_POST_TYPE_OCR_DETECTION || p->cfg.type == HAL_POST_TYPE_OCR_RECOGNITION))
        HAL_LOG_INFO("hailo15_postprocess: OCR using built-in Paddle det/rec (set backend_lib_path + backend_function for external .so)");
    if (p->yolov8_pose_builtin && p->cfg.type == HAL_POST_TYPE_KEYPOINT)
        HAL_LOG_INFO("hailo15_postprocess: keypoint using built-in YOLOv8-Pose (native_yolov8_pose=true; no vendor .so)");
    if (p->cfg.type == HAL_POST_TYPE_DEPTH)
        HAL_LOG_INFO("hailo15_postprocess: depth using built-in SCDepthV3-style decode (no vendor .so)");

    if (!p->plugin_lib_path.empty() && !p->plugin_function.empty())
    {
        p->dl_handle = dlopen(p->plugin_lib_path.c_str(), RTLD_LAZY);
        if (!p->dl_handle)
        {
            HAL_LOG_ERROR("hailo15_postprocess: dlopen failed for %s: %s",
                          p->plugin_lib_path.c_str(), dlerror());
        }
        else
        {
            // Optional init/free_resources used by some Hailo plugins.
            dlerror();
            auto init_sym = (void *(*)(std::string, std::string))dlsym(p->dl_handle, "init");
            if (init_sym)
            {
                p->post_params = init_sym(p->plugin_config_path, p->plugin_function);
                p->post_free = (void (*)(void *))dlsym(p->dl_handle, "free_resources");
            }
            // Prefer two-arg (HailoROIPtr, void*), then single-arg (HailoROIPtr).
            p->post_fn = (void (*)(HailoROIPtr, void *))dlsym(p->dl_handle, p->plugin_function.c_str());
            if (!p->post_fn)
            {
                void (*one_arg)(HailoROIPtr) =
                    (void (*)(HailoROIPtr))dlsym(p->dl_handle, p->plugin_function.c_str());
                if (one_arg)
                    p->post_fn_no_params = one_arg;
            }
            if (!p->post_fn && !p->post_fn_no_params)
            {
                HAL_LOG_ERROR("hailo15_postprocess: dlsym failed for %s: %s",
                              p->plugin_function.c_str(), dlerror());
            }
        }
    }
#endif

    return reinterpret_cast<HalPostprocessSession *>(p);
}

static void hailo15_post_destroy(HalPostprocessSession *session)
{
    if (!session)
        return;
    auto *p = reinterpret_cast<Hailo15PostPriv *>(session);

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
    if (p->post_free && p->post_params)
        p->post_free(p->post_params);
    p->post_params = nullptr;
    p->post_fn = nullptr;
    p->post_fn_no_params = nullptr;
    p->post_free = nullptr;
    if (p->dl_handle)
        dlclose(p->dl_handle);
    p->dl_handle = nullptr;
    if (!p->temp_config_path.empty())
        std::remove(p->temp_config_path.c_str());
    p->temp_config_path.clear();
    delete p->clip_scorer;
    p->clip_scorer = nullptr;
    delete p->clip_text_encoder;
    p->clip_text_encoder = nullptr;
#endif

    delete p;
}

static int run_vendor_plugin(Hailo15PostPriv *p, const HalTensor *outputs, int num_outputs,
                             HalPostprocessResult *result)
{
#if !defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
    (void)p;
    (void)outputs;
    (void)num_outputs;
    (void)result;
    return HAL_ERR_NOT_SUPPORTED;
#else
    if (!p || !outputs || num_outputs <= 0 || !result)
        return HAL_ERR_INVALID_ARG;
    if (!outputs[0].priv)
        return HAL_ERR_NOT_READY;

    // Contract (Hailo-aligned): hailo15 inference attaches ROI to outputs[0].priv.
    const auto *hp = static_cast<const hal_v2::hailo15::TensorPriv *>(outputs[0].priv);
    if (!hp || !hp->roi)
        return HAL_ERR_NOT_READY;

    if (p->yolov8_pose_builtin && p->cfg.type == HAL_POST_TYPE_KEYPOINT)
    {
        std::memset(result, 0, sizeof(*result));
        result->type = HAL_POST_TYPE_KEYPOINT;
        result->priv = nullptr;
        const int rc = hal_v2::internal_yolov8_pose::run(p->cfg, hp->roi, &result->result.keypoint);
        result->result.keypoint.priv = (void *)&hp->roi;
        return rc;
    }

    if (p->cfg.type == HAL_POST_TYPE_DEPTH)
    {
        std::memset(result, 0, sizeof(*result));
        result->type = HAL_POST_TYPE_DEPTH;
        result->priv = nullptr;
        const int rc = hal_v2::internal_scdepth::run(p->cfg, hp->roi, &result->result.depth);
        result->result.depth.priv = (void *)&hp->roi;
        return rc;
    }

    /* Built-in Paddle OCR (no dlopen): decode tensors directly into HAL structs. */
    if (p->ocr_builtin && p->cfg.type == HAL_POST_TYPE_OCR_DETECTION)
    {
        std::memset(result, 0, sizeof(*result));
        result->type = HAL_POST_TYPE_OCR_DETECTION;
        result->priv = nullptr;
        const int rc =
            hal_v2::internal_ocr::run_detection(p->cfg, hp->roi, &result->result.detection);
        result->result.detection.priv = (void *)&hp->roi;
        return rc;
    }
    if (p->ocr_builtin && p->cfg.type == HAL_POST_TYPE_OCR_RECOGNITION)
    {
        std::memset(result, 0, sizeof(*result));
        result->type = HAL_POST_TYPE_OCR_RECOGNITION;
        result->priv = nullptr;
        const int rc = hal_v2::internal_ocr::run_recognition(p->cfg, hp->roi, p->ocr_rec_charset,
                                                             &result->result.ocr);
        result->result.ocr.priv = (void *)&hp->roi;
        return rc;
    }

    if (!p->dl_handle || (!p->post_fn && !p->post_fn_no_params))
        return HAL_ERR_NOT_SUPPORTED;

    // Run plugin
    if (p->post_fn_no_params)
        p->post_fn_no_params(hp->roi);
    else
        p->post_fn(hp->roi, p->post_params);

    std::memset(result, 0, sizeof(*result));
    result->priv = nullptr;

    // Translate objects back to HAL results.
    switch (p->cfg.type)
    {
        case HAL_POST_TYPE_DETECTION:
        {
            result->type = HAL_POST_TYPE_DETECTION;
            auto &out = result->result.detection;
            out.num_detections = 0;
            out.priv = (void *)&hp->roi;
            for (auto &obj : hp->roi->get_objects_typed(HAILO_DETECTION))
            {
                if (out.num_detections >= HAL_MAX_DETECTIONS)
                    break;
                auto det = std::dynamic_pointer_cast<HailoDetection>(obj);
                if (!det)
                    continue;
                HalDetection d{};
                const HailoBBox bb = det->get_bbox();
                d.bbox = HalBBox{bb.xmin(), bb.ymin(), bb.width(), bb.height()};
                d.confidence = det->get_confidence();
                d.class_id = det->get_class_id();
                d.track_id = -1;
                const std::string lbl = det->get_label();
                if (!lbl.empty())
                {
                    std::snprintf(d.label, sizeof(d.label), "%s", lbl.c_str());
                }
                else if (p && !p->labels.empty())
                {
                    // Fallback: map class_id to vendor JSON labels[] if plugin doesn't provide label text.
                    const int32_t cid = d.class_id;
                    auto in_range = [&](int32_t i) { return i >= 0 && (size_t)i < p->labels.size(); };
                    int32_t li = cid;
                    if (!in_range(li) && p->label_offset != 0 && in_range(cid + p->label_offset))
                        li = cid + p->label_offset;
                    else if (!in_range(li) && p->label_offset != 0 && in_range(cid - p->label_offset))
                        li = cid - p->label_offset;
                    if (in_range(li))
                        std::snprintf(d.label, sizeof(d.label), "%s", p->labels[(size_t)li].c_str());
                }
                out.detections[out.num_detections++] = d;
            }
            return HAL_OK;
        }
        case HAL_POST_TYPE_CLASSIFICATION:
        {
            result->type = HAL_POST_TYPE_CLASSIFICATION;
            auto &out = result->result.classification;
            out.num_classes = 0;
            out.top1_class_id = -1;
            out.priv = (void *)&hp->roi;
            for (auto &obj : hp->roi->get_objects_typed(HAILO_CLASSIFICATION))
            {
                if (out.num_classes >= HAL_MAX_CLASSES)
                    break;
                auto cls = std::dynamic_pointer_cast<HailoClassification>(obj);
                if (!cls)
                    continue;
                HalClassification c{};
                c.class_id = cls->get_class_id();
                const std::string type = cls->get_classification_type();
                const std::string lbl = cls->get_label();
                std::snprintf(c.type, sizeof(c.type), "%s", type.c_str());
                std::snprintf(c.label, sizeof(c.label), "%s", lbl.c_str());
                c.confidence = cls->get_confidence();
                if (out.top1_class_id < 0)
                    out.top1_class_id = c.class_id;
                out.classes[out.num_classes++] = c;
            }
            return HAL_OK;
        }
        case HAL_POST_TYPE_CLIP:
        {
            result->type = HAL_POST_TYPE_CLIP;
            auto &out = result->result.classification;
            out.num_classes = 0;
            out.top1_class_id = -1;
            out.priv = (void *)&hp->roi;

            // 1. Check if vendor plugin already added classifications
            for (auto &obj : hp->roi->get_objects_typed(HAILO_CLASSIFICATION))
            {
                if (out.num_classes >= HAL_MAX_CLASSES)
                    break;
                auto cls = std::dynamic_pointer_cast<HailoClassification>(obj);
                if (!cls)
                    continue;
                HalClassification c{};
                c.class_id = cls->get_class_id();
                const std::string type = cls->get_classification_type();
                const std::string lbl = cls->get_label();
                std::snprintf(c.type, sizeof(c.type), "%s", type.c_str());
                std::snprintf(c.label, sizeof(c.label), "%s", lbl.c_str());
                c.confidence = cls->get_confidence();
                if (out.top1_class_id < 0)
                    out.top1_class_id = c.class_id;
                out.classes[out.num_classes++] = c;
            }

            // 2. If no classifications, extract embedding and run zero-shot scoring
            if (out.num_classes == 0 && p->clip_scorer && p->clip_scorer->ready())
            {
                for (auto &obj : hp->roi->get_objects_typed(HAILO_MATRIX))
                {
                    auto mat = std::dynamic_pointer_cast<HailoMatrix>(obj);
                    if (!mat)
                        continue;
                    const auto &vec = mat->get_data();
                    std::vector<float> image_emb(vec.begin(), vec.end());

                    // Store embedding via priv for downstream consumers (grpc_service)
                    if (!vec.empty())
                    {
                        auto *emb_copy = new (std::nothrow) std::vector<float>(vec.begin(), vec.end());
                        if (emb_copy)
                            out.priv = emb_copy;
                    }

                    HalPostprocessResult score_result{};
                    const int rc = p->clip_scorer->score_zero_shot_top1(image_emb, score_result);
                    if (rc == HAL_OK)
                    {
                        auto &sr = score_result.result.classification;
                        for (uint32_t i = 0; i < sr.num_classes && out.num_classes < HAL_MAX_CLASSES; i++)
                        {
                            out.classes[out.num_classes] = sr.classes[i];
                            if (out.top1_class_id < 0)
                                out.top1_class_id = sr.classes[i].class_id;
                            out.num_classes++;
                        }
                    }
                    break; // Only process first matrix
                }
            }
            else if (out.num_classes == 0)
            {
                // No scorer — still extract raw embedding from HailoMatrix
                for (auto &obj : hp->roi->get_objects_typed(HAILO_MATRIX))
                {
                    auto mat = std::dynamic_pointer_cast<HailoMatrix>(obj);
                    if (!mat)
                        continue;
                    const auto &vec = mat->get_data();
                    if (!vec.empty())
                    {
                        auto *emb_copy = new (std::nothrow) std::vector<float>(vec.begin(), vec.end());
                        if (emb_copy)
                            out.priv = emb_copy;
                    }
                    break;
                }
            }
            return HAL_OK;
        }
        case HAL_POST_TYPE_EMBEDDING:
        {
            result->type = HAL_POST_TYPE_EMBEDDING;
            auto &out = result->result.embedding;
            out.dim = 0;
            out.priv = (void *)&hp->roi;

            for (auto &obj : hp->roi->get_objects_typed(HAILO_MATRIX))
            {
                auto mat = std::dynamic_pointer_cast<HailoMatrix>(obj);
                if (!mat)
                    continue;

                const auto &vec = mat->get_data();
                const uint32_t n = (uint32_t)((vec.size() > HAL_MAX_EMBEDDING_DIM) ? HAL_MAX_EMBEDDING_DIM : vec.size());
                out.dim = n;
                for (uint32_t i = 0; i < n; i++)
                    out.data[i] = vec[i];

                if (p->cfg.config.embedding.normalize && n > 0)
                {
                    double sum = 0.0;
                    for (uint32_t i = 0; i < n; i++)
                        sum += (double)out.data[i] * (double)out.data[i];
                    const double norm = std::sqrt(sum);
                    if (norm > 1e-12)
                    {
                        const float inv = (float)(1.0 / norm);
                        for (uint32_t i = 0; i < n; i++)
                            out.data[i] *= inv;
                    }
                }

                return (n > 0) ? HAL_OK : HAL_ERR_NOT_READY;
            }

            return HAL_ERR_NOT_READY;
        }
        case HAL_POST_TYPE_KEYPOINT:
        {
            result->type = HAL_POST_TYPE_KEYPOINT;
            auto &out = result->result.keypoint;
            out.num_objects = 0;
            out.num_links = 0;
            out.priv = (void *)&hp->roi;
            for (auto &obj : hp->roi->get_objects_typed(HAILO_LANDMARKS))
            {
                if (out.num_objects >= HAL_MAX_DETECTIONS)
                    break;
                auto lm = std::dynamic_pointer_cast<HailoLandmarks>(obj);
                if (!lm)
                    continue;
                HalKeypointObject ko{};
                const auto pts = lm->get_points();
                uint32_t n = static_cast<uint32_t>(pts.size());
                if (n > HAL_MAX_KEYPOINTS)
                    n = HAL_MAX_KEYPOINTS;
                ko.num_keypoints = n;
                ko.bbox = HalBBox{0, 0, 0, 0};
                ko.confidence = 1.0f;
                ko.class_id = 0;
                ko.track_id = -1;
                for (uint32_t i = 0; i < n; i++)
                {
                    ko.keypoints[i] = HalPoint2D{pts[i].x(), pts[i].y(), pts[i].confidence()};
                }
                out.objects[out.num_objects++] = ko;
            }
            return HAL_OK;
        }
        case HAL_POST_TYPE_SEGMENTATION:
        {
            // Best-effort mapping: merge HAILO_CLASS_MASK objects into a single uint8 mask.
            // Some postprocesses (e.g., linknet_post) may attach multiple class masks (one per output slice).
            result->type = HAL_POST_TYPE_SEGMENTATION;
            auto &out = result->result.segmentation;
            out.width = 0;
            out.height = 0;
            out.num_classes = 0;
            out.mask_data = nullptr;
            out.confidence_map = nullptr;
            out.priv = nullptr;

            uint8_t *merged = nullptr;
            size_t merged_sz = 0;

            for (auto &obj : hp->roi->get_objects())
            {
                if (obj->get_type() != HAILO_CLASS_MASK)
                    continue;
                auto cm = std::dynamic_pointer_cast<HailoClassMask>(obj);
                if (!cm)
                    continue;

                const int w = cm->get_width();
                const int h = cm->get_height();
                if (w <= 0 || h <= 0)
                    continue;
                const size_t sz = static_cast<size_t>(w) * static_cast<size_t>(h);
                if (!merged)
                {
                    merged = static_cast<uint8_t *>(std::malloc(sz));
                    if (!merged)
                        return HAL_ERR_NO_MEM;
                    std::memcpy(merged, cm->get_data(), sz);
                    merged_sz = sz;
                    out.width = static_cast<uint32_t>(w);
                    out.height = static_cast<uint32_t>(h);
                }
                else
                {
                    if (sz != merged_sz)
                        continue;
                    uint8_t *src = cm->get_data();
                    for (size_t i = 0; i < merged_sz; i++)
                        merged[i] = std::max<uint8_t>(merged[i], src[i]);
                }
                out.num_classes++;
            }
            if (!merged)
                return HAL_ERR_NOT_READY;
            out.mask_data = merged;
            out.priv = merged; // free_result will free
            return HAL_OK;
        }
        case HAL_POST_TYPE_OCR_DETECTION:
        {
            result->type = HAL_POST_TYPE_OCR_DETECTION;
            auto &out = result->result.detection;
            out.num_detections = 0;
            out.priv = (void *)&hp->roi;
            const float min_c = p->cfg.config.ocr_detection.min_confidence;
            for (auto &obj : hp->roi->get_objects_typed(HAILO_DETECTION))
            {
                if (out.num_detections >= HAL_MAX_DETECTIONS)
                    break;
                auto det = std::dynamic_pointer_cast<HailoDetection>(obj);
                if (!det)
                    continue;
                HalDetection d{};
                const HailoBBox bb = det->get_bbox();
                d.bbox = HalBBox{bb.xmin(), bb.ymin(), bb.width(), bb.height()};
                d.confidence = det->get_confidence();
                d.class_id = det->get_class_id();
                d.track_id = -1;
                const std::string lbl = det->get_label();
                if (!lbl.empty())
                {
                    std::snprintf(d.label, sizeof(d.label), "%s", lbl.c_str());
                }
                else if (p && !p->labels.empty())
                {
                    const int32_t cid = d.class_id;
                    auto in_range = [&](int32_t i) { return i >= 0 && (size_t)i < p->labels.size(); };
                    int32_t li = cid;
                    if (!in_range(li) && p->label_offset != 0 && in_range(cid + p->label_offset))
                        li = cid + p->label_offset;
                    else if (!in_range(li) && p->label_offset != 0 && in_range(cid - p->label_offset))
                        li = cid - p->label_offset;
                    if (in_range(li))
                        std::snprintf(d.label, sizeof(d.label), "%s", p->labels[(size_t)li].c_str());
                }
                if (min_c > 0.f && d.confidence < min_c)
                    continue;
                out.detections[out.num_detections++] = d;
            }
            return HAL_OK;
        }
        case HAL_POST_TYPE_OCR_RECOGNITION:
        {
            /* CTC / DB decode and bbox unprojection are done in the vendor plugin; HAL only
             * flattens HailoDetection + nested HailoClassification (or root classification) into
             * HalOcrResult. */
            result->type = HAL_POST_TYPE_OCR_RECOGNITION;
            auto &out = result->result.ocr;
            out.num_lines = 0;
            out.priv = (void *)&hp->roi;
            const float min_c = p->cfg.config.ocr_recognition.min_confidence;

            for (auto &obj : hp->roi->get_objects_typed(HAILO_DETECTION))
            {
                if (out.num_lines >= HAL_MAX_OCR_LINES)
                    break;
                auto det = std::dynamic_pointer_cast<HailoDetection>(obj);
                if (!det)
                    continue;

                HalOcrLine line{};
                const HailoBBox bb = det->get_bbox();
                line.bbox = HalBBox{bb.xmin(), bb.ymin(), bb.width(), bb.height()};
                line.track_id = -1;
                line.confidence = det->get_confidence();
                line.text[0] = '\0';

                for (auto &cobj : det->get_objects_typed(HAILO_CLASSIFICATION))
                {
                    auto cls = std::dynamic_pointer_cast<HailoClassification>(cobj);
                    if (!cls)
                        continue;
                    const std::string lbl = cls->get_label();
                    std::snprintf(line.text, sizeof(line.text), "%s", lbl.c_str());
                    line.confidence = cls->get_confidence();
                    break;
                }

                if (line.text[0] == '\0')
                {
                    const std::string dl = det->get_label();
                    if (!dl.empty() && dl != "text_region")
                        std::snprintf(line.text, sizeof(line.text), "%s", dl.c_str());
                }

                if (min_c > 0.f && line.confidence < min_c)
                    continue;

                out.lines[out.num_lines++] = line;
            }

            if (out.num_lines == 0)
            {
                for (auto &obj : hp->roi->get_objects_typed(HAILO_CLASSIFICATION))
                {
                    if (out.num_lines >= HAL_MAX_OCR_LINES)
                        break;
                    auto cls = std::dynamic_pointer_cast<HailoClassification>(obj);
                    if (!cls)
                        continue;
                    HalOcrLine line{};
                    line.bbox = HalBBox{0.0f, 0.0f, 1.0f, 1.0f};
                    line.track_id = -1;
                    line.confidence = cls->get_confidence();
                    const std::string lbl = cls->get_label();
                    std::snprintf(line.text, sizeof(line.text), "%s", lbl.c_str());
                    if (min_c > 0.f && line.confidence < min_c)
                        continue;
                    out.lines[out.num_lines++] = line;
                }
            }

            return HAL_OK;
        }
        case HAL_POST_TYPE_DEPTH:
            return HAL_ERR_NOT_SUPPORTED;
        default:
            return HAL_ERR_NOT_SUPPORTED;
    }
    #endif
}

static int hailo15_post_run(HalPostprocessSession *session,
                            const HalTensor *outputs, int num_outputs,
                            HalPostprocessResult *result)
{
    if (!session || !outputs || num_outputs <= 0 || !result)
        return HAL_ERR_INVALID_ARG;
    std::memset(result, 0, sizeof(*result));
    auto *p = reinterpret_cast<Hailo15PostPriv *>(session);

    return run_vendor_plugin(p, outputs, num_outputs, result);
}

static void hailo15_post_free_result(HalPostprocessResult *result)
{
    if (!result)
        return;
    if (result->type == HAL_POST_TYPE_SEGMENTATION)
    {
        if (result->result.segmentation.mask_data)
        {
            std::free(result->result.segmentation.mask_data);
        }
        result->result.segmentation.mask_data = nullptr;
        result->result.segmentation.confidence_map = nullptr;
        result->result.segmentation.priv = nullptr;
    }
    if (result->type == HAL_POST_TYPE_DEPTH)
    {
        if (result->result.depth.depth_m)
            std::free(result->result.depth.depth_m);
        result->result.depth.depth_m = nullptr;
        result->result.depth.priv = nullptr;
    }
    std::memset(result, 0, sizeof(*result));
}

static void dyn_zero(HalPostprocessResultDyn *r)
{
    if (!r)
        return;
    std::memset(r, 0, sizeof(*r));
    r->type = HAL_POST_TYPE_NONE;
}

static void hailo15_post_free_result_dyn(HalPostprocessResultDyn *result)
{
    if (!result)
        return;
    switch (result->type)
    {
        case HAL_POST_TYPE_DETECTION:
        case HAL_POST_TYPE_OCR_DETECTION:
            std::free(result->result.detection.detections);
            break;
        case HAL_POST_TYPE_CLASSIFICATION:
        case HAL_POST_TYPE_CLIP:
            std::free(result->result.classification.classes);
            break;
        case HAL_POST_TYPE_SEGMENTATION:
            std::free(result->result.segmentation.mask_data);
            std::free(result->result.segmentation.confidence_map);
            break;
        case HAL_POST_TYPE_KEYPOINT:
        {
            auto &kp = result->result.keypoint;
            if (kp.objects)
            {
                for (uint32_t i = 0; i < kp.num_objects; i++)
                    std::free(kp.objects[i].keypoints);
            }
            std::free(kp.objects);
            std::free(kp.links);
            break;
        }
        case HAL_POST_TYPE_EMBEDDING:
            std::free(result->result.embedding.data);
            break;
        case HAL_POST_TYPE_OCR_RECOGNITION:
            std::free(result->result.ocr.lines);
            break;
        case HAL_POST_TYPE_DEPTH:
            std::free(result->result.depth.depth_m);
            break;
        default:
            break;
    }
    dyn_zero(result);
}

static int hailo15_post_run_dyn(HalPostprocessSession *session,
                                const HalTensor *outputs, int num_outputs,
                                HalPostprocessResultDyn *result)
{
    if (!result)
        return HAL_ERR_INVALID_ARG;
    dyn_zero(result);

    // Use existing static run, but avoid stack allocation of the large union.
    auto *tmp = static_cast<HalPostprocessResult *>(std::malloc(sizeof(HalPostprocessResult)));
    if (!tmp)
        return HAL_ERR_NO_MEM;
    std::memset(tmp, 0, sizeof(*tmp));
    const int rc = hailo15_post_run(session, outputs, num_outputs, tmp);
    if (rc != HAL_OK)
    {
        std::free(tmp);
        return rc;
    }

    result->type = tmp->type;
    result->priv = tmp->priv;

    switch (tmp->type)
    {
        case HAL_POST_TYPE_DETECTION:
        case HAL_POST_TYPE_OCR_DETECTION:
        {
            const uint32_t n = tmp->result.detection.num_detections;
            result->result.detection.num_detections = n;
            result->result.detection.priv = tmp->result.detection.priv;
            if (n > 0)
            {
                result->result.detection.detections =
                    static_cast<HalDetection *>(std::malloc(sizeof(HalDetection) * (size_t)n));
                if (!result->result.detection.detections)
                {
                    std::free(tmp);
                    hailo15_post_free_result_dyn(result);
                    return HAL_ERR_NO_MEM;
                }
                std::memcpy(result->result.detection.detections, tmp->result.detection.detections,
                            sizeof(HalDetection) * (size_t)n);
            }
            break;
        }
        case HAL_POST_TYPE_CLASSIFICATION:
        case HAL_POST_TYPE_CLIP:
        {
            const uint32_t n = tmp->result.classification.num_classes;
            result->result.classification.num_classes = n;
            result->result.classification.top1_class_id = tmp->result.classification.top1_class_id;
            result->result.classification.priv = tmp->result.classification.priv;
            if (n > 0)
            {
                result->result.classification.classes =
                    static_cast<HalClassification *>(std::malloc(sizeof(HalClassification) * (size_t)n));
                if (!result->result.classification.classes)
                {
                    std::free(tmp);
                    hailo15_post_free_result_dyn(result);
                    return HAL_ERR_NO_MEM;
                }
                std::memcpy(result->result.classification.classes, tmp->result.classification.classes,
                            sizeof(HalClassification) * (size_t)n);
            }
            break;
        }
        case HAL_POST_TYPE_SEGMENTATION:
        {
            result->result.segmentation.width = tmp->result.segmentation.width;
            result->result.segmentation.height = tmp->result.segmentation.height;
            result->result.segmentation.num_classes = tmp->result.segmentation.num_classes;
            result->result.segmentation.priv = tmp->result.segmentation.priv;
            const size_t wh = (size_t)tmp->result.segmentation.width * (size_t)tmp->result.segmentation.height;
            if (tmp->result.segmentation.mask_data && wh > 0)
            {
                result->result.segmentation.mask_data = static_cast<uint8_t *>(std::malloc(wh));
                if (!result->result.segmentation.mask_data)
                {
                    std::free(tmp);
                    hailo15_post_free_result_dyn(result);
                    return HAL_ERR_NO_MEM;
                }
                std::memcpy(result->result.segmentation.mask_data, tmp->result.segmentation.mask_data, wh);
            }
            if (tmp->result.segmentation.confidence_map && wh > 0 && tmp->result.segmentation.num_classes > 0)
            {
                const size_t nfloat = wh * (size_t)tmp->result.segmentation.num_classes;
                result->result.segmentation.confidence_map =
                    static_cast<float *>(std::malloc(sizeof(float) * nfloat));
                if (!result->result.segmentation.confidence_map)
                {
                    std::free(tmp);
                    hailo15_post_free_result_dyn(result);
                    return HAL_ERR_NO_MEM;
                }
                std::memcpy(result->result.segmentation.confidence_map, tmp->result.segmentation.confidence_map,
                            sizeof(float) * nfloat);
            }
            break;
        }
        case HAL_POST_TYPE_KEYPOINT:
        {
            auto &src = tmp->result.keypoint;
            auto &dst = result->result.keypoint;
            dst.num_objects = src.num_objects;
            dst.num_links = src.num_links;
            dst.priv = src.priv;
            if (dst.num_objects > 0)
            {
                dst.objects = static_cast<HalKeypointObjectDyn *>(std::calloc(dst.num_objects, sizeof(HalKeypointObjectDyn)));
                if (!dst.objects)
                {
                    std::free(tmp);
                    hailo15_post_free_result_dyn(result);
                    return HAL_ERR_NO_MEM;
                }
                for (uint32_t oi = 0; oi < dst.num_objects; oi++)
                {
                    const auto &so = src.objects[oi];
                    auto &do0 = dst.objects[oi];
                    do0.num_keypoints = so.num_keypoints;
                    do0.bbox = so.bbox;
                    do0.confidence = so.confidence;
                    do0.class_id = so.class_id;
                    do0.track_id = so.track_id;
                    if (do0.num_keypoints > 0)
                    {
                        do0.keypoints = static_cast<HalPoint2D *>(std::malloc(sizeof(HalPoint2D) * (size_t)do0.num_keypoints));
                        if (!do0.keypoints)
                        {
                            std::free(tmp);
                            hailo15_post_free_result_dyn(result);
                            return HAL_ERR_NO_MEM;
                        }
                        std::memcpy(do0.keypoints, so.keypoints, sizeof(HalPoint2D) * (size_t)do0.num_keypoints);
                    }
                }
            }
            if (dst.num_links > 0)
            {
                dst.links = static_cast<HalSkeletonLink *>(std::malloc(sizeof(HalSkeletonLink) * (size_t)dst.num_links));
                if (!dst.links)
                {
                    std::free(tmp);
                    hailo15_post_free_result_dyn(result);
                    return HAL_ERR_NO_MEM;
                }
                std::memcpy(dst.links, src.links, sizeof(HalSkeletonLink) * (size_t)dst.num_links);
            }
            break;
        }
        case HAL_POST_TYPE_EMBEDDING:
        {
            result->result.embedding.dim = tmp->result.embedding.dim;
            result->result.embedding.priv = tmp->result.embedding.priv;
            const uint32_t dim = tmp->result.embedding.dim;
            if (dim > 0)
            {
                result->result.embedding.data = static_cast<float *>(std::malloc(sizeof(float) * (size_t)dim));
                if (!result->result.embedding.data)
                {
                    std::free(tmp);
                    hailo15_post_free_result_dyn(result);
                    return HAL_ERR_NO_MEM;
                }
                std::memcpy(result->result.embedding.data, tmp->result.embedding.data, sizeof(float) * (size_t)dim);
            }
            break;
        }
        case HAL_POST_TYPE_OCR_RECOGNITION:
        {
            const uint32_t n = tmp->result.ocr.num_lines;
            result->result.ocr.num_lines = n;
            result->result.ocr.priv = tmp->result.ocr.priv;
            if (n > 0)
            {
                result->result.ocr.lines =
                    static_cast<HalOcrLine *>(std::malloc(sizeof(HalOcrLine) * (size_t)n));
                if (!result->result.ocr.lines)
                {
                    std::free(tmp);
                    hailo15_post_free_result_dyn(result);
                    return HAL_ERR_NO_MEM;
                }
                std::memcpy(result->result.ocr.lines, tmp->result.ocr.lines, sizeof(HalOcrLine) * (size_t)n);
            }
            break;
        }
        case HAL_POST_TYPE_DEPTH:
        {
            result->result.depth.width = tmp->result.depth.width;
            result->result.depth.height = tmp->result.depth.height;
            result->result.depth.priv = tmp->result.depth.priv;
            const size_t wh = (size_t)tmp->result.depth.width * (size_t)tmp->result.depth.height;
            if (tmp->result.depth.depth_m && wh > 0)
            {
                result->result.depth.depth_m = static_cast<float *>(std::malloc(sizeof(float) * wh));
                if (!result->result.depth.depth_m)
                {
                    std::free(tmp);
                    hailo15_post_free_result_dyn(result);
                    return HAL_ERR_NO_MEM;
                }
                std::memcpy(result->result.depth.depth_m, tmp->result.depth.depth_m, sizeof(float) * wh);
            }
            break;
        }
        default:
            std::free(tmp);
            hailo15_post_free_result_dyn(result);
            return HAL_ERR_NOT_SUPPORTED;
    }

    // Free tmp (and any internal allocations).
    hailo15_post_free_result(tmp);
    std::free(tmp);
    return HAL_OK;
}

static const char *hailo15_post_get_version(void)
{
    return "Hailo15 HAL-POSTPROCESS (vendor plugin)";
}

static int hailo15_post_apply_config_json(HalPostprocessSession *session, const char *patch_json)
{
    if (!session || !patch_json || !str_has_json_object_prefix(patch_json))
        return HAL_ERR_INVALID_ARG;
    auto *p = reinterpret_cast<Hailo15PostPriv *>(session);
    const std::string patch(patch_json);
    bool changed = false;

    if (p->merged_vendor_json.empty())
        p->merged_vendor_json = "{}";

    static const char *kNumericKeys[] = {
        /* YOLO / detection (see hailo-analytics apps/webserver configs yolov5*.json, yolov8n_personface.json) */
        "detection_threshold",
        "confidence_threshold",
        "iou_threshold",
        "nms_threshold",
        "max_boxes",
        "max_detections",
        "label_offset",
        /* Classification / CLIP */
        "score_threshold",
        "top_k",
        /* Segmentation / keypoint */
        "output_width",
        "output_height",
        "keypoint_threshold",
        "num_keypoints",
        "min_confidence",
        "det_bin_thresh",
        "det_box_thresh",
        "det_unclip_ratio",
        "det_max_candidates",
        "det_min_box_size",
        "det_map_h",
        "det_map_w",
        "text_conf_smooth",
        "max_edit_distance",
        "charset_index_offset",
        "blank_index",
    };

    for (const char *key : kNumericKeys)
    {
        const float v = extract_json_float_after_key(patch, key);
        if (!std::isfinite(v))
            continue;
        if (!replace_or_insert_json_number(p->merged_vendor_json, key, v))
            continue;
        changed = true;
        sync_hal_postprocess_scalar(&p->cfg, p->cfg.type, key, v);
    }

    {
        bool nb = false;
        if (extract_json_bool_after_key(patch, "normalize", &nb))
        {
            if (replace_or_insert_json_bool(p->merged_vendor_json, "normalize", nb))
            {
                changed = true;
                if (p->cfg.type == HAL_POST_TYPE_EMBEDDING)
                    p->cfg.config.embedding.normalize = nb;
            }
        }
    }

    {
        bool df = false;
        if (extract_json_bool_after_key(patch, "depth_float32", &df) && p->cfg.type == HAL_POST_TYPE_DEPTH)
        {
            if (replace_or_insert_json_bool(p->merged_vendor_json, "depth_float32", df))
                changed = true;
        }
    }

    {
        bool lb = false;
        if (extract_json_bool_after_key(patch, "letterbox_fix", &lb) &&
            (p->cfg.type == HAL_POST_TYPE_OCR_DETECTION || p->cfg.type == HAL_POST_TYPE_OCR_RECOGNITION))
        {
            if (replace_or_insert_json_bool(p->merged_vendor_json, "letterbox_fix", lb))
            {
                changed = true;
                if (p->cfg.type == HAL_POST_TYPE_OCR_DETECTION)
                    p->cfg.config.ocr_detection.letterbox_fix = lb;
            }
        }
    }
    {
        bool b1 = false;
        if (extract_json_bool_after_key(patch, "logits_are_softmax", &b1) &&
            p->cfg.type == HAL_POST_TYPE_OCR_RECOGNITION)
        {
            if (replace_or_insert_json_bool(p->merged_vendor_json, "logits_are_softmax", b1))
            {
                changed = true;
                p->cfg.config.ocr_recognition.logits_are_softmax = b1;
            }
        }
    }
    {
        bool b2 = false;
        if (extract_json_bool_after_key(patch, "time_major", &b2) && p->cfg.type == HAL_POST_TYPE_OCR_RECOGNITION)
        {
            if (replace_or_insert_json_bool(p->merged_vendor_json, "time_major", b2))
            {
                changed = true;
                p->cfg.config.ocr_recognition.time_major = b2;
            }
        }
    }
    {
        bool b3 = false;
        if (extract_json_bool_after_key(patch, "attach_caption_box", &b3) &&
            p->cfg.type == HAL_POST_TYPE_OCR_RECOGNITION)
        {
            if (replace_or_insert_json_bool(p->merged_vendor_json, "attach_caption_box", b3))
            {
                changed = true;
                p->cfg.config.ocr_recognition.attach_caption_box = b3;
            }
        }
    }

    {
        const std::string mp = json_extract_string_best_effort(patch, "match_policy");
        if (!mp.empty() && replace_or_insert_json_string(p->merged_vendor_json, "match_policy", mp))
            changed = true;
        if (p->cfg.type == HAL_POST_TYPE_CLIP && !mp.empty())
        {
            if (mp == "margin")
                p->cfg.config.clip.match_policy = HAL_CLIP_MATCH_MARGIN;
            else if (mp == "pos_only" || mp == "posonly")
                p->cfg.config.clip.match_policy = HAL_CLIP_MATCH_POS_ONLY;
            else
                p->cfg.config.clip.match_policy = HAL_CLIP_MATCH_SOFTMAX;
        }
    }
    {
        const std::string pp = json_extract_string_best_effort(patch, "positive_prompt");
        if (!pp.empty() && replace_or_insert_json_string(p->merged_vendor_json, "positive_prompt", pp))
            changed = true;
        if (p->cfg.type == HAL_POST_TYPE_CLIP && !pp.empty())
        {
            std::snprintf(p->cfg.config.clip.positive_prompt, sizeof(p->cfg.config.clip.positive_prompt), "%s",
                          pp.c_str());
        }
    }

    {
        const std::string oa = json_extract_string_best_effort(patch, "output_activation");
        if (!oa.empty() && replace_or_insert_json_string(p->merged_vendor_json, "output_activation", oa))
            changed = true;
    }

    // CLIP zero-shot prompts array (e.g., {"prompts": ["a cat", "a dog"]})
    {
        auto prompts = parse_string_array_from_json_key(patch, "prompts");
        if (!prompts.empty() && p->cfg.type == HAL_POST_TYPE_CLIP)
        {
            auto &c = p->cfg.config.clip;
            c.num_zero_shot_prompts = 0;
            for (size_t i = 0; i < prompts.size() && i < HAL_MAX_CLASSES; i++)
            {
                std::snprintf(c.zero_shot_prompts[i], sizeof(c.zero_shot_prompts[i]),
                              "%s", prompts[i].c_str());
                c.num_zero_shot_prompts++;
            }
            // Rebuild JSON array for vendor merge
            std::string arr = "[";
            for (size_t i = 0; i < prompts.size(); i++)
            {
                if (i > 0) arr += ",";
                // Escape inner quotes
                arr += "\"";
                for (char ch : prompts[i])
                {
                    if (ch == '"' || ch == '\\') arr += '\\';
                    arr += ch;
                }
                arr += "\"";
            }
            arr += "]";
            (void)replace_or_insert_json_raw_value(p->merged_vendor_json, "prompts", arr);
            changed = true;
        }
    }

    // CLIP negative prompts array
    {
        auto neg = parse_string_array_from_json_key(patch, "negative_prompts");
        if (!neg.empty() && p->cfg.type == HAL_POST_TYPE_CLIP)
        {
            auto &c = p->cfg.config.clip;
            c.num_negative_prompts = 0;
            for (size_t i = 0; i < neg.size() && i < HAL_MAX_CLIP_NEGATIVE_PROMPTS; i++)
            {
                std::snprintf(c.negative_prompts[i], sizeof(c.negative_prompts[i]),
                              "%s", neg[i].c_str());
                c.num_negative_prompts++;
            }
            std::string arr = "[";
            for (size_t i = 0; i < neg.size(); i++)
            {
                if (i > 0) arr += ",";
                arr += "\"";
                for (char ch : neg[i])
                {
                    if (ch == '"' || ch == '\\') arr += '\\';
                    arr += ch;
                }
                arr += "\"";
            }
            arr += "]";
            (void)replace_or_insert_json_raw_value(p->merged_vendor_json, "negative_prompts", arr);
            changed = true;
        }
    }

    if (!changed)
        return HAL_ERR_RESULT;

    repoint_config_json(&p->cfg, p->merged_vendor_json.c_str());
    refresh_labels_from_vendor_json(p);
    refresh_hal_ocr_structs_from_merged_json(p);

    // CLIP zero-shot: reconfigure scorer with updated prompts
    if (p->cfg.type == HAL_POST_TYPE_CLIP && p->clip_scorer)
    {
        HalClipPostprocessConfig merged = p->cfg.config.clip;
        hal_v2::hal_clip_postprocess_config_merge_json(&merged, p->merged_vendor_json.c_str());
        int src = p->clip_scorer->configure(merged);
        if (src != HAL_OK)
            HAL_LOG_WARNING("CLIP scorer reconfigure failed: %d", src);
        else
            HAL_LOG_INFO("CLIP scorer reconfigured (%u prompts)", merged.num_zero_shot_prompts);
    }

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)
    if (p->cfg.type == HAL_POST_TYPE_OCR_RECOGNITION && p->ocr_builtin)
        hal_v2::internal_ocr::load_recognition_charset(p->cfg.config.ocr_recognition, p->ocr_rec_charset);
    if (!p->temp_config_path.empty() && p->dl_handle)
    {
        const std::string plugin_only_json = json_strip_hailo_postprocess_loader_keys(p->merged_vendor_json);
        if (!write_string_to_file(p->temp_config_path.c_str(), plugin_only_json))
            return HAL_ERR_RESULT;
        dlerror();
        auto init_sym = (void *(*)(std::string, std::string))dlsym(p->dl_handle, "init");
        if (init_sym)
        {
            if (p->post_free && p->post_params)
                p->post_free(p->post_params);
            p->post_params = nullptr;
            p->post_params = init_sym(p->plugin_config_path, p->plugin_function);
        }
    }
#endif
    return HAL_OK;
}

HalPostprocessOps HAL_POSTPROCESS_OPS = {
    .create = hailo15_post_create,
    .destroy = hailo15_post_destroy,
    .run = hailo15_post_run,
    .free_result = hailo15_post_free_result,
    .run_dyn = hailo15_post_run_dyn,
    .free_result_dyn = hailo15_post_free_result_dyn,
    .get_version = hailo15_post_get_version,
    .apply_config_json = hailo15_post_apply_config_json,
};

} // extern "C"

