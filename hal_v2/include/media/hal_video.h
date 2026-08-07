/**
 * @file hal_video.h
 * @brief HAL Video - Video capture and streaming interface.
 *
 * Supports multiple video source types:
 *   - HAL_VIDEO_TYPE_CSI:        MIPI-CSI camera sensor
 *   - HAL_VIDEO_TYPE_UVC:        USB Video Class device (/dev/videoX)
 *   - HAL_VIDEO_TYPE_FROM_MEDIA: obtained from media pipeline (get_video_list)
 *
 * FROM_MEDIA contexts are created by the media component; init() and
 * deinit() are not supported for this type.
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
 * Video types
 * -------------------------------------------------------------------- */

/** Video input source type. */
typedef enum {
    HAL_VIDEO_TYPE_CSI = 0,         /* MIPI-CSI camera sensor */
    HAL_VIDEO_TYPE_UVC,             /* USB Video Class device */
    HAL_VIDEO_TYPE_FROM_MEDIA,      /* obtained from media library (see HalMediaOps.get_video_list) */
    HAL_VIDEO_TYPE_MAX,             /* sentinel (not a valid type) */
} HalVideoType;

/* --------------------------------------------------------------------
 * Video configuration
 * -------------------------------------------------------------------- */

/**
 * Video device configuration.
 *
 * For CSI / UVC types: all fields are set by the caller before init().
 * For FROM_MEDIA type: fields are populated by media->get_video_list()
 * and must not be modified by the caller.
 */
typedef struct {
    HalVideoType    type;               /* video source type */
    char           *path;               /* device path e.g. "/dev/video0" (NULL for FROM_MEDIA) */
    void           *media_ptr;          /* pointer to parent HalMediaContext (only for FROM_MEDIA) */

    uint32_t        width;              /* output frame width in pixels */
    uint32_t        height;             /* output frame height in pixels */
    uint32_t        framerate;          /* target frame rate (fps) */
    HalPixelFormat  format;             /* output pixel format */
    uint32_t        pool_max_buffers;   /* max buffers in the frame pool (0 = platform default) */

    void           *priv;               /* platform-specific extension (opaque) */
} HalVideoConfig;

/* --------------------------------------------------------------------
 * Callback type
 * -------------------------------------------------------------------- */

/**
 * Frame delivery callback for push (subscribe) mode.
 *
 * @param video_ctx  The video context that produced the frame.
 * @param frame      Frame buffer; caller must call release_frame() when done.
 * @param userdata   Opaque pointer registered at subscribe time.
 *
 * @note The callback is invoked from an internal worker thread.
 *       Implementations should return quickly to avoid dropping frames.
 */
typedef void (*HalVideoFrameCallback)(void *video_ctx, HalFrameBuffer *frame, void *userdata);

/* --------------------------------------------------------------------
 * Sensor module information (CSI / media pipeline)
 * -------------------------------------------------------------------- */

/** @name HalVideoSensorModuleInfo::valid_fields */
/** @{ */
#define HAL_VIDEO_SENSOR_INFO_VALID_MODEL_NAME   (1u << 0) /**< sensor_model_name is valid */
#define HAL_VIDEO_SENSOR_INFO_VALID_I2C          (1u << 1) /**< i2c_bus / i2c_address are valid */
#define HAL_VIDEO_SENSOR_INFO_VALID_PIXEL_FORMAT (1u << 2) /**< sensor_pixel_format is valid (V4L2 fourcc) */
/** @} */

/**
 * Generic sensor module descriptor filled by the video HAL when supported.
 *
 * For HAL_VIDEO_TYPE_FROM_MEDIA, populated via the media library SensorRegistry
 * ( sysfs / v4l subdevice probe ). Other video types return HAL_ERR_NOT_SUPPORTED.
 */
typedef struct {
    uint32_t valid_fields;           /**< Bitmask of HAL_VIDEO_SENSOR_INFO_VALID_* */
    char     sensor_model_name[64];  /**< e.g. "IMX678" */
    int32_t  i2c_bus;                /**< I2C bus number, or -1 if unknown */
    char     i2c_address[16];        /**< Address string from driver (e.g. "001a") */
    int32_t  sensor_pixel_format;    /**< V4L2 pixel format fourcc as int, or -1 if unknown */
} HalVideoSensorModuleInfo;

/* --------------------------------------------------------------------
 * Video operations table
 * -------------------------------------------------------------------- */

/**
 * Function-pointer table for video device operations.
 * Platform implementations populate HAL_VIDEO_OPS at link time.
 */
