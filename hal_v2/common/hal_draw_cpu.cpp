/**
 * @file hal_draw_cpu.cpp
 * @brief CPU drawing backend for HAL v2 (in-place on HalFrameBuffer).
 *
 * Supported formats:
 * - NV12: primitives (rect/line/circle/text) and simple mask overlay.
 *
 * This is intentionally lightweight and avoids external rendering deps.
 */

#include "common/hal_common.h"
#include "common/hal_buffer.h"
#include "common/hal_log.h"
#include "model/hal_draw.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

namespace hal_draw_cpu_internal
{

static inline uint8_t clamp_u8(int v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return static_cast<uint8_t>(v);
}

// Basic RGB->BT.601 YUV conversion (limited range not enforced).
static inline void rgb_to_yuv(uint8_t r, uint8_t g, uint8_t b, uint8_t &y, uint8_t &u, uint8_t &v)
{
    const int yi = (66 * r + 129 * g + 25 * b + 128) >> 8;
    const int ui = (-38 * r - 74 * g + 112 * b + 128) >> 8;
    const int vi = (112 * r - 94 * g - 18 * b + 128) >> 8;
    y = clamp_u8(yi + 16);
    u = clamp_u8(ui + 128);
    v = clamp_u8(vi + 128);
}

static inline bool frame_is_nv12(const HalFrameBuffer *f)
{
    return f && f->format == HAL_PIX_FMT_NV12 && f->num_planes >= 2 && f->planes[0] && f->planes[1];
}

static inline void set_nv12_pixel(HalFrameBuffer *f, int x, int y, const HalColor &c)
{
    if (!frame_is_nv12(f))
        return;
    if (x < 0 || y < 0 || x >= (int)f->width || y >= (int)f->height)
        return;
    uint8_t yy, uu, vv;
    rgb_to_yuv(c.r, c.g, c.b, yy, uu, vv);

    uint8_t *Y = static_cast<uint8_t *>(f->planes[0]);
    uint8_t *UV = static_cast<uint8_t *>(f->planes[1]);
    const uint32_t y_stride = f->strides[0] ? f->strides[0] : f->width;
    const uint32_t uv_stride = f->strides[1] ? f->strides[1] : f->width;

    Y[y * (int)y_stride + x] = yy;
    // UV is subsampled 2x2
    const int uvx = (x / 2) * 2;
    const int uvy = (y / 2);
    UV[uvy * (int)uv_stride + uvx + 0] = uu;
    UV[uvy * (int)uv_stride + uvx + 1] = vv;
}

static inline void blend_nv12_pixel(HalFrameBuffer *f, int x, int y, const HalColor &c, float alpha)
{
    if (!frame_is_nv12(f))
        return;
    if (x < 0 || y < 0 || x >= (int)f->width || y >= (int)f->height)
        return;
    alpha = std::clamp(alpha, 0.0f, 1.0f);
    if (alpha <= 0.0f)
        return;
    if (alpha >= 1.0f)
    {
        set_nv12_pixel(f, x, y, c);
        return;
    }

    uint8_t yy, uu, vv;
    rgb_to_yuv(c.r, c.g, c.b, yy, uu, vv);
    uint8_t *Y = static_cast<uint8_t *>(f->planes[0]);
    uint8_t *UV = static_cast<uint8_t *>(f->planes[1]);
    const uint32_t y_stride = f->strides[0] ? f->strides[0] : f->width;
    const uint32_t uv_stride = f->strides[1] ? f->strides[1] : f->width;

    uint8_t &Yo = Y[y * (int)y_stride + x];
    Yo = clamp_u8((int)((1.0f - alpha) * Yo + alpha * yy));

    const int uvx = (x / 2) * 2;
    const int uvy = (y / 2);
    uint8_t &Uo = UV[uvy * (int)uv_stride + uvx + 0];
    uint8_t &Vo = UV[uvy * (int)uv_stride + uvx + 1];
    Uo = clamp_u8((int)((1.0f - alpha) * Uo + alpha * uu));
    Vo = clamp_u8((int)((1.0f - alpha) * Vo + alpha * vv));
}

static void draw_line_nv12(HalFrameBuffer *f, int x0, int y0, int x1, int y1, const HalColor &c, int thickness)
{
    if (!frame_is_nv12(f))
        return;
    thickness = std::max(1, thickness);
    const int dx = std::abs(x1 - x0);
    const int sx = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;)
    {
        for (int ty = -thickness / 2; ty <= thickness / 2; ty++)
            for (int tx = -thickness / 2; tx <= thickness / 2; tx++)
                set_nv12_pixel(f, x0 + tx, y0 + ty, c);
        if (x0 == x1 && y0 == y1)
            break;
        const int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

static void draw_rect_nv12(HalFrameBuffer *f, const HalDrawRect *r)
{
    if (!frame_is_nv12(f) || !r)
        return;
    const int x0 = r->x;
    const int y0 = r->y;
    const int x1 = r->x + r->width - 1;
    const int y1 = r->y + r->height - 1;
    const int t = r->thickness;
    if (t < 0)
    {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++)
                set_nv12_pixel(f, x, y, r->color);
        return;
    }
    draw_line_nv12(f, x0, y0, x1, y0, r->color, t);
    draw_line_nv12(f, x0, y1, x1, y1, r->color, t);
    draw_line_nv12(f, x0, y0, x0, y1, r->color, t);
    draw_line_nv12(f, x1, y0, x1, y1, r->color, t);
}

// 5x7 bitmap font (ASCII 32–127). Glyphs from Adafruit-GFX-Library glcdfont.c (BSD), row-major,
// 5 px wide; MSB is the left column (matches prior digit font bit layout in draw_char_nv12).
static const uint8_t kGlcdFont5x7Rows[96][7] = {
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00},
    {0x08, 0x08, 0x08, 0x08, 0x08, 0x00, 0x08},
    {0x14, 0x14, 0x14, 0x00, 0x00, 0x00, 0x00},
    {0x14, 0x14, 0x3E, 0x14, 0x3E, 0x14, 0x14},
    {0x08, 0x1E, 0x28, 0x1C, 0x0A, 0x3C, 0x08},
    {0x30, 0x32, 0x04, 0x08, 0x10, 0x26, 0x06},
    {0x10, 0x28, 0x28, 0x10, 0x2A, 0x24, 0x1A},
    {0x0C, 0x0C, 0x08, 0x10, 0x00, 0x00, 0x00},
    {0x04, 0x08, 0x10, 0x10, 0x10, 0x08, 0x04},
    {0x10, 0x08, 0x04, 0x04, 0x04, 0x08, 0x10},
    {0x08, 0x2A, 0x1C, 0x3E, 0x1C, 0x2A, 0x08},
    {0x00, 0x08, 0x08, 0x3E, 0x08, 0x08, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C, 0x08},
    {0x00, 0x00, 0x00, 0x3E, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x0C, 0x0C},
    {0x00, 0x02, 0x04, 0x08, 0x10, 0x20, 0x00},
    {0x1C, 0x22, 0x26, 0x2A, 0x32, 0x22, 0x1C},
    {0x08, 0x18, 0x08, 0x08, 0x08, 0x08, 0x1C},
    {0x1C, 0x22, 0x02, 0x1C, 0x20, 0x20, 0x3E},
    {0x3E, 0x02, 0x04, 0x0C, 0x02, 0x22, 0x1C},
    {0x04, 0x0C, 0x14, 0x24, 0x3E, 0x04, 0x04},
    {0x3E, 0x20, 0x3C, 0x02, 0x02, 0x22, 0x1C},
    {0x0E, 0x10, 0x20, 0x3C, 0x22, 0x22, 0x1C},
    {0x3E, 0x02, 0x02, 0x04, 0x08, 0x10, 0x20},
    {0x1C, 0x22, 0x22, 0x1C, 0x22, 0x22, 0x1C},
    {0x1C, 0x22, 0x22, 0x1E, 0x02, 0x04, 0x38},
    {0x00, 0x00, 0x08, 0x00, 0x08, 0x00, 0x00},
    {0x00, 0x00, 0x08, 0x00, 0x08, 0x08, 0x10},
    {0x02, 0x04, 0x08, 0x10, 0x08, 0x04, 0x02},
    {0x00, 0x00, 0x3E, 0x00, 0x3E, 0x00, 0x00},
    {0x10, 0x08, 0x04, 0x02, 0x04, 0x08, 0x10},
    {0x1C, 0x22, 0x02, 0x0C, 0x08, 0x00, 0x08},
    {0x1C, 0x22, 0x2A, 0x2E, 0x2C, 0x20, 0x1E},
    {0x08, 0x14, 0x22, 0x22, 0x3E, 0x22, 0x22},
    {0x3C, 0x22, 0x22, 0x3C, 0x22, 0x22, 0x3C},
    {0x1C, 0x22, 0x20, 0x20, 0x20, 0x22, 0x1C},
    {0x3C, 0x22, 0x22, 0x22, 0x22, 0x22, 0x3C},
    {0x3E, 0x20, 0x20, 0x3C, 0x20, 0x20, 0x3E},
    {0x3E, 0x20, 0x20, 0x3C, 0x20, 0x20, 0x20},
    {0x1E, 0x22, 0x20, 0x20, 0x26, 0x22, 0x1E},
    {0x22, 0x22, 0x22, 0x3E, 0x22, 0x22, 0x22},
    {0x1C, 0x08, 0x08, 0x08, 0x08, 0x08, 0x1C},
    {0x0E, 0x04, 0x04, 0x04, 0x04, 0x24, 0x18},
    {0x22, 0x24, 0x28, 0x30, 0x28, 0x24, 0x22},
    {0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x3E},
    {0x22, 0x36, 0x2A, 0x2A, 0x2A, 0x22, 0x22},
    {0x22, 0x22, 0x32, 0x2A, 0x26, 0x22, 0x22},
    {0x1C, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C},
    {0x3C, 0x22, 0x22, 0x3C, 0x20, 0x20, 0x20},
    {0x1C, 0x22, 0x22, 0x22, 0x2A, 0x24, 0x1A},
    {0x3C, 0x22, 0x22, 0x3C, 0x28, 0x24, 0x22},
    {0x1C, 0x22, 0x20, 0x1C, 0x02, 0x22, 0x1C},
    {0x3E, 0x2A, 0x08, 0x08, 0x08, 0x08, 0x08},
    {0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x1C},
    {0x22, 0x22, 0x22, 0x22, 0x22, 0x14, 0x08},
    {0x22, 0x22, 0x22, 0x2A, 0x2A, 0x2A, 0x14},
    {0x22, 0x22, 0x14, 0x08, 0x14, 0x22, 0x22},
    {0x22, 0x22, 0x14, 0x08, 0x08, 0x08, 0x08},
    {0x3E, 0x02, 0x04, 0x1C, 0x10, 0x20, 0x3E},
    {0x1E, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1E},
    {0x00, 0x20, 0x10, 0x08, 0x04, 0x02, 0x00},
    {0x1E, 0x02, 0x02, 0x02, 0x02, 0x02, 0x1E},
    {0x08, 0x14, 0x22, 0x00, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x3E},
    {0x18, 0x18, 0x08, 0x04, 0x00, 0x00, 0x00},
    {0x00, 0x00, 0x18, 0x04, 0x1C, 0x24, 0x1E},
    {0x20, 0x20, 0x2C, 0x32, 0x22, 0x32, 0x2C},
    {0x00, 0x00, 0x1C, 0x22, 0x20, 0x22, 0x1C},
    {0x02, 0x02, 0x1A, 0x26, 0x22, 0x26, 0x1A},
    {0x00, 0x00, 0x1C, 0x22, 0x3E, 0x20, 0x1C},
    {0x04, 0x0A, 0x08, 0x1C, 0x08, 0x08, 0x08},
    {0x00, 0x00, 0x1C, 0x26, 0x26, 0x1A, 0x02},
    {0x20, 0x20, 0x2C, 0x32, 0x22, 0x22, 0x22},
    {0x08, 0x00, 0x18, 0x08, 0x08, 0x08, 0x1C},
    {0x04, 0x00, 0x04, 0x04, 0x04, 0x24, 0x18},
    {0x20, 0x20, 0x24, 0x28, 0x30, 0x28, 0x24},
    {0x18, 0x08, 0x08, 0x08, 0x08, 0x08, 0x1C},
    {0x00, 0x00, 0x34, 0x2A, 0x2A, 0x2A, 0x2A},
    {0x00, 0x00, 0x2C, 0x32, 0x22, 0x22, 0x22},
    {0x00, 0x00, 0x1C, 0x22, 0x22, 0x22, 0x1C},
    {0x00, 0x00, 0x2C, 0x32, 0x32, 0x2C, 0x20},
    {0x00, 0x00, 0x1A, 0x26, 0x26, 0x1A, 0x02},
    {0x00, 0x00, 0x2C, 0x32, 0x20, 0x20, 0x20},
    {0x00, 0x00, 0x1E, 0x20, 0x1C, 0x02, 0x3C},
    {0x08, 0x08, 0x3E, 0x08, 0x08, 0x0A, 0x04},
    {0x00, 0x00, 0x22, 0x22, 0x22, 0x26, 0x1A},
    {0x00, 0x00, 0x22, 0x22, 0x22, 0x14, 0x08},
    {0x00, 0x00, 0x22, 0x22, 0x2A, 0x2A, 0x14},
    {0x00, 0x00, 0x22, 0x14, 0x08, 0x14, 0x22},
    {0x00, 0x00, 0x22, 0x22, 0x1E, 0x02, 0x22},
    {0x00, 0x00, 0x3E, 0x04, 0x08, 0x10, 0x3E},
    {0x04, 0x08, 0x08, 0x10, 0x08, 0x08, 0x04},
    {0x08, 0x08, 0x08, 0x00, 0x08, 0x08, 0x08},
    {0x10, 0x08, 0x08, 0x04, 0x08, 0x08, 0x10},
    {0x10, 0x2A, 0x04, 0x00, 0x00, 0x00, 0x00},
    {0x08, 0x1C, 0x36, 0x22, 0x22, 0x3E, 0x00}};

