/**
 * @file hal_clip_prompt_scorer.cpp
 */

#include "common/hal_clip_prompt_scorer.hpp"

#include "common/hal_common.h"

#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstring>
#include <string>

namespace hal_v2
{
namespace
{

std::vector<std::string> parse_string_array_from_json_key(const std::string &json, const char *array_key)
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

float extract_json_float_key(const std::string &json, const char *key, float default_val)
{
    if (!key)
        return default_val;
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = json.find(kq);
    if (p == std::string::npos)
        return default_val;
    p = json.find(':', p);
    if (p == std::string::npos)
        return default_val;
    p++;
    while (p < json.size() && std::isspace((unsigned char)json[p]))
        p++;
    if (p >= json.size())
        return default_val;
    char *end = nullptr;
    const float v = std::strtof(json.c_str() + p, &end);
    if (!end || end == json.c_str() + p)
        return default_val;
    return v;
}

std::string extract_json_string_key(const std::string &json, const char *key)
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
    const size_t q = json.find('"', p);
    if (q == std::string::npos)
        return {};
    return json.substr(p, q - p);
}

static void copy_trunc(char *dst, size_t dst_sz, const std::string &s)
{
    if (!dst || dst_sz == 0)
        return;
    std::snprintf(dst, dst_sz, "%s", s.c_str());
}

HalClipMatchPolicy cpp_policy_from_kind(HalClipMatchPolicyKind k)
{
    switch (k)
    {
        case HAL_CLIP_MATCH_MARGIN:
            return HalClipMatchPolicy::Margin;
        case HAL_CLIP_MATCH_POS_ONLY:
            return HalClipMatchPolicy::PosOnly;
        case HAL_CLIP_MATCH_SOFTMAX:
        default:
            return HalClipMatchPolicy::Softmax;
    }
}

} // namespace

int hal_clip_postprocess_config_merge_json(HalClipPostprocessConfig *cfg, const char *json_object)
{
    if (!cfg || !json_object || json_object[0] == '\0')
        return HAL_ERR_INVALID_ARG;
    const std::string j(json_object);

    const float st = extract_json_float_key(j, "score_threshold", cfg->score_threshold);
    cfg->score_threshold = st;

    const float tk = extract_json_float_key(j, "top_k", (float)cfg->top_k);
    if (std::isfinite(tk) && tk > 0.f)
        cfg->top_k = (uint32_t)(tk + 0.5f);

    const std::string pol = extract_json_string_key(j, "match_policy");
    if (!pol.empty())
    {
        const HalClipMatchPolicy p = hal_clip_match_policy_from_string(pol);
        if (p == HalClipMatchPolicy::Margin)
            cfg->match_policy = HAL_CLIP_MATCH_MARGIN;
        else if (p == HalClipMatchPolicy::PosOnly)
            cfg->match_policy = HAL_CLIP_MATCH_POS_ONLY;
        else
            cfg->match_policy = HAL_CLIP_MATCH_SOFTMAX;
    }

    const std::string ty = extract_json_string_key(j, "type");
    if (!ty.empty())
        copy_trunc(cfg->semantic_type, sizeof(cfg->semantic_type), ty);

    const std::string pos = extract_json_string_key(j, "positive_prompt");
    if (!pos.empty())
        copy_trunc(cfg->positive_prompt, sizeof(cfg->positive_prompt), pos);
    else
    {
        // Backward/lenient: if user passes "positive_prompt": ["a","b","c"], treat it as zero-shot prompts.
        // (The canonical multi-prompt key is "prompts".)
        const auto pos_arr = parse_string_array_from_json_key(j, "positive_prompt");
        if (!pos_arr.empty() && cfg->num_zero_shot_prompts == 0)
        {
            cfg->num_zero_shot_prompts = 0;
            for (size_t i = 0; i < pos_arr.size() && i < HAL_MAX_CLASSES; i++)
            {
                copy_trunc(cfg->zero_shot_prompts[i], sizeof(cfg->zero_shot_prompts[i]), pos_arr[i]);
                cfg->num_zero_shot_prompts++;
            }
        }
    }

    const auto negs = parse_string_array_from_json_key(j, "negative_prompts");
    cfg->num_negative_prompts = 0;
    for (size_t i = 0; i < negs.size() && i < HAL_MAX_CLIP_NEGATIVE_PROMPTS; i++)
    {
        copy_trunc(cfg->negative_prompts[i], sizeof(cfg->negative_prompts[i]), negs[i]);
        cfg->num_negative_prompts++;
    }

    const auto zp = parse_string_array_from_json_key(j, "prompts");
    if (!zp.empty())
    {
        cfg->num_zero_shot_prompts = 0;
        for (size_t i = 0; i < zp.size() && i < HAL_MAX_CLASSES; i++)
        {
            copy_trunc(cfg->zero_shot_prompts[i], sizeof(cfg->zero_shot_prompts[i]), zp[i]);
            cfg->num_zero_shot_prompts++;
        }
    }

    return HAL_OK;
}

