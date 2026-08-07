/**
 * @file hal_genai.h
 * @brief HAL GenAI — LLM / VLM (HailoRT GenAI preview API)
 * @version 2.0
 *
 * Session holds one loaded model (LLM or VLM). Streaming generation invokes token callbacks
 * from the caller thread. Call hal_genai_abort_generation() from another thread to stop.
 */

#pragma once

#include "common/hal_common.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_GENAI_MAX_MODEL_PATH 512

typedef struct HalGenaiSession HalGenaiSession;

typedef enum {
    HAL_GENAI_KIND_LLM = 0,
    HAL_GENAI_KIND_VLM = 1,
} HalGenaiKind;

/**
 * @brief Creation parameters for hal_genai_create().
 */
typedef struct {
    char hef_path[HAL_GENAI_MAX_MODEL_PATH];
    HalGenaiKind kind;
    /** Optional VDevice group id (NULL or empty = default). */
    const char *vdevice_group_id;
    /** LoRA name for LLM only; NULL or empty = none. */
    const char *lora_name;
    /** When true, matches SDK client-side tokenization memory optimization (LLM/VLM). */
    bool optimize_memory_on_device;
} HalGenaiCreateParams;

/**
 * @brief Sampling / decoding parameters (maps to hailort::genai::LLMGeneratorParams).
 */
typedef struct {
    float temperature;
    float top_p;
    uint32_t top_k;
    float frequency_penalty;
    uint32_t max_generated_tokens;
    bool do_sample;
    /** If false, seed is ignored and HAILO_RANDOM_SEED is used (SDK random). */
    bool use_fixed_seed;
    uint32_t seed;
} HalGenaiGeneratorParams;

typedef enum {
    HAL_GENAI_FINISH_DONE = 0,
    HAL_GENAI_FINISH_ABORTED,
    HAL_GENAI_FINISH_MAX_TOKENS,
    HAL_GENAI_FINISH_ERROR,
} HalGenaiFinishReason;

typedef void (*HalGenaiTokenCallback)(const char *utf8_token_fragment, void *user);

typedef void (*HalGenaiFinishCallback)(HalGenaiFinishReason reason, int error_code, void *user);

/** One RGB / packed image buffer for VLM (caller-owned during hal_genai_generate_stream). */
typedef struct {
    const uint8_t *data;
    size_t byte_size;
} HalGenaiImageFrame;

/** VLM expected frame geometry (after model is loaded). */
typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t features;
    uint32_t bytes_per_frame;
    uint32_t format_type;
    uint32_t format_order;
} HalGenaiVlmInputLayout;

typedef struct {
    HalGenaiSession *(*create)(const HalGenaiCreateParams *params);
    void (*destroy)(HalGenaiSession *session);

    /** Clears on-device KV cache. Do not call while hal_genai_generate_stream is running. */
    int (*clear_context)(HalGenaiSession *session);

    /** Optional default sampling params applied before each generate if set_generator_params was not used. */
    int (*set_generator_params)(HalGenaiSession *session, const HalGenaiGeneratorParams *params);

    /**
     * Structured chat messages as JSON strings (SDK chat template), same as hailort::genai examples.
     * LLM: uses continuation optimization when the new array extends the previous prefix.
     * VLM: clears context each call; frame count must match image placeholders in messages.
     */
    int (*generate_stream)(HalGenaiSession *session,
                           const char *const *messages_json,
                           int num_messages,
                           const HalGenaiImageFrame *frames,
                           int num_frames,
                           const HalGenaiGeneratorParams *generator_params,
                           HalGenaiTokenCallback on_token,
                           void *token_user,
                           HalGenaiFinishCallback on_finish,
                           void *finish_user);

    /** Stop current generation (safe from another thread). */
    int (*abort_generation)(HalGenaiSession *session);

    /** Valid only for HAL_GENAI_KIND_VLM after create(). */
    int (*get_vlm_input_layout)(HalGenaiSession *session, HalGenaiVlmInputLayout *out);

    /**
     * Set custom stop token sequences (UTF-8). Replaces any previous list.
     * @param utf8_sequences NULL only when num_sequences==0 (clears custom stops).
     */
    int (*set_stop_tokens)(HalGenaiSession *session, const char *const *utf8_sequences, int num_sequences);

    const char *(*get_version)(void);
} HalGenaiOps;

extern HalGenaiOps HAL_GENAI_OPS;

#ifdef __cplusplus
}
#endif