static void draw_char_nv12(HalFrameBuffer *f, int x, int y, char ch, const HalColor &c, int scale)
{
    const unsigned char uch = static_cast<unsigned char>(ch);
    if (uch < 32 || uch > 127)
        return;
    const uint8_t *glyph = kGlcdFont5x7Rows[uch - 32];
    scale = std::max(1, scale);
    for (int row = 0; row < 7; row++)
    {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 6; col++)
        {
            if (bits & (1u << (5 - col)))
            {
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        set_nv12_pixel(f, x + col * scale + sx, y + row * scale + sy, c);
            }
        }
    }
}

/** Scale OSD text so it stays readable on 4K (builtin font is 5x7 px per unit scale). */
static float draw_auto_font_scale(const HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    if (!f || !cfg)
        return 2.0f;
    const float h = (float)std::max(1u, f->height);
    return std::max(2.0f, cfg->default_font_scale * (h / 480.0f));
}

/** Decode next UTF-8 codepoint from text starting at byte position i.
 *  Returns the codepoint and advances i past the encoded bytes. */
static uint32_t utf8_decode(const char *text, size_t len, size_t &i)
{
    if (i >= len)
        return 0;
    auto b0 = static_cast<uint8_t>(text[i++]);
    if (b0 < 0x80)
        return b0;
    if ((b0 & 0xE0) == 0xC0 && i < len)
    {
        uint32_t cp = (b0 & 0x1F) << 6;
        cp |= static_cast<uint8_t>(text[i++]) & 0x3F;
        return cp;
    }
    if ((b0 & 0xF0) == 0xE0 && i + 1 < len)
    {
        uint32_t cp = (b0 & 0x0F) << 12;
        cp |= (static_cast<uint8_t>(text[i++]) & 0x3F) << 6;
        cp |= static_cast<uint8_t>(text[i++]) & 0x3F;
        return cp;
    }
    if ((b0 & 0xF8) == 0xF0 && i + 2 < len)
    {
        uint32_t cp = (b0 & 0x07) << 18;
        cp |= (static_cast<uint8_t>(text[i++]) & 0x3F) << 12;
        cp |= (static_cast<uint8_t>(text[i++]) & 0x3F) << 6;
        cp |= static_cast<uint8_t>(text[i++]) & 0x3F;
        return cp;
    }
    return '?';
}

