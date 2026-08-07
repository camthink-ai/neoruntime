#include "common/hal_common.h"


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
