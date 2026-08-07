/**
 * @file hal_osd.h
 * @brief HAL OSD - On-Screen Display overlay management.
 *
 * Provides per-encoder-stream overlay operations (add, update, remove,
 * enable/disable, query) for image, text, date-time, and custom overlays.
 *
 * All overlay positions and dimensions use normalised coordinates [0.0 .. 1.0].
 * Font sizes are in absolute pixels; the HAL internally rescales them when the
 * encoder resolution or rotation changes (matching the webserver behaviour).
 *
 * OSD operations take a codec context pointer (HalCodecContext*) as the first
 * argument because overlays are blended into the encoder output stream.
 * For FROM_MEDIA type codec contexts, the underlying media library
 * osd::Configurer / osd::OsdBlender is used.
 *
 * There is no separate init / deinit -- OSD lifecycle is bound to the codec
 * context (and by extension the media pipeline).
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "../common/hal_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
 * Overlay type
 * -------------------------------------------------------------------- */

/** Discriminator for the overlay union in HalOsdOverlay. */
typedef enum {
    HAL_OSD_OVERLAY_IMAGE    = 0,
    HAL_OSD_OVERLAY_TEXT     = 1,
    HAL_OSD_OVERLAY_DATETIME = 2,
    HAL_OSD_OVERLAY_CUSTOM   = 3,
} HalOsdOverlayType;

/* --------------------------------------------------------------------
 * Alignment / rotation policy
 * -------------------------------------------------------------------- */

/** Rotation anchor policy. */
typedef enum {
    HAL_OSD_ROTATION_POLICY_CENTER   = 0,  /* rotate around overlay centre */
    HAL_OSD_ROTATION_POLICY_TOP_LEFT = 1,  /* rotate around top-left corner */
} HalOsdRotationPolicy;

/** Horizontal alignment applied to the overlay relative to (x, y). */
typedef enum {
    HAL_OSD_HALIGN_LEFT   = 0,  /* left edge at x */
    HAL_OSD_HALIGN_CENTER = 1,  /* centre at x */
    HAL_OSD_HALIGN_RIGHT  = 2,  /* right edge at x */
} HalOsdHorizontalAlignment;

/** Vertical alignment applied to the overlay relative to (x, y). */
typedef enum {
    HAL_OSD_VALIGN_TOP    = 0,  /* top edge at y */
    HAL_OSD_VALIGN_CENTER = 1,  /* centre at y */
    HAL_OSD_VALIGN_BOTTOM = 2,  /* bottom edge at y */
} HalOsdVerticalAlignment;

/** Font weight for text overlays. */
typedef enum {
    HAL_OSD_FONT_WEIGHT_NORMAL = 0,
    HAL_OSD_FONT_WEIGHT_BOLD   = 1,
} HalOsdFontWeight;

/** Custom overlay pixel format. */
typedef enum {
    HAL_OSD_CUSTOM_FMT_A420 = 0,
    HAL_OSD_CUSTOM_FMT_ARGB = 1,
} HalOsdCustomFormat;

/* --------------------------------------------------------------------
 * Colour
 * -------------------------------------------------------------------- */

/** RGBA colour (8 bits per channel). */
typedef struct {
    int r;   /**< red   [0..255] or negative to indicate "disabled" (shadow/outline) */
    int g;   /**< green [0..255] */
    int b;   /**< blue  [0..255] */
    int a;   /**< alpha [0..255] */
} HalOsdColor;

/* --------------------------------------------------------------------
 * Overlay base (fields common to every overlay type)
 * -------------------------------------------------------------------- */

/**
 * Fields shared by all overlay types.
 *
 * Coordinates are normalised: (0.0, 0.0) = top-left, (1.0, 1.0) = bottom-right.
 */
typedef struct {
    char                      id[64];            /**< unique identifier */
    HalOsdOverlayType         type;              /**< discriminator (set automatically on add) */
    bool                      enabled;           /**< visibility flag */
    float                     x;                 /**< normalised X position [0..1] */
    float                     y;                 /**< normalised Y position [0..1] */
    uint32_t                  z_index;           /**< blend order (higher = on top) */
    uint32_t                  angle;             /**< clockwise rotation in degrees */
    HalOsdRotationPolicy      rotation_policy;   /**< rotation anchor */
    HalOsdHorizontalAlignment h_align;           /**< horizontal alignment */
    HalOsdVerticalAlignment   v_align;           /**< vertical alignment */
} HalOsdOverlayBase;

/* --------------------------------------------------------------------
 * Per-type overlay structs
 * -------------------------------------------------------------------- */

/** Image overlay (PNG / BMP file rendered at a fixed region). */
typedef struct {
    HalOsdOverlayBase base;
    float             width;                   /**< normalised width  [0..1] */
    float             height;                  /**< normalised height [0..1] */
    char              image_path[256];         /**< filesystem path to the source image */
} HalOsdImageOverlay;

/**
 * Static text overlay.
 *
 * font_size is in absolute pixels at the current encoder resolution.
 * The HAL rescales font_size automatically when resolution changes.
 */
typedef struct {
    HalOsdOverlayBase base;
    char              label[256];              /**< text string to render */
    HalOsdColor       text_color;              /**< foreground colour */
    HalOsdColor       background_color;        /**< background rectangle colour */
    char              font_path[256];          /**< path to TTF font file */
    float             font_size;               /**< font size in pixels (absolute, rescaled on resolution change) */
    int               line_thickness;          /**< text stroke thickness */
    HalOsdColor       shadow_color;            /**< drop-shadow colour (r < 0 disables) */
    float             shadow_offset_x;         /**< shadow X offset (normalised) */
    float             shadow_offset_y;         /**< shadow Y offset (normalised) */
    int               outline_size;            /**< outline thickness in pixels (0 = none) */
    HalOsdColor       outline_color;           /**< outline colour (r < 0 disables) */
    HalOsdFontWeight  font_weight;             /**< normal or bold */
} HalOsdTextOverlay;