/** Map common full-width / CJK punctuation to ASCII equivalents for the 5x7 font. */
static char fullwidth_to_ascii(uint32_t cp)
{
    switch (cp)
    {
    case 0xFF01: return '!';   // ！
    case 0xFF08: return '(';   // （
    case 0xFF09: return ')';   // ）
    case 0xFF0C: return ',';   // ，
    case 0xFF0E: return '.';   // ．
    case 0xFF1A: return ':';   // ：
    case 0xFF1B: return ';';   // ；
    case 0xFF1F: return '?';   // ？
    case 0x3001: return ',';   // 、
    case 0x3002: return '.';   // 。
    case 0x2018: return '\'';  // '
    case 0x2019: return '\'';  // '
    case 0x201C: return '"';   // "
    case 0x201D: return '"';   // "
    case 0x300A: return '<';   // 《
    case 0x300B: return '>';   // 》
    default: return 0;
    }
}

static void draw_text_nv12(HalFrameBuffer *f, const HalDrawText *t)
{
    if (!frame_is_nv12(f) || !t)
        return;
    int x = t->x;
    int y = t->y;
    const int scale = std::max(1, (int)std::round(t->font_scale));
    const size_t len = strnlen(t->text, sizeof(t->text));
    size_t i = 0;
    while (i < len)
    {
        uint32_t cp = utf8_decode(t->text, len, i);
        if (cp == 0)
            break;
        char ch = static_cast<char>(cp);
        if (cp >= 0x80)
        {
            char mapped = fullwidth_to_ascii(cp);
            if (mapped == 0)
            {
                /* Unmapped CJK — draw a box placeholder so text isn't silently invisible. */
                for (int sy = 0; sy < 7 * scale; sy++)
                    for (int sx = 0; sx < 5 * scale; sx++)
                        set_nv12_pixel(f, x + sx, y + sy, t->color);
                x += 6 * scale;
                continue;
            }
            ch = mapped;
        }
        if (ch == ' ')
        {
            x += 6 * scale;
            continue;
        }
        draw_char_nv12(f, x, y, ch, t->color, scale);
        x += 6 * scale;
    }
}

