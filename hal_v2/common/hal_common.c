#include "common/hal_common.h"
#include "model/hal_inference.h"

#include <string.h>


const char* hal_error_to_string(HalErrorCode code)
{
    switch (code)
    {
        case HAL_OK:
            return "HAL_OK";
        case HAL_ERROR:
            return "HAL_ERROR";
        case HAL_ERR_INVALID_ARG:
            return "HAL_ERR_INVALID_ARG";
        case HAL_ERR_INVALID_STATE:
            return "HAL_ERR_INVALID_STATE";
        case HAL_ERR_INVALID_FMT:
            return "HAL_ERR_INVALID_FMT";
        case HAL_ERR_INVALID_SIZE:
            return "HAL_ERR_INVALID_SIZE";
        case HAL_ERR_TIMEOUT:
            return "HAL_ERR_TIMEOUT";
        case HAL_ERR_NO_MEM:
            return "HAL_ERR_NO_MEM";
        case HAL_ERR_NOT_FINISHED:
            return "HAL_ERR_NOT_FINISHED";
        case HAL_ERR_NOT_SUPPORTED:
            return "HAL_ERR_NOT_SUPPORTED";
        case HAL_ERR_NOT_IMPLEMENTED:
            return "HAL_ERR_NOT_IMPLEMENTED";
        case HAL_ERR_NOT_INITIALIZED:
            return "HAL_ERR_NOT_INITIALIZED";
        case HAL_ERR_NOT_READY:
            return "HAL_ERR_NOT_READY";
        case HAL_ERR_MUTEX:
            return "HAL_ERR_MUTEX";
        case HAL_ERR_CHECK:
            return "HAL_ERR_CHECK";
        case HAL_ERR_RESULT:
            return "HAL_ERR_RESULT";
        case HAL_ERR_NOT_FOUND:
            return "HAL_ERR_NOT_FOUND";
        case HAL_ERR_INSUFFICIENT_BUFFER:
            return "HAL_ERR_INSUFFICIENT_BUFFER";
        case HAL_ERR_PROFILE_RESTRICTED:
            return "HAL_ERR_PROFILE_RESTRICTED";
        case HAL_ERR_PROFILE_INVALID:
            return "HAL_ERR_PROFILE_INVALID";
        case HAL_ERR_UNKNOW:
            return "HAL_ERR_UNKNOW";
        default:
            break;
    }
    return "HAL_ERR_UNKNOWN";
}

static const char version_strings[] = {HAL_VERSION_MAJOR + '0', '.' , HAL_VERSION_MINOR + '0', '.' , HAL_VERSION_PATCH + '0', '\0'};
const char* hal_get_version_string(void)
{
    return version_strings;
}

/* ===== On-chip NMS output decoding (platform-neutral) =====
 * Layout measured on Hailo-15 / HailoRT 5.3.0 (see hal_inference.h docs):
 *   [count : float32] [count x {y_min, x_min, y_max, x_max, score : float32}]
 */
int hal_inference_decode_nms(const HalTensor *t, HalNmsDetection *out,
                             uint32_t max_count, uint32_t *count_out)
{
    if (!t || !count_out || (!out && max_count > 0U))
    {
        return HAL_ERR_INVALID_ARG;
    }
    *count_out = 0;
    if (!t->data || t->byte_size < sizeof(float))
    {
        return HAL_ERR_INVALID_ARG;
    }
    const uint8_t *b = (const uint8_t *)t->data;
    float cnt = 0.0f;
    memcpy(&cnt, b, sizeof(cnt));
    if (cnt < 0.0f || cnt > 1.0e6f)
    {
        return HAL_ERR_INVALID_SIZE;
    }
    const uint32_t n = (uint32_t)(cnt + 0.5f);
    /* Sanity: each box needs 5 floats; reject absurd counts. */
    if (((uint64_t)n * 5U * sizeof(float) + sizeof(float)) > (uint64_t)t->byte_size)
    {
        return HAL_ERR_INVALID_SIZE;
    }
    const uint32_t m = (n < max_count) ? n : max_count;
    for (uint32_t i = 0; i < m; ++i)
    {
        const float *f = (const float *)(b + sizeof(float) + (size_t)i * 5U * sizeof(float));
        out[i].y_min = f[0];
        out[i].x_min = f[1];
        out[i].y_max = f[2];
        out[i].x_max = f[3];
        out[i].score = f[4];
    }
    *count_out = m;
    return HAL_OK;
}