/** Date-time overlay (auto-updating text with a strftime format string). */
typedef struct {
    HalOsdTextOverlay text;                    /**< inherits all text overlay fields */
    char              datetime_format[64];     /**< strftime pattern, default "%d-%m-%Y %H:%M:%S" */
} HalOsdDateTimeOverlay;

/** Custom overlay (caller-managed pixel buffer). */
typedef struct {
    HalOsdOverlayBase  base;
    float              width;                  /**< normalised width  [0..1] */
    float              height;                 /**< normalised height [0..1] */
    HalOsdCustomFormat format;                 /**< pixel format of the data buffer */
    void              *data;                   /**< pointer to pixel data (owned by caller) */
    uint32_t           data_size;              /**< byte length of data */
} HalOsdCustomOverlay;

/* --------------------------------------------------------------------
 * Generic overlay container (for get_overlays output)
 * -------------------------------------------------------------------- */

/** Tagged union returned by get_overlays / get_overlay. */
typedef struct {
    HalOsdOverlayType type;
    union {
        HalOsdImageOverlay    image;
        HalOsdTextOverlay     text;
        HalOsdDateTimeOverlay datetime;
        HalOsdCustomOverlay   custom;
    } data;
} HalOsdOverlay;

/* --------------------------------------------------------------------
 * OSD operations table
 * -------------------------------------------------------------------- */

/**
 * Function-pointer table for OSD overlay operations.
 * Platform implementations populate HAL_OSD_OPS at link time.
 *
 * All functions take a codec_ctx pointer (HalCodecContext*) as the first
 * argument, since overlays are blended into the encoder output.
 */
typedef struct {
    /* ---- add (creates a new overlay; fails if id already exists) ---- */

    /**
     * @brief Add an image overlay.
     * @param codec_ctx Codec context (encoder stream).
     * @param overlay   Image overlay definition (id must be unique).
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*add_image_overlay)(void *codec_ctx, const HalOsdImageOverlay *overlay);

    /** @brief Add a static text overlay. */
    int (*add_text_overlay)(void *codec_ctx, const HalOsdTextOverlay *overlay);

    /** @brief Add a date-time overlay. */
    int (*add_datetime_overlay)(void *codec_ctx, const HalOsdDateTimeOverlay *overlay);

    /** @brief Add a custom pixel-buffer overlay. */
    int (*add_custom_overlay)(void *codec_ctx, const HalOsdCustomOverlay *overlay);

    /* ---- set (updates an existing overlay by id) ---- */

    /** @brief Update an existing image overlay (matched by id). */
    int (*set_image_overlay)(void *codec_ctx, const HalOsdImageOverlay *overlay);

    /** @brief Update an existing text overlay. */
    int (*set_text_overlay)(void *codec_ctx, const HalOsdTextOverlay *overlay);

    /** @brief Update an existing date-time overlay. */
    int (*set_datetime_overlay)(void *codec_ctx, const HalOsdDateTimeOverlay *overlay);

    /** @brief Update an existing custom overlay. */
    int (*set_custom_overlay)(void *codec_ctx, const HalOsdCustomOverlay *overlay);

    /* ---- remove / enable / disable ---- */

    /**
     * @brief Remove an overlay by id.
     * @param codec_ctx Codec context.
     * @param overlay_id Overlay identifier.
     * @return 0 on success, HAL_ERR_NOT_FOUND if id does not exist.
     */
    int (*remove_overlay)(void *codec_ctx, const char *overlay_id);

    /**
     * @brief Enable or disable an overlay without removing it.
     * @param codec_ctx Codec context.
     * @param overlay_id Overlay identifier.
     * @param enabled    true = visible, false = hidden.
     * @return 0 on success, HAL_ERR_NOT_FOUND if id does not exist.
     */
    int (*set_overlay_enabled)(void *codec_ctx, const char *overlay_id, bool enabled);

    /* ---- query ---- */

    /**
     * @brief Retrieve all overlays on this encoder stream.
     *
     * On entry, *count holds the capacity of the overlays array.
     * On exit, *count holds the actual number of overlays written.
     * If the array is too small, returns HAL_ERR_INSUFFICIENT_BUFFER and
     * sets *count to the required capacity.
     *
     * @param codec_ctx Codec context.
     * @param overlays  Caller-allocated array to receive overlays (may be NULL to query count only).
     * @param count     [in/out] capacity / actual count.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_overlays)(void *codec_ctx, HalOsdOverlay *overlays, uint32_t *count);

    /**
     * @brief Retrieve a single overlay by id.
     * @param codec_ctx  Codec context.
     * @param overlay_id Overlay identifier.
     * @param overlay    Receives the overlay data.
     * @return 0 on success, HAL_ERR_NOT_FOUND if id does not exist.
     */
    int (*get_overlay)(void *codec_ctx, const char *overlay_id, HalOsdOverlay *overlay);

    /**
     * @brief Remove all overlays from this encoder stream.
     * @param codec_ctx Codec context.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*clear_overlays)(void *codec_ctx);

    /**
     * @brief Get the OSD HAL version string.
     * @return Static version string e.g. "Hailo15 HAL-OSD 2.0.0".
     */
    const char *(*get_version)(void);
} HalOsdOps;

/** Platform-specific OSD operations (resolved at link time). */
extern HalOsdOps HAL_OSD_OPS;

#ifdef __cplusplus
}
#endif
