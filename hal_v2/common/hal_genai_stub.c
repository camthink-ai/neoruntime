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
};
