/**
 * @file hal_draw.h
 * @brief HAL drawing interface
 * @version 2.0
 *
 * Platform-agnostic drawing interface
 * Supports high-level API (draw results) and low-level API (draw primitives)
 * Works with multiple frame formats (NV12, RGB, etc.)
 *
 * Backends (CMake @c HAL_DRAW_BACKEND):
 * - @c cpu — hal_draw_ops_cpu.cpp: NV12 in-place rasterizer (hal_draw_cpu.cpp).
 * - @c hailo15 — Default on Hailo-15: hal_draw_hailo15.cpp uses @c HailoNV12Mat (hailo_postprocess_tools),
 *   same idea as V1 @c ml_overlay_hailo15 — draw on NV12 Y/UV planes with OpenCV, no full-frame NV12↔BGR.
 *   Without postprocess tools headers, it falls back to @c hal_draw_cpu_draw_*.
 */

#pragma once

#include "common/hal_types.h"
#include "common/hal_buffer.h"
#include "hal_postprocess.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_MAX_TEXT_LEN 256
#define HAL_MAX_POLYGON_POINTS 128

/* ========== Drawing Primitives ========== */

/* Rectangle */
typedef struct {
    int32_t x, y;           // Top-left corner (pixel coordinates)
    int32_t width, height;
    HalColor color;
    int32_t thickness;      // Line thickness, -1 for filled
} HalDrawRect;

/* Line */
typedef struct {
    int32_t x1, y1;         // Start point
    int32_t x2, y2;         // End point
    HalColor color;
    int32_t thickness;
} HalDrawLine;

/* Circle */
typedef struct {
    int32_t x, y;           // Center
    int32_t radius;
    HalColor color;
    int32_t thickness;      // -1 for filled
} HalDrawCircle;

/* Text */
typedef struct {
    int32_t x, y;           // Position
    char text[HAL_MAX_TEXT_LEN];
    HalColor color;
    HalColor bg_color;      // Background color (a=0 for transparent)
    float font_scale;
    int32_t thickness;
} HalDrawText;

/* Polygon */
typedef struct {
    uint32_t num_points;
    int32_t points_x[HAL_MAX_POLYGON_POINTS];
    int32_t points_y[HAL_MAX_POLYGON_POINTS];
    HalColor color;
    int32_t thickness;      // -1 for filled
} HalDrawPolygon;

/* Mask overlay */
typedef struct {
    int32_t x, y;           // Top-left corner
    uint32_t width, height;
    uint8_t *mask_data;     // Binary mask (0 or 255)
    HalColor color;
    float alpha;            // Transparency (0.0-1.0)
} HalDrawMask;

/* Mosaic/Blur region */
typedef struct {
    int32_t x, y;
    int32_t width, height;
    int32_t block_size;     // Mosaic block size (0 for blur)
} HalDrawMosaic;

/* ========== Drawing Style Config ========== */

/* Per-class style */
typedef struct {
    int32_t class_id;       // -1 for default style
    HalColor box_color;
    int32_t box_thickness;
    HalColor keypoint_color;
    int32_t keypoint_radius;
    bool draw_label;
    bool draw_confidence;
    bool blur_region;       // Blur/mosaic this class (e.g., face blur)
} HalDrawClassStyle;

#define HAL_MAX_CLASS_STYLES 64