/** True if class_id passes optional HalDrawConfig.class_ids_filter (empty = all). */
static bool class_id_allowed(int32_t class_id, const HalDrawConfig *cfg)
{
    if (!cfg || cfg->num_class_ids_filter == 0)
        return true;
    for (uint32_t i = 0; i < cfg->num_class_ids_filter; i++)
    {
        if (cfg->class_ids_filter[i] == class_id)
            return true;
    }
    return false;
}

/** Segmentation / keypoint color: per-class style if configured, else deterministic palette. */
static HalColor color_for_class_id(int32_t class_id, const HalDrawConfig *cfg, bool use_keypoint_field)
{
    if (cfg)
    {
        for (uint32_t i = 0; i < cfg->num_class_styles; i++)
        {
            if (cfg->class_styles[i].class_id == class_id)
            {
                return use_keypoint_field ? cfg->class_styles[i].keypoint_color : cfg->class_styles[i].box_color;
            }
        }
    }
    /* Golden ratio hue steps for distinct colors */
    const unsigned u = (unsigned)class_id * 2654435761u;
    const uint8_t r = (uint8_t)(64 + (u & 0x7F));
    const uint8_t g = (uint8_t)(64 + ((u >> 8) & 0x7F));
    const uint8_t b = (uint8_t)(64 + ((u >> 16) & 0x7F));
    return HalColor{r, g, b, 255};
}

static void draw_disk_nv12(HalFrameBuffer *f, int cx, int cy, int radius, const HalColor &c)
{
    if (!frame_is_nv12(f) || radius <= 0)
        return;
    for (int dy = -radius; dy <= radius; dy++)
    {
        for (int dx = -radius; dx <= radius; dx++)
        {
            if (dx * dx + dy * dy <= radius * radius)
                set_nv12_pixel(f, cx + dx, cy + dy, c);
        }
    }
}