HalClipPromptScorer::HalClipPromptScorer(HalClipTextEncoder &encoder) : m_enc(&encoder) {}

int HalClipPromptScorer::configure(const HalClipPostprocessConfig &cfg)
{
    std::lock_guard<std::mutex> lk(m_mu);
    m_cfg = cfg;
    m_ready = false;
    m_pos_neg_mode = (cfg.positive_prompt[0] != '\0');
    m_policy_cpp = cpp_policy_from_kind(cfg.match_policy);
    m_score_threshold = cfg.score_threshold;

    if (!m_enc || !m_enc->is_ready())
        return HAL_ERR_NOT_SUPPORTED;

    if (m_pos_neg_mode)
    {
        m_pos_emb.clear();
        m_neg_embs.clear();
        std::string pos(cfg.positive_prompt);
        int r = m_enc->encode_prompt(pos, m_pos_emb);
        if (r != HAL_OK)
            return r;

        std::vector<std::string> neg_strs;
        neg_strs.reserve(cfg.num_negative_prompts);
        for (uint32_t i = 0; i < cfg.num_negative_prompts && i < HAL_MAX_CLIP_NEGATIVE_PROMPTS; i++)
            neg_strs.emplace_back(cfg.negative_prompts[i]);

        r = m_enc->encode_prompts(neg_strs, m_neg_embs);
        if (r != HAL_OK)
        {
            m_pos_emb.clear();
            return r;
        }
        m_zero_shot_prompts.clear();
        m_zero_shot_embs.clear();
    }
    else
    {
        m_zero_shot_prompts.clear();
        m_zero_shot_embs.clear();
        for (uint32_t i = 0; i < cfg.num_zero_shot_prompts && i < HAL_MAX_CLASSES; i++)
        {
            if (cfg.zero_shot_prompts[i][0] == '\0')
                continue;
            m_zero_shot_prompts.emplace_back(cfg.zero_shot_prompts[i]);
        }
        if (m_zero_shot_prompts.empty())
            return HAL_ERR_INVALID_ARG;

        const int r = m_enc->encode_prompts(m_zero_shot_prompts, m_zero_shot_embs);
        if (r != HAL_OK)
            return r;
        if (m_zero_shot_embs.size() != m_zero_shot_prompts.size())
            return HAL_ERR_RESULT;
        m_pos_emb.clear();
        m_neg_embs.clear();
    }

    m_ready = true;
    return HAL_OK;
}

int HalClipPromptScorer::score_normalized_image(const std::vector<float> &image_emb, HalClipScoreResult &out)
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_ready)
        return HAL_ERR_NOT_READY;
    if (m_pos_neg_mode)
    {
        return hal_clip_score_normalized(image_emb, m_pos_emb, m_neg_embs, m_policy_cpp, m_score_threshold, out);
    }
    return HAL_ERR_NOT_SUPPORTED;
}

