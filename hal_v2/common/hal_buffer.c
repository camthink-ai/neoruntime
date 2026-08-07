/**
 * @file hal_buffer.c
 * @brief Pixel format helpers for HAL buffers.
 */

#include "common/hal_buffer.h"

uint32_t hal_pixel_format_plane_count(HalPixelFormat fmt)
{
    switch (fmt)
    {
        case HAL_PIX_FMT_NV12:
        case HAL_PIX_FMT_NV21:
            return 2;
        case HAL_PIX_FMT_YUV420P:
            return 3;
        case HAL_PIX_FMT_YUYV:
        case HAL_PIX_FMT_RGB24:
        case HAL_PIX_FMT_BGR24:
        case HAL_PIX_FMT_GRAY8:
        case HAL_PIX_FMT_RAW10:
        case HAL_PIX_FMT_RAW12:
            return 1;
        case HAL_PIX_FMT_ARGB32:
        case HAL_PIX_FMT_RGBA32:
            return 1;
        default:
            return 0;
    }
}

const char *hal_pixel_format_to_string(HalPixelFormat fmt)
{
    switch (fmt)
    {
        case HAL_PIX_FMT_NV12:
            return "NV12";
        case HAL_PIX_FMT_NV21:
            return "NV21";
        case HAL_PIX_FMT_YUV420P:
            return "YUV420P";
        case HAL_PIX_FMT_YUYV:
            return "YUYV";
        case HAL_PIX_FMT_RGB24:
            return "RGB24";
        case HAL_PIX_FMT_BGR24:
            return "BGR24";
        case HAL_PIX_FMT_ARGB32:
            return "ARGB32";
        case HAL_PIX_FMT_RGBA32:
            return "RGBA32";
        case HAL_PIX_FMT_GRAY8:
            return "GRAY8";
        case HAL_PIX_FMT_RAW10:
            return "RAW10";
        case HAL_PIX_FMT_RAW12:
            return "RAW12";
        default:
            return "UNKNOWN";
    }
}
