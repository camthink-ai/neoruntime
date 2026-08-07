/**
 * @file hal_common.h
 * @brief Common HAL Interface - Common Hardware Abstraction Layer
 *
 * Common definitions shared across all HAL modules: version info,
 * error codes, and device status enumeration.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "hal_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* HAL semantic version */
#define HAL_VERSION_MAJOR       0
#define HAL_VERSION_MINOR       1
#define HAL_VERSION_PATCH       0

/**
 * HAL error / info codes.
 * All HAL functions return 0 (HAL_OK) on success, or a negative error code.
 * A small set of POSITIVE info sentinels signal a successful call that ALSO
 * performed a heavier side effect than a plain setter. Callers MUST test
 * `ret < 0` for errors FIRST; only a negative value is an error. A positive
 * value means success + a known side effect the caller may need to react to.
 */
typedef enum {
    HAL_OK = 0,                 /* success */
    HAL_REINIT_PERFORMED = 1,   /* success + dynamic_change_image_config tore down + rebuilt the whole medialib (rotation, or flip-OOM fallback). Encoder contexts/pools are brand new and must be re-attached by the caller. */
    HAL_ERROR = -0x0AFF,        /* generic / unspecified error */
    HAL_ERR_INVALID_ARG,        /* one or more arguments are invalid */
    HAL_ERR_INVALID_STATE,      /* operation not allowed in current state */
    HAL_ERR_INVALID_FMT,        /* unsupported pixel / packet format */
    HAL_ERR_INVALID_SIZE,       /* buffer or dimension size is invalid */
    HAL_ERR_TIMEOUT,            /* operation timed out */
    HAL_ERR_NO_MEM,             /* memory allocation failed */
    HAL_ERR_NOT_FINISHED,       /* previous operation still in progress */
    HAL_ERR_NOT_SUPPORTED,      /* feature not supported on this platform */
    HAL_ERR_NOT_IMPLEMENTED,    /* function stub, not yet implemented */
    HAL_ERR_NOT_INITIALIZED,    /* module or context has not been initialized */
    HAL_ERR_NOT_READY,          /* resource exists but is not ready for use */
    HAL_ERR_MUTEX,              /* mutex lock / unlock failed */
    HAL_ERR_CHECK,              /* internal consistency check failed */
    HAL_ERR_RESULT,             /* upstream returned an unexpected result */
    HAL_ERR_NOT_FOUND,          /* requested resource / id does not exist */
    HAL_ERR_INSUFFICIENT_BUFFER,/* caller-supplied buffer is too small */
    HAL_ERR_PROFILE_RESTRICTED, /* profile rejected: thermal/power restriction (e.g. AI Denoise gated off) */
    HAL_ERR_PROFILE_INVALID,    /* profile rejected: validation against rules failed */
    HAL_ERR_UNKNOW,             /* unknown error */
} HalErrorCode;

/**
 * Device / context lifecycle status.
 */
typedef enum {
    HAL_STATUS_UNINITIALIZED = 0,   /* not yet initialized */
    HAL_STATUS_INITIALIZED,         /* initialized but not running */
    HAL_STATUS_RUNNING,             /* actively capturing / encoding */
    HAL_STATUS_STOPPED,             /* explicitly stopped after running */
    HAL_STATUS_ERROR,               /* entered an error state */
    HAL_STATUS_MAX,                 /* sentinel (not a valid status) */
} HalStatus;

/**
 * @brief Convert an error code to a human-readable string.
 * @param code The error code to convert.
 * @return Static string describing the error (never NULL).
 */
const char* hal_error_to_string(HalErrorCode code);

/**
 * @brief Get the HAL version as a "MAJOR.MINOR.PATCH" string.
 * @return Static version string (never NULL).
 */
const char* hal_get_version_string(void);

#ifdef __cplusplus
}
#endif