/* Global drawing config */
typedef struct {
    /* Detection drawing */
    bool draw_detections;
    bool draw_detection_labels;
    bool draw_detection_confidence;

    /* Keypoint drawing */
    bool draw_keypoints;
    bool draw_skeleton;

    /* Segmentation drawing */
    bool draw_segmentation;
    float segmentation_alpha;   // Mask transparency

    /* Classification drawing */
    bool draw_classifications;
    uint32_t clip_max_lines;        // For HAL_POST_TYPE_CLIP: max lines (top-N) to draw

    /* OCR: boxes + decoded text (same style as detection labels) */
    bool draw_ocr;

    /**
     * Monocular depth (@c HAL_POST_TYPE_DEPTH): pseudo-color **thumbnail** (top-right), not full-frame.
     * Uses min/max of @c HalDepthResult.depth_m for normalization each frame.
     */
    bool draw_depth_colormap;
    /** Blend strength 0..1 for thumbnail pixels. */
    float depth_colormap_alpha;
    /** Thumbnail max width (px); height follows depth map aspect. 0 = default 200. */
    uint32_t depth_thumbnail_max_width;
    /** Inset from top and right edges (px). 0 = default 10. */
    uint32_t depth_thumbnail_margin;

    /* Face blur */
    bool enable_face_blur;
    int32_t face_blur_block_size;

    /* Default styles */
    HalColor default_box_color;
    int32_t default_box_thickness;
    HalColor default_keypoint_color;
    int32_t default_keypoint_radius;
    HalColor default_text_color;
    HalColor default_text_bg_color;
    float default_font_scale;

    /* Per-class styles */
    uint32_t num_class_styles;
    HalDrawClassStyle class_styles[HAL_MAX_CLASS_STYLES];

    /* Class filter (draw only these classes, empty = draw all) */
    uint32_t num_class_ids_filter;
    int32_t class_ids_filter[HAL_MAX_CLASSES];
} HalDrawConfig;

/* ========== Drawing Operations ========== */
typedef struct HalDrawOps {
    /* ========== High-level API (draw results) ========== */

    /**
     * @brief Draw detection result on frame
     * @param result Detection result
     * @param frame Frame buffer (modified in-place)
     * @param config Drawing config (NULL for default)
     * @return HAL_OK on success
     */
    int (*draw_detection_result)(const HalDetectionResult *result,
                                 HalFrameBuffer *frame,
                                 const HalDrawConfig *config);

    /**
     * @brief Draw classification result on frame
     * @param result Classification result
     * @param frame Frame buffer
     * @param config Drawing config
     * @return HAL_OK on success
     */
    int (*draw_classification_result)(const HalClassificationResult *result,
                                      HalFrameBuffer *frame,
                                      const HalDrawConfig *config);

    /**
     * @brief Draw segmentation result on frame
     * @param result Segmentation result
     * @param frame Frame buffer
     * @param config Drawing config
     * @return HAL_OK on success
     */
    int (*draw_segmentation_result)(const HalSegmentationResult *result,
                                    HalFrameBuffer *frame,
                                    const HalDrawConfig *config);

    /**
     * @brief Draw keypoint result on frame
     * @param result Keypoint result
     * @param frame Frame buffer
     * @param config Drawing config
     * @return HAL_OK on success
     */
    int (*draw_keypoint_result)(const HalKeypointResult *result,
                                HalFrameBuffer *frame,
                                const HalDrawConfig *config);

    /**
     * @brief Draw all results (unified API)
     * @param result Postprocess result
     * @param frame Frame buffer (modified in-place)
     * @param config Drawing config
     * @return HAL_OK on success
     */
    int (*draw_result)(const HalPostprocessResult *result,
                       HalFrameBuffer *frame,
                       const HalDrawConfig *config);

    /* ========== Low-level API (draw primitives) ========== */

    /**
     * @brief Draw rectangle
     */
    int (*draw_rect)(HalFrameBuffer *frame, const HalDrawRect *rect);

    /**
     * @brief Draw line
     */
    int (*draw_line)(HalFrameBuffer *frame, const HalDrawLine *line);

    /**
     * @brief Draw circle
     */
    int (*draw_circle)(HalFrameBuffer *frame, const HalDrawCircle *circle);

    /**
     * @brief Draw text
     */
    int (*draw_text)(HalFrameBuffer *frame, const HalDrawText *text);

    /**
     * @brief Draw polygon
     */
    int (*draw_polygon)(HalFrameBuffer *frame, const HalDrawPolygon *polygon);

    /**
     * @brief Draw mask overlay
     */
    int (*draw_mask)(HalFrameBuffer *frame, const HalDrawMask *mask);

    /**
     * @brief Draw mosaic/blur region
     */
    int (*draw_mosaic)(HalFrameBuffer *frame, const HalDrawMosaic *mosaic);

    /**
     * @brief Get version string
     */
    const char* (*get_version)(void);
} HalDrawOps;

