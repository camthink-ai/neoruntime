/**
 * @file hal_clip_prompt_scorer.hpp
 * @brief CLIP prompt scoring façade: HalClipTextEncoder + hal_clip_scoring + HalPostprocessResult mapping.
 *
 * Aligns with official clip app query scoring and hal_postprocess.h CLIP classification mapping.
 */

#pragma once

#include "common/hal_clip_scoring.hpp"
#include "model/hal_clip_text_encoder.hpp"
#include "model/hal_postprocess.h"

#include <mutex>
#include <string>
#include <vector>

namespace hal_v2
{

/**
 * Best-effort parse of JSON object fields into HalClipPostprocessConfig (after hal_clip_postprocess_config_init).
 * Recognizes: score_threshold, top_k, match_policy, type, positive_prompt, negative_prompts[], prompts[].
 */
int hal_clip_postprocess_config_merge_json(HalClipPostprocessConfig *cfg, const char *json_object);

/**
 * Encodes prompts from @p cfg, caches embeddings, scores normalized image embeddings.
 */
class HalClipPromptScorer
{
  public:
    explicit HalClipPromptScorer(HalClipTextEncoder &encoder);

    /** Load text side from config; returns HAL_OK on success. */
    int configure(const HalClipPostprocessConfig &cfg);

    bool ready() const { return m_ready; }
    bool positive_negative_mode() const { return m_pos_neg_mode; }

    /** Image embedding must be L2-normalized (same contract as official clip cosine path). */
    int score_normalized_image(const std::vector<float> &image_emb, HalClipScoreResult &out);

    /** Fill HAL_POST_TYPE_CLIP classification for OSD (two rows: primary + margin aux when pos/neg). */
    void fill_clip_classification_result(const HalClipScoreResult &sr, HalPostprocessResult &out);

    /** Zero-shot mode: single best prompt + confidence. */
    int score_zero_shot_top1(const std::vector<float> &image_emb, HalPostprocessResult &out);

    const HalClipPostprocessConfig &config() const { return m_cfg; }

  private:
    HalClipTextEncoder *m_enc;
    HalClipPostprocessConfig m_cfg{};
    bool m_ready = false;
    bool m_pos_neg_mode = false;

    std::vector<float> m_pos_emb;
    std::vector<std::vector<float>> m_neg_embs;
    std::vector<std::string> m_zero_shot_prompts;
    std::vector<std::vector<float>> m_zero_shot_embs;

    HalClipMatchPolicy m_policy_cpp = HalClipMatchPolicy::Softmax;
    float m_score_threshold = 0.8f;

    mutable std::mutex m_mu;
};

} // namespace hal_v2
