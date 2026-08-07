/**
 * @file hal_dsp.h
 * @brief HAL DSP - Image processing interface (crop/resize, convert, blend, privacy mask).
 *
 * This module provides a unified, platform-agnostic interface for image
 * processing operations that may be offloaded to a DSP (e.g. Hailo-15
 * HailoDSP).  The design mirrors other HAL V2 modules:
 *   - C API with plain structs and enums.
 *   - Function-pointer table populated by platform implementations at link time.
 *
 * The initial implementation targets Hailo-15 and is backed by the vendor
 * HailoDSP library, but the public API must remain platform-neutral.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "../common/hal_common.h"
#include "../common/hal_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------
 * Basic types
 * -------------------------------------------------------------------- */

/** Interpolation method for scaling / resize operations. */
typedef enum {
    HAL_DSP_INTERPOLATION_NEAREST = 0,
    HAL_DSP_INTERPOLATION_BILINEAR,
    HAL_DSP_INTERPOLATION_AREA,
    HAL_DSP_INTERPOLATION_BICUBIC,
    HAL_DSP_INTERPOLATION_MAX,
} HalDspInterpolation;

/** Scaling mode for crop+resize operations. */
typedef enum {
    HAL_DSP_SCALING_STRETCH = 0,          /* scale to fill without preserving aspect ratio */
    HAL_DSP_SCALING_LETTERBOX_MIDDLE,     /* scale to fit, letterbox centered */
    HAL_DSP_SCALING_LETTERBOX_UP_LEFT,    /* scale to fit, letterbox aligned to top/left */
    HAL_DSP_SCALING_SCALE_AND_CROP,       /* preserve aspect ratio, crop overflowing region */
    HAL_DSP_SCALING_MAX,
} HalDspScalingMode;

/** Letterbox alignment for crop+resize with letterbox padding. */
typedef enum {
    HAL_DSP_LETTERBOX_NONE = 0,
    HAL_DSP_LETTERBOX_MIDDLE,
    HAL_DSP_LETTERBOX_UP_LEFT,
    HAL_DSP_LETTERBOX_MAX,
} HalDspLetterboxAlignment;

/** Color triple (RGB or YUV) or single grayscale component. */
typedef struct {
    union {
        struct {
            uint8_t gray;
        };
        struct {
            uint8_t r;
            uint8_t g;
            uint8_t b;
        };
        struct {
            uint8_t y;
            uint8_t u;
            uint8_t v;
        };
    };
} HalDspColor;

/** Region-of-interest rectangle in pixel coordinates (inclusive ranges). */
typedef struct {
    uint32_t start_x;    /* left-most pixel (0-based, inclusive) */
    uint32_t start_y;    /* top-most pixel (0-based, inclusive) */
    uint32_t end_x;      /* right-most pixel (1-based, exclusive upper bound) */
    uint32_t end_y;      /* bottom-most pixel (1-based, exclusive upper bound) */
} HalDspRoi;

/* --------------------------------------------------------------------
 * Operation parameter structs (synchronous)
 * -------------------------------------------------------------------- */

/**
 * Parameters for a simple resize (no crop).
 *
 * Both src and dst are user-provided frame buffers; implementations must
 * not modify src contents.
 */
typedef struct {
    const HalFrameBuffer *src;
    HalFrameBuffer       *dst;
    HalDspInterpolation   interpolation;
} HalDspResizeParams;

/**
 * Parameters for crop + resize (single output).
 *
 * The crop ROI is applied on src, the result is resized into dst.
 */
typedef struct {
    const HalFrameBuffer *src;
    HalFrameBuffer       *dst;
    HalDspRoi             crop;
    HalDspInterpolation   interpolation;
    HalDspScalingMode     scaling_mode;
    HalDspLetterboxAlignment letterbox_alignment;
    HalDspColor           letterbox_color;  /* used when scaling_mode is letterbox-style */
} HalDspCropResizeParams;

/**
 * Parameters for a single crop+resize output in a multi-output operation.
 */
typedef struct {
    HalDspRoi             crop;
    HalFrameBuffer       *dst;
    HalDspScalingMode     scaling_mode;
    HalDspColor           letterbox_color;  /* used when scaling mode implies padding */
} HalDspMultiCropOutput;