void HalClipPromptScorer::fill_clip_classification_result(const HalClipScoreResult &sr, HalPostprocessResult &out)
{
    std::memset(&out, 0, sizeof(out));
    out.type = HAL_POST_TYPE_CLIP;
    auto &cr = out.result.classification;

    std::string pos_label;
    {
        std::lock_guard<std::mutex> lk(m_mu);
        if (m_pos_neg_mode)
            pos_label = std::string(m_cfg.positive_prompt);
        else if (!m_zero_shot_prompts.empty())
            pos_label = m_zero_shot_prompts[0];
    }

    HalClassification &c0 = cr.classes[0];
    std::snprintf(c0.label, sizeof(c0.label), "%s", pos_label.c_str());
    if (m_pos_neg_mode)
    {
        if (m_policy_cpp == HalClipMatchPolicy::Softmax)
            c0.confidence = sr.softmax_positive_prob;
        else if (m_policy_cpp == HalClipMatchPolicy::Margin)
            c0.confidence = sr.margin;
        else
            c0.confidence = sr.pos_similarity;
    }
    else
    {
        c0.confidence = sr.pos_similarity;
    }
    std::snprintf(c0.type, sizeof(c0.type), "%s", sr.match ? "match" : "nomatch");
    c0.class_id = sr.match ? 1 : 0;

    HalClassification &c1 = cr.classes[1];
    std::snprintf(c1.label, sizeof(c1.label), "margin");
    c1.confidence = sr.margin;
    c1.class_id = 2;
    std::snprintf(c1.type, sizeof(c1.type), "aux");

    cr.num_classes = 2;
    cr.top1_class_id = 0;
}

int HalClipPromptScorer::score_zero_shot_top1(const std::vector<float> &image_emb, HalPostprocessResult &out)
{
    std::lock_guard<std::mutex> lk(m_mu);
    if (!m_ready || m_pos_neg_mode)
        return HAL_ERR_NOT_READY;
    if (m_zero_shot_embs.empty() || m_zero_shot_embs.size() != m_zero_shot_prompts.size())
        return HAL_ERR_NOT_READY;

    // Compute cosine similarities (dot on L2-normalized vectors), then convert to a probability-like score
    // using the same CLIP temperature convention used in hal_clip_score_normalized(): softmax(scale=100).
    const size_t P = m_zero_shot_prompts.size();
    std::vector<float> dots(P);
    size_t best_i = 0;
    float best_dot = -1e30f;
    for (size_t pi = 0; pi < P; pi++)
    {
        const auto &te = m_zero_shot_embs[pi];
        const size_t n = std::min<size_t>(image_emb.size(), te.size());
        float dot = 0.f;
        for (size_t k = 0; k < n; k++)
            dot += image_emb[k] * te[k];
        dots[pi] = dot;
        if (dot > best_dot)
        {
            best_dot = dot;
            best_i = pi;
        }
    }

    // Softmax probs over all prompts (stable).
    std::vector<float> probs(P, 0.f);
    if (!dots.empty())
    {
        float maxv = dots[0] * 100.0f;
        for (size_t i = 1; i < dots.size(); i++)
            maxv = std::max(maxv, dots[i] * 100.0f);
        double sum = 0.0;
        for (size_t i = 0; i < dots.size(); i++)
        {
            const double e = std::exp((double)dots[i] * 100.0 - (double)maxv);
            probs[i] = (float)e;
            sum += e;
        }
        if (sum > 1e-12)
        {
            const float inv = (float)(1.0 / sum);
            for (float &p : probs)
                p *= inv;
        }
    }

    std::memset(&out, 0, sizeof(out));
    out.type = HAL_POST_TYPE_CLIP;
    auto &cr = out.result.classification;

    // Pick top-K by probability (K from config top_k; default 5).
    const uint32_t want_k = (m_cfg.top_k > 0) ? m_cfg.top_k : 5U;
    const uint32_t k = (uint32_t)std::min<size_t>(
        (size_t)want_k, std::min<size_t>(P, (size_t)HAL_MAX_CLASSES));

    std::vector<size_t> idx(P);
    for (size_t i = 0; i < P; i++)
        idx[i] = i;
    std::partial_sort(idx.begin(), idx.begin() + std::min<size_t>(k, idx.size()), idx.end(),
                      [&](size_t a, size_t b) { return probs[a] > probs[b]; });

    for (uint32_t oi = 0; oi < k; oi++)
    {
        const size_t pi = idx[oi];
        std::snprintf(cr.classes[oi].label, sizeof(cr.classes[oi].label), "%s", m_zero_shot_prompts[pi].c_str());
        cr.classes[oi].confidence = probs[pi];
        std::snprintf(cr.classes[oi].type, sizeof(cr.classes[oi].type), "zs_softmax");
        cr.classes[oi].class_id = (int32_t)pi;
    }
    cr.num_classes = k;
    cr.top1_class_id = (int32_t)best_i;
    return HAL_OK;
}

} // namespace hal_v2
