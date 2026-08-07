/**
 * @file hal_clip_scoring.cpp
 */

#include "common/hal_clip_scoring.hpp"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <numeric>

namespace hal_v2
{
namespace
{

float dot_product(const std::vector<float> &a, const std::vector<float> &b)
{
    if (a.size() != b.size())
        return NAN;
    return std::inner_product(a.begin(), a.end(), b.begin(), 0.0f);
}

void apply_scale(std::vector<float> &scores, float scale)
{
    for (auto &s : scores)
        s *= scale;
}

void apply_softmax_inplace(std::vector<float> &scores)
{
    if (scores.empty())
        return;
    const float max_score = *std::max_element(scores.begin(), scores.end());
    float sum = 0.f;
    for (float &s : scores)
    {
        s = std::exp(s - max_score);
        sum += s;
    }
    if (sum > 1e-12f)
    {
        const float inv = 1.0f / sum;
        for (float &s : scores)
            s *= inv;
    }
}

} // namespace

HalClipMatchPolicy hal_clip_match_policy_from_string(const std::string &s)
{
    std::string t;
    t.reserve(s.size());
    for (unsigned char c : s)
        t.push_back((char)std::tolower(c));
    if (t == "margin")
        return HalClipMatchPolicy::Margin;
    if (t == "pos_only" || t == "posonly")
        return HalClipMatchPolicy::PosOnly;
    return HalClipMatchPolicy::Softmax;
}

int hal_clip_score_normalized(const std::vector<float> &image_emb, const std::vector<float> &pos_emb,
                              const std::vector<std::vector<float>> &neg_embs, HalClipMatchPolicy policy,
                              float score_threshold, HalClipScoreResult &out)
{
    out = HalClipScoreResult{};
    if (image_emb.empty() || pos_emb.empty() || image_emb.size() != pos_emb.size())
        return HAL_ERR_INVALID_ARG;
    for (const auto &n : neg_embs)
    {
        if (n.size() != image_emb.size())
            return HAL_ERR_INVALID_ARG;
    }

    const float pos_dot = dot_product(image_emb, pos_emb);
    if (!std::isfinite(pos_dot))
        return HAL_ERR_RESULT;
    out.pos_similarity = pos_dot;

    float max_neg = -1e30f;
    for (const auto &n : neg_embs)
    {
        const float d = dot_product(image_emb, n);
        if (!std::isfinite(d))
            return HAL_ERR_RESULT;
        max_neg = std::max(max_neg, d);
    }
    if (neg_embs.empty())
        max_neg = -1e30f;
    out.margin = pos_dot - max_neg;

    switch (policy)
    {
        case HalClipMatchPolicy::PosOnly:
            out.softmax_positive_prob = 1.0f;
            out.match = (pos_dot >= score_threshold);
            break;

        case HalClipMatchPolicy::Margin:
            out.softmax_positive_prob = 0.f; // not used
            if (neg_embs.empty())
                out.match = (pos_dot >= score_threshold);
            else
                out.match = (out.margin >= score_threshold);
            break;

        case HalClipMatchPolicy::Softmax:
        default:
        {
            std::vector<float> scores;
            scores.reserve(1 + neg_embs.size());
            scores.push_back(pos_dot);
            for (const auto &n : neg_embs)
                scores.push_back(dot_product(image_emb, n));
            apply_scale(scores, 100.0f);
            apply_softmax_inplace(scores);
            out.softmax_positive_prob = scores.empty() ? 0.f : scores[0];
            out.match = (out.softmax_positive_prob > score_threshold);
            break;
        }
    }

    return HAL_OK;
}

} // namespace hal_v2
