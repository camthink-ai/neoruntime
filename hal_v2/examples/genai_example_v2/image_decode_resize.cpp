/**
 * @file image_decode_resize.cpp
 * @brief STB decode + resize (same approach as hailo_model_zoo_genai).
 */

#include "image_decode_resize.hpp"

#include <stdexcept>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#define STB_IMAGE_RESIZE2_IMPLEMENTATION
#include <stb_image_resize2.h>

DecodedRgbFrame decode_and_resize_image(const std::vector<uint8_t> &encoded, uint32_t target_w, uint32_t target_h)
{
    if (encoded.empty())
        throw std::runtime_error("decode_and_resize_image: empty input");
    if (target_w == 0 || target_h == 0)
        throw std::runtime_error("decode_and_resize_image: invalid target size");

    int w = 0, h = 0, channels = 0;
    stbi_uc *decoded =
        stbi_load_from_memory(encoded.data(), static_cast<int>(encoded.size()), &w, &h, &channels, 3);
    if (!decoded)
        throw std::runtime_error("decode_and_resize_image: stbi_load_from_memory failed");

    DecodedRgbFrame out;
    out.width = target_w;
    out.height = target_h;
    out.pixels.resize(static_cast<size_t>(target_w) * static_cast<size_t>(target_h) * 3U);

    unsigned char *const resized_ptr = stbir_resize_uint8_linear(
        decoded, w, h, 0, out.pixels.data(), static_cast<int>(target_w), static_cast<int>(target_h), 0,
        static_cast<stbir_pixel_layout>(3));
    stbi_image_free(decoded);

    if (!resized_ptr)
        throw std::runtime_error("decode_and_resize_image: resize failed");

    return out;
}

DecodedRgbFrame resize_rgb888_linear(const std::vector<uint8_t> &src_rgb, uint32_t src_w, uint32_t src_h,
                                      uint32_t dst_w, uint32_t dst_h)
{
    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0)
        throw std::runtime_error("resize_rgb888_linear: invalid dimensions");
    const size_t need = static_cast<size_t>(src_w) * static_cast<size_t>(src_h) * 3U;
    if (src_rgb.size() != need)
        throw std::runtime_error("resize_rgb888_linear: buffer size mismatch");

    DecodedRgbFrame out;
    out.width = dst_w;
    out.height = dst_h;
    out.pixels.resize(static_cast<size_t>(dst_w) * static_cast<size_t>(dst_h) * 3U);

    unsigned char *const resized_ptr = stbir_resize_uint8_linear(
        src_rgb.data(), static_cast<int>(src_w), static_cast<int>(src_h), 0, out.pixels.data(),
        static_cast<int>(dst_w), static_cast<int>(dst_h), 0, static_cast<stbir_pixel_layout>(3));
    if (!resized_ptr)
        throw std::runtime_error("resize_rgb888_linear: stbir_resize_uint8_linear failed");

    return out;
}
