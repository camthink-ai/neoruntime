/**
 * @file hal_types.h
 * @brief Shared scalar / tensor type definitions for HAL v2.
 *
 * This header intentionally contains only small, platform-agnostic type
 * definitions that are used across multiple modules (model, postprocess, draw).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
 * Scalar / tensor data types
 * -------------------------------------------------------------------- */
typedef enum {
    HAL_DTYPE_UNKNOWN = 0,
    HAL_DTYPE_UINT8,
    HAL_DTYPE_INT8,
    HAL_DTYPE_UINT16,
    HAL_DTYPE_INT16,
    HAL_DTYPE_UINT32,
    HAL_DTYPE_INT32,
    HAL_DTYPE_FLOAT16,
    HAL_DTYPE_FLOAT32,
} HalDataType;

/**
 * @brief Return the element size (bytes) for a HalDataType.
 * @param t Data type.
 * @return Element size in bytes, or 0 for unknown.
 */
static inline uint32_t hal_dtype_size(HalDataType t)
{
    switch (t)
    {
        case HAL_DTYPE_UINT8:
        case HAL_DTYPE_INT8:
            return 1U;
        case HAL_DTYPE_UINT16:
        case HAL_DTYPE_INT16:
        case HAL_DTYPE_FLOAT16:
            return 2U;
        case HAL_DTYPE_UINT32:
        case HAL_DTYPE_INT32:
        case HAL_DTYPE_FLOAT32:
            return 4U;
        case HAL_DTYPE_UNKNOWN:
        default:
            return 0U;
    }
}

#ifdef __cplusplus
}
#endif

