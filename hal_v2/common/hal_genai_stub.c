/**
 * @file hal_genai_stub.c
 * @brief HAL_GENAI_OPS stub when GenAI is unavailable or disabled.
 */

#include "common/hal_common.h"
#include "model/hal_genai.h"

#include <stddef.h>

static HalGenaiSession *stub_genai_create(const HalGenaiCreateParams *params)
{
    (void)params;
    return NULL;
}

static void stub_genai_destroy(HalGenaiSession *session)
{
    (void)session;
}

static int stub_genai_clear(HalGenaiSession *session)
{
    (void)session;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_genai_set_params(HalGenaiSession *session, const HalGenaiGeneratorParams *params)
{
    (void)session;
    (void)params;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_genai_generate(HalGenaiSession *session, const char *const *messages_json, int num_messages,
                               const HalGenaiImageFrame *frames, int num_frames,
                               const HalGenaiGeneratorParams *generator_params, HalGenaiTokenCallback on_token,
                               void *token_user, HalGenaiFinishCallback on_finish, void *finish_user)
{
    (void)session;
    (void)messages_json;
    (void)num_messages;
    (void)frames;
    (void)num_frames;
    (void)generator_params;
    (void)on_token;
    (void)token_user;
    if (on_finish)
        on_finish(HAL_GENAI_FINISH_ERROR, HAL_ERR_NOT_SUPPORTED, finish_user);
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_genai_abort(HalGenaiSession *session)
{
    (void)session;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_genai_layout(HalGenaiSession *session, HalGenaiVlmInputLayout *out)
{
    (void)session;
    (void)out;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_genai_stop(HalGenaiSession *session, const char *const *utf8_sequences, int num_sequences)
{
    (void)session;
    (void)utf8_sequences;
    (void)num_sequences;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_genai_save_context(HalGenaiSession *session, void **buf, size_t *len)
{
    (void)session;
    if (buf)
        *buf = NULL;
    if (len)
        *len = 0;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_genai_load_context(HalGenaiSession *session, const void *buf, size_t len)
{
    (void)session;
    (void)buf;
    (void)len;
    return HAL_ERR_NOT_SUPPORTED;
}

static int stub_genai_get_context_usage(HalGenaiSession *session, size_t *used, size_t *capacity)
{
    (void)session;
    if (used)
        *used = 0;
    if (capacity)
        *capacity = 0;
    return HAL_ERR_NOT_SUPPORTED;
}

static void stub_genai_free_context_buffer(void *buf)
{
    (void)buf;
}

static const char *stub_genai_version(void)
{
    return "HAL-GenAI stub";
}

HalGenaiOps HAL_GENAI_OPS = {
    .create = stub_genai_create,
    .destroy = stub_genai_destroy,
    .clear_context = stub_genai_clear,
    .set_generator_params = stub_genai_set_params,
    .generate_stream = stub_genai_generate,
    .abort_generation = stub_genai_abort,
    .get_vlm_input_layout = stub_genai_layout,
    .set_stop_tokens = stub_genai_stop,
    .get_version = stub_genai_version,
    /* M3 additions (appended at the table tail, after get_version) */
    .save_context = stub_genai_save_context,
    .load_context = stub_genai_load_context,
    .get_context_usage = stub_genai_get_context_usage,
    .free_context_buffer = stub_genai_free_context_buffer,
};