typedef struct {
    /**
     * @brief Initialize a video device and allocate context.
     *
     * Not supported for HAL_VIDEO_TYPE_FROM_MEDIA (returns HAL_ERR_NOT_SUPPORTED).
     *
     * @param config           Device configuration (copied internally).
     * @param video_ctx_return Receives the allocated video context pointer.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*init)(const HalVideoConfig *config, void **video_ctx_return);

    /**
     * @brief Release a video device and free its context.
     *
     * Not supported for HAL_VIDEO_TYPE_FROM_MEDIA (returns HAL_ERR_NOT_SUPPORTED).
     *
     * @param video_ctx Video context to destroy.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*deinit)(void *video_ctx);

    /**
     * @brief Start frame capture.
     *
     * For FROM_MEDIA type: start is delegated to the parent media pipeline.
     *
     * @param video_ctx Initialized video context.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*start)(void *video_ctx);

    /**
     * @brief Stop frame capture.
     *
     * For FROM_MEDIA type: stop is delegated to the parent media pipeline.
     *
     * @param video_ctx Running video context.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*stop)(void *video_ctx);

    /**
     * @brief Query the current device status.
     * @param video_ctx Video context.
     * @return HalStatus value.
     */
    HalStatus (*get_status)(void *video_ctx);

    /**
     * @brief Get a snapshot of the current video configuration.
     * @param video_ctx Video context.
     * @param config    Receives the current configuration.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*get_current_config)(void *video_ctx, HalVideoConfig *config);

    /**
     * @brief Dynamically change the output resolution.
     *
     * On Hailo-15 (FROM_MEDIA / medialib), this applies the change via the media library's
     * in-place set_override_parameters() (pipeline stop/reconfigure/start) — NOT a full
     * MediaLibrary reinit, so it is relatively light.
     *
     * To avoid the encoder receiving a stride-mismatched buffer mid-reconfigure (which makes
     * VCEnc reject the new geometry with error -3 and silently stalls the encoder), the call
     * briefly suspends encoder feeding for the stream around the change — both auto_feed
     * (frontend->encoder forwarding) and manual input_frame() — and stops delivering new raw
     * frames to the video subscriber during the switch window. The pipeline stop's EOS flush
     * drains in-flight encoder buffers.
     *
     * Best-effort reliability: if the caller is holding a previously-obtained raw frame and has
     * not released it (so its buffer cannot return to the frontend pool), the in-place
     * reconfigure may still fail (the medialib does not expose pool buffer counts, so this
     * cannot be detected and reported as an error). For strict reliability in that case, or if
     * a previous change left a stream in a bad state, use a full media reconfigure
     * (override_stream_params / pipeline reinit) instead — that path is safe with held buffers.
     *
     * @param video_ctx Video context.
     * @param width     New width in pixels (must be a supported encoder resolution).
     * @param height    New height in pixels (must be a supported encoder resolution).
     * @return 0 on success, HAL_ERR_INVALID_ARG for an unsupported resolution, negative
     *         HalErrorCode on failure.
     */
    int (*dynamic_change_resolution)(void *video_ctx, uint32_t width, uint32_t height);

    /**
     * @brief Dynamically change the output frame rate.
     * @param video_ctx Video context.
     * @param framerate New frame rate (fps).
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*dynamic_change_framerate)(void *video_ctx, uint32_t framerate);

    /**
     * @brief Dynamically change the output pixel format.
     * @param video_ctx Video context.
     * @param format    New pixel format.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*dynamic_change_format)(void *video_ctx, HalPixelFormat format);

    /**
     * @brief Dynamically change the frame pool size.
     * @param video_ctx      Video context.
     * @param pool_max_buffers New maximum buffer count.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*dynamic_change_pool_max_buffers)(void *video_ctx, uint32_t pool_max_buffers);

    /**
     * @brief Subscribe to frame delivery (push mode).
     *
     * For FROM_MEDIA type, stream_name corresponds to the frontend output
     * stream ID defined in the media library config JSON.
     * For CSI / UVC types, stream_name is user-defined and may be NULL
     * (single-stream devices).
     *
     * @param video_ctx   Video context.
     * @param stream_name Stream identifier (see note above).
     * @param callback    Frame delivery callback.
     * @param userdata    Opaque pointer passed to the callback.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*subscribe_stream)(void *video_ctx, const char *stream_name,
                            HalVideoFrameCallback callback, void *userdata);

    /**
     * @brief Unsubscribe from frame delivery.
     * @param video_ctx   Video context.
     * @param stream_name Stream identifier used at subscribe time.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*unsubscribe_stream)(void *video_ctx, const char *stream_name);

    /**
     * @brief Release a frame buffer back to the pool.
     *
     * Must be called for every frame obtained via subscribe callback.
     *
     * @param video_ctx Video context.
     * @param frame     Frame buffer to release.
     * @return 0 on success, negative HalErrorCode on failure.
     */
    int (*release_frame)(void *video_ctx, HalFrameBuffer *frame);

    /**
     * @brief Get the video HAL version string.
     * @return Static version string e.g. "Hailo15 HAL-VIDEO 2.0.0".
     */
    const char *(*get_version)(void);

    /**
     * @brief Query sensor module information for this video context.
     *
     * When the context type is HAL_VIDEO_TYPE_FROM_MEDIA, the implementation
     * uses the media library SensorRegistry to probe the sensor (model string,
     * optional I2C bus/address, pixel format). For HAL_VIDEO_TYPE_CSI,
     * HAL_VIDEO_TYPE_UVC, and any other type, returns HAL_ERR_NOT_SUPPORTED.
     *
     * @param video_ctx    Video context (must match a supported FROM_MEDIA context when used).
     * @param sensor_index Physical sensor index (0 = primary, 1 = secondary on dual-sensor systems).
     * @param info         Output; zeroed then filled on success.
     * @return HAL_OK on success;
     *         HAL_ERR_NOT_SUPPORTED for HAL_VIDEO_TYPE_CSI / UVC / other (no SensorRegistry path);
     *         HAL_ERR_NOT_FOUND when SensorRegistry cannot match any sensor in sysfs (wrong index,
     *         sensor driver not loaded, or not running on hardware);
     *         HAL_ERR_INVALID_ARG for bad pointers or invalid FROM_MEDIA context.
     */
    int (*get_sensor_module_info)(void *video_ctx, uint32_t sensor_index, HalVideoSensorModuleInfo *info);
} HalVideoOps;

/** Platform-specific video operations (resolved at link time). */
extern HalVideoOps HAL_VIDEO_OPS;

#ifdef __cplusplus
}
#endif