/* ========== Global Operations Table ========== */
extern HalDrawOps HAL_DRAW_OPS;

/* ========== Helper Functions ========== */

/**
 * @brief Initialize default drawing config
 * @param config Config to initialize
 */
void hal_draw_config_init_default(HalDrawConfig *config);

/**
 * @brief Add per-class style
 * @param config Drawing config
 * @param class_id Class ID
 * @param color Box/keypoint color
 * @param thickness Line thickness
 * @return HAL_OK on success
 */
int hal_draw_config_add_class_style(HalDrawConfig *config,
                                    int32_t class_id,
                                    HalColor color,
                                    int32_t thickness);

/** Clamp normalized coord; NaN/Inf → 0 (avoids UB when casting float to int in draw paths). */
static inline float hal_bbox_sanitize_norm(float v)
{
    if (v != v)
        return 0.f;
    if (v < 0.f)
        return 0.f;
    if (v > 1.f)
        return 1.f;
    return v;
}

/**
 * @brief Convert normalized bbox to pixel coordinates
 * @param bbox Normalized bbox (0-1)
 * @param frame_width Frame width
 * @param frame_height Frame height
 * @param rect Output rectangle
 */
static inline void hal_bbox_to_rect(const HalBBox *bbox,
                                    uint32_t frame_width,
                                    uint32_t frame_height,
                                    HalDrawRect *rect) {
    if (!bbox || !rect || frame_width == 0 || frame_height == 0)
    {
        rect->x = rect->y = rect->width = rect->height = 0;
        return;
    }
    float x0 = hal_bbox_sanitize_norm(bbox->x);
    float y0 = hal_bbox_sanitize_norm(bbox->y);
    float x1 = hal_bbox_sanitize_norm(bbox->x + bbox->w);
    float y1 = hal_bbox_sanitize_norm(bbox->y + bbox->h);
    if (x1 < x0)
    {
        const float t = x0;
        x0 = x1;
        x1 = t;
    }
    if (y1 < y0)
    {
        const float t = y0;
        y0 = y1;
        y1 = t;
    }
    const float fw = (float)frame_width;
    const float fh = (float)frame_height;
    int32_t ix0 = (int32_t)(x0 * fw);
    int32_t iy0 = (int32_t)(y0 * fh);
    int32_t ix1 = (int32_t)(x1 * fw);
    int32_t iy1 = (int32_t)(y1 * fh);
    if (ix0 < 0)
        ix0 = 0;
    if (iy0 < 0)
        iy0 = 0;
    if (ix1 > (int32_t)frame_width)
        ix1 = (int32_t)frame_width;
    if (iy1 > (int32_t)frame_height)
        iy1 = (int32_t)frame_height;
    rect->x = ix0;
    rect->y = iy0;
    rect->width = ix1 - ix0;
    rect->height = iy1 - iy0;
}

/**
 * @brief Convert normalized point to pixel coordinates
 */
static inline void hal_point_to_pixel(const HalPoint2D *point,
                                      uint32_t frame_width,
                                      uint32_t frame_height,
                                      int32_t *x, int32_t *y) {
    *x = (int32_t)(point->x * frame_width);
    *y = (int32_t)(point->y * frame_height);
}

/**
 * @brief Create color from RGB
 */
static inline HalColor hal_color_rgb(uint8_t r, uint8_t g, uint8_t b) {
    HalColor c = {r, g, b, 255};
    return c;
}

/**
 * @brief Create color from RGBA
 */
static inline HalColor hal_color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    HalColor c = {r, g, b, a};
    return c;
}

#ifdef __cplusplus
}
#endif
