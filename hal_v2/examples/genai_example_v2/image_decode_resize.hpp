#pragma once

#include <cstdint>
#include <vector>

struct DecodedRgbFrame {
    std::vector<uint8_t> pixels;
    uint32_t width{};
    uint32_t height{};
};

/** Decode encoded image bytes (JPEG/PNG/...) and resize to WxH RGB888 row-major. */
DecodedRgbFrame decode_and_resize_image(const std::vector<uint8_t> &encoded, uint32_t target_w, uint32_t target_h);

/** RGB888 row-major resize (linear). `src_rgb.size()` must be `src_w * src_h * 3`. */
DecodedRgbFrame resize_rgb888_linear(const std::vector<uint8_t> &src_rgb, uint32_t src_w, uint32_t src_h,
                                      uint32_t dst_w, uint32_t dst_h);
