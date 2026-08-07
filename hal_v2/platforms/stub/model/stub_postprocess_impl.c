/**
 * @file stub_postprocess_impl.c
 * @brief Stub platform — HAL_POSTPROCESS_OPS (no vendor postprocess plugins).
 */

#include "common/hal_common.h"
#include "model/hal_postprocess.h"

#include <stddef.h>
#include <string.h>

struct HalPostprocessSession { int unused; };
static HalPostprocessSession stub_post_session_singleton;

static HalPostprocessSession *stub_post_create(const HalPostprocessConfig *config)
{
    (void)config;
    /* Return a valid pointer so callers can test the full lifecycle */
    return &stub_post_session_singleton;
}

static void stub_post_destroy(HalPostprocessSession *session)
{
    (void)session;
}

static int stub_post_run(HalPostprocessSession *session, const HalTensor *outputs, int num_outputs,
                         HalPostprocessResult *result)
{
    if (!session) return HAL_ERR_INVALID_ARG;
    (void)outputs;
    (void)num_outputs;
    (void)result;
    return HAL_ERR_NOT_SUPPORTED;
}

static void stub_post_free_result(HalPostprocessResult *result)
{
    (void)result;
}

static int stub_post_run_dyn(HalPostprocessSession *session, const HalTensor *outputs, int num_outputs,
                             HalPostprocessResultDyn *result)
{
    (void)session;
    (void)outputs;
    (void)num_outputs;
    if (result)
    {
        /* Keep deterministic output for callers. */
        memset(result, 0, sizeof(*result));
        result->type = HAL_POST_TYPE_NONE;
    }
    return HAL_ERR_NOT_SUPPORTED;
}

static void stub_post_free_result_dyn(HalPostprocessResultDyn *result)
{
    if (!result)
        return;
    memset(result, 0, sizeof(*result));
    result->type = HAL_POST_TYPE_NONE;
}

static const char *stub_post_get_version(void)
{
    return "HAL-POSTPROCESS stub 2.0.0 (platform stub)";
}

static int stub_post_apply_config_json(HalPostprocessSession *session, const char *patch_json)
{
    (void)session;
    (void)patch_json;
    return HAL_ERR_NOT_SUPPORTED;
}

HalPostprocessOps HAL_POSTPROCESS_OPS = {
    .create = stub_post_create,
    .destroy = stub_post_destroy,
    .run = stub_post_run,
    .free_result = stub_post_free_result,
    .run_dyn = stub_post_run_dyn,
    .free_result_dyn = stub_post_free_result_dyn,
    .get_version = stub_post_get_version,
    .apply_config_json = stub_post_apply_config_json,
};
