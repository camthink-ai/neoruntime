/**
 * @file hal_clip_scoring.hpp
 * @brief CLIP-style scoring for L2-normalized image and text embeddings (positive / negative prompts).
 *
 * Aligns with the softmax + threshold behavior used in Hailo clip app query_service_ext (filter_with_negative_embeddings).
 */

#pragma once

#include "common/hal_common.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hal_v2
{

enum class HalClipMatchPolicy
{
    /** Match if dot(image, pos) >= score_threshold. */
    PosOnly,
    /** Match if dot(image, pos) - max_i dot(image, neg_i) >= score_threshold. */
    Margin,
    /**
     * Build [pos_dot, neg_dots...], scale by 100, softmax; match if softmax(pos) > score_threshold
     * (same structure as official clip query path).
     */
    Softmax,
};

struct HalClipScoreResult
{
    /** Cosine similarity when embeddings are L2-normalized (dot product). */
    float pos_similarity = 0.f;
    /** pos_similarity - max(negative similarities); meaningful for Margin. */
    float margin = 0.f;
    /** Softmax probability mass on the positive slot (index 0) when policy is Softmax. */
    float softmax_positive_prob = 0.f;
    bool match = false;
};

/**
 * Score a single frame embedding against normalized positive and negative text embeddings.
 *
 * @param image_emb L2-normalized image embedding
 * @param pos_emb L2-normalized positive text embedding
 * @param neg_embs Each row: L2-normalized negative text embedding (may be empty)
 * @param policy Scoring policy
 * @param score_threshold Interpreted per policy (softmax probability threshold, margin gate, or raw dot)
 * @param out Filled on success
 * @return HAL_OK, HAL_ERR_INVALID_ARG, or HAL_ERR_RESULT on dimension mismatch
 */
int hal_clip_score_normalized(const std::vector<float> &image_emb, const std::vector<float> &pos_emb,
                              const std::vector<std::vector<float>> &neg_embs, HalClipMatchPolicy policy,
                              float score_threshold, HalClipScoreResult &out);

/** Parse match_policy JSON string: "softmax" | "margin" | "pos_only" (case-insensitive). Default: Softmax. */
HalClipMatchPolicy hal_clip_match_policy_from_string(const std::string &s);

} // namespace hal_v2