/** Map normalized depth t∈[0,1] to RGB (far=cool → near=warm), similar to common depth colormaps. */
static HalColor depth_t_to_colormap_rgb(float t)
{
    t = std::clamp(t, 0.0f, 1.0f);
    static const float pk[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    static const uint8_t pr[] = {40, 0, 0, 255, 255};
    static const uint8_t pg[] = {60, 180, 220, 220, 120};
    static const uint8_t pb[] = {140, 220, 80, 40, 20};
    for (int i = 0; i < 4; i++)
    {
        if (t <= pk[i + 1])
        {
            const float u = (t - pk[i]) / (pk[i + 1] - pk[i]);
            const float r = pr[i] + (pr[i + 1] - pr[i]) * u;
            const float g = pg[i] + (pg[i + 1] - pg[i]) * u;
            const float b = pb[i] + (pb[i + 1] - pb[i]) * u;
            return hal_color_rgb((uint8_t)(r + 0.5f), (uint8_t)(g + 0.5f), (uint8_t)(b + 0.5f));
        }
    }
    return hal_color_rgb(255, 120, 20);
}

static void draw_depth_colormap_nv12(HalFrameBuffer *f, const HalDepthResult *dep, const HalDrawConfig *cfg)
{
    if (!frame_is_nv12(f) || !dep || !dep->depth_m || dep->width == 0 || dep->height == 0 || !cfg)
        return;
    if (!cfg->draw_depth_colormap)
        return;
    float alpha = cfg->depth_colormap_alpha;
    if (alpha <= 0.0f)
        return;
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    const uint32_t dw = dep->width;
    const uint32_t dh = dep->height;
    const size_t n = (size_t)dw * (size_t)dh;
    float mn = dep->depth_m[0], mx = dep->depth_m[0];
    for (size_t i = 1; i < n; i++)
    {
        mn = std::min(mn, dep->depth_m[i]);
        mx = std::max(mx, dep->depth_m[i]);
    }
    const float inv = 1.0f / (mx - mn + 1e-6f);

    /* Thumbnail (top-right): O(thumb_w*thumb_h) instead of full frame. */
    const uint32_t margin = cfg->depth_thumbnail_margin ? cfg->depth_thumbnail_margin : 10u;
    const uint32_t want_w = cfg->depth_thumbnail_max_width ? cfg->depth_thumbnail_max_width : 200u;
    if (f->width <= margin + 2u || f->height <= margin + 2u)
        return;

    uint32_t thumb_w = std::min(want_w, f->width - 2u * margin);
    uint32_t thumb_h = (uint32_t)(((uint64_t)thumb_w * (uint64_t)dh + (uint64_t)dw / 2) / (uint64_t)dw);
    const uint32_t max_h = f->height - 2u * margin;
    if (thumb_h > max_h && max_h > 0u)
    {
        thumb_h = max_h;
        thumb_w = (uint32_t)(((uint64_t)thumb_h * (uint64_t)dw + (uint64_t)dh / 2) / (uint64_t)dh);
        thumb_w = std::max(2u, std::min(thumb_w, f->width - 2u * margin));
    }
    if (thumb_w < 2u || thumb_h < 2u)
        return;

    const int ox = (int)f->width - (int)margin - (int)thumb_w;
    const int oy = (int)margin;
    if (ox < 0 || oy < 0)
        return;

    for (uint32_t ty = 0; ty < thumb_h; ty++)
    {
        for (uint32_t tx = 0; tx < thumb_w; tx++)
        {
            const uint32_t mx = (tx * dw) / thumb_w;
            const uint32_t my = (ty * dh) / thumb_h;
            const float d = dep->depth_m[(size_t)my * (size_t)dw + (size_t)mx];
            const float t = (d - mn) * inv;
            const HalColor col = depth_t_to_colormap_rgb(t);
            blend_nv12_pixel(f, ox + (int)tx, oy + (int)ty, col, alpha);
        }
    }
}

static void draw_segmentation_nv12(HalFrameBuffer *f, const HalSegmentationResult *seg, const HalDrawConfig *cfg)
{
    if (!frame_is_nv12(f) || !seg || !seg->mask_data || seg->width == 0 || seg->height == 0 || !cfg)
        return;
    if (!cfg->draw_segmentation)
        return;
    float alpha = cfg->segmentation_alpha;
    if (alpha <= 0.0f)
        return;
    alpha = std::clamp(alpha, 0.0f, 1.0f);

    for (uint32_t fy = 0; fy < f->height; fy++)
    {
        for (uint32_t fx = 0; fx < f->width; fx++)
        {
            const uint32_t mx = (fx * seg->width) / f->width;
            const uint32_t my = (fy * seg->height) / f->height;
            const uint8_t cid_u8 = seg->mask_data[my * seg->width + mx];
            const int32_t cid = static_cast<int32_t>(cid_u8);
            /* Convention: 255 often means "ignore"; 0 = background — skip both for overlay. */
            if (cid_u8 == 0 || cid_u8 == 255)
                continue;
            if (!class_id_allowed(cid, cfg))
                continue;
            const HalColor col = color_for_class_id(cid, cfg, false);
            blend_nv12_pixel(f, (int)fx, (int)fy, col, alpha);
        }
    }
}

static void draw_keypoint_nv12(HalFrameBuffer *f, const HalKeypointResult *kp, const HalDrawConfig *cfg)
{
    if (!frame_is_nv12(f) || !kp || !cfg)
        return;

    const int kp_radius =
        cfg->default_keypoint_radius > 0 ? cfg->default_keypoint_radius : 3;

    for (uint32_t oi = 0; oi < kp->num_objects; oi++)
    {
        const HalKeypointObject &obj = kp->objects[oi];
        if (!class_id_allowed(obj.class_id, cfg))
            continue;

        /* Optional object bbox (normalized). */
        if (obj.bbox.w > 0.0f && obj.bbox.h > 0.0f)
        {
            HalDrawRect rect{};
            hal_bbox_to_rect(&obj.bbox, f->width, f->height, &rect);
            rect.color = cfg->default_box_color;
            rect.thickness = cfg->default_box_thickness > 0 ? cfg->default_box_thickness : 2;
            draw_rect_nv12(f, &rect);
        }

        /* Skeleton (pose): shared link table applies to each object. */
        if (cfg->draw_skeleton && kp->num_links > 0)
        {
            for (uint32_t li = 0; li < kp->num_links; li++)
            {
                const HalSkeletonLink &link = kp->links[li];
                if (link.from_idx < 0 || link.to_idx < 0)
                    continue;
                if (static_cast<uint32_t>(link.from_idx) >= obj.num_keypoints ||
                    static_cast<uint32_t>(link.to_idx) >= obj.num_keypoints)
                    continue;
                const HalPoint2D &a = obj.keypoints[link.from_idx];
                const HalPoint2D &b = obj.keypoints[link.to_idx];
                if (a.confidence <= 0.0f || b.confidence <= 0.0f)
                    continue;
                int ax, ay, bx, by;
                hal_point_to_pixel(&a, f->width, f->height, &ax, &ay);
                hal_point_to_pixel(&b, f->width, f->height, &bx, &by);
                const int thick = link.thickness > 0.0f ? (int)std::lround(link.thickness) : 2;
                draw_line_nv12(f, ax, ay, bx, by, link.color, std::max(1, thick));
            }
        }

        /* Keypoints (and fallback lines when no skeleton table: chain consecutive visible points). */
        if (cfg->draw_keypoints)
        {
            for (uint32_t ki = 0; ki < obj.num_keypoints; ki++)
            {
                const HalPoint2D &pt = obj.keypoints[ki];
                if (pt.confidence <= 0.0f)
                    continue;
                HalColor kcol = cfg->default_keypoint_color;
                for (uint32_t si = 0; si < cfg->num_class_styles; si++)
                {
                    if (cfg->class_styles[si].class_id == obj.class_id)
                    {
                        kcol = cfg->class_styles[si].keypoint_color;
                        break;
                    }
                }
                int px, py;
                hal_point_to_pixel(&pt, f->width, f->height, &px, &py);
                const int r = kp_radius;
                draw_disk_nv12(f, px, py, r, kcol);
            }
        }

        /* Simple pose fallback: if no links defined, connect consecutive visible keypoints (open chain). */
        if (cfg->draw_skeleton && kp->num_links == 0 && cfg->draw_keypoints && obj.num_keypoints > 1)
        {
            int prev_x = 0, prev_y = 0;
            bool have_prev = false;
            for (uint32_t ki = 0; ki < obj.num_keypoints; ki++)
            {
                const HalPoint2D &pt = obj.keypoints[ki];
                if (pt.confidence <= 0.0f)
                {
                    have_prev = false;
                    continue;
                }
                int px, py;
                hal_point_to_pixel(&pt, f->width, f->height, &px, &py);
                if (have_prev)
                    draw_line_nv12(f, prev_x, prev_y, px, py, cfg->default_keypoint_color, 2);
                prev_x = px;
                prev_y = py;
                have_prev = true;
            }
        }
    }
}

static int draw_result_impl(const HalPostprocessResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    if (!r || !f)
        return HAL_ERR_INVALID_ARG;
    HalDrawConfig local{};
    if (!cfg)
    {
        hal_draw_config_init_default(&local);
        cfg = &local;
    }

    if ((r->type == HAL_POST_TYPE_DETECTION || r->type == HAL_POST_TYPE_OCR_DETECTION) && cfg->draw_detections)
    {
        const float fs = draw_auto_font_scale(f, cfg);
        for (uint32_t i = 0; i < r->result.detection.num_detections; i++)
        {
            const HalDetection &d = r->result.detection.detections[i];
            HalDrawRect rect{};
            hal_bbox_to_rect(&d.bbox, f->width, f->height, &rect);
            rect.color = cfg->default_box_color;
            rect.thickness = cfg->default_box_thickness ? cfg->default_box_thickness : 2;
            draw_rect_nv12(f, &rect);

            if (cfg->draw_detection_labels)
            {
                HalDrawText tx{};
                tx.x = rect.x;
                tx.y = std::max(0, rect.y - (int)std::lround(10.f * fs));
                tx.color = cfg->default_text_color;
                tx.bg_color = cfg->default_text_bg_color;
                tx.font_scale = fs;
                const bool want_conf = cfg->draw_detection_confidence;
                if (d.label[0] != '\0')
                {
                    if (want_conf)
                        std::snprintf(tx.text, sizeof(tx.text), "%s %.2f", d.label, (double)d.confidence);
                    else
                        std::snprintf(tx.text, sizeof(tx.text), "%s", d.label);
                }
                else
                {
                    if (want_conf)
                        std::snprintf(tx.text, sizeof(tx.text), "%d %.2f", (int)d.class_id, (double)d.confidence);
                    else
                        std::snprintf(tx.text, sizeof(tx.text), "%d", (int)d.class_id);
                }
                draw_text_nv12(f, &tx);
            }
        }
        return HAL_OK;
    }
    if (r->type == HAL_POST_TYPE_CLIP)
    {
        const float fs = draw_auto_font_scale(f, cfg);
        const uint32_t max_show = (cfg->clip_max_lines > 0) ? cfg->clip_max_lines : 5U;
        const uint32_t n = std::min(r->result.classification.num_classes, max_show);

        int y = (int)(10.f * fs);
        for (uint32_t i = 0; i < n; i++)
        {
            const HalClassification &c = r->result.classification.classes[i];
            HalDrawText tx{};
            tx.x = (int)(10.f * fs);
            tx.y = y;
            tx.color = cfg->default_text_color;
            tx.font_scale = fs;
            if (c.label[0] != '\0')
                std::snprintf(tx.text, sizeof(tx.text), "%s: %.3f", c.label, (double)c.confidence);
            else
                std::snprintf(tx.text, sizeof(tx.text), "%d: %.3f", (int)c.class_id, (double)c.confidence);
            draw_text_nv12(f, &tx);
            y += (int)(10.f * fs);
        }

        return HAL_OK;
    }
    if (r->type == HAL_POST_TYPE_SEGMENTATION)
    {
        draw_segmentation_nv12(f, &r->result.segmentation, cfg);
        return HAL_OK;
    }
    if (r->type == HAL_POST_TYPE_DEPTH)
    {
        draw_depth_colormap_nv12(f, &r->result.depth, cfg);
        return HAL_OK;
    }
    if (r->type == HAL_POST_TYPE_KEYPOINT)
    {
        draw_keypoint_nv12(f, &r->result.keypoint, cfg);
        return HAL_OK;
    }
    if (r->type == HAL_POST_TYPE_CLASSIFICATION && cfg->draw_classifications)
    {
        const float fs = draw_auto_font_scale(f, cfg);
        int y = (int)std::lround(10.f * fs);
        for (uint32_t i = 0; i < r->result.classification.num_classes && i < HAL_MAX_CLASSES; i++)
        {
            const HalClassification &c = r->result.classification.classes[i];
            HalDrawText tx{};
            tx.x = (int)std::lround(10.f * fs);
            tx.y = y;
            tx.color = cfg->default_text_color;
            tx.font_scale = fs;
            if (c.label[0] != '\0')
                std::snprintf(tx.text, sizeof(tx.text), "%s: %.3f", c.label, (double)c.confidence);
            else
                std::snprintf(tx.text, sizeof(tx.text), "%d: %.3f", (int)c.class_id, (double)c.confidence);
            draw_text_nv12(f, &tx);
            y += (int)std::lround(10.f * fs);
        }
        return HAL_OK;
    }
    if (r->type == HAL_POST_TYPE_OCR_RECOGNITION && cfg->draw_ocr)
    {
        const float fs = draw_auto_font_scale(f, cfg);
        for (uint32_t i = 0; i < r->result.ocr.num_lines; i++)
        {
            const HalOcrLine &ln = r->result.ocr.lines[i];
            HalDrawRect rect{};
            hal_bbox_to_rect(&ln.bbox, f->width, f->height, &rect);
            rect.color = cfg->default_box_color;
            rect.thickness = cfg->default_box_thickness ? cfg->default_box_thickness : 2;
            draw_rect_nv12(f, &rect);

            HalDrawText tx{};
            tx.x = rect.x;
            tx.y = std::max(0, rect.y - (int)std::lround(10.f * fs));
            tx.color = cfg->default_text_color;
            tx.bg_color = cfg->default_text_bg_color;
            tx.font_scale = fs;
            if (ln.text[0] != '\0')
            {
                if (cfg->draw_detection_confidence)
                {
                    /* tx.text is HAL_MAX_TEXT_LEN; keep format truncation deterministic vs long OCR strings. */
                    std::snprintf(tx.text, sizeof(tx.text), "%.200s %.2f", ln.text, (double)ln.confidence);
                }
                else
                    std::snprintf(tx.text, sizeof(tx.text), "%s", ln.text);
            }
            else if (cfg->draw_detection_confidence)
                std::snprintf(tx.text, sizeof(tx.text), "%.2f", (double)ln.confidence);
            else
                tx.text[0] = '\0';
            if (tx.text[0] != '\0')
                draw_text_nv12(f, &tx);
        }
        return HAL_OK;
    }
    return HAL_ERR_NOT_SUPPORTED;
}

int cpu_draw_detection(const HalDetectionResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    HalPostprocessResult pr{};
    pr.type = HAL_POST_TYPE_DETECTION;
    pr.result.detection = *r;
    return draw_result_impl(&pr, f, cfg);
}

int cpu_draw_classification(const HalClassificationResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    HalPostprocessResult pr{};
    pr.type = HAL_POST_TYPE_CLASSIFICATION;
    pr.result.classification = *r;
    return draw_result_impl(&pr, f, cfg);
}

int cpu_draw_segmentation(const HalSegmentationResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    if (!r || !f)
        return HAL_ERR_INVALID_ARG;
    HalDrawConfig local{};
    const HalDrawConfig *c = cfg;
    if (!c)
    {
        hal_draw_config_init_default(&local);
        c = &local;
    }
    if (!frame_is_nv12(f))
        return HAL_ERR_NOT_SUPPORTED;
    draw_segmentation_nv12(f, r, c);
    return HAL_OK;
}

int cpu_draw_keypoint(const HalKeypointResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    if (!r || !f)
        return HAL_ERR_INVALID_ARG;
    HalDrawConfig local{};
    const HalDrawConfig *c = cfg;
    if (!c)
    {
        hal_draw_config_init_default(&local);
        c = &local;
    }
    if (!frame_is_nv12(f))
        return HAL_ERR_NOT_SUPPORTED;
    draw_keypoint_nv12(f, r, c);
    return HAL_OK;
}

int cpu_draw_result(const HalPostprocessResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    return draw_result_impl(r, f, cfg);
}

int cpu_draw_rect(HalFrameBuffer *f, const HalDrawRect *r)
{
    if (!frame_is_nv12(f))
        return HAL_ERR_NOT_SUPPORTED;
    draw_rect_nv12(f, r);
    return HAL_OK;
}

int cpu_draw_line(HalFrameBuffer *f, const HalDrawLine *l)
{
    if (!frame_is_nv12(f) || !l)
        return HAL_ERR_INVALID_ARG;
    draw_line_nv12(f, l->x1, l->y1, l->x2, l->y2, l->color, l->thickness);
    return HAL_OK;
}

int cpu_draw_circle(HalFrameBuffer *f, const HalDrawCircle *c)
{
    if (!frame_is_nv12(f) || !c)
        return HAL_ERR_INVALID_ARG;
    const int xc = c->x;
    const int yc = c->y;
    const int r = c->radius;
    if (r <= 0)
        return HAL_ERR_INVALID_ARG;
    if (c->thickness < 0)
    {
        // Filled circle (naive)
        for (int y = -r; y <= r; y++)
        {
            for (int x = -r; x <= r; x++)
            {
                if (x * x + y * y <= r * r)
                    set_nv12_pixel(f, xc + x, yc + y, c->color);
            }
        }
        return HAL_OK;
    }
    const int t = std::max(1, c->thickness);
    int x = r;
    int y = 0;
    int err = 0;
    while (x >= y)
    {
        for (int tt = -t / 2; tt <= t / 2; tt++)
        {
            set_nv12_pixel(f, xc + x, yc + y + tt, c->color);
            set_nv12_pixel(f, xc + y, yc + x + tt, c->color);
            set_nv12_pixel(f, xc - y, yc + x + tt, c->color);
            set_nv12_pixel(f, xc - x, yc + y + tt, c->color);
            set_nv12_pixel(f, xc - x, yc - y + tt, c->color);
            set_nv12_pixel(f, xc - y, yc - x + tt, c->color);
            set_nv12_pixel(f, xc + y, yc - x + tt, c->color);
            set_nv12_pixel(f, xc + x, yc - y + tt, c->color);
        }
        y += 1;
        if (err <= 0)
        {
            err += 2 * y + 1;
        }
        if (err > 0)
        {
            x -= 1;
            err -= 2 * x + 1;
        }
    }
    return HAL_OK;
}

int cpu_draw_text(HalFrameBuffer *f, const HalDrawText *t)
{
    if (!frame_is_nv12(f))
        return HAL_ERR_NOT_SUPPORTED;
    draw_text_nv12(f, t);
    return HAL_OK;
}

int cpu_draw_polygon(HalFrameBuffer *f, const HalDrawPolygon *p)
{
    if (!frame_is_nv12(f) || !p)
        return HAL_ERR_INVALID_ARG;
    if (p->num_points < 2 || p->num_points > HAL_MAX_POLYGON_POINTS)
        return HAL_ERR_INVALID_ARG;
    const int t = (p->thickness == 0) ? 1 : p->thickness;
    // Outline only (filled polygons can be added later).
    for (uint32_t i = 0; i < p->num_points; i++)
    {
        const uint32_t j = (i + 1) % p->num_points;
        draw_line_nv12(f, p->points_x[i], p->points_y[i], p->points_x[j], p->points_y[j], p->color, std::max(1, t));
    }
    return HAL_OK;
}

int cpu_draw_mask(HalFrameBuffer *f, const HalDrawMask *m)
{
    if (!frame_is_nv12(f) || !m || !m->mask_data)
        return HAL_ERR_INVALID_ARG;
    for (uint32_t yy = 0; yy < m->height; yy++)
    {
        for (uint32_t xx = 0; xx < m->width; xx++)
        {
            const uint8_t v = m->mask_data[yy * m->width + xx];
            if (v == 0)
                continue;
            blend_nv12_pixel(f, (int)m->x + (int)xx, (int)m->y + (int)yy, m->color, m->alpha);
        }
    }
    return HAL_OK;
}

int cpu_draw_mosaic(HalFrameBuffer *f, const HalDrawMosaic *m)
{
    if (!frame_is_nv12(f) || !m)
        return HAL_ERR_INVALID_ARG;
    const int x0 = std::max(0, m->x);
    const int y0 = std::max(0, m->y);
    const int x1 = std::min((int)f->width, m->x + m->width);
    const int y1 = std::min((int)f->height, m->y + m->height);
    if (x0 >= x1 || y0 >= y1)
        return HAL_OK;
    const int bs = m->block_size;
    uint8_t *Y = static_cast<uint8_t *>(f->planes[0]);
    const uint32_t y_stride = f->strides[0] ? f->strides[0] : f->width;

    // Blur mode when block_size <= 0: simple 3x3 box blur on luma.
    if (bs <= 0)
    {
        // In-place blur using a small temp line buffer to avoid full-frame alloc.
        // This is intentionally lightweight for privacy masking use.
        const int radius = 1;
        std::vector<uint8_t> tmp((size_t)(x1 - x0) * (size_t)(y1 - y0));
        const int w = x1 - x0;
        const int h = y1 - y0;
        for (int yy = 0; yy < h; yy++)
        {
            for (int xx = 0; xx < w; xx++)
            {
                uint32_t sum = 0;
                uint32_t cnt = 0;
                for (int ky = -radius; ky <= radius; ky++)
                {
                    const int sy = std::clamp(y0 + yy + ky, y0, y1 - 1);
                    for (int kx = -radius; kx <= radius; kx++)
                    {
                        const int sx = std::clamp(x0 + xx + kx, x0, x1 - 1);
                        sum += Y[sy * (int)y_stride + sx];
                        cnt++;
                    }
                }
                tmp[(size_t)yy * (size_t)w + (size_t)xx] = (uint8_t)(sum / std::max(1u, cnt));
            }
        }
        for (int yy = 0; yy < h; yy++)
        {
            std::memcpy(&Y[(y0 + yy) * (int)y_stride + x0], &tmp[(size_t)yy * (size_t)w], (size_t)w);
        }
        return HAL_OK;
    }

    const int bs2 = std::max(2, bs);
    for (int by = y0; by < y1; by += bs)
    {
        for (int bx = x0; bx < x1; bx += bs)
        {
            const int ex = std::min(bx + bs2, x1);
            const int ey = std::min(by + bs2, y1);
            // Average luma
            uint32_t sum = 0;
            uint32_t cnt = 0;
            for (int yy = by; yy < ey; yy++)
            {
                for (int xx = bx; xx < ex; xx++)
                {
                    sum += Y[yy * (int)y_stride + xx];
                    cnt++;
                }
            }
            const uint8_t avg = (cnt > 0) ? (uint8_t)(sum / cnt) : 0;
            for (int yy = by; yy < ey; yy++)
            {
                for (int xx = bx; xx < ex; xx++)
                {
                    Y[yy * (int)y_stride + xx] = avg;
                }
            }
        }
    }
    return HAL_OK;
}

const char *cpu_draw_get_version(void)
{
    return "HAL-DRAW CPU (NV12)";
}

} // namespace hal_draw_cpu_internal

