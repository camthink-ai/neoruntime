#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Opaque handle for CLIP text encoder instance.
 */
typedef struct HalClipTextEncoderHandle_ HalClipTextEncoderHandle;

/**
 * @brief CLIP text encoder ops — resolved from HAL shared library via dlsym("HAL_CLIP_TEXT_ENCODER_OPS").
 *
 * Provides a C-linkage interface to hal_v2::HalClipTextEncoder so that ai-runtime
 * can use text encoding without compile-time linking to the full HAL.
 */
typedef struct HalClipTextEncoderOps {

    /** Create and initialize a CLIP text encoder using default on-device config. Returns NULL on failure. */
    HalClipTextEncoderHandle* (*create)(void);

    /** Destroy a text encoder instance. */
    void (*destroy)(HalClipTextEncoderHandle *enc);

    /**
     * Encode a text prompt to a normalized embedding vector.
     *
     * @param enc       Encoder handle
     * @param text      Input text (UTF-8, null-terminated)
     * @param out_buf   Output buffer for embedding floats (caller-allocated, must hold at least out_dim floats)
     * @param out_dim   On entry: capacity of out_buf in floats. On success: actual embedding dimension.
     * @return 0 on success, non-zero on error
     */
    int (*encode)(HalClipTextEncoderHandle *enc, const char *text,
                  float *out_buf, uint32_t *out_dim);

    /** Query the embedding dimension without encoding. Returns 0 if not initialized. */
    uint32_t (*embedding_dim)(HalClipTextEncoderHandle *enc);

} HalClipTextEncoderOps;

/** Exported symbol in HAL shared library. */
extern HalClipTextEncoderOps HAL_CLIP_TEXT_ENCODER_OPS;

#ifdef __cplusplus
}
#endif