/**
 * Parameters for multi crop+resize on one source frame.
 */
typedef struct {
    const HalFrameBuffer   *src;
    HalDspMultiCropOutput  *outputs;
    uint32_t                output_count;
    HalDspInterpolation     interpolation;
} HalDspMultiCropResizeParams;

/** Parameters for format conversion between two frame buffers. */
typedef struct {
    const HalFrameBuffer *src;
    HalFrameBuffer       *dst;
} HalDspConvertFormatParams;

/** Overlay image and placement for blend operation. */
typedef struct {
    HalFrameBuffer *overlay;
    int32_t         x_offset;
    int32_t         y_offset;
} HalDspOverlay;

/** Parameters for alpha blend operation. */
typedef struct {
    HalFrameBuffer *base;          /* base image to be modified in-place */
    HalDspOverlay  *overlays;      /* array of overlays */
    uint32_t        overlay_count; /* number of overlays */
} HalDspBlendParams;

/** Flip direction for flip/rotate operations. */
typedef enum {
    HAL_DSP_FLIP_DIRECTION_NONE = 0,
    HAL_DSP_FLIP_DIRECTION_HORIZONTAL,
    HAL_DSP_FLIP_DIRECTION_VERTICAL,
    HAL_DSP_FLIP_DIRECTION_BOTH,
    HAL_DSP_FLIP_DIRECTION_MAX,
} HalDspFlipDirection;

/** Rotation angle (clockwise). */
typedef enum {
    HAL_DSP_ROTATION_ANGLE_0 = 0,
    HAL_DSP_ROTATION_ANGLE_90,
    HAL_DSP_ROTATION_ANGLE_180,
    HAL_DSP_ROTATION_ANGLE_270,
    HAL_DSP_ROTATION_ANGLE_MAX,
} HalDspRotationAngle;

/** Parameters for flip + rotate operation on a frame. */
typedef struct {
    const HalFrameBuffer *src;
    HalFrameBuffer       *dst;
    HalDspFlipDirection   flip_direction;
    HalDspRotationAngle   rotation_angle;
    HalDspInterpolation   interpolation;
} HalDspFlipRotateParams;

/** Privacy mask type (solid color or blur / pixelization). */
typedef enum {
    HAL_DSP_PRIVACY_MASK_COLOR = 0,
    HAL_DSP_PRIVACY_MASK_BLUR,
    HAL_DSP_PRIVACY_MASK_MAX,
} HalDspPrivacyMaskType;

/** Privacy mask region based on a bitmask and ROI hints. */
typedef struct {
    uint8_t              *bitmask;        /* bitmask buffer (see implementation notes) */
    uint32_t              stride_bytes;   /* stride of bitmask in bytes */
    HalDspPrivacyMaskType type;
    union {
        HalDspColor       color;          /* used when type == COLOR */
        uint32_t          blur_radius;    /* used when type == BLUR (kernel / radius) */
    };
    HalDspRoi            *rois;           /* optional array of ROIs in bitmask space */
    uint32_t              roi_count;
} HalDspPrivacyMaskRegion;

/** Parameters for applying privacy mask(s) on an image. */
typedef struct {
    HalFrameBuffer          *image;       /* image modified in-place */
    HalDspPrivacyMaskRegion *regions;
    uint32_t                 region_count;
} HalDspPrivacyMaskParams;

/* --------------------------------------------------------------------
 * Asynchronous job API
 * -------------------------------------------------------------------- */

/** DSP operation type for asynchronous submission. */
typedef enum {
    HAL_DSP_OP_CONVERT_FORMAT = 0,
    HAL_DSP_OP_RESIZE,
    HAL_DSP_OP_CROP_RESIZE,
    HAL_DSP_OP_MULTI_CROP_RESIZE,
    HAL_DSP_OP_BLEND,
    HAL_DSP_OP_FLIP_ROTATE,
    HAL_DSP_OP_PRIVACY_MASK,
    HAL_DSP_OP_MAX,
} HalDspOpType;

/** Opaque handle to an asynchronous DSP job. */
typedef struct HalDspJobTag *HalDspJobHandle;