extern "C" {

int hal_draw_cpu_draw_detection(const HalDetectionResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    return hal_draw_cpu_internal::cpu_draw_detection(r, f, cfg);
}

int hal_draw_cpu_draw_classification(const HalClassificationResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    return hal_draw_cpu_internal::cpu_draw_classification(r, f, cfg);
}

int hal_draw_cpu_draw_segmentation(const HalSegmentationResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    return hal_draw_cpu_internal::cpu_draw_segmentation(r, f, cfg);
}

int hal_draw_cpu_draw_keypoint(const HalKeypointResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    return hal_draw_cpu_internal::cpu_draw_keypoint(r, f, cfg);
}

int hal_draw_cpu_draw_result(const HalPostprocessResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    return hal_draw_cpu_internal::cpu_draw_result(r, f, cfg);
}

int hal_draw_cpu_draw_rect(HalFrameBuffer *f, const HalDrawRect *r)
{
    return hal_draw_cpu_internal::cpu_draw_rect(f, r);
}

int hal_draw_cpu_draw_line(HalFrameBuffer *f, const HalDrawLine *l)
{
    return hal_draw_cpu_internal::cpu_draw_line(f, l);
}

int hal_draw_cpu_draw_circle(HalFrameBuffer *f, const HalDrawCircle *c)
{
    return hal_draw_cpu_internal::cpu_draw_circle(f, c);
}

int hal_draw_cpu_draw_text(HalFrameBuffer *f, const HalDrawText *t)
{
    return hal_draw_cpu_internal::cpu_draw_text(f, t);
}

int hal_draw_cpu_draw_polygon(HalFrameBuffer *f, const HalDrawPolygon *p)
{
    return hal_draw_cpu_internal::cpu_draw_polygon(f, p);
}

int hal_draw_cpu_draw_mask(HalFrameBuffer *f, const HalDrawMask *m)
{
    return hal_draw_cpu_internal::cpu_draw_mask(f, m);
}

int hal_draw_cpu_draw_mosaic(HalFrameBuffer *f, const HalDrawMosaic *m)
{
    return hal_draw_cpu_internal::cpu_draw_mosaic(f, m);
}

const char *hal_draw_cpu_get_version(void)
{
    return hal_draw_cpu_internal::cpu_draw_get_version();
}

void hal_draw_config_init_default(HalDrawConfig *c)
{
    if (!c)
        return;
    std::memset(c, 0, sizeof(*c));
    c->draw_detections = true;
    c->draw_detection_labels = true;
    c->draw_detection_confidence = true;
    c->draw_keypoints = true;
    c->draw_skeleton = true;
    c->draw_segmentation = true;
    c->segmentation_alpha = 0.4f;
    c->draw_classifications = true;
    c->clip_max_lines = 5;
    c->draw_ocr = true;
    c->draw_depth_colormap = false;
    c->depth_colormap_alpha = 0.4f;
    c->depth_thumbnail_max_width = 0;
    c->depth_thumbnail_margin = 0;
    c->enable_face_blur = false;
    c->face_blur_block_size = 8;
    c->default_box_color = hal_color_rgb(0, 255, 0);
    c->default_box_thickness = 2;
    c->default_keypoint_color = hal_color_rgb(255, 0, 0);
    c->default_keypoint_radius = 2;
    c->default_text_color = hal_color_rgb(255, 255, 255);
    c->default_text_bg_color = hal_color_rgba(0, 0, 0, 0);
    c->default_font_scale = 1.0f;
    c->num_class_styles = 0;
    c->num_class_ids_filter = 0;
}

int hal_draw_config_add_class_style(HalDrawConfig *config, int32_t class_id, HalColor color, int32_t thickness)
{
    if (!config)
        return HAL_ERR_INVALID_ARG;
    if (config->num_class_styles >= HAL_MAX_CLASS_STYLES)
        return HAL_ERR_INSUFFICIENT_BUFFER;
    HalDrawClassStyle &s = config->class_styles[config->num_class_styles++];
    std::memset(&s, 0, sizeof(s));
    s.class_id = class_id;
    s.box_color = color;
    s.box_thickness = thickness;
    s.keypoint_color = color;
    s.keypoint_radius = 2;
    s.draw_label = true;
    s.draw_confidence = false;
    s.blur_region = false;
    return HAL_OK;
}

} // extern "C"

