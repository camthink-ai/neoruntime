/**
 * @file hal_clip_text_encoder.hpp
 * @brief CLIP text encoder utility (tokenize + embedding lookup + Hailo text-encoder HEF inference).
 *
 * This aligns with the reference CLIP app under:
 * `hal_v2/doc/hailo-media-library/hailo-analytics/apps/clip/`
 * (`ClipTextEncoder`: SOS/token/EOS sequence, pad token id 0 + lookup, EOT slice, projection, L2).
 * Call `init()` with no arguments to use built-in paths from `clip_app_config.yaml` (CLIP Vit B32 on device).
 *
 * Build notes:
 * - Requires tokenizers C++ headers/libs on target (`tokenizers_cpp.h`, `libtokenizers_cpp`, `libtokenizers_c`).
 * - When tokenizers are unavailable, this component compiles but returns HAL_ERR_NOT_SUPPORTED.
 */
#pragma once

#include "model/hal_inference.h"

#include <cstdint>
#include <string>
#include <vector>

namespace hal_v2
{

/** OpenAI CLIP BPE specials — same as `apps/clip/.../clip_text_encoder.hpp` (TOKEN_START_ID / TOKEN_END_ID). */
inline constexpr uint32_t kHalClipTokenStartId = 49406;
inline constexpr uint32_t kHalClipTokenEndId = 49407;

struct HalClipTextEncoderConfig
{
    std::string hef_path;
    std::string tokenizer_json_path;
    std::string embedding_lookup_bin_path;
    std::string projection_weights_bin_path;
    std::string projection_bias_bin_path;

    // CLIP context length for text encoder (typically 77); may be overridden from HEF after init.
    uint32_t context_length = 77;

    // Expected output embedding size (e.g. 512 for ViT-B/32); optional cross-check vs projection weights.
    uint32_t embedding_size = 512;

    // Token IDs (OpenAI CLIP defaults; same as reference ClipTextEncoder).
    uint32_t token_start_id = kHalClipTokenStartId;
    uint32_t token_end_id = kHalClipTokenEndId;

    /**
     * Default resource paths aligned with `clip_app_config.yaml` (text_encoders → CLIP Vit B32 on device):
     * root `/home/root/apps/clip/resources/`.
     */
    static HalClipTextEncoderConfig default_config();
};

class HalClipTextEncoder
{
  public:
    HalClipTextEncoder() = default;
    ~HalClipTextEncoder();

    HalClipTextEncoder(const HalClipTextEncoder &) = delete;
    HalClipTextEncoder &operator=(const HalClipTextEncoder &) = delete;

    /** Initialize using `HalClipTextEncoderConfig::default_config()` (clip_app_config.yaml CLIP Vit B32 paths). */
    int init();

    int init(const HalClipTextEncoderConfig &cfg);
    void deinit();

    bool is_ready() const { return m_ready; }

    /**
     * Encode a prompt string into a normalized embedding vector.
     *
     * Thread safety: one encoder instance must not be used concurrently from multiple threads unless
     * all callers use only `encode_prompts()` for batching, or serialize externally. `encode_prompt`
     * takes an internal lock so concurrent `encode_prompt` / `encode_prompts` calls are serialized.
     *
     * @param prompt input text
     * @param out_embedding normalized float vector
     * @return HAL_OK on success
     */
    int encode_prompt(const std::string &prompt, std::vector<float> &out_embedding);

    /**
     * Encode multiple prompts under a single lock (avoids interleaving with other threads between prompts).
     * @return HAL_OK on success; on first failure returns the error and leaves @p out_embeddings partial.
     */
    int encode_prompts(const std::vector<std::string> &prompts, std::vector<std::vector<float>> &out_embeddings);

    uint32_t embedding_dim() const { return m_embedding_dim; }

  private:
    struct Impl;
    static int encode_prompt_impl(Impl *impl, const std::string &prompt, std::vector<float> &out_embedding);
    Impl *m_impl = nullptr;
    bool m_ready = false;
    uint32_t m_embedding_dim = 0;
};

} // namespace hal_v2