/** Job status for asynchronous operations. */
typedef enum {
    HAL_DSP_JOB_PENDING = 0,
    HAL_DSP_JOB_RUNNING,
    HAL_DSP_JOB_COMPLETED,
    HAL_DSP_JOB_CANCELLED,
    HAL_DSP_JOB_FAILED,
} HalDspJobStatus;

/** Result info for a completed or pending job. */
typedef struct {
    HalDspJobStatus status;
    int             result_code;      /* 0 on success, negative HalErrorCode on failure */
} HalDspJobResult;

/* --------------------------------------------------------------------
 * Context configuration
 * -------------------------------------------------------------------- */

/** DSP context configuration. Reserved for future extensions. */
typedef struct {
    int  device_priority;    /* optional device priority, 0 = default */
    void *priv;              /* platform-specific extension (opaque) */
} HalDspConfig;

/* --------------------------------------------------------------------
 * Operations table
 * -------------------------------------------------------------------- */

/**
 * Function-pointer table for DSP image processing operations.
 * Platform implementations populate HAL_DSP_OPS at link time.
 */
typedef struct {
    /**
     * @brief Initialize a DSP context.
     * @param config            Configuration (copied internally).
     * @param dsp_ctx_return    Receives the allocated context pointer.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*init)(const HalDspConfig *config, void **dsp_ctx_return);

    /**
     * @brief Destroy a DSP context and release its resources.
     * @param dsp_ctx DSP context pointer returned by init().
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*deinit)(void *dsp_ctx);

    /* ----------------- Synchronous operations ----------------- */

    int (*convert_format)(void *dsp_ctx, const HalDspConvertFormatParams *params);
    int (*resize)(void *dsp_ctx, const HalDspResizeParams *params);
    int (*crop_and_resize)(void *dsp_ctx, const HalDspCropResizeParams *params);
    int (*multi_crop_and_resize)(void *dsp_ctx, const HalDspMultiCropResizeParams *params);
    int (*blend)(void *dsp_ctx, const HalDspBlendParams *params);
    int (*flip_rotate)(void *dsp_ctx, const HalDspFlipRotateParams *params);
    int (*privacy_mask)(void *dsp_ctx, const HalDspPrivacyMaskParams *params);

    /* ----------------- Asynchronous operations ----------------- */

    /**
     * @brief Submit a DSP operation for asynchronous execution.
     *
     * The params pointer must remain valid until the job completes or is
     * cancelled. Implementations may copy the parameter struct internally
     * to support fully decoupled lifetimes.
     *
     * @param dsp_ctx   DSP context.
     * @param op_type   Operation type.
     * @param params    Pointer to the operation-specific parameter struct.
     * @param job_out   Receives a job handle on success.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*submit)(void *dsp_ctx, HalDspOpType op_type, const void *params, HalDspJobHandle *job_out);

    /**
     * @brief Wait for a job to complete or timeout.
     *
     * @param dsp_ctx     DSP context.
     * @param job         Job handle obtained from submit().
     * @param timeout_ms  Timeout in milliseconds; 0 = non-blocking poll,
     *                    UINT32_MAX = wait forever.
     * @param result_out  Receives job result information (may be NULL).
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*wait)(void *dsp_ctx, HalDspJobHandle job, uint32_t timeout_ms, HalDspJobResult *result_out);

    /**
     * @brief Attempt to cancel a pending job.
     *
     * If the job is already running, implementations may return
     * HAL_ERR_INVALID_STATE.
     *
     * @param dsp_ctx DSP context.
     * @param job     Job handle.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*cancel)(void *dsp_ctx, HalDspJobHandle job);

    /**
     * @brief Release a job handle and associated resources.
     *
     * Must be called exactly once for each job obtained from submit().
     *
     * @param dsp_ctx DSP context.
     * @param job     Job handle to destroy (may be NULL).
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*job_release)(void *dsp_ctx, HalDspJobHandle job);

    /**
     * @brief Get the DSP HAL version string.
     * @return Static version string, e.g. \"Hailo15 HAL-DSP 2.0.0\".
     */
    const char *(*get_version)(void);
} HalDspOps;

/** Platform-specific DSP operations (resolved at link time). */
extern HalDspOps HAL_DSP_OPS;

#ifdef __cplusplus
}
#endif

